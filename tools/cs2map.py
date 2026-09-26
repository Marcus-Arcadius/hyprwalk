#!/usr/bin/env python3
"""cs2map: export a Counter-Strike 2 map from your own install to a GLB hypr3d can walk around in.

    python3 tools/cs2map.py MAP [options]

MAP is a map name such as de_mirage, looked up in the CS2 install Steam knows about, or the path
of a map's .vpk. The map is exported with the command line version of Source 2 Viewer
(Source2Viewer-CLI, https://github.com/ValveResourceFormat/ValveResourceFormat) and then made
to suit hypr3d:

  - the 3D skybox (the town around the playable area) is put in at its real size
  - the sky texture goes on a dome (reading its .exr needs Blender; without it hypr3d's own sky
    shows)
  - CS2's own lighting comes along: the lightmaps and light probes the map was baked with (for the
    3D skybox too), the sun, the fog and the map's exposure range and tone curve (hypr3d's
    HYPR3D_lighting extension, the _LIGHTMAP_UV vertex attribute; needs Blender to read the HDR
    lightmaps)
  - materials keep their normal, roughness and metalness maps, and what CS2's shaders do beyond
    glTF: detail textures, self-illumination, which ones have specular, decals that multiply
    (hypr3d's HYPR3D_materials_source2 extension)
  - blended materials keep their second layer, painted in by the vertices the way the game does
    (hypr3d's HYPR3D_materials_blend extension and a _BLEND vertex attribute)
  - foliage drops the wind data Valve keeps in its vertex colours
  - decals (the bombsite letters, stains, posters) lie 1 cm off what they're on: Source 2 Viewer 20
    lifts them 39 cm
  - entities that start disabled (the Retakes barriers, for one) are left out
  - there's a start point (hypr3d_spawn) at a team's spawn, and the desktop's wall when you give
    one (hypr3d_desktop); otherwise hypr3d looks for a wall itself

Source 2 Viewer has to be able to read the shaders of your CS2 build, or materials lose their
transparency and tint. When it can't, cs2map gets a newer one: the latest release if that is newer,
else it builds the current source (which needs git, and the .NET 10 SDK or nix).

options:
  -o OUT.glb            where to write (default ~/.local/share/hypr3d/maps/MAP.glb)
  --game DIR            the CS2 folder (".../Counter-Strike Global Offensive"), default: found
                        through Steam's library list
  --vrf PATH            Source2Viewer-CLI to use (default: $SOURCE2VIEWER_CLI, then PATH, then
                        the newest in ~/.cache/hypr3d/source2viewer/*/, else the latest release is
                        downloaded there)
  --no-lighting         leave CS2's lighting out (hypr3d then lights the map itself)
  --spawn WHERE         where hypr3d starts you: t or ct (a team's spawn, default t), or X,Y,Z,YAW
                        in the map's own units and degrees (the numbers Hammer and the game's
                        getpos show)
  --desktop X,Y,Z,YAW   the middle of the desktop, on a wall, and the way it faces, likewise
  --no-skybox           leave the 3D skybox out
  --no-sky              leave the sky dome out
  --keep DIR            export into DIR and leave the raw export there
  --list                list the maps in the install and stop

Maps are Valve's: this only reads the copy of the game you have, for your own use.
"""

import sys, os, re, io, json, math, struct, shutil, subprocess, tempfile, argparse, glob, zipfile, time, bisect
import urllib.request

EXT = 'HYPR3D_materials_blend'
EXT_S2 = 'HYPR3D_materials_source2'
EXT_LIGHT = 'HYPR3D_lighting'
INCH = 0.0254
REPO = 'https://github.com/ValveResourceFormat/ValveResourceFormat'
RELEASES = 'https://api.github.com/repos/ValveResourceFormat/ValveResourceFormat/releases/latest'
# what Source 2 Viewer logs when the game's shaders are newer than it knows
VCS_ERROR = 'Only VCS file versions'
# the lightmaps hypr3d reads: CS2's lightmap format 8.2 (irradiance, its main direction, baked shadows)
LIGHTMAPS = ('irradiance', 'directional_irradiance', 'direct_light_shadows')
# glTF accessors: struct's letter for each component type, and how many numbers each type has
COMPONENT = {5120: 'b', 5121: 'B', 5122: 'h', 5123: 'H', 5125: 'I', 5126: 'f'}
WIDTH = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4, 'MAT2': 4, 'MAT3': 9, 'MAT4': 16}

# where to start and where the desktop goes, for maps that have been looked at: in the map's own
# units, as --spawn and --desktop take them
PRESETS = {
    # T spawn, facing the big yellow wall by the gate, with the spawn's streets behind
    'de_mirage': {'spawn': '1249,447,-262,0', 'desktop': '1375,447,-197,180'},
}

WARNINGS = []


def log(*a):
    print('cs2map:', *a, flush=True)


def warn(msg):
    if msg not in WARNINGS:
        WARNINGS.append(msg)
        print('cs2map: warning:', msg, flush=True)


class Fail(Exception):
    pass


# ---------------------------------------------------------------- coordinates

def to_gltf(p):
    """Source (inches, z up) -> the glTF Source 2 Viewer writes (meters, y up)"""
    x, y, z = p
    return [y * INCH, z * INCH, x * INCH]


def yaw_quat(theta):
    """a turn of theta radians about +y, as a glTF (x, y, z, w) quaternion"""
    return [0.0, math.sin(theta / 2), 0.0, math.cos(theta / 2)]


def floats(s, n, what):
    try:
        v = [float(t) for t in s.split(',')]
    except ValueError:
        v = []
    if len(v) != n:
        raise Fail(f'{what} takes {n} numbers separated by commas, not "{s}"')
    return v


def srgb_to_linear(c):
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


def source_rotation(angles):
    """Source's AngleMatrix for pitch, yaw, roll in degrees: its columns are the forward, left and up
    axes of something turned that way"""
    p, y, r = (math.radians(a) for a in angles)
    sp, cp, sy, cy, sr, cr = math.sin(p), math.cos(p), math.sin(y), math.cos(y), math.sin(r), math.cos(r)
    return [[cp * cy, sr * sp * cy - cr * sy, cr * sp * cy + sr * sy],
            [cp * sy, sr * sp * sy + cr * cy, cr * sp * sy - sr * cy],
            [-sp, sr * cp, cr * cp]]


def mat_mul(a, b):
    """4x4 row-major matrices, applied to column vectors"""
    return [[sum(a[i][k] * b[k][j] for k in range(4)) for j in range(4)] for i in range(4)]


def affine(m3=None, t=(0, 0, 0)):
    m3 = m3 or [[1, 0, 0], [0, 1, 0], [0, 0, 1]]
    return [list(m3[0]) + [t[0]], list(m3[1]) + [t[1]], list(m3[2]) + [t[2]], [0, 0, 0, 1]]


def apply(m, p):
    return [sum(m[i][k] * p[k] for k in range(3)) + m[i][3] for i in range(3)]


def column_major(m):
    return [float(m[r][c]) for c in range(4) for r in range(4)]


def node_matrix(nd):
    """a glTF node's own transform, row-major"""
    if 'matrix' in nd:
        m = nd['matrix']
        return [[float(m[c * 4 + r]) for c in range(4)] for r in range(4)]
    x, y, z, w = nd.get('rotation', [0, 0, 0, 1])
    s = nd.get('scale', [1, 1, 1])
    r = [[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
         [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
         [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]]
    return affine([[r[i][k] * s[k] for k in range(3)] for i in range(3)], nd.get('translation', [0, 0, 0]))


def invert_affine(m):
    """the inverse of an affine 4x4 (row-major), or None when it flattens space"""
    a, b, c = m[0][:3], m[1][:3], m[2][:3]
    cof = [[b[1] * c[2] - b[2] * c[1], a[2] * c[1] - a[1] * c[2], a[1] * b[2] - a[2] * b[1]],
           [b[2] * c[0] - b[0] * c[2], a[0] * c[2] - a[2] * c[0], a[2] * b[0] - a[0] * b[2]],
           [b[0] * c[1] - b[1] * c[0], a[1] * c[0] - a[0] * c[1], a[0] * b[1] - a[1] * b[0]]]
    det = a[0] * cof[0][0] + a[1] * cof[1][0] + a[2] * cof[2][0]
    if abs(det) < 1e-12:
        return None
    inv = [[cof[i][k] / det for k in range(3)] for i in range(3)]
    t = [m[i][3] for i in range(3)]
    return affine(inv, [-sum(inv[i][k] * t[k] for k in range(3)) for i in range(3)])


# glTF (meters, y up) -> Source (inches, z up): the inverse of to_gltf
GLTF_TO_SOURCE = affine([[0, 0, 1 / INCH], [1 / INCH, 0, 0], [0, 1 / INCH, 0]])


# ---------------------------------------------------------------- finding the game

def parse_vdf(text):
    """Valve's KeyValues text (libraryfolders.vdf, appmanifest) as nested dicts"""
    tokens = re.findall(r'"((?:[^"\\]|\\.)*)"|([{}])', text)

    def block(i):
        d = {}
        while i < len(tokens):
            s, brace = tokens[i]
            if brace == '}':
                return d, i + 1
            if brace == '{' or i + 1 >= len(tokens):
                return d, len(tokens)
            s2, brace2 = tokens[i + 1]
            if brace2 == '{':
                d[s], i = block(i + 2)
            else:
                d[s] = s2
                i += 2
        return d, i

    return block(0)[0]


def steam_libraries():
    seen, out = set(), []
    for root in ('~/.local/share/Steam', '~/.steam/steam', '~/.steam/root', '~/.var/app/com.valvesoftware.Steam/data/Steam'):
        vdf = os.path.join(os.path.expanduser(root), 'steamapps', 'libraryfolders.vdf')
        try:
            data = parse_vdf(open(vdf, encoding='utf-8', errors='replace').read())
        except OSError:
            continue
        for v in data.get('libraryfolders', {}).values():
            p = v.get('path') if isinstance(v, dict) else None
            if p and os.path.realpath(p) not in seen:
                seen.add(os.path.realpath(p))
                out.append(p)
    return out


def find_game(arg):
    """the game/ folder of CS2"""
    cands = []
    if arg:
        a = os.path.abspath(os.path.expanduser(arg))
        cands += [a, os.path.join(a, 'game'), os.path.dirname(a)]
    else:
        cands += [os.path.join(lib, 'steamapps', 'common', 'Counter-Strike Global Offensive', 'game') for lib in steam_libraries()]
    for c in cands:
        if os.path.isfile(os.path.join(c, 'csgo', 'gameinfo.gi')):
            return c
    if arg:
        raise Fail(f'no CS2 in {arg} (looked for game/csgo/gameinfo.gi)')
    raise Fail('no Counter-Strike 2 install found in the Steam libraries; give its folder with --game')


def list_maps(game):
    skip = re.compile(r'(_vanity|^graphics_settings|^lobby_|^workshop_preview_)')
    return sorted(os.path.basename(p)[:-4] for p in glob.glob(os.path.join(game, 'csgo', 'maps', '*.vpk')) if not skip.search(os.path.basename(p)[:-4]))


# ---------------------------------------------------------------- Source 2 Viewer

class ShadersTooNew(Exception):
    """Source 2 Viewer can't read the game's shaders, so its materials would come out wrong"""


class VRF:
    def __init__(self, exe):
        self.exe = exe
        self.prefix = []
        self.env = dict(os.environ, DOTNET_SYSTEM_GLOBALIZATION_INVARIANT='1', DOTNET_CLI_TELEMETRY_OPTOUT='1')
        self.version = self.probe()

    def probe(self):
        for prefix in ([], ['steam-run']):
            if prefix and not shutil.which(prefix[0]):
                continue
            try:
                r = subprocess.run(prefix + [self.exe, '--version'], env=self.env, capture_output=True, text=True, timeout=120)
            except OSError:
                continue
            m = re.search(r'Version:\s*(\S+)', r.stdout)
            if r.returncode == 0 and m:
                self.prefix = prefix
                return m.group(1)
        raise Fail(f"{self.exe} doesn't run here (on NixOS it needs nix-ld or steam-run)")

    def run(self, args, logfile, watch=False):
        """runs it with its output in logfile. With watch, it's stopped as soon as it says it can't read
        the game's shaders (which it does at the first material) and ShadersTooNew is raised."""
        with open(logfile, 'w') as f:
            p = subprocess.Popen(self.prefix + [self.exe] + args, env=self.env, stdout=f, stderr=subprocess.STDOUT)
        tail = ''
        with open(logfile, encoding='utf-8', errors='replace') as out:
            while True:
                done = p.poll() is not None
                new = out.read()
                if watch and VCS_ERROR in tail + new:
                    p.kill()
                    p.wait()
                    raise ShadersTooNew(self.version)
                tail = (tail + new)[-len(VCS_ERROR):]
                if done:
                    break
                time.sleep(0.2)
        text = open(logfile, encoding='utf-8', errors='replace').read()
        if VCS_ERROR in text:
            warn(f'Source 2 Viewer {self.version} is older than the shaders of this CS2 build, so some materials lose their '
                 'transparency or tint')
        if p.returncode != 0:
            tail = '\n'.join(text.strip().splitlines()[-8:])
            raise Fail(f'Source2Viewer-CLI failed ({p.returncode}), see {logfile}:\n{tail}')
        return text


def cache_dir():
    return os.path.join(os.environ.get('XDG_CACHE_HOME') or os.path.expanduser('~/.cache'), 'hypr3d', 'source2viewer')


def find_vrf(arg):
    for c in (arg, os.environ.get('SOURCE2VIEWER_CLI'), shutil.which('Source2Viewer-CLI')):
        if c:
            if not os.path.isfile(c):
                raise Fail(f'{c} not found')
            return VRF(c)
    # the newest one there: a build made because a release was too old is newer than that release
    found = sorted(glob.glob(os.path.join(cache_dir(), '*', 'Source2Viewer-CLI')), key=os.path.getmtime, reverse=True)
    if found:
        return VRF(found[0])
    return VRF(download_vrf(latest_release()))


def version_key(v):
    """'20.0', 'v21.1', '20.0.6980+a06886f' -> (20, 0): releases are numbered major.minor, builds add to that"""
    m = re.match(r'v?(\d+)\.(\d+)', v or '')
    return (int(m.group(1)), int(m.group(2))) if m else (0, 0)


def latest_release():
    try:
        with urllib.request.urlopen(RELEASES, timeout=60) as r:
            return json.load(r)
    except (OSError, ValueError) as e:
        raise Fail(f"couldn't ask GitHub for Source 2 Viewer's latest release ({e})")


def download_vrf(rel):
    log(f'downloading Source2Viewer-CLI {rel.get("tag_name")} ...')
    try:
        asset = next(a for a in rel['assets'] if a['name'] == 'cli-linux-x64.zip')
        dest = os.path.join(cache_dir(), rel['tag_name'])
        os.makedirs(dest, exist_ok=True)
        with urllib.request.urlopen(asset['browser_download_url'], timeout=600) as r:
            data = r.read()
    except (OSError, ValueError, KeyError, StopIteration) as e:
        raise Fail(f'could not download Source2Viewer-CLI ({e}); get cli-linux-x64.zip from {REPO}/releases and pass --vrf')
    zipfile.ZipFile(io.BytesIO(data)).extractall(dest)
    exe = os.path.join(dest, 'Source2Viewer-CLI')
    os.chmod(exe, 0o755)
    log(f'Source2Viewer-CLI {rel["tag_name"]} is in {dest}')
    return exe


def build_vrf(work):
    """builds Source2Viewer-CLI from the current source into the cache, returns it"""
    git = shutil.which('git')
    if not git:
        raise Fail('building Source 2 Viewer needs git')
    src = os.path.join(cache_dir(), 'src')
    logfile = os.path.join(work, 'vrf-build.log')
    # .NET keeps its packages and settings with the build, not in ~/.nuget and ~/.dotnet
    env = dict(os.environ, DOTNET_CLI_TELEMETRY_OPTOUT='1', DOTNET_NOLOGO='1', NUGET_PACKAGES=os.path.join(cache_dir(), 'nuget'),
               DOTNET_CLI_HOME=os.path.join(cache_dir(), 'dotnet-home'))
    with open(logfile, 'w') as f:
        def step(cmd, cwd=None):
            f.write('$ ' + ' '.join(cmd) + '\n')
            f.flush()
            if subprocess.run(cmd, cwd=cwd, stdout=f, stderr=subprocess.STDOUT, env=env).returncode:
                raise Fail(f'building Source 2 Viewer failed at "{" ".join(cmd[:3])} ...", see {logfile}')

        log('getting the source of Source 2 Viewer ...')
        if os.path.isdir(os.path.join(src, '.git')):
            step([git, '-C', src, 'fetch', '--depth', '1', 'origin', 'HEAD'])
            step([git, '-C', src, 'reset', '--hard', '--quiet', 'FETCH_HEAD'])
        else:
            shutil.rmtree(src, ignore_errors=True)
            step([git, 'clone', '--quiet', '--depth', '1', REPO + '.git', src])
        sha = subprocess.run([git, '-C', src, 'rev-parse', '--short', 'HEAD'], capture_output=True, text=True).stdout.strip()
        dest = os.path.join(cache_dir(), f'master-{sha}')
        exe = os.path.join(dest, 'Source2Viewer-CLI')
        if os.path.isfile(exe):
            return exe
        if shutil.which('dotnet'):
            dotnet = ['dotnet']
        elif shutil.which('nix'):
            dotnet = ['nix', 'shell', 'nixpkgs#dotnet-sdk_10', '--command', 'dotnet']
        else:
            raise Fail('building Source 2 Viewer needs the .NET 10 SDK (dotnet) or nix')
        log(f'building Source 2 Viewer {sha} (a few minutes) ...')
        step(dotnet + ['publish', 'CLI/CLI.csproj', '-c', 'Release', '-r', 'linux-x64', '--self-contained', '-o', dest], cwd=src)
    if not os.path.isfile(exe):
        raise Fail(f'building Source 2 Viewer wrote no {exe}, see {logfile}')
    log(f'Source2Viewer-CLI {sha} is in {dest}')
    return exe


def newer_vrf(current, work):
    """one that can read the shaders `current` can't: the latest release if it's newer, else a build of
    the current source"""
    try:
        rel = latest_release()
        if version_key(rel.get('tag_name')) > version_key(current.version):
            exe = os.path.join(cache_dir(), rel['tag_name'], 'Source2Viewer-CLI')
            return VRF(exe if os.path.isfile(exe) else download_vrf(rel))
    except Fail as e:
        warn(str(e))
    exe = build_vrf(work)
    if os.path.realpath(exe) == os.path.realpath(current.exe):
        raise Fail(f'the current source of Source 2 Viewer is what {current.exe} was built from')
    return VRF(exe)


# ---------------------------------------------------------------- entities

def parse_ents(path):
    """Source 2 Viewer's dump of an entity lump: '====N====' then 'key value' lines"""
    ents, cur = [], None
    for line in open(path, encoding='utf-8', errors='replace'):
        line = line.rstrip('\n')
        if re.match(r'^====\d+====$', line):
            cur = {}
            ents.append(cur)
            continue
        m = re.match(r'^(\S+)\s+(.*)$', line)
        if cur is None or not m:
            continue
        k, v = m.group(1), m.group(2).strip()
        if len(v) >= 2 and v[0] == '"' and v[-1] == '"':
            v = v[1:-1]
        elif v.startswith('['):
            try:
                v = [float(x) for x in v.strip('[] ').split(',')]
            except ValueError:
                pass
        cur[k] = v
    return ents


def vec(e, key, n=3):
    """a vector key of an entity, whether it's [x, y, z] or 'x y z'"""
    v = e.get(key)
    if isinstance(v, list):
        return (v + [0.0] * n)[:n]
    try:
        return ([float(t) for t in str(v).split()] + [0.0] * n)[:n]
    except ValueError:
        return [0.0] * n


def resource(v):
    """'resource_name:"maps/x.vmdl"' -> 'maps/x.vmdl'"""
    m = re.search(r'"([^"]+)"', v or '')
    # some lumps write the path's separators as (escaped) backslashes
    return re.sub(r'\\+', '/', m.group(1) if m else (v or ''))


def truthy(v):
    return str(v).strip().lower() in ('1', 'true', 'yes')


def num(e, key, default):
    """a number key of an entity (a 0 stays 0)"""
    try:
        return float(e.get(key, default))
    except (TypeError, ValueError):
        return default


def sun_channel(sun):
    """the channel (0-3) of the sun's baked shadow in the lightmaps' direct_light_shadows and the probes' atlas, or -1.
    CS2's shaders shadow the sun by (1 - dot(those, a mask with a 1 at that channel)) times its realtime shadow on every
    surface, so with no channel (the key missing, as it is for Dynamic suns and fully baked ones) only the realtime
    shadow is left. A Baked sun (directlight 1) without baked_light_indexing is fully baked and has none either"""
    if not sun or 'bakedshadowindex' not in sun:
        return -1
    if int(num(sun, 'directlight', 1)) == 1 and not truthy(sun.get('baked_light_indexing', 'true')):
        return -1
    c = int(num(sun, 'bakedshadowindex', -1))
    return c if 0 <= c < 4 else -1


def sun_runtime(sun):
    """whether CS2 lights with the sun at run time: a Dynamic (2) or Stationary (3) one, or a Baked (1) one with a baked
    shadow channel ("Stationary Light Shadows"). A fully baked sun's direct light is in the lightmaps and the probes
    already; a disabled or dark one gives none"""
    if not sun or not truthy(sun.get('enabled', 'true')) or num(sun, 'brightness', 1) * num(sun, 'brightnessscale', 1) <= 0:
        return False
    dl = int(num(sun, 'directlight', 1))
    return dl in (2, 3) or (dl == 1 and sun_channel(sun) >= 0)


# ---------------------------------------------------------------- glTF documents

class Doc:
    """a glTF file with its buffers in memory and its images as paths"""

    def __init__(self, path=None):
        self.j = {'asset': {'version': '2.0'}, 'scenes': [{'nodes': []}], 'scene': 0}
        self.buffers = []  # bytes, one per glTF buffer
        self.images = []   # per glTF image: a file, or None when it's in a buffer
        if path:
            self.load(path)

    def load(self, path):
        base = os.path.dirname(os.path.abspath(path))
        self.j = json.load(open(path, encoding='utf-8'))
        for b in self.j.get('buffers', []):
            self.buffers.append(open(os.path.join(base, urllib.request.url2pathname(b['uri'])), 'rb').read())
        for im in self.j.get('images', []):
            self.images.append(os.path.join(base, urllib.request.url2pathname(im['uri'])) if 'uri' in im else None)
        self.j.setdefault('scenes', [{'nodes': []}])

    def list(self, key):
        return self.j.setdefault(key, [])

    @property
    def roots(self):
        return self.j['scenes'][self.j.get('scene', 0)]['nodes']

    def add(self, key, obj):
        self.list(key).append(obj)
        return len(self.j[key]) - 1

    def add_data(self, data, target=None):
        """a buffer view over new bytes"""
        self.buffers.append(bytes(data))
        b = self.add('buffers', {'byteLength': len(data)})
        bv = {'buffer': b, 'byteOffset': 0, 'byteLength': len(data)}
        if target:
            bv['target'] = target
        return self.add('bufferViews', bv)

    def read(self, ai):
        """an accessor's numbers, row after row, in one flat tuple"""
        a = self.j['accessors'][ai]
        fmt, n = COMPONENT[a['componentType']], WIDTH[a['type']]
        if 'bufferView' not in a:
            return (0,) * (a['count'] * n)
        bv = self.j['bufferViews'][a['bufferView']]
        row = struct.Struct(f'<{n}{fmt}')
        stride = bv.get('byteStride') or row.size
        buf, at = self.buffers[bv['buffer']], bv.get('byteOffset', 0) + a.get('byteOffset', 0)
        if stride == row.size:
            return struct.unpack_from(f'<{a["count"] * n}{fmt}', buf, at)
        return tuple(x for i in range(a['count']) for x in row.unpack_from(buf, at + i * stride))

    def write(self, ai, values):
        """new numbers for an accessor, in place (laid out as read gives them), with its min and max"""
        a = self.j['accessors'][ai]
        fmt, n = COMPONENT[a['componentType']], WIDTH[a['type']]
        bv = self.j['bufferViews'][a['bufferView']]
        row = struct.Struct(f'<{n}{fmt}')
        stride = bv.get('byteStride') or row.size
        if not isinstance(self.buffers[bv['buffer']], bytearray):
            self.buffers[bv['buffer']] = bytearray(self.buffers[bv['buffer']])
        buf, at = self.buffers[bv['buffer']], bv.get('byteOffset', 0) + a.get('byteOffset', 0)
        values = struct.unpack(f'<{len(values)}{fmt}', struct.pack(f'<{len(values)}{fmt}', *values))  # as stored
        for i in range(a['count']):
            row.pack_into(buf, at + i * stride, *values[i * n:(i + 1) * n])
        if 'min' in a or 'max' in a:
            a['min'] = [min(values[k::n]) for k in range(n)]
            a['max'] = [max(values[k::n]) for k in range(n)]

    def placed(self):
        """(node, its matrix in the scene) for every node of the scene"""
        stack = [(r, affine()) for r in self.roots]
        while stack:
            n, parent = stack.pop()
            nd = self.j['nodes'][n]
            m = mat_mul(parent, node_matrix(nd))
            yield nd, m
            stack.extend((c, m) for c in nd.get('children', []))

    def add_accessor(self, fmt, rows, type_, component, minmax=False):
        data = b''.join(struct.pack('<' + fmt, *r) for r in rows)
        acc = {'bufferView': self.add_data(data), 'componentType': component, 'count': len(rows), 'type': type_}
        if minmax:
            acc['min'] = [min(r[k] for r in rows) for k in range(len(rows[0]))]
            acc['max'] = [max(r[k] for r in rows) for k in range(len(rows[0]))]
        return self.add('accessors', acc)

    def add_image(self, path, name=None):
        for i, p in enumerate(self.images):
            if p and os.path.abspath(p) == os.path.abspath(path):
                return i
        self.images.append(path)
        return self.add('images', {'name': name or os.path.splitext(os.path.basename(path))[0], 'uri': os.path.basename(path)})

    def add_texture(self, image):
        for i, t in enumerate(self.list('textures')):
            if t.get('source') == image:
                return i
        t = {'source': image}
        if self.list('samplers'):
            t['sampler'] = 0
        return self.add('textures', t)

    def merge(self, other, name, matrix):
        """puts all of other's scene under a new node of this one"""
        off = {k: len(self.list(k)) for k in ('accessors', 'bufferViews', 'buffers', 'images', 'samplers', 'textures', 'materials', 'meshes', 'nodes')}
        o = other.j
        self.buffers += other.buffers
        self.list('buffers').extend({'byteLength': len(b)} for b in other.buffers)
        for bv in o.get('bufferViews', []):
            self.list('bufferViews').append(dict(bv, buffer=bv['buffer'] + off['buffers']))
        for a in o.get('accessors', []):
            a = json.loads(json.dumps(a))
            if 'bufferView' in a:
                a['bufferView'] += off['bufferViews']
            for part in ('indices', 'values'):
                if 'sparse' in a and 'bufferView' in a['sparse'].get(part, {}):
                    a['sparse'][part]['bufferView'] += off['bufferViews']
            self.list('accessors').append(a)
        for im, path in zip(o.get('images', []), other.images):
            im = dict(im)
            if 'bufferView' in im:
                im['bufferView'] += off['bufferViews']
            self.list('images').append(im)
            self.images.append(path)
        self.list('samplers').extend(o.get('samplers', []))
        for t in o.get('textures', []):
            t = dict(t)
            if 'source' in t:
                t['source'] += off['images']
            if 'sampler' in t:
                t['sampler'] += off['samplers']
            self.list('textures').append(t)
        for m in o.get('materials', []):
            m = json.loads(json.dumps(m))
            for ref in texture_refs(m):
                ref['index'] += off['textures']
            self.list('materials').append(m)
        for me in o.get('meshes', []):
            me = json.loads(json.dumps(me))
            for p in me['primitives']:
                p['attributes'] = {k: v + off['accessors'] for k, v in p['attributes'].items()}
                if 'indices' in p:
                    p['indices'] += off['accessors']
                if 'material' in p:
                    p['material'] += off['materials']
                for t in p.get('targets', []):
                    for k in t:
                        t[k] += off['accessors']
            self.list('meshes').append(me)
        for n in o.get('nodes', []):
            n = {k: v for k, v in n.items() if k not in ('extensions', 'skin', 'camera')}  # its own sun, bones
            if 'mesh' in n:
                n['mesh'] += off['meshes']
            if 'children' in n:
                n['children'] = [c + off['nodes'] for c in n['children']]
            self.list('nodes').append(n)
        roots = o['scenes'][o.get('scene', 0)]['nodes']
        self.roots.append(self.add('nodes', {'name': name, 'matrix': matrix, 'children': [r + off['nodes'] for r in roots]}))

    def compact(self):
        """only what the scene uses, renumbered: the new JSON, the buffer views as (view, bytes) and the
        images as (image, file or bytes)"""
        j = self.j
        nodes, stack = [], list(reversed(self.roots))
        seen = set()
        while stack:
            n = stack.pop()
            if n in seen:
                continue
            seen.add(n)
            nodes.append(n)
            stack.extend(reversed(j['nodes'][n].get('children', [])))
        nmap = {n: i for i, n in enumerate(nodes)}

        def remap(order):
            return {v: i for i, v in enumerate(order)}

        meshes = sorted({j['nodes'][n]['mesh'] for n in nodes if 'mesh' in j['nodes'][n]})
        mmap = remap(meshes)
        prims = [p for m in meshes for p in j['meshes'][m]['primitives']]
        mats = sorted({p['material'] for p in prims if 'material' in p})
        matmap = remap(mats)
        texs = sorted({r['index'] for m in mats for r in texture_refs(j['materials'][m])})
        tmap = remap(texs)
        # images that aren't a material's texture: the lightmaps and probes
        extra = list(image_refs(j.get('extensions', {})))
        imgs = sorted({j['textures'][t]['source'] for t in texs if 'source' in j['textures'][t]} | {r['image'] for r in extra})
        imap = remap(imgs)
        samps = sorted({j['textures'][t]['sampler'] for t in texs if 'sampler' in j['textures'][t]})
        smap = remap(samps)
        accs = set()
        for p in prims:
            accs.update(p['attributes'].values())
            if 'indices' in p:
                accs.add(p['indices'])
            for t in p.get('targets', []):
                accs.update(t.values())
        accs = sorted(accs)
        amap = remap(accs)
        bvs = sorted({j['accessors'][a]['bufferView'] for a in accs if 'bufferView' in j['accessors'][a]} |
                     {s[part]['bufferView'] for a in accs for s in [j['accessors'][a].get('sparse', {})] for part in ('indices', 'values') if part in s})
        bvmap = remap(bvs)

        out = {k: v for k, v in j.items() if k not in ('nodes', 'meshes', 'materials', 'textures', 'images', 'samplers', 'accessors', 'bufferViews', 'buffers', 'scenes', 'scene')}
        if 'extensions' in out:
            out['extensions'] = json.loads(json.dumps(out['extensions']))
            for r in image_refs(out['extensions']):
                r['image'] = imap[r['image']]
        out['scene'] = 0
        out['scenes'] = [{'nodes': [nmap[r] for r in self.roots]}]
        out['nodes'] = []
        for n in nodes:
            nd = dict(j['nodes'][n])
            if 'mesh' in nd:
                nd['mesh'] = mmap[nd['mesh']]
            if 'children' in nd:
                nd['children'] = [nmap[c] for c in nd['children']]
            out['nodes'].append(nd)
        out['meshes'] = []
        for m in meshes:
            me = json.loads(json.dumps(j['meshes'][m]))
            for p in me['primitives']:
                p['attributes'] = {k: amap[v] for k, v in p['attributes'].items()}
                if 'indices' in p:
                    p['indices'] = amap[p['indices']]
                if 'material' in p:
                    p['material'] = matmap[p['material']]
                for t in p.get('targets', []):
                    for k in t:
                        t[k] = amap[t[k]]
            out['meshes'].append(me)
        out['materials'] = []
        for m in mats:
            mt = json.loads(json.dumps(j['materials'][m]))
            for r in texture_refs(mt):
                r['index'] = tmap[r['index']]
            out['materials'].append(mt)
        out['textures'] = []
        for t in texs:
            tx = dict(j['textures'][t])
            if 'source' in tx:
                tx['source'] = imap[tx['source']]
            if 'sampler' in tx:
                tx['sampler'] = smap[tx['sampler']]
            out['textures'].append(tx)
        out['samplers'] = [j['samplers'][s] for s in samps]
        out['accessors'] = []
        for a in accs:
            ac = json.loads(json.dumps(j['accessors'][a]))
            if 'bufferView' in ac:
                ac['bufferView'] = bvmap[ac['bufferView']]
            for part in ('indices', 'values'):
                if part in ac.get('sparse', {}):
                    ac['sparse'][part]['bufferView'] = bvmap[ac['sparse'][part]['bufferView']]
            out['accessors'].append(ac)
        def view(bv):
            start = bv.get('byteOffset', 0)
            return memoryview(self.buffers[bv['buffer']])[start:start + bv['byteLength']]

        views = [(j['bufferViews'][b], view(j['bufferViews'][b])) for b in bvs]
        images = [(j['images'][i], self.images[i] or view(j['bufferViews'][j['images'][i]['bufferView']])) for i in imgs]
        return out, views, images

    def write_glb(self, path):
        """one .glb with every buffer and image in it, written as it goes (the images alone can be
        hundreds of MB)"""
        out, views, images = self.compact()
        pieces, size = [], 0  # (bytes or file, length) in the order they go in the binary chunk

        def place(data, length):
            nonlocal size
            at = size
            pieces.append((data, length))
            size += length + (-length % 4)
            return at

        out['bufferViews'] = []
        for bv, data in views:
            nbv = {k: v for k, v in bv.items() if k != 'buffer'}
            nbv.update(buffer=0, byteOffset=place(data, len(data)), byteLength=len(data))
            out['bufferViews'].append(nbv)
        out['images'] = []
        for im, data in images:
            length = os.path.getsize(data) if isinstance(data, str) else len(data)
            head = open(data, 'rb').read(2) if isinstance(data, str) else bytes(data[:2])
            nim = {k: v for k, v in im.items() if k not in ('uri', 'bufferView', 'mimeType')}
            nim['bufferView'] = len(out['bufferViews'])
            nim['mimeType'] = 'image/jpeg' if head == b'\xff\xd8' else 'image/png'
            out['bufferViews'].append({'buffer': 0, 'byteOffset': place(data, length), 'byteLength': length})
            out['images'].append(nim)
        out['buffers'] = [{'byteLength': size}]
        text = json.dumps(out, separators=(',', ':')).encode()
        text += b' ' * (-len(text) % 4)
        tmp = path + '.part'
        with open(tmp, 'wb') as f:
            f.write(struct.pack('<III', 0x46546C67, 2, 12 + 8 + len(text) + 8 + size))
            f.write(struct.pack('<II', len(text), 0x4E4F534A) + text)
            f.write(struct.pack('<II', size, 0x004E4942))
            for data, length in pieces:
                if isinstance(data, str):
                    with open(data, 'rb') as src:
                        shutil.copyfileobj(src, f, 1 << 20)
                else:
                    f.write(data)
                f.write(b'\0' * (-length % 4))
        os.replace(tmp, path)
        return out


def texture_refs(m):
    """the {"index": n} texture references of a material, including ours"""
    for key in ('normalTexture', 'occlusionTexture', 'emissiveTexture'):
        if key in m:
            yield m[key]
    pbr = m.get('pbrMetallicRoughness', {})
    for key in ('baseColorTexture', 'metallicRoughnessTexture'):
        if key in pbr:
            yield pbr[key]
    ext = m.get('extensions', {}).get(EXT, {})
    for key in ('texture', 'maskTexture', 'normalTexture'):
        if key in ext:
            yield ext[key]
    detail = m.get('extensions', {}).get(EXT_S2, {}).get('detail', {})
    for key in ('texture', 'maskTexture'):
        if key in detail:
            yield detail[key]
    for mask in m.get('extensions', {}).get(EXT_S2, {}).get('effect', {}).get('masks', []):
        yield mask['texture']


def image_refs(obj):
    """the {"image": n} references in a JSON tree (HYPR3D_lighting's)"""
    if isinstance(obj, dict):
        if isinstance(obj.get('image'), int):
            yield obj
        for v in obj.values():
            yield from image_refs(v)
    elif isinstance(obj, list):
        for v in obj:
            yield from image_refs(v)


def vmat(m):
    return m.get('extras', {}).get('vmat', {}) if isinstance(m.get('extras'), dict) else {}


# ---------------------------------------------------------------- the export

class Export:
    def __init__(self, args):
        self.args = args
        self.game = find_game(args.game)
        self.gameinfo = os.path.join(self.game, 'csgo', 'gameinfo.gi')
        if args.map.endswith('.vpk') or os.sep in args.map:
            self.vpk = os.path.abspath(os.path.expanduser(args.map))
            self.name = os.path.basename(self.vpk)[:-4]
        else:
            self.name = args.map
            self.vpk = os.path.join(self.game, 'csgo', 'maps', self.name + '.vpk')
        if not os.path.isfile(self.vpk):
            raise Fail(f'no map {self.name} in {os.path.join(self.game, "csgo", "maps")} (see --list)')
        self.out = os.path.abspath(os.path.expanduser(args.output or os.path.join(
            os.environ.get('XDG_DATA_HOME') or os.path.expanduser('~/.local/share'), 'hypr3d', 'maps', self.name + '.glb')))
        self.vrf = find_vrf(args.vrf)
        self.stuck = False  # no Source 2 Viewer that reads the game's shaders to be had
        self.skybox = None  # the 3D skybox, when there is one: its .vpk, map path, entities and placement
        log(f'{self.name} from {self.vpk}, with Source2Viewer-CLI {self.vrf.version}')
        # everything the game can read files from, for textures that aren't in the map's own .vpk
        self.paks = [self.vpk] + [p for p in (os.path.join(self.game, d, 'pak01_dir.vpk') for d in ('csgo', 'csgo_imported', 'csgo_core', 'core')) if os.path.isfile(p)]

    def vrf_export(self, vpk, path, out, logname):
        args = ['-i', vpk, '-f', path, '--game', self.gameinfo, '-o', out, '-d', '--gltf_export_format', 'gltf', '--gltf_export_materials',
                '--gltf_textures_adapt', '--gltf_export_extras']
        logfile = os.path.join(self.work, logname)
        # a Source 2 Viewer older than the game's shaders gets a newer one: the latest release, or a build
        # of the current source when that's older too
        upgrades = 0
        while True:
            try:
                self.vrf.run(args, logfile, watch=not self.stuck)
                break
            except ShadersTooNew:
                log(f"Source 2 Viewer {self.vrf.version} can't read the shaders of this CS2 build, getting a newer one ...")
                try:
                    if upgrades == 2:
                        raise Fail("the newest Source 2 Viewer can't read them either")
                    upgrades += 1
                    self.vrf = newer_vrf(self.vrf, self.work)
                    log(f'now with Source2Viewer-CLI {self.vrf.version}')
                except Fail as e:
                    warn(f"{e}; exporting with Source 2 Viewer {self.vrf.version} anyway, so some materials lose their transparency or tint")
                    self.stuck = True
        if not os.path.isfile(out):
            raise Fail(f'Source2Viewer-CLI wrote no {os.path.basename(out)}, see {logfile}')
        return Doc(out)

    def vrf_files(self, paths, outdir, logname):
        """decompiles files (e.g. materials/x.vtex_c) from whichever .vpk has them; returns {path: local file}"""
        found, left = {}, list(paths)
        for i, pak in enumerate(self.paks):
            if not left:
                break
            self.vrf.run(['-i', pak, '-f', ','.join(left), '-d', '-o', outdir], os.path.join(self.work, f'{logname}{i}.log'))
            for p in list(left):
                # x.vmat_c comes out as x.vmat, a texture x.vtex_c as x.png or x.exr
                base = os.path.join(outdir, p[:-2] if p.endswith('_c') else p)
                hits = [base] if os.path.isfile(base) else \
                    [h for h in glob.glob(glob.escape(os.path.splitext(base)[0]) + '.*') if h.endswith(('.png', '.exr', '.jpg', '.tga'))]
                if hits:
                    found[p] = hits[0]
                    left.remove(p)
        return found

    def entities(self, vpk, lump, sub):
        out = os.path.join(self.work, sub)
        self.vrf.run(['-i', vpk, '-f', lump, '-d', '-o', out], os.path.join(self.work, sub + '.log'))
        dumps = glob.glob(os.path.join(out, '**', '*.vents'), recursive=True)
        return parse_ents(dumps[0]) if dumps else []

    def run(self):
        args = self.args
        self.work = os.path.abspath(args.keep) if args.keep else tempfile.mkdtemp(prefix='cs2map-')
        os.makedirs(self.work, exist_ok=True)
        try:
            self.convert()
        finally:
            if not args.keep:
                shutil.rmtree(self.work, ignore_errors=True)

    def convert(self):
        args = self.args
        t0 = time.time()
        log('exporting the map (a minute or so) ...')
        doc = self.vrf_export(self.vpk, f'maps/{self.name}.vmap_c', os.path.join(self.work, 'map', self.name + '.gltf'), 'map.log')
        ents = self.entities(self.vpk, f'maps/{self.name}/entities/default_ents.vents_c', 'ents')
        if not ents:
            warn('the map has no entity lump, so no spawn points or disabled entities')
        log(f'{len(doc.j.get("meshes", []))} meshes, {len(doc.j.get("materials", []))} materials, {len(doc.images)} textures, {len(ents)} entities')

        self.drop_disabled(doc, ents)
        if not args.no_skybox:
            self.add_skybox(doc, ents)
        self.fix_decals(doc)
        self.fix_materials(doc)
        sky = None if args.no_sky else self.add_sky(doc, ents)
        if not args.no_lighting:
            self.add_lighting(doc, ents, sky)
        self.add_anchors(doc, ents)

        for m in doc.list('materials'):
            m.pop('extras', None)
        doc.j['asset']['generator'] = f'{doc.j["asset"].get("generator", "Source 2 Viewer")}, cs2map (hypr3d)'
        doc.j['asset']['copyright'] = 'Valve Corporation; exported from a local copy of Counter-Strike 2 for personal use'
        for ext in (EXT, EXT_S2):
            if any(ext in m.get('extensions', {}) for m in doc.list('materials')):
                used = doc.j.setdefault('extensionsUsed', [])
                if ext not in used:
                    used.append(ext)

        os.makedirs(os.path.dirname(self.out), exist_ok=True)
        log(f'writing {self.out} ...')
        out = doc.write_glb(self.out)
        size = os.path.getsize(self.out)
        log(f'wrote {self.out}: {len(out["meshes"])} meshes, {len(out["materials"])} materials, {len(out["images"])} textures, '
            f'{size / 1e6:.0f} MB, in {time.time() - t0:.0f} s')
        log(f'load it with: hyprctl hypr3d map {self.out}')
        log(f'or keep it: plugin:hypr3d:map = {self.out}')

    # ------------------------------------------------ entities that start disabled

    def entity_nodes(self, doc):
        """the nodes of the scene that come from entity models, by the model's file name"""
        by = {}
        for r in doc.roots:
            name = doc.j['nodes'][r].get('name', '')
            by.setdefault(name.split('.')[0].lower(), []).append(r)
        return by

    def drop_disabled(self, doc, ents):
        by = self.entity_nodes(doc)
        gone = set()
        for e in ents:
            model = resource(e.get('model', ''))
            if not model or not truthy(e.get('startdisabled', 'false')):
                continue
            stem = os.path.splitext(os.path.basename(model))[0].lower()
            at = to_gltf(vec(e, 'origin'))
            for r in by.get(stem, []):
                m = doc.j['nodes'][r].get('matrix')
                t = m[12:15] if m else doc.j['nodes'][r].get('translation', [0, 0, 0])
                if math.dist(t, at) < 0.05:
                    gone.add(r)
        if gone:
            doc.j['scenes'][doc.j.get('scene', 0)]['nodes'] = [r for r in doc.roots if r not in gone]
            log(f'left out {len(gone)} parts of entities that start disabled')

    # ------------------------------------------------ the 3D skybox

    def add_skybox(self, doc, ents):
        ref = next((e for e in ents if e.get('classname') == 'skybox_reference'), None)
        if not ref:
            return
        target = ref.get('targetmapname', '')
        vpk = os.path.join(self.game, 'csgo', os.path.splitext(target)[0] + '.vpk')
        if not target or not os.path.isfile(vpk):
            warn(f'the 3D skybox {target or "(unnamed)"} is missing, leaving it out')
            return
        log(f'exporting the 3D skybox {os.path.basename(target)} ...')
        vmap = os.path.splitext(target)[0] + '.vmap_c'
        sky = self.vrf_export(vpk, vmap, os.path.join(self.work, 'skybox', 'skybox.gltf'), 'skybox.log')
        sents = self.entities(vpk, os.path.splitext(target)[0] + '/entities/default_ents.vents_c', 'skybox_ents')
        cam = next((e for e in sents if e.get('classname') == 'sky_camera'), None)
        if not cam:
            warn('the 3D skybox has no sky_camera, leaving it out')
            return
        s = float(cam.get('scale', 16) or 16)
        c = to_gltf(vec(cam, 'origin'))
        o = to_gltf(vec(ref, 'origin'))
        # what's at the sky camera in the skybox is at the reference's origin in the map, s times bigger
        t = [o[k] - c[k] * s for k in range(3)]
        doc.merge(sky, 'hypr3d_backdrop', [s, 0, 0, 0, 0, s, 0, 0, 0, 0, s, 0, t[0], t[1], t[2], 1])
        self.skybox = {'vpk': vpk, 'path': os.path.splitext(target)[0], 'ents': sents, 'scale': s, 'offset': t}
        log(f'3D skybox: {len(sky.j.get("meshes", []))} meshes at {s:g}x')

    # ------------------------------------------------ decals

    # Source 2 Viewer lifts decals (the materials it takes for overlays) 1 cm off what they're on, along
    # their normals, so that viewers without depth bias don't show the surface through them. Since release
    # 20 puts the meters into the vertices, what it adds, 1 cm in Source's inches (0.01 / 0.0254), comes
    # out as that many meters: the bombsite letters float 39 cm above the floor (a step you walk up onto),
    # and the 3D skybox's, 16 times bigger, stand 6 m off its walls.
    DECAL_LIFT = 0.01
    DECAL_LIFT_WRONG = 0.01 / INCH
    DECAL_REACH = 0.6  # how far behind a decal to look for what it's on, in its mesh's meters

    @staticmethod
    def is_decal(v):
        """the materials Source 2 Viewer lifts (its IsMaterialOverlay)"""
        ip, s = v.get('IntParams', {}), v.get('ShaderName', '')
        return any(int(ip.get(k, 0)) == 1 for k in ('F_OVERLAY', 'F_DEPTHBIAS', 'F_DEPTH_BIAS')) or \
            s.endswith('static_overlay.vfx') or s == 'citadel_overlay.vfx'

    @staticmethod
    def decal_rays(doc, p, m, count=3):
        """rays from the middles of a decal's biggest triangles back to what it's on, in the scene: (origin,
        normal there), the normal as long as the mesh's are in the scene"""
        P = doc.read(p['attributes']['POSITION'])
        N = doc.read(p['attributes']['NORMAL'])
        I = doc.read(p['indices']) if 'indices' in p else range(len(P) // 3)
        tris = []
        for t in range(0, len(I) - 2, 3):
            a, b, c = 3 * I[t], 3 * I[t + 1], 3 * I[t + 2]
            e = [P[b + i] - P[a + i] for i in range(3)]
            f = [P[c + i] - P[a + i] for i in range(3)]
            area = (e[1] * f[2] - e[2] * f[1]) ** 2 + (e[2] * f[0] - e[0] * f[2]) ** 2 + (e[0] * f[1] - e[1] * f[0]) ** 2
            if area > 0:
                tris.append((area, a, b, c))
        out = []
        for _, a, b, c in sorted(tris, reverse=True)[:count]:
            mid = [(P[a + i] + P[b + i] + P[c + i]) / 3 for i in range(3)]
            n = [(N[a + i] + N[b + i] + N[c + i]) / 3 for i in range(3)]
            out.append((apply(m, mid), [sum(m[i][k] * n[k] for k in range(3)) for i in range(3)]))
        return out

    def decal_gaps(self, doc, rays, targets):
        """for each ray (origin o, normal v), how many normals back along it (o - t v) the nearest of the
        targets' triangles is, up to DECAL_REACH; None when there's none"""
        j = doc.j
        near, far = 0.002, self.DECAL_REACH
        boxes = []
        for o, v in rays:
            ends = [[o[i] - t * v[i] for i in range(3)] for t in (near, far)]
            boxes.append(([min(e[i] for e in ends) for i in range(3)], [max(e[i] for e in ends) for i in range(3)]))
        # sorted by where they start along x, to find those that reach a mesh
        order = sorted(range(len(rays)), key=lambda k: boxes[k][0][0])
        xs = [boxes[k][0][0] for k in order]
        widest = max((hi[0] - lo[0] for lo, hi in boxes), default=0)
        gaps = [None] * len(rays)
        # the world's meshes share vertices by the hundred: each set of them once, in each place it's in
        groups = {}
        for p, m in targets:
            key = (p['attributes']['POSITION'], tuple(x for row in m[:3] for x in row))
            groups.setdefault(key, (m, []))[1].append(p)
        spaces = {}  # by matrix: its inverse, and the rays already moved into it
        for (ai, mkey), (m, prims) in groups.items():
            a = j['accessors'][ai]
            if 'min' not in a or 'max' not in a:
                continue
            corners = [apply(m, [a['max'][i] if (c >> i) & 1 else a['min'][i] for i in range(3)]) for c in range(8)]
            lo = [min(q[i] for q in corners) for i in range(3)]
            hi = [max(q[i] for q in corners) for i in range(3)]
            mine = [k for k in order[bisect.bisect_left(xs, lo[0] - widest):bisect.bisect_right(xs, hi[0])]
                    if all(boxes[k][0][i] <= hi[i] and boxes[k][1][i] >= lo[i] for i in range(3))]
            if not mine:
                continue
            if mkey not in spaces:
                spaces[mkey] = (invert_affine(m), {})
            inv, moved = spaces[mkey]
            if not inv:
                continue
            # the rays in the mesh's own space, where t still counts the same: their boxes first, sorted by
            # where they start along x
            local = []
            for k in mine:
                if k not in moved:
                    o, v = rays[k]
                    o, d = apply(inv, o), [-sum(inv[i][c] * v[c] for c in range(3)) for i in range(3)]
                    ends = [[o[i] + t * d[i] for i in range(3)] for t in (near, far)]
                    moved[k] = (*[f(e[i] for e in ends) for i in range(3) for f in (min, max)], k, *o, *d)
                local.append(moved[k])
            local.sort()
            starts = [r[0] for r in local]
            wide = max(r[1] - r[0] for r in local)
            x0, x1 = starts[0], max(r[1] for r in local)
            y0, y1 = min(r[2] for r in local), max(r[3] for r in local)
            z0, z1 = min(r[4] for r in local), max(r[5] for r in local)
            P = doc.read(ai)
            for p in prims:
                self.cast_rays(P, doc.read(p['indices']) if 'indices' in p else range(len(P) // 3), local, starts, wide,
                               (x0, x1, y0, y1, z0, z1), near, far, gaps)
        return gaps

    @staticmethod
    def cast_rays(P, I, local, starts, wide, box, near, far, gaps):
        """the triangles (flat positions P, indices I) against rays in their space, sorted as decal_gaps
        sorts them: the nearest hit of each goes in gaps"""
        x0, x1, y0, y1, z0, z1 = box
        for t in range(0, len(I) - 2, 3):
            a, b, c = 3 * I[t], 3 * I[t + 1], 3 * I[t + 2]
            ax, bx, cx = P[a], P[b], P[c]
            if (ax < x0 and bx < x0 and cx < x0) or (ax > x1 and bx > x1 and cx > x1):
                continue
            ay, by, cy = P[a + 1], P[b + 1], P[c + 1]
            if (ay < y0 and by < y0 and cy < y0) or (ay > y1 and by > y1 and cy > y1):
                continue
            az, bz, cz = P[a + 2], P[b + 2], P[c + 2]
            if (az < z0 and bz < z0 and cz < z0) or (az > z1 and bz > z1 and cz > z1):
                continue
            tx0, tx1 = min(ax, bx, cx), max(ax, bx, cx)
            ty0, ty1 = min(ay, by, cy), max(ay, by, cy)
            tz0, tz1 = min(az, bz, cz), max(az, bz, cz)
            near_rays = [r for r in local[bisect.bisect_left(starts, tx0 - wide):bisect.bisect_right(starts, tx1)]
                         if r[1] >= tx0 and r[2] <= ty1 and r[3] >= ty0 and r[4] <= tz1 and r[5] >= tz0]
            if not near_rays:
                continue
            # Moller-Trumbore
            e1x, e1y, e1z = bx - ax, by - ay, bz - az
            e2x, e2y, e2z = cx - ax, cy - ay, cz - az
            for _, _, _, _, _, _, k, ox, oy, oz, dx, dy, dz in near_rays:
                hx, hy, hz = dy * e2z - dz * e2y, dz * e2x - dx * e2z, dx * e2y - dy * e2x
                det = e1x * hx + e1y * hy + e1z * hz
                if abs(det) < 1e-15:
                    continue
                sx, sy, sz = ox - ax, oy - ay, oz - az
                u = (sx * hx + sy * hy + sz * hz) / det
                if u < -1e-6 or u > 1 + 1e-6:
                    continue
                qx, qy, qz = sy * e1z - sz * e1y, sz * e1x - sx * e1z, sx * e1y - sy * e1x
                w = (dx * qx + dy * qy + dz * qz) / det
                if w < -1e-6 or u + w > 1 + 1e-6:
                    continue
                hit = (e2x * qx + e2y * qy + e2z * qz) / det
                if near < hit <= far and (gaps[k] is None or hit < gaps[k]):
                    gaps[k] = hit

    def fix_decals(self, doc):
        """puts the decals Source 2 Viewer lifted 39 cm back to 1 cm off what they're on. Whether it did is
        measured, from each decal back along its normals, so that one that gets it right is left alone."""
        j = doc.j
        decal = {i for i, m in enumerate(doc.list('materials')) if self.is_decal(vmat(m))}
        if not decal:
            return
        rays, owner, targets, placed = [], [], [], 0
        for nd, m in doc.placed():
            if 'mesh' not in nd:
                continue
            for p in j['meshes'][nd['mesh']]['primitives']:
                if p.get('mode', 4) != 4:
                    continue
                if p.get('material') not in decal:
                    targets.append((p, m))
                elif 'NORMAL' in p['attributes']:
                    r = self.decal_rays(doc, p, m)
                    rays += r
                    owner += [placed] * len(r)
                    placed += 1
        # each decal by its middle ray (they differ where a decal wraps a corner or a step); the lift adds to
        # what a decal stood off its surface already (de_dust2's window insets: up to 2 cm)
        found = [[] for _ in range(placed)]
        for who, gap in zip(owner, self.decal_gaps(doc, rays, targets)):
            if gap is not None:
                found[who].append(gap)
        gaps = [sorted(g)[len(g) // 2] for g in found if g]
        wrong = sum(1 for g in gaps if -0.004 < g - self.DECAL_LIFT_WRONG < 0.03)
        right = sum(1 for g in gaps if -0.004 < g - self.DECAL_LIFT < 0.03)
        if not wrong or wrong <= right or 3 * wrong < len(gaps):
            log(f'decals: of the {len(gaps)} measured, {right} are 1 cm off what they are on and {wrong} 39 cm; '
                'leaving them where Source 2 Viewer put them')
            return
        # back along the same normals it lifted them by
        drop, done = self.DECAL_LIFT_WRONG - self.DECAL_LIFT, set()
        for me in j['meshes']:
            for p in me['primitives']:
                at = p['attributes']
                if p.get('material') in decal and 'NORMAL' in at and at['POSITION'] not in done:
                    done.add(at['POSITION'])
                    P, N = doc.read(at['POSITION']), doc.read(at['NORMAL'])
                    doc.write(at['POSITION'], [P[i] - drop * N[i] for i in range(len(P))])
        log(f'decals: Source 2 Viewer lifted them 39 cm off what they are on ({wrong} of the {len(gaps)} measured); '
            f'put {len(done)} mesh{"" if len(done) == 1 else "es"} back to 1 cm')

    # ------------------------------------------------ materials

    @staticmethod
    def detail_params(shader, v):
        """a material's detail texture as HYPR3D_materials_source2's "detail" has it, with the vmat's
        texture paths in "texture" and "mask", or None"""
        ip, fp, vp, tp = v.get('IntParams', {}), v.get('FloatParams', {}), v.get('VectorParams', {}), v.get('TextureParams', {})

        def vec2(key, default):
            x = vp.get(key)
            return [float(x[0]), float(x[1])] if isinstance(x, list) and len(x) >= 2 else default

        # csgo_lightmappedgeneric: the first layer's detail, times two, tinted, at a multiple of the
        # colour's uvs (F_DETAILBLENDMODE 0; the other modes aren't used on the maps)
        if shader.startswith('csgo_lightmappedgeneric') and int(ip.get('F_DETAILTEXTURE', 0)) and tp.get('g_tLayer1Detail'):
            if int(ip.get('F_DETAILBLENDMODE', 0)) != 0:
                return None
            tb = vp.get('g_vLayer1DetailTintAndBlend') or [1, 1, 1, 1]
            sx, sy = vec2('g_vLayer1DetailScale', [1.0, 1.0])
            return {'texture': tp['g_tLayer1Detail'], 'mode': 'mod2x', 'blend': float(tb[3]), 'tint': [float(c) for c in tb[:3]],
                    'transform': [sx, 0.0, 0.0, sy, 0.0, 0.0]}
        # csgo_vertexlitgeneric and friends: F_DETAIL_TEXTURE 1 is mod2x, 2 and 4 an overlay (3 and 4's
        # detail normals aren't done), through a mask, with its own uv transform
        mode = int(ip.get('F_DETAIL_TEXTURE', 0))
        if mode not in (1, 2, 4) or not tp.get('g_tDetail'):
            return None
        sx, sy = vec2('g_vDetailTexCoordScale', [1.0, 1.0])
        ox, oy = vec2('g_vDetailTexCoordOffset', [0.0, 0.0])
        a = math.radians(float(fp.get('g_flDetailTexCoordRotation', 0)))
        c, s = math.cos(a), math.sin(a)
        # scale * rotate(uv - 0.5) + 0.5 + offset, as a matrix's columns and an offset
        col0, col1 = [sx * c, sy * s], [-sx * s, sy * c]
        off = [0.5 + ox - 0.5 * (col0[0] + col1[0]), 0.5 + oy - 0.5 * (col0[1] + col1[1])]
        second = int(ip.get('F_FORCE_UV2', 0)) or (int(ip.get('F_SECONDARY_UV', 0)) and int(ip.get('g_bUseSecondaryUvForDetailMask', 1)))
        mask = tp.get('g_tDetailMask')
        return {'texture': tp['g_tDetail'], 'mask': None if not mask or '/default/' in mask else mask, 'mode': 'mod2x' if mode == 1 else 'overlay',
                'blend': float(fp.get('g_flDetailBlendFactor', 1)), 'blendToFull': float(fp.get('g_flDetailBlendToFull', 0)),
                'transform': col0 + col1 + off, 'maskUV': 1 if second else 0}

    @staticmethod
    def effect_params(v, texture):
        """csgo_effects as HYPR3D_materials_source2's "effect" has it: the color times three scrolling masks,
        faded by distance and by how square on it's seen"""
        ip, fp, vp, tp = v.get('IntParams', {}), v.get('FloatParams', {}), v.get('VectorParams', {}), v.get('TextureParams', {})

        def vec2(key, default):
            x = vp.get(key)
            return [float(x[0]), float(x[1])] if isinstance(x, list) and len(x) >= 2 else default

        masks = []
        for k in (1, 2, 3):
            path = tp.get(f'g_tMask{k}')
            tex = texture(path) if path and '/default/' not in path else None
            if tex is not None:
                masks.append({'texture': {'index': tex}, 'scale': vec2(f'g_vMask{k}Scale', [1.0, 1.0]), 'pan': vec2(f'g_vMask{k}PanSpeed', [0.0, 0.0])})
        f = lambda key, d: float(fp.get(key, d))
        return {'masks': masks, 'colorBoost': f('g_flColorBoost', 1), 'opacity': f('g_flOpacityScale', 1),
                'additive': bool(int(ip.get('F_ADDITIVE_BLEND', 0))), 'fog': bool(int(ip.get('g_bFogEnabled', 1))),
                # distances in meters (in the 3D skybox's own units there)
                'fade': [f('g_flFadeDistance', 1) * INCH, f('g_flFadeFalloff', 1), f('g_flFadeMin', 0), f('g_flFadeMax', 1)],
                'fresnel': [f('g_flFresnelExponent', 0.001), f('g_flFresnelFalloff', 1), f('g_flFresnelMin', 0), f('g_flFresnelMax', 1)]}

    def fix_materials(self, doc):
        mats = doc.list('materials')
        shader = [vmat(m).get('ShaderName', '') for m in mats]
        ints = [vmat(m).get('IntParams', {}) for m in mats]
        foliage = {i for i, s in enumerate(shader) if s.startswith('csgo_foliage')}

        # nothing hypr3d can't draw: even the effects shader's clouds, dust sheets and sun glows come
        # along (their scrolling masks, without the softening where they meet the ground)
        drop = set()
        for i, s in enumerate(shader):
            m = mats[i]
            blend = int(ints[i].get('F_BLEND_MODE', 0))
            if s.startswith(('csgo_static_overlay', 'csgo_unlitgeneric')) and blend in (1, 3):
                m['alphaMode'] = 'BLEND'  # translucent decals Source 2 Viewer leaves opaque
            if s.startswith(('csgo_unlitgeneric', 'csgo_black_unlit', 'csgo_effects')) or (s.startswith('csgo_static_overlay') and not int(ints[i].get('F_LIT', 0))):
                m.setdefault('extensions', {})['KHR_materials_unlit'] = {}
            if s.startswith('csgo_effects'):
                m['alphaMode'] = 'BLEND'
            if s.startswith('csgo_black_unlit'):
                m.setdefault('pbrMetallicRoughness', {})['baseColorFactor'] = [0.0, 0.0, 0.0, 1.0]
        if any('KHR_materials_unlit' in m.get('extensions', {}) for m in mats):
            used = doc.j.setdefault('extensionsUsed', [])
            if 'KHR_materials_unlit' not in used:
                used.append('KHR_materials_unlit')
        dropped_prims = 0
        for n in doc.list('nodes'):
            if 'mesh' not in n:
                continue
            prims = doc.j['meshes'][n['mesh']]['primitives']
            keep = [p for p in prims if p.get('material') not in drop]
            if len(keep) != len(prims):
                dropped_prims += len(prims) - len(keep)
                if keep:
                    doc.j['meshes'][n['mesh']]['primitives'] = keep
                else:
                    del n['mesh']
        if dropped_prims:
            log(f'left out {dropped_prims} parts with effects: {", ".join(sorted({mats[i]["name"] for i in drop}))}')

        # blended layers: Source 2 paints the second layer in per vertex (TEXCOORD4's x), sharpened by a
        # modulation texture: g is where the layers meet and r how soft the edge is (F_FANCY_BLENDING 1),
        # or g with a fixed softness (2), or alpha with a fixed softness (3)
        layered, detailed, wanted = {}, {}, set()
        for i, m in enumerate(mats):
            v = vmat(m)
            ip, tp = v.get('IntParams', {}), v.get('TextureParams', {})
            if int(ip.get('F_LAYERS', 0)) >= 1 and tp.get('g_tLayer2Color'):
                fancy = int(ip.get('F_FANCY_BLENDING', 0))
                mask = tp.get('g_tBlendModulation') if fancy in (1, 2, 3) else None
                normal = tp.get('g_tLayer2NormalRoughness')
                layered[i] = (tp['g_tLayer2Color'], mask, fancy, normal)
                wanted.update(p + '_c' for p in (tp['g_tLayer2Color'], mask, normal) if p)
            detail = self.detail_params(shader[i], v)
            if detail:
                detailed[i] = detail
                wanted.update(p + '_c' for p in (detail['texture'], detail.get('mask')) if p)
            if shader[i].startswith('csgo_effects'):
                wanted.update(tp[k] + '_c' for k in ('g_tMask1', 'g_tMask2', 'g_tMask3') if tp.get(k))
        files = {}
        if wanted:
            log(f'exporting the second layers of {len(layered)} blended materials and {len(detailed)} detail textures ...')
            files = self.vrf_files(sorted(wanted), os.path.join(self.work, 'layers'), 'layers')
            missing = wanted - set(files)
            if missing:
                warn(f'{len(missing)} layer and detail textures not found, e.g. {sorted(missing)[0]}')

        def texture(path):
            f = files.get(path + '_c') if path else None
            return doc.add_texture(doc.add_image(f)) if f else None

        count = 0
        for i, (color, mask, fancy, normal) in layered.items():
            tex = texture(color)
            if tex is None:
                continue
            v = vmat(mats[i])
            fp, vp = v.get('FloatParams', {}), v.get('VectorParams', {})
            ext = {'texture': {'index': tex}}
            tint = vp.get('g_vLayer2Tint')
            if isinstance(tint, list) and len(tint) >= 3 and tint[:3] != [1, 1, 1]:
                ext['factor'] = [float(x) for x in tint[:3]] + [1.0]
            scale = vp.get('g_vTexCoordScale2')
            if isinstance(scale, list) and len(scale) >= 2 and scale[:2] != [1, 1]:
                ext['uvScale'] = [float(x) for x in scale[:2]]
            mtex = texture(mask)
            if mtex is not None:
                ext['maskTexture'] = {'index': mtex}
                if fancy in (2, 3):
                    ext['softness'] = float(fp.get('g_flBlendSoftness', 0.5))
                if fancy == 3:
                    ext['maskChannel'] = 3
            # its normal map, with the roughness in alpha (Source 2 Viewer decodes them that way)
            ntex = texture(normal)
            if ntex is not None:
                ext['normalTexture'] = {'index': ntex}
            mats[i].setdefault('extensions', {})[EXT] = ext
            count += 1

        # what CS2's shaders do that glTF doesn't say
        glows = 0
        for i, m in enumerate(mats):
            v = vmat(m)
            if not v:
                continue
            s, ip, fp, vp = shader[i], v.get('IntParams', {}), v.get('FloatParams', {}), v.get('VectorParams', {})
            # Valve's normal maps point y down the bitangent; Source 2 Viewer leaves them that way
            ext = {'normalYDown': True}
            # the legacy shaders only have the specular a material asks for; the others always do
            if s.startswith(('csgo_lightmappedgeneric', 'csgo_vertexlitgeneric')):
                ext['specular'] = [bool(int(ip.get('F_SPECULAR_DIRECT', 0))), bool(int(ip.get('F_SPECULAR_INDIRECT', 0)))]
            elif s.startswith(('csgo_unlitgeneric', 'csgo_black_unlit', 'csgo_static_overlay', 'generic')):
                ext['specular'] = [False, False]
            else:
                ext['specular'] = [True, True]
            if s.startswith('csgo_static_overlay') and int(ip.get('F_BLEND_MODE', 0)) == 3:
                ext['blendMode'] = 'mod2x'  # multiplies what's under it by twice its colour
            # what the vertex colors are to the shader (Source 2 Viewer passes them on as they are)
            if s.startswith(('csgo_lightmappedgeneric', 'csgo_vertexlitgeneric')):
                ext['vertexColor'] = 'srgb' if int(ip.get('F_VERTEX_COLOR', 0)) or int(ip.get('F_PAINT_VERTEX_COLORS', 0)) else 'none'
            elif s.startswith('csgo_environment'):
                ext['vertexColor'] = 'paint'  # tints by rgb as much as alpha says
            if i in detailed:
                d = detailed[i]
                tex = texture(d['texture'])
                if tex is not None:
                    dx = {k: d[k] for k in ('mode', 'blend', 'blendToFull', 'transform', 'tint', 'maskUV') if k in d}
                    dx['texture'] = {'index': tex}
                    mtex = texture(d.get('mask'))
                    if mtex is not None:
                        dx['maskTexture'] = {'index': mtex}
                    ext['detail'] = dx
            if s.startswith('csgo_effects'):
                ext['effect'] = self.effect_params(v, texture)
            if s.startswith('csgo_glass'):
                # Source 2 Viewer leaves it white and opaque: see-through and tinted, with reflections that
                # don't fade with its opacity
                tint = (vp.get('GlassTintColor') or [1, 1, 1])[:3]
                m.setdefault('pbrMetallicRoughness', {})['baseColorFactor'] = [srgb_to_linear(float(c)) for c in tint] + [0.2]
                m['alphaMode'] = 'BLEND'
                ext['glass'] = True
            # self-illumination: Source 2 Viewer exports the mask as the emissive texture
            if int(ip.get('F_SELF_ILLUM', 0)) and s.startswith(('csgo_vertexlitgeneric', 'csgo_complex', 'generic')):
                tint = (vp.get('g_vSelfIllumTint') or [1, 1, 1])[:3]
                m['emissiveFactor'] = [min(max(float(c), 0.0), 1.0) for c in tint]
                strength = 2.0 ** float(fp.get('g_flSelfIllumBrightness', 0)) * float(fp.get('g_flSelfIllumScale', 1))
                m.setdefault('extensions', {})['KHR_materials_emissive_strength'] = {'emissiveStrength': strength}
                ext['selfIllumAlbedo'] = float(fp.get('g_flSelfIllumAlbedoFactor', 0))
                glows += 1
            else:
                m.pop('emissiveTexture', None)
                m.pop('emissiveFactor', None)
            m.setdefault('extensions', {})[EXT_S2] = ext
        if glows:
            log(f'{glows} self-illuminated materials')
            used = doc.j.setdefault('extensionsUsed', [])
            if 'KHR_materials_emissive_strength' not in used:
                used.append('KHR_materials_emissive_strength')

        # the vertex attributes that go with them
        blends = dropped = 0
        for me in doc.list('meshes'):
            for p in me['primitives']:
                mat = p.get('material')
                if mat in foliage and 'COLOR_0' in p['attributes']:
                    del p['attributes']['COLOR_0']  # wind: how far each part sways
                    dropped += 1
                if mat in layered and EXT in mats[mat].get('extensions', {}) and '_TEXCOORD_4' in p['attributes']:
                    p['attributes']['_BLEND'] = p['attributes'].pop('_TEXCOORD_4')
                    blends += 1
        log(f'{count} blended materials on {blends} meshes, foliage colours fixed on {dropped} meshes')

    # ------------------------------------------------ CS2's lighting

    PROBE_VOLUMES = ('env_light_probe_volume', 'env_combined_light_probe_volume')

    def lighting_set(self, vpk, path, ents, key):
        """decompiles the lightmaps and probe atlas of a map (or of its 3D skybox); returns what they came
        out as, and the probe volumes, or None when it has none hypr3d can read"""
        listing = self.vrf.run(['-i', vpk, '--vpk_list', '-f', path + '/lightmaps/'], os.path.join(self.work, key + '_list.log'))
        have = set(re.findall(r'^(\S+\.vtex_c)\s', listing, re.M))
        lightmaps = {n: f'{path}/lightmaps/{n}.vtex_c' for n in LIGHTMAPS}
        if not all(p in have for p in lightmaps.values()):
            other = sorted(os.path.basename(p) for p in have)
            if other:
                warn(f'{path} has lightmaps hypr3d can\'t read yet ({", ".join(other[:4])}), so hypr3d lights it itself')
            return None
        volumes = [e for e in ents if e.get('classname') in self.PROBE_VOLUMES and 'light_probe_atlas_x' in e
                   and not truthy(e.get('startdisabled', 'false'))]
        atlas = {k: resource(volumes[0].get(k)) for k in ('lightprobetexture', 'lightprobetexture_dlshd')} if volumes else {}
        wanted = list(lightmaps.values()) + [p + '_c' for p in atlas.values() if p]
        out = os.path.join(self.work, key + '_lighting')
        self.vrf.run(['-i', vpk, '-f', ','.join(wanted), '-d', '-o', out], os.path.join(self.work, key + '_lighting.log'))

        def image(p):
            base = os.path.join(out, os.path.splitext(p)[0])
            hits = [base + e for e in ('.exr', '.png') if os.path.isfile(base + e)]
            return hits[0] if hits else None

        def slices(p):
            return sorted(glob.glob(glob.escape(os.path.join(out, os.path.splitext(p)[0])) + '_z[0-9][0-9][0-9].*'))

        files = {n: image(p) for n, p in lightmaps.items()}
        if not all(files.values()):
            warn(f'the lightmaps of {path} did not decompile, so hypr3d lights it itself')
            return None
        probes = None
        if volumes and all(atlas.values()):
            irr, shd = slices(atlas['lightprobetexture']), slices(atlas['lightprobetexture_dlshd'])
            if irr and shd and len(irr) == 6 * len(shd):
                probes = {'irradiance': irr, 'shadows': shd, 'volumes': volumes}
            else:
                warn(f'the light probes of {path} came out as {len(irr)} and {len(shd)} slices, leaving them out')
        return {'files': files, 'probes': probes}

    @staticmethod
    def probe_volume(e, to_map):
        """a light probe volume as HYPR3D_lighting has it: from world space (glTF) into the volume's box,
        normalized to 0..1, and where its probes are in the atlas (in texels)"""
        rot = source_rotation(vec(e, 'angles'))
        origin = vec(e, 'origin')
        lo, hi = vec(e, 'box_mins'), vec(e, 'box_maxs')
        size = [max(hi[k] - lo[k], 1e-3) for k in range(3)]
        # world (glTF) -> the map's glTF (the 3D skybox's is scaled) -> Source -> the volume's own axes -> 0..1
        to_local = affine([[rot[c][r] for c in range(3)] for r in range(3)], apply(affine([[rot[c][r] for c in range(3)] for r in range(3)]), [-o for o in origin]))
        to_box = affine([[1 / size[0], 0, 0], [0, 1 / size[1], 0], [0, 0, 1 / size[2]]], [-lo[k] / size[k] for k in range(3)])
        m = mat_mul(to_box, mat_mul(to_local, mat_mul(GLTF_TO_SOURCE, to_map)))
        # its corners in world space, for picking a volume by position
        inv_map = [[to_map[r][c] for c in range(4)] for r in range(4)]
        corners = []
        for i in range(8):
            q = [lo[k] if not (i >> k) & 1 else hi[k] for k in range(3)]
            s = [sum(rot[r][c] * q[c] for c in range(3)) + origin[r] for r in range(3)]
            g = to_gltf(s)
            # undo to_map (a scale and an offset) to get from the map's glTF to world space
            corners.append([(g[k] - inv_map[k][3]) / inv_map[k][k] for k in range(3)])
        return {'matrix': column_major(m),
                'min': [min(c[k] for c in corners) for k in range(3)], 'max': [max(c[k] for c in corners) for k in range(3)],
                'atlasOffset': [int(float(e.get(f'light_probe_atlas_{a}', 0))) for a in 'xyz'],
                'atlasSize': [int(float(e.get(f'light_probe_size_{a}', 1))) for a in 'xyz'],
                'priority': int(float(e.get('indoor_outdoor_level', 0) or 0))}

    def add_lighting(self, doc, ents, sky):
        blender = shutil.which('blender')
        if not blender:
            warn("no Blender to read the lightmaps with, so hypr3d lights the map itself")
            return
        log('exporting the lightmaps and light probes ...')
        sets = [('map', self.vpk, f'maps/{self.name}', ents, affine())]
        if self.skybox:
            # the 3D skybox is s times bigger in the world than in its own glTF
            s, t = self.skybox['scale'], self.skybox['offset']
            sets.append(('skybox', self.skybox['vpk'], self.skybox['path'], self.skybox['ents'],
                         affine([[1 / s, 0, 0], [0, 1 / s, 0], [0, 0, 1 / s]], [-t[k] / s for k in range(3)])))
        found = []
        for key, vpk, path, sents, to_map in sets:
            got = self.lighting_set(vpk, path, sents, key)
            if not got and key == 'map':
                return  # the skybox alone isn't worth it
            found.append((key, got, sents, to_map))

        # the conversions, all in one Blender
        work = os.path.join(self.work, 'lighting')
        os.makedirs(work, exist_ok=True)
        jobs = []
        for key, got, sents, to_map in found:
            if not got:
                continue
            sun = next((e for e in sents if e.get('classname') == 'light_environment'), None)
            channel = sun_channel(sun)
            got['channel'] = channel
            f = got['files']
            jobs.append({'op': 'rgbe', 'src': f['irradiance'], 'dst': os.path.join(work, f'{key}_irradiance.png'), 'maxsize': 4096})
            jobs.append({'op': 'rgba8', 'src': f['directional_irradiance'], 'dst': os.path.join(work, f'{key}_directional.png'), 'maxsize': 4096})
            if 0 <= channel < 4:
                jobs.append({'op': 'channel', 'src': f['direct_light_shadows'], 'dst': os.path.join(work, f'{key}_shadows.png'), 'channel': channel})
            if got['probes']:
                p = got['probes']
                jobs.append({'op': 'atlas_rgbe', 'src': p['irradiance'], 'dst': os.path.join(work, f'{key}_probes.png'), 'cols': 16})
                # (no channel: no probe shadows either, rather than another light's)
                if 0 <= channel < 4:
                    jobs.append({'op': 'atlas_channel', 'src': p['shadows'], 'dst': os.path.join(work, f'{key}_probe_shadows.png'), 'cols': 16,
                                 'channel': channel})
        script = os.path.join(work, 'lighttool.py')
        open(script, 'w').write(LIGHTING_TOOL)
        jobfile = os.path.join(work, 'job.json')
        json.dump({'jobs': jobs, 'stats': os.path.join(work, 'stats.json')}, open(jobfile, 'w'))
        r = subprocess.run([blender, '-b', '--factory-startup', '--python-exit-code', '1', '-P', script, '--', jobfile], capture_output=True, text=True)
        if r.returncode != 0:
            open(os.path.join(work, 'blender.log'), 'w').write(r.stdout + r.stderr)
            warn(f"Blender couldn't convert the lightmaps, so hypr3d lights the map itself (see {os.path.join(work, 'blender.log')})")
            return
        stats = json.load(open(os.path.join(work, 'stats.json')))

        def img(name):
            p = os.path.join(work, name)
            return {'image': doc.add_image(p, os.path.splitext(name)[0])} if os.path.isfile(p) else None

        light = {'sets': []}
        for key, got, sents, to_map in found:
            entry = {'name': key}
            if got:
                entry['lightmaps'] = {k: img(f'{key}_{k}.png') for k in ('irradiance', 'directional', 'shadows')}
                entry['lightmaps'] = {k: v for k, v in entry['lightmaps'].items() if v}
                if got['probes']:
                    st = stats[os.path.join(work, f'{key}_probes.png')]
                    w, h, n = st['slice']
                    dims = [w, h, n // 6]
                    entry['probes'] = {'irradiance': img(f'{key}_probes.png'), 'shadows': img(f'{key}_probe_shadows.png'),
                                       'size': dims, 'columns': 16,
                                       'volumes': [self.probe_volume(e, to_map) for e in got['probes']['volumes']]}
                    if not entry['probes']['shadows']:
                        del entry['probes']['shadows']
            light['sets'].append(entry)

        # the sun: its colour times its brightness, linear, the way CS2's shaders have it; black when CS2 doesn't light
        # with it at run time (hypr3d would light a map without one with its own)
        sun = next((e for e in ents if e.get('classname') == 'light_environment' and truthy(e.get('enabled', 'true'))), None)
        sun = sun or next((e for e in ents if e.get('classname') == 'light_environment'), None)
        if sun:
            k = num(sun, 'brightness', 1) * num(sun, 'brightnessscale', 1) if sun_runtime(sun) else 0.0
            p, y = (math.radians(a) for a in vec(sun, 'angles')[:2])
            towards = [-math.cos(p) * math.cos(y), -math.cos(p) * math.sin(y), math.sin(p)]
            light['sun'] = {'color': [srgb_to_linear(c / 255.0) * k for c in vec(sun, 'color')], 'direction': to_gltf([c / INCH for c in towards])}
            if not k:
                log(f"its sun lights nothing at run time (directlight {sun.get('directlight', 1)}, brightness "
                    f"{num(sun, 'brightness', 1) * num(sun, 'brightnessscale', 1):g}): the lightmaps and probes have all its light")
        else:
            light['sun'] = {'color': [0.0, 0.0, 0.0], 'direction': [0.0, 1.0, 0.0]}
        fog = next((e for e in ents if e.get('classname') == 'env_cubemap_fog' and not truthy(e.get('startdisabled', 'false'))), None)
        if fog:
            g = lambda key, d: float(fog.get(key, d) or d)
            fj = {'start': g('cubemapfogstartdistance', 0) * INCH, 'end': g('cubemapfogenddistance', 4000) * INCH,
                  'exponent': g('cubemapfogfalloffexponent', 1), 'maxOpacity': g('cubemapfogmaxopacity', 1), 'lodBias': g('cubemapfoglodbiase', 0)}
            if truthy(fog.get('cubemapheightfog', 'cubemapfogheightexponent' in fog)):
                start = g('cubemapfogheightstart', 0)
                end = g('cubemapfogheightend', 0) if 'cubemapfogheightend' in fog else start + g('cubemapfogheightwidth', 0)
                if end > start:
                    fj.update(heightStart=start * INCH, heightEnd=end * INCH, heightExponent=g('cubemapfogheightexponent', 1))
            light['fog'] = fj
        if sky:
            light['sky'] = sky
        self.post_processing(ents, light)

        doc.j.setdefault('extensions', {})[EXT_LIGHT] = light
        used = doc.j.setdefault('extensionsUsed', [])
        if EXT_LIGHT not in used:
            used.append(EXT_LIGHT)
        lit = self.lightmap_uvs(doc)
        vols = sum(len(s.get('probes', {}).get('volumes', [])) for s in light['sets'])
        log(f'lighting: {", ".join(s["name"] for s in light["sets"] if "lightmaps" in s)} lightmaps on {lit} meshes, {vols} probe volumes'
            f'{", fog" if fog else ""}, exposure {light.get("exposure", {}).get("min", 1):g}-{light.get("exposure", {}).get("max", 1):g}')

    def post_processing(self, ents, light):
        """the map's exposure range and tone curve (its post_processing_volume and .vpost)"""
        vols = [e for e in ents if e.get('classname') == 'post_processing_volume' and not truthy(e.get('startdisabled', 'false'))]
        vol = next((e for e in vols if truthy(e.get('master', 'false'))), vols[0] if vols else None)
        if not vol:
            return
        if truthy(vol.get('enableexposure', 'true')):
            g = lambda key, d: float(vol.get(key, d) or d)
            light['exposure'] = {'min': g('minexposure', 1), 'max': g('maxexposure', 1), 'speedUp': g('exposurespeedup', 1), 'speedDown': g('exposurespeeddown', 1)}
        vpost = resource(vol.get('postprocessing', ''))
        if not vpost:
            return
        got = self.vrf_files([vpost + '_c'], os.path.join(self.work, 'postprocess'), 'postprocess')
        path = got.get(vpost + '_c')
        if not path:
            warn(f"the map's post processing {vpost} did not export")
            return
        text = open(path, encoding='utf-8', errors='replace').read()
        m = re.search(r'_class\s*=\s*"CToneMappingLayer".*?m_params\s*=\s*\{(.*?)\}', text, re.S)
        if m:
            params = dict(re.findall(r'(m_fl\w+)\s*=\s*([-\d.eE+]+)', m.group(1)))
            names = {'m_flExposureBias': 'exposureBias', 'm_flShoulderStrength': 'shoulderStrength', 'm_flLinearStrength': 'linearStrength',
                     'm_flLinearAngle': 'linearAngle', 'm_flToeStrength': 'toeStrength', 'm_flToeNum': 'toeNum', 'm_flToeDenom': 'toeDenom',
                     'm_flWhitePoint': 'whitePoint'}
            light['tonemap'] = {v: float(params[k]) for k, v in names.items() if k in params}
        # colour correction: a 32x32x32 table; hypr3d leaves it out, which only matters when it isn't neutral
        raw = re.search(r'm_fileName\s*=\s*"([^"]+\.raw)"', text)
        lut = os.path.join(os.path.dirname(path), os.path.basename(raw.group(1))) if raw else None
        if lut and os.path.isfile(lut):
            data = open(lut, 'rb').read()
            if len(data) == 32 ** 3 * 3:
                off = max(abs(data[((b * 32 + g) * 32 + r) * 3 + c] - round((r, g, b)[c] * 255 / 31))
                          for b in range(32) for g in range(32) for r in range(32) for c in range(3))
                if off > 12:
                    warn(f"the map's colour grading ({os.path.basename(lut)}) isn't done by hypr3d (it shifts colours by up to {off}/255)")

    WORLD_NODE = re.compile(r'^(n|node)\d+_', re.I)

    def lightmap_uvs(self, doc):
        """names the lightmap uvs of the world's meshes _LIGHTMAP_UV: of their second and further uv sets,
        the last one inside 0..1 (Source 2 Viewer has fitted them to the lightmap). Entities and world
        meshes without one are lit by the light probes."""
        j = doc.j
        accs = j['accessors']
        count = 0
        stack = [(r, False) for r in doc.roots]
        while stack:
            n, world = stack.pop()
            nd = j['nodes'][n]
            world = world or bool(self.WORLD_NODE.match(nd.get('name', '')))
            stack.extend((c, world) for c in nd.get('children', []))
            if not world or 'mesh' not in nd:
                continue
            for p in j['meshes'][nd['mesh']]['primitives']:
                at = p['attributes']
                if '_LIGHTMAP_UV' in at:
                    count += 1
                    continue
                cands = []
                for k in at:
                    if not k.startswith('TEXCOORD_') or k == 'TEXCOORD_0':
                        continue
                    a = accs[at[k]]
                    lo, hi = a.get('min'), a.get('max')
                    if lo and hi and min(lo) >= -0.001 and max(hi) <= 1.001 and hi[0] - lo[0] > 1e-6 and hi[1] - lo[1] > 1e-6:
                        cands.append(k)
                if not cands:
                    continue
                at['_LIGHTMAP_UV'] = at.pop(max(cands, key=lambda s: int(s.split('_')[1])))
                # the others stay numbered from 0 up
                rest = sorted((int(s.split('_')[1]), s) for s in at if s.startswith('TEXCOORD_'))
                for i, (old, s) in enumerate(rest):
                    if i != old:
                        at[f'TEXCOORD_{i}'] = at.pop(s)
                count += 1
        return count

    # ------------------------------------------------ the sky

    def add_sky(self, doc, ents):
        """the sky dome; returns what HYPR3D_lighting's "sky" says about it (its image, how bright it is), or None"""
        sky = next((e for e in ents if e.get('classname') == 'env_sky' and not truthy(e.get('startdisabled', 'false'))), None)
        vm = resource(sky.get('skyname', '')) if sky else ''
        if not vm:
            return None
        blender = shutil.which('blender')
        if not blender:
            warn("no Blender to read the sky's .exr with, hypr3d's own sky will show")
            return None
        got = self.vrf_files([vm + '_c'], os.path.join(self.work, 'sky'), 'sky')
        text = open(got[vm + '_c'], encoding='utf-8', errors='replace').read() if got else ''
        m = re.search(r'"g_tSkyTexture"\s+"([^"]+)"', text)
        if not m:
            warn(f'no sky texture in {vm}, leaving the sky out')
            return None
        tex = self.vrf_files([m.group(1) + '_c'], os.path.join(self.work, 'sky'), 'skytex')
        src = tex.get(m.group(1) + '_c')
        png = os.path.join(self.work, 'sky', self.name + '_sky.png')
        if not src:
            warn(f'the sky texture {m.group(1)} did not export, leaving the sky out')
            return None
        script = os.path.join(self.work, 'sky', 'topng.py')
        open(script, 'w').write(SKY_TO_PNG)
        r = subprocess.run([blender, '-b', '--factory-startup', '--python-exit-code', '1', '-P', script, '--', src, png], capture_output=True, text=True)
        if r.returncode != 0 or not os.path.isfile(png):
            warn(f"Blender couldn't turn the sky into a PNG, leaving it out ({(r.stdout + r.stderr).strip().splitlines()[-1:]})")
            return None
        w, h = struct.unpack('>II', open(png, 'rb').read()[16:24])
        if w != 2 * h:
            warn(f'the sky texture is {w}x{h}, not a panorama, leaving it out')
            return None
        image = self.sky_dome(doc, png)
        log(f'sky dome with {os.path.basename(m.group(1))}')
        # the sky shader's exposure bias and the env_sky's brightness and tint
        bias = re.search(r'"g_flBrightnessExposureBias"\s+"([-\d.e]+)"', text)
        k = 2.0 ** float(bias.group(1) if bias else 0) * float(sky.get('brightnessscale', 1) or 1)
        tint = [c / 255.0 for c in vec(sky, 'tint_color')] if 'tint_color' in sky else [1.0, 1.0, 1.0]
        return {'image': image, 'color': [srgb_to_linear(c) * k for c in tint]}

    def sky_dome(self, doc, png):
        # far enough out that walking around doesn't move it; hypr3d draws it behind everything
        R, cols, rows = 2000.0, 48, 24
        pos, uv, idx = [], [], []
        for r in range(rows + 1):
            v = r / rows
            for c in range(cols + 1):
                u = c / cols
                a, b = u * 2 * math.pi, v * math.pi
                # Source 2 Viewer's lat-long: u runs round from +x towards +z, v down from straight up
                pos.append((R * math.sin(b) * math.cos(a), R * math.cos(b), R * math.sin(b) * math.sin(a)))
                uv.append((u, v))
        for r in range(rows):
            for c in range(cols):
                i = r * (cols + 1) + c
                idx += [(i, i + cols + 1, i + 1), (i + 1, i + cols + 1, i + cols + 2)]
        p = doc.add_accessor('3f', pos, 'VEC3', 5126, minmax=True)
        t = doc.add_accessor('2f', uv, 'VEC2', 5126)
        ix = doc.add_accessor('I', [(k,) for tri in idx for k in tri], 'SCALAR', 5125)
        image = doc.add_image(png, 'sky')
        tex = doc.add_texture(image)
        mat = doc.add('materials', {'name': 'skydome', 'pbrMetallicRoughness': {'baseColorTexture': {'index': tex}, 'metallicFactor': 0},
                                    'extensions': {'KHR_materials_unlit': {}}})
        used = doc.j.setdefault('extensionsUsed', [])
        if 'KHR_materials_unlit' not in used:
            used.append('KHR_materials_unlit')
        mesh = doc.add('meshes', {'name': 'skydome', 'primitives': [{'attributes': {'POSITION': p, 'TEXCOORD_0': t}, 'indices': ix, 'material': mat}]})
        doc.roots.append(doc.add('nodes', {'name': 'skydome', 'mesh': mesh}))
        return image

    # ------------------------------------------------ where to start, where the desktop hangs

    def add_anchors(self, doc, ents):
        preset = PRESETS.get(self.name, {})
        spawn = self.args.spawn or preset.get('spawn') or 't'
        if spawn in ('t', 'ct'):
            cls = 'info_player_terrorist' if spawn == 't' else 'info_player_counterterrorist'
            cands = [e for e in ents if e.get('classname') == cls and truthy(e.get('enabled', 'true'))]
            if not cands:
                warn(f'no {cls} in the map, hypr3d will pick a start')
                at = None
            else:
                # the one nearest the middle of the team's spawn, facing the way it does
                pts = [vec(e, 'origin') for e in cands]
                mid = [sum(p[k] for p in pts) / len(pts) for k in range(3)]
                best = min(cands, key=lambda e: (-int(e.get('priority', 0) or 0), math.dist(vec(e, 'origin'), mid)))
                at = vec(best, 'origin') + [vec(best, 'angles')[1]]
        else:
            at = floats(spawn, 4, '--spawn')
        if at:
            # hypr3d looks down the node's -z: a turn of (yaw - 180 degrees) about +y
            doc.roots.append(doc.add('nodes', {'name': 'hypr3d_spawn', 'translation': to_gltf(at[:3]), 'rotation': yaw_quat(math.radians(at[3] - 180))}))
            log(f'start at {at[0]:.0f} {at[1]:.0f} {at[2]:.0f}, facing {at[3]:.0f} degrees')
        desk = self.args.desktop or preset.get('desktop')
        if desk:
            d = floats(desk, 4, '--desktop')
            # the desktop faces the node's +z
            doc.roots.append(doc.add('nodes', {'name': 'hypr3d_desktop', 'translation': to_gltf(d[:3]), 'rotation': yaw_quat(math.radians(d[3]))}))
            log(f'desktop at {d[0]:.0f} {d[1]:.0f} {d[2]:.0f}, facing {d[3]:.0f} degrees')


# linear .exr panorama -> sRGB .png, inside Blender
SKY_TO_PNG = r'''
import bpy, sys, numpy as np
src, dst = sys.argv[sys.argv.index("--") + 1:][:2]
img = bpy.data.images.load(src)
w, h = img.size
px = np.empty(w * h * 4, dtype=np.float32)
img.pixels.foreach_get(px)
px = px.reshape(h, w, 4)
c = np.clip(px[..., :3], 0.0, 1.0)
px[..., :3] = np.where(c <= 0.0031308, c * 12.92, 1.055 * np.power(c, 1 / 2.4) - 0.055)
px[..., 3] = 1.0
out = bpy.data.images.new("sky", w, h, alpha=False)
out.pixels.foreach_set(px.ravel())
out.filepath_raw = dst
out.file_format = "PNG"
out.save()
'''


# the lightmaps and probe atlases -> PNGs, inside Blender (it reads the .exr files, and has numpy)
LIGHTING_TOOL = r'''
# Converts CS2's decompiled lightmaps and probe atlases into the PNGs hypr3d reads: HDR data as RGBE
# (8-bit mantissas, a shared exponent in alpha), single channels as greyscale, 3D atlases as their
# slices laid out in a grid. Rows are written top first, the way the textures are addressed.
import bpy, sys, json, zlib, struct, time
import numpy as np


def load(path):
    img = bpy.data.images.load(path)
    img.colorspace_settings.name = 'Non-Color'
    w, h = img.size
    px = np.empty(w * h * 4, dtype=np.float32)
    img.pixels.foreach_get(px)
    bpy.data.images.remove(img)
    return px.reshape(h, w, 4)[::-1]  # Blender's rows go bottom up


def write_png(path, arr):
    """uint8 (h, w, c): c = 1 grey, 3 rgb, 4 rgba; every row "Up" filtered"""
    h, w, c = arr.shape
    rows = arr.reshape(h, w * c)
    filt = np.empty_like(rows)
    filt[0] = rows[0]
    filt[1:] = rows[1:] - rows[:-1]  # uint8 wraps around, as PNG wants
    raw = np.concatenate([np.full((h, 1), 2, np.uint8), filt], axis=1)

    def chunk(tag, data):
        return struct.pack('>I', len(data)) + tag + data + struct.pack('>I', zlib.crc32(tag + data) & 0xffffffff)

    ihdr = struct.pack('>IIBBBBB', w, h, 8, {1: 0, 3: 2, 4: 6}[c], 0, 0, 0)
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', ihdr) + chunk(b'IDAT', zlib.compress(raw.tobytes(), 6)) + chunk(b'IEND', b''))


def rgbe(rgb):
    """linear float (h, w, 3) -> uint8 (h, w, 4); decodes as (m + 0.5) * 2^(e - 136)"""
    rgb = np.maximum(rgb, 0.0)
    m = rgb.max(axis=-1)
    lit = m > 1e-9
    e = np.zeros(m.shape, np.int32)
    e[lit] = np.floor(np.log2(m[lit])).astype(np.int32) + 1
    e = np.clip(e, -127, 127)
    mant = np.floor(rgb * np.ldexp(np.float32(256.0), -e)[..., None])
    out = np.empty(m.shape + (4,), np.uint8)
    out[..., :3] = np.clip(mant, 0, 255)
    out[..., 3] = (e + 128).astype(np.uint8)
    out[~lit] = 0
    return out


def shrink(px, maxsize):
    """halves until both sides fit, averaging 2x2 blocks"""
    while maxsize and max(px.shape[0], px.shape[1]) > maxsize and px.shape[0] % 2 == 0 and px.shape[1] % 2 == 0:
        h, w, c = px.shape
        px = px.reshape(h // 2, 2, w // 2, 2, c).mean(axis=(1, 3))
    return px


def grid(slices, cols):
    """(n, h, w, c) -> one image, slice k at column k % cols, row k // cols"""
    n, h, w, c = slices.shape
    rows = (n + cols - 1) // cols
    out = np.zeros((rows * h, cols * w, c), slices.dtype)
    for k in range(n):
        r, q = divmod(k, cols)
        out[r * h:(r + 1) * h, q * w:(q + 1) * w] = slices[k]
    return out


def to8(px):
    return np.clip(np.round(px * 255.0), 0, 255).astype(np.uint8)


args = sys.argv[sys.argv.index('--') + 1:]
job = json.load(open(args[0]))
stats = {}
for j in job['jobs']:
    t0 = time.time()
    op, dst = j['op'], j['dst']
    if op in ('rgbe', 'channel', 'rgba8'):
        px = shrink(load(j['src']), j.get('maxsize', 0))
        if op == 'rgbe':
            out = rgbe(px[..., :3])
            stats[dst] = {'mean': px[..., :3].reshape(-1, 3).mean(0).tolist(), 'max': float(px[..., :3].max())}
        elif op == 'channel':
            out = to8(px[..., j['channel']:j['channel'] + 1])
            stats[dst] = {'mean': float(px[..., j['channel']].mean())}
        else:
            out = to8(px)
    elif op in ('atlas_rgbe', 'atlas_channel'):
        sl = np.stack([load(p) for p in j['src']])
        if op == 'atlas_rgbe':
            out = grid(rgbe(sl[..., :3]), j['cols'])
            stats[dst] = {'mean': sl[..., :3].reshape(-1, 3).mean(0).tolist(), 'max': float(sl[..., :3].max())}
        else:
            out = grid(to8(sl[..., j['channel']:j['channel'] + 1]), j['cols'])
        stats.setdefault(dst, {})['slice'] = [int(sl.shape[2]), int(sl.shape[1]), int(sl.shape[0])]
    else:
        raise SystemExit(f'unknown op {op}')
    write_png(dst, out)
    stats.setdefault(dst, {}).update(size=[int(out.shape[1]), int(out.shape[0])], seconds=round(time.time() - t0, 1))
    print('lighttool:', op, dst, stats[dst], flush=True)
json.dump(stats, open(job['stats'], 'w'))
'''


def main():
    if any(a in ('-h', '--help') for a in sys.argv[1:]):
        print(__doc__)
        return 0
    ap = argparse.ArgumentParser(prog='cs2map', usage='%(prog)s MAP [options] (see --help)', add_help=False)
    ap.add_argument('map', nargs='?')
    ap.add_argument('-o', '--output')
    ap.add_argument('--game')
    ap.add_argument('--vrf')
    ap.add_argument('--spawn')
    ap.add_argument('--desktop')
    ap.add_argument('--no-skybox', action='store_true')
    ap.add_argument('--no-sky', action='store_true')
    ap.add_argument('--no-lighting', action='store_true')
    ap.add_argument('--keep')
    ap.add_argument('--list', action='store_true')
    args = ap.parse_args()
    try:
        if args.list:
            game = find_game(args.game)
            log(f'maps in {game}:')
            for m in list_maps(game):
                print('  ' + m)
            return 0
        if not args.map:
            ap.error('which map? (see --list)')
        Export(args).run()
    except Fail as e:
        print(f'cs2map: error: {e}', file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        return 130
    if WARNINGS:
        log(f'done, with {len(WARNINGS)} warning(s)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
