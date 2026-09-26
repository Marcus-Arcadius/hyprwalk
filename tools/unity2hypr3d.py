#!/usr/bin/env python3
"""unity2hypr3d: turn a VRChat avatar into a GLB plus the settings file hypr3d reads, without Unity.

    blender -b --factory-startup -P tools/unity2hypr3d.py -- INPUT... [options]
    python3 tools/unity2hypr3d.py INPUT... [options]     (runs itself again inside Blender)

INPUT is a .unitypackage, a .zip, a Unity project (or its Assets folder), or a .prefab/.unity
file inside a project. Several inputs are merged, e.g. an avatar package and its outfit package.

options:
  -o OUT.glb         where to write (default: <avatar name>.glb here); the settings go next to
                     it as OUT.hypr3d.json
  --avatar NAME      which avatar when the input holds several (see --list)
  --list             list the avatars found and stop (does not need Blender)
  --max-texture N    shrink textures bigger than N pixels (default 2048)
  --keep DIR         unpack packages into DIR and leave them there
  --outfit NAME|PATH put an outfit on the avatar (a prefab or model by name or file, or a
                     package or folder holding one), as dragging it onto the avatar in Unity and
                     running Modular Avatar's Setup Outfit would; repeatable

What it carries over, from the avatar descriptor and the files it points to:
  the humanoid bone map (from the FBX import settings), visemes, the blink shape, the eye bones,
  face gestures from the FX controller (both hands' signs together too) and the Gesture layer's
  finger poses, Expressions Menu toggles (objects shown and hidden, shape keys set, materials
  changed, objects moved, turned and scaled) and radial puppets (as sliders; two- and four-axis
  ones as two-axis sliders), objects that start hidden, PhysBones and Dynamic Bones as springs and
  colliders (spheres, capsules, planes, and ones that keep bones inside), and the materials (colour,
  texture, cutout or transparent, emission, culling; Standard, lilToon, Poiyomi, MToon and UnlitWF).
  A material a toggle or slider puts in a slot, or whose colour, emission or tiling it changes, is
  written as a glTF material variant (KHR_materials_variants) the settings file names. Unity's
  render queue, stencil (UnlitWF's stencil masks, lilToon's and Poiyomi's), toon outlines (UnlitWF,
  lilToon, Poiyomi, MToon), UnlitWF's back faces and light clamp go in the materials' glTF extras;
  UnlitWF's alpha from a mask, or inverted, is baked into the base texture.

Emotes: humanoid clips in the Action layer (the avatar's, an MA Merge Animator's, a VRCFury Full
Controller's) are written next to the GLB as VRM animations (OUT.<name>.vrma) and listed in the
settings file's "emotes". Their muscle curves become bone turns for the avatar's T pose, the body
curves move the hips, and shape key curves (MMD faces too) set the avatar's shape keys or VRM
expressions. Weighted keys are Unity's Bezier spans, and a state with Foot IK plants the feet on
the clip's goals. A dance motion set up with MA goes on with --outfit, like an outfit.

Modular Avatar setups are built as MA builds them for VRChat: Merge Armature (an outfit's bones
join the avatar's and its meshes follow the avatar's bones), Bone Proxy, Move To, Replace Object,
PhysBone Blocker, Platform Filter, Scale Adjuster, Floor Adjuster and Global Collider; its menus
(Menu Item, radial ones too, Menu Installer, Install Target and Group, Parameters) and reactive
components (Object Toggle, Shape Changer, Mesh Cutter with its vertex filters, Material Setter,
Material Swap); Blendshape Sync; Merge Animator for FX, Gesture and the Action layer, and Merge
Blend Tree. --outfit puts on an outfit the avatar's prefab does not have yet, as MA's Setup Outfit
does, whether or not the outfit is set up for MA. Visible Head Accessory, Mesh Settings, World
Scale Object, Convert Constraints and MA's VRChat-only settings have nothing to do here.

VRCFury setups are built after MA's, as VRCFury builds them: Armature Link (an outfit's bones
linked to the avatar's, snapped on if it says so, its meshes following the avatar's bones, bones
an animation moves kept apart), Toggles (objects turned on and off, shape keys, materials,
material properties, FX floats, clips, the Scale, Smooth Loop and World Drop actions; exclusive
tags, sliders, the resting state they give the avatar), Puppet (two-axis ones too), Gesture Driver
(and Senky's, both-hand combos too), Blinking, Visemes, Full Controller (FX, Gesture and Action
controllers with their path rewrites, menus and parameters), Blend Shape Link, Apply and
Delete During Upload, Move and Reorder Menu Item, and the older Modes, Object State, Bone
Constraint, Breathing and World Constraint features. An outfit linked with VRCFury is put on as
it is. Features for VRChat's own systems (security locks, avatar scale, toes, talking, first
person fixes and the like) change nothing hypr3d shows.

What it does not: shader effects beyond the above (toon shading, matcaps, rim lights and the like),
constraints, particles, audio, contacts; VRCFury's SPS, TPS and OGB (each is named in a warning). MA's
World Fixed Object is held in the world where its rest pose was when the avatar appeared (the settings
file's "fixed"): MA fixes it to the world's origin, and hypr3d's worlds put you at their start. Full Controller layers other than FX, Gesture and Action have nothing to do
here. Blender imports only binary FBX files, so a model in any other format stops the conversion.
"""

import sys, os, re, io, json, math, struct, zlib, tarfile, zipfile, tempfile, shutil, copy
import subprocess, argparse, time

try:
    import bpy
    import mathutils
    from mathutils import Matrix, Vector, Quaternion, Euler
except ImportError:
    bpy = mathutils = Matrix = Vector = Quaternion = Euler = None

WARNINGS = []


def log(*a):
    print('unity2hypr3d:', *a, flush=True)


def warn(msg):
    if msg not in WARNINGS:
        WARNINGS.append(msg)
        print('unity2hypr3d: warning:', msg, flush=True)


class Fail(Exception):
    pass


# ---------------------------------------------------------------- small helpers

def num(x, d=0.0):
    try:
        v = float(x)
        return v if math.isfinite(v) else d
    except (TypeError, ValueError):
        return d


def inum(x, d=0):
    if isinstance(x, bool):
        return int(x)
    if isinstance(x, int):
        return x
    try:
        return int(x)
    except (TypeError, ValueError):
        try:
            return int(float(x))
        except (TypeError, ValueError, OverflowError):
            return d


def truthy(x):
    if isinstance(x, str):
        s = x.strip().lower()
        if s in ('true', 'yes'):
            return True
        return num(s) != 0
    return bool(x)


def vec3(v, d=(0.0, 0.0, 0.0)):
    if not isinstance(v, dict):
        return d
    return (num(v.get('x'), d[0]), num(v.get('y'), d[1]), num(v.get('z'), d[2]))


def quat(v):
    """(w, x, y, z) of a Unity {x, y, z, w}"""
    if not isinstance(v, dict):
        return (1.0, 0.0, 0.0, 0.0)
    return (num(v.get('w'), 1.0), num(v.get('x')), num(v.get('y')), num(v.get('z')))


def listof(x):
    return x if isinstance(x, list) else []


def dictof(x):
    return x if isinstance(x, dict) else {}


def ref(x):
    """(fileID, guid or None) of an unresolved {fileID, guid} reference"""
    if isinstance(x, dict):
        g = x.get('guid')
        return inum(x.get('fileID')), (g if isinstance(g, str) and g and g != '0' * 32 else None)
    return 0, None


def matkey(x):
    """a material reference as a value to compare and keep: (guid, fileID), ('fbx', name) for a model's own
    material as the model has it, None for none"""
    if isinstance(x, dict):
        if 'fbxmat' in x:
            return ('fbx', str(x['fbxmat']))
        f, g = ref(x)
        if f and g:
            return (g, f)
    return None


def rich_text(s):
    """a menu label without Unity rich text tags"""
    return re.sub(r'</?(?:b|i|u|s|color|size|material|quad|sup|sub|mark|alpha|font|voffset|'
                  r'line-height|cspace|mspace|nobr|noparse|lowercase|uppercase|smallcaps|space|'
                  r'sprite|link|style|align|indent|pos|margin|width|rotate)\b[^>]*>', '', s or '',
                  flags=re.I).strip()


def plain_name(s):
    return re.sub(r'[^0-9a-z]', '', (s or '').lower())


# ---------------------------------------------------------------- xxHash64 (Unity's model fileIDs)

_P1, _P2, _P3, _P4, _P5 = (11400714785074694791, 14029467366897019727, 1609587929392839161,
                           9650029242287828579, 2870177450012600261)
_M64 = (1 << 64) - 1


def _rotl(x, r):
    return ((x << r) | (x >> (64 - r))) & _M64


def _xround(acc, lane):
    acc = (acc + lane * _P2) & _M64
    return (_rotl(acc, 31) * _P1) & _M64


def _xmerge(acc, v):
    acc ^= _xround(0, v)
    return (acc * _P1 + _P4) & _M64


def xxh64(data, seed=0):
    n = len(data)
    i = 0
    if n >= 32:
        v1 = (seed + _P1 + _P2) & _M64
        v2 = (seed + _P2) & _M64
        v3 = seed & _M64
        v4 = (seed - _P1) & _M64
        while i + 32 <= n:
            a, b, c, d = struct.unpack_from('<4Q', data, i)
            v1 = _xround(v1, a)
            v2 = _xround(v2, b)
            v3 = _xround(v3, c)
            v4 = _xround(v4, d)
            i += 32
        h = (_rotl(v1, 1) + _rotl(v2, 7) + _rotl(v3, 12) + _rotl(v4, 18)) & _M64
        for v in (v1, v2, v3, v4):
            h = _xmerge(h, v)
    else:
        h = (seed + _P5) & _M64
    h = (h + n) & _M64
    while i + 8 <= n:
        k = _xround(0, struct.unpack_from('<Q', data, i)[0])
        h = (_rotl(h ^ k, 27) * _P1 + _P4) & _M64
        i += 8
    if i + 4 <= n:
        h = (_rotl(h ^ ((struct.unpack_from('<I', data, i)[0] * _P1) & _M64), 23) * _P2 + _P3) & _M64
        i += 4
    while i < n:
        h = (_rotl(h ^ ((data[i] * _P5) & _M64), 11) * _P1) & _M64
        i += 1
    h ^= h >> 33
    h = (h * _P2) & _M64
    h ^= h >> 29
    h = (h * _P3) & _M64
    h ^= h >> 32
    return h


def hash_id(s):
    """the signed 64-bit fileID Unity derives from a model sub-object's identifier"""
    h = xxh64(s.encode('utf-8'))
    return h - (1 << 64) if h >= 1 << 63 else h


# ---------------------------------------------------------------- Unity YAML
#
# Unity writes a small, regular subset of YAML: one document per object, block maps and
# sequences (a sequence sits at the same indent as its key), flow maps for references, and
# plain or quoted scalars that may wrap onto deeper lines. Scalars stay strings, since
# fileIDs are 64-bit and some values are hex blobs.

_HDR = re.compile(r'--- !u!(\d+) &(-?\d+)( stripped)?')
_KEY = re.compile(r'''^([^\s'"{\[\]},#&*!|>%@`-][^:]*?|-[^\s:][^:]*?|'(?:[^']|'')*'|"(?:[^"\\]|\\.)*")'''
                  r'''\s*:(?:[ \t]+(.*))?$''')
_ESC = {'n': '\n', 't': '\t', 'r': '\r', '0': '\0', 'a': '\a', 'b': '\b', 'e': '\x1b', 'f': '\f',
        'v': '\v', 'N': '\x85', '_': '\xa0', 'L': '\u2028', 'P': '\u2029', ' ': ' ', '/': '/',
        '"': '"', '\\': '\\', '\t': '\t'}


def _logical(lines):
    """[(indent, text)] with wrapped scalars joined back onto their line"""
    out = []  # [indent, text, column a continuation must pass, None | value is double-quoted]
    for raw in lines:
        s = raw.rstrip('\r\n')
        t = s.strip()
        if not t:
            continue
        ind = len(s) - len(s.lstrip(' '))
        if out:
            p = out[-1]
            if p[3] is not None and ind > p[2]:
                tail = len(p[1]) - len(p[1].rstrip('\\'))
                if p[3] and tail % 2 == 1:
                    p[1] = p[1][:-1] + t  # an escaped line break
                else:
                    p[1] = p[1] + ' ' + t
                continue
        val = None
        if t == '-' or t.startswith('- '):
            rest = t[1:].lstrip()
            m = _KEY.match(rest) if rest else None
            if m:
                col = ind + len(t) - len(rest)
                val = m.group(2)
            else:
                col = ind
                val = rest
        else:
            m = _KEY.match(t)
            col = ind
            val = m.group(2) if m else t
        out.append([ind, t, col, (val.startswith('"') if val else None)])
    return [(o[0], o[1]) for o in out]


def _unq(k):
    if k[:1] in ('"', "'"):
        return _quoted(k, 0)[0]
    return k.strip()


def _is_item(t):
    return t == '-' or t.startswith('- ')


def _block(ll, i, ind):
    if _is_item(ll[i][1]):
        return _seq(ll, i, ind)
    return _map(ll, i, ind)


def _map(ll, i, ind, head=None):
    d = {}
    first = True
    while i < len(ll):
        li, t = ll[i]
        if first and head is not None:
            t = head
        elif li != ind or _is_item(t):
            break
        first = False
        m = _KEY.match(t)
        if not m:
            i += 1
            continue
        k = _unq(m.group(1))
        v = m.group(2)
        i += 1
        if v is None or v == '':
            if i < len(ll) and ll[i][0] > ind:
                val, i = _block(ll, i, ll[i][0])
            elif i < len(ll) and ll[i][0] == ind and _is_item(ll[i][1]):
                val, i = _seq(ll, i, ind)
            else:
                val = ''
        else:
            val = _value(v)
        d[k] = val
    return d, i


def _seq(ll, i, ind):
    out = []
    while i < len(ll):
        li, t = ll[i]
        if li != ind or not _is_item(t):
            break
        rest = t[1:].lstrip()
        if not rest:
            i += 1
            if i < len(ll) and ll[i][0] > ind:
                val, i = _block(ll, i, ll[i][0])
            else:
                val = ''
            out.append(val)
            continue
        m = _KEY.match(rest)
        if m:
            col = ind + len(t) - len(rest)
            val, i = _map(ll, i, col, head=rest)
        else:
            val = _value(rest)
            i += 1
        out.append(val)
    return out, i


def _value(v):
    v = v.strip()
    if v[:1] in ('{', '['):
        return _flow(v, 0)[0]
    if v[:1] in ('"', "'"):
        return _quoted(v, 0)[0]
    return v


def _ws(s, i):
    while i < len(s) and s[i] in ' \t':
        i += 1
    return i


def _flow(s, i):
    close = '}' if s[i] == '{' else ']'
    out = {} if close == '}' else []
    i += 1
    while True:
        i = _ws(s, i)
        if i >= len(s):
            return out, i
        c = s[i]
        if c == close:
            return out, i + 1
        if c == ',':
            i += 1
            continue
        if close == ']':
            v, i = _flow_val(s, i)
            out.append(v)
            continue
        if c in '"\'':
            k, i = _quoted(s, i)
        else:
            j = i
            while j < len(s) and s[j] not in ',}' and not (
                    s[j] == ':' and (j + 1 >= len(s) or s[j + 1] in ' ,}]')):
                j += 1
            k = s[i:j].strip()
            i = j
        i = _ws(s, i)
        if i < len(s) and s[i] == ':':
            i = _ws(s, i + 1)
            v, i = _flow_val(s, i)
        else:
            v = ''
        out[k] = v


def _flow_val(s, i):
    if i >= len(s):
        return '', i
    c = s[i]
    if c in '{[':
        return _flow(s, i)
    if c in '"\'':
        return _quoted(s, i)
    j = i
    while j < len(s) and s[j] not in ',}]':
        j += 1
    return s[i:j].strip(), j


def _fix_surrogates(s):
    try:
        return s.encode('utf-16', 'surrogatepass').decode('utf-16')
    except UnicodeError:
        return s


def _quoted(s, i):
    q = s[i]
    i += 1
    out = []
    if q == "'":
        while i < len(s):
            c = s[i]
            if c == "'":
                if i + 1 < len(s) and s[i + 1] == "'":
                    out.append("'")
                    i += 2
                    continue
                return ''.join(out), i + 1
            out.append(c)
            i += 1
        return ''.join(out), i
    while i < len(s):
        c = s[i]
        if c == '"':
            return _fix_surrogates(''.join(out)), i + 1
        if c == '\\' and i + 1 < len(s):
            e = s[i + 1]
            i += 2
            n = {'x': 2, 'u': 4, 'U': 8}.get(e)
            if n:
                try:
                    out.append(chr(int(s[i:i + n], 16)))
                except ValueError:
                    pass
                i += n
            else:
                out.append(_ESC.get(e, e))
            continue
        out.append(c)
        i += 1
    return _fix_surrogates(''.join(out)), i


def parse_yaml_text(text):
    """a plain YAML document (a .meta file) as nested dicts, lists and strings"""
    ll = _logical([l for l in text.split('\n') if not l.startswith('%') and not l.startswith('---')])
    return _map(ll, 0, 0)[0] if ll else {}


class UFile:
    """a Unity YAML file: its documents by fileID, parsed on first use"""

    def __init__(self, path):
        self.path = path
        self.docs = {}  # fileID -> [class id, stripped, lines | (class name, body)]
        self.order = []
        self.binary = False
        with open(path, 'rb') as f:
            data = f.read()
        if not data.lstrip()[:5] == b'%YAML':
            self.binary = True
            return
        cur = None
        for line in data.decode('utf-8', 'replace').split('\n'):
            if line.startswith('--- '):
                m = _HDR.match(line)
                cur = None
                if m:
                    cur = [int(m.group(1)), bool(m.group(3)), []]
                    fid = int(m.group(2))
                    if fid not in self.docs:
                        self.order.append(fid)
                    self.docs[fid] = cur
                continue
            if cur is not None:
                cur[2].append(line)

    def cls(self, fid):
        d = self.docs.get(fid)
        return d[0] if d else None

    def stripped(self, fid):
        d = self.docs.get(fid)
        return bool(d and d[1])

    def get(self, fid):
        """(class name, body dict) of a document, or (None, {})"""
        d = self.docs.get(fid)
        if d is None:
            return None, {}
        if isinstance(d[2], list):
            ll = _logical(d[2])
            body = _map(ll, 0, 0)[0] if ll else {}
            kind, val = next(iter(body.items())) if len(body) == 1 else ('', body)
            d[2] = (kind, val if isinstance(val, dict) else {})
        return d[2]

    def main(self, cls=None):
        """the first document of a class (the asset's main object)"""
        for fid in self.order:
            if cls is None or self.docs[fid][0] == cls:
                return fid
        return None


# ---------------------------------------------------------------- property paths of prefab overrides

_SIZE = object()


def _ptoks(path):
    parts = path.split('.')
    out = []
    j = 0
    while j < len(parts):
        p = parts[j]
        if p == 'Array' and j + 1 < len(parts):
            q = parts[j + 1]
            m = re.match(r'data\[(\d+)\]$', q)
            if m:
                out.append(int(m.group(1)))
                j += 2
                continue
            if q == 'size':
                out.append(_SIZE)
                j += 2
                continue
        out.append(p)
        j += 1
    return out


def set_prop(data, path, val):
    """apply one prefab override (propertyPath = value) to an object's body"""
    m = re.match(r'managedReferences\[(-?\d+)\]\.(.+)$', path)
    if m:  # a field of a [SerializeReference] object: in the references block
        rid, refs = int(m.group(1)), dictof(data.get('references'))
        e = next((x for x in listof(refs.get('RefIds')) if isinstance(x, dict) and inum(x.get('rid'), None) == rid),
                 None)
        if e is None:
            e = next((x for k, x in refs.items() if k not in ('version', 'RefIds') and re.fullmatch(r'-?\d+', str(k))
                      and int(k) == rid and isinstance(x, dict)), None)
        if e is not None:
            if not isinstance(e.get('data'), dict):
                e['data'] = {}
            set_prop(e['data'], m.group(2), val)
        return
    toks = _ptoks(path)
    if not toks or toks[0] is _SIZE or isinstance(toks[0], int):
        return
    cur = data
    for k in range(len(toks) - 1):
        t, nxt = toks[k], toks[k + 1]
        want_list = isinstance(nxt, int) or nxt is _SIZE
        if isinstance(t, int):
            if not isinstance(cur, list) or t > 100000:
                return
            while len(cur) <= t:
                cur.append('')
            child = cur[t]
        else:
            if not isinstance(cur, dict):
                return
            child = cur.get(t)
        if isinstance(child, Obj):
            return
        if want_list and not isinstance(child, list):
            child = []
        elif not want_list and not isinstance(child, dict):
            child = {}
        if isinstance(t, int):
            cur[t] = child
        else:
            cur[t] = child
        if nxt is _SIZE:
            n = max(0, min(inum(val), 100000))
            del child[n:]
            while len(child) < n:
                child.append('')
            return
        cur = child
    t = toks[-1]
    if isinstance(t, int):
        if isinstance(cur, list) and t <= 100000:
            while len(cur) <= t:
                cur.append('')
            cur[t] = val
    elif isinstance(cur, dict):
        cur[t] = val


# ---------------------------------------------------------------- the assets: packages, zips, folders

SKIP_DIRS = {'Library', 'Temp', 'Logs', 'obj', 'UserSettings', 'Build', 'Builds', 'node_modules'}
SKIP_EXT = {'.wav', '.mp3', '.ogg', '.aif', '.aiff', '.flac', '.mp4', '.mov', '.webm', '.avi',
            '.dll', '.so', '.dylib', '.cs', '.pdb', '.mdb', '.exe'}
IMAGE_EXT = {'.png', '.jpg', '.jpeg', '.tga', '.psd', '.tif', '.tiff', '.bmp', '.exr', '.hdr',
             '.gif', '.dds', '.webp'}
MODEL_EXT = {'.fbx', '.obj', '.dae', '.3ds', '.dxf', '.blend', '.ma', '.mb', '.max', '.c4d'}


def _safe_rel(p):
    p = p.replace('\\', '/')
    if not p or p.startswith('/') or re.match(r'^[A-Za-z]:', p):
        return None
    parts = [x for x in p.split('/') if x not in ('', '.')]
    if not parts or any(x == '..' for x in parts):
        return None
    return '/'.join(parts)


class Asset:
    __slots__ = ('guid', 'path', 'file')

    def __init__(self, guid, path, file):
        self.guid, self.path, self.file = guid, path, file

    @property
    def ext(self):
        return os.path.splitext(self.file)[1].lower()

    @property
    def name(self):
        return os.path.splitext(os.path.basename(self.path))[0]


class DB:
    def __init__(self, work):
        self.work = work
        self.assets = {}
        self._yaml = {}
        self._meta = {}
        self._fbx = {}
        self._n = 0

    def add(self, guid, upath, file):
        guid = guid.lower()
        if guid not in self.assets:
            self.assets[guid] = Asset(guid, upath, file)

    def add_input(self, path):
        path = os.path.abspath(os.path.expanduser(path))
        if os.path.isdir(path):
            return self.add_folder(path)
        low = path.lower()
        if low.endswith('.unitypackage'):
            return self.add_package(path)
        if low.endswith('.zip'):
            return self.add_zip(path)
        if os.path.isfile(path):
            # a file inside a project: scan the project it belongs to
            d = os.path.dirname(path)
            root = None
            while True:
                if os.path.isdir(os.path.join(d, 'Assets')) and (
                        os.path.isdir(os.path.join(d, 'ProjectSettings')) or
                        os.path.isdir(os.path.join(d, 'Packages'))):
                    root = d
                    break
                up = os.path.dirname(d)
                if up == d:
                    break
                d = up
            if root is None:
                root = os.path.dirname(path)
                while os.path.basename(root) != 'Assets' and os.path.dirname(root) != root:
                    root = os.path.dirname(root)
                if os.path.basename(root) != 'Assets':
                    root = os.path.dirname(path)
            self.add_folder(root)
            return path
        raise Fail('no such file: %s' % path)

    def add_folder(self, root):
        n = 0
        # asset paths read like Unity's: Assets/...
        base = root if os.path.isdir(os.path.join(root, 'Assets')) else os.path.dirname(root)
        for dp, dns, fns in os.walk(root):
            dns[:] = [d for d in dns if d not in SKIP_DIRS and not d.startswith('.')]
            for fn in fns:
                if not fn.endswith('.meta'):
                    continue
                f = os.path.join(dp, fn[:-5])
                if not os.path.isfile(f) or os.path.splitext(f)[1].lower() in SKIP_EXT:
                    continue
                guid = self._guid_of(os.path.join(dp, fn))
                if guid:
                    self.add(guid, os.path.relpath(f, base).replace(os.sep, '/'), f)
                    n += 1
        log('%s: %d assets' % (root, n))
        return None

    @staticmethod
    def _guid_of(meta):
        try:
            with open(meta, 'r', encoding='utf-8', errors='replace') as f:
                for _ in range(8):
                    m = re.match(r'guid:\s*([0-9a-fA-F]{32})', f.readline())
                    if m:
                        return m.group(1).lower()
        except OSError:
            pass
        return None

    def _slot(self, what):
        self._n += 1
        d = os.path.join(self.work, '%s%d' % (what, self._n))
        os.makedirs(d, exist_ok=True)
        return d

    def add_package(self, path):
        base = self._slot('pkg')
        n = 0
        try:
            tf = tarfile.open(path, 'r:*')
        except (tarfile.TarError, OSError) as e:
            raise Fail('%s is not a readable .unitypackage (%s)' % (path, e))
        with tf:
            entries = {}
            for m in tf:
                parts = [x for x in m.name.replace('\\', '/').split('/') if x not in ('', '.')]
                if len(parts) == 2 and m.isfile() and re.fullmatch(r'[0-9a-fA-F]{32}', parts[0]):
                    entries.setdefault(parts[0].lower(), {})[parts[1]] = m
            for guid, e in entries.items():
                if 'pathname' not in e or 'asset' not in e:
                    continue  # a folder
                pn = tf.extractfile(e['pathname']).read().decode('utf-8', 'replace').split('\n')[0].strip()
                rel = _safe_rel(pn)
                if rel is None:
                    warn('skipped %r in %s: the path leaves the project' % (pn, os.path.basename(path)))
                    continue
                if os.path.splitext(rel)[1].lower() in SKIP_EXT:
                    continue
                d = os.path.join(base, guid)
                os.makedirs(d, exist_ok=True)
                out = os.path.join(d, os.path.basename(rel))
                with tf.extractfile(e['asset']) as src, open(out, 'wb') as dst:
                    shutil.copyfileobj(src, dst)
                if 'asset.meta' in e:
                    with tf.extractfile(e['asset.meta']) as src, open(out + '.meta', 'wb') as dst:
                        shutil.copyfileobj(src, dst)
                self.add(guid, rel, out)
                n += 1
        log('%s: %d assets' % (os.path.basename(path), n))
        return None

    def add_zip(self, path):
        base = self._slot('zip')
        try:
            zf = zipfile.ZipFile(path)
        except (zipfile.BadZipFile, OSError) as e:
            raise Fail('%s is not a readable zip (%s)' % (path, e))
        pkgs = []
        with zf:
            for info in zf.infolist():
                if info.is_dir():
                    continue
                name = info.filename
                if not (info.flag_bits & 0x800):
                    try:  # names without the UTF-8 flag are often Shift-JIS (Booth downloads)
                        name = name.encode('cp437').decode('cp932')
                    except (UnicodeError, LookupError):
                        pass
                rel = _safe_rel(name)
                if rel is None:
                    warn('skipped %r in %s: the path leaves the archive' % (name, os.path.basename(path)))
                    continue
                if os.path.splitext(rel)[1].lower() in SKIP_EXT:
                    continue
                out = os.path.join(base, *rel.split('/'))
                os.makedirs(os.path.dirname(out), exist_ok=True)
                with zf.open(info) as src, open(out, 'wb') as dst:
                    shutil.copyfileobj(src, dst)
                if rel.lower().endswith('.unitypackage'):
                    pkgs.append(out)
                elif rel.lower().endswith('.zip'):
                    pkgs.append(out)
        for p in pkgs:
            if p.lower().endswith('.zip'):
                self.add_zip(p)
            else:
                self.add_package(p)
        self.add_folder(base)
        return None

    # ---- access

    def get(self, guid):
        return self.assets.get(guid.lower()) if guid else None

    def yaml(self, guid):
        a = self.get(guid)
        if a is None:
            return None
        u = self._yaml.get(a.guid)
        if u is None:
            try:
                u = UFile(a.file)
            except OSError as e:
                warn('cannot read %s: %s' % (a.path, e))
                return None
            if u.binary:
                warn('%s is saved in binary form, which this tool cannot read '
                     '(set Asset Serialization to Force Text in Unity)' % a.path)
            self._yaml[a.guid] = u
        return u

    def meta(self, guid):
        a = self.get(guid)
        if a is None:
            return {}
        m = self._meta.get(a.guid)
        if m is None:
            try:
                with open(a.file + '.meta', 'r', encoding='utf-8', errors='replace') as f:
                    m = parse_yaml_text(f.read())
            except OSError:
                m = {}
            self._meta[a.guid] = m
        return m

    def importer(self, guid):
        """the importer settings block of a .meta (ModelImporter, TextureImporter, ...)"""
        for k, v in self.meta(guid).items():
            if k.endswith('Importer') and isinstance(v, dict):
                return v
        return {}

    def fbx(self, guid):
        a = self.get(guid)
        if a is None:
            return None
        f = self._fbx.get(a.guid)
        if f is None:
            if a.ext != '.fbx':
                raise Fail('%s is a %s model; only binary FBX can be converted' % (a.path, a.ext))
            f = FBXInfo(a.file, a.path)
            self._fbx[a.guid] = f
        return f

    def by_name(self, name, exts, near=None):
        """assets with this file name (case-insensitive), the ones closest to `near` first"""
        low = name.lower()
        hits = [a for a in self.assets.values() if a.ext in exts and a.name.lower() == low]
        if near:
            nd = os.path.dirname(near)

            def dist(a):
                ad = os.path.dirname(a.path)
                common = os.path.commonprefix([nd.split('/'), ad.split('/')])
                return -len(common)
            hits.sort(key=dist)
        return hits


# ---------------------------------------------------------------- binary FBX

def read_fbx(path):
    with open(path, 'rb') as f:
        data = f.read()
    if not data.startswith(b'Kaydara FBX Binary  \x00'):
        if b'FBXHeaderExtension' in data[:4096]:
            raise Fail('%s is an ASCII FBX; Blender imports only binary FBX files '
                       '(re-export it as binary)' % path)
        raise Fail('%s is not an FBX file' % path)
    version = struct.unpack_from('<I', data, 23)[0]
    wide = version >= 7500
    if version < 7100:
        raise Fail('%s is FBX %d; Blender needs FBX 7.1 or newer' % (path, version))

    def prop(pos):
        t = chr(data[pos])
        pos += 1
        if t == 'Y':
            return struct.unpack_from('<h', data, pos)[0], pos + 2
        if t == 'C':
            return bool(data[pos]), pos + 1
        if t == 'I':
            return struct.unpack_from('<i', data, pos)[0], pos + 4
        if t == 'F':
            return struct.unpack_from('<f', data, pos)[0], pos + 4
        if t == 'D':
            return struct.unpack_from('<d', data, pos)[0], pos + 8
        if t == 'L':
            return struct.unpack_from('<q', data, pos)[0], pos + 8
        if t in 'fdlib':
            n, enc, clen = struct.unpack_from('<III', data, pos)
            pos += 12
            raw = data[pos:pos + clen]
            pos += clen
            if enc == 1:
                raw = zlib.decompress(raw)
            fmt = {'f': 'f', 'd': 'd', 'l': 'q', 'i': 'i', 'b': 'B'}[t]
            return list(struct.unpack('<%d%s' % (n, fmt), raw[:n * struct.calcsize(fmt)])), pos
        if t in 'SR':
            n = struct.unpack_from('<I', data, pos)[0]
            pos += 4
            raw = data[pos:pos + n]
            pos += n
            return (raw if t == 'R' else raw.decode('utf-8', 'replace')), pos
        raise Fail('%s: bad FBX property type %r' % (path, t))

    def node(pos):
        if wide:
            end, nprops, _ = struct.unpack_from('<QQQ', data, pos)
            pos += 24
        else:
            end, nprops, _ = struct.unpack_from('<III', data, pos)
            pos += 12
        if end == 0:
            return None, pos + 1
        nlen = data[pos]
        pos += 1
        name = data[pos:pos + nlen].decode('utf-8', 'replace')
        pos += nlen
        props = []
        for _ in range(nprops):
            v, pos = prop(pos)
            props.append(v)
        kids = []
        while pos < end:
            k, pos = node(pos)
            if k is None:
                break
            kids.append(k)
        return (name, props, kids), end

    top = []
    pos = 27
    while pos < len(data) - 13:
        n, pos = node(pos)
        if n is None:
            break
        top.append(n)
    return version, top


def _fchild(n, name):
    for k in n[2]:
        if k[0] == name:
            return k
    return None


def _fname(s):
    return s.split('\x00\x01')[0] if isinstance(s, str) else ''


def _props70(n):
    out = {}
    p = _fchild(n, 'Properties70') if n else None
    for k in (p[2] if p else []):
        if k[0] == 'P' and k[1]:
            out[k[1][0]] = k[1][4:]
    return out


class FBXInfo:
    """what the converter needs from an FBX: the node tree, meshes, shape keys, materials"""

    def __init__(self, file, upath):
        self.path = upath
        version, top = read_fbx(file)
        self.version = version
        objs = next((n for n in top if n[0] == 'Objects'), ('', [], []))
        conns = next((n for n in top if n[0] == 'Connections'), ('', [], []))
        gs = _props70(next((n for n in top if n[0] == 'GlobalSettings'), None))
        self.unit = num((gs.get('UnitScaleFactor') or [1.0])[0], 1.0) or 1.0
        self.up = inum((gs.get('UpAxis') or [1])[0], 1)
        self.models = {}  # id -> [name, type, parent id]
        self.order = []
        geoms, deformers, materials, channels, shapes_of_geom = {}, {}, {}, {}, {}
        for n in objs[2]:
            if not n[1]:
                continue
            i = n[1][0]
            if n[0] == 'Model':
                self.models[i] = [_fname(n[1][1]), n[1][2] if len(n[1]) > 2 else '', 0]
                self.order.append(i)
            elif n[0] == 'Geometry' and len(n[1]) > 2 and n[1][2] == 'Mesh':
                geoms[i] = n
            elif n[0] == 'Deformer':
                deformers[i] = (n[1][2] if len(n[1]) > 2 else '', _fname(n[1][1]))
            elif n[0] == 'Material':
                materials[i] = _fname(n[1][1])
        geom_of = {}  # model -> geometry
        skins = set()  # geometries with a skin
        bs_of = {}  # geometry -> [blend shape deformers]
        ch_of = {}  # blend shape deformer -> [channels]
        mats_of = {}  # model -> [material names] in connection order
        for c in conns[2]:
            if c[0] != 'C' or len(c[1]) < 3 or c[1][0] not in ('OO', 'OP'):
                continue
            a, b = c[1][1], c[1][2]
            if c[1][0] != 'OO':
                continue
            if a in self.models and (b == 0 or b in self.models):
                self.models[a][2] = b
            elif a in geoms and b in self.models:
                geom_of[b] = a
            elif a in deformers and b in geoms:
                if deformers[a][0] == 'Skin':
                    skins.add(b)
                elif deformers[a][0] == 'BlendShape':
                    bs_of.setdefault(b, []).append(a)
            elif a in deformers and b in deformers and deformers[a][0] == 'BlendShapeChannel':
                ch_of.setdefault(b, []).append(deformers[a][1])
            elif a in materials and b in self.models:
                mats_of.setdefault(b, []).append(materials[a])
        self.mesh = {}  # model id -> {'skinned', 'shapes': [...], 'materials': [...], 'used': [...]}
        for mid, g in geom_of.items():
            shapes = []
            for bs in bs_of.get(g, []):
                shapes += ch_of.get(bs, [])
            mats = mats_of.get(mid, [])
            used = list(range(len(mats)))
            lem = _fchild(geoms[g], 'LayerElementMaterial')
            if lem is not None and mats:
                mapping = _fchild(lem, 'MappingInformationType')
                arr = _fchild(lem, 'Materials')
                if mapping and mapping[1] and mapping[1][0] == 'ByPolygon' and arr and arr[1]:
                    # Unity makes a submesh (a material slot) per material in the order the polygons first use them
                    used = list(dict.fromkeys(v for v in arr[1][0] if 0 <= v < len(mats))) or [0]
                elif mapping and mapping[1] and mapping[1][0] == 'AllSame':
                    used = [0]
            self.mesh[mid] = {'skinned': g in skins, 'shapes': shapes, 'materials': mats, 'used': used}
        self.kids = {}
        for mid in self.order:
            self.kids.setdefault(self.models[mid][2], []).append(mid)

    def name(self, mid):
        return self.models[mid][0] if mid in self.models else ''

    def kind(self, mid):
        return self.models[mid][1] if mid in self.models else ''


# ---------------------------------------------------------------- objects, prefab instances, the scene graph

MASK63 = 0x7FFFFFFFFFFFFFFF
DESC_GUID = '67cc4cb7839cd3741b63733d5adf0442'  # VRC.SDK3A.dll
DESC_FID = 542108242
TRACKING_FID = -646210727
PARAMS_FID = -1506855854
MENU_FID = -340790334
DYN_GUID = '2a2c05204084d904aa4945ccff20d8e5'  # VRC.SDK3.Dynamics.PhysBone.dll
PHYSBONE_FID = 1661641543
COLLIDER_FID = -1631200402
TRANSFORMS = (4, 224)
# Modular Avatar's components: their scripts' guids (MA is installed as source, fileID 11500000)
MA_SCRIPTS = {
    '2df373bf91cf30b4bbd495e11cb1a2ec': 'MergeArmature', '42581d8044b64899834d3d515ab3a144': 'BoneProxy',
    '4e6bb6a99e499d2489ccf296662fa3cd': 'MoveTo', 'a5bf908a199a4648845ebe2fd3b5a4bd': 'PBBlocker',
    '3b29d45007c5493d926d2cd45a489529': 'MenuItem', '97e46a47dd8a425eb4ce9411defe313d': 'MenuGroup',
    '7ef83cb0c23d4d7c9d41021e544a1978': 'MenuInstaller', '71a96d4ea0c344f39e277d82035bf9bd': 'Parameters',
    '1bb122659f724ebf85fe095ac02dc339': 'MergeAnimator', 'a162bb8ec7e24a5abcf457887f1df3fa': 'ObjectToggle',
    '2db441f589c3407bb6fb5f02ff8ab541': 'ShapeChanger', '0adf335711644e34b6c635e94ae61fa7': 'MaterialSetter',
    'b259b73280ead4e4fbbdafc5e29175d1': 'MaterialSwap', '6fd7cab7d93b403280f2f9da978d8a4f': 'BlendshapeSync',
    '33dac8cfeaeb4c399ddd90597f849f70': 'VisibleHeadAccessory', '560fdafd46c74b2db6422fdf0e7f2363': 'MeshSettings',
    '1895bf16884f4064f8e9550e7493c205': 'OutfitRoot', '7e949680c0864ee7b441d9b2c93b890b': 'ReplaceObject',
    '09a660aa9d4e47d992adcac5a05dd808': 'ScaleAdjuster', '762726b8618cac7419e39bdc2b572b3d': 'MeshCutter',
    '660848d04d7443b5b6fcfb627e6be5ea': 'VertexFilterByAxis', 'f8e2c9a1b3d44c6d9a7e5f2c1b8d3e4f': 'VertexFilterByBone',
    '96a7b00b1dae4a02b61b29bf02241063': 'VertexFilterByMask', 'da7788c69fae9ff4abae088a0dc92c5b': 'VertexFilterByShape',
    '8c38d6a064dbe9b91f24ee30e85c3c4f': 'VertexFilterByUVTile', '229dd561ca024a6588e388160921a70f': 'MergeBlendTree',
    '1fad1419b52a42ae89b0df52eb861e47': 'MenuInstallTarget', 'ba18e6eae93342fd8774b3f3f132928a': 'FloorAdjuster',
    '49bb23f95a7baca4186efa68bc5891b6': 'GlobalCollider'}
# Modular Avatar's other components (1.18.7): Platform Filter is converted (in Avatar); the rest are said to be left
# out (MA_WHY) or not (MA_QUIET), and they count as any other component would
MA_OTHER = {
    '8c8a67d5c01849629fa90c3b2eded93f': 'PlatformFilter', '0e2d9f1d69e34b92a96e6cc162770fad': 'WorldFixedObject',
    'e113c01563a14226b5e863befe6fe769': 'WorldScaleObject', 'e362b3df8a3d478c82bf5ffe18f622e6': 'ConvertConstraints',
    'dc5f8bfae24244aeaedcd6c2bb7264f9': 'RemoveVertexColor', '934543afe4744213b5621aa13a67e3b4': 'SyncParameterSequence',
    '89c938d7d8a741df99f2eda501b3a6fe': 'VRChatSettings', '04802bf95b218724a9f4b97003067857': 'RenameVRChatCollisionTags',
    'd1d979d3cedd4ddd969f414e2ea04fb8': 'MMDLayerControl', 'a8d5b07828ba4eefb9acc305478369d0': 'MoveIndependently'}
NDMF_VRCHAT = 'nadena.dev.ndmf.vrchat.avatar3'  # NDMF's name for the platform a VRChat avatar is built for
# and the fields that give them away when the script is not the one above (a copy, another version)
MA_FIELDS = (('MergeArmature', ('mergeTarget', 'prefix', 'suffix')),
             ('BoneProxy', ('boneReference', 'subPath', 'attachmentMode')),
             ('MoveTo', ('target', 'matchPosition', 'matchRotation')),
             ('MenuItem', ('Control', 'MenuSource')),
             ('ObjectToggle', ('m_objects', 'm_inverted')),
             ('ShapeChanger', ('m_shapes',)),
             ('MenuInstaller', ('menuToAppend', 'installTargetMenu')),
             ('MergeAnimator', ('animator', 'layerType', 'pathMode')))
# Unity's HumanBodyBones, in order (55 is LastBone); the fingers are named as the import settings name them
HUMAN_BONES = ('Hips LeftUpperLeg RightUpperLeg LeftLowerLeg RightLowerLeg LeftFoot RightFoot Spine Chest Neck Head '
               'LeftShoulder RightShoulder LeftUpperArm RightUpperArm LeftLowerArm RightLowerArm LeftHand RightHand '
               'LeftToes RightToes LeftEye RightEye Jaw').split() + [
    '%s %s %s' % (s, f, p) for s in ('Left', 'Right') for f in ('Thumb', 'Index', 'Middle', 'Ring', 'Little')
    for p in ('Proximal', 'Intermediate', 'Distal')] + ['UpperChest']


class Obj:
    """one Unity object (GameObject or component) of an instantiated prefab or scene"""
    __slots__ = ('cls', 'kind', 'data', 'sid', 'src', 'removed', 'fbx', 'node', 'comps',
                 'children', 'parent', 'tf', 'name')

    def __init__(self, cls, kind, data, sid=0, src=''):
        self.cls, self.kind, self.data, self.sid, self.src = cls, kind, data, sid, src
        self.removed = False
        self.fbx = None  # the FBXInst an object of a model belongs to
        self.node = None  # its FBX model id (0: the model's root, which the file does not have)
        self.comps, self.children, self.parent, self.tf = [], [], None, None
        self.name = None  # the name of its node in the GLB

    def __repr__(self):
        n = self.data.get('m_Name') if self.cls == 1 else ''
        return '<%s %s %s>' % (self.kind or self.cls, self.sid, n or '')

    @property
    def go(self):
        g = self.data.get('m_GameObject')
        return g if isinstance(g, Obj) else None

    @property
    def gname(self):
        g = self if self.cls == 1 else self.go
        return str(g.data.get('m_Name', '')) if g else ''

    def script(self):
        return ref(self.data.get('m_Script')) if self.cls == 114 else (0, None)


class Inst:
    def __init__(self):
        self.objs = {}  # fileID in this file's id space -> Obj
        self.all = []
        self.root_tf = None
        self.fbx = []  # FBXInst of every model instance inside


class FBXInst:
    """one instance of a model in the scene, imported into Blender by itself"""

    def __init__(self, key, guid, info, collapsed, top):
        self.key, self.guid, self.info = key, guid, info
        self.collapsed = collapsed
        self.top = top  # the stripped single root node, when collapsed
        self.gos = {}  # model id -> GameObject Obj (0: the root when not collapsed)
        self.bl = {}  # after the import: model id -> ('obj', object) | ('bone', armature, bone name)
        self.orig = {}  # model id -> its Unity matrix as imported (world, from Blender)
        self.used = False


def fbx_ids(guid, info, imp):
    """the fileIDs Unity gives the objects of a model: [(model id or 0, class id, fileID)]"""
    out = []
    kids = info.kids
    top = kids.get(0, [])

    def walk(parent_path, nodes, counts, skip=None):
        for mid in nodes:
            if mid == skip:
                continue
            nm = info.name(mid)
            p = (parent_path + '/' + nm) if parent_path else nm
            yield mid, p
            yield from walk(p, kids.get(mid, []), counts)

    def comps(mid):
        c = [4]
        m = info.mesh.get(mid)
        if m is not None:
            c += [137] if (m['skinned'] or m['shapes']) else [33, 23]
        return c
    classes = {1: 'GameObject', 4: 'Transform', 137: 'SkinnedMeshRenderer', 33: 'MeshFilter',
               23: 'MeshRenderer', 95: 'Animator', 111: 'Animation'}

    def emit(mid, path, cls, counts):
        seg = ('/' + path) if path else ''
        key = (cls, path)
        k = counts.get(key, 0)
        counts[key] = k + 1
        if cls == 1:
            s = 'Type:GameObject->//RootNode/root%s%d' % (seg, k)
        else:
            s = 'Type:%s->//RootNode/root%s/%s%d' % (classes[cls], seg, classes[cls], k)
        out.append((mid, cls, hash_id(s)))

    atype = inum(imp.get('animationType'), 2)
    root_comp = {2: 95, 3: 95, 1: 111}.get(atype)
    # the model as Unity would build it with an explicit root
    counts = {}
    for cls in (1, 4) + ((root_comp,) if root_comp else ()):
        emit(0, '', cls, counts)
    for mid, p in walk('', top, counts):
        for cls in [1] + comps(mid):
            emit(mid, p, cls, counts)
    # and with its single empty root node stripped (the ids of the other variant stay usable)
    if len(top) == 1:
        counts = {}
        t = top[0]
        for cls in (1, 4) + ((root_comp,) if root_comp else ()):
            emit(t, '', cls, counts)
        for mid, p in walk('', kids.get(t, []), counts):
            for cls in [1] + comps(mid):
                emit(mid, p, cls, counts)
    # older imports list their ids by node name
    names = {}
    for mid in info.order:
        names.setdefault(info.name(mid), mid)
    table = imp.get('fileIDToRecycleName')
    if isinstance(table, dict):
        for fid, nm in table.items():
            f = inum(fid)
            mid = 0 if nm == '//RootNode' else names.get(nm)
            if mid is not None and f:
                out.append((mid, f // 100000, f))
    for e in listof(imp.get('internalIDToNameTable')):
        first = dictof(dictof(e).get('first'))
        nm = dictof(e).get('second')
        mid = 0 if nm == '//RootNode' else names.get(nm)
        for c, f in first.items():
            if mid is not None and inum(f):
                out.append((mid, inum(c), inum(f)))
    return out


class World:
    """builds objects from prefabs, scenes and models, applying prefab overrides"""

    def __init__(self, db):
        self.db = db
        self.nfbx = 0

    def instantiate(self, guid, stack=()):
        a = self.db.get(guid)
        if a is None:
            return None
        if a.guid in stack:
            warn('%s contains itself' % a.path)
            return None
        if len(stack) > 24:
            warn('prefabs nested too deeply at %s' % a.path)
            return None
        if a.ext in MODEL_EXT:
            return self.instantiate_model(a)
        uf = self.db.yaml(a.guid)
        if uf is None or uf.binary:
            return None
        return self.instantiate_yaml(uf, stack + (a.guid,))

    def instantiate_model(self, a):
        info = self.db.fbx(a.guid)
        imp = self.db.importer(a.guid)
        meshes = dictof(imp.get('meshes'))
        top = info.kids.get(0, [])
        collapsed = (not truthy(meshes.get('preserveHierarchy', '0')) and len(top) == 1 and
                     top[0] not in info.mesh and info.kind(top[0]) in ('Null', 'Root', ''))
        self.nfbx += 1
        fi = FBXInst(self.nfbx, a.guid, info, collapsed, top[0] if collapsed else None)
        inst = Inst()
        inst.fbx.append(fi)
        rootname = a.name
        root_mid = fi.top if collapsed else 0
        atype = inum(imp.get('animationType'), 2)
        gos, tfs = {}, {}

        def make(mid, parent_tf):
            nm = rootname if mid == root_mid else info.name(mid)
            go = Obj(1, 'GameObject', {'m_Name': nm, 'm_IsActive': '1', 'm_TagString': 'Untagged'},
                     0, a.path)
            tf = Obj(4, 'Transform', {'m_GameObject': go, 'm_Father': parent_tf, 'm_Children': [],
                                      'm_LocalPosition': None, 'm_LocalRotation': None,
                                      'm_LocalScale': None}, 0, a.path)
            if mid == 0:
                tf.data.update({'m_LocalPosition': {'x': '0', 'y': '0', 'z': '0'},
                                'm_LocalRotation': {'x': '0', 'y': '0', 'z': '0', 'w': '1'},
                                'm_LocalScale': {'x': '1', 'y': '1', 'z': '1'}})
            if parent_tf is not None:
                parent_tf.data['m_Children'].append(tf)
            objs = {1: go, 4: tf}
            m = info.mesh.get(mid)
            if m is not None:
                mats = [{'fbxmat': m['materials'][i]} for i in m['used'] if i < len(m['materials'])]
                mesh = {'fbxmesh': mid}
                if m['skinned'] or m['shapes']:
                    objs[137] = Obj(137, 'SkinnedMeshRenderer', {
                        'm_GameObject': go, 'm_Enabled': '1', 'm_Materials': mats,
                        'm_BlendShapeWeights': [], 'm_Mesh': mesh}, 0, a.path)
                else:
                    objs[33] = Obj(33, 'MeshFilter', {'m_GameObject': go, 'm_Mesh': mesh}, 0, a.path)
                    objs[23] = Obj(23, 'MeshRenderer', {'m_GameObject': go, 'm_Enabled': '1',
                                                        'm_Materials': mats}, 0, a.path)
            if mid == root_mid and atype in (1, 2, 3):
                if atype == 1:
                    objs[111] = Obj(111, 'Animation', {'m_GameObject': go}, 0, a.path)
                else:
                    objs[95] = Obj(95, 'Animator', {
                        'm_GameObject': go, 'm_Enabled': '1',
                        'm_Avatar': {'fileID': '9000000', 'guid': a.guid, 'type': '3'}}, 0, a.path)
            for o in objs.values():
                o.fbx, o.node = fi, mid
                inst.all.append(o)
            fi.gos[mid] = go
            gos[mid], tfs[mid] = objs, tf
            for k in info.kids.get(mid, []) if mid else top:
                make(k, tf)
        if collapsed:
            make(fi.top, None)
        else:
            make(0, None)
        inst.root_tf = tfs[root_mid]
        # both id variants lead to the same objects; the root of one is the stripped node of the other
        for mid, cls, fid in fbx_ids(a.guid, info, imp):
            o = gos.get(mid if not (collapsed and mid == 0) else fi.top, {}).get(cls)
            if o is not None:
                inst.objs.setdefault(fid, o)
        return inst

    def instantiate_yaml(self, uf, stack):
        inst = Inst()
        pis, own = [], []
        for fid in uf.order:
            cls = uf.cls(fid)
            if uf.stripped(fid):
                continue
            kind, body = uf.get(fid)
            if cls == 1001:
                if 'm_SourcePrefab' in body:
                    pis.append((fid, body))
                continue  # a pre-2018.3 Prefab object: its objects are all written out already
            o = Obj(cls, kind, copy.deepcopy(body), fid, uf.path)
            inst.objs[fid] = o
            inst.all.append(o)
            own.append(o)
        subs = {}
        for fid, body in pis:
            _, src = ref(body.get('m_SourcePrefab'))
            child = self.instantiate(src, stack) if src else None
            if child is None:
                a = self.db.get(src)
                warn('%s uses %s, which is missing' % (uf.path, a.path if a else 'a prefab (%s)' % src))
                continue
            subs[fid] = child
            for sid, o in child.objs.items():
                inst.objs.setdefault((fid ^ sid) & MASK63, o)
            inst.all += child.all
            inst.fbx += child.fbx
        for fid in uf.order:
            if not uf.stripped(fid):
                continue
            kind, body = uf.get(fid)
            cs, _ = ref(body.get('m_CorrespondingSourceObject'))
            pi, _ = ref(body.get('m_PrefabInstance'))
            child = subs.get(pi)
            o = child.objs.get(cs) if child else None
            if o is not None:
                inst.objs[fid] = o
        for o in own:
            o.data = _resolve(o.data, inst.objs)
        for fid, body in pis:
            child = subs.get(fid)
            if child is None:
                continue
            mod = dictof(body.get('m_Modification'))
            f, g = ref(mod.get('m_TransformParent'))
            if child.root_tf is not None:
                child.root_tf.data['m_Father'] = inst.objs.get(f) if f and not g else None
            for m in listof(mod.get('m_Modifications')):
                m = dictof(m)
                t, _ = ref(m.get('target'))
                o = child.objs.get(t)
                if o is None:
                    continue
                orf, org = ref(m.get('objectReference'))
                if orf:
                    val = dict(m['objectReference']) if org else inst.objs.get(orf)
                else:
                    val = m.get('value', '')
                    if not isinstance(val, str):
                        val = ''
                set_prop(o.data, str(m.get('propertyPath') or ''), val)
            for key in ('m_RemovedComponents', 'm_RemovedGameObjects'):
                for r in listof(mod.get(key)):
                    t, _ = ref(r)
                    o = child.objs.get(t)
                    if o is not None:
                        o.removed = True
        roots = [o for o in inst.all if o.cls in TRANSFORMS and not o.removed
                 and not isinstance(o.data.get('m_Father'), Obj)]
        if len(roots) == 1:
            inst.root_tf = roots[0]
        return inst


def _resolve(x, objs):
    """a document's local {fileID} references as the objects they name (external ones stay dicts)"""
    if isinstance(x, dict):
        if 'fileID' in x and len(x) <= 3 and set(x) <= {'fileID', 'guid', 'type'}:
            f, g = ref(x)
            if g:
                return x
            return objs.get(f) if f else None
        return {k: _resolve(v, objs) for k, v in x.items()}
    if isinstance(x, list):
        return [_resolve(v, objs) for v in x]
    return x


def build_graph(inst):
    """link GameObjects to their components, parents and children; returns the GameObjects"""
    gos = [o for o in inst.all if o.cls == 1 and not o.removed]
    for g in gos:
        g.comps, g.children, g.parent, g.tf = [], [], None, None
    for o in inst.all:
        if o.removed or o.cls == 1:
            continue
        g = o.go
        if g is not None and not g.removed:
            g.comps.append(o)
            if o.cls in TRANSFORMS and g.tf is None:
                g.tf = o
    for g in gos:
        f = g.tf.data.get('m_Father') if g.tf else None
        if isinstance(f, Obj) and not f.removed:
            pg = f.go
            if pg is not None and not pg.removed and pg is not g:
                g.parent = pg
                pg.children.append(g)
    for g in gos:
        if len(g.children) > 1 and g.tf is not None:
            order = {}
            for i, c in enumerate(listof(g.tf.data.get('m_Children'))):
                if isinstance(c, Obj):
                    order.setdefault(id(c), i)
            g.children.sort(key=lambda c: order.get(id(c.tf), 1 << 30))
    return gos


def is_descriptor(o):
    if o.cls != 114:
        return False
    f, g = o.script()
    if g == DESC_GUID and f == DESC_FID:
        return True
    d = o.data
    return 'ViewPosition' in d and ('VisemeSkinnedMesh' in d or 'lipSync' in d)


def is_script(o, fid, guid):
    f, g = o.script()
    return f == fid and g == guid


def is_physbone(c):
    d = c.data
    return c.cls == 114 and (is_script(c, PHYSBONE_FID, DYN_GUID) or ('pull' in d and 'multiChildType' in d))


def is_pb_collider(c):
    d = c.data
    return c.cls == 114 and (is_script(c, COLLIDER_FID, DYN_GUID) or ('shapeType' in d and 'insideBounds' in d))


def is_contact(c):
    return c.cls == 114 and 'collisionTags' in c.data and 'shapeType' in c.data


def is_unity_constraint(c):
    return c.cls != 114 and 'm_Sources' in c.data and 'm_Weight' in c.data


def is_constraint(c):
    """a Unity constraint or a VRChat one"""
    return is_unity_constraint(c) or (c.cls == 114 and 'Sources' in c.data and 'GlobalWeight' in c.data)


def ma_platform_out(gos):
    """the objects Modular Avatar's Platform Filter leaves out of a VRChat build (PlatformFilterPass, the first thing
    MA does): one whose filters include other platforms but not VRChat's, or one that excludes VRChat's"""
    by = {}
    for g in gos:
        for c in g.comps:
            if c.cls == 114 and (MA_OTHER.get(c.script()[1] or '') == 'PlatformFilter' or (
                    'm_platform' in c.data and 'm_excludePlatform' in c.data)):
                by.setdefault(id(g), []).append(c)
    out = set()
    for k, fs in by.items():
        ex = [(truthy(f.data.get('m_excludePlatform', '1')), str(f.data.get('m_platform') or '') == NDMF_VRCHAT)
              for f in fs]
        if any(e and here for e, here in ex) or (not all(e for e, _ in ex) and not any(
                here and not e for e, here in ex)):
            out.add(k)
    return out


def ma_kind(c):
    """which Modular Avatar component a MonoBehaviour is ('MergeArmature', 'BoneProxy', ...), or None"""
    if c.cls != 114:
        return None
    k = MA_SCRIPTS.get(c.script()[1] or '')
    if k is None:
        k = next((k for k, keys in MA_FIELDS if all(x in c.data for x in keys)), None)
    return k


# ---------------------------------------------------------------- finding the avatars

class Found:
    def __init__(self, asset, desc, root):
        self.asset, self.desc, self.root = asset, desc, root
        self.name = str(root.data.get('m_Name', '')) or asset.name
        self.score = 0


def find_avatars(db):
    texts = {}
    for a in db.assets.values():
        if a.ext in ('.prefab', '.unity'):
            try:
                with open(a.file, 'rb') as f:
                    texts[a.guid] = f.read().decode('utf-8', 'replace')
            except OSError:
                pass
    cand = {g for g, t in texts.items() if DESC_GUID in t or 'ViewPosition:' in t}
    uses = {g: set(re.findall(r'm_SourcePrefab: \{fileID: \d+, guid: ([0-9a-f]{32})', t))
            for g, t in texts.items()}
    grew = True
    while grew:
        grew = False
        for g, u in uses.items():
            if g not in cand and u & cand:
                cand.add(g)
                grew = True
    found = []
    world = World(db)
    for g in sorted(cand, key=lambda g: db.assets[g].path):
        a = db.assets[g]
        try:
            inst = world.instantiate(g)
        except Fail as e:
            warn(str(e))
            continue
        if inst is None:
            continue
        build_graph(inst)
        for o in inst.all:
            if not o.removed and is_descriptor(o) and o.go is not None and not o.go.removed:
                f = Found(a, o, o.go)
                s = 0
                low = (f.name + ' ' + a.path).lower()
                if a.ext == '.prefab':
                    s += 10
                if re.search(r'quest|android|mobile|\blite\b|_lite|ios', low):
                    s -= 20
                if not truthy(o.data.get('m_Enabled', '1')):
                    s -= 5
                if o.go.data.get('m_TagString') == 'EditorOnly':
                    s -= 30
                f.score = s
                found.append(f)
    found.sort(key=lambda f: (-f.score, f.asset.path, f.name))
    return found

# ---------------------------------------------------------------- the avatar, its animations and menus

RENDERERS = (137, 23)  # SkinnedMeshRenderer, MeshRenderer
# a Transform's properties as animation curves name them: position, rotation (quaternion), Euler angles, scale
TF_ATTRS = {'m_LocalPosition': 'p', 'm_LocalRotation': 'q', 'localEulerAnglesRaw': 'e', 'localEulerAngles': 'e',
            'm_LocalEulerAngles': 'e', 'localEulerAnglesBaked': 'e', 'm_LocalScale': 's'}
BUILTIN_PARAMS = {'IsLocal': 1, 'Grounded': 1, 'Upright': 1, 'TrackingType': 3, 'AvatarVersion': 3,
                  'VRMode': 0, 'IsAnimatorEnabled': 1, 'ScaleFactor': 1, 'ScaleFactorInverse': 1,
                  'EyeHeightAsMeters': 1.6, 'EyeHeightAsPercent': 0.5}
GESTURES = ['neutral', 'fist', 'open', 'point', 'victory', 'rocknroll', 'handgun', 'thumbsup']


def ptr(r, here):
    """a reference as (file guid, fileID); a local one points into the file `here`"""
    f, g = ref(r)
    return (g or here, f) if f else None


class Avatar:
    """the avatar under its descriptor: GameObjects, animator paths, renderers, default values"""

    def __init__(self, db, found):
        self.db = db
        self.root = found.root
        self.desc = found.desc.data
        self.name = found.name
        self.gos, self.paths = [], {}
        gone, todo, every = set(), [self.root], []  # what VRCFury deletes during the upload
        while todo:
            g = todo.pop()
            todo += g.children
            every.append(g)
            for c in g.comps:
                if is_vrcfury(c):
                    for f in vrcf_features(c):
                        if f['@class'] == 'DeleteDuringUpload':
                            t = f.get('@target', c.go)
                            gone.add(id(t if isinstance(t, Obj) and t.cls == 1 else getattr(t, 'go', None)))
        vf_gone, filtered = len(gone), ma_platform_out(every)
        gone |= filtered

        def walk(g, p):
            if g is not self.root and (g.data.get('m_TagString') == 'EditorOnly' or id(g) in gone):
                return
            self.gos.append(g)
            self.paths.setdefault(p, g)
            for c in g.children:
                walk(c, (p + '/' if p else '') + str(c.data.get('m_Name', '')))
        walk(self.root, '')
        if vf_gone:
            log('VRCFury: Delete During Upload leaves out %d object(s)' % vf_gone)
        if filtered:
            log('Modular Avatar: Platform Filter leaves out %d object(s) (not for VRChat)' % len(filtered))
        self.inside = set(map(id, self.gos))
        self.renderers = [c for g in self.gos for c in g.comps if c.cls in RENDERERS]
        self._shapes = {}
        self._mats = None
        self.ma = None  # the ModularAvatar once it has run
        self.tf_rest = {}  # id(GameObject) -> {'p', 'q', 'e', 's'}: its Transform's local values (transform_rests)

    def comp(self, go, cls):
        for c in go.comps:
            if c.cls == cls:
                return c
        return None

    def shape_names(self, smr):
        """the blend shape names of a renderer's mesh, in order"""
        n = self._shapes.get(id(smr))
        if n is None:
            n = []
            m = smr.data.get('m_Mesh')
            if smr.fbx is not None and smr.node in smr.fbx.info.mesh:
                if isinstance(m, dict) and 'fbxmesh' not in m and ref(m)[1] not in (None, smr.fbx.guid):
                    warn('%s: its mesh is swapped for another one, which is not converted' % smr.gname)
                n = smr.fbx.info.mesh[smr.node]['shapes']
            self._shapes[id(smr)] = n
        return n

    def default(self, prop):
        k, o = prop[0], prop[1]
        if k == 'a':
            return 1.0 if truthy(o.data.get('m_IsActive', '1')) else 0.0
        if k == 'c':  # an MA Mesh Cutter's cut (or a Shape Changer's delete): not in effect
            return 0.0
        if k == 't':  # a Transform's local value, as the avatar has it placed (transform_rests); 'm': a Scale multiplier
            rest = self.tf_rest.get(id(o))
            c = prop[2]
            if c == 'm':
                return 1.0
            if rest is None:
                return 1.0 if c[0] == 's' or c == 'qw' else 0.0
            return rest[c[0]]['xyzw'.index(c[1])]
        if k == 'e':
            return 1.0 if truthy(o.data.get('m_Enabled', '1')) else 0.0
        if k == 'm':
            ms = listof(o.data.get('m_Materials'))
            if prop[2] >= len(ms):
                return None
            x = ms[prop[2]]
            if isinstance(x, dict) and 'fbxmat' in x and o.fbx is not None:  # a model's material a .mat stands in for
                r = self.materials().remaps(o.fbx.guid).get(x['fbxmat'])
                if matkey(r) is not None:
                    return matkey(r)
            return matkey(x)
        if k == 'mp':
            return next((v for v in (self.matprop(o, i, prop[2]) for i in range(len(listof(o.data.get('m_Materials')))))
                         if v is not None), None)
        names = self.shape_names(o)
        if prop[2] in names:
            w = listof(o.data.get('m_BlendShapeWeights'))
            i = names.index(prop[2])
            return num(w[i]) if i < len(w) else 0.0
        return 0.0

    def materials(self):
        """the materials, read when first asked for"""
        if self._mats is None:
            self._mats = Materials(self.db)
        return self._mats

    def matprop(self, r, k, name, key=None):
        """a property hypr3d carries ("_Color.r", "_MainTex_ST.z", "_Cutoff") of the material in a renderer's slot k (or
        of the one `key` names); None if that material has no such property"""
        mk = key if key is not None else self.default(('m', r, k))
        if not mk or mk[0] == 'fbx':
            return None
        p = self.materials().props(mk[0], mk[1])
        if p is None:
            return None
        _, _, _, _, _, tex, fl, col = p
        base, _, ch = name.partition('.')
        if not ch:
            return fl.get(base)
        if base.endswith('_ST'):
            t = tex.get(base[:-3])
            return None if t is None else {'x': t[1][0], 'y': t[1][1], 'z': t[2][0], 'w': t[2][1]}.get(ch)
        c = col.get(base)
        return None if c is None else {'r': c[0], 'g': c[1], 'b': c[2], 'a': c[3], 'x': c[0], 'y': c[1], 'z': c[2],
                                       'w': c[3]}.get(ch)

    def prop(self, path, cls, attr):
        """what an animation curve drives: ('a', GameObject) active, ('e', renderer) enabled,
        ('s', renderer, shape) a blend shape weight, ('m', renderer, slot) a material slot, ('mp', renderer,
        "_Color.r") a property of its materials that hypr3d carries; None for anything else"""
        go = self.paths.get(path)
        if go is None:
            return None
        if cls == 1 and attr == 'm_IsActive':
            return ('a', go)
        if cls == 95 and attr in MUSCLE_OF:  # a humanoid muscle: ('h', the root, MUSCLES index)
            return ('h', go, MUSCLE_OF[attr])
        if cls == 4:  # a Transform: ('t', GameObject, 'px'..'pz' | 'qx'..'qw' | 'ex'..'ez' | 'sx'..'sz'), local values
            k, _, axis = attr.rpartition('.')
            kind = TF_ATTRS.get(k)
            return ('t', go, kind + axis) if kind and axis in ('xyzw' if kind == 'q' else 'xyz') else None
        if cls in RENDERERS and attr == 'm_Enabled':
            c = self.comp(go, cls)
            return ('e', c) if c else None
        if cls == 137 and attr.startswith('blendShape.'):
            c = self.comp(go, 137)
            return ('s', c, attr[11:]) if c else None
        if cls in RENDERERS and (attr.startswith('m_Materials.Array.data[') or attr.startswith('material.')):
            c = self.comp(go, cls)
            m = re.fullmatch(r'm_Materials\.Array\.data\[(\d+)\]', attr)
            if c is not None and m:
                return ('m', c, int(m.group(1)))
            if c is not None and attr[9:] in MP_NAMES:
                return ('mp', c, attr[9:])
        return None

    def visible(self, r, vals):
        """is a renderer drawn, with these animated values over the defaults"""
        def get(p):
            v = vals.get(p)
            return self.default(p) if v is None else v
        if get(('e', r)) <= 0.5:
            return False
        # what MA merges still shows and hides with its old parents (MA puts stand-ins for them in between); what a
        # Bone Proxy moves goes with its new ones
        g = r.go
        while g is not None and g is not self.root:
            if get(('a', g)) <= 0.5:
                return False
            g = self.ma.vparent.get(id(g), g.parent) if self.ma else g.parent
        return True


def av_desc_collider(desc, slot):
    """one of the avatar descriptor's colliders ("head", "handL", "fingerIndexR"...): {state, transform, radius,
    height, position, rotation}"""
    return dictof(desc.get('collider_' + slot))


def blend2d(pts, p, polar=True):
    """the weights of a 2D blend tree's children at p: gradient band interpolation (Johansen's, as Unity's 2D blend trees
    use it), in polar space for the directional kinds (magnitudes and angles, the angles counting twice); a child at
    the origin is the one with no direction"""
    n = len(pts)
    if n <= 1:
        return [1.0] * n
    P = [Vector(q) for q in pts]
    X = Vector(p)
    ang = lambda a, b: math.atan2(a.x * b.y - a.y * b.x, a.dot(b))
    out = []
    for i in range(n):
        w = 1.0
        for j in range(n):
            if i == j:
                continue
            if polar:
                li, lj, lp = P[i].length, P[j].length, X.length
                if li < 1e-6 and lj < 1e-6:
                    continue
                mean = (li + lj) / 2
                if li < 1e-6:  # from the middle out toward j: by how far only
                    vij, vip = Vector((lj / mean, 0.0)), Vector((lp / mean, 0.0))
                elif lj < 1e-6:
                    vij, vip = Vector((-li / mean, 0.0)), Vector(((lp - li) / mean, 0.0))
                else:
                    a = ang(P[i], X) if lp > 1e-6 else 0.0
                    vij = Vector(((lj - li) / mean, 2.0 * ang(P[i], P[j])))
                    vip = Vector(((lp - li) / mean, 2.0 * a))
            else:
                vij, vip = P[j] - P[i], X - P[i]
            dd = vij.dot(vij)
            if dd < 1e-12:
                continue
            w = min(w, 1.0 - vip.dot(vij) / dd)
        out.append(max(w, 0.0))
    tot = sum(out)
    if tot <= 1e-9:  # past the ends: the nearest
        k = min(range(n), key=lambda i: (P[i] - X).length)
        return [1.0 if i == k else 0.0 for i in range(n)]
    return [w / tot for w in out]


def join_path(base, path):
    return base + '/' + path if base and path else base or path


class Clip:
    def __init__(self, av, body, base='', rewrite=None):
        self.name = str(body.get('m_Name', ''))
        st = dictof(body.get('m_AnimationClipSettings'))
        self.loop = truthy(st.get('m_LoopTime', '0'))
        self.curves = []  # (prop, [(time, value)])
        self.other = 0  # curves this tool does not carry over
        end = 0.0

        def find(path, cid, attr):
            """what a curve drives (see Avatar.prop); False for a binding the rewrite deletes"""
            if rewrite is not None:
                path = rewrite(path)
                if path is None:
                    return False
            if isinstance(base, tuple):  # VRCFury's: the first of these places that has it ("/...": the root)
                return av.prop(path[1:], cid, attr) if path.startswith('/') else next(
                    (x for x in (av.prop(join_path(b, path), cid, attr) for b in base) if x is not None), None)
            return av.prop(join_path(base, path), cid, attr)
        # a Transform's curves: vectors of its position, rotation (a quaternion, or Euler angles) or scale; Unity
        # keeps them as floats among the editor curves too
        vectors = (('m_PositionCurves', 'm_LocalPosition', 'xyz'), ('m_RotationCurves', 'm_LocalRotation', 'xyzw'),
                   ('m_EulerCurves', 'localEulerAnglesRaw', 'xyz'), ('m_ScaleCurves', 'm_LocalScale', 'xyz'))
        has_vectors = any(listof(body.get(k)) for k, _, _ in vectors)
        for arr, attr, axes in vectors:
            for c in listof(body.get(arr)):
                c = dictof(c)
                ks = sorted(((num(k.get('time')), dictof(k.get('value'))) for k in listof(dictof(c.get('curve')).get(
                    'm_Curve')) if isinstance(k, dict)), key=lambda k: k[0])
                if not ks:
                    continue
                for a in axes:
                    p = find(str(c.get('path') or ''), 4, '%s.%s' % (attr, a))
                    if p is False:
                        continue
                    if p is None:
                        self.other += 1
                        continue
                    self.curves.append((p, [(t, num(v.get(a))) for t, v in ks]))
                    end = max(end, ks[-1][0])
        curves = listof(body.get('m_FloatCurves')) or listof(body.get('m_EditorCurves'))
        for c in curves:
            c = dictof(c)
            keys = sorted((num(k.get('time')), num(k.get('value')))
                          for k in listof(dictof(c.get('curve')).get('m_Curve')) if isinstance(k, dict))
            if not keys:
                continue
            cid = inum(c.get('classID'))
            if cid == 4 and has_vectors:  # the vectors above have them
                continue
            p = find(str(c.get('path') or ''), cid, str(c.get('attribute') or ''))
            if p is False:
                continue
            if p is None:
                self.other += 1
                continue
            self.curves.append((p, keys))
            end = max(end, keys[-1][0])
        for c in listof(body.get('m_PPtrCurves')):  # object references: materials put in a renderer's slots
            c = dictof(c)
            p = find(str(c.get('path') or ''), inum(c.get('classID')), str(c.get('attribute') or ''))
            if p is False:
                continue
            keys = sorted(((num(k.get('time')), matkey(k.get('value'))) for k in listof(c.get('curve'))
                           if isinstance(k, dict)), key=lambda k: k[0])
            if p is None or p[0] != 'm' or not keys:
                self.other += 1
                continue
            self.curves.append((p, keys))
            end = max(end, keys[-1][0])
        self.length = max(num(st.get('m_StopTime'), end) - num(st.get('m_StartTime'), 0.0), 0.0) or end

    def sample(self, u=None):
        """{prop: value} at normalized time u; None: where the clip rests (its start if it loops, else its end)"""
        out = {}
        for p, keys in self.curves:
            if u is None:
                v = keys[0][1] if self.loop else keys[-1][1]
            else:
                # a looping clip wraps around at the end, as a radial puppet at 100% does in VRChat
                t = (u % 1.0 if self.loop else min(max(u, 0.0), 1.0)) * self.length
                v = keys[-1][1]
                if t <= keys[0][0]:
                    v = keys[0][1]
                else:
                    for (t0, v0), (t1, v1) in zip(keys, keys[1:]):
                        if t < t1:
                            v = v0 if p[0] not in ('s', 'mp', 't', 'h') or t1 <= t0 else v0 + (v1 - v0) * (t - t0) / (
                                t1 - t0)
                            break
            out[p] = v
        return out


class Anim:
    """clips, blend trees and animator controllers, read when first used; base: where their paths start (an
    animator MA merges with relative paths), or a tuple of places to look in turn (VRCFury's)"""

    def __init__(self, db, av, base='', rewrite=None):
        self.db, self.av, self.base, self.rewrite = db, av, base, rewrite
        self._clips = {}
        self.other = set()  # names of clips with curves that are not converted

    def body(self, p, cls=None):
        if p is None:
            return None
        a = self.db.get(p[0])
        if a is None or a.ext in MODEL_EXT:
            return None
        uf = self.db.yaml(p[0])
        if uf is None or uf.binary or (cls is not None and uf.cls(p[1]) != cls):
            return None
        return uf.get(p[1])[1]

    def cls(self, p):
        a = self.db.get(p[0]) if p else None
        if a is None or a.ext in MODEL_EXT:
            return None
        uf = self.db.yaml(p[0])
        return uf.cls(p[1]) if uf and not uf.binary else None

    def clip(self, p):
        if p not in self._clips:
            b = self.body(p, 74)
            self._clips[p] = Clip(self.av, b, self.base, self.rewrite) if b is not None else None
        return self._clips[p]

    def mix(self, parts):
        """[(weight, values)] with weights adding up to one"""
        props = set()
        for _, d in parts:
            props.update(d)
        out = {}
        for pr in props:
            vs = [(w, d[pr] if pr in d else self.av.default(pr)) for w, d in parts]
            if pr[0] == 'm' or any(v is None for _, v in vs):  # a material: the one with the most weight
                out[pr] = max(vs, key=lambda x: x[0])[1]
            else:
                out[pr] = sum(w * v for w, v in vs)
        return out

    def motion(self, p, params, u, over, names, depth=0):
        """the values a clip or blend tree sets; `names` collects the clips used, by property"""
        if p is None or depth > 20:
            return {}
        p = over.get(p, p)
        c = self.cls(p)
        if c == 74:
            clip = self.clip(p)
            if clip is None:
                return {}
            if clip.other:
                self.other.add(clip.name)
            v = clip.sample(u)
            for pr in v:
                names.setdefault(pr, clip.name)
            return v
        if c != 206:
            return {}
        bt = self.body(p)
        kids = [dictof(k) for k in listof(bt.get('m_Childs'))]
        if not kids:
            return {}
        typ = inum(bt.get('m_BlendType'))

        def val(k):
            return self.motion(ptr(k.get('m_Motion'), p[0]), params, u, over, names, depth + 1)
        if typ == 4:
            ws = [params.get(str(k.get('m_DirectBlendParameter') or ''), 0.0) for k in kids]
            if truthy(bt.get('m_NormalizedBlendValues', '0')) and sum(ws) > 0:
                ws = [w / sum(ws) for w in ws]
            out, most = {}, {}
            for w, k in zip(ws, kids):
                if w == 0:
                    continue
                for pr, v in val(k).items():
                    d = self.av.default(pr)
                    if pr[0] == 'm' or d is None or v is None:  # a material: the child with the most weight sets it
                        if w > most.get(pr, 0.0):
                            most[pr] = w
                            out[pr] = v
                        continue
                    out[pr] = out.get(pr, d) + w * (v - d)
            return out
        x = params.get(str(bt.get('m_BlendParameter') or ''), 0.0)
        if typ == 0:
            kids.sort(key=lambda k: num(k.get('m_Threshold')))
            ts = [num(k.get('m_Threshold')) for k in kids]
            if x <= ts[0]:
                return val(kids[0])
            if x >= ts[-1]:
                return val(kids[-1])
            for i in range(len(kids) - 1):
                if ts[i] <= x <= ts[i + 1]:
                    f = (x - ts[i]) / (ts[i + 1] - ts[i]) if ts[i + 1] > ts[i] else 1.0
                    if f <= 0:
                        return val(kids[i])
                    if f >= 1:
                        return val(kids[i + 1])
                    return self.mix([(1 - f, val(kids[i])), (f, val(kids[i + 1]))])
            return val(kids[-1])
        y = params.get(str(bt.get('m_BlendParameterY') or ''), 0.0)
        pts = [vec3(k.get('m_Position'))[:2] for k in kids]
        ws = blend2d(pts, (x, y), polar=typ in (1, 2))  # Simple and Freeform Directional: polar; Cartesian: not
        parts = [(w, val(k)) for w, k in zip(ws, kids) if w > 1e-6]
        return self.mix(parts) if len(parts) > 1 else parts[0][1] if parts else {}


class Controller:
    """an animator controller (or an override controller over one), evaluated for fixed parameters"""

    def __init__(self, anim, guid):
        self.anim, self.db = anim, anim.db
        self.over = {}
        self.guid = None
        self.name = ''
        seen = set()
        while guid and guid not in seen:
            seen.add(guid)
            uf = self.db.yaml(guid)
            if uf is None or uf.binary:
                break
            fid = uf.main(91) or uf.main(221)
            if fid is None:
                break
            kind, body = uf.get(fid)
            self.name = self.name or str(body.get('m_Name', ''))
            if uf.cls(fid) == 221:
                for c in listof(body.get('m_Clips')):
                    c = dictof(c)
                    a, b = ptr(c.get('m_OriginalClip'), guid), ptr(c.get('m_OverrideClip'), guid)
                    if a and b and a not in self.over:
                        self.over[a] = b
                guid = ref(body.get('m_Controller'))[1]
                continue
            self.guid, self.body = guid, body
            break
        self.params = {}
        self.layers = []
        if self.guid is None:
            return
        for p in listof(self.body.get('m_AnimatorParameters')):
            p = dictof(p)
            t = inum(p.get('m_Type'))
            d = {1: num(p.get('m_DefaultFloat')), 3: inum(p.get('m_DefaultInt')),
                 4: 1.0 if truthy(p.get('m_DefaultBool', '0')) else 0.0}.get(t, 0.0)
            self.params[str(p.get('m_Name', ''))] = (t, float(d))
        for i, l in enumerate(listof(self.body.get('m_AnimatorLayers'))):
            l = dictof(l)
            self.layers.append({
                'name': str(l.get('m_Name', '')),
                'sm': ptr(l.get('m_StateMachine'), self.guid),
                'mask': mask_parts(self.db, ptr(l.get('m_Mask'), self.guid)),
                'weight': 1.0 if i == 0 else num(l.get('m_DefaultWeight'), 1.0),
                'synced': inum(l.get('m_SyncedLayerIndex'), -1),
                'motions': {ptr(dictof(m).get('m_State'), self.guid): ptr(dictof(m).get('m_Motion'), self.guid)
                            for m in listof(l.get('m_Motions'))},
            })

    def doc(self, p):
        return self.anim.body(p) or {}

    def cond(self, t, params):
        for c in listof(t.get('m_Conditions')):
            c = dictof(c)
            mode = inum(c.get('m_ConditionMode'))
            v = params.get(str(c.get('m_ConditionEvent') or ''), 0.0)
            th = num(c.get('m_EventTreshold'))
            ok = {1: v != 0, 2: v == 0, 3: v > th, 4: v < th,
                  6: round(v) == round(th), 7: round(v) != round(th)}.get(mode, False)
            if not ok:
                return False
        return True

    def usable(self, ts):
        """transitions that count: the solo ones if there are any, never the muted"""
        ts = [t for t in ts if t is not None]
        solo = [t for t in ts if truthy(t.get('m_Solo', '0'))]
        return [t for t in (solo or ts) if not truthy(t.get('m_Mute', '0'))]

    def enter(self, sm, params, depth=0):
        """the state entering a state machine leads to"""
        if sm is None or depth > 16:
            return None
        b = self.doc(sm)
        for t in self.usable([self.doc(ptr(x, sm[0])) for x in listof(b.get('m_EntryTransitions'))]):
            if self.cond(t, params):
                d = self.dest(t, sm[0], params, depth)
                if d is not None:
                    return d
        d = ptr(b.get('m_DefaultState'), sm[0])
        if d is None:
            kids = listof(b.get('m_ChildStates'))
            d = ptr(dictof(kids[0]).get('m_State'), sm[0]) if kids else None
        return d

    def dest(self, t, here, params, depth=0):
        s = ptr(t.get('m_DstState'), here)
        if s is not None:
            return s
        m = ptr(t.get('m_DstStateMachine'), here)
        if m is not None:
            return self.enter(m, params, depth + 1)
        return None

    def machines(self, sm, out=None, depth=0):
        out = [] if out is None else out
        if sm is None or sm in out or depth > 16:
            return out
        out.append(sm)
        for c in listof(self.doc(sm).get('m_ChildStateMachines')):
            self.machines(ptr(dictof(c).get('m_StateMachine'), sm[0]), out, depth + 1)
        return out

    def settle(self, sm, params):
        """the states a layer passes through to where it rests with these parameters"""
        path = []
        s = self.enter(sm, params)
        anys = []
        for m in self.machines(sm):
            anys += [self.doc(ptr(x, m[0])) for x in listof(self.doc(m).get('m_AnyStateTransitions'))]
        anys = self.usable(anys)
        while s is not None and len(path) < 64:
            if s in path:
                break
            path.append(s)
            nxt = None
            for t in anys:
                d = ptr(t.get('m_DstState'), sm[0])
                if d == s and not truthy(t.get('m_CanTransitionToSelf', '1')):
                    continue
                if self.cond(t, params):
                    nxt = self.dest(t, sm[0], params) if d is None else d
                    break
            if nxt is None:
                b = self.doc(s)
                for t in self.usable([self.doc(ptr(x, s[0])) for x in listof(b.get('m_Transitions'))]):
                    conds = listof(t.get('m_Conditions'))
                    if not conds and not truthy(t.get('m_HasExitTime', '0')):
                        continue
                    if self.cond(t, params):
                        if truthy(t.get('m_IsExit', '0')):
                            nxt = self.enter(sm, params)
                        else:
                            nxt = self.dest(t, s[0], params)
                        break
            if nxt is None or nxt == s:
                break
            s = nxt
        return path

    def behaviours(self, s):
        return [self.doc(ptr(x, s[0])) for x in listof(self.doc(s).get('m_StateMachineBehaviours'))]

    def evaluate(self, params):
        """(values, clip names by property, tracking {'eyes', 'mouth'}, parameters changed by drivers)"""
        vals, names, track = {}, {}, {}
        params = dict(params)
        for it in range(4):
            vals, names, track, changed = {}, {}, {}, False
            for layer in self.layers:
                if layer['weight'] <= 0.001:
                    continue
                src = layer
                if layer['synced'] >= 0:
                    if layer['synced'] >= len(self.layers):
                        continue
                    src = self.layers[layer['synced']]
                path = self.settle(src['sm'], params)
                if not path:
                    continue
                for s in path:
                    for b in self.behaviours(s):
                        if 'trackingEyes' in b:
                            for k, f in (('eyes', 'trackingEyes'), ('mouth', 'trackingMouth')):
                                v = inum(b.get(f))
                                if v:
                                    track[k] = v
                        if 'localOnly' in b and isinstance(b.get('parameters'), list):
                            for d in listof(b.get('parameters')):
                                d = dictof(d)
                                t, nm = inum(d.get('type')), str(d.get('name') or '')
                                if t == 0:
                                    v = num(d.get('value'))
                                elif t == 3:
                                    v = params.get(str(d.get('source') or ''), 0.0)
                                else:
                                    continue
                                if nm and params.get(nm) != v:
                                    params[nm] = v
                                    changed = True
                s = path[-1]
                st = self.doc(s)
                m = layer['motions'].get(s) if layer is not src else ptr(st.get('m_Motion'), s[0])
                u = None
                if truthy(st.get('m_TimeParameterActive', '0')):
                    u = params.get(str(st.get('m_TimeParameter') or ''), 0.0)
                ln = {}  # a later layer's clip names what it sets, as its values win
                got = self.anim.motion(m, params, u, self.over, ln)
                if layer['mask'] is not None:  # the humanoid parts its avatar mask lets it move
                    got = {pr: v for pr, v in got.items() if pr[0] != 'h' or muscle_part(pr[2]) in layer['mask']}
                vals.update(got)
                names.update(ln)
            if not changed:
                break
        return vals, names, track, params


class MotionLayer:
    """what MA's Merge Motion (Blend Tree) components merge: one FX layer before all the others, a direct blend tree
    that plays each of their motions at full weight; parts: [(Anim, motion, {its parameter names: the avatar's})]"""

    def __init__(self, parts):
        self.parts = parts
        self.guid = parts[0][1][0] if parts else None
        self.name = 'ModularAvatar: Merge Blend Tree'
        self.params = {}  # the parameters its trees blend by: floats, 0 unless FX says otherwise
        for anim, p, view in parts:
            for pn in self.tree_params(anim, p, set()):
                self.params.setdefault(view.get(pn, pn), (1, 0.0))

    @staticmethod
    def tree_params(anim, p, seen, depth=0):
        if p is None or p in seen or depth > 20 or anim.cls(p) != 206:
            return []
        seen.add(p)
        bt = anim.body(p) or {}
        typ, out = inum(bt.get('m_BlendType')), []
        kids = [dictof(k) for k in listof(bt.get('m_Childs'))]
        if typ == 4:
            out += [_str(k.get('m_DirectBlendParameter')) for k in kids]
        else:
            out.append(_str(bt.get('m_BlendParameter')))
            if typ != 0:
                out.append(_str(bt.get('m_BlendParameterY')))
        for k in kids:
            out += MotionLayer.tree_params(anim, ptr(k.get('m_Motion'), p[0]), seen, depth + 1)
        return [x for x in out if x]

    def evaluate(self, params):
        vals, names, most = {}, {}, set()
        for anim, p, view in self.parts:
            local = dict(params)
            for a, b in view.items():
                if b in params:
                    local[a] = params[b]
            for pr, v in anim.motion(p, local, None, {}, names).items():
                d = anim.av.default(pr)
                if pr[0] == 'm' or d is None or v is None:  # a material: the first child sets it
                    if pr not in most:
                        most.add(pr)
                        vals[pr] = v
                    continue
                vals[pr] = vals.get(pr, d) + (v - d)
        return vals, names, {}, params


# VRChat's SDK's avatar masks (not in avatar packages: the SDK is installed on its own), by guid: the humanoid parts
# (AvatarMaskBodyPart) each lets a layer move
VRC_MASKS = {'7ff0199655202a04eb175de45a6e078a': {7},  # vrc_Hand Left
             '903ce375d5f609d44b9f00b425d6eda9': {8},  # vrc_Hand Right
             'b2b8bad9583e56a46a3e21795e96ad92': {7, 8},  # vrc_HandsOnly
             '2bd8e9669f928cb47854a2dd69b5c54f': set(range(9)),  # vrc_MusclesOnly
             '30ba51f92a6526c4b9bdf7001676046f': set()}  # vrc_FXNoMusclesIkTransforms


def mask_parts(db, p):
    """an AvatarMask's humanoid parts a layer may move (0 root, 1 body, 2 head, 3/4 left/right leg, 5/6 arm, 7/8
    fingers, 9-12 IK goals), or None for no mask (all of them)"""
    if not p or not p[0]:
        return None
    if p[0] in VRC_MASKS:
        return set(VRC_MASKS[p[0]])
    uf = db.yaml(p[0]) if db is not None and db.get(p[0]) is not None else None
    if uf is None or uf.binary:
        return None
    fid = p[1] if uf.cls(p[1]) == 319 else uf.main(319)
    if fid is None:
        return None
    raw = uf.get(fid)[1].get('m_Mask')
    if isinstance(raw, str) and re.fullmatch(r'[0-9a-fA-F]*', raw):
        vals = [struct.unpack('<I', bytes.fromhex(raw[i:i + 8]))[0] for i in range(0, len(raw) - 7, 8)]
    else:
        vals = [inum(x) for x in listof(raw)]
    return {i for i, v in enumerate(vals) if v}


def muscle_part(i):
    """the AvatarMaskBodyPart a muscle (MUSCLES index) moves"""
    b = MUSCLES[i][1]
    if 24 <= b <= 38:
        return 7
    if 39 <= b <= 53:
        return 8
    return {0: 1, 7: 1, 8: 1, 54: 1, 9: 2, 10: 2, 21: 2, 22: 2, 23: 2, 1: 3, 3: 3, 5: 3, 19: 3, 2: 4, 4: 4, 6: 4,
            20: 4, 11: 5, 13: 5, 15: 5, 17: 5, 12: 6, 14: 6, 16: 6, 18: 6}.get(b, 1)


def read_menu(db, guid, out, prefix=(), seen=None, depth=0):
    """the controls of an Expressions Menu and its sub menus: [(path of menu names, control)]"""
    seen = set() if seen is None else seen
    if not guid or guid in seen or depth > 12:
        return out
    seen.add(guid)
    uf = db.yaml(guid)
    if uf is None or uf.binary:
        return out
    fid = uf.main(114)
    if fid is None:
        return out
    body = uf.get(fid)[1]
    for c in listof(body.get('controls')):
        c = dictof(c)
        t = inum(c.get('type'))
        name = rich_text(str(c.get('name') or ''))
        if t == 103:
            read_menu(db, ref(c.get('subMenu'))[1], out, prefix + (name,), seen, depth + 1)
        else:
            out.append((prefix, c))
    return out


def read_params(db, guid):
    """an Expression Parameters asset: {name: (value type, default)}"""
    out = {}
    uf = db.yaml(guid) if guid else None
    if uf is None or uf.binary:
        return out
    fid = uf.main(114)
    if fid is None:
        return out
    for p in listof(uf.get(fid)[1].get('parameters')):
        p = dictof(p)
        nm = str(p.get('name') or '')
        if nm:
            out[nm] = (inum(p.get('valueType')), num(p.get('defaultValue')))
    return out

# ---------------------------------------------------------------- Modular Avatar's menus and toggles

MA_REACTIVE = ('ObjectToggle', 'ShapeChanger', 'MaterialSetter', 'MaterialSwap', 'MeshCutter')
MA_MENU = ('MenuItem', 'MenuGroup', 'MenuInstaller', 'ObjectToggle', 'ShapeChanger', 'MergeAnimator', 'Parameters',
           'MaterialSetter', 'MaterialSwap', 'BlendshapeSync', 'MergeBlendTree', 'MenuInstallTarget', 'MeshCutter')


def ma_objref(av, r):
    """AvatarObjectReference.Get on the hierarchy as the files have it (MA reads every reference before it moves
    anything)"""
    r = dictof(r)
    path = r.get('referencePath')
    if not isinstance(path, str) or not path:
        return None
    t = r.get('targetObject')
    if isinstance(t, Obj):
        t = t if t.cls == 1 else t.go
        if t is not None and id(t) in av.inside:
            return t
    if path == '$$$AVATAR_ROOT$$$':
        return av.root
    g = tf_find(av.root, path)
    if g is not None and g.parent is not None and go_name(g) == 'Armature' and not g.children:
        g = next((x for x in g.parent.children if go_name(x) == 'Armature' and x.children), g)
    return g if g is not None and id(g) in av.inside else None


def vertex_filter(av, c):
    """an MA vertex filter (one of a Mesh Cutter's components) as {'kind': 'axis', 'bone', 'mask', 'shape' or 'uvtile',
    its settings, 'mode': 0 any vertex, 1 all vertices or 2 the centroid of a triangle picks it, 'key': what tells it
    from another}; None if c is none"""
    k, d = ma_kind(c), c.data
    mode = inum(d.get('m_selectionMode'))
    if mode not in (0, 1, 2):
        mode = 0
    if k == 'VertexFilterByAxis':
        f = {'kind': 'axis', 'center': vec3(d.get('m_center')), 'axis': vec3(d.get('m_axis'), (-1.0, 0.0, 0.0))}
    elif k == 'VertexFilterByBone':
        b = ma_objref(av, d.get('m_bone'))
        f = {'kind': 'bone', 'bone': b, 'threshold': min(max(num(d.get('m_threshold'), 0.01), 0.0), 1.0)}
        mode = 0 if mode == 2 else mode
    elif k == 'VertexFilterByMask':
        f = {'kind': 'mask', 'slot': inum(d.get('m_materialIndex')), 'texture': ref(d.get('m_maskTexture'))[1],
             'white': inum(d.get('m_deleteMode')) == 1, 'uv': min(max(inum(d.get('m_uvChannel')), 0), 7)}
    elif k == 'VertexFilterByShape':
        f = {'kind': 'shape', 'shapes': [str(x) for x in listof(d.get('m_shapes')) if str(x)],
             'threshold': num(d.get('m_threshold'), 0.001)}
        mode = 0 if mode == 2 else mode
    elif k == 'VertexFilterByUVTile':
        side = lambda n, dflt: (truthy(d.get('m_use' + n, '0')), truthy(d.get('m_%sInclusive' % n[0].lower() + n[1:],
                                                                               '0')), num(d.get('m_' + n[0].lower() + n[1:]), dflt))
        f = {'kind': 'uvtile', 'uv': min(max(inum(d.get('m_uvChannel')), 0), 7), 'umin': side('UMin', 0.0),
             'umax': side('UMax', 1.0), 'vmin': side('VMin', 0.0), 'vmax': side('VMax', 1.0),
             'invert': truthy(d.get('m_invert', '0'))}
    else:
        return None
    f['mode'] = mode
    f['key'] = tuple((a, id(v) if isinstance(v, Obj) else tuple(v) if isinstance(v, list) else v)
                     for a, v in sorted(f.items()))
    return f


def av_path(av, g):
    parts = []
    while g is not None and g is not av.root:
        parts.append(go_name(g))
        g = g.parent
    return '/'.join(reversed(parts))


def go_chain(g):
    """g and its parents"""
    out = []
    while g is not None:
        out.append(g)
        g = g.parent
    return out


def set_data(av, pr, v):
    """change the avatar's own data: an object on or off, a shape key's weight (what VRCFury and MA leave in the
    avatar they upload)"""
    if pr[0] == 'a':
        pr[1].data['m_IsActive'] = '1' if v >= 0.5 else '0'
    elif pr[0] == 's':
        names = av.shape_names(pr[1])
        if pr[2] in names:
            w = pr[1].data.get('m_BlendShapeWeights')
            if not isinstance(w, list):
                w = pr[1].data['m_BlendShapeWeights'] = []
            i = names.index(pr[2])
            while len(w) <= i:
                w.append('0')
            w[i] = v


def remap(v, pts):
    """a value through a remap curve's points (MA's RemapCurve: straight lines between them, and on past the ends)"""
    if not pts or len(pts) < 2:
        return v
    for x, y in pts:
        if x == v:
            return y
    i = sum(1 for x, _ in pts if x < v)
    r = min(max(i - 1, 0), len(pts) - 2)
    (x0, y0), (x1, y1) = pts[r], pts[r + 1]
    return y0 + (y1 - y0) / (x1 - x0) * (v - x0) if x1 != x0 else y0


def follow_shapes(vals, syncs):
    """shape keys that follow another mesh's (MA's Blendshape Sync, VRCFury's Blend Shape Link): what animates the
    source, remapped, animates them too"""
    for src, a, dst, b, pts in syncs:
        v = vals.get(('s', src, a))
        if v is not None:
            vals[('s', dst, b)] = remap(v, pts)


class MAToggles:
    """what Modular Avatar (1.18) adds to the menu and FX: menu items under menu installers (and the parameters MA
    gives items that have none), Object Toggles and Shape Changers (reactive components: in effect while their menu
    item is on and their object active), animators merged into FX, and parameters from MA Parameters"""

    def __init__(self, an, comps):
        self.an, self.av, self.db, self.comps = an, an.av, an.db, comps
        self.item = {}  # id(MenuItem) -> (parameter, value)
        self.declared = {}  # parameter -> (value type, default): MA Parameters', and the ones MA makes for items
        self.renames = {}  # id(GameObject with MA Parameters) -> {name: the name it becomes}
        for c in comps.get('Parameters', []):
            for e in listof(c.data.get('parameters')):
                e = dictof(e)
                name, to = _str(e.get('nameOrPrefix')), _str(e.get('remapTo'))
                if not name or truthy(e.get('isPrefix', '0')):
                    continue
                if to:
                    self.renames.setdefault(id(c.go), {})[name] = to
                sync = inum(e.get('syncType'))
                if sync in (1, 2, 3):
                    self.declared.setdefault(to or name, ({1: 0, 2: 1, 3: 2}[sync], num(e.get('defaultValue'))))
        self.assign()
        self.cutters = {}  # (id(renderer), key) -> {'r', 'multi', 'filters', 'labels'}: what ('c', renderer, key) cuts
        self.rules = self.reactions()
        self.merged = self.animators()
        self.syncs = self.blendshape_syncs()
        for src, a, dst, b, pts in self.syncs:  # the followers rest as their sources do
            set_data(self.av, ('s', dst, b), remap(self.av.default(('s', src, a)), pts))

    # ---- parameters

    def rename(self, g, name):
        """a parameter's name after the MA Parameters above g rename it"""
        while g is not None:
            name = self.renames.get(id(g), {}).get(name, name)
            g = g.parent
        return name

    def view(self, g):
        """{name in an animator under g: the avatar's name for it}"""
        out, x, chain = {}, g, []
        while x is not None:
            if id(x) in self.renames:
                chain.append(self.renames[id(x)])
            x = x.parent
        for m in chain:
            for a in m:
                out.setdefault(a, self.rename(g, a))
        return out

    @staticmethod
    def control(item):
        return dictof(item.data.get('Control'))

    def reacts(self, item):
        """ShouldAssignParametersToMami: a toggle or button with a reactive component of its own"""
        if inum(self.control(item).get('type')) not in (101, 102):
            return False
        for g in subtree(item.go):
            if any(ma_kind(c) in MA_REACTIVE for c in g.comps):
                x = g
                while x is not None and not any(ma_kind(c) == 'MenuItem' for c in x.comps):
                    x = x.parent
                if x is item.go:
                    return True
        return False

    def assign(self):
        """ParameterAssignerPass: every menu item's parameter and value"""
        declared = dict(self.declared)
        declared.update(self.an.eparams)
        by_param = {}
        for it in self.comps.get('MenuItem', []):
            pn = _str(dictof(self.control(it).get('parameter')).get('name'))
            if not pn.strip():
                if not self.reacts(it):
                    continue
                pn = '__MA/AutoParam/' + go_name(it.go)
            else:
                pn = self.rename(it.go, pn)
            by_param.setdefault(pn, []).append(it)
        for pn, items in by_param.items():
            auto = {id(m): truthy(m.data.get('automaticValue', '0')) for m in items}
            dflt, p = None, declared.get(pn)
            if p is not None and abs(p[1]) > 1e-6:
                dflt = int(p[1])
            else:
                f = next((num(self.control(m).get('value'), 1.0) for m in items
                          if truthy(m.data.get('isDefault', '0')) and not auto[id(m)]), None)
                if f is not None:
                    dflt = int(f)
                if len(items) == 1 and truthy(items[0].data.get('isDefault', '0')) and auto[id(items[0])]:
                    dflt = 1
            used = {dflt} if dflt is not None else set()
            is_default = {}
            for m in items:
                if auto[id(m)]:
                    is_default[id(m)] = truthy(m.data.get('isDefault', '0'))
                else:
                    v = int(num(self.control(m).get('value'), 1.0))
                    used.add(v)
                    is_default[id(m)] = dflt is not None and v == dflt
            if dflt is None:
                dflt = next(i for i in range(257) if i not in used)
                used.add(dflt)
            nxt, vtype = 1, 2
            for m in items:
                v = num(self.control(m).get('value'), 1.0)
                if auto[id(m)]:
                    if is_default[id(m)]:
                        v = float(dflt)
                    elif p is not None and p[0] != 0:
                        v = 1.0
                    else:
                        while nxt in used and nxt < 256:
                            nxt += 1
                        if nxt >= 256:
                            warn('the menu items of %s need more than 255 values' % pn)
                            break
                        v = float(nxt)
                        used.add(nxt)
                t = 0 if v > 1 else 2
                if v < 0 or abs(v - round(v)) > 0.01:
                    t = 1
                if vtype == 2 or t == 1:
                    vtype = t
                self.item[id(m)] = (pn, v)
            if pn not in declared:
                self.declared[pn] = (vtype, float(dflt))

    # ---- the menu

    @staticmethod
    def source(g):
        """the menu item, group or install target on a GameObject (its first MenuSource)"""
        return next((c for c in g.comps if ma_kind(c) in ('MenuItem', 'MenuGroup', 'MenuInstallTarget')), None)

    def controls(self):
        """[(path of menu names, control)] MA adds to the avatar's menu (VirtualMenu)"""
        out, av = [], self.av
        by_target = {}
        # an installer a Menu Install Target names puts its menu there instead, and only there (even when the install
        # target is off)
        targeted = {id(t.data['installer']) for t in self.comps.get('MenuInstallTarget', [])
                    if isinstance(t.data.get('installer'), Obj)}
        for c in self.comps.get('MenuInstaller', []):
            if not truthy(c.data.get('m_Enabled', '1')) or id(c) in targeted:
                continue
            if self.source(c.go) is None and not ref(c.data.get('menuToAppend'))[1]:
                continue
            by_target.setdefault(ref(c.data.get('installTargetMenu'))[1], []).append(c)
        if not by_target:
            return out
        busy = set()

        def vrc(guid, prefix, emit):
            """a menu asset's controls, then what installers put in it"""
            if not guid or guid in busy or len(prefix) > 12:
                return
            uf = self.db.yaml(guid)
            fid = uf.main(114) if uf is not None and not uf.binary else None
            if fid is None:
                return
            busy.add(guid)
            for c in listof(uf.get(fid)[1].get('controls')):
                c = dictof(c)
                if inum(c.get('type')) == 103:
                    vrc(ref(c.get('subMenu'))[1], prefix + (rich_text(str(c.get('name') or '')),), emit)
                elif emit:
                    out.append((prefix, c))
            for i in by_target.get(guid, []):
                installer(i, prefix)
            busy.discard(guid)

        def installer(i, prefix):
            src = self.source(i.go)
            if src is not None:
                node(src, prefix)
            else:
                vrc(ref(i.data.get('menuToAppend'))[1], prefix, True)

        def node(src, prefix):
            if id(src) in busy:
                return
            busy.add(id(src))
            d = src.data
            if ma_kind(src) == 'MenuInstallTarget':  # the installer's menu, here
                i = d.get('installer')
                if isinstance(i, Obj) and ma_kind(i) == 'MenuInstaller' and i.go is not None and \
                        id(i.go) in av.inside and id(i) not in busy:
                    busy.add(id(i))
                    installer(i, prefix)
                    busy.discard(id(i))
            elif ma_kind(src) == 'MenuGroup':
                t = d.get('targetObject')
                under(t if isinstance(t, Obj) and id(t) in av.inside else src.go, prefix)
            else:
                ctl = self.control(src)
                name = rich_text(_str(d.get('label')) or go_name(src.go))
                t = inum(ctl.get('type'))
                if t == 103:
                    if inum(d.get('MenuSource')) == 0:
                        vrc(ref(ctl.get('subMenu'))[1], prefix + (name,), True)
                    else:
                        o = d.get('menuSource_otherObjectChildren')
                        under(o if isinstance(o, Obj) and id(o) in av.inside else src.go, prefix + (name,))
                else:
                    pn, v = self.item.get(id(src), ('', num(ctl.get('value'), 1.0)))
                    e = {'name': name, 'type': t, 'parameter': {'name': pn}, 'value': v}
                    if t in (201, 202, 203):  # puppets: the floats they drive
                        e['subParameters'] = [{'name': self.rename(src.go, _str(dictof(s).get('name')))}
                                              for s in listof(ctl.get('subParameters'))]
                    out.append((prefix, e))
            busy.discard(id(src))

        def under(g, prefix):
            for c in g.children:
                src = self.source(c) if id(c) in av.inside else None
                if src is not None:
                    node(src, prefix)
        d = av.desc
        vrc(ref(d.get('expressionsMenu'))[1] if truthy(d.get('customExpressions', '0')) else None, (), False)
        for i in by_target.get(None, []):
            installer(i, ())
        return out

    # ---- reactive components

    def reactions(self):
        """{property: [(value, conditions, inverted)]}: the last rule in effect sets it (ReactiveObjectAnalyzer)"""
        props = {}
        for c in self.comps.get('ShapeChanger', []):
            for e in listof(c.data.get('m_shapes', c.data.get('Shapes'))):
                e = dictof(e)
                g = ma_objref(self.av, e.get('Object'))
                smr = next((x for x in g.comps if x.cls == 137), None) if g is not None else None
                shape = _str(e.get('ShapeName'))
                if smr is None or shape not in self.av.shape_names(smr):
                    continue
                if inum(e.get('ChangeType')) == 0:  # Delete: the triangles the shape key moves go (a vertex filter)
                    key = ('shape', shape)
                    cut = self.cutters.setdefault((id(smr), key), {
                        'r': smr, 'multi': 0, 'filters': [{'kind': 'shape', 'shapes': [shape], 'mode': 0,
                                                           'threshold': num(c.data.get('m_threshold'), 0.01)}],
                        'labels': []})
                    f = cut['filters'][0]
                    f['threshold'] = min(f['threshold'], num(c.data.get('m_threshold'), 0.01))
                    cut['labels'].append('%s: %s' % (go_name(c.go), shape))
                    props.setdefault(('c', smr, key), []).append((1.0, self.conditions(c), truthy(
                        c.data.get('m_inverted', '0'))))
                    continue
                props.setdefault(('s', smr, shape), []).append(
                    (num(e.get('Value')), self.conditions(c), truthy(c.data.get('m_inverted', '0'))))
        # Mesh Cutters: the triangles their vertex filters pick go while they are in effect
        for c in self.comps.get('MeshCutter', []):
            g = ma_objref(self.av, c.data.get('m_object'))
            smr = next((x for x in g.comps if x.cls == 137), None) if g is not None else None
            filters = [f for f in (vertex_filter(self.av, x) for x in c.go.comps) if f is not None]
            if smr is None or not filters:
                continue
            multi = inum(c.data.get('m_multiMode'), 1) if len(filters) > 1 else 0
            key = ('cutter', multi, tuple(f['key'] for f in filters))
            cut = self.cutters.setdefault((id(smr), key), {'r': smr, 'multi': multi, 'filters': filters, 'labels': []})
            cut['labels'].append(go_name(c.go))
            props.setdefault(('c', smr, key), []).append((1.0, self.conditions(c, smr.go), truthy(
                c.data.get('m_inverted', '0'))))
        for c in self.comps.get('ObjectToggle', []):
            for e in listof(c.data.get('m_objects')):
                e = dictof(e)
                g = ma_objref(self.av, e.get('Object'))
                if g is not None:
                    props.setdefault(('a', g), []).append((1.0 if truthy(e.get('Active', '0')) else 0.0,
                                                           self.conditions(c), truthy(c.data.get('m_inverted', '0'))))
        # the material changers: a material put in a renderer's slot, or one material swapped for another in every
        # renderer (under a root, if it has one)
        for c in self.comps.get('MaterialSetter', []):
            for e in listof(c.data.get('m_objects')):
                e = dictof(e)
                g = ma_objref(self.av, e.get('Object'))
                r = next((x for x in g.comps if x.cls in RENDERERS), None) if g is not None else None
                k = inum(e.get('MaterialIndex'), -1)
                if r is not None and 0 <= k < len(listof(r.data.get('m_Materials'))):
                    props.setdefault(('m', r, k), []).append((matkey(e.get('Material')), self.conditions(c),
                                                              truthy(c.data.get('m_inverted', '0'))))
        for c in self.comps.get('MaterialSwap', []):
            root = ma_objref(self.av, c.data.get('m_root'))
            for e in listof(c.data.get('m_swaps')):
                e = dictof(e)
                frm = matkey(e.get('From'))
                if frm is None:
                    continue
                for r in self.av.renderers:
                    if root is not None and not any(g is root for g in go_chain(r.go)):
                        continue
                    for k in range(len(listof(r.data.get('m_Materials')))):
                        if self.av.default(('m', r, k)) == frm:  # (a model's own material: the .mat that replaces it)
                            props.setdefault(('m', r, k), []).append((matkey(e.get('To')), self.conditions(c),
                                                                      truthy(c.data.get('m_inverted', '0'))))
        return props

    def conditions(self, comp, affected=None):
        """BuildConditions: the first menu item from the component up is on, and every object from it up is active
        (but the affected object and the ones above it: a rule for a hidden object does not matter)"""
        out, item = [], False
        g = comp.go
        above = set(map(id, go_chain(affected))) if affected is not None else set()
        while g is not None and g is not self.av.root:
            if not item:
                mi = next((c for c in g.comps if ma_kind(c) == 'MenuItem'), None)
                if mi is not None:
                    item = True
                    pv = self.item.get(id(mi))
                    if pv is not None:
                        out.append(('p',) + pv)
            if id(g) not in above:
                out.append(('a', g))
            g = g.parent
        return out

    def blendshape_syncs(self):
        """Blendshape Sync: [(the reference mesh, its shape key, the component's mesh, its shape key, remap points)]"""
        out = []
        for c in self.comps.get('BlendshapeSync', []):
            me = next((x for x in c.go.comps if x.cls == 137), None)
            if me is None:
                continue
            for b in listof(c.data.get('Bindings')):
                b = dictof(b)
                g = ma_objref(self.av, b.get('ReferenceMesh'))
                src = next((x for x in g.comps if x.cls == 137), None) if g is not None else None
                shape = _str(b.get('Blendshape'))
                local = _str(b.get('LocalBlendshape')) if _str(b.get('LocalBlendshape')).strip() else shape
                if src is None or shape not in self.av.shape_names(src) or local not in self.av.shape_names(me):
                    continue
                pts = None
                if truthy(b.get('RemapCurveIsValid', '0')):
                    pts = sorted((num(k.get('time')), num(k.get('value'))) for k in
                                 listof(dictof(b.get('RemapCurve')).get('m_Curve')) if isinstance(k, dict))
                    if len(pts) < 2 or pts == [(0.0, 0.0), (100.0, 100.0)]:
                        pts = None
                out.append((src, shape, me, local, pts))
        return out

    def apply(self, params, vals):
        """what the reactive components set, over the animators' values; then what Blendshape Sync copies"""
        vals = self.react(params, vals)
        if self.syncs:
            vals = dict(vals)
            follow_shapes(vals, self.syncs)
        return vals

    def react(self, params, vals):
        if not self.rules:
            return vals
        got = {}

        def on(c):
            if c[0] == 'p':
                return abs(params.get(c[1], 0.0) - c[2]) < 0.005
            v = got.get(c)
            if v is None:
                v = vals.get(c)
            if v is None:
                v = self.av.default(c)
            return v >= 0.5
        for _ in range(8):  # objects whose showing turns other rules on or off
            changed = False
            for key, rules in self.rules.items():
                v = None
                for value, conds, inverted in rules:
                    if all(on(c) for c in conds) != inverted:
                        v = value
                if got.get(key) != v:
                    changed = True
                    if v is None:
                        del got[key]
                    else:
                        got[key] = v
            if not changed:
                break
        if not got:
            return vals
        vals = dict(vals)
        vals.update(got)
        return vals

    # ---- animators merged into FX

    def animators(self):
        """[(priority, Controller, {its parameter names: the avatar's})] of Merge Animator FX layers; a Replace one
        first (it takes the FX controller's place)"""
        out = []
        for c in self.comps.get('MergeAnimator', []):
            d = c.data
            if inum(d.get('layerType'), 5) != 5:
                continue
            g = ref(d.get('animator'))[1]
            if not g:
                continue
            if self.db.get(g) is None:
                warn('%s: the animator its Merge Animator merges is not in the input' % go_name(c.go))
                continue
            base = ''
            if inum(d.get('pathMode')) == 0:
                base = av_path(self.av, ma_objref(self.av, d.get('relativePathRoot')) or c.go)
            ctl = Controller(Anim(self.db, self.av, base), g)
            if ctl.guid is None:
                warn('%s: the animator its Merge Animator merges cannot be read' % go_name(c.go))
                continue
            replace = inum(d.get('mergeAnimatorMode')) == 1
            out.append((-1 << 30 if replace else inum(d.get('layerPriority')), len(out), ctl, self.view(c.go), replace))
        # Merge Motion (Blend Tree): the motions, in one layer before everything else in FX (MergeBlendTreePass)
        parts = []
        for c in self.comps.get('MergeBlendTree', []):
            d = c.data
            p = ptr(d.get('BlendTree'), None)
            if p is None or not p[0]:
                continue
            if self.db.get(p[0]) is None:
                warn('%s: the motion its Merge Motion merges is not in the input' % go_name(c.go))
                continue
            base = ''
            if inum(d.get('PathMode')) == 0:
                base = av_path(self.av, ma_objref(self.av, d.get('RelativePathRoot')) or c.go)
            anim = Anim(self.db, self.av, base)
            if anim.cls(p) not in (74, 206):
                warn('%s: the motion its Merge Motion merges cannot be read' % go_name(c.go))
                continue
            parts.append((anim, p, self.view(c.go)))
        if parts:
            out.append((-1 << 31, len(out), MotionLayer(parts), {}, False))
        out.sort(key=lambda x: (x[0], x[1]))
        return out


# ---------------------------------------------------------------- what the avatar does: toggles, faces, visemes

def tf_same(a, b, rest):
    """do two sets of a Transform's components put it in the same place (within 0.1 mm, 0.01 degrees, 0.01%)"""
    if rest is None:
        rest = {'p': (0.0, 0.0, 0.0), 'q': (0.0, 0.0, 0.0, 1.0), 'e': (0.0, 0.0, 0.0), 's': (1.0, 1.0, 1.0)}
    A, B = tf_local(a, rest), tf_local(b, rest)
    ta, qa, sa = A.decompose()
    tb, qb, sb = B.decompose()
    return (ta - tb).length < 1e-4 and abs(qa.dot(qb)) > math.cos(math.radians(0.01) / 2) and (sa - sb).length < 1e-4


def cuts_in(vals):
    """the Mesh Cutters' cuts (and Shape Changers' deletes) in effect with these values"""
    return frozenset(p for p, v in vals.items() if p[0] == 'c' and v is not None and v >= 0.5)


def norm_name(s):
    """the plugin's loose name compare: "vrc.v_aa" and "V_AA" are both "vaa\""""
    dot = s.rfind('.')
    if dot >= 0 and len(s) - dot > 2 and not s[dot + 1:].isdigit():
        s = s[dot + 1:]
    return ''.join((chr(ord(c) + 32) if 'A' <= c <= 'Z' else c) for c in s if c not in '_-. ')


def _tokens(s):
    s = re.sub(r'([a-z])([A-Z])', r'\1 \2', s)
    return [t for t in re.split(r'[^0-9a-zA-Z\u0080-\uffff]+', s.lower()) if t]


PRESET_WORDS = [('happy', ('happy', 'joy', 'smile', 'laugh', 'grin', 'egao', '笑')),
                ('angry', ('angry', 'anger', 'mad', 'rage', 'ikari', '怒')),
                ('sad', ('sad', 'sorrow', 'cry', 'crying', 'tear', 'tears', 'kanashi', '悲', '泣')),
                ('relaxed', ('relax', 'calm', 'fun', 'content', 'nagomi', '和')),
                ('surprised', ('surprise', 'shock', 'astonish', 'odoroki', '驚'))]


def preset_of(name):
    toks = _tokens(name)
    for p, words in PRESET_WORDS:
        for w in words:
            if ord(w[0]) >= 0x80:
                if w in name:
                    return p
            elif any(t == w or (len(w) >= 5 and t.startswith(w)) for t in toks):
                return p
    return None


def face_name(clip):
    """a gesture face's name from its clip's: "Smile_L On" is "Smile\""""
    s = clip.strip()
    while True:
        t = re.sub(r'(?:[ _.\-]+(?i:left|right|on|anim|fx|gesture|[lr]|[0-9]+)'
                   r'|(?<=[a-z0-9])(?:Left|Right|On|Anim|FX|Fx|L|R))$', '', s).strip(' _.-')
        if t == s or not t:
            break
        s = t
    return s


def humanoid_map(db, root, gos, quiet=False):
    """{Unity human bone name: GameObject} from the humanoid rig of root's Animator, among gos (the ones of the rig's
    own model first); quiet: {} for anything but a humanoid, without a word"""
    say = (lambda m: None) if quiet else warn
    an = next((c for c in root.comps if c.cls == 95), None)
    if an is None:
        say('the avatar has no Animator, so no humanoid rig')
        return {}
    g = ref(an.data.get('m_Avatar'))[1]
    a = db.get(g)
    if a is None:
        say('the avatar\'s humanoid rig is not in the input')
        return {}
    if a.ext not in MODEL_EXT:
        say('the avatar\'s humanoid rig comes from %s, which this tool does not read' % a.path)
        return {}
    imp = db.importer(g)
    if inum(imp.get('animationType'), 2) != 3:
        if quiet:
            return {}
        warn('%s is not imported as a humanoid' % a.path)
    human = listof(dictof(imp.get('humanDescription')).get('human'))
    by_name = {}
    for go in gos:
        n = str(go.data.get('m_Name', ''))
        mine = go.fbx is not None and go.fbx.guid == g
        if n not in by_name or (mine and not by_name[n][1]):
            by_name[n] = (go, mine)
    out = {}
    for h in human:
        h = dictof(h)
        hit = by_name.get(str(h.get('boneName') or ''))
        if hit and h.get('humanName'):
            out[str(h['humanName'])] = hit[0]
    if not out:
        say('%s has no humanoid bone map' % a.path)
    return out


class Analysis:
    """the avatar's descriptor, FX controller and menu, read into what hypr3d has"""

    def __init__(self, db, av):
        self.db, self.av = db, av
        self.anim = Anim(db, av)
        d = av.desc
        self.fx = None
        if truthy(d.get('customizeAnimationLayers', '0')):
            for l in listof(d.get('baseAnimationLayers')):
                l = dictof(l)
                if inum(l.get('type')) != 5 or truthy(l.get('isDefault', '0')):
                    continue
                g = ref(l.get('animatorController'))[1]
                if g and db.get(g):
                    self.fx = Controller(self.anim, g)
                    if self.fx.guid is None:
                        warn('the FX controller %s cannot be read' % db.get(g).path)
                        self.fx = None
                elif g:
                    warn('the FX controller is not in the input: toggles and gesture faces are not converted')
        custom = truthy(d.get('customExpressions', '0'))
        self.eparams = read_params(db, ref(d.get('expressionParameters'))[1]) if custom else {}
        self.menu = read_menu(db, ref(d.get('expressionsMenu'))[1], []) if custom else []
        comps = {}
        for g in av.gos:
            for c in g.comps:
                k = ma_kind(c)
                if k in MA_MENU:
                    comps.setdefault(k, []).append(c)
        self.mat = MAToggles(self, comps) if comps else None
        self.merged = []  # [(priority, order, Controller, {its parameter names: the avatar's}, replaces FX)]
        if self.mat:
            for k, v in self.mat.declared.items():
                self.eparams.setdefault(k, v)
            self.menu += self.mat.controls()
            self.merged = self.mat.merged
            if any(x[4] for x in self.merged):
                self.fx = None
        vcomps = [c for g in av.gos for c in g.comps if is_vrcfury(c)]
        self.vrcf = VRCFury(self, vcomps) if vcomps else None  # after MA: VRCFury builds after it
        if self.vrcf:
            for k, v in self.vrcf.declared.items():
                self.eparams.setdefault(k, v)
            self.menu += self.vrcf.menu
            self.menu = self.vrcf.menu_moved(self.menu)
            self.merged = list(self.merged) + self.vrcf.merged
        self.params = {}
        if self.fx:
            self.params.update({k: v for k, (t, v) in self.fx.params.items()})
        for x in self.merged:
            for k, (t, v) in x[2].params.items():
                self.params.setdefault(x[3].get(k, k), v)
        self.params.update({k: v for k, (t, v) in self.eparams.items()})
        self.params.update({k: float(v) for k, v in BUILTIN_PARAMS.items()})
        self.skipped = list(self.vrcf.skipped) if self.vrcf else []  # menu controls not converted: (name, why)
        self.sliders = []  # radial puppets, as toggles() finds them
        self.kept = []  # the toggles, as toggles() finds them
        self.cuts0 = frozenset()  # the Mesh Cutters' cuts in effect at rest (toggles() finds them)
        self.pieces = []  # [(renderer, {cuts}, part name)]: what cut_meshes() made parts of
        self.emote_params = set()  # the parameters that start the Action layers' emotes
        self.ma = None  # the ModularAvatar once it has run

    def evaluate(self, params):
        runs = [(x[2], x[3]) for x in self.merged if x[0] < 0] + ([(self.fx, {})] if self.fx else []) + [
            (x[2], x[3]) for x in self.merged if x[0] >= 0]
        if not runs and self.mat is None and self.vrcf is None:
            return {}, {}, {}, params
        if len(runs) == 1 and not runs[0][1] and self.mat is None and self.vrcf is None and not (
                self.ma is not None and self.ma.replaced):
            return runs[0][0].evaluate(params)
        vals, names, track, out = {}, {}, {}, dict(params)
        if self.vrcf:  # the FX floats VRCFury's toggles set, for the animators to read
            out = self.vrcf.drive(out)
        for ctl, view in runs:  # the layers MA merges come after FX's (or before, at a priority below 0)
            local = dict(out)
            for a, b in view.items():
                if b in out:
                    local[a] = out[b]
            v, n, t, got = ctl.evaluate(local)
            vals.update(v)
            names.update(n)
            track.update(t)
            for k, x in got.items():
                out[view.get(k, k)] = x
        if self.mat:
            vals = self.mat.apply(out, vals)
        if self.ma is not None and self.ma.replaced:  # what animated a replaced object animates its replacement
            vals = {q: v for q, v in ((self.ma.swap(p), v) for p, v in vals.items()) if q is not None}
            names = {q: v for q, v in ((self.ma.swap(p), v) for p, v in names.items()) if q is not None}
        if self.vrcf:  # VRCFury's toggle layers come after everything MA made
            vals = self.vrcf.apply(out, vals, names, track)
        return vals, names, track, out

    def animator_param(self, pn):
        """(type, default) of a parameter the animators have"""
        if self.fx and pn in self.fx.params:
            return self.fx.params[pn]
        for x in self.merged:
            for k, v in x[2].params.items():
                if x[3].get(k, k) == pn:
                    return v
        return None

    def shape_diff(self, vals, base, positive=False):
        av = self.av
        out, neg = {}, False
        for pr in set(vals) | set(base):
            if pr[0] != 's':
                continue
            v1 = vals[pr] if pr in vals else av.default(pr)
            v0 = base[pr] if pr in base else av.default(pr)
            if positive:
                if v1 - v0 > 0.5:
                    out[(pr[1], pr[2])] = min((v1 - v0) / 100.0, 1.0)
                elif v1 - v0 < -0.5:
                    neg = True
            elif abs(v1 - v0) > 0.5:
                out[(pr[1], pr[2])] = min(max(v1 / 100.0, 0.0), 1.0)
        return out, neg

    def transform_diff(self, vals, base):
        """{GameObject: {component: value}}: the Transforms these values put elsewhere than base does, each with every
        component the values animate ('px', 'qw', 'ey', 'sz', 'm': a scale multiplier, see Avatar.prop)"""
        av = self.av
        moved = {}
        for pr in set(vals) | set(base):
            if pr[0] != 't':
                continue
            g = pr[1]
            if id(g) in moved:
                continue
            a = {q[2]: (vals[q] if q in vals else av.default(q)) for q in set(vals) | set(base) if q[0] == 't' and
                 q[1] is g}
            b = {q[2]: (base[q] if q in base else av.default(q)) for q in set(vals) | set(base) if q[0] == 't' and
                 q[1] is g}
            if not tf_same(a, b, av.tf_rest.get(id(g))):
                moved[id(g)] = (g, a)
        return {g: a for g, a in moved.values()}

    def material_state(self, vals, r, k):
        """what a renderer's slot k shows with these values: (material key, ((property, value), ...)) with the
        properties hypr3d carries that differ from the material's own"""
        pr = ('m', r, k)
        mk = vals[pr] if pr in vals else self.av.default(pr)
        over = []
        for p, v in vals.items():
            if p[0] == 'mp' and p[1] is r and v is not None:
                own = self.av.matprop(r, k, p[2], mk)
                if own is not None and abs(v - own) > 1e-5:
                    over.append((p[2], round(v, 5)))
        return mk, tuple(sorted(over))

    def material_diff(self, vals, base):
        """{(renderer, slot): material state} where these values show another material than `base`"""
        out = {}
        rs = {id(p[1]): p[1] for p in list(vals) + list(base) if p[0] in ('m', 'mp')}
        for r in rs.values():
            for k in range(len(listof(r.data.get('m_Materials')))):
                s = self.material_state(vals, r, k)
                if s != self.material_state(base, r, k):
                    out[(r, k)] = s
        return out

    def toggles(self):
        """(toggles, the values the avatar rests at with every toggle off)"""
        av = self.av
        ctrls, radial, puppets = [], [], []
        for prefix, c in self.menu:
            t = inum(c.get('type'))
            name = rich_text(str(c.get('name') or ''))
            pn = str(dictof(c.get('parameter')).get('name') or '')
            if t == 203:  # a radial puppet: a slider of the float its first sub parameter is
                sp = _str(dictof((listof(c.get('subParameters')) or [None])[0]).get('name'))
                if sp:
                    radial.append((prefix, name or sp, sp))
                else:
                    self.skipped.append((name, 'a radial puppet with no parameter'))
                continue
            if t in (201, 202):  # a two-axis puppet (x, y) or a four-axis one (up, right, down, left): a 2D slider
                sps = [_str(dictof(x).get('name')) for x in listof(c.get('subParameters'))][:2 if t == 201 else 4]
                if any(sps):
                    puppets.append((prefix, name or next(x for x in sps if x), sps, t))
                else:
                    self.skipped.append((name, 'a puppet with no parameters'))
                continue
            if t not in (101, 102):
                self.skipped.append((name, 'not a toggle'))
                continue
            if not pn:
                continue
            if pn in self.emote_params:
                self.skipped.append((name, 'an emote: in the emotes'))
                continue
            grps = tuple(_str(x) for x in listof(c.get('groups')) if _str(x)) or (
                (_str(c.get('group')),) if _str(c.get('group')) else ())
            ctrls.append((prefix, name or pn, pn, num(c.get('value'), 1.0), t, grps))
        values = {}
        for _, _, pn, v, _, _ in ctrls:
            values.setdefault(pn, set()).add(v)
        base_p = dict(self.params)
        for pn in values:
            base_p[pn] = 0.0
        base = self.evaluate(base_p)[0]
        vis0 = {id(r): av.visible(r, base) for r in av.renderers}
        self.cuts0 = cut0 = cuts_in(base)
        out, used = [], set()
        for prefix, name, pn, v, t, grp in ctrls:
            p = dict(base_p)
            p[pn] = v
            vals = self.evaluate(p)[0]
            show = [r for r in av.renderers if av.visible(r, vals) and not vis0[id(r)]]
            hide = [r for r in av.renderers if not av.visible(r, vals) and vis0[id(r)]]
            shapes = self.shape_diff(vals, base)[0]
            mats = self.material_diff(vals, base)
            cuts = cuts_in(vals)
            tfs = self.transform_diff(vals, base)
            loop = self.vrcf.loops.get(pn) if self.vrcf is not None else None
            drop = self.vrcf.drops.get(pn, []) if self.vrcf is not None else []
            ptype = self.eparams.get(pn, (None, 0))[0]
            if ptype is None and self.animator_param(pn):
                ptype = {1: 1, 3: 0, 4: 2}.get(self.animator_param(pn)[0], 2)
            group = grp[0] if grp else (pn if (len(values[pn]) > 1 or ptype in (0, 1)) else '')
            on = t == 102 and abs(self.params.get(pn, 0.0) - v) < 1e-4
            nm = name
            if norm_name(nm) in used and prefix:
                nm = '%s %s' % (prefix[-1], name)
            k, stem = 2, nm
            while norm_name(nm) in used or not norm_name(nm):
                nm = '%s %d' % (stem, k)
                k += 1
            used.add(norm_name(nm))
            out.append({'name': nm, 'group': group, 'groups': list(grp[1:]) and list(grp), 'on': on, 'show': show,
                        'hide': hide, 'shapes': shapes, 'materials': mats, 'cuts': cuts, 'transforms': tfs,
                        'loop': loop, 'drop': drop, 'param': pn, 'value': v})
        # one that changes nothing is kept only as the "none of these" choice of a group
        does = lambda x: x['show'] or x['hide'] or x['shapes'] or x['materials'] or x['cuts'] != cut0 or \
            x['transforms'] or x['loop'] or x['drop']
        busy = {g for x in out if does(x) for g in (x['groups'] or [x['group']])}
        kept = []
        for x in out:
            if does(x) or any(
                    g and g in busy for g in (x['groups'] or [x['group']])):
                kept.append(x)
            else:
                self.skipped.append((x['name'], 'it changes nothing this tool carries over'))
        self.sliders = []
        seen = set()
        for prefix, name, pn in radial:
            if pn in seen:
                continue
            seen.add(pn)
            s = self.slider(name, pn, base_p, base, vis0)
            if s is None:
                self.skipped.append((name, 'a radial puppet that changes nothing this tool carries over'))
                continue
            nm = s['name']
            if norm_name(nm) in used and prefix:
                nm = '%s %s' % (prefix[-1], nm)
            k, stem = 2, nm
            while norm_name(nm) in used or not norm_name(nm):
                nm = '%s %d' % (stem, k)
                k += 1
            used.add(norm_name(nm))
            s['name'] = nm
            self.sliders.append(s)
        for prefix, name, sps, t in puppets:
            if tuple(sps) in seen:
                continue
            seen.add(tuple(sps))
            s = self.slider2(name, sps, t, base_p, base, vis0)
            if s is None:
                self.skipped.append((name, 'a puppet that changes nothing this tool carries over'))
                continue
            nm = s['name']
            if norm_name(nm) in used and prefix:
                nm = '%s %s' % (prefix[-1], nm)
            k, stem = 2, nm
            while norm_name(nm) in used or not norm_name(nm):
                nm = '%s %d' % (stem, k)
                k += 1
            used.add(norm_name(nm))
            s['name'] = nm
            self.sliders.append(s)
        self.kept = kept
        return kept, base

    GRID = 9  # a 2D slider's keys: this many across and up, over -1..1

    def slider2(self, name, sps, kind, base_p, base, vis0):
        """a puppet's two axes as a 2D slider: {'name', 'axes': 2, 'value': (x, y) where it starts, 'grid': n,
        'keys': [{'at': (x, y), 'shapes', 'show', 'hide', 'materials', 'cuts', 'transforms'}]} at n x n points over
        -1..1, row by row from the bottom; a four-axis puppet's parameters are how far up, right, down and left"""
        av, n = self.av, self.GRID

        def params(x, y):
            p = dict(base_p)
            if kind == 201:
                for pn, v in zip(sps, (x, y)):
                    if pn:
                        p[pn] = v
            else:
                for pn, v in zip(sps, (max(y, 0.0), max(x, 0.0), max(-y, 0.0), max(-x, 0.0))):
                    if pn:
                        p[pn] = v
            return p
        keys = []
        for j in range(n):
            for i in range(n):
                x, y = -1 + 2.0 * i / (n - 1), -1 + 2.0 * j / (n - 1)
                vals = self.evaluate(params(x, y))[0]
                keys.append({'at': (x, y), 'vals': vals, 'materials': self.material_diff(vals, base),
                             'show': [r for r in av.renderers if av.visible(r, vals) and not vis0[id(r)]],
                             'hide': [r for r in av.renderers if not av.visible(r, vals) and vis0[id(r)]],
                             'cuts': cuts_in(vals)})
        props = sorted({pr for k in keys for pr in k['vals'] if pr[0] == 's'}, key=lambda p: (str(p[1].gname), p[2]))
        for k in keys:
            k['shapes'] = {(pr[1], pr[2]): min(max((k['vals'][pr] if pr in k['vals'] else av.default(pr)) / 100.0, 0.0),
                                                1.0) for pr in props}
        for sh in [sh for sh in keys[0]['shapes'] if all(abs(k['shapes'][sh] - keys[0]['shapes'][sh]) < 1e-3
                                                          for k in keys)]:
            for k in keys:
                del k['shapes'][sh]
        moved = {}
        for k in keys:
            for g in self.transform_diff(k['vals'], keys[0]['vals']):
                moved[id(g)] = g
        allp = {x for y in keys for x in y['vals']}
        for k in keys:
            k['transforms'] = {g: {q[2]: (k['vals'][q] if q in k['vals'] else av.default(q)) for q in allp
                                   if q[0] == 't' and q[1] is g} for g in moved.values()}
        state = lambda k: ([id(r) for r in k['show']], [id(r) for r in k['hide']],
                           sorted(((id(r), s), v) for (r, s), v in k['materials'].items()), k['cuts'])
        if not any(k['shapes'] for k in keys) and not moved and all(state(k) == state(keys[0]) for k in keys) and not (
                keys[0]['show'] or keys[0]['hide'] or keys[0]['materials']):
            return None
        for k in keys:
            k.pop('vals')
        if kind == 201:
            at = tuple(min(max(self.params.get(pn, 0.0), -1.0), 1.0) if pn else 0.0 for pn in sps + [''] * (2 - len(sps)))
        else:
            u, r, d, l = (self.params.get(pn, 0.0) if pn else 0.0 for pn in sps + [''] * (4 - len(sps)))
            at = (min(max(r - l, -1.0), 1.0), min(max(u - d, -1.0), 1.0))
        return {'name': name, 'axes': 2, 'params': sps, 'value': at, 'grid': n, 'keys': keys}

    def slider(self, name, pn, base_p, base, vis0):
        """a radial puppet's float, 0 to 1, as keys: {'name', 'value' (where it starts), 'keys': [{'at', 'shapes',
        'show', 'hide', 'materials'}]}; shape keys go in straight lines from key to key, and the rest is as the last
        key at or below the value has it"""
        av = self.av

        def at(x):
            p = dict(base_p)
            p[pn] = x
            vals = self.evaluate(p)[0]
            return {'at': x,
                    'show': [r for r in av.renderers if av.visible(r, vals) and not vis0[id(r)]],
                    'hide': [r for r in av.renderers if not av.visible(r, vals) and vis0[id(r)]],
                    'vals': vals, 'materials': self.material_diff(vals, base), 'cuts': cuts_in(vals)}

        def state(k):
            return ([id(r) for r in k['show']], [id(r) for r in k['hide']],
                    sorted(((id(r), s), v) for (r, s), v in k['materials'].items()), k['cuts'])
        n = 16
        keys = [at(i / n) for i in range(n + 1)]
        i = 1
        while i < len(keys):  # where what shows changes, to within 1/512
            a, b = keys[i - 1], keys[i]
            if state(a) != state(b) and b['at'] - a['at'] > 1 / 512:
                keys.insert(i, at((a['at'] + b['at']) / 2))
                continue
            i += 1
        props = sorted({pr for k in keys for pr in k['vals'] if pr[0] == 's'}, key=lambda p: (str(p[1].gname), p[2]))
        for k in keys:
            k['shapes'] = {}
            for pr in props:
                v = k['vals'][pr] if pr in k['vals'] else av.default(pr)
                k['shapes'][(pr[1], pr[2])] = min(max(v / 100.0, 0.0), 1.0)
        for s in [s for s in keys[0]['shapes'] if all(abs(k['shapes'][s] - keys[0]['shapes'][s]) < 1e-3 for k in keys)]:
            for k in keys:  # only what the slider moves
                del k['shapes'][s]
        # the Transforms it moves: at every key, where they are there
        moved = {}
        for k in keys:
            for g in self.transform_diff(k['vals'], keys[0]['vals']):
                moved[id(g)] = g
        for k in keys:
            k['transforms'] = {g: {q[2]: (k['vals'][q] if q in k['vals'] else av.default(q)) for q in
                                   set(k['vals']) | {x for y in keys for x in y['vals']} if q[0] == 't' and q[1] is g}
                               for g in moved.values()}
        base_tf = self.transform_diff(keys[0]['vals'], base)
        if not any(k['shapes'] != keys[0]['shapes'] or state(k) != state(keys[0]) for k in keys) and not moved and not (
                keys[0]['show'] or keys[0]['hide'] or keys[0]['materials'] or keys[0]['cuts'] != cuts_in(base) or base_tf):
            return None
        # keys the ones on each side of them make anyway go
        out = [keys[0]]
        for i in range(1, len(keys) - 1):
            a, k, b = out[-1], keys[i], keys[i + 1]
            f = (k['at'] - a['at']) / (b['at'] - a['at'])
            straight = all(abs(a['shapes'][s] + (b['shapes'][s] - a['shapes'][s]) * f - k['shapes'][s]) < 0.002
                           for s in k['shapes']) and all(tf_same(
                {c: a['transforms'][g][c] + (b['transforms'][g][c] - a['transforms'][g][c]) * f for c in k['transforms'][g]},
                k['transforms'][g], av.tf_rest.get(id(g))) for g in k['transforms'])
            if not (straight and state(k) == state(a)):
                out.append(k)
        out.append(keys[-1])
        for k in out:
            k.pop('vals')
        return {'name': name, 'param': pn, 'value': min(max(self.params.get(pn, 0.0), 0.0), 1.0), 'keys': out}

    def gestures(self):
        """(the gesture map, the expressions it uses); (None, []) when FX has no gesture faces"""
        if self.fx is None and not self.merged and self.vrcf is None:
            return None, []
        av = self.av
        base = self.evaluate(dict(self.params))[0]
        faces, exprs, neg = {}, {}, False

        def face(p, label):
            """the key of the face these parameters give (registered in exprs), None for none"""
            nonlocal neg
            vals, names, track, _ = self.evaluate(p)
            shapes, n = self.shape_diff(vals, base, positive=True)
            neg = neg or n
            if not shapes:
                return None
            src = {}
            for (smr, sh), w in shapes.items():
                c = names.get(('s', smr, sh))
                if c:
                    src[c] = src.get(c, 0.0) + w
            key = (tuple(sorted(((id(k[0]), k[1]), round(w, 3)) for k, w in shapes.items())),
                   track.get('eyes') == 2, track.get('mouth') == 2)
            if key not in exprs:
                # named as what sets most of it (a tie by name: the values' order follows sets of objects)
                exprs[key] = {'shapes': shapes, 'clip': min(src, key=lambda c: (-src[c], c)) if src else '',
                              'eyes': key[1], 'mouth': key[2], 'label': label}
            return key
        for side, P in (('left', 'GestureLeft'), ('right', 'GestureRight')):
            for g in range(1, 8):
                p = dict(self.params)
                p[P] = float(g)
                p[P + 'Weight'] = 1.0
                faces[(side, g)] = face(p, '%s %s' % (side, GESTURES[g]))
        # both hands at once, where the avatar says so (a transition that tests both hands' signs, a VRCFury Gesture
        # Driver's combo): a face neither hand's alone gives. Otherwise hypr3d shows the face of the hand that made its
        # sign last
        combos = {}
        for l, r in sorted(self.gesture_pairs()):
                p = dict(self.params)
                p.update({'GestureLeft': float(l), 'GestureRight': float(r), 'GestureLeftWeight': 1.0,
                          'GestureRightWeight': 1.0})
                key = face(p, '%s+%s' % (GESTURES[l], GESTURES[r]))
                if key != faces[('left', l)] and key != faces[('right', r)]:
                    combos[(l, r)] = key
        if not exprs:
            return None, []
        if neg:
            warn('some gesture faces turn shape keys down, which hypr3d does not do; only what they turn up is kept')
        shape_names = {norm_name(n) for r in av.renderers if r.cls == 137 for n in av.shape_names(r)}
        used, presets, out = set(), set(), []
        name_of = {}
        for key, e in exprs.items():
            nm = face_name(e['clip']) or e['label']
            single = len(e['shapes']) == 1 and abs(next(iter(e['shapes'].values())) - 1) < 1e-3
            if norm_name(nm) in shape_names and not single:
                nm += ' face'
            k, stem = 2, nm
            while norm_name(nm) in used:
                nm = '%s %d' % (stem, k)
                k += 1
            used.add(norm_name(nm))
            x = {'name': nm, 'shapes': e['shapes']}
            pr = preset_of(nm)
            if pr and pr not in presets:
                presets.add(pr)
                x['preset'] = pr
            if e['eyes']:
                x['blink'] = x['lookAt'] = 'block'
            if e['mouth']:
                x['mouth'] = 'block'
            out.append(x)
            name_of[key] = nm
        gmap = {}
        for g in range(1, 8):
            l = name_of.get(faces.get(('left', g)), 'none')
            r = name_of.get(faces.get(('right', g)), 'none')
            if l == r:
                gmap.setdefault('both', {})[GESTURES[g]] = l
            else:
                gmap.setdefault('left', {})[GESTURES[g]] = l
                gmap.setdefault('right', {})[GESTURES[g]] = r
        for (l, r), key in sorted(combos.items()):
            gmap.setdefault('combos', {})['%s+%s' % (GESTURES[l], GESTURES[r])] = name_of.get(key, 'none')
        return gmap, out

    def gesture_pairs(self):
        """{(left sign, right sign)} the avatar has a face for, with both hands: its controllers' transitions that test
        GestureLeft and GestureRight for signs at once, and VRCFury's Gesture Driver combos"""
        out = set()
        guids = ([self.fx.guid] if self.fx else []) + [x[2].guid for x in self.merged if isinstance(x[2], Controller)]
        for g in dict.fromkeys(x for x in guids if x):
            uf = self.db.yaml(g)
            if uf is None or uf.binary:
                continue
            for fid in uf.order:
                if uf.cls(fid) != 1101:
                    continue
                eq = {}
                for cd in listof(uf.get(fid)[1].get('m_Conditions')):
                    cd = dictof(cd)
                    if inum(cd.get('m_ConditionMode')) == 6:
                        eq[str(cd.get('m_ConditionEvent') or '')] = int(round(num(cd.get('m_EventTreshold'))))
                l, r = eq.get('GestureLeft'), eq.get('GestureRight')
                if l is not None and r is not None and 1 <= l <= 7 and 1 <= r <= 7:
                    out.add((l, r))
        for test, arg, _ in (self.vrcf.rules if self.vrcf else []):
            if test == 'gesture' and arg[0] == 3 and 1 <= arg[1] <= 7 and 1 <= arg[2] <= 7:
                out.add((arg[1], arg[2]))
        return out

    def hand_poses(self):
        """the Gesture layers' hand poses (the avatar's own, Modular Avatar's Merge Animators', VRCFury's Full
        Controllers'), with both hands making each sign: {(hand: 0 left, 1 right; sign): {MUSCLES index: value}} of
        each hand's fingers, where its layers move them"""
        db, av = self.db, self.av
        ctls = []  # (Controller, {its parameter names: the avatar's}, the humanoid parts the descriptor lets it move)
        d = av.desc
        if truthy(d.get('customizeAnimationLayers', '0')):
            for l in listof(d.get('baseAnimationLayers')):  # (VRChat's AnimLayerType: 3 is Gesture)
                l = dictof(l)
                g = ref(l.get('animatorController'))[1]
                if inum(l.get('type')) == 3 and not truthy(l.get('isDefault', '0')) and g and db.get(g):
                    ctls.append((Controller(Anim(db, av), g), {}, mask_parts(db, ptr(l.get('mask'), None))))
        for c in (self.mat.comps.get('MergeAnimator', []) if self.mat else []):
            dd = c.data
            g = ref(dd.get('animator'))[1]
            if inum(dd.get('layerType'), 5) == 3 and g and db.get(g):
                base = av_path(av, ma_objref(av, dd.get('relativePathRoot')) or c.go) if inum(dd.get('pathMode')) == 0 \
                    else ''
                ctls.append((Controller(Anim(db, av, base), g), self.mat.view(c.go), None))
        for c, f in (self.vrcf.feats if self.vrcf else []):
            if f['@class'] == 'FullController':
                root = self.vrcf.go(f.get('rootObjOverride')) or c.go
                for e in listof(f.get('controllers')):
                    p = vrcf_asset(dictof(e).get('controller'))
                    if p and inum(dictof(e).get('type'), 5) == 3 and db.get(p[0]):
                        ctls.append((Controller(Anim(db, av, self.vrcf.bases(root), vrcf_rewrite(
                            f.get('rewriteBindings'))), p[0]), {}, None))
        ctls = [x for x in ctls if x[0].guid is not None]
        out = {}
        for g in range(8):
            params = dict(self.params)
            params.update({'GestureLeft': float(g), 'GestureRight': float(g), 'GestureLeftWeight': 1.0,
                           'GestureRightWeight': 1.0})
            mus = {}
            for ctl, view, parts in ctls:
                local = dict(params)
                for a, b in view.items():
                    if b in params:
                        local[a] = params[b]
                for pr, v in ctl.evaluate(local)[0].items():
                    if pr[0] == 'h' and (parts is None or muscle_part(pr[2]) in parts):
                        mus[pr[2]] = v
            for hand, part in ((0, 7), (1, 8)):
                m = {i: v for i, v in mus.items() if muscle_part(i) == part}
                if m:
                    out[(hand, g)] = m
        return out

    def consonants(self):
        """the visemes of the consonants (VRChat's PP ... RR): {'pp': {(renderer, shape key): weight}, ...}; hypr3d's lip
        sync shows pp, ff, ss and ch"""
        if self.vrcf is not None and self.vrcf.consonants:  # VRCFury's Visemes, in place of the descriptor's
            return dict(self.vrcf.consonants)
        d, av = self.av.desc, self.av
        smr = d.get('VisemeSkinnedMesh')
        if inum(d.get('lipSync')) != 3 or not (isinstance(smr, Obj) and smr.cls == 137):
            return {}
        names = [str(x) for x in listof(d.get('VisemeBlendShapes'))]
        have = av.shape_names(smr)
        # (the descriptor's order: sil, PP, FF, TH, DD, kk, CH, SS, nn, RR, then the vowels)
        return {n: {(smr, names[i]): 1.0} for i, (n, _) in enumerate(CONSONANT_VISEMES, 1) if i < len(names) and names[i] in have}

    def visemes(self):
        if self.vrcf is not None and self.vrcf.visemes:  # VRCFury's Visemes, in place of the descriptor's
            return [{'name': p, 'preset': p, 'shapes': sh} for p, sh in self.vrcf.visemes.items()]
        d, av = self.av.desc, self.av
        ls = inum(d.get('lipSync'))
        smr = d.get('VisemeSkinnedMesh')
        out = []
        if ls in (2, 3) and not (isinstance(smr, Obj) and smr.cls == 137):
            warn('the visemes\' face mesh is missing')
            return out
        if ls == 3:
            names = [str(x) for x in listof(d.get('VisemeBlendShapes'))]
            have = av.shape_names(smr)
            for pr, i in (('aa', 10), ('ee', 11), ('ih', 12), ('oh', 13), ('ou', 14)):
                if i < len(names) and names[i] in have:
                    out.append({'name': pr, 'preset': pr, 'shapes': {(smr, names[i]): 1.0}})
        elif ls == 2:
            n = str(d.get('MouthOpenBlendShapeName') or '')
            if n in av.shape_names(smr):
                out.append({'name': 'aa', 'preset': 'aa', 'shapes': {(smr, n): 1.0}})
        elif ls == 1:
            warn('the visemes move a jaw bone, which is not converted')
        return out

    def eyes(self):
        """(blink expression or None, {'LeftEye': transform, 'RightEye': transform})"""
        d, av = self.av.desc, self.av
        ce = dictof(d.get('customEyeLookSettings'))
        if not truthy(d.get('enableEyeLook', '0')):
            if self.vrcf is not None and self.vrcf.blink:
                return {'name': 'blink', 'preset': 'blink', 'shapes': dict(self.vrcf.blink)}, {}
            return None, {}
        bones = {}
        for k, h in (('leftEye', 'LeftEye'), ('rightEye', 'RightEye')):
            t = ce.get(k)
            if isinstance(t, Obj) and t.go is not None and id(t.go) in av.inside:
                bones[h] = t.go
        blink = None
        smr = ce.get('eyelidsSkinnedMesh')
        if inum(ce.get('eyelidType')) == 2 and isinstance(smr, Obj) and smr.cls == 137:
            raw = ce.get('eyelidsBlendshapes')
            idx = []
            if isinstance(raw, str) and re.fullmatch(r'[0-9a-fA-F]*', raw):
                idx = [struct.unpack('<i', bytes.fromhex(raw[i:i + 8]))[0] for i in range(0, len(raw) - 7, 8)]
            elif isinstance(raw, list):
                idx = [inum(x, -1) for x in raw]
            names = av.shape_names(smr)
            if idx and 0 <= idx[0] < len(names):
                blink = {'name': 'blink', 'preset': 'blink', 'shapes': {(smr, names[idx[0]]): 1.0}}
        if self.vrcf is not None and self.vrcf.blink:  # VRCFury's Blinking, in place of VRChat's
            blink = {'name': 'blink', 'preset': 'blink', 'shapes': dict(self.vrcf.blink)}
        return blink, bones

    def humanoid(self):
        """{Unity human bone name: GameObject}"""
        return humanoid_map(self.db, self.av.root, self.av.gos)

    def dynamics(self):
        """([PhysBone or DynamicBone components], [collider components])"""
        bones, cols = [], []
        dead = self.ma.dead if self.ma else ()
        for go in self.av.gos:
            for c in go.comps:
                if c.cls != 114 or not truthy(c.data.get('m_Enabled', '1')) or id(c) in dead:
                    continue
                d = c.data
                if is_physbone(c):
                    bones.append(c)
                elif is_pb_collider(c):
                    cols.append(c)
                elif ('m_Root' in d or 'm_Roots' in d) and 'm_Elasticity' in d:
                    bones.append(c)
                elif 'm_Bound' in d and 'm_Direction' in d and ('m_Radius' in d or 'm_Center' in d):  # (or a plane)
                    cols.append(c)
        return bones, cols

    def chained(self):
        """the GameObjects the PhysBones and Dynamic Bones move"""
        av, out = self.av, set()
        for c in self.dynamics()[0]:
            d = c.data
            gos = lambda k: [x.go for x in listof(d.get(k)) if isinstance(x, Obj) and x.go is not None]
            if 'pull' in d:
                t = d.get('rootTransform')
                roots = [t.go if isinstance(t, Obj) and t.go is not None else c.go]
                stop = set(map(id, gos('ignoreTransforms') + (self.ma.ignores(c, roots[0]) if self.ma else [])))
            else:
                roots = [x.go for x in [d.get('m_Root')] + listof(d.get('m_Roots')) if isinstance(x, Obj) and x.go]
                stop = set(map(id, gos('m_Exclusions')))
            todo = [r for r in roots if r is not None]
            while todo:
                g = todo.pop()
                if id(g) in stop or id(g) not in av.inside or id(g) in out:
                    continue
                out.add(id(g))
                todo += self.ma.down(g) if self.ma else g.children
        return out

# ---------------------------------------------------------------- materials, from the .mat files

BUILTIN_GUID = '0000000000000000f000000000000000'
BUILTIN_SHADERS = {45: 'Standard (Specular setup)', 46: 'Standard'}
TEX_KEYS = ('_MainTex', '_BaseMap', '_BaseColorMap', '_MainTexture', '_BaseTexture', '_Albedo',
            '_AlbedoMap', '_Diffuse', '_DiffuseMap', '_Tex')
COLOR_KEYS = ('_Color', '_BaseColor', '_MainColor', '_TintColor', '_Tint')
BLENDER_IMAGES = {'.png', '.jpg', '.jpeg', '.tga', '.bmp', '.tif', '.tiff', '.exr', '.hdr', '.psd',
                  '.dds', '.webp'}
# the material properties an animation may change that hypr3d carries (MatInfo's): colour, emission colour, the
# textures' tiling and offset, the cutoff
MP_NAMES = frozenset(['%s.%s' % (c, ch) for c in COLOR_KEYS + ('_EmissionColor',) for ch in 'rgba'] +
                     ['%s_ST.%s' % (t, ch) for t in TEX_KEYS + ('_EmissionMap',) for ch in 'xyzw'] + ['_Cutoff'])


def to_linear(c):
    if c > 1.0:  # an HDR colour
        return c ** 2.2
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


# VRChat's consonant visemes: the settings file's names for them, and VRCFury's Visemes' (state_PP ...)
CONSONANT_VISEMES = (('pp', 'PP'), ('ff', 'FF'), ('th', 'TH'), ('dd', 'DD'), ('kk', 'kk'), ('ch', 'CH'), ('ss', 'SS'), ('nn', 'nn'),
                     ('rr', 'RR'))


def linear_rgba(c):
    return tuple(round(x, 5) for x in (to_linear(c[0]), to_linear(c[1]), to_linear(c[2]), min(max(c[3], 0.0), 1.0)))


# UnlitWF's shaders that draw stencil masks or outlines, by GUID (the .meta files of whiteflare's Unlit_WF_ShaderSuite,
# zlib licence), for materials whose shader is not in the input
UNLITWF_SHADERS = {
    '2efe527cfcbf0e1408b67463225f552f': 'UnlitWF/WF_UnToon_Transparent_Mask',
    '0b53cf0bcd0f9db4fa9d1297d255d06d': 'UnlitWF/WF_UnToon_Transparent_MaskOut',
    'd01a5c313ada49e488b2ef8c6b00f56d': 'UnlitWF/WF_UnToon_Transparent_MaskOut_Blend',
    '0299954f2a9b0994f8c9587945948766': 'UnlitWF/UnToon_TriShade/WF_UnToon_TriShade_Transparent_Mask',
    '06e9294a93df4474cac2f4157b5e1d1d': 'UnlitWF/UnToon_TriShade/WF_UnToon_TriShade_Transparent_MaskOut',
    'dfb821bc7afadc14591e4338a8ec865f': 'UnlitWF/UnToon_TriShade/WF_UnToon_TriShade_Transparent_MaskOut_Blend',
    'a5ae7f40ac53e274ea0bc1262e1f6895': 'UnlitWF/UnToon_Outline/WF_UnToon_Outline_Opaque',
    'ab4eb87c406a22f46887cf72178e2685': 'UnlitWF/UnToon_Outline/WF_UnToon_Outline_TransCutout',
    '5523e041d29d259439fa14bd131f5c82': 'UnlitWF/UnToon_Outline/WF_UnToon_Outline_Transparent',
    '5498b01615002d948bea7542f55e0c07': 'UnlitWF/UnToon_Outline/WF_UnToon_Outline_Transparent3Pass',
    '9350854c6e88f3f4eb873d2f94ff3328': 'UnlitWF/UnToon_Outline/WF_UnToon_Outline_Transparent_MaskOut',
    'ad88000744b4fb241835ba6ec106caf4': 'UnlitWF/UnToon_Outline/WF_UnToon_Outline_Transparent_MaskOut_Blend',
    '4eef00f52cc21b04e9e34e4caefa6bbf': 'UnlitWF/UnToon_Outline/WF_UnToon_OutlineOnly_Opaque',
    '64bf3ca653a7b274fab3e8a87016bfb0': 'UnlitWF/UnToon_Outline/WF_UnToon_OutlineOnly_TransCutout',
    '660abd485057f4740ac9050f7ab3237d': 'UnlitWF/UnToon_Outline/WF_UnToon_OutlineOnly_Transparent',
    '3c07b964e541eef45bc195a029b878b3': 'UnlitWF/UnToon_Outline/WF_UnToon_OutlineOnly_Transparent_MaskOut',
    '98bb3de5d5444094aa041a65f8a85708': 'UnlitWF/UnToon_Mobile/WF_UnToon_Mobile_Outline_Opaque',
    '3276a740671679f44b1141523bf73e33': 'UnlitWF/UnToon_Mobile/WF_UnToon_Mobile_Outline_TransCutout',
    'd279a88eda1ae0e4c89e92539639eb16': 'UnlitWF/UnToon_Mobile/WF_UnToon_Mobile_OutlineOnly_Opaque',
    'e0b93fdad2eeedf42baccbc0975cdd1d': 'UnlitWF/UnToon_Mobile/WF_UnToon_Mobile_OutlineOnly_TransCutout',
    '871fd7a51a8ea3e4980c3fe7b8347619': 'UnlitWF/UnToon_PowerCap_Outline/WF_UnToon_PowerCap_Outline_Opaque',
    '7240817400475dd41b084e32b1264d6f': 'UnlitWF/UnToon_PowerCap_Outline/WF_UnToon_PowerCap_Outline_TransCutout',
    '90cac9ec3b2a7524eb99b36ab87f25f1': 'UnlitWF/Custom/WF_UnToon_Custom_OffsetOutline_Opaque',
}
# lilToon's shaders that draw outlines, by GUID (the .meta files of lilxyzw/lilToon, MIT licence)
LILTOON_OUTLINE = {
    'efa77a80ca0344749b4f19fdd5891cbe': 'Hidden/lilToonOutline',
    '3b4aa19949601f046a20ca8bdaee929f': 'Hidden/lilToonCutoutOutline',
    '3c79b10c7e0b2784aaa4c2f8dd17d55e': 'Hidden/lilToonTransparentOutline',
    '7171688840c632447b22ec14e2bdef7e': 'Hidden/lilToonOnePassTransparentOutline',
    '9cf054060007d784394b8b0bb703e441': 'Hidden/lilToonTwoPassTransparentOutline',
    'c6d605ee23b18fc46903f38c67db701f': 'Hidden/lilToonTessellationOutline',
    '5ba517885727277409feada18effa4a6': 'Hidden/lilToonTessellationCutoutOutline',
    '9b0c2630b12933248922527d4507cfa9': 'Hidden/lilToonTessellationTransparentOutline',
    '67ed0252d63362a4ab23707a720508b7': 'Hidden/lilToonTessellationOnePassTransparentOutline',
    '7e61dbad981ad4f43a03722155db1c6a': 'Hidden/lilToonTessellationTwoPassTransparentOutline',
    '583a88005abb81a4ebbce757b4851a0d': 'Hidden/lilToonLiteOutline',
    '8cf5267d397b04846856f6d3d9561da0': 'Hidden/lilToonLiteCutoutOutline',
    '1c12a37046f07ac4486881deaf0187ea': 'Hidden/lilToonLiteTransparentOutline',
    '701268c07d37f5441b25b2cb99fae4b3': 'Hidden/lilToonLiteOnePassTransparentOutline',
    '62df797f407281640a224388953448cc': 'Hidden/lilToonLiteTwoPassTransparentOutline',
    '51b2dee0ab07bd84d8147601ff89e511': 'Hidden/lilToonMultiOutline',
    'fba17785d6b2c594ab6c0303c834da65': '_lil/[Optional] lilToonOutlineOnly',
    '3b3957e6c393b114bab6f835b4ed8f5d': '_lil/[Optional] lilToonOutlineOnlyCutout',
    '0c762f24b85918a49812fc5690619178': '_lil/[Optional] lilToonOutlineOnlyTransparent',
}
UNITY_QUEUES = {'background': 1000, 'geometry': 2000, 'alphatest': 2450, 'geometrylast': 2500, 'transparent': 3000,
                'overlay': 4000}
MODE_QUEUE = {'OPAQUE': 2000, 'MASK': 2450, 'BLEND': 3000}  # what hypr3d takes a material's queue for without one
STENCIL_COMPS = ('always', 'never', 'less', 'equal', 'lequal', 'greater', 'notequal', 'gequal', 'always')  # by value
STENCIL_OPS = ('keep', 'zero', 'replace', 'incrsat', 'decrsat', 'invert', 'incrwrap', 'decrwrap')
STENCIL_WORDS = {'lessequal': 'lequal', 'greaterequal': 'gequal', 'disabled': 'always', 'incr': 'incrsat',
                 'decr': 'decrsat'}


class MatInfo:
    """a Unity material, as far as glTF carries it"""

    def __init__(self, name):
        self.name = name
        self.shader = ''
        self.tex = None  # image file
        self.tex_xf = ((1.0, 1.0), (0.0, 0.0))  # Unity's tiling and offset
        self.color = (1.0, 1.0, 1.0, 1.0)  # linear
        self.mode = 'OPAQUE'  # OPAQUE, MASK, BLEND
        self.alpha = None  # alpha from elsewhere than the texture: (image file, 'R' or 'A'), or '' for none
        self.cutoff = 0.5
        self.double = False
        self.emit = (0.0, 0.0, 0.0)  # linear, may go over 1
        self.emit_tex = None
        self.emit_xf = ((1.0, 1.0), (0.0, 0.0))
        self.invert = None  # the alpha is 1 - its source's times this (UnlitWF's _AL_InvMaskVal)
        # what the glTF material's extras carry for hypr3d (patch_materials): Unity's render queue when it is not
        # the one of the material's alpha mode, the stencil test, the toon outline, the back faces, the light clamp
        self.queue = -1
        self.stencil = None  # {ref, read, write, comp, pass, fail, zfail}, and 'again': {comp, alpha} (MaskOut_Blend)
        self.outline = None  # {width (m), space, color (linear), base, tint, mask: (image, channel), shift (m), fix, lit,
        #                       tex: (image, (scale, offset), blend or None: times it)}
        self.back = None  # {color (linear), tex: image or None (the main texture), xf}
        self.light = None  # (min, max, chroma): UnlitWF's _GL_LevelMin, _GL_LevelMax, _GL_BlendPower
        self.toon = None  # {shade: [r, g, b] (linear), base: times the base colour, tex: image or None, lo, hi, strength}
        self.matcap = None  # {tex: image, color: [r, g, b, amount], mode: add|multiply|mix|median, lit}

    def key(self):
        return (self.name, self.tex, self.tex_xf, self.color, self.mode, self.alpha, self.cutoff, self.double,
                self.emit, self.emit_tex, self.emit_xf, self.invert, self.queue, repr(self.stencil),
                repr(self.outline), repr(self.back), self.light, repr(self.toon), repr(self.matcap))


def _prop_entries(sp, key):
    """[(name, value)] of a m_SavedProperties list, in either of its layouts"""
    v = sp.get(key)
    if isinstance(v, dict):
        return [(str(k), x) for k, x in v.items()]
    out = []
    for e in listof(v):
        if not isinstance(e, dict):
            continue
        if 'first' in e and 'second' in e:  # Unity 5.x: {first: {name: _Color}, second: ...}
            f = e['first']
            out.append((str(dictof(f).get('name') if isinstance(f, dict) else f), e['second']))
        else:
            out += [(str(k), x) for k, x in e.items()]
    return out


class Materials:
    """what the renderers' material slots hold"""

    def __init__(self, db):
        self.db = db
        self._mats = {}  # (guid, fileID) -> MatInfo | None
        self._shaders = {}
        self._shader_src = {}  # shader guid -> its source, when the input has it
        self._remaps = {}  # model guid -> {material name: reference}
        self.tex_meta = {}  # image file -> (largest size, has alpha)
        self.default = MatInfo('Default-Material')
        self.default.color = (0.6, 0.6, 0.6, 1.0)

    def shader_name(self, r):
        f, g = ref(r)
        if g is None or g == BUILTIN_GUID:
            return BUILTIN_SHADERS.get(f, '')
        if g not in self._shaders:
            a = self.db.get(g)
            name = a.name if a is not None else ''
            if a is not None and a.ext == '.shader':
                try:
                    with open(a.file, 'r', encoding='utf-8', errors='replace') as fh:
                        src = self._shader_src[g] = fh.read(1 << 20)
                    m = re.search(r'^\s*Shader\s+"([^"]*)"', src, re.M)
                    if m:
                        name = m.group(1)
                except OSError:
                    pass
            self._shaders[g] = name
        return self._shaders[g]

    def unlitwf_double(self, shader, low, fl, blend):
        """whether an UnlitWF material shows both faces: its transparent shaders draw the back faces (a MAIN_BACK pass)
        and then the front ones; the others cull by _CullMode, which defaults to the shader's (0 in the cutout
        ones and the mobile transparent ones, else 2). Without the shader or its name, a see-through one is taken for
        one of the transparent shaders"""
        self.shader_name(shader)
        src = self._shader_src.get(ref(shader)[1] or '')
        if src:
            if re.search(r'Name\s+"MAIN_BACK"', src) or not re.search(r'Cull\s+\[_CullMode\]', src) and \
                    re.search(r'Cull\s+OFF', src, re.I) and not re.search(r'Cull\s+(BACK|FRONT)', src, re.I):
                return True
            d = re.search(r'^\s*_CullMode\s*\(\s*"[^"]*"\s*,\s*\w+\s*\)\s*=\s*(\d+)', src, re.M)
            default = int(d.group(1)) if d else 2
        else:  # the shader is not in the input: by its name
            if 'fakefur' in low or 'transparent' in low and not ('mobile' in low or 'lameonly' in low) or (
                    not low and blend):
                return True
            default = 0 if 'transcutout' in low or 'mobile' in low and 'transparent' in low else 2
        return int(round(fl.get('_CullMode', default))) == 0

    def image(self, r):
        """the file of a texture reference, if Blender can read it"""
        f, g = ref(r)
        a = self.db.get(g) if f else None
        if a is None:
            return None
        if a.ext not in BLENDER_IMAGES:
            warn('%s: %s textures are not converted' % (a.path, a.ext))
            return None
        if a.file not in self.tex_meta:
            imp = self.db.importer(a.guid)
            size = inum(imp.get('maxTextureSize'), 2048)
            for p in listof(imp.get('platformSettings')):
                p = dictof(p)
                t = str(p.get('buildTarget') or '')
                if t == 'DefaultTexturePlatform' or (t == 'Standalone' and truthy(p.get('overridden', '0'))):
                    size = inum(p.get('maxTextureSize'), size)
            # alphaUsage 0: the texture's alpha is not used (1: from the file, 2: from its brightness)
            self.tex_meta[a.file] = (size if size > 0 else 2048,
                                     inum(imp.get('alphaUsage', imp.get('alphaSource')), 1) != 0)
        return a.file

    def props(self, guid, fid, depth=0):
        """(name, shader, keywords, render queue, tags, textures, numbers, colours) of a material, over its
        parent's when it is a variant"""
        uf = self.db.yaml(guid)
        if uf is None or uf.binary:
            return None
        if uf.cls(fid) != 21:
            fid = uf.main(21)
            if fid is None:
                return None
        body = uf.get(fid)[1]
        base = None
        pf, pg = ref(body.get('m_Parent'))
        if pf and depth < 8:
            base = self.props(pg or guid, pf, depth + 1)
        sp = dictof(body.get('m_SavedProperties'))
        tex, fl, col = {}, {}, {}
        for k, v in _prop_entries(sp, 'm_TexEnvs'):
            v = dictof(v)
            sc, of = dictof(v.get('m_Scale')), dictof(v.get('m_Offset'))
            tex[k] = (v.get('m_Texture'), (num(sc.get('x'), 1.0), num(sc.get('y'), 1.0)),
                      (num(of.get('x')), num(of.get('y'))))
        for k, v in _prop_entries(sp, 'm_Ints') + _prop_entries(sp, 'm_Floats'):
            fl[k] = num(v)
        for k, v in _prop_entries(sp, 'm_Colors'):
            v = dictof(v)
            col[k] = (num(v.get('r'), 1.0), num(v.get('g'), 1.0), num(v.get('b'), 1.0), num(v.get('a'), 1.0))
        kw = set(str(body.get('m_ShaderKeywords') or '').split())
        kw |= {str(x) for x in listof(body.get('m_ValidKeywords'))}
        shader = body.get('m_Shader')
        rq = inum(body.get('m_CustomRenderQueue'), -1)
        tags = {str(k): str(v) for k, v in dictof(body.get('stringTagMap')).items()}
        if base is not None:
            _, bsh, bkw, brq, btags, btex, bfl, bcol = base
            btex.update(tex)
            bfl.update(fl)
            bcol.update(col)
            btags.update(tags)
            tex, fl, col, tags = btex, bfl, bcol, btags
            kw = kw or bkw
            if not ref(shader)[0]:
                shader = bsh
            if rq < 0:
                rq = brq
        return str(body.get('m_Name', '')), shader, kw, rq, tags, tex, fl, col

    def material(self, r):
        """the MatInfo of a material reference; None if it is missing or not a .mat"""
        f, g = ref(r)
        if not f:
            return None
        if g == BUILTIN_GUID:
            return self.default
        a = self.db.get(g)
        if a is None or a.ext != '.mat':
            return None
        key = (a.guid, f)
        if key not in self._mats:
            p = self.props(a.guid, f)
            self._mats[key] = self.build(*p) if p is not None else None
        return self._mats[key]

    def build(self, name, shader, kw, rq, tags, tex, fl, col):
        m = MatInfo(name or 'material')
        m.shader = sh = self.shader_name(shader)
        low = sh.lower()
        for k in TEX_KEYS:
            t = tex.get(k)
            if t and ref(t[0])[0]:
                m.tex = self.image(t[0])
                if m.tex:
                    m.tex_xf = (t[1], t[2])
                break
        for k in COLOR_KEYS:
            if k in col:
                c = col[k]
                if k == '_TintColor':  # the particle shaders double it
                    c = tuple(min(x * 2, 1.0) for x in c)
                m.color = (to_linear(c[0]), to_linear(c[1]), to_linear(c[2]), c[3])
                break
        m.mode = self.alpha_mode(low, kw, rq, tags, fl, col)
        # UnlitWF, also by its property names when its shader is not in the input
        wf = 'unlitwf' in low.replace('_', '') or not sh and any(k in fl for k in ('_AL_Source', '_GL_LevelMin',
                                                                                    '_ES_Enable'))
        blend = m.mode == 'BLEND'  # (before the alpha source may make it opaque)
        if m.mode != 'OPAQUE' and wf:
            self.unlitwf_alpha(m, tex, fl)
        if m.mode != 'OPAQUE' and m.tex and m.alpha is None and not self.tex_meta.get(m.tex, (0, True))[1] and \
                m.color[3] >= 0.999:
            m.mode = 'OPAQUE'  # the texture's alpha is switched off and nothing else is see-through
        m.cutoff = min(max(fl.get('_Cutoff', fl.get('_Clipping_Level', fl.get('_AlphaCutoff', 0.5))), 0.0), 1.0)
        m.double = int(round(fl.get('_Cull', fl.get('_CullMode', fl.get('_Culling', 2.0))))) == 0
        if wf:
            m.double = self.unlitwf_double(shader, low, fl, blend)
        on = '_EMISSION' in kw or 'mtoon' in low or any(
            fl.get(k, 0.0) > 0.5 for k in ('_EnableEmission', '_UseEmission', '_EmissionEnabled', '_UseEmissive'))
        level = 1.0
        if wf:  # UnlitWF's emission ("emissive scroll"): constant, or a wave
            on = on or '_ES_ENABLE' in kw or fl.get('_ES_Enable', 0.0) > 0.5
            off = fl.get('_ES_LevelOffset', 0.0)
            level = min(max(1 + off, 0.0), 1.0) if int(fl.get('_ES_Shape', 3)) == 3 else min(max(0.5 + off, 0.0), 1.0)
        c = col.get('_EmissionColor')
        if on and c and max(c[:3]) > 0.004 and level > 0:
            m.emit = tuple(to_linear(x) * level for x in c[:3])
            t = tex.get('_EmissionMap')
            if t and ref(t[0])[0]:
                m.emit_tex = self.image(t[0])
                if m.emit_tex:
                    m.emit_xf = (t[1], t[2])
        m.queue = self.render_queue(shader, rq)
        m.stencil = self.stencil(shader, fl)
        m.outline = self.outline(shader, low, kw, tex, fl, col)
        m.toon, m.matcap = self.toon(shader, low, kw, tex, fl, col, wf)
        if wf:
            m.back = self.unlitwf_back(m, kw, tex, fl, col)
            # the light's brightness kept between these (UnlitWF's anti-glare), its colour this saturated; the
            # minimum moved towards 0 or 1 by _GL_LevelTweak (newer UnlitWF), then taken from gamma to linear
            # (calcLightColorFrag's GammaToCurrentColorSpaceExact)
            lo, tw = fl.get('_GL_LevelMin', 0.125), fl.get('_GL_LevelTweak', 0.0)
            lo = lo + (0.0 - lo) * -tw if tw < 0 else lo + (1.0 - lo) * tw
            m.light = (round(to_linear(min(max(lo, 0.0), 1.0)), 4), round(min(max(fl.get('_GL_LevelMax', 0.8), 0.0), 1.0), 4),
                       round(min(max(fl.get('_GL_BlendPower', 0.8), 0.0), 1.0), 4))
        return m

    def unlitwf_alpha(self, m, tex, fl):
        """UnlitWF's alpha (WF_UnToon_Function.cginc pickAlpha): the main texture's and colour's (_AL_Source 0) or
        a mask texture's red or alpha (1, 2) alone, times _AL_Power when blending; an empty mask is Unity's
        default white, so the material is opaque (eyes drawn with the stencil mask shaders often are)"""
        src = int(fl.get('_AL_Source', 0))
        power = min(max(fl.get('_AL_Power', 1.0), 0.0), 2.0) if m.mode == 'BLEND' else 1.0
        inv = fl.get('_AL_InvMaskVal', 0.0) > 0.5  # 1 - the source's alpha: baked into the texture (Build)
        if src in (1, 2):
            t = tex.get('_AL_MaskTex')
            mask = self.image(t[0]) if t and ref(t[0])[0] else None
            if mask is not None:
                m.alpha = (mask, 'R' if src == 1 else 'A')
                m.invert = 1.0 if inv else None
            elif inv:  # white, inverted: nothing shows
                m.alpha, m.color = '', m.color[:3] + (0.0,)
                return
            elif power >= 0.999:
                m.mode, m.color = 'OPAQUE', m.color[:3] + (1.0,)
                return
            else:
                m.alpha = ''
            m.color = m.color[:3] + (1.0,)
        elif inv:  # 1 - the main texture's alpha times the colour's
            m.invert, m.color = m.color[3], m.color[:3] + (1.0,)
        m.color = m.color[:3] + (min(m.color[3] * power, 1.0),)

    def shader_source(self, shader):
        self.shader_name(shader)
        return self._shader_src.get(ref(shader)[1] or '') or ''

    def known_shader(self, shader):
        """a shader's name: its own, or for one not in the input, the UnlitWF or lilToon tables'"""
        g = ref(shader)[1] or ''
        return self.shader_name(shader) or UNLITWF_SHADERS.get(g) or LILTOON_OUTLINE.get(g) or ''

    @staticmethod
    def shader_default(src, prop, default):
        """a property's default in a shader's source"""
        m = re.search(r'^[ \t]*(?:\[[^\]\n]*\][ \t]*)*%s\s*\(\s*"[^"]*"\s*,(?:[^()\n]|\([^()\n]*\))*\)\s*=\s*(-?[\d.]+)'
                      % re.escape(prop), src, re.M)
        return float(m.group(1)) if m else default

    def render_queue(self, shader, rq):
        """Unity's render queue: the material's, else its shader's (-1: not known)"""
        if rq >= 0:
            return rq
        q = re.search(r'"Queue"\s*=\s*"(\w+)\s*(?:([+-])\s*(\d+))?\s*"', self.shader_source(shader))
        if q:
            return UNITY_QUEUES.get(q.group(1).lower(), 2000) + (int(q.group(3)) * (1 if q.group(2) == '+' else -1)
                                                                   if q.group(3) else 0)
        name = UNLITWF_SHADERS.get(ref(shader)[1] or '', '')
        if name:
            return 3000 + ('MaskOut' in name) if 'Transparent' in name else 2450 if 'TransCutout' in name else 2000
        return -1

    def stencil(self, shader, fl):
        """the stencil test and write of a material's shader: from the Stencil blocks of its passes (but outlines,
        shadows and such), the first of them and another test a later pass makes (UnlitWF's MaskOut_Blend draws what
        its mask hides again, fainter: 'again'); or, the shader not in the input, UnlitWF's by its name and lilToon's
        and Poiyomi's by their material properties. None: it neither tests nor writes"""
        src = self.shader_source(shader)

        def value(tok, default):
            if tok is None:
                return default
            if tok.startswith('['):
                p = tok[1:-1]
                return fl.get(p, self.shader_default(src, p, default))
            try:
                return float(tok)
            except ValueError:
                return tok.lower()

        def word(v, table):
            if isinstance(v, str):
                return STENCIL_WORDS.get(v, v)
            i = int(round(v))
            return table[i] if 0 <= i < len(table) else table[0]

        def state(kv):
            return {'ref': int(value(kv.get('Ref'), 0)) & 255, 'read': int(value(kv.get('ReadMask'), 255)) & 255,
                    'write': int(value(kv.get('WriteMask'), 255)) & 255,
                    'comp': word(value(kv.get('Comp'), 8), STENCIL_COMPS),
                    'pass': word(value(kv.get('Pass'), 0), STENCIL_OPS),
                    'fail': word(value(kv.get('Fail'), 0), STENCIL_OPS),
                    'zfail': word(value(kv.get('ZFail'), 0), STENCIL_OPS)}
        states = []
        for st in re.finditer(r'\bStencil\s*\{([^{}]*)\}', src):
            names = re.findall(r'\bName\s+"([^"]*)"', src[:st.start()])
            pn = names[-1].upper() if names else ''
            if any(x in pn for x in ('OUTLINE', 'SHADOW', 'META', 'DEPTH', 'CLR_BG', 'CANCEL')):
                continue
            states.append(state(dict(re.findall(r'\b(Ref|ReadMask|WriteMask|Comp|Pass|Fail|ZFail)\s+(\[\w+\]|\w+)',
                                                st.group(1)))))
        name = self.known_shader(shader)
        if not src and 'UnlitWF' in name and '_Mask' in name:
            i = str(int(fl.get('_StencilMaskID', 8)))  # (8: the shaders' default)
            if 'MaskOut' in name:
                states = [state({'Ref': i, 'ReadMask': '15', 'Comp': 'notEqual'})]
                if 'MaskOut_Blend' in name:
                    states.append(state({'Ref': i, 'ReadMask': '15', 'Comp': 'equal'}))
            else:
                states = [state({'Ref': i, 'WriteMask': i, 'Comp': 'always', 'Pass': 'replace'})]
        elif not src:
            for props in (('_StencilRef', '_StencilReadMask', '_StencilWriteMask', '_StencilComp', '_StencilPass',
                           '_StencilFail', '_StencilZFail'),  # lilToon
                          ('_StencilRef', '_StencilReadMask', '_StencilWriteMask', '_StencilCompareFunction',
                           '_StencilPassOp', '_StencilFailOp', '_StencilZFailOp')):  # Poiyomi
                if props[3] in fl:
                    states = [state(dict(zip(('Ref', 'ReadMask', 'WriteMask', 'Comp', 'Pass', 'Fail', 'ZFail'),
                                             ('[%s]' % p for p in props))))]
                    break
        if not states:
            return None
        s = states[0]
        if s['comp'] == 'always' and s['pass'] == s['fail'] == s['zfail'] == 'keep':
            return None
        again = next((x for x in states[1:] if x['comp'] != s['comp']), None)
        if again is not None:
            alpha = 1.0
            if re.search(r'#define\s+_AL_CustomValue\s+_AL_StencilPower', src) or 'MaskOut_Blend' in name:
                alpha = fl.get('_AL_StencilPower', self.shader_default(src, '_AL_StencilPower', 0.5))
            s['again'] = {'comp': again['comp'], 'alpha': round(min(max(alpha, 0.0), 1.0), 4)}
        return s

    def outline(self, shader, low, kw, tex, fl, col):
        """the toon outline a material's shader draws (an inverted hull): UnlitWF's (_TL_*), lilToon's, Poiyomi's
        or MToon's; None: none"""
        name = self.known_shader(shader)
        nlow = name.lower()
        src = self.shader_source(shader)

        def mask(key, channel=0, inv=False):
            t = tex.get(key)
            f = self.image(t[0]) if t and ref(t[0])[0] else None
            return (f, 'RGBA'[min(max(int(channel), 0), 3)], bool(inv)) if f else None

        def color(key, default):
            return linear_rgba(col.get(key, default))

        def ctex(key, uv=None, blend=None):
            """the line's colour texture: (image, its tiling, how far the colour goes towards it; None: times it)"""
            t = tex.get(key)
            f = self.image(t[0]) if t and ref(t[0])[0] else None
            if not f:
                return None
            u = uv or t
            return (f, (u[1], u[2]), blend)
        tl = '_TL_ENABLE' in kw or not kw and fl.get('_TL_Enable', 0.0) > 0.5
        if tl and ('unlitwf' in nlow.replace('_', '') and 'outline' in nlow or re.search(r'Name\s+"OUTLINE"', src) or
                   not name and '_TL_LineWidth' in fl):
            w = max(fl.get('_TL_LineWidth', 0.05), 0.0) * 0.01
            # the EDGE type's lines are pushed back ten widths, so only a silhouette's show
            back = fl.get('_TL_Z_Shift', 0.0) + (w * 10 if fl.get('_TL_LineType', 0.0) > 0.5 else 0.0)
            # its custom colour texture, mixed in by _TL_BlendCustom, on the main texture's uv (UnlitWF's
            # WF_TEX2D_OUTLINE_COLOR); white when it has none
            custom = min(max(fl.get('_TL_BlendCustom', 0.0), 0.0), 1.0)
            line = color('_TL_LineColor', (0.1, 0.1, 0.1, 1))
            ct = ctex('_TL_CustomColorTex', tex.get('_MainTex'), round(custom, 4)) if custom > 0 else None
            if custom > 0 and not ct:
                line = tuple(round(c + (1.0 - c) * custom, 5) for c in line[:3]) + tuple(line[3:])
            return {'width': round(w, 6), 'space': 'world', 'color': line,
                    'base': round(min(max(fl.get('_TL_BlendBase', 0.0), 0.0), 1.0), 4),
                    'mask': mask('_TL_MaskTex', 0, fl.get('_TL_InvMaskVal', 0.0) > 0.5), 'shift': round(-back, 5) + 0.0,
                    'lit': 1.0, 'tex': ct} if w > 0 else None
        g = ref(shader)[1] or ''
        if g in LILTOON_OUTLINE or 'liltoon' in nlow and 'outline' in nlow:
            w = max(fl.get('_OutlineWidth', 0.08), 0.0) * 0.01
            # its colour: _OutlineTex (often the main texture) times _OutlineColor (lilToon's OVERRIDE_OUTLINE_COLOR)
            return {'width': round(w, 6), 'space': 'object', 'color': color('_OutlineColor', (0.6, 0.56, 0.73, 1)),
                    'mask': mask('_OutlineWidthMask'), 'shift': round(-fl.get('_OutlineZBias', 0.0), 5) + 0.0,
                    'fix': [round(min(max(fl.get('_OutlineFixWidth', 0.5), 0.0), 1.0), 4), 1.0],
                    'lit': round(min(max(fl.get('_OutlineEnableLighting', 1.0), 0.0), 1.0), 4),
                    'tex': ctex('_OutlineTex')} if w > 0 else None
        if ('poiyomi' in low or '.poi' in low or not name and '_EnableOutlines' in fl) and \
                fl.get('_EnableOutlines', 0.0) > 0.5:
            w = max(fl.get('_LineWidth', 1.0), 0.0) * 0.01
            fix = None
            if fl.get('_OutlineFixedSize', 1.0) > 0.5:
                fix = [round(min(max(fl.get('_OutlineFixWidth', 0.5), 0.0), 1.0), 4),
                       round(max(fl.get('_OutlinesMaxDistance', 1.0), 0.0), 4)]
            # its colour: _OutlineTexture (on the first uv set) times the line's colour, times the base as much as
            # _OutlineTintMix says (Poiyomi's outline fragment)
            return {'width': round(w, 6), 'space': 'world' if fl.get('_OutlineSpace', 0.0) > 0.5 else 'object',
                    'color': color('_LineColor', (1, 1, 1, 1)), 'tint': round(fl.get('_OutlineTintMix', 0.0), 4),
                    'mask': mask('_OutlineMask', fl.get('_OutlineMaskChannel', 0.0)), 'fix': fix,
                    'lit': round(min(max(fl.get('_OutlineLit', 1.0), 0.0), 1.0), 4),
                    'tex': ctex('_OutlineTexture') if int(fl.get('_OutlineTextureUV', 0.0)) == 0 else None} if w > 0 else None
        if 'mtoon' in low or not name and '_OutlineWidthMode' in fl:
            mode = int(fl.get('_OutlineWidthMode', 0))
            ten = 'mtoon10' in low or '_OutlineWidthTex' in tex  # UniVRM's MToon10: metres, the mask's green
            w = max(fl.get('_OutlineWidth', 0.5), 0.0) * (1.0 if ten else 0.01)
            if mode not in (1, 2) or w <= 0:
                return None
            lit = fl.get('_OutlineLightingMix', 1.0)
            out = {'width': round(w, 6), 'space': 'world' if mode == 1 else 'screen',
                   'color': color('_OutlineColor', (0, 0, 0, 1)),
                   'mask': mask('_OutlineWidthTex', 1) if ten else mask('_OutlineWidthTexture'),
                   'lit': round(lit, 4) if ten or int(fl.get('_OutlineColorMode', 0)) == 1 else 0.0}
            if mode == 2 and ten:  # of the screen's height, however far
                out['width'], out['max'] = round(w * 2, 6), 1e6
            elif mode == 2:  # in clip space, up to so far away
                out['max'] = round(max(fl.get('_OutlineScaledMaxDistance', 1.0), 0.0), 4)
            return out
        return None

    def toon(self, shader, low, kw, tex, fl, col, wf):
        """a material's toon shading and matcap, as hypr3d draws them (MatInfo's toon and matcap): UnlitWF's toon shade
        and matcap (_TS_*, _HL_*: WF_UnToon_Function.cginc's drawToonShade and calcMatcapColor), lilToon's first
        shadow and matcap (lil_common_frag.hlsl), Poiyomi's Multilayer Math (lilToon's) and Flat lighting and its
        first matcap, MToon's (0.x: MToonCore.cginc; MToon10: VRMC_materials_mtoon). The shade is lit by the sun as
        the lit colour is, from where N·L is lo (all shade) to hi (all lit); a toon step on half-Lambert (N·L / 2 + 1/2,
        as lilToon and UnlitWF have it) is taken to N·L. UnlitWF without its toon shade and Poiyomi's Flat are lit
        the same all round (lo = hi = -1). (None, None): neither"""
        name = self.known_shader(shader)
        nlow = name.lower()

        def image(key):
            t = tex.get(key)
            return self.image(t[0]) if t and ref(t[0])[0] else None

        def color(key, default):
            return list(linear_rgba(col.get(key, default)))

        def clamp(x, lo=0.0, hi=1.0):
            return min(max(x, lo), hi)

        def step(lo, hi):  # N·L from where it's all shade to where it's all lit
            return round(lo, 4), round(max(hi, lo + 0.001), 4)

        def halfstep(lo, hi):  # from half-Lambert's
            lo, hi = clamp(lo), clamp(hi)
            return step(2 * lo - 1, 2 * hi - 1)

        shade = cap = None
        flat = {'shade': [1.0, 1.0, 1.0], 'base': True, 'tex': None, 'lo': -1.0, 'hi': -1.0, 'strength': 1.0}
        if wf:
            # UnlitWF's light has no N·L in it (calcLightColorVertex: the lights' colours and the ambient's), so
            # without its toon shade it's lit the same all round
            shade = flat
            if '_TS_ENABLE' in kw or not kw and fl.get('_TS_Enable', 0.0) > 0.5:
                # the colour times 1st / base, _TS_Power of the way there; UnlitWF weakens it as the other light gets
                # stronger (calcShadowPower), to about 3/4 under hypr3d's sun and sky, unless _TS_FixContrast
                fix = '_TS_FIXC_ENABLE' in kw or fl.get('_TS_FixContrast', 0.0) > 0.5
                p = clamp(fl.get('_TS_Power', 1.0), 0.0, 2.0) * (1.0 if fix else 0.75)
                first, base = color('_TS_1stColor', (0.81, 0.81, 0.9, 1)), color('_TS_BaseColor', (1, 1, 1, 1))
                s = [round(max(0.0, 1 + p * (min(f / max(b, 1e-4), 4.0) - 1)), 4) for f, b in zip(first[:3], base[:3])]
                border = clamp(fl.get('_TS_1stBorder', 0.4))
                lo, hi = halfstep(border, border + clamp(fl.get('_TS_1stFeather', 0.05), 0.001))
                shade = {'shade': s, 'base': True, 'tex': None, 'lo': lo, 'hi': hi, 'strength': 1.0}
            f = image('_HL_MatcapTex') if '_HL_ENABLE' in kw or not kw and fl.get('_HL_Enable', 0.0) > 0.5 else None
            if f:
                # its tint doubled (grey: none); the light and shade caps take it gamma encoded
                kind = int(fl.get('_HL_CapType', 0))
                two = [min(2 * c, 4.0) for c in color('_HL_MatcapColor', (0.5, 0.5, 0.5, 1))[:3]]
                if kind in (1, 2):
                    two = [max(1.055 * c ** (1 / 2.4) - 0.055, 0.0) for c in two]
                cap = {'tex': f, 'color': [round(c, 4) for c in two] + [round(clamp(fl.get('_HL_Power', 1.0), 0.0, 2.0), 4)],
                       'mode': {1: 'add', 2: 'multiply'}.get(kind, 'median'), 'lit': 1.0}
        elif 'liltoon' in nlow or 'liltoon' in low or not name and '_UseShadow' in fl and '_TransparentMode' in fl:
            if fl.get('_UseShadow', 0.0) > 0.5:
                border, blur = fl.get('_ShadowBorder', 0.5), fl.get('_ShadowBlur', 0.1)
                lo, hi = halfstep(border - blur / 2, border + blur / 2)
                t = image('_ShadowColorTex')  # (in place of the base colour)
                shade = {'shade': color('_ShadowColor', (0.82, 0.76, 0.85, 1))[:3], 'base': t is None, 'tex': t,
                         'lo': lo, 'hi': hi, 'strength': round(clamp(fl.get('_ShadowStrength', 1.0)), 4)}
            f = image('_MatCapTex') if fl.get('_UseMatCap', 0.0) > 0.5 else None
            if f:
                c = color('_MatCapColor', (1, 1, 1, 1))
                cap = {'tex': f, 'color': c[:3] + [round(c[3] * clamp(fl.get('_MatCapBlend', 1.0)), 4)],
                       'mode': {0: 'mix', 3: 'multiply'}.get(int(fl.get('_MatCapBlendMode', 1)), 'add'),  # (screen: added)
                       'lit': round(clamp(fl.get('_MatCapEnableLighting', 1.0)), 4)}
        elif 'poiyomi' in low or '.poi' in low or not name and '_LightingMode' in fl:
            mode = int(fl.get('_LightingMode', 5))
            if mode == 1:  # Multilayer Math: lilToon's
                border, blur = fl.get('_ShadowBorder', 0.5), fl.get('_ShadowBlur', 0.1)
                lo, hi = halfstep(border - blur / 2, border + blur / 2)
                t = image('_ShadowColorTex')
                shade = {'shade': color('_ShadowColor', (0.82, 0.76, 0.85, 1))[:3], 'base': t is None, 'tex': t,
                         'lo': lo, 'hi': hi, 'strength': round(clamp(fl.get('_ShadowStrength', 1.0)), 4)}
            elif mode == 5:  # Flat: the same light all round
                shade = flat
            f = image('_Matcap') if fl.get('_MatcapEnable', 0.0) > 0.5 else None
            if f:
                c = color('_MatcapColor', (1, 1, 1, 1))
                how, amount = max((('mix', fl.get('_MatcapReplace', 1.0)), ('multiply', fl.get('_MatcapMultiply', 0.0)),
                                   ('add', fl.get('_MatcapAdd', 0.0))), key=lambda x: x[1])
                if amount > 0:
                    k = clamp(fl.get('_MatcapIntensity', 1.0), 0.0, 5.0) * c[3] * clamp(amount)
                    cap = {'tex': f, 'color': c[:3] + [round(k, 4)], 'mode': how, 'lit': 1.0}
        elif 'mtoon' in low or not name and ('_ShadeToony' in fl or '_ShadingToonyFactor' in fl):
            if 'mtoon10' in low or '_ShadingToonyFactor' in fl:  # UniVRM's MToon10: VRMC_materials_mtoon's
                toony, shift = clamp(fl.get('_ShadingToonyFactor', 0.9)), clamp(fl.get('_ShadingShiftFactor', 0.0), -1.0)
                lo, hi = step(-1 + toony - shift, 1 - toony - shift)
                shade = {'shade': color('_ShadeColor', (1, 1, 1, 1))[:3], 'base': False, 'tex': image('_ShadeTex'),
                         'lo': lo, 'hi': hi, 'strength': 1.0}
                f = image('_MatcapTex')
                if f:
                    cap = {'tex': f, 'color': color('_MatcapColor', (1, 1, 1, 1))[:3] + [1.0], 'mode': 'add',
                           'lit': round(clamp(fl.get('_RimLightingMix', 1.0)), 4)}
            else:
                toony, shift = clamp(fl.get('_ShadeToony', 0.9)), clamp(fl.get('_ShadeShift', 0.0), -1.0)
                lo, hi = step(shift, shift + (1 - shift) * (1 - toony))
                shade = {'shade': color('_ShadeColor', (0.97, 0.81, 0.86, 1))[:3], 'base': False,
                         'tex': image('_ShadeTexture'), 'lo': lo, 'hi': hi, 'strength': 1.0}
                f = image('_SphereAdd')
                if f:  # added as it is
                    cap = {'tex': f, 'color': [1.0, 1.0, 1.0, 1.0], 'mode': 'add', 'lit': 0.0}
        return shade, cap

    def unlitwf_back(self, m, kw, tex, fl, col):
        """UnlitWF's back faces (_BK_*): its back texture (white when empty; often the main one) times its colour, in
        place of the main texture and colour"""
        if not ('_BK_ENABLE' in kw or not kw and fl.get('_BK_Enable', 0.0) > 0.5):
            return None
        t = tex.get('_BK_BackTex')
        f = self.image(t[0]) if t and ref(t[0])[0] else None
        return {'color': linear_rgba(col.get('_BK_BackColor', (1, 1, 1, 1))), 'tex': f,
                'xf': (t[1], t[2]) if t else ((1.0, 1.0), (0.0, 0.0))}

    @staticmethod
    def alpha_mode(sh, kw, rq, tags, fl, col):
        """cutout or see-through, from what the common VRChat shaders keep in a material"""
        if '_TransparentMode' in fl and ('lil' in sh or not sh):  # lilToon
            return {0: 'OPAQUE', 1: 'MASK', 5: 'MASK'}.get(int(fl['_TransparentMode']), 'BLEND')
        if '_BlendMode' in fl and ('mtoon' in sh or (not sh and '_ShadeColor' in col)):
            return {0: 'OPAQUE', 1: 'MASK'}.get(int(fl['_BlendMode']), 'BLEND')
        if '_Mode' in fl and ('standard' in sh or 'poiyomi' in sh or '.poi' in sh or not sh):
            return {0: 'OPAQUE', 1: 'MASK'}.get(int(fl['_Mode']), 'BLEND')
        if fl.get('_ClippingMode', 0) > 0:  # Unity-chan Toon Shader
            return 'MASK'
        if 'cutout' in sh or 'clipping' in sh or 'alphatest' in sh:
            return 'MASK'
        if 'transparent' in sh or 'fade' in sh:
            return 'BLEND'
        if kw & {'_ALPHABLEND_ON', '_ALPHAPREMULTIPLY_ON', '_SURFACE_TYPE_TRANSPARENT'}:
            return 'BLEND'
        if '_ALPHATEST_ON' in kw:
            return 'MASK'
        if '_Mode' in fl:
            return {0: 'OPAQUE', 1: 'MASK'}.get(int(fl['_Mode']), 'BLEND')
        rt = tags.get('RenderType', '')
        if rq >= 3000 or rt == 'Transparent':
            return 'BLEND'
        if rq >= 2450 or rt == 'TransparentCutout':
            return 'MASK'
        return 'OPAQUE'

    def remaps(self, guid):
        """a model's material remaps: {its material's name: .mat reference}"""
        if guid not in self._remaps:
            out = {}
            for e in listof(self.db.importer(guid).get('externalObjects')):
                e = dictof(e)
                first = dictof(e.get('first'))
                if str(first.get('type', '')).endswith('Material') and first.get('name') is not None:
                    out[str(first['name'])] = e.get('second')
            self._remaps[guid] = out
        return self._remaps[guid]

    def model_material(self, fi, name):
        """what Unity puts in place of a model's own material: a MatInfo, or None for the model's own"""
        r = self.remaps(fi.guid).get(name)
        if r is not None:
            m = self.material(r)
            if m is not None:
                return m
        ms = self.db.importer(fi.guid)
        if inum(ms.get('materialImportMode', ms.get('importMaterials')), 1) == 0:
            return self.default
        if inum(ms.get('materialLocation'), 1) == 0:  # the old way: .mat files in a Materials folder
            a = self.db.get(fi.guid)
            nm = ('%s-%s' % (a.name, name)) if inum(ms.get('materialName')) == 2 else name
            search = inum(ms.get('materialSearch'), 1)
            here = os.path.dirname(a.path)
            for c in self.db.by_name(nm, {'.mat'}, near=a.path):
                d = os.path.dirname(c.path)
                up = os.path.dirname(d)
                if search == 2 or (os.path.basename(d) == 'Materials' and (
                        up == here if search == 0 else (here + '/').startswith(up + '/'))):
                    m = self.material({'fileID': '2100000', 'guid': c.guid})
                    if m is not None:
                        return m
        return None

    def state_material(self, state, r, k):
        """the MatInfo of what a renderer's slot k shows (Analysis.material_state's (material key, property changes));
        None: the model's own material"""
        mk, over = state
        fi = r.fbx
        if mk is None:
            return None
        if mk[0] == 'fbx':
            m = self.model_material(fi, mk[1]) if fi is not None else None
        elif fi is not None and mk[0] == fi.guid:  # the model's own, by its fileID: the slot's
            mesh = fi.info.mesh.get(r.node) or {}
            used = mesh.get('used', [])
            own = mesh['materials'][used[k]] if k < len(used) and used[k] < len(mesh.get('materials', [])) else ''
            m = self.model_material(fi, own)
        else:
            m = self.material({'fileID': str(mk[1]), 'guid': mk[0]})
            if m is None:
                a = self.db.get(mk[0])
                warn('%s: a material it switches to (%s) is not in the input' % (r.gname, a.path if a else mk[0]))
        if m is None or not over:
            return m
        m = copy.copy(m)
        m.name = '%s (%s)' % (m.name, ', '.join(dict.fromkeys(n.split('.')[0] for n, _ in over)))
        col, emit = list(m.color), list(m.emit)
        (sx, sy), (ox, oy) = m.tex_xf
        (ex, ey), (fx, fy) = m.emit_xf
        for name, v in over:
            base, _, ch = name.partition('.')
            if base in COLOR_KEYS and ch in 'rgb':
                col['rgb'.index(ch)] = to_linear(max(v, 0.0))
            elif base in COLOR_KEYS and ch == 'a':
                col[3] = min(max(v, 0.0), 1.0)
            elif base == '_EmissionColor' and ch in 'rgb':
                emit['rgb'.index(ch)] = to_linear(max(v, 0.0))
            elif base == '_EmissionMap_ST':
                ex, ey, fx, fy = [v if c == ch else x for c, x in zip('xyzw', (ex, ey, fx, fy))]
            elif base.endswith('_ST'):
                sx, sy, ox, oy = [v if c == ch else x for c, x in zip('xyzw', (sx, sy, ox, oy))]
            elif base == '_Cutoff':
                m.cutoff = min(max(v, 0.0), 1.0)
        m.color, m.emit = tuple(col), tuple(emit)
        m.tex_xf, m.emit_xf = ((sx, sy), (ox, oy)), ((ex, ey), (fx, fy))
        return m

    def slots(self, r):
        """[(Blender material slot, MatInfo or None)] of a renderer; None keeps the model's own"""
        fi, mid = r.fbx, r.node
        if fi is None or mid not in fi.info.mesh:
            return []
        mesh = fi.info.mesh[mid]
        entries = listof(r.data.get('m_Materials'))
        out = []
        for k, i in enumerate(mesh['used']):
            e = entries[k] if k < len(entries) else None
            own = mesh['materials'][i] if i < len(mesh['materials']) else ''
            m = None
            if isinstance(e, dict) and 'fbxmat' in e:
                m = self.model_material(fi, e['fbxmat'])
            elif isinstance(e, dict):
                f, g = ref(e)
                if g == fi.guid:
                    m = self.model_material(fi, own)
                elif f:
                    m = self.material(e)
                    if m is None:
                        a = self.db.get(g)
                        warn('%s: its material %s is not in the input' % (r.gname, a.path if a else g))
                        m = self.model_material(fi, own)
            out.append((i, m))
        return out


# ---------------------------------------------------------------- unpacked prefabs

def adopt(db, av):
    """GameObjects of the prefab's own that stand for a model's objects (an unpacked prefab): the
    renderers that draw a model's mesh, their bones and the rest by name"""
    fis = {}
    nfbx = [0]

    def inst(guid):
        if guid not in fis:
            info = db.fbx(guid)
            nfbx[0] += 1
            fi = FBXInst('adopted%d' % nfbx[0], guid, info, False, None)
            fi.names = {}
            for mid in info.order:
                fi.names.setdefault(info.name(mid), []).append(mid)
            fis[guid] = fi
        return fis[guid]

    def bind(fi, go, mid):
        if go.fbx is not None or mid in fi.gos:
            return
        go.fbx, go.node = fi, mid
        fi.gos[mid] = go
        for c in go.comps:
            if c.fbx is None:
                c.fbx, c.node = fi, mid
    for r in av.renderers:
        if r.fbx is not None or r.go is None or r.go.fbx is not None:
            continue
        m = r.data.get('m_Mesh') if r.cls == 137 else None
        if r.cls == 23:
            mf = av.comp(r.go, 33)
            m = mf.data.get('m_Mesh') if mf else None
        f, g = ref(m)
        a = db.get(g)
        if a is None:
            if f and g:
                warn('%s: its mesh is not in the input' % r.gname)
            continue
        if a.ext not in MODEL_EXT:
            warn('%s: its mesh is a %s file, which is not converted' % (r.gname, a.ext or 'mesh'))
            continue
        try:
            fi = inst(a.guid)
        except Fail as e:
            warn(str(e))
            continue
        info = fi.info
        # the mesh by its fileID in the old tables, else by the renderer's name
        imp = db.importer(a.guid)
        mesh_name = dictof(imp.get('fileIDToRecycleName')).get(str(f))
        for e in listof(imp.get('internalIDToNameTable')):
            e = dictof(e)
            if inum(dictof(e.get('first')).get('43')) == f:
                mesh_name = e.get('second')
        want = [mesh_name] if mesh_name else []
        want.append(str(r.go.data.get('m_Name', '')))
        mid = None
        for nm in want:
            hits = [x for x in fi.names.get(nm, []) if x in info.mesh and x not in fi.gos]
            if hits:
                mid = hits[0]
                break
        if mid is None:
            nshapes = len(listof(r.data.get('m_BlendShapeWeights')))
            hits = [x for x in info.order if x in info.mesh and x not in fi.gos and
                    len(info.mesh[x]['shapes']) == nshapes and (r.cls == 137) == (
                        info.mesh[x]['skinned'] or bool(info.mesh[x]['shapes']))]
            if len(hits) == 1:
                mid = hits[0]
        if mid is None:
            warn('%s: cannot tell which mesh of %s it draws' % (r.gname, a.path))
            continue
        bind(fi, r.go, mid)
        for b in listof(r.data.get('m_Bones')):
            bg = b.go if isinstance(b, Obj) else None
            if bg is not None and id(bg) in av.inside:
                hits = [x for x in fi.names.get(str(bg.data.get('m_Name', '')), []) if x not in fi.gos]
                # a bone may share its name with a mesh (a Skirt bone and a Skirt mesh)
                hits.sort(key=lambda x: (x in info.mesh, info.kind(x) not in ('LimbNode', 'Limb')))
                if hits:
                    bind(fi, bg, hits[0])
    # the rest of each model: ancestors of what is bound, then anything named as one of its nodes
    for fi in fis.values():
        info = fi.info
        for mid, go in list(fi.gos.items()):
            p, g = info.models[mid][2], go.parent
            while p and g is not None and id(g) in av.inside:
                if g.fbx is None and str(g.data.get('m_Name', '')) == info.name(p):
                    bind(fi, g, p)
                elif g.fbx is not fi or g.node != p:
                    break
                p, g = info.models[p][2], g.parent
        for go in av.gos:
            if go.fbx is None and go.parent is not None and go.parent.fbx is fi:
                pm = go.parent.node
                hits = [x for x in info.kids.get(pm, []) if info.name(x) == str(go.data.get('m_Name', ''))
                        and x not in fi.gos]
                draws = any(c.cls in RENDERERS or c.cls == 33 for c in go.comps)
                hits.sort(key=lambda x: (x in info.mesh) != draws)
                if hits:
                    bind(fi, go, hits[0])
        log('%s: %d of its %d objects are in the prefab, unpacked' % (
            db.get(fi.guid).path, len(fi.gos), len(info.order)))
    return list(fis.values())


# ---------------------------------------------------------------- Modular Avatar
#
# What Modular Avatar (1.18) does to the hierarchy when VRChat builds the avatar, in its order: Merge Armature
# (an outfit's bones join the avatar's, its meshes are weighted to the avatar's bones and the bones left over are
# removed), Bone Proxy, Move To and the PhysBone Blockers they leave. It runs on a copy of the hierarchy; the
# Blender scene is built as the files have it, with every object where MA leaves it, and the GLB gets MA's
# hierarchy afterwards.

# components hypr3d has no use for: Visible Head Accessory keeps an object in VRChat's first person view (hypr3d draws
# the avatar in third person only), Mesh Settings sets the light probe anchor and the bounds of meshes (hypr3d lights
# the avatar by one probe where it stands and never culls its meshes)
MA_QUIET = ('VisibleHeadAccessory', 'MeshSettings')
# of MA_OTHER, these have nothing to do here either: Remove Vertex Color (the GLB has no vertex colours), Sync Parameter
# Sequence (parameter order across PC and Quest uploads), VRChat Settings and MMD Layer Control (VRChat's MMD world
# handling), Rename Collision Tags (contacts, which hypr3d does not have), Move Independently (an editor tool), Convert
# Constraints (it turns Unity's constraints into VRChat's, and this tool converts neither kind) and World Scale Object
# (it keeps an object at the world's scale while a VRChat player scales the avatar; hypr3d scales an avatar only as
# avatar_height asks, and then all of it). World Fixed Object is converted: MA moves the object, as it is at rest, to
# a root its constraint holds at the world's origin (WorldFixedObjectProcessor); the settings file's "fixed" has
# hypr3d hold it in the world where its rest pose was when the avatar appeared (at the world's start, as a VRChat
# world's origin usually is)
MA_WHY = {}


class MergeSpec:
    """one Merge Armature: a component's settings, or those Setup Outfit would give an outfit"""

    def __init__(self, go, target, prefix='', suffix='', mangle=True, comp=None):
        self.go, self.target, self.prefix, self.suffix, self.mangle, self.comp = go, target, prefix, suffix, mangle, comp


def _str(x):
    return x if isinstance(x, str) else ''


class ModularAvatar:
    """the avatar's hierarchy as Modular Avatar leaves it; U, the Unity matrices, is changed to match"""

    def __init__(self, av, human, U, outfits=()):
        self.av, self.U, self.root = av, U, av.root
        self.U0 = {k: M.copy() for k, M in U.items()}  # before MA
        self.byid = {id(g): g for g in av.gos}
        self.parent = {id(g): g.parent if g is not av.root else None for g in av.gos}
        self.kids = {id(g): [c for c in g.children if id(c) in av.inside] for g in av.gos}
        self.nm = {id(g): str(g.data.get('m_Name', '')) for g in av.gos}  # names as MA leaves them
        self.human = human  # Unity human bone name -> GameObject
        self.humans = {id(g) for g in human.values()}
        self.deleted = set()
        self.retarget = {}  # id(merged bone) -> the bone its weights go to
        self.dead = set()  # ids of the PhysBone components MA removes as duplicates
        self.blocks = {}  # id(GameObject) -> the PhysBone Blocker tips under it
        self.vparent = {}  # id(GameObject a Bone Proxy moved) -> its new parent
        self.bound = {}  # id(bone VRCFury linked) -> its world matrix when its meshes were given the avatar's bone
        self.replaced = {}  # Replace Object: id(object or component replaced) -> what replaced it
        self.db = {}  # MA's bone database: id(bone) -> merged (True) or kept (False), in the order added
        self.counts = {'merged': 0, 'removed': 0, 'proxies': 0, 'moves': 0, 'duplicates': 0, 'replaced': 0}
        self._mangled = 0
        comps = {}
        for g in av.gos:
            for c in g.comps:
                k = ma_kind(c)
                if k:
                    comps.setdefault(k, []).append(c)
        self.comps = comps
        # World Fixed Objects: held in the world (Settings' "fixed")
        self.fixed = [g for g in av.gos if id(g) in av.inside and any(
            c.cls == 114 and MA_OTHER.get(c.script()[1] or '') == 'WorldFixedObject' for c in g.comps)]
        self.pbblock = {id(c.go) for c in comps.get('PBBlocker', [])}
        # Scale Adjuster, the first thing MA does to the hierarchy: the meshes weighted to its bone are weighted to a
        # child of it scaled so (in the bone's axes) instead, and the bone's own children keep their size (apply())
        self.scales = {}
        for c in comps.get('ScaleAdjuster', []):
            s = Vector(vec3(c.data.get('m_Scale'), (1.0, 1.0, 1.0)))
            if (s - Vector((1.0, 1.0, 1.0))).length > 1e-6 and id(c.go) in av.inside:
                self.scales[id(c.go)] = s
        other = {}
        for g in av.gos:
            for c in g.comps:
                k = MA_OTHER.get(c.script()[1] or '') if c.cls == 114 else None
                if k in MA_WHY:
                    other[k] = other.get(k, 0) + 1
        for k, n in other.items():
            warn('%d Modular Avatar %s component(s): not converted (%s)' % (
                n, re.sub(r'(?<=[a-z])(?=[A-Z])', ' ', k), MA_WHY[k]))
        # MA reads every object reference before it changes anything
        self.ref = {id(c): self.objref(c.data.get({'MergeArmature': 'mergeTarget', 'MoveTo': 'target',
                                                   'ReplaceObject': 'targetObject'}[k]))
                    for k in ('MergeArmature', 'MoveTo', 'ReplaceObject') for c in comps.get(k, [])}
        taken = {id(m.comp) for m in outfits if m.comp is not None}  # Setup Outfit's, retargeted
        merges = [MergeSpec(c.go, self.ref[id(c)], _str(c.data.get('prefix')), _str(c.data.get('suffix')),
                            truthy(c.data.get('mangleNames', '1')), c) for c in comps.get('MergeArmature', [])
                  if id(c) not in taken]
        merges += list(outfits)
        proxies = comps.get('BoneProxy', [])
        self.proxy_target = {id(p): self.proxy_resolve(p) for p in proxies}  # BoneProxyPrepass
        if merges:
            self.merge_armatures(merges, proxies)
        if proxies:
            self.bone_proxies(proxies)
        if comps.get('MoveTo'):
            self.move_tos()
        if comps.get('ReplaceObject'):
            self.replace_objects()
        if self.pbblock:
            self.blockers()
        # a removed bone moves with the one its children went to
        for k in self.deleted:
            d = self.retarget[k]
            self.U[k] = self.U[id(d)] @ self.U0[id(d)].inverted_safe() @ self.U0[k]
        n, said = self.counts, []
        if merges:
            said.append('%d Merge Armature%s (%d bones joined the avatar\'s, %d of them removed)' % (
                len(merges), '' if len(merges) == 1 else 's', n['merged'], n['removed']))
        if n['proxies']:
            said.append('%d Bone Prox%s' % (n['proxies'], 'y' if n['proxies'] == 1 else 'ies'))
        if n['moves']:
            said.append('%d Move To' % n['moves'])
        if n['replaced']:
            said.append('%d Replace Object' % n['replaced'])
        if n['duplicates']:
            said.append('%d duplicate PhysBone%s dropped' % (n['duplicates'], '' if n['duplicates'] == 1 else 's'))
        if said:
            log('Modular Avatar: ' + ', '.join(said))

    # ---- the hierarchy

    def up(self, g):
        """a GameObject's parent as MA leaves it"""
        return self.parent[id(g)] if id(g) in self.parent else g.parent

    def down(self, g):
        """its children as MA leaves them"""
        return self.kids[id(g)] if id(g) in self.kids else [c for c in g.children if id(c) in self.av.inside]

    def name(self, g):
        return self.nm.get(id(g), '')

    def find(self, t, path):
        """Transform.Find: a path of child names under t"""
        for part in path.split('/'):
            if part == '' or t is None:
                continue
            if part == '..':
                t = self.parent.get(id(t))
            else:
                t = next((c for c in self.kids.get(id(t), []) if self.nm[id(c)] == part), None)
        return t

    def under(self, g, a):
        """is g a or under it"""
        while g is not None:
            if g is a:
                return True
            g = self.parent.get(id(g))
        return False

    def set_parent(self, g, p):
        """SetParent(p, worldPositionStays): the world matrix stays, so U does too"""
        old = self.parent[id(g)]
        if old is p:
            return True
        if self.under(p, g):
            warn('%s cannot go under %s, which is under it' % (self.nm[id(g)], self.nm[id(p)]))
            return False
        if old is not None:
            self.kids[id(old)].remove(g)
        self.parent[id(g)] = p
        self.kids[id(p)].append(g)
        return True

    def preorder(self):
        out, stack = [], [self.root]
        while stack:
            g = stack.pop()
            out.append(g)
            stack += reversed(self.kids[id(g)])
        return out

    def path(self, g):
        """the path from the avatar root, as animations name objects"""
        parts = []
        while g is not None and g is not self.root:
            parts.append(self.nm[id(g)])
            g = self.parent[id(g)]
        return '/'.join(reversed(parts))

    def place(self, g, M):
        """give g the world matrix M, and its children with it"""
        D = M @ self.U[id(g)].inverted_safe()
        self.U[id(g)] = M
        stack = list(self.kids[id(g)])
        while stack:
            x = stack.pop()
            self.U[id(x)] = D @ self.U[id(x)]
            stack += self.kids[id(x)]

    def objref(self, r, now=False):
        """AvatarObjectReference.Get: the GameObject a reference names (MA resolves them before it changes anything;
        now: again, for one whose object is gone)"""
        r = dictof(r)
        path = r.get('referencePath')
        if not isinstance(path, str) or not path:
            return None
        t = r.get('targetObject')
        if isinstance(t, Obj):
            t = t if t.cls == 1 else t.go
            if t is not None and id(t) in self.av.inside and not (now and id(t) in self.deleted):
                return t
        if path == '$$$AVATAR_ROOT$$$':
            return self.root
        g = self.find(self.root, path)
        # avatars with an empty "Armature" beside the real one (to move VRChat's view point) mean the real one
        p = self.parent.get(id(g)) if g is not None else None
        if g is not None and self.nm[id(g)] == 'Armature' and p is not None and not self.kids[id(g)]:
            g = next((s for s in self.kids[id(p)] if self.nm[id(s)] == 'Armature' and self.kids[id(s)]), g)
        return g

    # ---- Merge Armature

    def physbones(self, g):
        return [c for c in g.comps if is_physbone(c) and id(c) not in self.dead]

    @staticmethod
    def pb_root(c):
        """a PhysBone's root transform as set, or None"""
        t = c.data.get('rootTransform')
        return t.go if isinstance(t, Obj) and t.go is not None else None

    @staticmethod
    def refs(c, key):
        return [x.go for x in listof(c.data.get(key)) if isinstance(x, Obj) and x.go is not None]

    def add_bone(self, g):
        self.db[id(g)] = True

    def retain(self, g):
        if g is not None and id(g) in self.db:
            self.db[id(g)] = False

    def dest(self, g):
        """BoneDatabase.GetRetargetedBone: the bone a merged bone's weights go to, None if it keeps them"""
        if g is None or id(g) not in self.db:
            return None
        while g is not None and self.db.get(id(g)) is True:
            g = self.parent[id(g)]
        return None if g is None or id(g) in self.db else g

    def similar(self, a, b, ignores):
        """IsSimilarChainInPosition: a is where b is (within a millimetre), and so are their children of one name"""
        if (self.U[id(b)].translation - self.U[id(a)].translation).length_squared > 1e-6:
            return False
        for c in self.kids[id(a)]:
            if id(c) in ignores:
                continue
            t = self.find(b, self.nm[id(c)])
            if t is not None and not self.similar(c, t, ignores):
                return False
        return True

    def merge_armatures(self, merges, proxies):
        av = self.av
        self.pb_at = {}  # physBoneByRootBone: the PhysBone that starts at a bone, the last one found
        for g in av.gos:
            for c in g.comps:
                if is_physbone(c):
                    self.pb_at[id(self.pb_root(c) or g)] = c
        by_go = {id(m.go): m for m in merges}
        self.merging = set(by_go)
        # a merge whose target is inside another merge's hierarchy goes first
        before = {}
        for m in merges:
            if m.target is None:
                warn('%s: its Merge Armature has no target in the avatar, so it is not merged' % self.nm[id(m.go)])
                continue
            g = m.target
            while g is not None and id(g) not in by_go:
                g = self.parent[id(g)]
            if g is not None:
                before.setdefault(id(by_go[id(g)]), []).append(m)
        done, stack = set(), []

        def topo(m):
            if id(m) in done:
                return
            if any(x is m for x in stack):
                warn('the Merge Armatures of %s merge into each other' % self.nm[id(m.go)])
                return
            stack.append(m)
            if m.target is not None:
                for p in before.get(id(m), []):
                    topo(p)
                self.merge(m)
            stack.pop()
            done.add(id(m))
        for m in merges:
            topo(m)
        # what has to stay: bone proxies and their targets, and every bone a PhysBone, collider, contact or
        # constraint names
        for p in proxies:
            self.retain(p.go)
            self.retain(self.proxy_target.get(id(p)))
        for g in av.gos:
            for c in g.comps:
                if id(c) in self.dead:
                    continue
                if is_physbone(c) or is_pb_collider(c) or is_contact(c):
                    if self.pb_root(c) is None:
                        self.retain(g)
                elif not is_constraint(c):
                    continue
                self.retain_refs(c)
        # and a mesh's root bone that is off its parent, or scaled the other way
        for r in av.renderers:
            rb = r.data.get('m_RootBone') if r.cls == 137 else None
            g = rb.go if isinstance(rb, Obj) else None
            if g is None or id(g) not in self.parent or self.parent[id(g)] is None:
                continue
            p = self.parent[id(g)]
            s1, s0 = self.local_scale(g), self.local_scale(p)
            if ((self.U[id(p)].translation - self.U[id(g)].translation).length_squared > 1e-6 or
                    s1.length < 1e-9 or s0.length < 1e-9 or s0.normalized().dot(s1.normalized()) < 0.9999):
                self.retain(g)
        self.retarget_meshes()

    def local_scale(self, g):
        p = self.parent[id(g)]
        M = self.U[id(g)] if p is None else self.U[id(p)].inverted_safe() @ self.U[id(g)]
        return M.decompose()[2]

    def retain_refs(self, c):
        """RetainBoneReferences: every Transform and GameObject a component names"""
        def walk(x):
            if isinstance(x, Obj):
                self.retain(x.go if x.cls in TRANSFORMS else x if x.cls == 1 else None)
            elif isinstance(x, dict):
                for k, v in x.items():
                    if k != 'm_GameObject':
                        walk(v)
            elif isinstance(x, list):
                for v in x:
                    walk(v)
        walk(c.data)

    def merge(self, m):
        t = m.target
        while self.db.get(id(t)) is True:
            t = self.parent[id(t)]
        self.prune = []  # prunePBsObjects
        self.added = set()  # thisPassAdded
        self.rmerge(m, m.go, t, True)
        self.prune_physbones()

    def rmerge(self, m, src, newp, zip_):
        """RecursiveMerge: src goes under newp; if zipping, its children go under newp's children of their names"""
        if src is newp:
            return
        if zip_:
            self.added.add(id(src))
        zip_ = zip_ and not any(is_unity_constraint(c) for c in src.comps)
        self.set_parent(src, newp)
        if m.mangle:
            self._mangled += 1
            self.nm[id(src)] += '$%d' % self._mangled
        self.pbblock.add(id(src))
        blocked = None
        pb = self.pb_at.get(id(src))
        if pb is not None and not self.similar(src, newp, {id(x) for x in self.refs(pb, 'ignoreTransforms')}):
            blocked = {id(x) for x in self.refs(pb, 'ignoreTransforms')}
        elif zip_:
            self.prune.append(src)
        if zip_ and blocked is None:
            self.add_bone(src)
            self.counts['merged'] += 1
        if not zip_:
            return
        reported = False
        for child in list(self.kids[id(src)]):
            if id(child) in self.merging:
                continue
            name, pre, suf = self.nm[id(child)], m.prefix, m.suffix
            to, zip2 = src, False
            if name.startswith(pre) and name.endswith(suf) and len(name) > len(pre) + len(suf):
                target = self.find(newp, name[len(pre):len(name) - len(suf)])
                if blocked is not None and id(child) not in blocked and id(child) not in self.pbblock:
                    # under a PhysBone of the outfit's own: it stays with that PhysBone
                    if not reported and target is not None and id(target) in self.humans:
                        warn('%s: a PhysBone of the outfit moves %s, a humanoid bone (Modular Avatar stops the build '
                             'here)' % (self.nm[id(m.go)].split('$')[0], name.split('$')[0]))
                        reported = True
                    continue
                if target is not None:
                    to, zip2 = target, True
            self.rmerge(m, child, to, zip2)

    def prune_physbones(self):
        """PruneDuplicatePhysBones: an outfit's copy of a PhysBone the avatar already has on that bone goes"""
        def origin(g):
            while g is not None and id(g) in self.added:
                g = self.parent[id(g)]
            return g
        for obj in dict.fromkeys(self.prune):
            pbs = self.physbones(obj)
            base = origin(obj) if pbs else None
            if base is None or not self.physbones(base):
                continue
            targets = {id(self.pb_root(c) or base) for c in self.physbones(base)}
            for c in pbs:
                r = self.pb_root(c)
                t = base if r is None else origin(r)
                if t is not None and id(t) in targets:
                    self.dead.add(id(c))
                    self.counts['duplicates'] += 1
                else:
                    self.retain(obj)

    def unknown(self, g):
        """does a bone have a component MA does not remove with it (anything but a Transform or an MA component)"""
        return any(c.cls not in TRANSFORMS and id(c) not in self.dead and not ma_kind(c) for c in g.comps)

    def retarget_meshes(self):
        """RetargetMeshes: meshes weighted to merged bones are weighted to the bones they merged into, and the
        merged bones that nothing else needs go, their children moving to those bones"""
        for k, v in self.db.items():
            d = self.dest(self.byid[k]) if v else None
            if d is not None:
                self.retarget[k] = d
        for k in list(self.db):
            if self.db[k] is not True:
                continue
            g = self.byid[k]
            d = self.dest(g)
            if d is None or self.unknown(g):
                continue
            for c in list(self.kids[k]):
                self.set_parent(c, d)
            self.kids[id(self.parent[k])].remove(g)
            self.parent[k] = None
            self.deleted.add(k)
            self.retarget[k] = d
            self.counts['removed'] += 1

    # ---- Bone Proxy

    def proxy_resolve(self, p):
        """ModularAvatarBoneProxy.target: a humanoid bone, or a path under one or under the avatar root"""
        d = p.data
        bone = inum(d.get('boneReference'), 55)
        sub = _str(d.get('subPath'))
        if bone == 55 and not sub.strip():
            return None
        if sub == '$$AVATAR':
            return self.root
        if bone == 55:
            return self.find(self.root, sub)
        g = self.human.get(HUMAN_BONES[bone]) if 0 <= bone < len(HUMAN_BONES) else None
        if g is None or id(g) not in self.parent:
            return None
        return g if not sub.strip() else self.find(g, sub)

    def bone_proxies(self, proxies):
        mine = {id(p) for p in proxies}
        info = []
        for g in self.preorder():
            for p in g.comps:
                if id(p) in mine:
                    t = self.proxy_target.get(id(p)) or self.proxy_resolve(p)
                    info.append((p, g, t, self.U[id(g)].copy()))
        for p, g, t, W in info:
            if t is None or id(t) in self.deleted:
                warn('%s: its Bone Proxy\'s target is not in the avatar' % self.nm[id(g)])
                continue
            if self.under(t, g):
                warn('%s: its Bone Proxy\'s target is under it' % self.nm[id(g)])
                continue
            name, sfx, i = self.nm[id(g)], '', 1
            while self.find(t, name + sfx) is not None:
                sfx = ' (%d)' % i
                i += 1
            self.nm[id(g)] = name + sfx
            if self.set_parent(g, t):
                self.vparent[id(g)] = t
                self.counts['proxies'] += 1
        # parents first, so that a proxy keeping its world pose keeps it under a moved one
        for p, g, t, W in sorted(info, key=lambda x: self.path(x[1])):
            d = p.data
            mode = inum(d.get('attachmentMode'))
            par = self.parent[id(g)]
            P = self.U[id(par)] if par is not None else Matrix.Identity(4)
            Pi = P.inverted_safe()
            _, _, s = (Pi @ self.U[id(g)]).decompose()
            pos = Pi @ W.translation if mode in (2, 4) else Vector((0.0, 0.0, 0.0))
            rot = P.decompose()[1].inverted() @ W.decompose()[1] if mode in (2, 3) else Quaternion()
            if truthy(d.get('matchScale', '0')):
                s = Vector((1.0, 1.0, 1.0))
            self.place(g, P @ Matrix.LocRotScale(pos, rot, s))

    # ---- Move To

    def move_tos(self):
        mine = {id(c) for c in self.comps['MoveTo']}
        for g in self.preorder():
            for c in g.comps:
                if id(c) not in mine:
                    continue
                t = self.ref.get(id(c))
                if t is not None and id(t) in self.deleted:
                    t = self.objref(c.data.get('target'), now=True)
                if t is None:
                    warn('%s: its Move To has no target in the avatar' % self.nm[id(g)])
                    continue
                d = c.data
                par = self.parent[id(g)]
                P = self.U[id(par)] if par is not None else Matrix.Identity(4)
                Pi = P.inverted_safe()
                T = self.U[id(t)]
                pos, rot, s = (Pi @ self.U[id(g)]).decompose()
                if truthy(d.get('matchPosition', '1')):
                    pos = Pi @ T.translation
                if truthy(d.get('matchRotation', '1')):
                    rot = P.decompose()[1].inverted() @ T.decompose()[1]
                if truthy(d.get('matchScale', '0')):
                    X = Matrix.LocRotScale(pos, rot, None) @ Pi @ T
                    s = Vector([X.col[i].xyz.length for i in range(3)])
                self.place(g, P @ Matrix.LocRotScale(pos, rot, s))
                self.counts['moves'] += 1

    # ---- Replace Object

    def replace_objects(self):
        """ReplaceObjectPass: the component's object takes its target's place (its parent, children and place among
        its siblings, where the world has it), and the target goes; what named the target or its components names the
        replacement or its components of that type (the n-th of the n-th), animations included"""
        reps = {}
        for c in self.comps['ReplaceObject']:
            t, g = self.ref.get(id(c)), c.go
            if t is None or id(t) in self.deleted:
                warn('%s: its Replace Object has no target in the avatar' % self.nm[id(g)])
                continue
            if self.under(g, t):
                warn('%s: its Replace Object\'s target (%s) is above it' % (self.nm[id(g)], self.nm[id(t)]))
                continue
            if id(t) in reps:
                warn('%s: %s is replaced by %s already' % (self.nm[id(g)], self.nm[id(t)], self.nm[id(reps[id(t)][1])]))
                continue
            reps[id(t)] = (t, g)
        for t, g in reps.values():
            par = self.parent[id(t)]
            if par is None or not self.set_parent(g, par):
                continue
            self.vparent[id(g)] = par  # shown and hidden with its new parents
            for ch in list(self.kids[id(t)]):
                if self.set_parent(ch, g):
                    self.vparent[id(ch)] = g
            sib = self.kids[id(par)]
            sib.remove(g)
            sib.insert(sib.index(t), g)
            sib.remove(t)
            self.parent[id(t)] = None
            self.deleted.add(id(t))
            self.retarget[id(t)] = g
            self.bound[id(t)] = self.U[id(g)].copy()  # its meshes go to the replacement as they were bound
            self.replaced[id(t)] = g
            seen = {}
            for comp in t.comps:  # its components: the replacement's of their type, in their order
                key = (comp.cls, comp.script())
                n = seen.get(key, 0)
                seen[key] = n + 1
                same = [x for x in g.comps if (x.cls, x.script()) == key]
                if n < len(same):
                    self.replaced[id(comp)] = same[n]
            self.counts['replaced'] += 1
        if not self.replaced:
            return
        rep = self.replaced

        def fix(x, depth=0):
            if isinstance(x, Obj):
                return rep.get(id(x), x)
            if isinstance(x, dict) and depth < 40:
                for k, v in x.items():
                    if k != 'm_GameObject':
                        x[k] = fix(v, depth + 1)
            elif isinstance(x, list) and depth < 40:
                for i, v in enumerate(x):
                    x[i] = fix(v, depth + 1)
            return x
        for g in self.av.gos:
            if id(g) not in self.deleted:
                for comp in g.comps:
                    if comp.cls not in TRANSFORMS:
                        fix(comp.data)
        for k, g in list(self.human.items()):
            self.human[k] = rep.get(id(g), g)

    def swap(self, pr):
        """a property as animations reach it once Replace Object has run; None if the replacement has no such"""
        if self.replaced and id(pr[1]) in self.replaced:
            r = self.replaced[id(pr[1])]
            if pr[0] == 's' and pr[2] not in self.av.shape_names(r) or pr[0] == 'c':
                return None
            return (pr[0], r) + tuple(pr[2:])
        return pr

    # ---- PhysBone Blocker

    def blockers(self):
        """every PhysBone that starts above a blocker ignores it (and all under it)"""
        for g in self.preorder():
            if id(g) not in self.pbblock:
                continue
            x = self.parent[id(g)]
            while x is not None:
                self.blocks.setdefault(id(x), []).append(g)
                if x is self.root:
                    break
                x = self.parent[id(x)]

    def ignores(self, c, root):
        """what MA adds to a PhysBone's ignore list"""
        return self.blocks.get(id(root), []) if 'pull' in c.data else []

    # ---- the GLB

    def apply(self, js, binc):
        """the exported GLB as MA leaves the avatar: nodes under their new parents (where they are stays), skins
        weighted to the bones their bones merged into (and scaled by the Scale Adjusters), and the merged bones that
        went removed"""
        av = self.av
        moved = [g for g in av.gos if id(g) not in self.deleted and self.parent[id(g)] is not (
            g.parent if g is not self.root else None)]
        # a Scale Adjuster does something only on a bone some mesh is weighted to
        joint_names = {js['nodes'][j].get('name') for sk in js.get('skins', []) for j in sk.get('joints', [])
                       if 0 <= j < len(js.get('nodes', []))}
        scaled = [k for k in self.scales if self.byid[k].name and self.byid[k].name in joint_names]
        if scaled:
            log('Modular Avatar: %d Scale Adjuster%s scale%s the meshes weighted to %s' % (
                len(scaled), '' if len(scaled) == 1 else 's', 's' if len(scaled) == 1 else '',
                ', '.join(sorted(self.byid[k].name for k in scaled))))
        if not (moved or self.deleted or self.retarget or scaled):
            return binc
        nodes = js.get('nodes', [])
        parent, W = gltf_tree(js)
        index = {}
        for i, n in enumerate(nodes):
            if n.get('name') is not None:
                index.setdefault(n['name'], i)
        node_of, go_of = {}, {}
        for g in av.gos:
            if g.name and g.name in index:
                node_of[id(g)] = index[g.name]
                go_of[index[g.name]] = g
        gone = {node_of[k] for k in self.deleted if k in node_of}

        def anchor(g):
            """the node of g, or of the nearest of its parents with one, as MA leaves them"""
            while g is not None:
                if id(g) in self.deleted:
                    g = self.retarget[id(g)]
                    continue
                if id(g) in node_of:
                    return node_of[id(g)]
                g = self.parent.get(id(g))
            return -1

        def old_anchor(g):
            g = g.parent if g is not self.root else None
            while g is not None and id(g) in av.inside:
                if id(g) in node_of:
                    return node_of[id(g)]
                g = g.parent
            return -1
        # (a) new parents, keeping every node where it is
        par = list(parent)
        roots = js.setdefault('scenes', [{'nodes': []}])[js.get('scene', 0)].setdefault('nodes', [])
        for i in range(len(nodes)):
            if i in gone:
                continue
            g = go_of.get(i)
            if g is not None:
                new = anchor(self.parent[id(g)])
                if new == old_anchor(g) and par[i] not in gone:
                    continue
            elif par[i] in gone:
                new = anchor(go_of[par[i]])
            else:
                continue
            if new == par[i]:
                continue
            x = new
            while x >= 0 and x != i:
                x = par[x]
            if x == i:
                warn('%s cannot go under %s, which is under it' % (nodes[i].get('name'), nodes[new].get('name')))
                continue
            if par[i] >= 0:
                nodes[par[i]]['children'].remove(i)
            elif i in roots:
                roots.remove(i)
            if new >= 0:
                nodes[new].setdefault('children', []).append(i)
            else:
                roots.append(i)
            par[i] = new
            L = (W[new].inverted_safe() if new >= 0 else Matrix.Identity(4)) @ W[i]
            t, q, s = L.decompose()
            for k in ('matrix', 'translation', 'rotation', 'scale'):
                nodes[i].pop(k, None)
            if t.length > 1e-9:
                nodes[i]['translation'] = list(t)
            if abs(q.w) < 1 - 1e-9:
                nodes[i]['rotation'] = [q.x, q.y, q.z, q.w]
            if any(abs(v - 1) > 1e-9 for v in s):
                nodes[i]['scale'] = list(s)
        # (b) skin joints on merged bones go to the bones they merged into; the inverse bind matrices change so
        # that the mesh stays where it is
        out = bytearray(binc)
        for sk in js.get('skins', []):
            joints = sk.get('joints', [])
            ibm = self.read_mat4(js, binc, sk.get('inverseBindMatrices'), len(joints))
            changed = False
            for j, n in enumerate(joints):
                g = go_of.get(n)
                if g is None or (id(g) not in self.retarget and id(g) not in self.scales):
                    continue
                m, Lc = n, None
                if id(g) in self.retarget:
                    m = anchor(self.retarget[id(g)])
                    if m < 0:
                        continue
                    d = self.retarget[id(g)]
                    # where MA's retargeting left the bone: moving with the bone it merged into since then
                    V = self.bound.get(id(g))
                    if V is None:
                        V = self.U[id(d)] @ self.U0[id(d)].inverted_safe() @ self.U0[id(g)]
                    Lc = FLIP @ V @ self.U[id(g)].inverted_safe() @ FLIP
                if id(g) in self.scales:  # first scaled about the bone, in its axes, as the Scale Adjuster's proxy does
                    Ug = self.U[id(g)]
                    X = FLIP @ Ug @ Matrix.Diagonal(self.scales[id(g)]).to_4x4() @ Ug.inverted_safe() @ FLIP
                    Lc = X if Lc is None else Lc @ X
                ibm[j] = W[m].inverted_safe() @ Lc @ W[n] @ ibm[j]
                joints[j] = m
                changed = True
            if changed:
                while len(out) % 4:
                    out.append(0)
                off = len(out)
                for M in ibm:
                    out += struct.pack('<16f', *[M[r][c] for c in range(4) for r in range(4)])
                js.setdefault('bufferViews', []).append({'buffer': 0, 'byteOffset': off, 'byteLength': 64 * len(ibm)})
                js.setdefault('accessors', []).append({'bufferView': len(js['bufferViews']) - 1, 'componentType': 5126,
                                                       'count': len(ibm), 'type': 'MAT4'})
                sk['inverseBindMatrices'] = len(js['accessors']) - 1
            sk.pop('skeleton', None)
        if js.get('buffers'):
            js['buffers'][0]['byteLength'] = len(out)
        # (c) the merged bones that went
        if gone:
            keep = [i for i in range(len(nodes)) if i not in gone]
            new_index = {o: k for k, o in enumerate(keep)}
            for n in nodes:
                if 'children' in n:
                    n['children'] = [new_index[c] for c in n['children'] if c in new_index]
                    if not n['children']:
                        del n['children']
            for sc in js.get('scenes', []):
                sc['nodes'] = [new_index[c] for c in sc.get('nodes', []) if c in new_index]
            for sk in js.get('skins', []):
                sk['joints'] = [new_index[j] if j in new_index else new_index.get(anchor(go_of.get(j)), 0)
                                for j in sk.get('joints', [])]
            js['nodes'] = [nodes[i] for i in keep]
        for k in self.deleted:
            self.byid[k].name = None
        return bytes(out)

    @staticmethod
    def read_mat4(js, binc, acc, count):
        """a skin's inverse bind matrices (identities when it has none)"""
        out = [Matrix.Identity(4) for _ in range(count)]
        if acc is None:
            return out
        a = js['accessors'][acc]
        bv = js['bufferViews'][a['bufferView']]
        off = bv.get('byteOffset', 0) + a.get('byteOffset', 0)
        stride = bv.get('byteStride', 64)
        for j in range(min(count, a.get('count', 0))):
            f = struct.unpack_from('<16f', binc, off + j * stride)
            out[j] = Matrix([[f[c * 4 + r] for c in range(4)] for r in range(4)])
        return out


# ---------------------------------------------------------------- VRCFury
#
# What VRCFury does when VRChat builds the avatar, as far as hypr3d carries it. VRCFury runs after Modular Avatar (NDMF's
# build comes first), so it sees the hierarchy MA leaves. A VRCFury component holds one feature as a [SerializeReference]
# ("content", or a list in the older "config.features") kept in the component's "references" block. This emulates what
# VRCFury does with them. It was written after reading VRCFury's source for its serialized format and its behaviour,
# and it contains none of VRCFury's code.
#
# - Armature Link: an outfit's bones ("Link From") are linked to an avatar bone ("Link To": a humanoid bone, falling back
#   to the ones above it, an object or a path), and with "recursive" their children to the avatar's children of the
#   same names, less a suffix. Each linked bone may be snapped onto the avatar's, goes under it, and the meshes weighted
#   to it are weighted to the avatar's bone instead (not for bones a PhysBone moves); what nothing uses is removed.
# - Toggle: a menu toggle (its name is a menu path) that turns objects on or off, sets shape keys and plays clips,
#   exclusive with the toggles that share a tag. An object a toggle turns on rests off, and one it turns off rests on.
# - Full Controller: an FX controller, menus (under a prefix) and parameters merged in, the parameters renamed apart
#   unless global; animation paths are looked up from the component's object up to the avatar's root.
# - Blend Shape Link: meshes whose shape keys follow a base mesh's (by name, loosely), at rest and when animated.
# - Apply During Upload, Delete During Upload, and the older Modes, Object State and Bone Constraint features, as
#   VRCFury upgrades them.

VRCF_GUID = 'd9e94e501a2d4c95bff3d5601013d923'  # the VRCFury component (VF.Model.VRCFury)
VRCF_DONE_ORDER = ('ArmatureLink', 'Toggle', 'FullController', 'BlendShapeLink', 'ApplyDuringUpload',
                   'DeleteDuringUpload', 'GestureDriver', 'SenkyGestureDriver', 'Blinking', 'Visemes', 'Puppet',
                   'MoveMenuItem', 'ReorderMenuItem')
VRCF_DONE = set(VRCF_DONE_ORDER)
# features that change nothing hypr3d shows: left out without a word
VRCF_QUIET = {
    # how VRChat builds, draws and syncs the avatar
    'AnchorOverrideFix', 'AnchorOverrideFix2', 'BoundingBoxFix', 'BoundingBoxFix2', 'BlendshapeOptimizer',
    'DirectTreeOptimizer', 'FixWriteDefaults', 'MakeWriteDefaultsOff', 'MakeWriteDefaultsOff2', 'Slot4Fix',
    'UnlimitedParameters', 'DescriptorDebug', 'Gizmo', 'SetIcon', 'OverrideMenuSettings', 'MmdCompatibility',
    # VRChat's first person view and eye tracking (hypr3d draws the avatar in third person only, eyes by bones)
    'CrossEyeFix', 'CrossEyeFix2', 'ShowInFirstPerson', 'HeadChopHead',
    # the avatar's scale menu (hypr3d: avatar_height), toes and talking (no tracking or voice here)
    'AvatarScale', 'AvatarScale2', 'Toes', 'Talking',
    # a PIN code in the menu: hypr3d has none, so every toggle works as if it had been entered
    'SecurityLock', 'SecurityRestricted',
    # colliders for other players' PhysBones and VRChat's hand colliders (hypr3d has neither)
    'AdvancedCollider',
    # VRChat's own blinking taken away (said only when no Blinking feature takes its place)
    'RemoveBlinking',
    'TpsScaleFix', 'SpsOptions'}
# VRCFury's SenkyGestureDriver, as it builds it: the sign of either hand shows some of its states, and each sign has a
# lock toggle in an "Emote Lock" menu; states of the eyes stop the blinking
SENKY = ((7, 'Happy', ('eyesHappy', 'mouthHappy')), (6, 'Sad', ('eyesSad', 'mouthSad', 'earsBack')),
         (5, 'Angry', ('eyesAngry', 'mouthAngry', 'earsBack')), (4, 'Tongue', ('mouthBlep',)))
# why the rest are not converted, where that is not obvious
VRCF_WHY = {'SPS': 'hypr3d has no contacts or haptics', 'TPSIntegration': 'hypr3d has no contacts or haptics',
            'TPSIntegration2': 'hypr3d has no contacts or haptics', 'OGBIntegration': 'hypr3d has no contacts or haptics',
            'OGBIntegration2': 'hypr3d has no contacts or haptics', 'ZawooIntegration': 'hypr3d has no contacts',
            'RemoveHandGestures': 'hypr3d curls the fingers for gestures itself',
            'RemoveHandGestures2': 'hypr3d curls the fingers for gestures itself',
            'ConstraintRetarget': 'constraints are not converted'}
# VRChat's own animator parameters, which a Full Controller never renames
VRC_PARAMS = set(('IsLocal Viseme Voice GestureLeft GestureRight GestureLeftWeight GestureRightWeight AngularY VelocityX '
                  'VelocityY VelocityZ VelocityMagnitude Upright Grounded Seated AFK TrackingType VRMode MuteSelf '
                  'InStation Earmuffs IsOnFriendsList AvatarVersion IsAnimatorEnabled ScaleModified ScaleFactor '
                  'ScaleFactorInverse EyeHeightAsMeters EyeHeightAsPercent PreviewMode').split())


def is_vrcfury(c):
    if c.cls != 114:
        return False
    if c.script()[1] == VRCF_GUID:
        return True
    d = c.data
    return 'somethingIsBroken' in d and ('content' in d or 'config' in d)


def vrcf_refs(d):
    """{id: (class, data)} of a component's [SerializeReference] objects: newer Unity keeps them as
    references: {version: 2, RefIds: [{rid, type: {class, ns, asm}, data}]}, Unity 2019 as {version: 1, <id>: ...}"""
    refs = dictof(d.get('references'))
    out = {}
    for e in listof(refs.get('RefIds')):
        e = dictof(e)
        out[inum(e.get('rid'), -2)] = (_str(dictof(e.get('type')).get('class')), dictof(e.get('data')))
    for k, e in refs.items():
        if k not in ('version', 'RefIds') and re.fullmatch(r'-?\d+', str(k)):
            e = dictof(e)
            out[int(k)] = (_str(dictof(e.get('type')).get('class')), dictof(e.get('data')))
    return out


def vrcf_deref(x, refs, depth=0):
    """a value with its references ({rid: n}, or {id: n} in the old layout) replaced by what they name, as
    {'@class': class, field: value...}; a null or missing reference is None"""
    if depth > 40:
        return None
    if isinstance(x, dict):
        if x and set(x) <= {'rid', 'id'}:
            e = refs.get(inum(x.get('rid', x.get('id')), -2))
            if e is None:
                return None
            out = {'@class': e[0]}
            for k, v in e[1].items():
                out[k] = vrcf_deref(v, refs, depth + 1)
            return out
        return {k: vrcf_deref(v, refs, depth + 1) for k, v in x.items()}
    if isinstance(x, list):
        return [vrcf_deref(v, refs, depth + 1) for v in x]
    return x


_VRCF_FEATURES = {}


def vrcf_features(c):
    """a VRCFury component's features, as VRCFury's upgrades leave them: [{'@class': ..., field: value...}]"""
    if id(c) not in _VRCF_FEATURES:
        d = c.data
        refs = vrcf_refs(d)
        out = []
        for r in [d.get('content')] + listof(dictof(d.get('config')).get('features')):
            f = vrcf_deref(r, refs)
            if isinstance(f, dict) and f.get('@class'):
                if f['@class'] == 'WorldConstraint':
                    f['@go'] = c.go  # what it leaves in the world
                out += vrcf_upgrade(f)
        _VRCF_FEATURES[id(c)] = out
    return _VRCF_FEATURES[id(c)]


def vrcf_upgrade(f):
    """what VRCFury's Upgrade and Migrate steps make of a feature saved by an older version: [features]"""
    cls, v = f['@class'], inum(f.get('version'), -1)
    if cls == 'Modes':  # every mode a toggle, the modes exclusive
        name = _str(f.get('name'))
        tag = 'mode_' + name.replace(' ', '').replace('/', '').strip()
        out = []
        for i, m in enumerate(listof(f.get('modes')), 1):
            out += vrcf_upgrade({'@class': 'Toggle', 'version': 0, 'name': '%s/Mode %d' % (name, i),
                                 'saved': f.get('saved'), 'state': dictof(m).get('state'), 'enableExclusiveTag': '1',
                                 'exclusiveTag': tag})
        return out
    if cls == 'ObjectState':  # objects turned on or off, or deleted, during the upload
        acts, out = [], []
        for s in listof(f.get('states')):
            s = dictof(s)
            o, a = s.get('obj'), inum(s.get('action'))
            if not isinstance(o, Obj):
                continue
            if a == 2:
                out.append({'@class': 'DeleteDuringUpload', '@target': o})
            else:
                acts.append({'@class': 'ObjectToggleAction', 'version': 1, 'obj': o, 'mode': 0 if a == 1 else 1})
        if acts:
            out.append({'@class': 'ApplyDuringUpload', 'version': 0, 'action': {'actions': acts}})
        return out
    if cls == 'BoneConstraint':  # an object put on a bone
        return [{'@class': 'ArmatureLink', 'version': 7, 'propBone': f.get('obj'), 'recursive': '0',
                 'linkTo': [{'useBone': '1', 'bone': f.get('bone'), 'useObj': '0', 'obj': None, 'offset': ''}],
                 'alignPosition': '1', 'alignRotation': '1', 'alignScale': '1'}]
    if cls == 'SenkyGestureDriver':  # a Gesture Driver with a gesture per state
        gs = []
        for sign, lock, states in SENKY:
            for s in states:
                st = dictof(f.get(s))
                acts = listof(st.get('actions')) + ([{'@class': 'BlockBlinkingAction'}] if s.startswith('eyes') else [])
                gs.append({'hand': 0, 'sign': sign, 'comboSign': 0, 'state': {'actions': acts},
                           'enableLockMenuItem': '1', 'lockMenuItem': 'Emote Lock/' + lock, 'enableExclusiveTag': '1',
                           'exclusiveTag': re.match(r'[a-z]+', s).group(0), '@senky': lock})
        return vrcf_upgrade({'@class': 'GestureDriver', 'version': 1, 'gestures': gs})
    if cls == 'Breathing':  # a toggle, on at first, that goes between breathing in and out
        ins, outs = dictof(f.get('inState')), dictof(f.get('outState'))
        if v < 1:
            ins = {'actions': listof(ins.get('actions'))}
            outs = {'actions': listof(outs.get('actions'))}
            if isinstance(f.get('obj'), Obj):
                ins['actions'].append({'@class': 'ScaleAction', 'obj': f['obj'], 'scale': f.get('scaleMin')})
                outs['actions'].append({'@class': 'ScaleAction', 'obj': f['obj'], 'scale': f.get('scaleMax')})
            if _str(f.get('blendshape')).strip():
                ins['actions'].append({'@class': 'BlendShapeAction', 'blendShape': f['blendshape'],
                                       'blendShapeValue': 0, 'allRenderers': '1'})
                outs['actions'].append({'@class': 'BlendShapeAction', 'blendShape': f['blendshape'],
                                        'blendShapeValue': 100, 'allRenderers': '1'})
        return vrcf_upgrade({'@class': 'Toggle', 'version': 3, 'name': 'Breathing', 'defaultOn': '1', 'state': {
            'actions': [{'@class': 'SmoothLoopAction', 'state1': outs, 'state2': ins, 'loopTime': 5}]}})
    if cls == 'WorldConstraint':  # a toggle that leaves the object where it is in the world
        return vrcf_upgrade({'@class': 'Toggle', 'version': 3, 'name': _str(f.get('menuPath')), 'state': {
            'actions': [{'@class': 'WorldDropAction', 'obj': f.get('@go')}]}})
    if cls == 'GestureDriver':
        for g in listof(f.get('gestures')):
            g = dictof(g)
            if inum(g.get('version'), -1) < 1 and truthy(g.get('disableBlinking', '0')):
                st = g.setdefault('state', {})
                if not isinstance(st, dict):
                    st = g['state'] = {}
                acts = st.setdefault('actions', [])
                if isinstance(acts, list) and not any(dictof(a).get('@class') == 'BlockBlinkingAction' for a in acts):
                    acts.append({'@class': 'BlockBlinkingAction'})
    if cls == 'ArmatureLink' and v < 7:
        # the old link modes: 0 skin rewrite, 1 merge as children, 2 parent constraint, 3 reparent root, 4 auto;
        # bone offsets kept: 0 auto, 1 yes, 2 no
        mode = inum(f.get('linkMode'), 4)
        if v < 1:
            mode = 0 if truthy(f.get('useBoneMerging', '0')) else 1
        scale = num(f.get('skinRewriteScalingFactor'), 1.0) if v >= 2 else 1.0
        keep = inum(f.get('keepBoneOffsets2')) if v >= 3 else (1 if truthy(f.get('keepBoneOffsets', '0')) else 2)
        if v < 4 and mode != 0:
            scale = 0.0
        if v < 5 and mode == 1:
            f['scalingFactorPowersOf10Only'] = '0'
        if v < 6:
            path = _str(f.get('bonePathOnAvatar'))
            if path.strip():
                f['linkTo'] = [{'useBone': '0', 'useObj': '0', 'obj': None, 'offset': path}]
            else:
                f['linkTo'] = [{'useBone': '1', 'bone': b, 'useObj': '0', 'obj': None, 'offset': ''}
                               for b in [f.get('boneOnAvatar', 0)] + listof(f.get('fallbackBones'))]
        # None: decided when it is linked, from whether it is recursive
        f['recursive'] = None if mode == 4 else ('0' if mode == 3 else '1')
        f['@align'] = None if keep == 0 else keep == 2
        f['autoScaleFactor'] = None if scale <= 0 else '0'
        f['skinRewriteScalingFactor'] = 1.0 if scale <= 0 else scale
    if cls == 'BlendShapeLink' and v < 1:  # objects, then their meshes
        f['linkSkins'] = [{'renderer': next((x for x in o.comps if x.cls == 137), None)}
                          for o in listof(f.get('objs')) if isinstance(o, Obj) and o.cls == 1]
    if cls == 'Toggle' and v < 2:
        if not truthy(f.get('defaultOn', '0')):
            f['defaultSliderValue'] = 0
        f['sliderInactiveAtZero'] = '1'
    if cls == 'FullController':
        if v < 1:
            f['allNonsyncedAreGlobal'] = '1'
        if v < 2:
            for old, new in (('controller', 'controllers'), ('menu', 'menus'), ('parameters', 'prms')):
                if vrcf_asset(f.get(old)):
                    e = {old: f.get(old)}
                    if old == 'menu':
                        e['prefix'] = _str(f.get('submenu'))
                    f[new] = listof(f.get(new)) + [e]
        if v < 3:
            rw = listof(f.get('rewriteBindings'))
            rw += [{'from': s, 'to': ''} for s in listof(f.get('removePrefixes')) if _str(s).strip()]
            if _str(f.get('addPrefix')).strip():
                rw.append({'from': '', 'to': f.get('addPrefix')})
            f['rewriteBindings'] = rw

    def actions(x, depth=0):  # actions saved by older versions
        if isinstance(x, dict):
            k, v = x.get('@class'), inum(x.get('version'), -1)
            if k == 'ObjectToggleAction' and v < 1:  # from before modes: it flips the object
                x['mode'] = 2
            elif k == 'MaterialAction' and v < 1 and isinstance(x.get('obj'), Obj):  # an object, then its renderer
                o = x['obj'] if x['obj'].cls == 1 else x['obj'].go
                x['renderer'] = next((c for c in o.comps if c.cls in RENDERERS), None) if o is not None else None
            elif k == 'MaterialPropertyAction':
                if v < 1:  # a renderer, then its object
                    r = x.get('renderer')
                    x['renderer2'] = r.go if isinstance(r, Obj) and r.cls != 1 else r
                if v < 2:
                    x['propertyType'] = 4  # the type found from the materials
            for y in x.values():
                if depth < 20:
                    actions(y, depth + 1)
        elif isinstance(x, list):
            for y in x:
                actions(y, depth + 1)
    actions(f)
    return [f]


def vrcf_rewrite(rules):
    """a Full Controller's rewriteBindings as a function of a clip's path (None: nothing to rewrite). Each rule in
    turn moves the paths at or under its "from" to its "to" (an empty "from" puts every path under "to"; a path that
    starts with "/" is from the avatar's root and stays), and one marked delete drops the paths it matches (None)"""
    rs = []
    for r in listof(rules):
        r = dictof(r)
        a, b = _str(r.get('from')), _str(r.get('to'))
        rs.append((a.rstrip('/') or a[:1], b.rstrip('/') or b[:1], truthy(r.get('delete', '0'))))
    if not rs:
        return None

    def put(to, rest):
        if not to or rest.startswith('/'):
            return rest
        if not rest:
            return to
        return '/' + rest if to == '/' else to + '/' + rest

    def rewrite(path):
        for a, b, drop in rs:
            if not a:
                path = put(b, path)
            elif path.startswith(a + '/'):
                path = put(b, path[len(a) + 1:])
            elif path == a:
                path = b
            else:
                continue
            if drop:
                return None
        return path
    return rewrite


def animated_transforms(an):
    """the GameObjects whose Transforms the avatar's animations move, as VRCFury's Armature Link finds them before
    it links (FindAnimatedTransformsService): ({ids moved or turned}, {ids scaled}), from every controller the avatar
    plays (its own, Modular Avatar's Merge Animators', VRCFury's Full Controllers') and VRCFury's toggle clips. A
    Transform curve counts by what its name has in it: position, euler (so not the quaternion m_LocalRotation) or
    scale"""
    db, av = an.db, an.av
    moved, scaled = set(), set()
    todo = []  # (controller or clip guid, fileID or None for all, base, rewrite)
    d = av.desc
    if truthy(d.get('customizeAnimationLayers', '0')):
        for l in listof(d.get('baseAnimationLayers')) + listof(d.get('specialAnimationLayers')):
            l = dictof(l)
            g = ref(l.get('animatorController'))[1]
            if not truthy(l.get('isDefault', '0')) and g:
                todo.append((g, None, '', None))
    for c in (an.mat.comps.get('MergeAnimator', []) if an.mat else []):
        g = ref(c.data.get('animator'))[1]
        base = ''
        if inum(c.data.get('pathMode')) == 0:
            base = av_path(av, ma_objref(av, c.data.get('relativePathRoot')) or c.go)
        if g:
            todo.append((g, None, base, None))
    vf = an.vrcf
    for c, f in (vf.feats if vf else []):
        if f['@class'] == 'FullController':
            root = vf.go(f.get('rootObjOverride')) or c.go
            rw = vrcf_rewrite(f.get('rewriteBindings'))
            for e in listof(f.get('controllers')):
                p = vrcf_asset(dictof(e).get('controller'))
                if p:
                    todo.append((p[0], None, vf.bases(root), rw))
        for st in ([f.get('state')] + [dictof(x).get('state') for x in listof(f.get('localStates'))]
                   if f['@class'] == 'Toggle' else []):
            for a in listof(dictof(st).get('actions')):
                a = dictof(a)
                if a.get('@class') == 'AnimationClipAction':
                    p = vrcf_asset(a.get('clip'))
                    if p:
                        todo.append((p[0], p[1] or None, vf.bases(c.go), None))
                elif a.get('@class') == 'ScaleAction':
                    g = vf.go(a.get('obj'))
                    if g is not None:
                        scaled.add(id(g))

    def find(base, path, rewrite):
        if rewrite is not None:
            path = rewrite(path)
            if path is None:
                return None
        if isinstance(base, tuple):
            if path.startswith('/'):
                return av.paths.get(path[1:])
            return next((av.paths[join_path(b, path)] for b in base if join_path(b, path) in av.paths), None)
        return av.paths.get(join_path(base, path))

    seen = set()
    while todo:
        g, fid, base, rewrite = todo.pop()
        uf = db.yaml(g) if db.get(g) is not None and db.get(g).ext not in MODEL_EXT else None
        if uf is None or uf.binary or (g, fid, base) in seen:
            continue
        seen.add((g, fid, base))
        for f in ([fid] if fid is not None else uf.order):
            k, body = uf.cls(f), uf.get(f)[1]
            if k == 1102:  # a state: its motion
                p = ptr(body.get('m_Motion'), g)
                if p:
                    todo.append((p[0], p[1], base, rewrite))
            elif k == 206:  # a blend tree: its children's
                for ch in listof(body.get('m_Childs')):
                    p = ptr(dictof(ch).get('m_Motion'), g)
                    if p:
                        todo.append((p[0], p[1], base, rewrite))
            elif k == 74:
                curves = [(str(dictof(c).get('path') or ''), 'position') for c in listof(body.get('m_PositionCurves'))]
                curves += [(str(dictof(c).get('path') or ''), 'euler') for c in listof(body.get('m_EulerCurves'))]
                curves += [(str(dictof(c).get('path') or ''), 'scale') for c in listof(body.get('m_ScaleCurves'))]
                for c in listof(body.get('m_FloatCurves')) + listof(body.get('m_EditorCurves')):
                    c = dictof(c)
                    if inum(c.get('classID')) == 4:
                        curves.append((str(c.get('path') or ''), str(c.get('attribute') or '').lower()))
                for path, attr in curves:
                    t = find(base, path, rewrite)
                    if t is None:
                        continue
                    if 'scale' in attr:
                        scaled.add(id(t))
                    elif 'euler' in attr or 'position' in attr:
                        moved.add(id(t))
    return moved, scaled


def vrcf_asset(w):
    """what one of VRCFury's asset fields (GuidAnimationClip, GuidController, GuidMenu, GuidParams...) names:
    (guid, fileID; 0 for the file's main object), or None: the reference itself, else its id (guid[:fileID]|path|name)"""
    if isinstance(w, dict):
        f, g = ref(w.get('objRef')) if isinstance(w.get('objRef'), dict) else (0, None)
        if g:
            return g, f
        m = re.match(r'\s*([0-9a-fA-F]{32})(?::(-?\d+))?\s*(?:\||$)', _str(w.get('id')))
        if m:
            return m.group(1).lower(), int(m.group(2) or 0)
        g = _str(w.get('guid'))
        if re.fullmatch(r'[0-9a-fA-F]{32}', g):
            return g.lower(), inum(w.get('fileID'))
    return None


def vrcf_path(s):
    """a VRCFury menu path as menu names: "Clothes/Jacket"; "\\/" is a slash in a name"""
    return [p.replace('\\/', '/').strip() for p in re.split(r'(?<!\\)/', s or '') if p.replace('\\/', '/').strip()]


def human_fallbacks(b):
    """the humanoid bones (HumanBodyBones numbers) an Armature Link falls back to when bone b is not in the avatar:
    for a finger bone its lower segments, the same segments of the fingers nearest it, then the hand; for any other
    bone the one above it, and so on up to the hips"""
    up = {'Jaw': 'Head', 'LeftEye': 'Head', 'RightEye': 'Head', 'Head': 'Neck', 'Neck': 'UpperChest',
          'UpperChest': 'Chest', 'Chest': 'Spine', 'Spine': 'Hips', 'LeftShoulder': 'UpperChest',
          'RightShoulder': 'UpperChest', 'LeftToes': 'LeftFoot', 'RightToes': 'RightFoot'}
    for s in ('Left', 'Right'):
        for a, b2 in (('Hand', 'LowerArm'), ('LowerArm', 'UpperArm'), ('UpperArm', 'Shoulder'), ('Foot', 'LowerLeg'),
                      ('LowerLeg', 'UpperLeg'), ('UpperLeg', None)):
            up[s + a] = s + b2 if b2 else 'Hips'
    out = []
    if 24 <= b <= 53:  # a finger: 24 + side * 15 + finger * 3 + segment
        side, k = divmod(b - 24, 15)
        finger, seg = divmod(k, 3)
        out += [24 + side * 15 + finger * 3 + s for s in range(seg - 1, -1, -1)]
        near = sorted((f for f in range(5) if f != finger), key=lambda f: (abs(f - finger), -f))
        for f in near:
            out += [24 + side * 15 + f * 3 + s for s in range(seg, -1, -1)]
        hand = 18 if side else 17
        return out + [hand] + human_fallbacks(hand)
    name = HUMAN_BONES[b] if 0 <= b < len(HUMAN_BONES) else None
    while name in up:
        name = up[name]
        out.append(HUMAN_BONES.index(name))
    return out


class VRCFury:
    """what VRCFury adds to the menu, the parameters and FX, and the resting state it gives the avatar (applied to
    the objects' own data, as VRCFury changes the avatar it uploads)"""

    def __init__(self, an, comps):
        self.an, self.av, self.db = an, an.av, an.db
        self.feats = [(c, f) for c in comps for f in vrcf_features(c)]
        self.declared = {}  # parameter -> (value type, default)
        # [(test, what it tests, {property: value})], in the order of their FX layers: 'on' (a bool at 1), 'nonzero',
        # 'slider' (a float: from where the property rests to the value), 'gesture' ((hand, sign, the right hand's
        # sign of a combo, the lock parameter)), 'puppet' ([(stop, {property: value})] along the float)
        self.rules = []
        self.drives = []  # [(test, what it tests, {FX float: value})]: Set an FX Float, while a toggle is on
        self.blocks = []  # [(test, what it tests, 'eyes' | 'mouth')]: Block Blinking and Block Visemes
        self.named = []  # [(test, what it tests, name)]: what a gesture face is called
        self.menu = []  # [(path of menu names, control)]
        self.merged = []  # [(priority, order, Controller, {its parameter names: the avatar's}, False)]
        self.links = [(c, f) for c, f in self.feats if f['@class'] == 'ArmatureLink']
        self.skipped = []
        self.missed = {}  # actions and features that are not converted: kind -> count
        self.blink = None  # Blinking's face: {(renderer, shape key): weight}
        self.visemes = None  # Visemes': {preset: {(renderer, shape key): weight}}
        self.consonants = None  # and its consonants': {'pp' ... 'rr': {(renderer, shape key): weight}}
        self.loops = {}  # toggle parameter -> (seconds, {property: value} at one end, at the other): Smooth Loops
        self.drops = {}  # toggle parameter -> [GameObjects left in the world while it is on]: World Drops
        others = {}
        for c, f in self.feats:
            k = f['@class']
            if k not in VRCF_DONE and k not in VRCF_QUIET:
                others[k] = others.get(k, 0) + 1
        self.upload()
        self.rest()
        self.syncs = [(base, a, sk, b, None) for base, sk, m in self.blendshape_links() for a, bs in m.items()
                      for b in bs]
        for src, a, dst, b, pts in self.syncs:  # the linked meshes rest as the base does
            set_data(self.av, ('s', dst, b), self.av.default(('s', src, a)))
        self.toggles()
        self.full_controllers()
        self.puppets()
        self.gesture_drivers()
        self.faces()
        self.move_menus()
        if any(f['@class'] == 'RemoveBlinking' for c, f in self.feats) and self.blink is None:
            warn('VRCFury: Remove Blinking: hypr3d blinks with the shape keys it finds anyway')
        for k, n in sorted(others.items()):
            why = VRCF_WHY.get(k)
            warn('%d VRCFury %s feature(s): not converted%s' % (n, re.sub(r'(?<=[a-z0-9])(?=[A-Z])', ' ', k),
                                                                 ' (%s)' % why if why else ''))
        for k, n in sorted(self.missed.items()):
            warn('VRCFury: %d %s: not converted' % (n, k))
        n = {k: sum(1 for c, f in self.feats if f['@class'] == k) for k in VRCF_DONE}
        said = ['%d %s' % (n[k], re.sub(r'(?<=[a-z])(?=[A-Z])', ' ', k)) for k in VRCF_DONE_ORDER if n.get(k)]
        if said:
            log('VRCFury: ' + ', '.join(said))

    def miss(self, what):
        self.missed[what] = self.missed.get(what, 0) + 1

    def go(self, x):
        """the avatar's GameObject a reference names (a GameObject or one of its components)"""
        if isinstance(x, Obj):
            g = x if x.cls == 1 else x.go
            if g is not None and id(g) in self.av.inside:
                return g
        return None

    def bases(self, g):
        """where a clip's paths are looked for, from g up to the avatar's root"""
        out = []
        while g is not None and id(g) in self.av.inside:
            out.append(av_path(self.av, g))
            if g is self.av.root:
                break
            g = g.parent
        return tuple(out) or ('',)

    # ---- actions

    def actions(self, state):
        """the actions of a State that play here: on the desktop, for the wearer"""
        out = []
        for a in listof(dictof(state).get('actions')):
            if not isinstance(a, dict) or not a.get('@class'):
                continue
            if (truthy(a.get('desktopActive', '0')) or truthy(a.get('androidActive', '0'))) and not truthy(
                    a.get('desktopActive', '0')):
                continue
            if truthy(a.get('remoteOnly', '0')):
                continue
            out.append(a)
        return out

    def props(self, state, comp, count=True):
        """{property: value} a State's actions set"""
        av, out = self.av, {}
        for a in self.actions(state):
            k = a['@class']
            if k == 'ObjectToggleAction':
                g = self.go(a.get('obj'))
                if g is None:
                    continue
                mode = inum(a.get('mode'))
                on = mode == 0 or (mode == 2 and av.default(('a', g)) < 0.5)
                out[('a', g)] = 1.0 if on else 0.0
            elif k == 'BlendShapeAction':
                shape = _str(a.get('blendShape'))
                r = a.get('renderer')
                one = r if isinstance(r, Obj) and r.cls == 137 else None
                for smr in av.renderers:
                    if smr.cls != 137 or (not truthy(a.get('allRenderers', '1')) and smr is not one):
                        continue
                    if shape in av.shape_names(smr):
                        out[('s', smr, shape)] = num(a.get('blendShapeValue'), 100.0)
            elif k == 'AnimationClipAction':
                p = vrcf_asset(a.get('clip'))
                if p and not p[1]:  # the file's main object
                    uf = self.db.yaml(p[0])
                    p = (p[0], uf.main(74)) if uf is not None and not uf.binary and uf.main(74) is not None else None
                clip = Anim(self.db, av, self.bases(comp.go)).clip(p) if p else None
                if clip is not None:
                    if clip.other and count:
                        self.miss('toggle clip(s) with curves other than objects, shape keys and materials')
                    out.update(clip.sample(None))
            elif k == 'MaterialAction':  # a material in a renderer's slot
                r, i = a.get('renderer'), inum(a.get('materialIndex'))
                p = vrcf_asset(a.get('mat'))
                if isinstance(r, Obj) and r.cls in RENDERERS and r.go is not None and id(r.go) in av.inside and p and (
                        0 <= i < len(listof(r.data.get('m_Materials')))):
                    out[('m', r, i)] = (p[0], p[1] or 2100000)
            elif k == 'MaterialPropertyAction':
                out.update(self.material_properties(a, count))
            elif k == 'ScaleAction':  # the object's scale, times this
                g = self.go(a.get('obj'))
                if g is not None:
                    out[('t', g, 'm')] = num(a.get('scale'), 1.0)
            elif k in ('FxFloatAction', 'BlockBlinkingAction', 'BlockVisemesAction', 'ResetPhysboneAction',
                       'SmoothLoopAction', 'WorldDropAction'):
                pass  # extras() has the first three, toggles() the loops and drops; PhysBones settle by themselves
            elif count:
                self.miss('%s action(s) in toggles' % re.sub(r'(?<=[a-z])(?=[A-Z])', ' ', k[:-6] if k.endswith(
                    'Action') else k))
        return out

    def material_properties(self, a, count=True):
        """a Material Property action's values, of the properties hypr3d carries"""
        av = self.av
        name = _str(a.get('propertyName')).strip()
        if not name or '.' in name:
            return {}
        if truthy(a.get('affectAllMeshes', '0')):
            rs = list(av.renderers)
        else:
            g = self.go(a.get('renderer2'))
            rs = [c for c in g.comps if c.cls in RENDERERS] if g is not None else []
        t = inum(a.get('propertyType'))
        if t == 4:  # what it is, from the materials: a colour, a tiling and offset, else a number
            t = 3 if name.endswith('_ST') else 1 if any(av.matprop(r, k, name + '.r') is not None for r in rs
                                                         for k in range(len(listof(r.data.get('m_Materials'))))) else 0
        vec = dictof(a.get('valueVector'))
        colr = dictof(a.get('valueColor'))
        if t == 0:
            vals = {name: num(a.get('value'))}
        elif t == 1:
            vals = {'%s.%s' % (name, ch): num(colr.get(ch), 1.0) for ch in 'rgba'}
        else:
            vals = {'%s.%s' % (name, ch): num(vec.get(ch)) for ch in 'xyzw'}
        out = {}
        for n, v in vals.items():
            if n not in MP_NAMES:
                if count:
                    self.miss('material propert%s hypr3d does not carry (%s)' % ('y' if len(vals) == 1 else 'ies', name))
                break
            for r in rs:
                out[('mp', r, n)] = v
        return out

    def extras(self, state):
        """what a State does besides properties: ({FX float: value} it sets, {'eyes', 'mouth'} it blocks)"""
        drives, blocks = {}, set()
        for a in self.actions(state):
            k = a['@class']
            if k == 'FxFloatAction' and _str(a.get('name')).strip() and _str(a.get('name')) not in VRC_PARAMS:
                drives[_str(a.get('name'))] = num(a.get('value'), 1.0)
            elif k == 'BlockBlinkingAction':
                blocks.add('eyes')
            elif k == 'BlockVisemesAction':
                blocks.add('mouth')
        return drives, blocks

    def rule(self, test, arg, state, comp, props=None):
        """a State's actions in effect while the test passes"""
        self.rules.append((test, arg, self.props(state, comp) if props is None else props))
        drives, blocks = self.extras(state)
        if drives:
            self.drives.append((test, arg, drives))
        for b in blocks:
            self.blocks.append((test, arg, b))

    def states(self, f):
        """the States of a feature whose actions give the avatar a resting state (Apply During Upload's do not)"""
        out = []

        def walk(x, depth=0):
            if isinstance(x, dict):
                if isinstance(x.get('actions'), list) and '@class' not in x:
                    out.append(x)
                for k, y in x.items():
                    if depth < 20 and not k.startswith('@'):
                        walk(y, depth + 1)
            elif isinstance(x, list):
                for y in x:
                    walk(y, depth + 1)
        if f['@class'] != 'ApplyDuringUpload':
            walk(f)
        return out

    # ---- the avatar as VRCFury uploads it

    def set_data(self, pr, v):
        set_data(self.av, pr, v)

    def upload(self):
        """Apply During Upload: its actions, done to the avatar"""
        for c, f in self.feats:
            if f['@class'] == 'ApplyDuringUpload':
                for pr, v in self.props(f.get('action'), c).items():
                    self.set_data(pr, v)

    def rest(self):
        """the implicit resting state: an object some action turns on rests off, one it turns off rests on; a Full
        Controller toggled by a parameter rests off"""
        for c, f in self.feats:
            for st in self.states(f):
                for a in self.actions(st):
                    if a['@class'] == 'ObjectToggleAction' and inum(a.get('mode')) in (0, 1):
                        g = self.go(a.get('obj'))
                        if g is not None:
                            self.set_data(('a', g), 0.0 if inum(a.get('mode')) == 0 else 1.0)
            if f['@class'] == 'FullController' and _str(f.get('toggleParam')).strip():
                g = self.go(f.get('rootObjOverride')) or c.go
                if g is not None:
                    self.set_data(('a', g), 0.0)

    # ---- toggles

    def toggles(self):
        entries = []
        for k, (c, f) in enumerate(x for x in self.feats if x[1]['@class'] == 'Toggle'):
            name = _str(f.get('name'))
            path = vrcf_path(name)
            local = truthy(f.get('separateLocal', '0'))
            state = f.get('localState' if local else 'state')
            props = self.props(state, c)
            if not props and truthy(f.get('hasTransition', '0')):  # an empty main state holds the end of the in one
                state = f.get('localTransitionStateIn' if local else 'transitionStateIn')
                props = self.props(state, c)
            g = _str(f.get('globalParam')).strip() if truthy(f.get('useGlobalParam', '0')) else ''
            param = g or 'VF%d_%s' % (k, name or 'Toggle')
            for a in self.actions(state):
                if a['@class'] == 'SmoothLoopAction':  # from state 1 to 2 and back, over and over, while it is on
                    self.loops.setdefault(param, (max(num(a.get('loopTime'), 5.0), 0.05), self.props(
                        a.get('state1'), c), self.props(a.get('state2'), c)))
                elif a['@class'] == 'WorldDropAction':  # the object stays in the world, where it is when it turns on
                    obj = self.go(a.get('obj'))
                    if obj is not None:
                        self.drops.setdefault(param, []).append(obj)
            if truthy(f.get('slider', '0')):  # a radial: from where the properties rest to the state's
                self.declared.setdefault(param, (1, num(f.get('defaultSliderValue'))))
                self.rule('slider', param, state, c, props)
                entries.append((path, param, None, [], False, False))
                continue
            dflt = 1.0 if truthy(f.get('defaultOn', '0')) else 0.0
            self.declared.setdefault(param, (2, dflt))
            self.rule('on', param, state, c, props)
            tags = [t.strip() for t in _str(f.get('exclusiveTag')).split(',') if t.strip()] if truthy(
                f.get('enableExclusiveTag', '0')) else []
            entries.append((path, param, dflt, tags, truthy(f.get('holdButton', '0')),
                            truthy(f.get('exclusiveOffState', '0'))))
        count = {}
        for e in entries:
            for t in dict.fromkeys(e[3]):
                count[t] = count.get(t, 0) + 1
        group_on = {}
        for path, param, dflt, tags, hold, off in entries:
            for t in tags:
                if count[t] > 1 and dflt:
                    group_on[t] = True
        for path, param, dflt, tags, hold, off in entries:
            if dflt is None:  # a slider
                if path:
                    self.menu.append((tuple(path[:-1]), {'name': path[-1], 'type': 203, 'parameter': {'name': ''},
                                                         'subParameters': [{'name': param}], 'value': 1.0}))
                continue
            groups = list(dict.fromkeys(t for t in tags if count[t] > 1))  # exclusive with every toggle of each
            if off and groups and not any(group_on.get(t) for t in groups):  # on while the others are off: at first
                self.declared[param] = (2, 1.0)
                for t in groups:
                    group_on[t] = True
            if not path:  # no menu item
                continue
            ctl = {'name': path[-1], 'type': 101 if hold else 102, 'parameter': {'name': param}, 'value': 1.0}
            if groups:
                ctl['group'] = groups[0]
                if len(groups) > 1:
                    ctl['groups'] = groups
            self.menu.append((tuple(path[:-1]), ctl))

    def puppets(self):
        """Puppets: a radial along its stops (the one that moves along a single axis, VRCFury's slider), from where the
        properties rest at 0; else a two-axis puppet whose stops are a 2D freeform directional blend tree's, the
        resting state at the middle"""
        for c, f in self.feats:
            if f['@class'] != 'Puppet':
                continue
            name = _str(f.get('name'))
            stops = [dictof(s) for s in listof(f.get('stops'))]
            if any(abs(num(s.get('y'))) > 1e-6 for s in stops) or not truthy(f.get('slider', '0')):
                px, py = '%s_x' % (name or 'Puppet'), '%s_y' % (name or 'Puppet')
                ux = any(abs(num(s.get('x'))) > 1e-6 for s in stops)
                uy = any(abs(num(s.get('y'))) > 1e-6 for s in stops)
                self.declared.setdefault(px, (1, num(f.get('defaultX'))))
                self.declared.setdefault(py, (1, num(f.get('defaultY'))))
                self.rules.append(('puppet2', (px, py), [((num(s.get('x')), num(s.get('y'))), self.props(s.get('state'), c))
                                                         for s in stops]))
                path = vrcf_path(name)
                if path:
                    self.menu.append((tuple(path[:-1]), {'name': path[-1], 'type': 201, 'parameter': {'name': ''},
                                                         'subParameters': [{'name': px if ux else ''},
                                                                           {'name': py if uy else ''}], 'value': 1.0}))
                continue
            param = '%s_x' % (name or 'Puppet')
            self.declared.setdefault(param, (1, num(f.get('defaultX'))))
            self.rules.append(('puppet', param, sorted([(num(s.get('x')), self.props(s.get('state'), c))
                                                        for s in stops], key=lambda x: x[0])))
            path = vrcf_path(name)
            if path:
                self.menu.append((tuple(path[:-1]), {'name': path[-1], 'type': 203, 'parameter': {'name': ''},
                                                     'subParameters': [{'name': param}], 'value': 1.0}))

    def gesture_drivers(self):
        """Gesture Drivers (and Senky's): a hand's sign shows a State; a lock toggle in the menu shows it too"""
        locks = {}
        n = 0
        for c, f in self.feats:
            if f['@class'] != 'GestureDriver':
                continue
            for g in listof(f.get('gestures')):
                g = dictof(g)
                n += 1
                hand, sign, combo = inum(g.get('hand')), inum(g.get('sign')), inum(g.get('comboSign'))
                lock = ''
                item = _str(g.get('lockMenuItem')).strip()
                if truthy(g.get('enableLockMenuItem', '0')) and item:
                    lock = locks.get(item)
                    if lock is None:
                        lock = locks[item] = 'gesture_%d_lock' % n
                        self.declared.setdefault(lock, (2, 0.0))
                        path = vrcf_path(item)
                        if path:
                            self.menu.append((tuple(path[:-1]), {'name': path[-1], 'type': 102,
                                                                 'parameter': {'name': lock}, 'value': 1.0}))
                arg = (hand, sign, combo, lock)
                self.rule('gesture', arg, g.get('state'), c)
                st = dictof(g.get('state'))
                clip = next((vrcf_asset(dictof(a).get('clip')) for a in listof(st.get('actions'))
                             if dictof(a).get('@class') == 'AnimationClipAction'), None)
                nm = g.get('@senky') or (vrcf_path(item)[-1] if item and vrcf_path(item) else '')
                if not nm and clip and self.db.get(clip[0]) is not None:
                    nm = self.db.get(clip[0]).name
                if not nm:  # the shape keys it sets, else the hand's sign
                    nm = ' '.join(dict.fromkeys(_str(dictof(a).get('blendShape')) for a in listof(st.get('actions'))
                                                if dictof(a).get('@class') == 'BlendShapeAction')).strip()
                self.named.append(('gesture', arg, nm or GESTURES[min(max(sign, 0), 7)].capitalize()))

    def faces(self):
        """Blinking's and Visemes' faces"""
        for c, f in self.feats:
            if f['@class'] == 'Blinking':
                sh = {(p[1], p[2]): min(max(v / 100.0, 0.0), 1.0) for p, v in self.props(f.get('state'), c).items()
                      if p[0] == 's' and v > 0.5}
                if sh:
                    self.blink = sh
            elif f['@class'] == 'Visemes':
                out = {}
                for pr, key in (('aa', 'state_aa'), ('ee', 'state_E'), ('ih', 'state_I'), ('oh', 'state_O'),
                                ('ou', 'state_U')):
                    sh = {(p[1], p[2]): min(max(v / 100.0, 0.0), 1.0) for p, v in self.props(f.get(key), c).items()
                          if p[0] == 's' and v > 0.5}
                    if sh:
                        out[pr] = sh
                if out:
                    self.visemes = out
                cons = {}
                for name, key in CONSONANT_VISEMES:
                    sh = {(p[1], p[2]): min(max(v / 100.0, 0.0), 1.0) for p, v in self.props(f.get('state_' + key), c).items()
                          if p[0] == 's' and v > 0.5}
                    if sh:
                        cons[name] = sh
                if cons:
                    self.consonants = cons

    def move_menus(self):
        """Move Menu Item and Reorder Menu Item, done to the whole menu when the rest is in (Analysis calls it)"""
        self.moves = [f for c, f in self.feats if f['@class'] in ('MoveMenuItem', 'ReorderMenuItem')]

    def menu_moved(self, menu):
        """the avatar's menu ([(path, control)]) with the moves done"""
        def norm(s):
            return ' '.join(rich_text(s).replace('\\n', ' ').lower().split())

        def same(a, b):
            return len(a) == len(b) and all(x == y or norm(x) == norm(y) for x, y in zip(a, b))
        for f in self.moves:
            if f['@class'] == 'MoveMenuItem':
                frm, to = vrcf_path(_str(f.get('fromPath'))), vrcf_path(_str(f.get('toPath')))
                if not frm:
                    continue
                moved, rest = [], []
                for p, c in menu:
                    full = tuple(p) + (rich_text(str(c.get('name') or '')),)
                    if same(full, frm):  # the item: renamed and moved
                        if to:
                            c = dict(c)
                            c['name'] = to[-1]
                            moved.append((tuple(to[:-1]), c))
                    elif len(p) >= len(frm) and same(tuple(p[:len(frm)]), frm):  # in the submenu
                        if to:
                            moved.append((tuple(to) + tuple(p[len(frm):]), c))
                    else:
                        rest.append((p, c))
                if not moved and len(rest) == len(menu):
                    warn('VRCFury: Move Menu Item: nothing at %s' % '/'.join(frm))
                    continue
                # to the end of the menu it goes to
                at = max((i + 1 for i, (p, c) in enumerate(rest) if moved and tuple(p[:len(moved[0][0])]) ==
                          moved[0][0]), default=len(rest))
                menu = rest[:at] + moved + rest[at:]
            else:
                path, pos = vrcf_path(_str(f.get('path'))), inum(f.get('position'))
                if not path:
                    continue
                idx = [i for i, (p, c) in enumerate(menu) if same(tuple(p) + (rich_text(str(c.get('name') or '')),),
                                                                   path)]
                if not idx:
                    continue
                items = [menu[i] for i in idx]
                rest = [x for i, x in enumerate(menu) if i not in idx]
                sib = [i for i, (p, c) in enumerate(rest) if same(tuple(p), tuple(path[:-1]))]
                if pos < 0:
                    pos = len(sib) + pos
                pos = min(max(pos, 0), len(sib))
                at = sib[pos] if pos < len(sib) else (sib[-1] + 1 if sib else len(rest))
                menu = rest[:at] + items + rest[at:]
        return menu

    def blendshape_links(self):
        """Blend Shape Links: [(base mesh, linked mesh, {base shape key: [linked ones]})]"""
        av, out = self.av, []
        for c, f in self.feats:
            if f['@class'] != 'BlendShapeLink':
                continue
            name = _str(f.get('baseObj'))
            cands = [g for g in av.gos if go_name(g) == name and any(x.cls == 137 for x in g.comps)]
            if not cands:
                warn('%s: its Blend Shape Link\'s base mesh %s is not in the avatar' % (go_name(c.go), name))
                continue
            bg = min(cands, key=lambda g: len(av_path(av, g)))
            base = next(x for x in bg.comps if x.cls == 137)
            for e in listof(f.get('linkSkins')):
                sk = dictof(e).get('renderer')
                if isinstance(sk, Obj) and sk.cls == 137 and sk.go is not None and id(sk.go) in av.inside:
                    m = self.shape_map(f, av.shape_names(base), av.shape_names(sk))
                    if m:
                        out.append((base, sk, m))
        return out

    @staticmethod
    def shape_map(f, base_names, link_names):
        """which of a linked mesh's shape keys follow which of the base's: named alike (exactly, else ignoring case and
        spaces, where that is unambiguous), all of them or the ones listed"""
        norms = [lambda x: x] + ([] if truthy(f.get('exactMatch', '0')) else
                                 [lambda x: re.sub(r'\s', '', x.lower())])

        def finder(names):
            tables = []
            for n in norms:
                t = {}
                for x in names:
                    t.setdefault(n(x), []).append(x)
                tables.append({k: v[0] for k, v in t.items() if len(v) == 1})
            return lambda x: next((t[n(x)] for n, t in zip(norms, tables) if n(x) in t), None)
        fb, fl = finder(base_names), finder(link_names)
        out = {}

        def attempt(a, b, again):
            a = fb(a)
            if a is None or (a in out and not again):
                return
            b = fl(b)
            if b is not None and b not in out.get(a, []):
                out.setdefault(a, []).append(b)
        for e in listof(f.get('includes')):
            e = dictof(e)
            a, b = _str(e.get('nameOnBase')).strip(), _str(e.get('nameOnLinked')).strip()
            if a or b:
                attempt(a or b, b or a, True)
        if truthy(f.get('includeAll', '1')):
            ex = {_str(dictof(e).get('name')) for e in listof(f.get('excludes'))}
            for x in base_names:
                if x not in ex:
                    attempt(x, x, False)
        return out

    @staticmethod
    def passes(test, arg, params):
        """does a rule's test pass with these parameters"""
        if test == 'on':
            return abs(params.get(arg, 0.0) - 1.0) < 0.5
        if test == 'nonzero':
            return abs(params.get(arg, 0.0)) > 1e-6
        if test == 'gesture':
            hand, sign, combo, lock = arg
            l, r = round(params.get('GestureLeft', 0.0)), round(params.get('GestureRight', 0.0))
            on = {0: l == sign or r == sign, 1: l == sign, 2: r == sign, 3: l == sign and r == combo}.get(hand, False)
            return on or bool(lock and params.get(lock, 0.0) >= 0.5)
        return False

    def drive(self, params):
        """the FX floats the toggles set (Set an FX Float), for the animators to read"""
        out = dict(params)
        for test, arg, drives in self.drives:
            if self.passes(test, arg, params) or (test == 'slider' and params.get(arg, 0.0) > 0):
                out.update(drives)
        return out

    def apply(self, params, vals, names=None, track=None):
        """what the toggles and the rest set, over the animators' values (their FX layers come last); then what
        linked shape keys follow. names and track collect a gesture face's name and what it blocks."""
        if not self.rules and not self.syncs:
            return vals
        vals = dict(vals)
        av = self.av

        def lerp(pr, d, t, w):
            if pr[0] in ('s', 'mp', 't') and d is not None and t is not None:
                return d + (t - d) * w
            if pr[0] == 'm':  # a material: an object curve steps, so only the end has it
                return t if w >= 0.999 else d
            return t if w >= 0.5 else d
        for test, arg, props in self.rules:
            if test == 'slider':  # from where the properties rest to the state's; VRCFury's two keys are flat
                x = min(max(params.get(arg, 0.0), 0.0), 1.0)
                if x <= 0:
                    continue
                w = x * x * (3 - 2 * x)
                for pr, t in props.items():
                    vals[pr] = lerp(pr, vals[pr] if pr in vals else av.default(pr), t, w)
            elif test == 'puppet2':  # a 2D freeform directional blend tree of the stops, the rest at the middle
                pts = [(0.0, 0.0)] + [s[0] for s in props]
                ws = blend2d(pts, (params.get(arg[0], 0.0), params.get(arg[1], 0.0)), polar=True)
                sets = [{}] + [s[1] for s in props]
                for pr in {q for d in sets for q in d}:
                    d = vals[pr] if pr in vals else av.default(pr)
                    got = [(w, x.get(pr, d)) for w, x in zip(ws, sets) if w > 1e-6]
                    if not got:
                        continue
                    if pr[0] in ('s', 'mp', 't') and all(v is not None for _, v in got):
                        vals[pr] = sum(w * v for w, v in got)
                    else:  # a material, an object: the stop with the most weight
                        vals[pr] = max(got, key=lambda x: x[0])[1]
            elif test == 'puppet':  # between the stops on each side of it; 0 is where the properties rest
                x = params.get(arg, 0.0)
                stops = [(0.0, {})] + [s for s in props if s[0] > 1e-6]
                lo = max((s for s in stops if s[0] <= x), key=lambda s: s[0])
                hi = min((s for s in stops if s[0] >= x), key=lambda s: s[0], default=lo)
                f = 0.0 if hi[0] <= lo[0] else (x - lo[0]) / (hi[0] - lo[0])
                for pr in set(lo[1]) | set(hi[1]):
                    d = vals[pr] if pr in vals else av.default(pr)
                    vals[pr] = lerp(pr, lo[1].get(pr, d), hi[1].get(pr, d), f)
            elif self.passes(test, arg, params):
                vals.update(props)
        if names is not None:
            for test, arg, nm in self.named:
                if self.passes(test, arg, params):
                    for t, a, p in self.rules:
                        if t == test and a == arg:
                            for pr in p:
                                names[pr] = nm
        if track is not None:
            for test, arg, what in self.blocks:
                if self.passes(test, arg, params):
                    track[what] = 2
        follow_shapes(vals, self.syncs)
        return vals

    # ---- full controllers

    def full_controllers(self):
        k = 0
        for c, f in self.feats:
            if f['@class'] != 'FullController':
                continue
            k += 1
            root = self.go(f.get('rootObjOverride')) or c.go
            prms = {}
            for e in listof(f.get('prms')):
                p = vrcf_asset(dictof(e).get('parameters'))
                if p and self.db.get(p[0]):
                    prms.update(read_params(self.db, p[0]))
                elif p:
                    warn('%s: a parameters file its Full Controller merges is not in the input' % go_name(c.go))
            glob = [_str(x) for x in listof(f.get('globalParams'))]
            free = truthy(f.get('allNonsyncedAreGlobal', '0'))
            seen = {}

            def rn(n, k=k, prms=prms, glob=glob, free=free, seen=seen):
                """a parameter's name once merged: its own if global, else one of its own"""
                if not n or n in VRC_PARAMS or (free and n not in prms):
                    return n
                if n not in seen:
                    g = False
                    for x in glob:
                        neg, x = x.startswith('!'), x[1:] if x.startswith('!') else x
                        star, x = x.endswith('*'), x[:-1] if x.endswith('*') else x
                        if n == x or (star and n.startswith(x)):
                            g = not neg
                            if neg:
                                break
                    seen[n] = n if g else 'VF%d_%s' % (k, n)
                return seen[n]
            for n, v in prms.items():
                self.declared.setdefault(rn(n), v)
            rewrite = vrcf_rewrite(f.get('rewriteBindings'))
            for e in listof(f.get('controllers')):
                e = dictof(e)
                p = vrcf_asset(e.get('controller'))
                if not p:
                    continue
                if inum(e.get('type'), 5) != 5:
                    # (VRChat's AnimLayerType) an Action controller's humanoid clips are emotes (action_clips), a
                    # Gesture one's are hand poses (Analysis.hand_poses); Base, Additive, Sitting, T Pose and IK Pose
                    # ones are how VRChat walks, sits and calibrates the avatar, which hypr3d does in its own way
                    continue
                if self.db.get(p[0]) is None:
                    warn('%s: the controller its Full Controller merges is not in the input' % go_name(c.go))
                    continue
                ctl = Controller(Anim(self.db, self.av, self.bases(root), rewrite), p[0])
                if ctl.guid is None:
                    warn('%s: the controller its Full Controller merges cannot be read' % go_name(c.go))
                    continue
                self.merged.append((1 << 20, len(self.merged), ctl, {n: rn(n) for n in ctl.params}, False))
            for e in listof(f.get('menus')):
                e = dictof(e)
                p = vrcf_asset(e.get('menu'))
                if not p or self.db.get(p[0]) is None:
                    continue
                for pf, ctl in read_menu(self.db, p[0], [], tuple(vrcf_path(_str(e.get('prefix'))))):
                    ctl = dict(ctl)
                    ctl['parameter'] = {'name': rn(_str(dictof(ctl.get('parameter')).get('name')))}
                    ctl['subParameters'] = [{'name': rn(_str(dictof(s).get('name')))}
                                            for s in listof(ctl.get('subParameters'))]
                    self.menu.append((pf, ctl))
            tp = _str(f.get('toggleParam')).strip()
            if tp and root is not None:
                self.rules.append(('nonzero', rn(tp), {('a', root): 1.0}))


def link_armatures(h, vf, human, animated=(set(), set())):
    """VRCFury's Armature Links, on the hierarchy Modular Avatar left (h); animated: animated_transforms()'s"""
    av, U = h.av, h.U
    humans = {id(g) for g in human.values()}
    moved, scaled = animated
    # a PhysBone's root and the bones it moves are neither moved nor given the avatar's bones' weights
    pb_roots, pb_kids = set(), set()
    for g in av.gos:
        for c in g.comps:
            if not is_physbone(c) or id(c) in h.dead or not truthy(c.data.get('m_Enabled', '1')):
                continue
            r = h.pb_root(c) or g
            pb_roots.add(id(r))
            stop = {id(x) for x in h.refs(c, 'ignoreTransforms')}
            todo = list(h.down(r))
            while todo:
                x = todo.pop()
                if id(x) not in stop and id(x) not in pb_kids:
                    pb_kids.add(id(x))
                    todo += h.down(x)
    done, n_bones, n_links = [], 0, 0
    for c, f in vf.links:
        prop = vf.go(f.get('propBone'))
        where = go_name(c.go)
        if prop is None or id(prop) in h.deleted:
            warn('%s: its Armature Link has no Link From object' % where)
            continue
        t = link_target(h, vf, f, human)
        if t is None:
            warn('%s: its Armature Link finds nothing in the avatar to link %s to' % (where, h.name(prop)))
            continue
        if any(id(g) in h.parent and h.under(g, prop) for g in human.values()):
            warn('%s: its Armature Link\'s Link From (%s) holds bones of the avatar, so it is not linked' % (
                where, h.name(prop)))
            continue
        rec = f.get('recursive')
        rec = external_skin(h, prop) if rec is None else truthy(rec)
        if '@align' in f:  # an old link: alignment follows recursion unless it said
            ap = ar = asc = rec if f['@align'] is None else f['@align']
        else:
            ap, ar, asc = (truthy(f.get(k, '0')) for k in ('alignPosition', 'alignRotation', 'alignScale'))
        suffix = _str(f.get('removeBoneSuffix'))
        if not suffix.strip():
            a, p = h.name(t), h.name(prop)
            if a in p and a != p:
                suffix = p.replace(a, '')
        pairs = [(prop, t)]
        if rec:
            stack = [(prop, t)]
            while stack:
                p, q = stack.pop()
                for ch in list(h.down(p)):
                    nm = h.name(ch).replace(suffix, '') if suffix.strip() else h.name(ch)
                    qc = next((x for x in h.down(q) if h.name(x) == nm), None)
                    if qc is not None:
                        pairs.append((ch, qc))
                        stack.append((ch, qc))
        auto = f.get('autoScaleFactor')
        auto = rec if auto is None else truthy(auto)
        factor = num(f.get('skinRewriteScalingFactor'), 1.0)
        if auto:
            factor = 1.0
            if rec:
                a = abs(U[id(t)].col[0].xyz.length) or 1e-12
                factor = U[id(prop)].col[0].xyz.length / a
                if truthy(f.get('scalingFactorPowersOf10Only', '1')) and factor > 0:
                    lg = math.log10(factor)
                    lg = math.ceil(lg) if lg % 1.0 > 0.75 else math.floor(lg)
                    factor = 10.0 ** lg
        one = truthy(f.get('forceOneWorldScale', '0'))
        def moved_above(p):  # is p under a bone of prop's that an animation moves or turns
            x = h.up(p)
            while x is not None:
                if id(x) in moved:
                    return True
                if x is prop:
                    return False
                x = h.up(x)
            return False
        stay = []  # bones left where they are, and so all under them
        for p, q in reversed(pairs):  # the ones found last first, as VRCFury does
            if p is not prop and id(q) not in humans and (
                    id(p) in pb_kids or any(h.under(p, s) for s in stay) or moved_above(p)):
                stay.append(p)
                continue
            if ap or ar or asc or one:
                tl, tr, ts = U[id(q)].decompose()
                pl, pr, ps = U[id(p)].decompose()
                s = Vector((1.0, 1.0, 1.0)) if one else (ts * factor if asc else ps)
                h.place(p, Matrix.LocRotScale(tl if ap else pl, tr if ar else pr, s))
            h.set_parent(p, q)
            x = q
            while x is not None:  # a PhysBone above where it goes leaves it out
                if id(x) in pb_roots:
                    h.blocks.setdefault(id(x), []).append(p)
                x = h.up(x)
            if id(p) not in pb_roots and id(p) not in pb_kids and id(p) not in moved and id(p) not in scaled:
                h.retarget[id(p)] = q
                h.bound[id(p)] = U[id(p)].copy()
                n_bones += 1
            done.append((p, q))
        n_links += 1
    # what is left unused goes: an object nothing uses, with nothing under it that is used
    used = vrcf_used(h)
    gone = 0
    for p, q in done:
        if id(p) in h.deleted or id(p) in used:
            continue
        sub, todo = [], [p]
        while todo:
            x = todo.pop()
            sub.append(x)
            todo += h.down(x)
        par = h.parent[id(p)]
        if par is not None:
            h.kids[id(par)].remove(p)
        h.parent[id(p)] = None
        for x in sub:
            h.deleted.add(id(x))
            h.retarget.setdefault(id(x), q)
            gone += 1
    if n_links:
        log('VRCFury: %d Armature Link%s (%d objects linked to the avatar\'s, %d meshes\' bones given the avatar\'s, '
            '%d objects left unused and removed)' % (n_links, '' if n_links == 1 else 's', len(done), n_bones, gone))


def link_target(h, vf, f, human):
    """an Armature Link's Link To: the first of its targets the avatar has (a humanoid bone, an object or the root,
    with an offset path under it), then the humanoid bones above the first humanoid one"""
    tos = [dictof(x) for x in listof(f.get('linkTo'))] or [{'useBone': '1', 'bone': 0}]
    tries = list(tos)
    first = next((x for x in tos if truthy(x.get('useBone', '1')) and not _str(x.get('offset')).strip()), None)
    if first is not None:
        tries += [{'useBone': '1', 'bone': b} for b in human_fallbacks(inum(first.get('bone')))]
    for x in tries:
        if truthy(x.get('useBone', '1')):
            b = inum(x.get('bone'))
            g = human.get(HUMAN_BONES[b]) if 0 <= b < len(HUMAN_BONES) else None
        elif truthy(x.get('useObj', '0')):
            g = vf.go(x.get('obj'))
        else:
            g = h.root
        if g is None or id(g) in h.deleted or id(g) not in h.parent:
            continue
        off = _str(x.get('offset'))
        if off.strip():
            g = h.find(g, off)
        if g is not None and h.under(g, h.root):
            return g
    return None


def external_skin(h, prop):
    """does a mesh outside prop use a bone inside it (an old Armature Link on Auto merges only then)"""
    for r in h.av.renderers:
        if r.cls != 137 or h.under(r.go, prop):
            continue
        bones = [x.go for x in [r.data.get('m_RootBone')] + listof(r.data.get('m_Bones')) if isinstance(x, Obj)]
        if any(b is not None and id(b) in h.parent and h.under(b, prop) for b in bones):
            return True
    return False


def vrcf_used(h):
    """the GameObjects VRCFury's clean-up keeps: ones with a component (a Transform or a constraint aside), ones some
    component names (a PhysBone's ignore list and the bones of meshes given the avatar's aside), and their parents"""
    av, out = h.av, set()

    def mark(g):
        while g is not None and id(g) not in out:
            out.add(id(g))
            g = h.up(g)

    def walk(x, skip_bones, key=''):
        if isinstance(x, Obj):
            g = x if x.cls == 1 else x.go
            if g is not None and id(g) in av.inside and not (skip_bones and id(g) in h.retarget):
                mark(g)
        elif isinstance(x, dict):
            for k, v in x.items():
                if k not in ('m_GameObject', 'ignoreTransforms', 'm_Script'):
                    walk(v, skip_bones or k == 'm_Bones', k)
        elif isinstance(x, list):
            for v in x:
                walk(v, skip_bones, key)
    for g in av.gos:
        if id(g) in h.deleted:
            continue
        for c in g.comps:
            if c.cls in TRANSFORMS or id(c) in h.dead or ma_kind(c):
                continue
            if not is_constraint(c):
                mark(g)
            walk(c.data, False)
    return out


# ---------------------------------------------------------------- Setup Outfit
#
# What MA's "Setup Outfit" does to an outfit put on the avatar with --outfit (SetupOutfit.cs and
# HeuristicBoneMapper.cs): it finds the outfit's hips, works out the prefix and suffix of its bone names, renames the
# bones its heuristics match to the avatar's, turns A-pose arms to the avatar's pose, and adds a Merge Armature (a
# second one for an UpperChest the avatar does not have).
#
# The bone names, in HumanBodyBones order, are MA's (Copyright (c) 2022 bd_, MIT License), which has them from
# HhotateA's AvatarModifyTools (Copyright (c) 2021 @HhotateA_xR, MIT License) and Azukimochi's BoneRenamer
# (Copyright (c) 2023 Azukimochi, MIT License). MA's license is in THIRD_PARTY.md.

BONE_NAMES = (
    ('Hips', 'Hip', 'pelvis'),
    ('LeftUpperLeg', 'UpperLeg_Left', 'UpperLeg_L', 'Leg_Left', 'Leg_L', 'ULeg_L', 'Left leg', 'LeftUpLeg', 'UpLeg.L',
     'Thigh_L'),
    ('RightUpperLeg', 'UpperLeg_Right', 'UpperLeg_R', 'Leg_Right', 'Leg_R', 'ULeg_R', 'Right leg', 'RightUpLeg',
     'UpLeg.R', 'Thigh_R'),
    ('LeftLowerLeg', 'LowerLeg_Left', 'LowerLeg_L', 'Knee_Left', 'Knee_L', 'LLeg_L', 'Left knee', 'LeftLeg', 'leg_L',
     'shin.L'),
    ('RightLowerLeg', 'LowerLeg_Right', 'LowerLeg_R', 'Knee_Right', 'Knee_R', 'LLeg_R', 'Right knee', 'RightLeg',
     'leg_R', 'shin.R'),
    ('LeftFoot', 'Foot_Left', 'Foot_L', 'Ankle_L', 'Foot.L.001', 'Left ankle', 'heel.L', 'heel'),
    ('RightFoot', 'Foot_Right', 'Foot_R', 'Ankle_R', 'Foot.R.001', 'Right ankle', 'heel.R', 'heel'),
    ('Spine', 'spine01'),
    ('Chest', 'Bust', 'spine02', 'upper_chest'),
    ('Neck',),
    ('Head',),
    ('LeftShoulder', 'Shoulder_Left', 'Shoulder_L'),
    ('RightShoulder', 'Shoulder_Right', 'Shoulder_R'),
    ('LeftUpperArm', 'UpperArm_Left', 'UpperArm_L', 'Arm_Left', 'Arm_L', 'UArm_L', 'Left arm', 'UpperLeftArm'),
    ('RightUpperArm', 'UpperArm_Right', 'UpperArm_R', 'Arm_Right', 'Arm_R', 'UArm_R', 'Right arm', 'UpperRightArm'),
    ('LeftLowerArm', 'LowerArm_Left', 'LowerArm_L', 'LArm_L', 'Left elbow', 'LeftForeArm', 'Elbow_L', 'forearm_L',
     'ForArm_L'),
    ('RightLowerArm', 'LowerArm_Right', 'LowerArm_R', 'LArm_R', 'Right elbow', 'RightForeArm', 'Elbow_R', 'forearm_R',
     'ForArm_R'),
    ('LeftHand', 'Hand_Left', 'Hand_L', 'Left wrist', 'Wrist_L'),
    ('RightHand', 'Hand_Right', 'Hand_R', 'Right wrist', 'Wrist_R'),
    ('LeftToes', 'Toes_Left', 'Toe_Left', 'ToeIK_L', 'Toes_L', 'Toe_L', 'Foot.L.002', 'Left Toe', 'LeftToeBase'),
    ('RightToes', 'Toes_Right', 'Toe_Right', 'ToeIK_R', 'Toes_R', 'Toe_R', 'Foot.R.002', 'Right Toe', 'RightToeBase'),
    ('LeftEye', 'Eye_Left', 'Eye_L'),
    ('RightEye', 'Eye_Right', 'Eye_R'),
    ('Jaw',),
    ('LeftThumbProximal', 'ProximalThumb_Left', 'ProximalThumb_L', 'Thumb1_L', 'ThumbFinger1_L', 'LeftHandThumb1',
     'Thumb Proximal.L', 'Thunb1_L', 'finger01_01_L'),
    ('LeftThumbIntermediate', 'IntermediateThumb_Left', 'IntermediateThumb_L', 'Thumb2_L', 'ThumbFinger2_L',
     'LeftHandThumb2', 'Thumb Intermediate.L', 'Thunb2_L', 'finger01_02_L'),
    ('LeftThumbDistal', 'DistalThumb_Left', 'DistalThumb_L', 'Thumb3_L', 'ThumbFinger3_L', 'LeftHandThumb3',
     'Thumb Distal.L', 'Thunb3_L', 'finger01_03_L'),
    ('LeftIndexProximal', 'ProximalIndex_Left', 'ProximalIndex_L', 'Index1_L', 'IndexFinger1_L', 'LeftHandIndex1',
     'Index Proximal.L', 'finger02_01_L', 'f_index.01.L'),
    ('LeftIndexIntermediate', 'IntermediateIndex_Left', 'IntermediateIndex_L', 'Index2_L', 'IndexFinger2_L',
     'LeftHandIndex2', 'Index Intermediate.L', 'finger02_02_L', 'f_index.02.L'),
    ('LeftIndexDistal', 'DistalIndex_Left', 'DistalIndex_L', 'Index3_L', 'IndexFinger3_L', 'LeftHandIndex3',
     'Index Distal.L', 'finger02_03_L', 'f_index.03.L'),
    ('LeftMiddleProximal', 'ProximalMiddle_Left', 'ProximalMiddle_L', 'Middle1_L', 'MiddleFinger1_L', 'LeftHandMiddle1',
     'Middle Proximal.L', 'finger03_01_L', 'f_middle.01.L'),
    ('LeftMiddleIntermediate', 'IntermediateMiddle_Left', 'IntermediateMiddle_L', 'Middle2_L', 'MiddleFinger2_L',
     'LeftHandMiddle2', 'Middle Intermediate.L', 'finger03_02_L', 'f_middle.02.L'),
    ('LeftMiddleDistal', 'DistalMiddle_Left', 'DistalMiddle_L', 'Middle3_L', 'MiddleFinger3_L', 'LeftHandMiddle3',
     'Middle Distal.L', 'finger03_03_L', 'f_middle.03.L'),
    ('LeftRingProximal', 'ProximalRing_Left', 'ProximalRing_L', 'Ring1_L', 'RingFinger1_L', 'LeftHandRing1',
     'Ring Proximal.L', 'finger04_01_L', 'f_ring.01.L'),
    ('LeftRingIntermediate', 'IntermediateRing_Left', 'IntermediateRing_L', 'Ring2_L', 'RingFinger2_L', 'LeftHandRing2',
     'Ring Intermediate.L', 'finger04_02_L', 'f_ring.02.L'),
    ('LeftRingDistal', 'DistalRing_Left', 'DistalRing_L', 'Ring3_L', 'RingFinger3_L', 'LeftHandRing3', 'Ring Distal.L',
     'finger04_03_L', 'f_ring.03.L'),
    ('LeftLittleProximal', 'ProximalLittle_Left', 'ProximalLittle_L', 'Little1_L', 'LittleFinger1_L', 'LeftHandPinky1',
     'Little Proximal.L', 'finger05_01_L', 'f_pinky.01.L', 'Pinky1.L'),
    ('LeftLittleIntermediate', 'IntermediateLittle_Left', 'IntermediateLittle_L', 'Little2_L', 'LittleFinger2_L',
     'LeftHandPinky2', 'Little Intermediate.L', 'finger05_02_L', 'f_pinky.02.L', 'Pinky2.L'),
    ('LeftLittleDistal', 'DistalLittle_Left', 'DistalLittle_L', 'Little3_L', 'LittleFinger3_L', 'LeftHandPinky3',
     'Little Distal.L', 'finger05_03_L', 'f_pinky.03.L', 'Pinky3.L'),
    ('RightThumbProximal', 'ProximalThumb_Right', 'ProximalThumb_R', 'Thumb1_R', 'ThumbFinger1_R', 'RightHandThumb1',
     'Thumb Proximal.R', 'Thunb1_R', 'finger01_01_R'),
    ('RightThumbIntermediate', 'IntermediateThumb_Right', 'IntermediateThumb_R', 'Thumb2_R', 'ThumbFinger2_R',
     'RightHandThumb2', 'Thumb Intermediate.R', 'Thunb2_R', 'finger01_02_R'),
    ('RightThumbDistal', 'DistalThumb_Right', 'DistalThumb_R', 'Thumb3_R', 'ThumbFinger3_R', 'RightHandThumb3',
     'Thumb Distal.R', 'Thunb3_R', 'finger01_03_R'),
    ('RightIndexProximal', 'ProximalIndex_Right', 'ProximalIndex_R', 'Index1_R', 'IndexFinger1_R', 'RightHandIndex1',
     'Index Proximal.R', 'finger02_01_R', 'f_index.01.R'),
    ('RightIndexIntermediate', 'IntermediateIndex_Right', 'IntermediateIndex_R', 'Index2_R', 'IndexFinger2_R',
     'RightHandIndex2', 'Index Intermediate.R', 'finger02_02_R', 'f_index.02.R'),
    ('RightIndexDistal', 'DistalIndex_Right', 'DistalIndex_R', 'Index3_R', 'IndexFinger3_R', 'RightHandIndex3',
     'Index Distal.R', 'finger02_03_R', 'f_index.03.R'),
    ('RightMiddleProximal', 'ProximalMiddle_Right', 'ProximalMiddle_R', 'Middle1_R', 'MiddleFinger1_R',
     'RightHandMiddle1', 'Middle Proximal.R', 'finger03_01_R', 'f_middle.01.R'),
    ('RightMiddleIntermediate', 'IntermediateMiddle_Right', 'IntermediateMiddle_R', 'Middle2_R', 'MiddleFinger2_R',
     'RightHandMiddle2', 'Middle Intermediate.R', 'finger03_02_R', 'f_middle.02.R'),
    ('RightMiddleDistal', 'DistalMiddle_Right', 'DistalMiddle_R', 'Middle3_R', 'MiddleFinger3_R', 'RightHandMiddle3',
     'Middle Distal.R', 'finger03_03_R', 'f_middle.03.R'),
    ('RightRingProximal', 'ProximalRing_Right', 'ProximalRing_R', 'Ring1_R', 'RingFinger1_R', 'RightHandRing1',
     'Ring Proximal.R', 'finger04_01_R', 'f_ring.01.R'),
    ('RightRingIntermediate', 'IntermediateRing_Right', 'IntermediateRing_R', 'Ring2_R', 'RingFinger2_R',
     'RightHandRing2', 'Ring Intermediate.R', 'finger04_02_R', 'f_ring.02.R'),
    ('RightRingDistal', 'DistalRing_Right', 'DistalRing_R', 'Ring3_R', 'RingFinger3_R', 'RightHandRing3',
     'Ring Distal.R', 'finger04_03_R', 'f_ring.03.R'),
    ('RightLittleProximal', 'ProximalLittle_Right', 'ProximalLittle_R', 'Little1_R', 'LittleFinger1_R',
     'RightHandPinky1', 'Little Proximal.R', 'finger05_01_R', 'f_pinky.01.R', 'Pinky1.R'),
    ('RightLittleIntermediate', 'IntermediateLittle_Right', 'IntermediateLittle_R', 'Little2_R', 'LittleFinger2_R',
     'RightHandPinky2', 'Little Intermediate.R', 'finger05_02_R', 'f_pinky.02.R', 'Pinky2.R'),
    ('RightLittleDistal', 'DistalLittle_Right', 'DistalLittle_R', 'Little3_R', 'LittleFinger3_R', 'RightHandPinky3',
     'Little Distal.R', 'finger05_03_R', 'f_pinky.03.R', 'Pinky3.R'),
    ('UpperChest', 'UChest'))


def bone_key(s):
    """HeuristicBoneMapper.NormalizeName: "Bone_Upper_Leg.L" is "upperlegl\""""
    return re.sub(r'^bone_|[0-9 ._]', '', s.lower())


def _bone_maps():
    to_bones, to_names = {}, {}
    for i, names in enumerate(BONE_NAMES):
        for n in names:
            m = re.search(r'[_.]([LR])$', n)
            for k in (bone_key(n), bone_key(m.group(1) + '.' + n[:-2] if m else 'C.' + n)):  # "L.UpLeg", VRM's "C_Hips"
                to_bones.setdefault(k, []).append(i)
                if k not in to_names.setdefault(i, []):
                    to_names[i].append(k)
    return to_bones, to_names


ALL_BONE_KEYS = frozenset(bone_key(n) for names in BONE_NAMES for n in names)
NAME_TO_BONES, BONE_TO_NAMES = _bone_maps()


def go_name(g):
    return str(g.data.get('m_Name', ''))


def tf_find(t, path):
    """Transform.Find on the hierarchy as the files have it"""
    for part in path.split('/'):
        if part == '' or t is None:
            continue
        t = t.parent if part == '..' else next((c for c in t.children if go_name(c) == part), None)
    return t


def subtree(g):
    out, todo = [], [g]
    while todo:
        x = todo.pop()
        out.append(x)
        todo += reversed(x.children)
    return out


class OutfitSetup:
    """Setup Outfit on an outfit put under the avatar: specs, the Merge Armatures it adds (none if the outfit has one)"""

    def __init__(self, db, av, human, root):
        self.av, self.human, self.root = av, human, root
        self.name = go_name(root)
        self.specs, self.pairs = [], []  # pairs: (outfit bone, avatar bone), as matched
        gos = subtree(root)
        merges = [c for g in gos for c in g.comps if ma_kind(c) == 'MergeArmature']
        if merges and all(ma_objref(av, c.data.get('mergeTarget')) is not None for c in merges):
            log('%s: set up for Modular Avatar already' % self.name)
            return
        if any(f['@class'] == 'ArmatureLink' for g in gos for c in g.comps if is_vrcfury(c) for f in vrcf_features(c)):
            log('%s: set up for VRCFury already' % self.name)
            return
        ahips = human.get('Hips')
        if ahips is None or ahips.parent is None:
            warn('%s: the avatar has no humanoid Hips, so Setup Outfit cannot put it on' % self.name)
            return
        ohuman = humanoid_map(db, root, gos, quiet=True)
        ohips = ohuman.get('Hips')
        if ohips is None or ohips.parent is root:  # a rig with its hips at the top is taken for a broken one
            ohips, ohuman = None, {}
        if ohips is None:
            ohips = self.find_hips(go_name(ahips))
        if ohips is None or ohips.parent is None:
            if not any(c.cls in RENDERERS for g in gos for c in g.comps):  # an emote, a menu, animators: nothing to wear
                log('%s: no armature and no meshes, so nothing to merge' % self.name)
            else:
                warn('%s: Setup Outfit finds no hips in it, so it is not merged' % self.name)
            return
        m = MergeSpec(ohips.parent, ahips.parent)
        armature = go_name(m.go)
        own = next((c for c in m.go.comps if ma_kind(c) == 'MergeArmature'), None)
        if own is not None:  # its own, pointed at this avatar's armature (and MA leaves the unresolved one alone)
            m.prefix, m.suffix = _str(own.data.get('prefix')), _str(own.data.get('suffix'))
            m.mangle, m.comp = truthy(own.data.get('mangleNames', '1')), own
            log('%s: its Merge Armature targets nothing in this avatar; Setup Outfit points it at %s' % (
                self.name, go_name(m.target)))
        if not m.prefix and not m.suffix:
            self.infer(m, ahips)
        skipped, renamed = [], [0]
        obones = {id(g): HUMAN_BONES.index(h) for h, g in ohuman.items() if h in HUMAN_BONES} if ohuman else None

        def traverse(src, dst):
            for k, v in self.assign(m, src, dst, skipped, None, obones, human):
                new = m.prefix + go_name(v) + m.suffix
                if go_name(k) != new:
                    k.data['m_Name'] = new
                    renamed[0] += 1
                self.pairs.append((k, v))
                traverse(k, v)
        traverse(m.go, m.target)
        self.specs = [m]
        for sub in skipped:  # an UpperChest the avatar lacks: merged into what its parent merges into
            t = self.map_bone(m, sub.parent)
            if t is not None:
                self.specs.append(MergeSpec(sub, t, m.prefix, m.suffix, mangle=False))
        if not m.prefix and not m.suffix and tf_find(av.root, go_name(m.go)) is not None:
            m.go.data['m_Name'] = go_name(m.go) + '.1'  # so that Unity's humanoid mapping takes the avatar's
        log('Setup Outfit: %s/%s merges into %s%s: %d bones matched, %d renamed' % (
            self.name, armature, go_name(m.target), ' (bone names "%s…%s")' % (m.prefix, m.suffix) if m.prefix or m.suffix
            else '', len(self.pairs), renamed[0]))

    def find_hips(self, want):
        """FindBones: the outfit's hips, by name, under root/<child>/ or a level deeper"""
        extra = []
        for c in self.root.children:
            for h in c.children:
                if want in go_name(h):
                    return h
                extra.append(h)
        for c in extra:
            for h in c.children:
                if want in go_name(h):
                    return h
        cands = [want] + [n for n in BONE_TO_NAMES[0] if n != want]
        found, extra = None, []
        for c in self.root.children:
            for h in c.children:
                for k in cands:
                    if k in bone_key(go_name(h)):
                        found = h
                    extra.append(h)
        if found is None:
            for c in extra:
                for h in c.children:
                    if any(k in bone_key(go_name(h)) for k in cands):
                        found = h
        return found

    @staticmethod
    def strip(name, pre, suf):
        """a bone name without the prefix and suffix; None if it does not have them"""
        if name.startswith(pre) and name.endswith(suf) and len(name) > len(pre) + len(suf):
            return name[len(pre):len(name) - len(suf)]
        return None

    def count(self, merge, base, pre, suf):
        """how many bones the prefix and suffix match (MergeArmature.GetBonesMapping)"""
        n = 0
        for t in merge.children:
            nm = self.strip(go_name(t), pre, suf)
            b = tf_find(base, nm) if nm is not None else None
            if b is not None:
                n += 1 + self.count(t, b, pre, suf)
        return n

    def guess_count(self, root, pre, suf):
        """CountHeuristicMatches: the bones under root that are bone names between the prefix and suffix"""
        n = 1
        todo = [root]
        while todo:
            for c in todo.pop().children:
                nm = go_name(c)
                if (nm.startswith(pre) and nm.endswith(suf) and len(nm) >= len(pre) + len(suf) and
                        bone_key(nm[len(pre):len(nm) - len(suf)]) in ALL_BONE_KEYS):
                    n += 1
                    todo.append(c)
        return n

    def infer(self, m, ahips):
        """MergeArmature.InferPrefixSuffix"""
        if ahips.parent is not m.target or len(m.go.children) != 1:
            return
        cands = [(m.prefix, m.suffix, self.count(m.go, m.target, m.prefix, m.suffix))]
        base, mh = go_name(ahips), m.go.children[0]
        mn = go_name(mh)
        i = mn.find(base)
        if i >= 0:
            cands.append((mn[:i], mn[i + len(base):], self.count(m.go, m.target, mn[:i], mn[i + len(base):])))
        for h in sorted(BONE_NAMES[0], key=len, reverse=True):
            i = mn.lower().find(h.lower())
            if i >= 0:
                pre, suf = mn[:i], mn[i + len(h):]
                cands.append((pre, suf, (self.guess_count(mh, pre, suf) + 1) // 2))
                break
        best = max(cands, key=lambda c: c[2])
        if best[2] > 0:
            m.prefix, m.suffix = best[0], best[1]
        if m.prefix == 'J_Bip_C_':  # VRM
            m.prefix = 'J_Bip_'

    def assign(self, m, src, dst, skipped, unassigned, obones, human):
        """AssignBoneMappings: [(child of src, the child of dst it matches)], by name, then by the outfit's own
        humanoid rig, the avatar's and the name table"""
        out = {}
        guess = []
        if unassigned is None:
            unassigned = list(dst.children)
        for c in src.children:
            nm = self.strip(go_name(c), m.prefix, m.suffix)
            if nm is None:
                continue
            t = tf_find(dst, nm)
            if t is not None and any(x is t for x in unassigned):
                out[id(c)] = (c, t)
                unassigned.remove(t)
            else:
                guess.append(c)
        keys = {}
        for t in unassigned:
            keys[bone_key(go_name(t))] = t

        def take(c, t):
            out[id(c)] = (c, t)
            unassigned.remove(t)
            keys.pop(bone_key(go_name(t)), None)
        for c in guess:
            nm = self.strip(go_name(c), m.prefix, m.suffix)
            bones, done = None, False
            if obones is not None and id(c) in obones:
                t = human.get(HUMAN_BONES[obones[id(c)]]) if human is not None else None
                if t is not None and any(x is t for x in unassigned):
                    take(c, t)
                    done = True
                else:
                    bones = [obones[id(c)]]
            if not done and bones is None:
                bones = NAME_TO_BONES.get(bone_key(nm))
                if bones is None:
                    continue
            if not done and human is not None:
                for b in bones:
                    t = human.get(HUMAN_BONES[b])
                    if t is not None and any(x is t for x in unassigned):
                        take(c, t)
                        done = True
                        break
            if not done:
                for k in (k for b in bones for k in BONE_TO_NAMES[b]):
                    if k in keys:
                        t = keys.pop(k)
                        out[id(c)] = (c, t)
                        if t in unassigned:  # an UpperChest's children may have taken it already
                            unassigned.remove(t)
                        break
            if id(c) not in out and 54 in bones and skipped is not None:  # an UpperChest the avatar does not have
                skipped.append(c)
                for k, t in self.assign(m, c, dst, skipped, unassigned, None, None):
                    out[id(k)] = (k, t)
        return list(out.values())

    def map_bone(self, m, bone):
        """MergeArmature.MapBone: the avatar's bone an outfit bone merges into"""
        parts = []
        while bone is not None and bone is not m.go:
            parts.append(go_name(bone))
            bone = bone.parent
        if bone is None:
            return None
        t = m.target
        for seg in reversed(parts):
            nm = self.strip(seg, m.prefix, m.suffix)
            t = tf_find(t, nm) if nm is not None else None
            if t is None:
                return None
        return t

    def fix(self, U):
        """FixAPose: the outfit's shoulders and upper arms turned to point where the avatar's do, if they start where
        the avatar's do and are as long; then a warning if what it matched does not line up"""
        if not self.specs:
            return
        m = self.specs[0]
        others = {id(x.go) for x in self.specs[1:]}

        def outfit_bone(b):
            if b is None or id(b) not in U:
                return None
            parts, x = [], b
            while x is not None and x is not m.target:
                parts.append(go_name(x))
                x = x.parent
            if x is None:
                return None
            c = tf_find(m.go, '/'.join(m.prefix + p + m.suffix for p in reversed(parts)))
            x = c
            while x is not None and x is not m.go:
                if id(x) in others:
                    return None
                x = x.parent
            return c if c is not None and id(c) in U else None
        turned = 0
        for arm in (11, 12, 13, 14):  # the shoulders and upper arms, down to the upper and lower arms
            a0, a1 = self.human.get(HUMAN_BONES[arm]), self.human.get(HUMAN_BONES[arm + 2])
            o0, o1 = outfit_bone(a0), outfit_bone(a1)
            if o0 is None or o1 is None:
                continue
            pa, pl, qa, ql = (U[id(x)].translation for x in (a0, a1, o0, o1))
            if (pa - qa).length > 0.001 or abs((pl - pa).length - (ql - qa).length) > 0.001:
                continue
            if (ql - qa).length < 1e-9 or (pl - pa).length < 1e-9:
                continue
            q = (ql - qa).rotation_difference(pl - pa)
            if q.angle < 1e-5:
                continue
            R = Matrix.Translation(qa) @ q.to_matrix().to_4x4() @ Matrix.Translation(-qa)
            for x in subtree(o0):
                if id(x) in U:
                    U[id(x)] = R @ U[id(x)]
            turned += 1
        if turned:
            log('Setup Outfit: %s: %d arm bone(s) turned from an A pose to the avatar\'s' % (self.name, turned))
        far = [(k, v, (U[id(k)].translation - U[id(v)].translation).length) for k, v in self.pairs
               if id(k) in U and id(v) in U]
        far = [x for x in far if x[2] > 0.01]
        if far:
            k, v, d = max(far, key=lambda x: x[2])
            warn('%s: %d of its bones are more than 1 cm from the avatar\'s they merge into (%s: %.1f cm), so it may not '
                 'fit this avatar' % (self.name, len(far), go_name(v), d * 100))


def find_outfit(db, want):
    """the prefab (or model) --outfit names: a file, the prefab of a package or folder, or an asset's name"""
    path = os.path.abspath(os.path.expanduser(want))
    if os.path.exists(path):
        low = path.lower()
        if os.path.isfile(path) and not low.endswith(('.unitypackage', '.zip')):
            f = db.add_input(path)
            real = os.path.realpath(f)
            a = next((a for a in db.assets.values() if os.path.realpath(a.file) == real), None)
            if a is None:
                raise Fail('%s has no .meta file, so it is not an asset of its project' % want)
            return a
        if low.endswith('.unitypackage'):
            with tarfile.open(path, 'r:*') as tf:
                guids = {m.name.replace('\\', '/').lstrip('./').split('/')[0].lower() for m in tf}
            if not guids & set(db.assets):
                db.add_input(path)
            pool = [db.assets[g] for g in guids if g in db.assets]
        else:
            before = set(db.assets)
            db.add_input(path)
            new = set(db.assets) - before
            real = os.path.realpath(path)
            pool = [a for g, a in db.assets.items() if g in new or os.path.realpath(a.file).startswith(real + os.sep)]
        stem = plain_name(os.path.splitext(os.path.basename(path))[0])
    else:
        pool, stem = list(db.assets.values()), plain_name(want)
        low = want.lower()
        for test in (lambda a: a.name.lower() == low, lambda a: plain_name(a.name) == stem,
                     lambda a: low in a.name.lower(), lambda a: low in a.path.lower()):
            hits = [a for a in pool if (a.ext == '.prefab' or a.ext in MODEL_EXT) and test(a)]
            if hits:
                pool = hits
                break
        else:
            raise Fail('no prefab or model called "%s" in the input' % want)
    pool = [a for a in pool if a.ext == '.prefab'] or [a for a in pool if a.ext in MODEL_EXT]
    if not pool:
        raise Fail('no prefab or model in %s' % want)
    pool.sort(key=lambda a: (plain_name(a.name) != stem, stem not in plain_name(a.name), a.path.count('/'), a.path))
    if len(pool) > 1:
        log('--outfit %s: using %s (of %d; name one with --outfit NAME)' % (want, pool[0].path, len(pool)))
    return pool[0]


def put_on(db, found, a, k):
    """an outfit's prefab or model instantiated under the avatar's root, as dragged onto it in Unity"""
    w = World(db)
    w.nfbx = 1000 * k  # the outfit's models apart from the avatar's
    inst = w.instantiate(a.guid)
    if inst is None or inst.root_tf is None or inst.root_tf.go is None:
        raise Fail('%s cannot be put on the avatar: it has no single root object' % a.path)
    build_graph(inst)
    if any(is_descriptor(o) and not o.removed for o in inst.all):
        raise Fail('%s is an avatar, not an outfit' % a.path)
    root = inst.root_tf.go
    root.parent = found.root
    found.root.children.append(root)
    if found.root.tf is not None:
        inst.root_tf.data['m_Father'] = found.root.tf
    log('outfit %s (%s) put on "%s"' % (go_name(root), a.path, found.name))
    return root

# ---------------------------------------------------------------- Unity's humanoid clips, as VRM animations
#
# An Action layer (the avatar's own, one Modular Avatar merges in, a VRCFury Full Controller's) plays emotes and dances.
# Unity keeps a humanoid clip as "muscles": for each humanoid bone, up to three angles given as fractions of their
# limits (x a twist about the bone, y and z a swing), and the body's place (RootT, the centre of mass over the avatar's
# human scale) and turn (RootQ). Unity turns them into bone rotations with axes it works out from the avatar's T pose
# when it imports the model, and keeps nowhere in the files. This does the same, for the avatar's own T pose (its model's
# humanDescription skeleton, else the model as it stands), and writes each clip as a VRM animation (.vrma) the plugin
# retargets like any other: every humanoid bone's turn, the hips' place, and the faces the clip sets (by shape key name,
# as expressions of the same name, else as the VRM preset of an MMD name).
#
# A muscle m is an angle of m times its limit's max (m >= 0) or its min's negative (m < 0), times the bone's sign for that
# axis; the angles make a rotation twist-then-swing with each part as tan(angle / 2) (Unity's, not an exponential map); a
# bone's rotation from its parent is preQ * that * postQ^-1, the twist of an upper arm or leg shared with the bone below
# it, and the body goes where RootT and RootQ put its centre of mass and its frame (shoulders and hips). The muscle table,
# the signs, the masses and the twist sharing are from lox9973's work on Unity's humanoid: ShaderMotion (MIT License,
# Copyright 2020-2021 lox9973) and uvw.js (Apache License 2.0, Copyright 2022-2023 lox9973); see THIRD_PARTY.md. The
# rules that build preQ and postQ from a T pose were worked out from those avatars' axes and checked against Unity's own
# T pose clips. The jaw and the eyes are left as they are.

# the muscles: the clip's name for it, its bone (HumanBodyBones), axis (0 x twist, 1 y, 2 z), and default limits
_MUSCLE_ROWS = """Spine Front-Back,7,2,-40,40|Spine Left-Right,7,1,-40,40|Spine Twist Left-Right,7,0,-40,40
Chest Front-Back,8,2,-40,40|Chest Left-Right,8,1,-40,40|Chest Twist Left-Right,8,0,-40,40
UpperChest Front-Back,54,2,-20,20|UpperChest Left-Right,54,1,-20,20|UpperChest Twist Left-Right,54,0,-20,20
Neck Nod Down-Up,9,2,-40,40|Neck Tilt Left-Right,9,1,-40,40|Neck Turn Left-Right,9,0,-40,40
Head Nod Down-Up,10,2,-40,40|Head Tilt Left-Right,10,1,-40,40|Head Turn Left-Right,10,0,-40,40
Left Eye Down-Up,21,2,-10,15|Left Eye In-Out,21,1,-20,20|Right Eye Down-Up,22,2,-10,15|Right Eye In-Out,22,1,-20,20
Jaw Close,23,2,-10,10|Jaw Left-Right,23,1,-10,10
Left Upper Leg Front-Back,1,2,-90,50|Left Upper Leg In-Out,1,1,-60,60|Left Upper Leg Twist In-Out,1,0,-60,60
Left Lower Leg Stretch,3,2,-80,80|Left Lower Leg Twist In-Out,3,0,-90,90|Left Foot Up-Down,5,2,-50,50
Left Foot Twist In-Out,5,1,-30,30|Left Toes Up-Down,19,2,-50,50
Right Upper Leg Front-Back,2,2,-90,50|Right Upper Leg In-Out,2,1,-60,60|Right Upper Leg Twist In-Out,2,0,-60,60
Right Lower Leg Stretch,4,2,-80,80|Right Lower Leg Twist In-Out,4,0,-90,90|Right Foot Up-Down,6,2,-50,50
Right Foot Twist In-Out,6,1,-30,30|Right Toes Up-Down,20,2,-50,50
Left Shoulder Down-Up,11,2,-15,30|Left Shoulder Front-Back,11,1,-15,15|Left Arm Down-Up,13,2,-60,100
Left Arm Front-Back,13,1,-100,100|Left Arm Twist In-Out,13,0,-90,90|Left Forearm Stretch,15,2,-80,80
Left Forearm Twist In-Out,15,0,-90,90|Left Hand Down-Up,17,2,-80,80|Left Hand In-Out,17,1,-40,40
Right Shoulder Down-Up,12,2,-15,30|Right Shoulder Front-Back,12,1,-15,15|Right Arm Down-Up,14,2,-60,100
Right Arm Front-Back,14,1,-100,100|Right Arm Twist In-Out,14,0,-90,90|Right Forearm Stretch,16,2,-80,80
Right Forearm Twist In-Out,16,0,-90,90|Right Hand Down-Up,18,2,-80,80|Right Hand In-Out,18,1,-40,40"""
MUSCLES = []  # (the clip's attribute, bone, axis, min, max)
for _row in _MUSCLE_ROWS.replace('\n', '|').split('|'):
    _n, _b, _a, _lo, _hi = _row.split(',')
    MUSCLES.append((_n, int(_b), int(_a), float(_lo), float(_hi)))
for _side, _base in (('Left', 24), ('Right', 39)):  # the fingers: 1 Stretched, Spread, 2 Stretched, 3 Stretched
    for _f, (_fn, _lim) in enumerate((('Thumb', ((-20, 20), (-25, 25), (-40, 35), (-40, 35))),
                                      ('Index', ((-50, 50), (-20, 20), (-45, 45), (-45, 45))),
                                      ('Middle', ((-50, 50), (-7.5, 7.5), (-45, 45), (-45, 45))),
                                      ('Ring', ((-50, 50), (-7.5, 7.5), (-45, 45), (-45, 45))),
                                      ('Little', ((-50, 50), (-20, 20), (-45, 45), (-45, 45))))):
        _b = _base + 3 * _f
        for (_part, _bone, _axis), (_lo, _hi) in zip((('1 Stretched', _b, 2), ('Spread', _b, 1), ('2 Stretched', _b + 1, 2),
                                                      ('3 Stretched', _b + 2, 2)), _lim):
            MUSCLES.append(('%sHand.%s.%s' % (_side, _fn, _part), _bone, _axis, float(_lo), float(_hi)))
MUSCLE_OF = {m[0]: i for i, m in enumerate(MUSCLES)}
# a muscle of a bone the avatar lacks goes to the bone next to it, scaled by their limits (uvw.js's guess)
MUSCLE_FALLBACK = ((54, 8), (8, 7), (9, 10), (11, 13), (12, 14))
# per bone: the sign of each axis' angle (x, y, z) on the left (the right's are in _SIGN_R), the mass (parts of 165)
_SIGN_L = {0: (1, 1, 1), 7: (1, 1, 1), 8: (1, 1, 1), 54: (1, 1, 1), 9: (1, 1, 1), 10: (1, 1, 1), 1: (1, 1, 1),
           3: (1, -1, -1), 5: (1, 1, 1), 19: (1, 1, 1), 11: (1, 1, -1), 13: (1, 1, -1), 15: (1, 1, -1), 17: (1, 1, -1),
           21: (-1, 1, -1)}
_SIGN_R = {2: (-1, -1, 1), 4: (-1, 1, -1), 6: (-1, -1, 1), 20: (-1, -1, 1), 12: (-1, 1, 1), 14: (-1, 1, 1),
           16: (-1, 1, 1), 18: (-1, 1, 1), 22: (1, -1, -1)}
for _k in range(15):  # thumbs, index and middle, ring and little
    _SIGN_L[24 + _k] = (1, -1, 1) if _k < 3 else (-1, -1, -1) if _k < 9 else (1, 1, -1)
    _SIGN_R[39 + _k] = (-1, -1, -1) if _k < 3 else (1, -1, 1) if _k < 9 else (-1, 1, 1)
MUSCLE_SIGN = {**_SIGN_L, **_SIGN_R}
BONE_MASS = {0: 24, 1: 20, 2: 20, 3: 8, 4: 8, 5: 1.6, 6: 1.6, 7: 5, 8: 24, 54: 24, 9: 2, 10: 8, 11: 1, 12: 1, 13: 4, 14: 4,
             15: 3, 16: 3, 17: 1, 18: 1, 19: 0.4, 20: 0.4}
# VRM 1.0's names of Unity's humanoid bones (Unity's thumb proximal is VRM's metacarpal)
VRM_BONES = ['hips', 'leftUpperLeg', 'rightUpperLeg', 'leftLowerLeg', 'rightLowerLeg', 'leftFoot', 'rightFoot', 'spine',
             'chest', 'neck', 'head', 'leftShoulder', 'rightShoulder', 'leftUpperArm', 'rightUpperArm', 'leftLowerArm',
             'rightLowerArm', 'leftHand', 'rightHand', 'leftToes', 'rightToes', 'leftEye', 'rightEye', 'jaw'] + [
    '%s%s%s' % (s, f, p) for s in ('left', 'right') for f, ps in (
        ('Thumb', ('Metacarpal', 'Proximal', 'Distal')),) + tuple((f, ('Proximal', 'Intermediate', 'Distal')) for f in (
            'Index', 'Middle', 'Ring', 'Little')) for p in ps] + ['upperChest']
# MMD's face names and the VRM presets they are (for an avatar without shape keys of those names)
MMD_PRESETS = {'あ': 'aa', 'い': 'ih', 'う': 'ou', 'え': 'ee', 'お': 'oh', 'えー': 'ee', 'ワ': 'aa', 'まばたき': 'blink',
               'ウィンク': 'blinkLeft', 'ウィンク２': 'blinkLeft', 'ウィンク右': 'blinkRight', 'ｳｨﾝｸ２右': 'blinkRight',
               '笑い': 'happy', 'にこり': 'happy', 'にやり': 'happy', '怒り': 'angry', '困る': 'sad', 'びっくり': 'surprised',
               'なごみ': 'relaxed', 'じと目': 'relaxed'}


def _uq(x):
    """a Unity quaternion {x, y, z, w} as mathutils'"""
    return Quaternion((num(x.get('w'), 1.0), num(x.get('x')), num(x.get('y')), num(x.get('z')))).normalized()


def swing_twist(x, y, z):
    """Unity's muscle rotation for the angles (radians): a twist about X, then a swing, each as tan(angle / 2)"""
    tx, ty, tz = math.tan(x / 2), math.tan(y / 2), math.tan(z / 2)
    return Quaternion((1.0, tx, ty + tx * tz, tz - tx * ty)).normalized()


def _frame(X, refZ):
    """a rotation whose X axis is X, its Z as near refZ as that allows"""
    X = X.normalized()
    Z = (refZ - refZ.dot(X) * X).normalized()
    Y = Z.cross(X)
    return Matrix((X, Y, Z)).transposed().to_quaternion()


class HumanAxes:
    """Unity's muscle axes for an avatar in its T pose (Unity space, the avatar's root): per bone preQ, postQ, the signs
    and limits, and what the body is made of"""
    SPINE = (7, 8, 54, 9, 10)

    def __init__(self, tpose, human, limits=None, twists=None):
        """tpose: [(name, parent index or -1, Unity local Matrix)]; human: {HumanBodyBones index: tpose index};
        limits: {bone: (min Vector, max Vector)} the avatar's own; twists: (arm, forearm, upper leg, leg)"""
        self.names = [t[0] for t in tpose]
        self.parent = [t[1] for t in tpose]
        self.local = [t[2] for t in tpose]
        self.world = []
        for i, (_, p, L) in enumerate(tpose):
            self.world.append(self.world[p] @ L if p >= 0 else L.copy())
        self.human = dict(human)
        self.limits = limits or {}
        self.twists = twists or (0.5, 0.5, 0.5, 0.5)
        W = {b: self.world[i].to_quaternion() for b, i in self.human.items()}
        P = {b: self.world[i].translation for b, i in self.human.items()}
        self.pre, self.post, self.frame = {}, {}, {}
        up, down, fwd = Vector((0, 1, 0)), Vector((0, -1, 0)), Vector((0, 0, 1))

        def toward(b, nxt):
            for n in nxt:
                if n in P and (P[n] - P[b]).length > 1e-6:
                    return P[n] - P[b]
            return None
        for b in self.human:
            X, refZ, v = None, None, (0.0, 0.0, 0.0)
            side = 1 if b in (2, 4, 6, 12, 14, 16, 18, 20, 22) or 39 <= b <= 53 else 0
            s = -1.0 if side else 1.0
            if b == 0:
                F = Quaternion()
            else:
                if b in self.SPINE[:-1]:
                    X, refZ = toward(b, self.SPINE[self.SPINE.index(b) + 1:]), Vector((-1, 0, 0))
                elif b == 10:
                    X, refZ = up, Vector((-1, 0, 0))
                elif b in (11, 12):
                    X, refZ = toward(b, (b + 2,)), fwd
                elif b in (13, 14):
                    X, refZ, v = toward(b, (b + 2,)), fwd, (0.0, 0.268 * s, 0.364 * s)
                elif b in (15, 16):
                    X, refZ, v = toward(b, (b + 2,)), up, (0.0, 0.839 * s, 0.0)
                elif b in (17, 18):
                    X, refZ = toward(b - 2, (b,)), fwd
                elif b in (1, 2):
                    X, refZ, v = toward(b, (b + 2,)), Vector((1, 0, 0)), (-0.268, 0.0, 0.0)
                elif b in (3, 4):
                    X, refZ, v = toward(b, (b + 2,)), Vector((1, 0, 0)), (0.839, 0.0, 0.0)
                elif b in (5, 6):
                    X, refZ = down, Vector((1, 0, 0))
                elif b in (19, 20, 21, 22):
                    X, refZ = fwd, Vector((1, 0, 0))
                elif 24 <= b <= 53:
                    k = (b - 24) % 15
                    f, seg = divmod(k, 3)
                    refZ = up if f == 0 else fwd
                    X = toward(b, (b + 1,)) if seg < 2 else toward(b - 1, (b,))
                    if f == 0:
                        v = (0.0, 0.125 * s, 0.125 * s) if seg == 0 else (0.0, -0.2 * s, 0.0)
                    else:
                        v = (0.0, (0.08, 0.04, -0.04, -0.08)[f - 1] * s, 0.3 * s) if seg == 0 else (0.0, 0.0, 0.33 * s)
                else:
                    continue  # the jaw
                if X is None:
                    continue
                F = _frame(X, refZ)
            self.frame[b] = F
            self.post[b] = W[b].inverted() @ F
            N = Quaternion((1.0,) + tuple(v)).normalized()
            pi = self.parent[self.human[b]]
            Wp = self.world[pi].to_quaternion() if pi >= 0 else Quaternion()
            self.pre[b] = Wp.inverted() @ N @ F
        # the body: its centre of mass (at the middle of each bone with a mass) and its frame, in the T pose
        mass = dict(BONE_MASS)
        if 54 not in self.human:
            mass[8] = mass.get(8, 0) + mass.pop(54)
        self.mass = {b: m for b, m in mass.items() if b in self.human}
        self.length = {}
        nxt = {0: (7,), 7: (8, 54, 9), 8: (54, 9, 10), 54: (9, 10), 9: (10,), 1: (3,), 2: (4,), 3: (5,), 4: (6,),
               5: (19,), 6: (20,), 11: (13,), 12: (14,), 13: (15,), 14: (16,), 15: (17,), 16: (18,), 17: (30, 27, 24),
               18: (45, 42, 39)}
        for b in self.mass:
            d = toward(b, nxt.get(b, ()))
            self.length[b] = d.length if d is not None and b != 0 else 0.0
        if 10 in self.human and 9 in self.human:
            self.length[10] = (P[10] - P[9]).length
        if 19 in self.human and 5 in self.human:
            self.length[19] = self.length.get(5, 0.1) * 0.5
        self.scale = self.com(self.world_of(self.local)).y or 1.0
        self.body_t = self.body_frame(self.world_of(self.local))

    def world_of(self, local):
        out = []
        for i, L in enumerate(local):
            p = self.parent[i]
            out.append(out[p] @ L if p >= 0 else L)
        return out

    def plant(self, local, root_t, root_q, goals):
        """Unity's Foot IK (a state's m_IKOnFeet): each foot where the clip's goal (HumanClip.goals_at) puts it, by two-bone
        IK on the leg, the knee bending the way it did, and turned as the goal says. A goal is in the body's frame (RootT,
        RootQ) and human scales, the foot's turn times its muscle frame (post); a foot's is its sole, the ankle's height in
        the T pose below the ankle along the foot's frame's x (down). (That is what the clips' goals match: the VRSuya
        dances' feet within 5 cm and 3 degrees on average, their hands within 5 cm, though Foot IK is off there.)"""
        world = self.world_of(local)
        out = list(local)
        for foot, upper, lower in ((5, 1, 3), (6, 2, 4)):
            if foot not in goals or any(b not in self.human for b in (foot, upper, lower)) or foot not in self.post:
                continue
            T, Q = goals[foot]
            iu, il, iff = self.human[upper], self.human[lower], self.human[foot]
            turn = root_q @ Q  # the foot's frame
            h = self.world[iff].translation.y  # the ankle above the ground in the T pose
            target = root_q @ (T * self.scale) + root_t * self.scale - turn @ Vector((h, 0.0, 0.0))
            H, K, A = world[iu].translation, world[il].translation, world[iff].translation
            l1, l2 = (K - H).length, (A - K).length
            to = target - H
            d = min(max(to.length, abs(l1 - l2) + 1e-5), l1 + l2 - 1e-5)
            if l1 < 1e-6 or l2 < 1e-6 or to.length < 1e-6:
                continue
            aim = to.normalized()
            bend = (K - H) - aim * (K - H).dot(aim)  # the knee's way, off the line to the foot
            if bend.length < 1e-6:
                bend = world[iu].to_quaternion() @ Vector((0.0, 0.0, 1.0))
                bend = bend - aim * bend.dot(aim)
            bend.normalize()
            ca = min(max((l1 * l1 + d * d - l2 * l2) / (2 * l1 * d), -1.0), 1.0)
            K2 = H + (aim * ca + bend * math.sqrt(max(0.0, 1 - ca * ca))) * l1
            A2 = H + aim * d
            ru = (K - H).rotation_difference(K2 - H)
            wu = ru @ world[iu].to_quaternion()
            A1 = K2 + ru @ (A - K)
            rl = (A1 - K2).rotation_difference(A2 - K2)
            wl = rl @ ru @ world[il].to_quaternion()
            wf = turn @ self.post[foot].inverted()
            # what is between them turns along (twist bones, if any): the parents' new turns
            par = lambda i: world[self.parent[i]].to_quaternion() if self.parent[i] >= 0 else Quaternion()
            for i, q in ((iu, par(iu).inverted() @ wu), (il, (ru @ par(il)).inverted() @ wl),
                         (iff, (rl @ ru @ par(iff)).inverted() @ wf)):
                t, _, sc = local[i].decompose()
                out[i] = Matrix.LocRotScale(t, q, sc)
        return out

    def com(self, world):
        tot, s = 0.0, Vector((0.0, 0.0, 0.0))
        for b, m in self.mass.items():
            if b not in self.post:
                continue
            W = world[self.human[b]]
            c = W.translation + W.to_quaternion() @ (self.post[b] @ Vector((self.length.get(b, 0.0) / 2, 0, 0)))
            s += c * m
            tot += m
        return s / tot if tot else s

    def body_frame(self, world):
        """the body's turn: its x across the shoulders and hips, y from the hips up to the shoulders"""
        p = {b: world[self.human[b]].translation for b in (13, 14, 1, 2) if b in self.human}
        if len(p) < 4:
            return Quaternion()
        x = (p[14] + p[2]) - (p[13] + p[1])
        y = (p[13] + p[14]) - (p[1] + p[2])
        z = x.cross(y).normalized()
        y = y.normalized()
        x = y.cross(z)
        return Matrix((x, y, z)).transposed().to_quaternion()

    def pose(self, muscles, root_t, root_q):
        """(every T pose node's local Matrix for these muscle values, the body's place and turn)"""
        m = dict(muscles)
        vals = {}
        for i, (name, b, axis, lo, hi) in enumerate(MUSCLES):
            v = m.get(i, 0.0)
            if v:
                vals.setdefault(b, [0.0, 0.0, 0.0])[axis] += v
        for src, dst in MUSCLE_FALLBACK:
            if src not in self.human and src in vals:
                for a in range(3):
                    ms = next((x for x in MUSCLES if x[1] == src and x[2] == a), None)
                    md = next((x for x in MUSCLES if x[1] == dst and x[2] == a), None)
                    if ms and md and md[4]:
                        vals.setdefault(dst, [0.0, 0.0, 0.0])[a] += vals[src][a] * ms[4] / md[4]
        local = list(self.local)
        push = {}
        split = {13: (15, 0), 14: (16, 0), 15: (17, 1), 16: (18, 1), 1: (3, 2), 2: (4, 2), 3: (5, 3), 4: (6, 3)}
        for b in sorted(self.post, key=lambda b: self.human[b]):  # parents before children, as the T pose lists them
            ang = [0.0, 0.0, 0.0]
            for a in range(3):
                v = vals.get(b, (0.0, 0.0, 0.0))[a]
                if not v:
                    continue
                mu = next((x for x in MUSCLES if x[1] == b and x[2] == a), None)
                if mu is None:
                    continue
                lo, hi = self.limits.get(b, (None, None))
                lo = lo[a] if lo is not None else mu[3]
                hi = hi[a] if hi is not None else mu[4]
                ang[a] = math.radians(v * (hi if v >= 0 else -lo)) * MUSCLE_SIGN.get(b, (1, 1, 1))[a]
            pre = self.pre[b]
            if b in push:  # the twist the bone above left it
                pre = push[b] @ pre
            w = self.twists[split[b][1]] if b in split else 1.0
            q = pre @ swing_twist(ang[0] * w, ang[1], ang[2]) @ self.post[b].inverted()
            if b in split:
                push[split[b][0]] = self.post[b] @ swing_twist(ang[0] * (1 - w), 0, 0) @ self.post[b].inverted()
            i = self.human[b]
            t, _, sc = self.local[i].decompose()
            local[i] = Matrix.LocRotScale(t, q, sc)
        # the body: its centre of mass where RootT says (in human scales), turned as RootQ says from the T pose's frame
        world = self.world_of(local)
        hi = self.human[0]
        C0, B0 = self.com(world), self.body_frame(world)
        P0, H0 = world[hi].translation.copy(), world[hi].to_quaternion()
        D = root_q @ self.body_t @ B0.inverted()
        hips_w = Matrix.LocRotScale(root_t * self.scale - D @ (C0 - P0), D @ H0, world[hi].to_scale())
        p = self.parent[hi]
        local[hi] = (world[p].inverted() @ hips_w) if p >= 0 else hips_w
        return local


def unity_curve(keys, t):
    """a Unity AnimationCurve at t: cubic Hermite between keys by their slopes, flat beyond the ends; a key with an
    infinite out slope holds its value. Keys are (time, value, in slope, out slope), and a weighted one (its
    weightedMode 1 in, 2 out, 3 both) (..., in weight, out weight, mode): Unity makes the span a cubic Bezier in time
    and value, its handles that share of the span along the slopes (a third, on a side not weighted), which is the
    Hermite again when both are a third"""
    if not keys:
        return 0.0
    if t <= keys[0][0]:
        return keys[0][1]
    if t >= keys[-1][0]:
        return keys[-1][1]
    lo, hi = 0, len(keys) - 1
    while hi - lo > 1:
        mid = (lo + hi) // 2
        if keys[mid][0] <= t:
            lo = mid
        else:
            hi = mid
    k0, k1 = keys[lo], keys[hi]
    t0, v0, o0 = k0[0], k0[1], k0[3]
    t1, v1, i1 = k1[0], k1[1], k1[2]
    dt = t1 - t0
    if dt <= 0 or math.isinf(o0) or math.isinf(i1):
        return v0
    wo = min(max(k0[5], 0.0), 1.0) if len(k0) > 6 and int(k0[6]) & 2 else None
    wi = min(max(k1[4], 0.0), 1.0) if len(k1) > 6 and int(k1[6]) & 1 else None
    if wo is None and wi is None:
        s = (t - t0) / dt
        s2, s3 = s * s, s * s * s
        return (2 * s3 - 3 * s2 + 1) * v0 + (s3 - 2 * s2 + s) * o0 * dt + (-2 * s3 + 3 * s2) * v1 + (s3 - s2) * i1 * dt
    a = 1.0 / 3 if wo is None else wo
    b = 1.0 / 3 if wi is None else wi
    x = (t - t0) / dt  # the Bezier's time, 0..1 over the span: 3a(1-u)^2 u + 3(1-b)(1-u)u^2 + u^3, rising with u

    def bx(q):
        return 3 * a * (1 - q) ** 2 * q + 3 * (1 - b) * (1 - q) * q * q + q ** 3
    lo_u, hi_u = 0.0, 1.0
    for _ in range(48):
        mid = (lo_u + hi_u) / 2
        if bx(mid) < x:
            lo_u = mid
        else:
            hi_u = mid
    q = (lo_u + hi_u) / 2
    y1, y2 = v0 + a * dt * o0, v1 - b * dt * i1
    return (1 - q) ** 3 * v0 + 3 * (1 - q) ** 2 * q * y1 + 3 * (1 - q) * q * q * y2 + q ** 3 * v1


def _slope(x):
    try:
        v = float(x)
    except (TypeError, ValueError):
        return 0.0
    return v if not math.isnan(v) else 0.0


def curve_keys(c):
    """an AnimationCurve's keys as unity_curve takes them, sorted: the weights only on weighted ones"""
    out = []
    for k in listof(dictof(c.get('curve')).get('m_Curve')):
        if not isinstance(k, dict):
            continue
        key = (num(k.get('time')), num(k.get('value')), _slope(k.get('inSlope')), _slope(k.get('outSlope')))
        mode = inum(k.get('weightedMode'))
        if mode:
            key += (num(k.get('inWeight'), 1.0 / 3), num(k.get('outWeight'), 1.0 / 3), mode)
        out.append(key)
    return sorted(out)


GOALS = {'LeftFoot': 5, 'RightFoot': 6, 'LeftHand': 17, 'RightHand': 18}  # the IK goals: HumanBodyBones


class HumanClip:
    """a humanoid clip: its muscle, body and face curves"""

    def __init__(self, body):
        self.name = str(body.get('m_Name', ''))
        st = dictof(body.get('m_AnimationClipSettings'))
        self.loop = truthy(st.get('m_LoopTime', '0'))
        self.baked = tuple(truthy(st.get(k, '1')) for k in ('m_LoopBlendOrientation', 'm_LoopBlendPositionY',
                                                             'm_LoopBlendPositionXZ'))
        self.rate = min(max(num(body.get('m_SampleRate'), 60.0), 30.0), 60.0)
        self.muscles, self.root, self.faces = {}, {}, {}
        self.goals = {}  # "LeftFootT.x"...: where the feet and hands were, in the body's frame (see HumanAxes.plant)
        self.tdof = set()  # the bones it moves as well as turns (Translation DoF), which only some avatars take
        self.foot_ik = False  # its state has Foot IK on (action_clips)
        end = 0.0
        for c in listof(body.get('m_FloatCurves')) or listof(body.get('m_EditorCurves')):
            c = dictof(c)
            keys = curve_keys(c)
            if not keys:
                continue
            attr, cid = str(c.get('attribute') or ''), inum(c.get('classID'))
            if cid == 95 and attr in MUSCLE_OF:
                self.muscles[MUSCLE_OF[attr]] = keys
            elif cid == 95 and re.fullmatch(r'Root[TQ]\.[xyzw]', attr):
                self.root[attr] = keys
            elif cid == 95 and re.fullmatch(r'(Left|Right)(Foot|Hand)[TQ]\.[xyzw]', attr):
                self.goals[attr] = keys
                continue  # (they add nothing to its length)
            elif cid == 95 and re.fullmatch(r'\w+TDOF\.[xyz]', attr):
                self.tdof.add(attr[:-6])
                continue
            elif cid == 137 and attr.startswith('blendShape.'):
                self.faces.setdefault(attr[11:], keys)
            else:
                continue
            end = max(end, keys[-1][0])
        self.length = max(num(st.get('m_StopTime'), end) - num(st.get('m_StartTime'), 0.0), 0.0) or end

    @property
    def humanoid(self):
        return bool(self.muscles) or bool(self.root)

    def at(self, t):
        """(muscles {index: value}, RootT, RootQ) at time t"""
        mus = {i: unity_curve(k, t) for i, k in self.muscles.items()}
        g = lambda a, d: unity_curve(self.root[a], t) if a in self.root else d
        rt = Vector((g('RootT.x', 0.0), g('RootT.y', 1.0), g('RootT.z', 0.0)))
        rq = Quaternion((g('RootQ.w', 1.0), g('RootQ.x', 0.0), g('RootQ.y', 0.0), g('RootQ.z', 0.0)))
        rq = rq.normalized() if rq.magnitude > 1e-6 else Quaternion()
        return mus, rt, rq

    def goals_at(self, t):
        """{HumanBodyBones foot or hand: (T, Q)} at time t, of the goals the clip has"""
        out = {}
        for name, b in GOALS.items():
            if not any(name + 'T.' + c in self.goals for c in 'xyz'):
                continue
            g = lambda a, d: unity_curve(self.goals[name + a], t) if name + a in self.goals else d
            q = Quaternion((g('Q.w', 1.0), g('Q.x', 0.0), g('Q.y', 0.0), g('Q.z', 0.0)))
            out[b] = (Vector((g('T.x', 0.0), g('T.y', 0.0), g('T.z', 0.0))),
                      q.normalized() if q.magnitude > 1e-6 else Quaternion())
        return out


def human_tpose(db, av, human, U0):
    """the avatar's T pose, as Unity's humanoid knows it: (HumanAxes, the humanoid's node names) from its model's
    humanDescription skeleton; else from the avatar as it stands (U0: the Unity matrices before MA moved anything)"""
    an = next((c for c in av.root.comps if c.cls == 95), None)
    g = ref(an.data.get('m_Avatar'))[1] if an is not None else None
    imp = db.importer(g) if g else {}
    hd = dictof(imp.get('humanDescription'))
    twists = tuple(num(hd.get(k), 0.5) for k in ('armTwist', 'foreArmTwist', 'upperLegTwist', 'legTwist'))
    names = {str(dictof(h).get('humanName') or ''): str(dictof(h).get('boneName') or '') for h in listof(hd.get('human'))}
    limits = {}
    for h in listof(hd.get('human')):
        h = dictof(h)
        lim = dictof(h.get('limit'))
        if truthy(lim.get('modified', '0')) and h.get('humanName') in HUMAN_BONES:
            limits[HUMAN_BONES.index(h['humanName'])] = (Vector(vec3(lim.get('min'))), Vector(vec3(lim.get('max'))))
    skel = [dictof(s) for s in listof(hd.get('skeleton'))]
    tpose, index = [], {}
    if skel:
        for s in skel:
            nm = str(s.get('name') or '')
            L = Matrix.LocRotScale(Vector(vec3(s.get('position'))), _uq(dictof(s.get('rotation'))),
                                   Vector(vec3(s.get('scale'), (1.0, 1.0, 1.0))))
            index.setdefault(nm, len(tpose))
            tpose.append([nm, str(s.get('parentName') or ''), L])
        for t in tpose:
            t[1] = index.get(t[1], -1) if t[1] != t[0] else -1
            if t[1] < 0:  # the model's root: the Animator's, whose space the humanoid is in
                t[2] = Matrix.Identity(4)
        hum = {HUMAN_BONES.index(h): index[b] for h, b in names.items() if h in HUMAN_BONES and b in index}
        if 0 in hum and len(hum) >= 10:
            axes = HumanAxes([tuple(t) for t in tpose], hum, limits, twists)
            axes.tdof = truthy(hd.get('hasTranslationDoF', '0'))
            return axes, [t[0] for t in tpose]
    # no skeleton in the model's settings: the avatar's own bones where they stand
    keep = set()
    for g2 in human.values():
        while g2 is not None and id(g2) in av.inside and g2 is not av.root:
            keep.add(id(g2))
            g2 = g2.parent
    order = [g2 for g2 in av.gos if id(g2) in keep]
    pos = {id(g2): i for i, g2 in enumerate(order)}
    for g2 in order:
        p = g2.parent if g2.parent is not None and id(g2.parent) in pos else None
        L = U0[id(p)].inverted_safe() @ U0[id(g2)] if p is not None else U0[id(g2)]
        tpose.append((go_name(g2), pos[id(p)] if p is not None else -1, L))
    hum = {HUMAN_BONES.index(h): pos[id(g2)] for h, g2 in human.items() if h in HUMAN_BONES and id(g2) in pos}
    if 0 not in hum:
        return None, []
    axes = HumanAxes(tpose, hum, limits, twists)
    axes.tdof = truthy(hd.get('hasTranslationDoF', '0'))
    return axes, [t[0] for t in tpose]


def action_clips(an):
    """the humanoid clips the avatar's Action layers play (its own, Modular Avatar's Merge Animators', VRCFury's Full
    Controllers'): [(name, HumanClip, loop, speed, {the parameters that start it})], named as the menu item that starts
    it, else as its state"""
    db, av = an.db, an.av
    ctls = []  # (controller guid, {its parameter names: the avatar's})
    d = av.desc
    if truthy(d.get('customizeAnimationLayers', '0')):
        for l in listof(d.get('baseAnimationLayers')):
            l = dictof(l)
            g = ref(l.get('animatorController'))[1]
            if inum(l.get('type')) == 4 and not truthy(l.get('isDefault', '0')) and g and db.get(g):
                ctls.append((g, {}))
    for c in (an.mat.comps.get('MergeAnimator', []) if an.mat else []):
        g = ref(c.data.get('animator'))[1]
        if inum(c.data.get('layerType'), 5) == 4 and g and db.get(g):
            ctls.append((g, an.mat.view(c.go)))
    for c, f in (an.vrcf.feats if an.vrcf else []):
        if f['@class'] == 'FullController':
            for e in listof(f.get('controllers')):
                p = vrcf_asset(dictof(e).get('controller'))
                if p and inum(dictof(e).get('type'), 5) == 4 and db.get(p[0]):
                    ctls.append((p[0], {}))
    out, seen = [], set()
    by_param = {}
    for prefix, c in an.menu:
        pn = _str(dictof(c.get('parameter')).get('name'))
        if pn:
            by_param.setdefault(pn, []).append((num(c.get('value'), 1.0), rich_text(_str(c.get('name')))))
    for g, view in ctls:
        uf = db.yaml(g)
        if uf is None or uf.binary:
            continue
        into = {}  # state -> [(parameter, value)] of the transitions to it
        for fid in uf.order:
            if uf.cls(fid) != 1101:
                continue
            t = uf.get(fid)[1]
            dst = ref(t.get('m_DstState'))[0]
            if dst:
                for cd in listof(t.get('m_Conditions')):
                    cd = dictof(cd)
                    if inum(cd.get('m_ConditionMode')) in (1, 3, 6):
                        pn = str(cd.get('m_ConditionEvent') or '')
                        into.setdefault(dst, []).append((view.get(pn, pn), num(cd.get('m_EventTreshold'))))
        for fid in uf.order:
            if uf.cls(fid) != 1102:
                continue
            st = uf.get(fid)[1]
            p = ptr(st.get('m_Motion'), g)
            a = db.get(p[0]) if p else None
            if a is None or a.ext in MODEL_EXT or p in seen:
                continue
            cu = db.yaml(p[0])
            if cu is None or cu.binary or cu.cls(p[1]) != 74:
                continue
            clip = HumanClip(cu.get(p[1])[1])
            if not clip.humanoid:  # (one of no length is a pose, held)
                continue
            clip.foot_ik = truthy(st.get('m_IKOnFeet', '0')) and bool(clip.goals)
            seen.add(p)
            params = {pn for pn, _ in into.get(fid, [])}
            name = ''
            for pn, v in into.get(fid, []):
                hit = next((n for val, n in by_param.get(pn, []) if abs(val - v) < 0.5 or v == 0), None)
                if hit:
                    name = hit
                    break
            name = name or str(st.get('m_Name', '')).replace('_', ' ').strip() or clip.name
            out.append((name, clip, clip.loop and clip.length > 0, num(st.get('m_Speed'), 1.0) or 1.0, params))
    return out


def write_vrma(path, axes, names, clip, shapes, speed=1.0):
    """a VRM animation of a humanoid clip on the avatar's T pose: its bones' turns, the hips' place, the faces (shapes:
    the avatar's shape key names, to tell the MMD faces it has from the ones to give as VRM presets)"""
    fps = clip.rate
    n = max(1, int(math.ceil(clip.length * fps - 1e-6)))
    times = [clip.length * k / n for k in range(n + 1)] if clip.length > 0 else [0.0]
    used = set()  # the T pose nodes the humanoid needs: its bones and what is above them
    for b, i in axes.human.items():
        while i >= 0 and i not in used:
            used.add(i)
            i = axes.parent[i]
    order = [i for i in range(len(names)) if i in used]
    node_of = {i: k for k, i in enumerate(order)}
    anim = sorted(b for b in axes.post if b not in (21, 22, 23))
    rots = {b: [] for b in anim}
    hips = []
    t0 = None
    for t in times:
        mus, rt, rq = clip.at(t)
        if not all(clip.baked):  # what the clip leaves to root motion stays where it starts
            if t0 is None:
                t0 = (rt.copy(), rq.copy())
            if not clip.baked[2]:
                rt.x, rt.z = t0[0].x, t0[0].z
            if not clip.baked[1]:
                rt.y = t0[0].y
            if not clip.baked[0]:
                yaw = rq.to_euler('YXZ').y - t0[1].to_euler('YXZ').y
                rq = Quaternion((0, 1, 0), -yaw) @ rq
        local = axes.pose(mus, rt, rq)
        if getattr(clip, 'foot_ik', False):
            local = axes.plant(local, rt, rq, clip.goals_at(t))
        for b in anim:
            q = local[axes.human[b]].to_quaternion()
            rots[b].append((q.x, -q.y, -q.z, q.w))  # Unity's left-handed space mirrored in X: glTF's
        tr = local[axes.human[0]].translation
        hips.append((-tr.x, tr.y, tr.z))
    # the faces: an expression node each, its x the weight
    faces = []
    for nm, keys in sorted(clip.faces.items()):
        if nm in shapes:
            faces.append(('custom', nm, keys))
        elif nm in MMD_PRESETS:
            faces.append(('preset', MMD_PRESETS[nm], keys))
    merged = {}
    for kind, nm, keys in faces:  # two MMD faces of one preset: the stronger
        merged.setdefault((kind, nm), []).append(keys)
    buf = bytearray()
    views, accessors = [], []

    def add(data, typ, fmt, count, minmax=None):
        while len(buf) % 4:
            buf.append(0)
        off = len(buf)
        buf.extend(struct.pack('<%d%s' % (len(data), fmt), *data))
        views.append({'buffer': 0, 'byteOffset': off, 'byteLength': len(buf) - off})
        a = {'bufferView': len(views) - 1, 'componentType': 5126, 'count': count, 'type': typ}
        if minmax:
            a['min'], a['max'] = minmax
        accessors.append(a)
        return len(accessors) - 1
    ta = add([float(x) for x in times], 'SCALAR', 'f', len(times), ([times[0]], [times[-1]]))
    nodes = []
    for k, i in enumerate(order):
        t, q, s = axes.local[i].decompose()
        nd = {'name': names[i], 'translation': [-t.x, t.y, t.z], 'rotation': [q.x, -q.y, -q.z, q.w]}
        if any(abs(v - 1) > 1e-6 for v in s):
            nd['scale'] = list(s)
        kids = [node_of[j] for j in order if axes.parent[j] == i]
        if kids:
            nd['children'] = kids
        nodes.append(nd)
    roots = [node_of[i] for i in order if axes.parent[i] not in node_of]
    channels, samplers = [], []
    for b in anim:
        a = add([v for q in rots[b] for v in q], 'VEC4', 'f', len(times))
        samplers.append({'input': ta, 'output': a, 'interpolation': 'LINEAR'})
        channels.append({'sampler': len(samplers) - 1, 'target': {'node': node_of[axes.human[b]], 'path': 'rotation'}})
    a = add([v for p in hips for v in p], 'VEC3', 'f', len(times))
    samplers.append({'input': ta, 'output': a, 'interpolation': 'LINEAR'})
    channels.append({'sampler': len(samplers) - 1, 'target': {'node': node_of[axes.human[0]], 'path': 'translation'}})
    expressions = {}
    for (kind, nm), curves in merged.items():
        nodes.append({'name': 'Expression.' + nm})
        k = len(nodes) - 1
        roots.append(k)
        w = [min(max(max(unity_curve(c, t) for c in curves) / 100.0, 0.0), 1.0) for t in times]
        a = add([v for x in w for v in (x, 0.0, 0.0)], 'VEC3', 'f', len(times))
        samplers.append({'input': ta, 'output': a, 'interpolation': 'LINEAR'})
        channels.append({'sampler': len(samplers) - 1, 'target': {'node': k, 'path': 'translation'}})
        expressions.setdefault(kind, {})[nm] = {'node': k}
    ext = {'specVersion': '1.0', 'humanoid': {'humanBones': {VRM_BONES[b]: {'node': node_of[i]}
                                                            for b, i in sorted(axes.human.items())}}}
    if expressions:
        ext['expressions'] = expressions
    js = {'asset': {'version': '2.0', 'generator': 'unity2hypr3d'}, 'scene': 0, 'scenes': [{'nodes': roots}],
          'nodes': nodes, 'animations': [{'name': clip.name, 'channels': channels, 'samplers': samplers}],
          'accessors': accessors, 'bufferViews': views, 'buffers': [{'byteLength': len(buf)}],
          'extensionsUsed': ['VRMC_vrm_animation'], 'extensions': {'VRMC_vrm_animation': ext}}
    if speed != 1.0:
        js['extras'] = {'speed': speed}
    write_glb(path, js, bytes(buf))
    return times[-1], len(faces)


# ---------------------------------------------------------------- Unity and Blender space

if Matrix is not None:
    FLIP = Matrix.Diagonal((-1.0, 1.0, 1.0, 1.0))
    AXIS = Matrix(((1, 0, 0, 0), (0, 0, 1, 0), (0, -1, 0, 0), (0, 0, 0, 1)))  # Blender (x, y, z) -> (x, z, -y)
    AXIS_INV = AXIS.inverted()


def to_unity(M, u):
    """a Blender world matrix as Unity has it: left-handed, Y up, positions in meters"""
    U = FLIP @ AXIS @ M @ FLIP
    for c in range(3):
        for r in range(3):
            U[r][c] /= u
    return U


def from_unity(U, u):
    M = AXIS_INV @ FLIP @ U @ FLIP
    for c in range(3):
        for r in range(3):
            M[r][c] *= u
    return M


def trs_merge(M, pos, rot, scl):
    """a local matrix with a Transform's position, rotation and scale over it (overrides may set only some)"""
    t, q, s = M.decompose()
    t, s, q = list(t), list(s), [q.w, q.x, q.y, q.z]
    for d, v in ((pos, t), (scl, s)):
        if isinstance(d, dict):
            for i, k in enumerate('xyz'):
                if k in d:
                    v[i] = num(d[k], v[i])
    if isinstance(rot, dict):
        given = {i: num(rot[k]) for i, k in enumerate('wxyz') if k in rot}
        if 0 < len(given) < 4 and sum(q[i] * v for i, v in given.items()) < 0:
            q = [-x for x in q]
        for i, v in given.items():
            q[i] = v
    qq = Quaternion(q)
    qq = qq.normalized() if qq.magnitude > 1e-8 else Quaternion()
    return Matrix.LocRotScale(Vector(t), qq, Vector(s))


def bl_name(s):
    """a name as Blender keeps it: at most 63 bytes"""
    return s.encode('utf-8')[:63].decode('utf-8', 'ignore')


def _base_name(s):
    return re.sub(r'\.\d{3,}$', '', s)


def transform_rests(av, U):
    """every GameObject's Transform in local values, as Unity has the avatar placed (what its animations' Transform
    curves start from): position, rotation (quaternion x, y, z, w), Euler angles (Unity's, degrees: Z, then X, then
    Y) and scale"""
    for g in av.gos:
        if id(g) not in U:
            continue
        P = U[id(g.parent)] if g is not av.root and g.parent is not None and id(g.parent) in U else Matrix.Identity(4)
        t, q, sc = (P.inverted_safe() @ U[id(g)]).decompose()
        d = g.tf.data if g.tf is not None else {}
        own = d.get('m_LocalRotation')
        if isinstance(own, dict) and sum(a * num(own.get(k)) for a, k in zip((q.x, q.y, q.z, q.w), 'xyzw')) < 0:
            q = Quaternion((-q.w, -q.x, -q.y, -q.z))  # the sign the Transform has
        e = [math.degrees(a) for a in q.to_matrix().to_euler('ZXY')]
        hint = d.get('m_LocalEulerAnglesHint')
        if isinstance(hint, dict):  # the editor's angles (maybe past 180), when they are the same turn
            h = [num(hint.get(k)) for k in 'xyz']
            H = Euler([math.radians(a) for a in h], 'ZXY').to_quaternion()
            if abs(H.dot(q)) > 1 - 1e-6:
                e = h
        av.tf_rest[id(g)] = {'p': tuple(t), 'q': (q.x, q.y, q.z, q.w), 'e': tuple(e), 's': tuple(sc)}


def tf_local(comps, rest):
    """a Transform's local matrix (Unity's) with these of its components set ({'px': ..., 'qw': ..., 'ey': ...,
    'sx': ..., 'm': a scale multiplier}) over the rest ({'p', 'q', 'e', 's'})"""
    t = Vector([comps.get('p' + a, rest['p'][i]) for i, a in enumerate('xyz')])
    if any(('q' + a) in comps for a in 'xyzw'):
        q = Quaternion([comps.get('q' + a, rest['q'][i]) for i, a in zip((3, 0, 1, 2), 'wxyz')])
        q = q.normalized() if q.magnitude > 1e-9 else Quaternion()
    elif any(('e' + a) in comps for a in 'xyz'):
        q = Euler([math.radians(comps.get('e' + a, rest['e'][i])) for i, a in enumerate('xyz')], 'ZXY').to_quaternion()
    else:
        q = Quaternion((rest['q'][3], rest['q'][0], rest['q'][1], rest['q'][2]))
    sc = Vector([comps.get('s' + a, rest['s'][i]) for i, a in enumerate('xyz')]) * comps.get('m', 1.0)
    return Matrix.LocRotScale(t, q, sc)


# ---------------------------------------------------------------- the Blender scene

class Build:
    """the avatar in Blender: its models imported, placed as Unity has them, and exported as a GLB"""

    def __init__(self, db, av, opts):
        self.db, self.av, self.opts = db, av, opts
        self.mats = av.materials()
        self.fis = []
        seen = set()
        for g in av.gos:
            if g.fbx is not None and id(g.fbx) not in seen:
                seen.add(id(g.fbx))
                self.fis.append(g.fbx)
        self.U = {}  # id(GameObject) -> its Unity world matrix, the avatar standing at the origin
        self.owner = {}  # Blender object pointer -> (FBXInst, model id)
        self.imported = []  # (FBXInst, [its Blender objects])
        self.post = {}  # Blender material name -> MatInfo
        self.keys = {}  # id(renderer) -> {its mesh's shape name: the shape key's name in Blender}
        self.variant_names = {}  # the material variants made: the name asked for -> the GLB's
        self._images = {}
        self._relinked = set()
        self._tmp = 0
        self._made = {}  # MatInfo key -> Blender material
        self._own = {}  # (id(renderer), slot) -> the Blender material the model came with

    # ---- import

    def _tmpname(self):
        self._tmp += 1
        return '~u2h%d' % self._tmp

    def import_models(self):
        for coll in (bpy.data.objects, bpy.data.meshes, bpy.data.materials, bpy.data.images, bpy.data.armatures,
                     bpy.data.cameras, bpy.data.lights):
            for x in list(coll):
                coll.remove(x)
        for fi in self.fis:
            a = self.db.get(fi.guid)
            me = dictof(self.db.importer(fi.guid).get('meshes'))
            gs = num(me.get('globalScale'), 1.0) or 1.0
            use_file = truthy(me.get('useFileScale', '1'))
            fi.u = gs * (fi.info.unit / 100.0 if use_file else 1.0)
            for o in bpy.data.objects:  # so that the new objects get their names exactly
                o.name = self._tmpname()
            before = {o.as_pointer() for o in bpy.data.objects}
            log('importing %s' % a.path)
            try:
                bpy.ops.import_scene.fbx(
                    # the importer always applies the file's unit: this undoes that when Unity does not
                    filepath=a.file, global_scale=gs * (1.0 if use_file else 100.0 / fi.info.unit),
                    use_custom_normals=True, use_image_search=True,
                    ignore_leaf_bones=False, automatic_bone_orientation=False, force_connect_children=False,
                    use_anim=False, use_custom_props=False, use_prepost_rot=True, use_manual_orientation=False,
                    bake_space_transform=False, primary_bone_axis='Y', secondary_bone_axis='X',
                    mtl_name_collision_mode='MAKE_UNIQUE')
            except (RuntimeError, TypeError, ValueError) as e:
                lines = [l for l in str(e).splitlines() if l.strip()]
                warn('Blender cannot import %s: %s' % (a.path, lines[-1] if lines else e))
                continue
            new = [o for o in bpy.data.objects if o.as_pointer() not in before]
            if bpy.context.object is not None and bpy.context.object.mode != 'OBJECT':
                bpy.ops.object.mode_set(mode='OBJECT')
            for o in new:  # a connected bone cannot move off its parent's tail
                if o.type == 'ARMATURE' and any(b.use_connect for b in o.data.bones):
                    for x in bpy.context.selected_objects:
                        x.select_set(False)
                    o.select_set(True)
                    bpy.context.view_layer.objects.active = o
                    bpy.ops.object.mode_set(mode='EDIT')
                    for eb in o.data.edit_bones:
                        eb.use_connect = False
                    bpy.ops.object.mode_set(mode='OBJECT')
            self.map_models(fi, new)
            self.imported.append((fi, new))
            bpy.context.view_layer.update()
            for mid, b in fi.bl.items():
                M = b[1].matrix_world @ b[1].pose.bones[b[2]].matrix if b[0] == 'bone' else b[1].matrix_world
                fi.orig[mid] = to_unity(M, fi.u)
        if not self.imported:
            raise Fail('none of the avatar\'s models could be imported')

    def map_models(self, fi, new):
        """which Blender object or bone each FBX model became"""
        info = fi.info
        objs, bones = {}, {}
        for o in new:
            objs.setdefault(_base_name(o.name), []).append(o)
            if o.type == 'ARMATURE':
                for b in o.data.bones:
                    bones.setdefault(_base_name(b.name), []).append((o, b.name))

        def key_of(c):
            return ('obj', c[1].as_pointer()) if c[0] == 'obj' else ('bone', c[1].as_pointer(), c[2])

        def parent_of(c):
            if c[0] == 'obj':
                o = c[1]
                if o.parent is None:
                    return None
                if o.parent_type == 'BONE':
                    return ('bone', o.parent.as_pointer(), o.parent_bone)
                return ('obj', o.parent.as_pointer())
            b = c[1].data.bones[c[2]]
            return ('bone', c[1].as_pointer(), b.parent.name) if b.parent else ('obj', c[1].as_pointer())

        def named(n, nm, key):
            return n == nm or re.fullmatch(re.escape(key) + r'\.\d{3,}', n) is not None
        order = []

        def walk(p):
            for m in info.kids.get(p, []):
                order.append(m)
                walk(m)
        walk(0)
        taken = set()
        for mid in order:
            nm = bl_name(info.name(mid))
            if not nm:
                continue
            key = _base_name(nm)
            oc = [('obj', o) for o in sorted(objs.get(key, []), key=lambda o: (o.name != nm, o.name))
                  if named(o.name, nm, key)]
            bc = [('bone', o, n) for o, n in sorted(bones.get(key, []), key=lambda b: (b[1] != nm, b[1]))
                  if named(n, nm, key)]
            cands = [c for c in ((bc + oc) if info.kind(mid) in ('LimbNode', 'Limb') else (oc + bc))
                     if key_of(c) not in taken]
            if not cands:
                continue
            pb = fi.bl.get(info.models[mid][2])
            want = key_of(pb) if pb else None
            pick = next((c for c in cands if parent_of(c) == want), cands[0])
            taken.add(key_of(pick))
            fi.bl[mid] = pick
            if pick[0] == 'obj':
                self.owner[pick[1].as_pointer()] = (fi, mid)

    # ---- where everything goes

    def counterpart(self, g):
        """the Blender object or bone a GameObject became"""
        fi = g.fbx
        if fi is None or g.node is None or fi.gos.get(g.node) is not g:
            return None
        return fi.bl.get(g.node)

    def anchor(self, g):
        """the Blender object or bone standing for the nearest of a GameObject and its parents"""
        while g is not None and id(g) in self.av.inside:
            b = self.counterpart(g)
            if b is not None:
                return b
            g = g.parent
        return None

    def compute(self, human):
        """every GameObject's matrix as Unity has it, the avatar standing at the origin"""
        av = self.av
        I = Matrix.Identity(4)
        for g in av.gos:
            d = g.tf.data if g.tf else {}
            L0 = I
            fi = g.fbx
            if fi is not None and g.node and g.node in fi.orig:
                pm = fi.info.models[g.node][2]
                while pm and pm not in fi.orig:  # a model that did not come through: it stays where it is
                    pm = fi.info.models[pm][2] if pm in fi.info.models else 0
                L0 = (fi.orig[pm].inverted_safe() if pm else I) @ fi.orig[g.node]
            L = trs_merge(L0, d.get('m_LocalPosition'), d.get('m_LocalRotation'), d.get('m_LocalScale'))
            if g is av.root or g.parent is None or id(g.parent) not in self.U:
                _, q, s = L.decompose()
                self.U[id(g)] = Matrix.LocRotScale(None, q, s)
            else:
                self.U[id(g)] = self.U[id(g.parent)] @ L
        # the plugin stands a humanoid up +Y: one lying down in its files is stood up
        hips, head = human.get('Hips'), human.get('Head')
        if hips is None or head is None or id(hips) not in self.U or id(head) not in self.U:
            return
        up = self.U[id(head)].translation - self.U[id(hips)].translation
        if up.length < 1e-6 or up.normalized().y > 0.7:
            return
        R = up.normalized().rotation_difference(Vector((0.0, 1.0, 0.0))).to_matrix()
        snap = Matrix([[round(R[i][j]) for j in range(3)] for i in range(3)])
        if abs(snap.determinant() - 1) < 1e-6 and all(abs(sum(abs(snap[i][j]) for j in range(3)) - 1) < 1e-6
                                                      for i in range(3)):
            R = snap
        R = R.to_4x4()
        warn('the avatar lies on its side in its files; it is stood up')
        for k in self.U:
            self.U[k] = R @ self.U[k]

    def skinned(self, o):
        return o.type == 'MESH' and any(m.type == 'ARMATURE' and m.object is not None for m in o.modifiers)

    def set_parent(self, o, b):
        """give an object a new parent (an object, a bone or none); its matrix is set again later"""
        p = b[1] if b else None
        q = p
        while q is not None:  # never under itself
            if q == o:
                return
            q = q.parent
        bone = b[2] if b and b[0] == 'bone' else ''
        if o.parent == p and (o.parent_type == 'BONE') == bool(bone) and (not bone or o.parent_bone == bone):
            return
        o.parent = p
        o.parent_type = 'BONE' if bone else 'OBJECT'
        if bone:
            o.parent_bone = bone
        o.matrix_parent_inverse = Matrix.Identity(4)

    def arrange(self):
        """drop what the avatar does not have, and parent the rest as Unity does"""
        av = self.av
        drop, replaced = [], []
        for fi, new in self.imported:
            for o in new:
                own = self.owner.get(o.as_pointer())
                g = fi.gos.get(own[1]) if own else None
                if o.type in ('CAMERA', 'LIGHT', 'LIGHT_PROBE', 'SPEAKER'):
                    drop.append(o)
                elif own is None:
                    if o.type != 'ARMATURE':
                        if o.type == 'MESH':
                            warn('%s: its mesh %s is not in the prefab, so it is left out' % (
                                self.db.get(fi.guid).path, o.name))
                        drop.append(o)
                elif g is None or id(g) not in av.inside:
                    drop.append(o)
                elif av.ma is not None and id(g) in av.ma.replaced:
                    drop.append(o)  # MA's Replace Object put another in its place
                    replaced.append(o)
                elif o.type == 'MESH' and not any(c.cls in RENDERERS for c in g.comps):
                    drop.append(o)  # its renderer is removed
        gone = {o.as_pointer() for o in drop}
        for fi, _ in self.imported:
            for mid in [m for m, b in fi.bl.items() if b[1].as_pointer() in gone]:
                del fi.bl[mid]
        for o in drop:
            mw = {c.as_pointer(): c.matrix_world.copy() for c in o.children}
            for c in list(o.children):
                c.parent = o.parent
                c.parent_type = 'BONE' if o.parent is not None and o.parent_type == 'BONE' else 'OBJECT'
                if c.parent_type == 'BONE':
                    c.parent_bone = o.parent_bone
                c.matrix_parent_inverse = Matrix.Identity(4)
                c.matrix_world = mw[c.as_pointer()]
            self.owner.pop(o.as_pointer(), None)
        self.imported = [(fi, [o for o in new if o.as_pointer() not in gone]) for fi, new in self.imported]
        meshes = [o.data for o in replaced if o.type == 'MESH']
        for o in drop:
            bpy.data.objects.remove(o, do_unlink=True)
        for me in meshes:  # a replaced object's mesh goes too, and its materials can give their names back
            if me.users == 0:
                bpy.data.meshes.remove(me)
        bpy.context.view_layer.update()
        for fi, new in self.imported:
            for mid, b in list(fi.bl.items()):
                g = fi.gos.get(mid)
                if b[0] != 'obj' or g is None or self.skinned(b[1]):
                    continue
                self.set_parent(b[1], self.anchor(g.parent) if g is not av.root else None)
            for o in new:  # an armature Blender made for bones at the top of a file
                if o.as_pointer() in self.owner or o.type != 'ARMATURE':
                    continue
                g = None
                for mid, b in fi.bl.items():
                    if b[0] == 'bone' and b[1] == o and o.data.bones[b[2]].parent is None:
                        g = fi.gos.get(mid)
                        break
                if g is not None:
                    self.set_parent(o, self.anchor(g.parent))
        # a model's mesh skinned to another model's bones does not follow them (no armature merging)
        for r in av.renderers:
            if r.cls != 137 or r.fbx is None:
                continue
            others = {id(b.go.fbx) for b in listof(r.data.get('m_Bones'))
                      if isinstance(b, Obj) and b.go is not None and b.go.fbx is not None and b.go.fbx is not r.fbx}
            if others:
                warn('%s: it is skinned to the bones of another model, which this tool does not merge; it '
                     'moves with its own model\'s bones' % r.gname)

    def place(self):
        """set every object and bone to its Unity matrix, parents first"""
        want_o, want_b = {}, {}
        for fi, _ in self.imported:
            for mid, b in fi.bl.items():
                g = fi.gos.get(mid)
                if g is None or id(g) not in self.U:
                    continue
                M = from_unity(self.U[id(g)], fi.u)
                if b[0] == 'obj':
                    want_o[b[1].as_pointer()] = M
                else:
                    want_b[(b[1].as_pointer(), b[2])] = M
        I = Matrix.Identity(4)

        def visit(o, PW):
            k = o.as_pointer()
            if k in want_o and not self.skinned(o):
                o.matrix_basis = (PW @ o.matrix_parent_inverse).inverted_safe() @ want_o[k]
                W = want_o[k]
            else:
                W = PW @ o.matrix_parent_inverse @ o.matrix_basis
            pose = {}
            if o.type == 'ARMATURE':
                Wi = W.inverted_safe()

                def bone(b, Pp):
                    pb = o.pose.bones[b.name]
                    rel = b.parent.matrix_local.inverted_safe() @ b.matrix_local if b.parent else b.matrix_local
                    t = want_b.get((k, b.name))
                    if t is not None:
                        P = Wi @ t
                        pb.matrix_basis = (Pp @ rel).inverted_safe() @ P
                    else:
                        P = Pp @ rel @ pb.matrix_basis
                    pose[b.name] = P
                    for c in b.children:
                        bone(c, P)
                for b in o.data.bones:
                    if b.parent is None:
                        bone(b, I)
            for c in o.children:
                if c.parent_type == 'BONE' and c.parent_bone in pose:
                    tail = Matrix.Translation((0.0, o.data.bones[c.parent_bone].length, 0.0))
                    visit(c, W @ pose[c.parent_bone] @ tail)
                else:
                    visit(c, W)
        for o in list(bpy.data.objects):
            if o.parent is None:
                visit(o, I)
        bpy.context.view_layer.update()

    # ---- materials, shape keys, names

    def load_image(self, path):
        if path in self._images:
            return self._images[path]
        try:
            im = bpy.data.images.load(path, check_existing=True)
        except RuntimeError as e:
            warn('Blender cannot read %s: %s' % (os.path.basename(path), str(e).strip()))
            im = None
        if im is not None:
            self.shrink(im, self.mats.tex_meta.get(path, (0, True))[0])
        self._images[path] = im
        return im

    def shrink(self, im, cap):
        n = min(cap, self.opts.max_texture) if cap and self.opts.max_texture else (cap or self.opts.max_texture)
        try:
            w, h = im.size
        except (RuntimeError, ValueError):
            return
        if n and max(w, h) > n:
            f = n / float(max(w, h))
            im.scale(max(1, int(round(w * f))), max(1, int(round(h * f))))

    def blender_material(self, m):
        bm = bpy.data.materials.new(bl_name(m.name or 'material'))
        nt = bm.node_tree
        if nt is None:
            bm.use_nodes = True
            nt = bm.node_tree
        bsdf = next((n for n in nt.nodes if n.type == 'BSDF_PRINCIPLED'), None)
        if bsdf is None:
            nt.nodes.clear()
            out = nt.nodes.new('ShaderNodeOutputMaterial')
            bsdf = nt.nodes.new('ShaderNodeBsdfPrincipled')
            nt.links.new(bsdf.outputs[0], out.inputs[0])
        bsdf.inputs['Base Color'].default_value = tuple(m.color[:3]) + (1.0,)
        bsdf.inputs['Metallic'].default_value = 0.0
        bsdf.inputs['Roughness'].default_value = 0.8
        if m.mode != 'OPAQUE':
            bsdf.inputs['Alpha'].default_value = m.color[3]
        baked = self.alpha_image(m) if m.mode != 'OPAQUE' and (m.alpha or m.invert is not None) else None
        if baked is not None:  # a mask texture's channel, or an inverted alpha: in the base texture's alpha
            tn = nt.nodes.new('ShaderNodeTexImage')
            tn.image = baked
            nt.links.new(tn.outputs['Color'], bsdf.inputs['Base Color'])
            nt.links.new(tn.outputs['Alpha'], bsdf.inputs['Alpha'])
        elif m.tex:
            im = self.load_image(m.tex)
            if im is not None:
                tn = nt.nodes.new('ShaderNodeTexImage')
                tn.image = im
                nt.links.new(tn.outputs['Color'], bsdf.inputs['Base Color'])
                if m.mode != 'OPAQUE' and m.alpha is None and self.mats.tex_meta.get(m.tex, (0, True))[1]:
                    nt.links.new(tn.outputs['Alpha'], bsdf.inputs['Alpha'])
        if max(m.emit) > 0:
            bsdf.inputs['Emission Color'].default_value = (1.0, 1.0, 1.0, 1.0)
            bsdf.inputs['Emission Strength'].default_value = 1.0
            if m.emit_tex:
                im = self.load_image(m.emit_tex)
                if im is not None:
                    tn = nt.nodes.new('ShaderNodeTexImage')
                    tn.image = im
                    nt.links.new(tn.outputs['Color'], bsdf.inputs['Emission Color'])
        if m.mode == 'BLEND':
            bm.surface_render_method = 'BLENDED'
        return bm

    def alpha_image(self, m):
        """the base colour texture with UnlitWF's alpha in its own: a mask texture's red or alpha (_AL_Source 1, 2,
        at the texture's size), or the texture's alpha times the colour's, inverted (_AL_InvMaskVal); None if neither
        image can be read"""
        key = ('alpha', m.tex, m.alpha, m.invert)
        if key in self._images:
            return self._images[key]
        import numpy as np
        base = self.load_image(m.tex) if m.tex else None
        mask = self.load_image(m.alpha[0]) if m.alpha else None
        out = None
        if (base or mask) is not None:
            w, h = (base or mask).size

            def pixels(im):
                px = np.empty(im.size[0] * im.size[1] * im.channels, np.float32)
                im.pixels.foreach_get(px)
                px = px.reshape(im.size[1], im.size[0], im.channels)
                if im.channels == 4:
                    return px
                full = np.ones((im.size[1], im.size[0], 4), np.float32)
                full[..., :min(im.channels, 3)] = px[..., :3]
                if im.channels < 3:
                    full[..., 1] = full[..., 2] = px[..., 0]
                return full
            rgba = pixels(base) if base is not None else np.ones((h, w, 4), np.float32)
            if base is not None and not self.mats.tex_meta.get(m.tex, (0, True))[1]:
                rgba[..., 3] = 1.0  # its alpha is not used
            if mask is not None:
                mk = mask
                if tuple(mask.size) != (w, h):
                    mk = mask.copy()
                    mk.scale(w, h)
                a = pixels(mk)[..., 0 if m.alpha[1] == 'R' else 3]
                if mk is not mask:
                    bpy.data.images.remove(mk)
            else:
                a = rgba[..., 3]
            if m.invert is not None:
                a = 1.0 - m.invert * a
            rgba[..., 3] = np.clip(a, 0.0, 1.0)
            stem = os.path.splitext((base or mask).name)[0]
            out = bpy.data.images.new(bl_name(stem + ' alpha'), w, h, alpha=True)
            if base is not None:
                out.colorspace_settings.name = base.colorspace_settings.name
            out.pixels.foreach_set(rgba.ravel())
        self._images[key] = out
        return out

    def blender_for(self, m):
        """the Blender material of a MatInfo, made once"""
        bm = self._made.get(m.key())
        if bm is None:
            bm = self._made[m.key()] = self.blender_material(m)
            self.post[bm.name] = m
        return bm

    def materials(self, an=None, base=None):
        """the renderers' materials, as the avatar rests (base: what its animators and components have it show)"""
        for r in self.av.renderers:
            b = self.counterpart(r.go) if r.go else None
            if b is None or b[0] != 'obj' or b[1].type != 'MESH':
                continue
            o = b[1]
            shared = o.data.users > 1
            for k, (i, m) in enumerate(self.mats.slots(r)):
                if i < len(o.material_slots) and o.material_slots[i].material is not None:
                    self._own.setdefault((id(r), k), o.material_slots[i].material)
                if an is not None and base:
                    s = an.material_state(base, r, k)
                    if s != an.material_state({}, r, k):  # another material at rest (MA's, VRCFury's, FX's)
                        m = self.mats.state_material(s, r, k) or m
                if m is None or i >= len(o.material_slots):
                    continue
                bm = self.blender_for(m)
                if shared:
                    o.material_slots[i].link = 'OBJECT'
                o.material_slots[i].material = bm
        # the models' own materials: their textures, wherever the package put them
        for fi, new in self.imported:
            a = self.db.get(fi.guid)
            for o in new:
                if o.type != 'MESH':
                    continue
                for s in o.material_slots:
                    if s.material is None or s.material.name in self.post or s.material.node_tree is None:
                        continue
                    for n in s.material.node_tree.nodes:
                        if n.type == 'TEX_IMAGE' and n.image is not None:
                            self.relink(n.image, a.path)
        # the models' materials nothing uses any more give their names back ("Skin", not "Skin.001")
        for mt in list(bpy.data.materials):
            if mt.users == 0 and mt.name not in self.post:
                bpy.data.materials.remove(mt)
        for old in list(self.post):
            mt = bpy.data.materials.get(old)
            want = bl_name(self.post[old].name or 'material')
            if mt is not None and old != want and bpy.data.materials.get(want) is None:
                mt.name = want
                self.post[mt.name] = self.post.pop(old)

    def variants(self, todo):
        """glTF material variants (KHR_materials_variants, which Blender's exporter writes): one per [(name,
        {(renderer, slot): material state})], each putting other materials in some renderers' slots"""
        todo = [(n, m) for n, m in todo if m]
        if not todo:
            return
        prefs = bpy.context.preferences.addons['io_scene_gltf2'].preferences
        if not prefs.KHR_materials_variants_ui:
            prefs.KHR_materials_variants_ui = True  # registers the variant properties the exporter reads
        scene = bpy.data.scenes[0]
        entries = {}  # (mesh, Blender slot, material) -> its variant mesh data
        copied = set()
        for name, mats in todo:
            v = scene.gltf2_KHR_materials_variants_variants.add()
            v.variant_idx = len(scene.gltf2_KHR_materials_variants_variants) - 1
            v.name = name
            self.variant_names[name] = v.name
            for (r, k), st in sorted(mats.items(), key=lambda x: (str(x[0][0].gname), x[0][1])):
                b = self.counterpart(r.go) if r.go else None
                slots = self.mats.slots(r)
                if b is None or b[0] != 'obj' or b[1].type != 'MESH' or k >= len(slots):
                    continue
                o, i = b[1], slots[k][0]
                if i >= len(o.material_slots):
                    continue
                m = self.mats.state_material(st, r, k)
                bm = self.blender_for(m) if m is not None else self._own.get((id(r), k))
                if bm is None:
                    continue
                if o.data.users > 1 and o.as_pointer() not in copied:  # its own mesh, for its own variants
                    o.data = o.data.copy()
                    copied.add(o.as_pointer())
                key = (o.data.as_pointer(), i, bm.as_pointer())
                e = entries.get(key)
                if e is None:
                    e = entries[key] = o.data.gltf2_variant_mesh_data.add()
                    e.material_slot_index = i
                    e.material = bm
                vv = e.variants.add()
                vv.variant.variant_idx = v.variant_idx
        log('%d material variant%s: %s' % (len(todo), '' if len(todo) == 1 else 's', ', '.join(n for n, _ in todo)))

    def relink(self, im, near):
        if im.as_pointer() in self._relinked:
            return
        self._relinked.add(im.as_pointer())
        if im.source == 'FILE' and im.packed_file is None:
            path = bpy.path.abspath(im.filepath)
            if not os.path.isfile(path):
                base = os.path.basename(path.replace('\\', '/'))
                stem, ext = os.path.splitext(base)
                hits = [h for h in self.db.by_name(stem, IMAGE_EXT, near=near) if h.ext in BLENDER_IMAGES]
                hits.sort(key=lambda h: h.ext != ext.lower())
                if not hits:
                    warn('the texture %s is not in the input' % base)
                    return
                im.filepath = hits[0].file
                im.reload()
                self.mats.image({'fileID': '2800000', 'guid': hits[0].guid})
                self.shrink(im, self.mats.tex_meta.get(hits[0].file, (0, True))[0])
                return
        self.shrink(im, 0)

    def shapes(self, base):
        """shape keys at the values the avatar rests with"""
        for r in self.av.renderers:
            if r.cls != 137 or r.go is None:
                continue
            b = self.counterpart(r.go)
            if b is None or b[0] != 'obj' or b[1].type != 'MESH' or b[1].data.shape_keys is None:
                continue
            kb = b[1].data.shape_keys.key_blocks
            names = self.av.shape_names(r)
            by_index = len(kb) - 1 == len(names)
            mine = self.keys[id(r)] = {}
            for i, n in enumerate(names):
                k = kb[i + 1] if by_index else kb.get(bl_name(n))
                if k is None:
                    continue
                mine.setdefault(n, k.name)
                w = base.get(('s', r, n))
                if w is None:
                    w = self.av.default(('s', r, n))
                k.slider_max = max(k.slider_max, 1.0)
                k.value = min(max(w / 100.0, 0.0), 1.0)

    def names(self, human):
        """unique node names, by the plugin's loose compare too; humanoid bones and meshes keep theirs"""
        hum = {id(g) for g in human.values()}
        items = []  # [priority, order, what, wanted name, GameObject]
        mapped = set()
        for fi, _ in self.imported:
            for mid, b in fi.bl.items():
                g = fi.gos.get(mid)
                if g is None or id(g) not in self.av.inside:
                    continue
                nm = str(g.data.get('m_Name') or '') or fi.info.name(mid)
                pri = 0 if id(g) in hum else 1 if any(c.cls in RENDERERS for c in g.comps) else 2 if b[0] == 'bone' else 3
                items.append([pri, len(items), b, nm, g])
                mapped.add((b[0], b[1].as_pointer(), b[2] if b[0] == 'bone' else ''))
        for o in bpy.data.objects:
            if ('obj', o.as_pointer(), '') not in mapped:
                items.append([4, len(items), ('obj', o), 'Armature' if o.name.startswith('~u2h') else o.name, None])
            if o.type == 'ARMATURE':
                for bn in o.data.bones.keys():
                    if ('bone', o.as_pointer(), bn) not in mapped:
                        items.append([4, len(items), ('bone', o, bn), bn, None])
        items.sort(key=lambda x: (x[0], x[1]))
        # out of the way first, so that no name is taken by one not renamed yet
        for it in items:
            t = self._tmpname()
            b = it[2]
            if b[0] == 'obj':
                b[1].name = t
            else:
                b[1].data.bones[b[2]].name = t
                it[2] = ('bone', b[1], t)
        used = set()
        for pri, k, b, nm, g in items:
            nm = bl_name(re.sub(r'\s+', ' ', nm).strip() or 'node')
            stem, j = nm, 2
            while norm_name(nm) in used or nm.lower() in used or not norm_name(nm):
                sfx = ' %d' % j
                nm = bl_name(stem.encode('utf-8')[:63 - len(sfx)].decode('utf-8', 'ignore') + sfx)
                j += 1
            used.add(norm_name(nm))
            used.add(nm.lower())
            if b[0] == 'obj':
                b[1].name = nm
                got = b[1].name
            else:
                bone = b[1].data.bones[b[2]]
                bone.name = nm
                got = bone.name
            if got != nm:
                warn('Blender named %s %s' % (nm, got))
            if g is not None:
                g.name = got
                fi = g.fbx
                if b[0] == 'bone' and fi is not None and fi.bl.get(g.node, (None,))[0] == 'bone':
                    fi.bl[g.node] = ('bone', b[1], got)

    # ---- out

    def export(self, path):
        log('writing %s' % path)
        bpy.ops.export_scene.gltf(
            filepath=path, export_format='GLB', export_animations=False, export_skins=True,
            export_morph=True, export_morph_normal=True, export_rest_position_armature=False,
            export_def_bones=False, export_yup=True, export_apply=False, export_lights=False,
            export_cameras=False, export_extras=False, export_image_format='AUTO', use_selection=False,
            use_visible=False, use_renderable=False, use_active_collection=False, export_leaf_bone=False,
            export_hierarchy_flatten_bones=False, export_hierarchy_flatten_objs=False,
            export_armature_object_remove=False, export_try_sparse_sk=True, export_materials='EXPORT')


# ---------------------------------------------------------------- the GLB, after Blender

GLB_MAGIC, GLB_JSON, GLB_BIN = 0x46546C67, 0x4E4F534A, 0x004E4942


def read_glb(path):
    with open(path, 'rb') as f:
        data = f.read()
    if len(data) < 20 or struct.unpack_from('<I', data, 0)[0] != GLB_MAGIC:
        raise Fail('%s is not a GLB file' % path)
    js, binc, pos = None, b'', 12
    while pos + 8 <= len(data):
        n, kind = struct.unpack_from('<II', data, pos)
        chunk = data[pos + 8:pos + 8 + n]
        if kind == GLB_JSON and js is None:
            js = json.loads(chunk.decode('utf-8'))
        elif kind == GLB_BIN and not binc:
            binc = chunk
        pos += 8 + n
    if js is None:
        raise Fail('%s has no glTF JSON' % path)
    return js, binc


def write_glb(path, js, binc):
    j = json.dumps(js, separators=(',', ':'), ensure_ascii=False).encode('utf-8')
    j += b' ' * (-len(j) % 4)
    b = binc + b'\0' * (-len(binc) % 4)
    body = struct.pack('<II', len(j), GLB_JSON) + j
    if b:
        body += struct.pack('<II', len(b), GLB_BIN) + b
    tmp = path + '.part'
    with open(tmp, 'wb') as f:
        f.write(struct.pack('<III', GLB_MAGIC, 2, 12 + len(body)))
        f.write(body)
    os.replace(tmp, path)


def gltf_tree(js):
    """(parent of each node, its world matrix)"""
    nodes = js.get('nodes', [])
    parent = [-1] * len(nodes)
    for i, n in enumerate(nodes):
        for c in n.get('children', []):
            if 0 <= c < len(nodes):
                parent[c] = i
    world = [None] * len(nodes)
    stack = [i for i in range(len(nodes)) if parent[i] < 0]
    while stack:
        i = stack.pop()
        n = nodes[i]
        if 'matrix' in n:
            m = n['matrix']
            L = Matrix([[m[c * 4 + r] for c in range(4)] for r in range(4)])
        else:
            t, q, s = n.get('translation', (0, 0, 0)), n.get('rotation', (0, 0, 0, 1)), n.get('scale', (1, 1, 1))
            L = Matrix.LocRotScale(Vector(t), Quaternion((q[3], q[0], q[1], q[2])), Vector(s))
        world[i] = L if parent[i] < 0 else world[parent[i]] @ L
        stack += [c for c in n.get('children', []) if 0 <= c < len(nodes)]
    return parent, [w if w is not None else Matrix.Identity(4) for w in world]


def avg_scale(M):
    return (M.col[0].xyz.length + M.col[1].xyz.length + M.col[2].xyz.length) / 3.0


def max_scale(M):
    return max(M.col[0].xyz.length, M.col[1].xyz.length, M.col[2].xyz.length)


def patch_materials(js, post):
    """what the Unity materials say that Blender's exporter leaves out or gets wrong: colour, cutout or
    blending, emission, tiling, culling"""
    used = set(js.get('extensionsUsed', []))

    def xform(info, xf):
        (sx, sy), (ox, oy) = xf
        ext = info.setdefault('extensions', {})
        if abs(sx - 1) < 1e-6 and abs(sy - 1) < 1e-6 and abs(ox) < 1e-6 and abs(oy) < 1e-6:
            ext.pop('KHR_texture_transform', None)
        else:
            ext['KHR_texture_transform'] = {'offset': [ox, 1.0 - sy - oy], 'scale': [sx, sy]}
            used.add('KHR_texture_transform')
        if not ext:
            info.pop('extensions')
    for m in js.get('materials', []):
        mi = post.get(m.get('name'))
        if mi is None:
            continue
        pbr = m.setdefault('pbrMetallicRoughness', {})
        c = [round(x, 5) for x in mi.color]
        if mi.mode == 'OPAQUE':
            c[3] = 1.0
        if any(abs(x - 1) > 1e-5 for x in c):
            pbr['baseColorFactor'] = c
        else:
            pbr.pop('baseColorFactor', None)
        pbr['metallicFactor'] = 0.0
        if 'baseColorTexture' in pbr:
            xform(pbr['baseColorTexture'], mi.tex_xf)
        m.pop('alphaCutoff', None)
        if mi.mode == 'OPAQUE':
            m.pop('alphaMode', None)
        else:
            m['alphaMode'] = mi.mode
            if mi.mode == 'MASK':
                m['alphaCutoff'] = round(mi.cutoff, 4)
        if mi.double:
            m['doubleSided'] = True
        else:
            m.pop('doubleSided', None)
        ext = m.setdefault('extensions', {})
        ext.pop('KHR_materials_emissive_strength', None)
        e = max(mi.emit)
        if e > 0:
            m['emissiveFactor'] = [round(x / max(e, 1.0), 5) for x in mi.emit]
            if e > 1:
                ext['KHR_materials_emissive_strength'] = {'emissiveStrength': round(e, 4)}
                used.add('KHR_materials_emissive_strength')
            if 'emissiveTexture' in m:
                xform(m['emissiveTexture'], mi.emit_xf)
        else:
            m.pop('emissiveFactor', None)
            m.pop('emissiveTexture', None)
        if not ext:
            m.pop('extensions')
    left = set()
    for m in js.get('materials', []):
        left |= set(m.get('extensions', {}))
        for k in ('baseColorTexture',):
            left |= set(m.get('pbrMetallicRoughness', {}).get(k, {}).get('extensions', {}))
        for k in ('emissiveTexture', 'normalTexture', 'occlusionTexture'):
            left |= set(m.get(k, {}).get('extensions', {}))
    for x in ('KHR_texture_transform', 'KHR_materials_emissive_strength'):
        if x not in left:
            used.discard(x)
    if used:
        js['extensionsUsed'] = sorted(used)
    else:
        js.pop('extensionsUsed', None)


def png_bytes(im):
    """a Blender image as a PNG file's bytes (8 bits a channel, as its pixels are stored)"""
    import numpy as np
    import zlib
    w, h = im.size
    c = im.channels
    px = np.empty(w * h * c, np.float32)
    im.pixels.foreach_get(px)
    a = (np.clip(px.reshape(h, w, c), 0.0, 1.0) * 255.0 + 0.5).astype(np.uint8)[::-1]
    if c == 2:
        a = a[..., :1]
    kind = {1: 0, 2: 0, 3: 2, 4: 6}[c]
    raw = np.concatenate([np.zeros((h, 1), np.uint8), a.reshape(h, -1)], axis=1).tobytes()

    def chunk(tag, data):
        return struct.pack('>I', len(data)) + tag + data + struct.pack('>I', zlib.crc32(tag + data) & 0xffffffff)
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, kind, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))


def material_extras(js, binc, b):
    """what hypr3d reads from the materials' extras that glTF has no place for (MatInfo's queue, stencil, outline,
    back faces, light clamp, toon shading, matcap): "hypr3d_queue", "hypr3d_stencil", "hypr3d_outline", "hypr3d_back",
    "hypr3d_light", "hypr3d_toon", "hypr3d_matcap". The textures only they use (outline masks, back textures, shade
    textures, matcaps) are added to the GLB as PNGs"""
    out = bytearray(binc)
    made = {}  # image file -> texture index
    stems = {}  # an image name the exporter gave one image only -> that image
    for i, im in enumerate(js.get('images', [])):
        stems[im.get('name')] = i if im.get('name') not in stems else None

    def texture(f, cap=0):
        if f in made:
            return made[f]
        t = None
        src = stems.get(os.path.splitext(os.path.basename(f))[0])  # the exporter's already: named after the file
        if src is not None:
            t = next((k for k, x in enumerate(js.get('textures', [])) if x.get('source') == src), None)
        if t is None:
            im = b.load_image(f)
            if im is None:
                made[f] = None
                return None
            if src is None:
                small = im
                if cap and max(im.size) > cap:  # (a mask sampled at the vertices needs no more)
                    k = cap / float(max(im.size))
                    small = im.copy()
                    small.scale(max(1, int(round(im.size[0] * k))), max(1, int(round(im.size[1] * k))))
                data = png_bytes(small)
                if small is not im:
                    bpy.data.images.remove(small)
                while len(out) % 4:
                    out.append(0)
                js.setdefault('bufferViews', []).append({'buffer': 0, 'byteOffset': len(out), 'byteLength': len(data)})
                out.extend(data)
                js.setdefault('images', []).append({'name': os.path.splitext(im.name)[0], 'mimeType': 'image/png',
                                                    'bufferView': len(js['bufferViews']) - 1})
                src = len(js['images']) - 1
            samplers = js.setdefault('samplers', [])
            want = {'magFilter': 9729, 'minFilter': 9987, 'wrapS': 10497, 'wrapT': 10497}
            s = next((k for k, x in enumerate(samplers) if x == want), None)
            if s is None:
                samplers.append(want)
                s = len(samplers) - 1
            js.setdefault('textures', []).append({'sampler': s, 'source': src})
            t = len(js['textures']) - 1
        made[f] = t
        return t

    def xform(xf):
        (sx, sy), (ox, oy) = xf
        if abs(sx - 1) < 1e-6 and abs(sy - 1) < 1e-6 and abs(ox) < 1e-6 and abs(oy) < 1e-6:
            return None
        return {'offset': [ox, 1.0 - sy - oy], 'scale': [sx, sy]}
    said = []
    for m in js.get('materials', []):
        mi = b.post.get(m.get('name'))
        if mi is None:
            continue
        ex = {}
        if mi.queue >= 0 and mi.queue != MODE_QUEUE[mi.mode]:
            ex['hypr3d_queue'] = mi.queue
        if mi.stencil:
            ex['hypr3d_stencil'] = mi.stencil
        if mi.outline:
            o = {k: v for k, v in mi.outline.items() if v is not None and k not in ('mask', 'tex')}
            mk = mi.outline.get('mask')
            if mk:
                t = texture(mk[0], 512)
                if t is not None:
                    o['mask'] = {'index': t, 'channel': 'RGBA'.index(mk[1])}
                    if mk[2]:
                        o['mask']['invert'] = True
            ct = mi.outline.get('tex')
            if ct:
                f, xf, blend = ct
                # (the main texture's own, when it's that: its RGB; its alpha may have been baked)
                t = m.get('pbrMetallicRoughness', {}).get('baseColorTexture', {}).get('index') if f == mi.tex else texture(f)
                if t is not None:
                    o['texture'] = {'index': t}
                    x = xform(xf)
                    if x:
                        o['texture']['transform'] = x
                    if blend is not None:
                        o['texture']['blend'] = blend
            ex['hypr3d_outline'] = o
        if mi.back:
            bk = {'color': list(mi.back['color'])}
            f = mi.back['tex']
            t = None
            if f and f == mi.tex:  # the main texture's (its RGB; its alpha may have been baked)
                t = m.get('pbrMetallicRoughness', {}).get('baseColorTexture', {}).get('index')
            elif f:
                t = texture(f)
            if t is not None:
                bk['texture'] = {'index': t}
                x = xform(mi.back['xf'])
                if x:
                    bk['texture']['transform'] = x
            ex['hypr3d_back'] = bk
        if mi.light:
            ex['hypr3d_light'] = {'min': mi.light[0], 'max': mi.light[1], 'chroma': mi.light[2]}
        if mi.toon:
            tn = {k: v for k, v in mi.toon.items() if k != 'tex'}
            f = mi.toon.get('tex')
            t = m.get('pbrMetallicRoughness', {}).get('baseColorTexture', {}).get('index') if f and f == mi.tex else \
                texture(f) if f else None
            if t is not None:
                tn['texture'] = {'index': t}
            ex['hypr3d_toon'] = tn
        if mi.matcap:
            t = texture(mi.matcap['tex'], 512)
            if t is not None:
                ex['hypr3d_matcap'] = dict({k: v for k, v in mi.matcap.items() if k != 'tex'}, index=t)
        if ex:
            m.setdefault('extras', {}).update(ex)
            said.append('%s (%s)' % (m.get('name'), ', '.join(k[7:] for k in ex)))
    if js.get('buffers'):
        js['buffers'][0]['byteLength'] = len(out)
    elif out:
        js['buffers'] = [{'byteLength': len(out)}]
    if said:
        log('materials: %s' % '; '.join(said))
    return bytes(out)


def gltf_array(js, binc, i, raw_type=False):
    """accessor i as a numpy array of floats, (count, components); sparse accessors too, normalized integers scaled
    (raw_type: in its own component type, as stored)"""
    import numpy as np
    a = js['accessors'][i]
    n = a.get('count', 0)
    k = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4, 'MAT2': 4, 'MAT3': 9, 'MAT4': 16}[a['type']]
    dt = np.dtype({5120: 'i1', 5121: 'u1', 5122: '<i2', 5123: '<u2', 5125: '<u4', 5126: '<f4'}[a['componentType']])

    def rows(view, off, count, width):
        bv = js['bufferViews'][view]
        start = bv.get('byteOffset', 0) + off
        stride = bv.get('byteStride') or dt.itemsize * width
        if count == 0:
            return np.zeros((0, width))
        raw = np.frombuffer(binc, np.uint8, count=stride * (count - 1) + dt.itemsize * width, offset=start)
        raw = np.lib.stride_tricks.as_strided(raw, shape=(count, dt.itemsize * width), strides=(stride, 1))
        got = raw.copy().view(dt).reshape(count, width)
        return got if raw_type else got.astype(np.float64)
    out = rows(a['bufferView'], a.get('byteOffset', 0), n, k) if 'bufferView' in a else np.zeros(
        (n, k), dt if raw_type else np.float64)
    sp = a.get('sparse')
    if sp:
        it = sp['indices']
        idt = np.dtype({5121: 'u1', 5123: '<u2', 5125: '<u4'}[it['componentType']])
        bv = js['bufferViews'][it['bufferView']]
        at = np.frombuffer(binc, idt, count=sp['count'], offset=bv.get('byteOffset', 0) + it.get('byteOffset', 0))
        out[at.astype(np.int64)] = rows(sp['values']['bufferView'], sp['values'].get('byteOffset', 0), sp['count'], k)
    if a.get('normalized') and dt.kind in 'iu' and not raw_type:
        m = float(np.iinfo(dt).max)
        out = np.maximum(out / m, -1.0)
    return out


def texture_wrap(db, guid):
    """a texture's wrap modes (u, v) as its importer has them: 0 repeat, 1 clamp, 2 mirror, 3 mirror once"""
    ts = dictof(dictof(db.importer(guid)).get('textureSettings'))
    w = inum(ts.get('m_WrapMode', ts.get('wrapMode')), 0)
    u_, v_ = inum(ts.get('m_WrapU', ts.get('wrapU')), w), inum(ts.get('m_WrapV', ts.get('wrapV')), w)
    return tuple(x if x in (0, 1, 2, 3) else 0 for x in (u_, v_))


def cut_meshes(js, binc, b, an):
    """MA's Mesh Cutters and Shape Changers' deletes on the exported GLB, as ReactiveObjectPass has them: the triangles
    their vertex filters pick go while they are in effect. A cut in effect in every state the avatar can be in
    (as its toggles and sliders give them) takes them out of the mesh; one that a toggle or slider switches puts them in
    a primitive of their own, a part ("hypr3d_part" in its extras) the settings file hides while the cut is in effect,
    as MA's NaNimation hides them in VRChat. Returns (the binary chunk, [(renderer, {its cuts}, part name)])"""
    cutters = an.mat.cutters if an.mat is not None else {}
    states = [an.cuts0] + [t['cuts'] for t in an.kept] + [k['cuts'] for s in an.sliders for k in s['keys']]
    ever = frozenset().union(*states)
    every = frozenset(ever).intersection(*states)
    by_r = {}
    for (rid, key), cut in cutters.items():
        pr = ('c', cut['r'], key)
        if pr in ever:
            by_r.setdefault(rid, []).append((pr, cut))
    if not by_r:
        return binc, []
    import numpy as np
    nodes = js.get('nodes', [])
    index = {}
    for i, n in enumerate(nodes):
        if n.get('name') is not None:
            index.setdefault(n['name'], i)
    go_of = {index[g.name]: g for g in b.av.gos if g.name in index}
    _, W = gltf_tree(js)
    Wn = np.array([[list(row) for row in M] for M in W]) if W else np.zeros((0, 4, 4))
    F = np.diag([-1.0, 1.0, 1.0])
    taken = {norm_name(n['name']) for n in nodes if n.get('name')}
    out = bytearray(binc)
    U0 = b.av.ma.U0 if b.av.ma is not None else b.U
    pieces, said = [], []

    def put(data, target, acc):
        while len(out) % 4:
            out.append(0)
        js['bufferViews'].append({'buffer': 0, 'byteOffset': len(out), 'byteLength': len(data), 'target': target})
        out.extend(data)
        js['accessors'].append(dict(acc, bufferView=len(js['bufferViews']) - 1))
        return len(js['accessors']) - 1

    def subset(prim, tris):
        """the primitive with only these triangles, and only the vertices they use"""
        used, inv = np.unique(tris.reshape(-1), return_inverse=True)
        x = copy.deepcopy(prim)

        def rows(a):
            acc = js['accessors'][a]
            v = gltf_array(js, binc, a, raw_type=True)[used]
            y = {k: acc[k] for k in ('componentType', 'normalized', 'type') if k in acc}
            y['count'] = len(used)
            if acc['type'] == 'VEC3' and acc['componentType'] == 5126 and 'min' in acc:
                y['min'] = [float(c) for c in v.min(0)] if len(v) else [0.0, 0.0, 0.0]
                y['max'] = [float(c) for c in v.max(0)] if len(v) else [0.0, 0.0, 0.0]
            return put(np.ascontiguousarray(v).tobytes(), 34962, y)
        x['attributes'] = {k: rows(a) for k, a in prim['attributes'].items()}
        if prim.get('targets'):
            x['targets'] = [{k: rows(a) for k, a in tg.items()} for tg in prim['targets']]
        flat = inv.reshape(-1)
        big = len(used) > 65535
        x['indices'] = put(flat.astype('<u4' if big else '<u2').tobytes(), 34963, {
            'componentType': 5125 if big else 5123, 'count': int(flat.size), 'type': 'SCALAR'})
        return x
    for rid, cuts in by_r.items():
        r = cuts[0][1]['r']
        ni = index.get(r.go.name) if r.go is not None and r.go.name else None
        if ni is None or 'mesh' not in nodes[ni] or id(r.go) not in U0:
            warn('%s: the mesh a Mesh Cutter cuts is not in the GLB' % r.gname)
            continue
        if sum(1 for n in nodes if n.get('mesh') == nodes[ni]['mesh']) > 1:  # shared: this node gets its own copy
            js['meshes'].append(copy.deepcopy(js['meshes'][nodes[ni]['mesh']]))
            nodes[ni]['mesh'] = len(js['meshes']) - 1
        mesh = js['meshes'][nodes[ni]['mesh']]
        names = dictof(mesh.get('extras')).get('targetNames', [])
        weights = nodes[ni].get('weights', mesh.get('weights', []))
        bl = b.counterpart(r.go)
        slots = [s.material.name if s.material else None for s in bl[1].material_slots] if bl and bl[0] == 'obj' else []
        skin = js['skins'][nodes[ni]['skin']] if 'skin' in nodes[ni] else None
        S = None
        if skin is not None:
            nj = len(skin['joints'])
            ibm = gltf_array(js, binc, skin['inverseBindMatrices']).reshape(nj, 4, 4).transpose(0, 2, 1) if \
                'inverseBindMatrices' in skin else np.tile(np.eye(4), (nj, 1, 1))
            S = np.einsum('jab,jbc->jac', Wn[skin['joints']], ibm)
        Ui = np.array([list(row) for row in U0[id(r.go)].inverted_safe()])
        react = sorted({pr for pr, _ in cuts if pr not in every}, key=lambda p: repr(p[2]))
        bit = {pr: 1 << i for i, pr in enumerate(react)}
        prims, removed, moved, codes_used = [], 0, 0, {}
        for prim in mesh['primitives']:
            if prim.get('mode', 4) != 4 or 'indices' not in prim:
                prims.append(prim)
                continue
            tris = gltf_array(js, binc, prim['indices'])[:, 0].astype(np.int64).reshape(-1, 3)
            at = prim['attributes']
            P = gltf_array(js, binc, at['POSITION'])
            targets = prim.get('targets', [])
            for t, w in enumerate(weights):
                if w and t < len(targets) and 'POSITION' in targets[t]:
                    P = P + w * gltf_array(js, binc, targets[t]['POSITION'])
            J = Wt = None
            if S is not None and 'JOINTS_0' in at and 'WEIGHTS_0' in at:
                J = np.concatenate([gltf_array(js, binc, at[k]) for k in ('JOINTS_0', 'JOINTS_1') if k in at], 1)
                Wt = np.concatenate([gltf_array(js, binc, at[k]) for k in ('WEIGHTS_0', 'WEIGHTS_1') if k in at], 1)
                J = J.astype(np.int64)
                tot = Wt.sum(1, keepdims=True)
                M = np.einsum('nk,nkab->nab', Wt / np.where(tot > 0, tot, 1.0), S[J])
                M[tot[:, 0] <= 0] = Wn[ni]
            else:
                M = np.tile(Wn[ni], (len(P), 1, 1))
            L = M[:, :3, :3]
            Pw = np.einsum('nab,nb->na', L, P) + M[:, :3, 3]
            Pu = (Ui[:3, :3] @ (F @ Pw.T)).T + Ui[:3, 3]  # in the renderer's space, as Unity bakes the mesh

            def uvs(ch):
                k = 'TEXCOORD_%d' % ch
                if k not in at:
                    return None
                uv = gltf_array(js, binc, at[k])
                return np.stack([uv[:, 0], 1.0 - uv[:, 1]], 1)  # Unity's v goes up

            def pick(sel_v, sel_c, mode):
                """triangles picked by a vertex test (any or all corners) or a centroid test"""
                if mode == 2 and sel_c is not None:
                    return sel_c
                return sel_v[tris].all(1) if mode == 1 else sel_v[tris].any(1)
            prim_slots = [k for k, nm in enumerate(slots) if nm is not None and nm == js['materials'][
                prim['material']].get('name')] if 'material' in prim else []

            def one(f):
                kind, mode = f['kind'], f['mode']
                if kind == 'axis':
                    ax, c0 = np.array(f['axis']), np.array(f['center'])
                    return pick((Pu - c0) @ ax > 0, (Pu[tris].mean(1) - c0) @ ax > 0, mode)
                if kind == 'bone':
                    if J is None or f['bone'] is None:
                        return np.zeros(len(tris), bool)
                    mine = np.array([go_of.get(n) is f['bone'] for n in skin['joints']])
                    part = (Wt * mine[J]).sum(1)
                    tot = Wt.sum(1)
                    return pick((part > 0) & (part >= f['threshold'] * np.where(tot > 0, tot, 1.0)), None, mode)
                if kind == 'shape':
                    sel = np.zeros(len(P), bool)
                    for sh in f['shapes']:
                        kn = b.keys.get(id(r), {}).get(sh)
                        t = names.index(kn) if kn in names else -1
                        if 0 <= t < len(targets) and 'POSITION' in targets[t]:
                            D = np.einsum('nab,nb->na', L, gltf_array(js, binc, targets[t]['POSITION']))
                            Du = (Ui[:3, :3] @ (F @ D.T)).T
                            sel |= (Du * Du).sum(1) > f['threshold'] ** 2
                    return pick(sel, None, mode)
                uv = uvs(f['uv'])
                if uv is None:
                    return np.zeros(len(tris), bool)
                if kind == 'uvtile':
                    def inside(x):
                        ok = np.ones(len(x), bool)
                        for (use, incl, v), col, lo in ((f['umin'], 0, 1), (f['umax'], 0, 0), (f['vmin'], 1, 1),
                                                        (f['vmax'], 1, 0)):
                            if use:
                                ok &= (x[:, col] >= v if incl else x[:, col] > v) if lo else (
                                    x[:, col] <= v if incl else x[:, col] < v)
                        return ok != f['invert']
                    return pick(inside(uv), inside(uv[tris].mean(1)), mode)
                if kind == 'mask':
                    if not slots or min(max(f['slot'], 0), len(slots) - 1) not in prim_slots:
                        return np.zeros(len(tris), bool)
                    img = mask_image(b, f['texture'])
                    if img is None:
                        return np.zeros(len(tris), bool)
                    pix, (wu, wv) = img

                    def black_white(x):
                        h, w = pix.shape[:2]

                        def wrap(c, size, mode):
                            i = np.floor(c * size).astype(np.int64)
                            if mode == 0:
                                return np.mod(i, size)
                            if mode == 2:
                                i = np.mod(i, 2 * size)
                                return np.where(i >= size, 2 * size - 1 - i, i)
                            if mode == 3:
                                i = np.where(i < 0, -1 - i, i)
                            return np.clip(i, 0, size - 1)
                        px = pix[wrap(x[:, 1], h, wv), wrap(x[:, 0], w, wu)]
                        return (px == (255 if f['white'] else 0)).all(1)
                    return pick(black_white(uv), black_white(uv[tris].mean(1)), mode)
                return np.zeros(len(tris), bool)
            gone = np.zeros(len(tris), bool)
            code = np.zeros(len(tris), np.int64)
            for pr, cut in cuts:
                ms = [one(f) for f in cut['filters']]
                m = np.logical_and.reduce(ms) if cut['multi'] == 1 else np.logical_or.reduce(ms)
                if pr in every:
                    gone |= m
                else:
                    code |= np.where(m, bit[pr], 0)
            removed += int(gone.sum())
            keep = ~gone
            main = keep & (code == 0)
            if main.all():
                prims.append(prim)
            elif main.any():
                prims.append(subset(prim, tris[main]))
            for cd in sorted(set(code[keep & (code != 0)].tolist())):
                sel = keep & (code == cd)
                moved += int(sel.sum())
                if cd not in codes_used:
                    ks = [pr for pr in react if bit[pr] & cd]
                    labels = [lb for pr in ks for lb in cutters[(rid, pr[2])]['labels']]
                    nm = stem = bl_name('%s (%s)' % (nodes[ni]['name'], ', '.join(dict.fromkeys(labels))))
                    k = 2
                    while norm_name(nm) in taken:
                        nm = '%s %d' % (stem, k)
                        k += 1
                    taken.add(norm_name(nm))
                    codes_used[cd] = nm
                    pieces.append((r, frozenset(ks), nm))
                x = subset(prim, tris[sel])
                x.setdefault('extras', {})['hypr3d_part'] = codes_used[cd]
                prims.append(x)
        if not prims:  # all of it cut away: one empty triangle keeps the mesh a mesh
            prims.append(subset(mesh['primitives'][0], np.zeros((1, 3), np.int64)))
        mesh['primitives'] = prims
        said.append('%s: %d triangles cut away%s' % (nodes[ni]['name'], removed, ', %d in %d part(s) toggles hide' % (
            moved, len(codes_used)) if moved else ''))
    if js.get('buffers'):
        js['buffers'][0]['byteLength'] = len(out)
    if said:
        log('Modular Avatar: Mesh Cutter: ' + '; '.join(said))
    return bytes(out), pieces


def mask_image(b, guid):
    """a mask texture's pixels as 8-bit RGB (rows from the bottom, as Unity reads them) and its wrap modes; None if
    it cannot be read"""
    if not hasattr(b, '_masks'):
        b._masks = {}
    if guid in b._masks:
        return b._masks[guid]
    import numpy as np
    a = b.db.get(guid) if guid else None
    got = None
    if a is not None:
        im = b.load_image(a.file)
        if im is not None and im.size[0] and im.size[1]:
            w, h = im.size
            px = np.array(im.pixels[:], np.float64).reshape(h, w, im.channels)
            if im.channels < 3:
                px = np.repeat(px[:, :, :1], 3, 2)
            got = (np.rint(px[:, :, :3] * 255).astype(np.int64), texture_wrap(b.db, guid))
    if got is None:
        warn('a Mesh Cutter\'s mask texture %s cannot be read' % (a.path if a else guid))
    b._masks[guid] = got
    return got


# ---------------------------------------------------------------- the settings file

def _r(x, n=5):
    return round(float(x), n) + 0.0


def _v(v, n=5):
    return [_r(x, n) for x in v]


class Settings:
    """<avatar>.hypr3d.json: what the GLB cannot say"""

    def __init__(self, build, an, js, human, kept, base, ma):
        self.b, self.an, self.av, self.ma = build, an, build.av, ma
        self.js, self.human, self.kept, self.base = js, human, kept, base
        self.nodes = js.get('nodes', [])
        self.index = {}
        for i, n in enumerate(self.nodes):
            if n.get('name') is not None:
                self.index.setdefault(n['name'], i)
        self.parent, self.world = gltf_tree(js)
        self.world0 = list(self.world)  # as exported, before rest_transforms()

    # ---- nodes

    def node(self, g):
        """the GLB node of a GameObject"""
        return self.index.get(g.name) if g is not None and g.name else None

    def near(self, g):
        """the GLB node of a GameObject, or else of its nearest parent there"""
        while g is not None and id(g) in self.av.inside:
            n = self.node(g)
            if n is not None:
                return n
            g = self.ma.up(g)
        return None

    def tops(self, g, skip=lambda n: False, stop=()):
        """the GLB nodes of a GameObject, or else of the nearest ones under it"""
        out = []

        def walk(x):
            if id(x) in stop or id(x) not in self.av.inside:
                return
            n = self.node(x)
            if n is not None and not skip(n):
                out.append(n)
                return
            for c in self.ma.down(x):
                walk(c)
        walk(g)
        return out

    def part(self, r):
        """the name of a renderer's node, if it has one with a mesh"""
        n = self.node(r.go)
        return self.nodes[n]['name'] if n is not None and 'mesh' in self.nodes[n] else None

    def parts(self, rs):
        out = []
        for r in rs:
            p = self.part(r)
            if p and p not in out:
                out.append(p)
        return out

    def shape(self, smr, sh):
        """how the settings file names a shape key: "mesh node/shape key", or the key alone"""
        n = self.node(smr.go)
        if n is None or 'mesh' not in self.nodes[n]:
            return None
        key = self.b.keys.get(id(smr), {}).get(sh)
        mesh = self.js['meshes'][self.nodes[n]['mesh']]
        names = mesh.get('extras', {}).get('targetNames', [])
        if key is None or key not in names:
            return None
        return key if '/' in key else '%s/%s' % (self.nodes[n]['name'], key)

    def shapes(self, d, zeros=False):
        out = {}
        for (smr, sh), w in sorted(d.items(), key=lambda x: (str(x[0][0].gname), x[0][1])):
            k = self.shape(smr, sh)
            if k is None:
                warn('the shape key %s of %s is not in the GLB' % (sh, smr.gname))
            elif w > 1e-4 or zeros:
                out[k] = _r(w, 4)
        return out

    def tf_node(self, g, comps):
        """(the node, its local TRS as the GLB has it: (t, r as x y z w, s)) once a Transform's components are set so
        (Unity's local values over where it is), or None"""
        n, U = self.node(g), self.b.U
        if n is None or id(g) not in U:
            return None
        up = self.ma.up(g) if self.ma is not None else g.parent
        P = U[id(up)] if up is not None and id(up) in U else Matrix.Identity(4)
        Ug = U[id(g)]
        t, q, sc = (P.inverted_safe() @ Ug).decompose()
        rest = {'p': tuple(t), 'q': (q.x, q.y, q.z, q.w), 's': tuple(sc),
                'e': tuple(math.degrees(a) for a in q.to_matrix().to_euler('ZXY'))}
        own = self.av.tf_rest.get(id(g))
        if own is not None and abs(Euler([math.radians(a) for a in own['e']], 'ZXY').to_quaternion().dot(q)) > 1 - 1e-6:
            rest['e'] = own['e']
        D = FLIP @ P @ tf_local(comps, rest) @ Ug.inverted_safe() @ FLIP  # how the world has it move
        pn = self.parent[n]
        Wp = self.world0[pn] if pn >= 0 else Matrix.Identity(4)  # (a local value: from where the export has them)
        t2, q2, s2 = (Wp.inverted_safe() @ D @ self.world0[n]).decompose()
        return n, (tuple(t2), (q2.x, q2.y, q2.z, q2.w), tuple(s2))

    def node_trs(self, n):
        """a node's own local TRS in the GLB: (t, r as x y z w, s)"""
        pn = self.parent[n]
        Wp = self.world[pn] if pn >= 0 else Matrix.Identity(4)
        t, q, sc = (Wp.inverted_safe() @ self.world[n]).decompose()
        return tuple(t), (q.x, q.y, q.z, q.w), tuple(sc)

    def transforms(self, d, every=None):
        """{node name: {'t', 'r', 's'}}: where these Transforms' components ({GameObject: {component: value}}) put
        their nodes; only what differs from the node's own, or `every` {node: set of 't', 'r', 's'} of them"""
        out = {}
        for g, comps in sorted(d.items(), key=lambda x: str(x[0].name)):
            got = self.tf_node(g, comps)
            if got is None:
                warn('%s: a toggle or slider moves it, but it is not in the GLB' % go_name(g))
                continue
            n, (t, r, sc) = got
            t0, r0, s0 = self.node_trs(n)
            x = {}
            want = every.get(n) if every is not None else None
            # what differs by more than the GLB's float precision: 0.01 mm, 0.05 degrees, 0.001%
            if (want and 't' in want) or (want is None and (Vector(t) - Vector(t0)).length > 1e-5):
                x['t'] = _v(t, 6)
            if (want and 'r' in want) or (want is None and abs(Quaternion((r[3], *r[:3])).dot(
                    Quaternion((r0[3], *r0[:3])))) < 1 - 1e-7):
                x['r'] = _v(r, 6)
            if (want and 's' in want) or (want is None and (Vector(sc) - Vector(s0)).length > 1e-5):
                x['s'] = _v(sc, 6)
            if x:
                out[self.nodes[n]['name']] = x
        return out

    def loop(self, seconds, a, b):
        """a toggle's Smooth Loop: {'seconds', 'a': {'shapes', 'transforms'}, 'b': {...}}: what goes from a to b and back;
        None if it moves nothing hypr3d carries"""
        av = self.av
        props = {pr for pr in set(a) | set(b) if pr[0] in ('s', 't')}
        va = {pr: (a[pr] if pr in a else av.default(pr)) for pr in props}
        vb = {pr: (b[pr] if pr in b else av.default(pr)) for pr in props}
        out = {'seconds': _r(seconds, 4)}
        for side, vals in (('a', va), ('b', vb)):
            out[side] = {}
            sh = self.shapes({(pr[1], pr[2]): min(max(v / 100.0, 0.0), 1.0) for pr, v in vals.items() if pr[0] == 's'},
                             zeros=True)
            if sh:
                out[side]['shapes'] = sh
        tfa = {}
        tfb = {}
        for pr in props:
            if pr[0] == 't':
                tfa.setdefault(pr[1], {})[pr[2]] = va[pr]
                tfb.setdefault(pr[1], {})[pr[2]] = vb[pr]
        every = {}
        for d in (tfa, tfb):
            for n, x in self.transforms(d).items():
                every.setdefault(self.index[n], set()).update(x)
        if every:
            out['a']['transforms'] = self.transforms(tfa, every)
            out['b']['transforms'] = self.transforms(tfb, every)
        if out['a'] == out['b']:
            return None
        return out

    def rest_transforms(self):
        """what the avatar's animations do to its Transforms at rest, with every toggle off, into the GLB's nodes (as
        the shape keys' resting values go into its meshes)"""
        moved = self.an.transform_diff(self.base, {})
        done = 0
        for g, comps in moved.items():
            got = self.tf_node(g, comps)
            if got is None:
                continue
            n, (t, r, sc) = got
            nd = self.nodes[n]
            for k in ('matrix', 'translation', 'rotation', 'scale'):
                nd.pop(k, None)
            if Vector(t).length > 1e-9:
                nd['translation'] = list(t)
            if abs(r[3]) < 1 - 1e-9:
                nd['rotation'] = list(r)
            if any(abs(v - 1) > 1e-9 for v in sc):
                nd['scale'] = list(sc)
            done += 1
        if done:
            self.parent, self.world = gltf_tree(self.js)
            log('%d object(s) moved, turned or scaled as the avatar\'s animations have them at rest' % done)

    # ---- the parts of the file

    def humanoid(self):
        out = {}
        for h, g in self.human.items():
            if self.node(g) is not None:
                out[h] = g.name
        _, eyes = self.an.eyes()
        for h, g in eyes.items():  # the eyes VRChat moves are the descriptor's
            if self.node(g) is not None:
                out[h] = g.name
        return out

    def expressions(self):
        exprs, taken, presets = [], set(), set()
        for x in self.an.visemes() + [e for e in [self.an.eyes()[0]] if e]:
            sh = self.shapes(x['shapes'])
            if sh:
                exprs.append({'name': x['name'], 'preset': x['preset'], 'shapes': sh})
                taken.add(x['name'].lower())
                presets.add(x['preset'])
        gmap, gex = self.an.gestures()
        rename = {}
        for x in gex:
            sh = self.shapes(x['shapes'])
            if not sh:
                rename[x['name']] = 'none'
                continue
            nm, k = x['name'], 2
            while nm.lower() in taken:
                nm = '%s %d' % (x['name'], k)
                k += 1
            taken.add(nm.lower())
            rename[x['name']] = nm
            y = {'name': nm}
            if x.get('preset') and x['preset'] not in presets:
                y['preset'] = x['preset']
                presets.add(x['preset'])
            for k2 in ('blink', 'lookAt', 'mouth'):
                if k2 in x:
                    y[k2] = x[k2]
            y['shapes'] = sh
            exprs.append(y)
        gestures = None
        if gmap:
            gestures = {side: {g: rename.get(e, e) for g, e in m.items()} for side, m in gmap.items()}
        return exprs, gestures

    def outfit(self):
        av = self.av
        toggles, shown = [], set()
        vis0 = {id(r): av.visible(r, self.base) for r in av.renderers}

        def cut(show, hide, cuts):
            """the parts cut_meshes() made: (shown, hidden) in a state that shows and hides these renderers and has
            these cuts in effect, against the rest; a part shows with its mesh unless one of its cuts is in effect"""
            on, off = [], []
            for r, ks, nm in self.an.pieces:
                was = vis0[id(r)] and not ks & self.an.cuts0
                now = (r in show or (vis0[id(r)] and r not in hide)) and not ks & cuts
                if now and not was:
                    on.append(nm)
                elif was and not now:
                    off.append(nm)
            return on, off
        for t in self.kept:
            x = {'name': t['name']}
            if t['group']:
                x['group'] = t['group']
            if t.get('groups'):
                x['groups'] = t['groups']
            if t['on']:
                x['on'] = True
            show, hide, sh = self.parts(t['show']), self.parts(t['hide']), self.shapes(t['shapes'])
            on, off = cut(t['show'], t['hide'], t['cuts'])
            show, hide = show + on, hide + off
            if show:
                x['show'] = show
                shown |= set(show)
            if hide:
                x['hide'] = hide
            if sh:
                x['shapes'] = sh
            var = self.b.variant_names.get(t['name']) if t.get('materials') else None
            if var:
                x['variants'] = [var]
            tf = self.transforms(t['transforms']) if t.get('transforms') else {}
            if tf:
                x['transforms'] = tf
            loop = self.loop(*t['loop']) if t.get('loop') else None
            if loop:
                x['loop'] = loop
            drop = [self.nodes[n]['name'] for n in (self.node(g) for g in t.get('drop', [])) if n is not None]
            if drop:
                x['drop'] = drop
            x['_busy'] = bool(show or hide or sh or var or tf or loop or drop)
            toggles.append(x)
        busy = {g for x in toggles if x['_busy'] for g in x.get('groups', [x.get('group')])}
        toggles = [x for x in toggles if x.pop('_busy') or any(g and g in busy for g in x.get('groups', [x.get('group')]))]
        sliders = []
        for s in self.an.sliders:
            keys = []
            every = {}  # the components of each node the slider moves, at any key
            for k in s['keys']:
                for n, x in self.transforms(k['transforms']).items():
                    every.setdefault(self.index[n], set()).update(x)
            for k in s['keys']:
                y = {'at': _v(k['at'], 4) if s.get('axes') == 2 else _r(k['at'], 4)}
                show, hide = self.parts(k['show']), self.parts(k['hide'])
                on, off = cut(k['show'], k['hide'], k['cuts'])
                show, hide = show + on, hide + off
                if show:
                    y['show'] = show
                    shown |= set(show)
                if hide:
                    y['hide'] = hide
                sh = self.shapes(k['shapes'], zeros=True)
                if sh:
                    y['shapes'] = sh
                var = self.b.variant_names.get(slider_variant(s, k)) if k['materials'] else None
                if var:
                    y['variants'] = [var]
                tf = self.transforms(k['transforms'], every) if every else {}
                if tf:
                    y['transforms'] = tf
                keys.append(y)
            if s.get('axes') == 2:  # a puppet's: n x n keys over -1..1, row by row from the bottom
                sliders.append({'name': s['name'], 'axes': 2, 'value': _v(s['value'], 4), 'grid': s['grid'],
                                'keys': keys})
            else:
                sliders.append({'name': s['name'], 'value': _r(s['value'], 4), 'keys': keys})
        hidden = [p for p in self.parts([r for r in av.renderers if not av.visible(r, self.base)])
                  if p not in shown]
        hidden += [nm for r, ks, nm in self.an.pieces if not (vis0[id(r)] and not ks & self.an.cuts0) and nm not in shown]
        return hidden, toggles, sliders

    GLOBAL_SLOTS = ('head', 'torso', 'handL', 'handR', 'fingerIndexL', 'fingerIndexR', 'fingerMiddleL', 'fingerMiddleR',
                    'fingerRingL', 'fingerRingR', 'fingerLittleL', 'fingerLittleR', 'footL', 'footR')

    def global_colliders(self):
        """MA's Global Colliders that end up as one of the avatar's hand or finger colliders, which its own PhysBones
        collide with when they allow collision (VRChatGlobalColliderPass: manual ones first, the low priority ones
        first among them; the others take the fingers left, ring, middle, little, then index). A head, torso or foot
        one only touches contacts, which hypr3d does not have"""
        comps = self.ma.comps.get('GlobalCollider', []) if self.ma else []
        if not comps:
            return []
        manual = lambda c: truthy(c.data.get('m_manualRemap', '0'))
        order = sorted(comps, key=lambda c: (not manual(c), not (manual(c) and truthy(c.data.get('m_lowPriority', '0')))))
        used, low, slot, failed = set(), set(), {}, 0
        for c in order:
            d = c.data
            if manual(c):
                t = inum(d.get('m_colliderToHijack'), 14)
                if not 0 <= t < 14:
                    continue
                (low if truthy(d.get('m_lowPriority', '0')) else used).add(t)
            else:
                t = next((f for f in (8, 9, 6, 7, 10, 11, 4, 5) if f not in used and f not in low), None)
                if t is None:
                    t = next((f for f in (8, 9, 6, 7, 10, 11, 4, 5) if f in low), None)
                    low.discard(t)
                if t is None:
                    failed += 1
                    continue
                used.add(t)
            data = dict(d)
            if truthy(d.get('m_copyHijackedShape', '0')):  # the descriptor's own collider's shape
                own = dictof(av_desc_collider(self.av.desc, self.GLOBAL_SLOTS[t]))
                for a, b in (('m_radius', 'radius'), ('m_height', 'height'), ('m_position', 'position'),
                             ('m_rotation', 'rotation')):
                    if b in own:
                        data[a] = own[b]
            x = Obj(114, 'MonoBehaviour', data)
            x.comps = []
            slot[t] = x  # the last one to take a slot has it
        if failed:
            warn('%d Modular Avatar Global Collider(s) found no finger collider left to take' % failed)
        return [slot[t] for t in sorted(slot) if 2 <= t <= 11]

    def dynamics(self):
        """(colliders, springs)"""
        av, U = self.av, self.b.U
        bones, cols = self.an.dynamics()
        human = set()
        for g in self.human.values():
            n = self.node(g)
            while n is not None and n >= 0 and n not in human:
                human.add(n)
                n = self.parent[n]
        out_c, name_of, used = [], {}, set()

        def unique(nm, taken):
            nm = nm or 'unnamed'
            stem, k = nm, 2
            while nm.lower() in taken:
                nm = '%s %d' % (stem, k)
                k += 1
            taken.add(nm.lower())
            return nm
        gcs = self.global_colliders()
        for c in cols + gcs:
            d, go = c.data, c.go
            if go is None or id(go) not in av.inside:
                continue
            kind = None  # 'inside': it keeps the bones in; 'plane': they keep to the side its normal (its Y) points to
            if 'm_colliderToHijack' in d:  # MA's Global Collider: a capsule along its root's Y
                T = ma_objref(av, d.get('m_rootTransform')) or go
                r = num(d.get('m_radius'), 0.05)
                half = max(num(d.get('m_height'), 0.2) / 2 - r, 0.0)
                axis = Quaternion(quat(d.get('m_rotation'))) @ Vector((0.0, 1.0, 0.0))
                pos = Vector(vec3(d.get('m_position')))
                grow = max_scale(U[id(T)]) if id(T) in U else 1.0
            elif 'shapeType' in d:  # VRC PhysBone Collider
                t = d.get('rootTransform')
                T = t.go if isinstance(t, Obj) and t.go is not None and id(t.go) in av.inside else go
                shape = inum(d.get('shapeType'))
                if shape == 2:
                    kind = 'plane'
                elif truthy(d.get('insideBounds', '0')):
                    kind = 'inside'
                r = num(d.get('radius'), 0.5)
                half = max(num(d.get('height'), 1.0) / 2 - r, 0.0) if shape == 1 else 0.0
                axis = Quaternion(quat(d.get('rotation'))) @ Vector((0.0, 1.0, 0.0))
                pos = Vector(vec3(d.get('position')))
                grow = max_scale(U[id(T)]) if id(T) in U else 1.0
            else:  # Dynamic Bone Collider, or Plane Collider (no radius: its bound says which side the bones keep to)
                T = go
                axis = Vector([(1.0, 0.0, 0.0), (0.0, 1.0, 0.0), (0.0, 0.0, 1.0)][min(max(inum(d.get('m_Direction')), 0), 2)])
                if 'm_Radius' not in d:
                    kind = 'plane'
                    if inum(d.get('m_Bound')) == 1:
                        axis = -axis
                elif inum(d.get('m_Bound')) == 1:
                    kind = 'inside'
                r = num(d.get('m_Radius'), 0.5)
                half = max(num(d.get('m_Height')) / 2 - r, 0.0)
                pos = Vector(vec3(d.get('m_Center')))
                grow = U[id(T)].col[0].xyz.length if id(T) in U else 1.0
            n = self.near(T)
            if n is None and id(T) in U:  # on the avatar's root, which is no node: on the armature's top one
                hips = self.node(self.human['Hips']) if 'Hips' in self.human else None
                while hips is not None and self.parent[hips] >= 0:
                    hips = self.parent[hips]
                n = hips
            if n is None or id(T) not in U:
                continue
            Gi = self.world[n].inverted_safe()
            ends = [pos - axis * half, pos + axis * half] if half > 1e-6 and kind != 'plane' else [pos]
            pts = [Gi @ (FLIP @ (U[id(T)] @ p)) for p in ends]
            x = {'name': unique(str(go.data.get('m_Name', '')), used), 'node': self.nodes[n]['name'],
                 'offset': _v(pts[0])}
            if kind == 'plane':
                x['normal'] = _v((Gi.to_3x3() @ (FLIP.to_3x3() @ (U[id(T)].to_3x3() @ axis))).normalized(), 4)
            else:
                if len(pts) > 1:
                    x['tail'] = _v(pts[1])
                x['radius'] = _r(r * grow / max(avg_scale(self.world[n]), 1e-9))
                if kind == 'inside':
                    x['inside'] = True
            name_of[id(c)] = x['name']
            out_c.append(x)
        globals_ = [name_of[id(c)] for c in gcs if id(c) in name_of]
        springs, snames, want = [], set(), set()
        for c in bones:
            d, go = c.data, c.go
            if go is None or id(go) not in av.inside:
                continue
            refs = lambda k: [x.go for x in listof(d.get(k)) if isinstance(x, Obj) and x.go is not None]
            if 'pull' in d:  # VRC PhysBone
                t = d.get('rootTransform')
                R = t.go if isinstance(t, Obj) and t.go is not None else go
                roots, ignore = [R], refs('ignoreTransforms') + self.ma.ignores(c, R)
                pull, spring = num(d.get('pull'), 0.2), num(d.get('spring'), 0.2)
                stiff = num(d.get('stiffness'), 0.2) if inum(d.get('integrationType')) == 1 else 0.0
                g = num(d.get('gravity'))
                x = {'stiffness': _r(0.5 + 3.5 * pull + 2 * stiff, 3),
                     'drag': _r(min(max(0.35 + 0.6 * (1 - spring), 0.05), 0.95), 3),
                     'gravity': _r(0.4 * abs(g), 3)}
                if g < 0:
                    x['gravityDir'] = [0.0, 1.0, 0.0]
                rad = num(d.get('radius'))
                cl = [name_of[id(y)] for y in listof(d.get('colliders')) if isinstance(y, Obj) and id(y) in name_of]
                if inum(d.get('limitType')) != 0:
                    cl.append('body')
                allow = inum(d.get('allowCollision'), 1)
                if allow == 1 or (allow == 2 and truthy(dictof(d.get('collisionFilter')).get('allowSelf', '1'))):
                    cl += globals_  # the avatar's own hands and fingers, as far as they are converted
                imm = num(d.get('immobile')) if inum(d.get('immobileType')) == 0 else 0.0
                if inum(d.get('multiChildType')) == 0:  # a root with several children stays put
                    kids = [k for k in self.ma.down(R) if k not in ignore]
                    if len(kids) > 1:
                        roots = kids
            else:  # Dynamic Bone
                roots = [x.go for x in [d.get('m_Root')] + listof(d.get('m_Roots')) if isinstance(x, Obj) and x.go]
                roots = [r for i, r in enumerate(roots) if r not in roots[:i]]
                if not roots:
                    continue
                ignore = refs('m_Exclusions')
                gv = Vector(vec3(d.get('m_Gravity'))) + Vector(vec3(d.get('m_Force')))
                x = {'stiffness': _r(0.5 + 3.5 * num(d.get('m_Elasticity'), 0.1) + 2 * num(d.get('m_Stiffness'), 0.1), 3),
                     'drag': _r(min(max(0.4 + 0.5 * num(d.get('m_Damping'), 0.1), 0.05), 0.95), 3),
                     'gravity': _r(min(gv.length * 8, 1.0), 3)}
                if gv.length > 1e-6:
                    x['gravityDir'] = _v((FLIP.to_3x3() @ gv).normalized(), 4)
                rad = num(d.get('m_Radius'))
                cl = [name_of[id(y)] for y in listof(d.get('m_Colliders')) if isinstance(y, Obj) and id(y) in name_of]
                imm = num(d.get('m_Inert'))
                R = roots[0]
            stop = {id(y) for y in ignore}
            nodes = []
            for r in roots:
                for n in self.tops(r, skip=lambda n: n in human, stop=stop):
                    if n not in nodes:
                        nodes.append(n)
            if not nodes:
                warn('%s: its bones are not in the GLB, or are all humanoid bones' % go.data.get('m_Name', ''))
                continue
            y = {'name': unique(str(go.data.get('m_Name', '')), snames),
                 'bones': [self.nodes[n]['name'] for n in nodes]}
            ig = []
            for i in ignore:
                for n in self.tops(i):
                    if self.nodes[n]['name'] not in ig:
                        ig.append(self.nodes[n]['name'])
            if ig:
                y['ignore'] = ig
            y.update(x)
            meters = rad * (U[id(R)].col[0].xyz.length if 'pull' not in d else max_scale(U[id(R)])) if id(R) in U else rad
            y['radius'] = _r(meters / max(avg_scale(self.world[nodes[0]]), 1e-9))
            if imm > 0.9:
                y['immobile'] = _r(imm, 3)
            y['colliders'] = list(dict.fromkeys(cl))
            want |= set(y['colliders'])
            springs.append(y)
        return [x for x in out_c if x['name'] in want], springs

    def floor(self):
        """MA's Floor Adjuster (FloorAdjusterPass, after everything else MA does): the one that is active marks where
        the floor is, and MA moves the hips so that the avatar stands there. hypr3d stands an avatar on its lowest point
        (which is the same for the usual heels) unless the settings file says where its floor is: the adjuster's
        height, in the GLB's units. None without one"""
        comps = self.ma.comps.get('FloorAdjuster', []) if self.ma else []
        if not comps:
            return None

        def active(g):
            while g is not None and id(g) in self.av.inside:
                if self.av.default(('a', g)) < 0.5:
                    return False
                if g is self.av.root:
                    break
                g = g.parent
            return True
        on = [c for c in comps if c.go is not None and active(c.go)]
        if len(on) != 1:
            if on:
                warn('%d Modular Avatar Floor Adjusters are active, so none is used (as in MA)' % len(on))
            return None
        hips = self.human.get('Hips')
        if hips is None or hips is self.av.root or self.ma.up(hips) is None or id(on[0].go) not in self.b.U:
            warn('the Modular Avatar Floor Adjuster needs the humanoid hips under the avatar, so it is not used')
            return None
        return self.b.U[id(on[0].go)].translation.y

    def build(self, info):
        out = {}
        self.rest_transforms()
        h = self.humanoid()
        if h:
            out['humanoid'] = h
        f = self.floor()
        if f is not None:
            out['floor'] = _r(f)
        exprs, gestures = self.expressions()
        if exprs:
            out['expressions'] = exprs
        vis = {n: sh for n, sh in ((n, self.shapes(x)) for n, x in self.an.consonants().items()) if sh}
        if vis:
            out['visemes'] = vis
        if gestures:
            out['gestures'] = gestures
        hidden, toggles, sliders = self.outfit()
        if hidden:
            out['hidden'] = hidden
        if toggles:
            out['toggles'] = toggles
        if sliders:
            out['sliders'] = sliders
        fixed = [self.nodes[n]['name'] for n in (self.node(g) for g in (self.ma.fixed if self.ma else [])) if n is not None]
        if fixed:
            out['fixed'] = fixed
            log('%d object(s) held in the world (Modular Avatar World Fixed Object): %s' % (len(fixed), ', '.join(fixed)))
        cols, springs = self.dynamics()
        if cols:
            out['colliders'] = cols
        if springs:
            out['springs'] = springs
        out['converter'] = info
        return out


# ---------------------------------------------------------------- running it

def write_emotes(db, av, human, ma, acts, out):
    """the Action layers' humanoid clips as VRM animations next to the GLB, for the settings file's emotes"""
    axes, names = human_tpose(db, av, human, ma.U0)
    if axes is None:
        warn('the avatar has no humanoid T pose, so its %d emote(s) are not converted' % len(acts))
        return []
    shapes = {n for r in av.renderers if r.cls == 137 for n in av.shape_names(r)}
    emotes, used = [], set()
    for name, clip, loop, speed, params in acts:
        stem, k = safe_file_name(name), 2
        fn = '%s.%s.vrma' % (os.path.splitext(out)[0], stem)
        while fn in used:
            fn = '%s.%s %d.vrma' % (os.path.splitext(out)[0], stem, k)
            k += 1
        used.add(fn)
        if clip.tdof and getattr(axes, 'tdof', False):  # (an avatar without Translation DoF leaves them out, as here)
            warn('emote "%s": the bones it moves (Translation DoF: %s) only turn' % (name, ', '.join(sorted(clip.tdof))))
        dur, faces = write_vrma(fn, axes, names, clip, shapes)
        e = {'file': os.path.basename(fn), 'name': name, 'loop': loop}
        if not loop:
            e['hold'] = True  # as VRChat holds its last frame till the menu item goes off
        if abs(speed - 1.0) > 1e-4:
            e['speed'] = _r(speed, 4)
        emotes.append(e)
        log('emote "%s": %s, %.2f s%s%s, %d face curve(s) -> %s' % (
            name, clip.name, dur, ' looping' if loop else '', ', Foot IK' if clip.foot_ik else '', faces,
            os.path.basename(fn)))
    return emotes


class HandPoses:
    """the hand poses as a humanoid clip for write_vrma: sign g at time g (0 neutral ... 7 thumbs up), both hands"""

    def __init__(self, poses):
        self.poses, self.name = poses, 'Hand poses'
        self.rate, self.length, self.baked, self.faces = 1.0, 7.0, (True, True, True), {}

    def at(self, t):
        g = min(max(int(round(t)), 0), 7)
        mus = {}
        for hand in (0, 1):
            mus.update(self.poses.get((hand, g), {}))
        return mus, Vector((0.0, 1.0, 0.0)), Quaternion()


def write_hands(db, av, human, ma, poses, out):
    """the Gesture layers' hand poses, as a VRM animation next to the GLB with a sign at each second (HandPoses), and
    what the settings file's "hands" says of it; None without any"""
    if not poses:
        return None
    axes, names = human_tpose(db, av, human, ma.U0)
    if axes is None:
        warn('the avatar has no humanoid T pose, so its Gesture layer\'s hand poses are not converted')
        return None
    fn = '%s.hands.vrma' % os.path.splitext(out)[0]
    write_vrma(fn, axes, names, HandPoses(poses), set())
    st = {'file': os.path.basename(fn)}
    for hand, side in ((0, 'left'), (1, 'right')):
        signs = {GESTURES[g]: g for (h, g) in sorted(poses) if h == hand}
        if signs:
            st[side] = signs
    log('hand poses from the Gesture layer: %s -> %s' % ('; '.join('%s %s' % (k, ', '.join(v)) for k, v in st.items()
                                                                   if k != 'file'), st['file']))
    return st


def slider_variant(s, k):
    """the name of the material variant a slider's key shows"""
    if s.get('axes') == 2:
        return '%s %g, %g' % (s['name'], round(k['at'][0], 3), round(k['at'][1], 3))
    return '%s %g%%' % (s['name'], round(k['at'] * 100, 1))


def safe_file_name(s):
    s = re.sub(r'[\x00-\x1f/\\:*?"<>|]+', '_', s).strip(' .')
    return s or 'avatar'


def choose(found, want, named=()):
    if not want:
        # a .prefab or .unity named on the command line says which
        pool = [f for f in found if os.path.realpath(f.asset.file) in named] or found
        if len(pool) > 1:
            log('%d avatars found%s; converting "%s" (choose with --avatar, see --list)' % (
                len(pool), ' in ' + pool[0].asset.path if pool is not found else '', pool[0].name))
        elif pool is not found and len(found) > 1:
            log('converting "%s" from %s (the input holds %d avatars, see --list)' % (
                pool[0].name, pool[0].asset.path, len(found)))
        return pool[0]
    if re.fullmatch(r'\d+', want) and 1 <= int(want) <= len(found):
        return found[int(want) - 1]
    low = want.lower()
    for test in (lambda f: f.name.lower() == low, lambda f: plain_name(f.name) == plain_name(want),
                 lambda f: low in f.name.lower(), lambda f: low in f.asset.path.lower()):
        hits = [f for f in found if test(f)]
        if hits:
            return hits[0]
    raise Fail('no avatar called "%s" in the input (see --list)' % want)


def convert(db, found, opts, outfits=()):
    t0 = time.time()
    av = Avatar(db, found)
    log('avatar "%s" in %s: %d objects, %d renderers' % (av.name, found.asset.path, len(av.gos), len(av.renderers)))
    adopt(db, av)
    an = Analysis(db, av)
    acts = action_clips(an)  # emotes and dances: their menu items are not outfit toggles
    an.emote_params = {pn for *_, params in acts for pn in params}
    human = an.humanoid()
    setups = [OutfitSetup(db, av, human, o) for o in outfits]
    b = Build(db, av, opts)
    b.import_models()
    b.compute(human)
    for x in setups:
        x.fix(b.U)
    transform_rests(av, b.U)
    ma = ModularAvatar(av, dict(human), b.U, [m for x in setups for m in x.specs])
    an.ma = av.ma = ma
    for k, g in list(human.items()):  # a humanoid bone MA's Replace Object replaced: its replacement
        human[k] = ma.replaced.get(id(g), g)
    if an.vrcf is not None and an.vrcf.links:
        link_armatures(ma, an.vrcf, human, animated_transforms(an))
    jaw = human.get('Jaw')
    if jaw is not None and id(jaw) in an.chained():  # Unity's guess, which the avatar does not use as a jaw
        warn('the Jaw bone %s swings with a PhysBone, so it is left out of the humanoid map' % jaw.data.get('m_Name'))
        del human['Jaw']
    kept, base = an.toggles()
    b.arrange()
    b.place()
    b.materials(an, base)
    b.variants([(t['name'], t['materials']) for t in kept] +
               [(slider_variant(s, k), k['materials']) for s in an.sliders for k in s['keys']])
    b.shapes(base)
    b.names(human)
    out = os.path.abspath(opts.output or safe_file_name(av.name) + '.glb')
    if not out.lower().endswith(('.glb', '.vrm')):
        out += '.glb'
    os.makedirs(os.path.dirname(out), exist_ok=True)
    b.export(out)
    if opts.blend:
        bpy.ops.wm.save_as_mainfile(filepath=os.path.abspath(opts.blend), copy=True)
        log('saved the Blender scene as %s' % opts.blend)
    js, binc = read_glb(out)
    patch_materials(js, b.post)
    binc = material_extras(js, binc, b)
    binc, an.pieces = cut_meshes(js, binc, b, an)
    binc = ma.apply(js, binc)
    info = {'tool': 'unity2hypr3d', 'input': [os.path.basename(p.rstrip('/')) for p in opts.inputs],
            'prefab': found.asset.path, 'avatar': av.name, 'date': time.strftime('%Y-%m-%d %H:%M:%S')}
    st = Settings(b, an, js, human, kept, base, ma).build(info)
    emotes = write_emotes(db, av, human, ma, acts, out) if acts else []
    if emotes:
        st['emotes'] = emotes
    hands = write_hands(db, av, human, ma, an.hand_poses(), out)
    if hands:
        st['hands'] = hands
    st['converter'] = st.pop('converter')
    if an.skipped:
        info['skipped'] = ['%s: %s' % x for x in an.skipped]
    info['warnings'] = list(WARNINGS)
    write_glb(out, js, binc)
    sp = os.path.splitext(out)[0] + '.hypr3d.json'
    with open(sp + '.part', 'w', encoding='utf-8') as f:
        json.dump(st, f, indent=1, ensure_ascii=False)
        f.write('\n')
    os.replace(sp + '.part', sp)
    ex = st.get('expressions', [])
    log('wrote %s (%.1f MB) and %s' % (out, os.path.getsize(out) / 1e6, os.path.basename(sp)))
    log('%d humanoid bones, %d expressions (%s), %s gestures, %d toggles, %d parts hidden, %d springs, %d colliders' % (
        len(st.get('humanoid', {})), len(ex), ', '.join(x['name'] for x in ex) or 'none',
        'face' if st.get('gestures') else 'default', len(st.get('toggles', [])), len(st.get('hidden', [])),
        len(st.get('springs', [])), len(st.get('colliders', []))))
    for n, why in an.skipped:
        log('not converted: menu control "%s" (%s)' % (n, why))
    log('done in %.1f s, %d warning(s)' % (time.time() - t0, len(WARNINGS)))
    return 0


def run_blender(argv):
    exe = os.environ.get('BLENDER') or shutil.which('blender')
    if not exe:
        print('unity2hypr3d: error: this needs Blender 4.2 or newer: put blender on PATH or set BLENDER', file=sys.stderr)
        return 2
    cmd = [exe, '-b', '--factory-startup', '--python-exit-code', '1', '-P', os.path.abspath(__file__), '--'] + argv
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace')
    tail = []
    for line in p.stdout:
        if line.startswith('unity2hypr3d:'):
            sys.stdout.write(line)
            sys.stdout.flush()
        else:
            tail = (tail + [line])[-60:]
    code = p.wait()
    if code != 0:
        sys.stdout.write(''.join(tail))
    return code


def main():
    argv = sys.argv[sys.argv.index('--') + 1:] if bpy is not None and '--' in sys.argv else (
        [] if bpy is not None else sys.argv[1:])
    ap = argparse.ArgumentParser(prog='unity2hypr3d', description=__doc__.split('\n\n')[0],
                                 epilog=__doc__.split('\n\n', 1)[1], formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('inputs', nargs='+', metavar='INPUT')
    ap.add_argument('-o', '--output', metavar='OUT.glb')
    ap.add_argument('--avatar', metavar='NAME')
    ap.add_argument('--list', action='store_true')
    ap.add_argument('--max-texture', type=int, default=2048, metavar='N')
    ap.add_argument('--keep', metavar='DIR')
    ap.add_argument('--outfit', action='append', default=[], metavar='NAME|PATH')
    ap.add_argument('--blend', metavar='FILE', help=argparse.SUPPRESS)
    opts = ap.parse_args(argv)
    if bpy is None and not opts.list:
        return run_blender(argv)
    work = os.path.abspath(opts.keep) if opts.keep else tempfile.mkdtemp(prefix='unity2hypr3d-')
    os.makedirs(work, exist_ok=True)
    try:
        db = DB(work)
        named = set()
        for p in opts.inputs:
            if not os.path.exists(p):
                raise Fail('%s does not exist' % p)
            f = db.add_input(p)
            if f:
                named.add(os.path.realpath(f))
        found = find_avatars(db)
        if not found:
            raise Fail('no VRChat avatar (an object with a VRC Avatar Descriptor) in the input')
        # the avatars in a file named on the command line come first
        found.sort(key=lambda f: os.path.realpath(f.asset.file) not in named)
        if named and not opts.avatar and os.path.realpath(found[0].asset.file) not in named:
            log('no avatar in %s itself; looking through its project' % ', '.join(
                sorted(os.path.basename(n) for n in named)))
        if opts.list:
            for i, f in enumerate(found, 1):
                print('%d. %s  (%s)' % (i, f.name, f.asset.path))
            return 0
        found = choose(found, opts.avatar, named)
        outfits = [put_on(db, found, find_outfit(db, x), k) for k, x in enumerate(opts.outfit, 1)]
        return convert(db, found, opts, outfits)
    except Fail as e:
        print('unity2hypr3d: error: %s' % e, flush=True)
        return 1
    finally:
        if not opts.keep:
            shutil.rmtree(work, ignore_errors=True)


if __name__ == '__main__':
    sys.exit(main())
