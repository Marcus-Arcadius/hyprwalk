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
  face gestures from the FX controller, Expressions Menu toggles (objects shown and hidden,
  shape keys set), objects that start hidden, PhysBones and Dynamic Bones as springs and
  colliders, and the materials (colour, texture, cutout or transparent, emission, culling).

Modular Avatar setups are built as MA builds them for VRChat: Merge Armature (an outfit's bones
join the avatar's and its meshes follow the avatar's bones), Bone Proxy, Move To and PhysBone
Blocker, and its menus and toggles (Menu Item, Menu Installer and Group, Object Toggle, Shape
Changer, Merge Animator for FX, Parameters). --outfit puts on an outfit the avatar's prefab does
not have yet, as MA's Setup Outfit does, whether or not the outfit is set up for MA.

VRCFury setups are built after MA's, as VRCFury builds them: Armature Link (an outfit's bones
linked to the avatar's, snapped on if it says so, its meshes following the avatar's bones),
Toggles (menu toggles that turn objects on and off, set shape keys and play clips, with exclusive
tags and the resting state they give the avatar), Full Controller (an FX controller, menus and
parameters merged in), Blend Shape Link, Apply and Delete During Upload, and the older Modes,
Object State and Bone Constraint features. An outfit linked with VRCFury is put on as it is.

What it does not: shader effects beyond the above, animations other than faces and toggles,
material swaps, MA's other components (Replace Object, Blendshape Sync, Material Setter and Swap,
Visible Head Accessory, Mesh Settings), VRCFury's other features (sliders, puppets, SPS, gesture
drivers, blinking and visemes and the like; each is named in a warning), constraints, particles,
audio, contacts. Blender imports only binary FBX files, so a model in any other format stops the
conversion.
"""

import sys, os, re, io, json, math, struct, zlib, tarfile, zipfile, tempfile, shutil, copy
import subprocess, argparse, time

try:
    import bpy
    import mathutils
    from mathutils import Matrix, Vector, Quaternion
except ImportError:
    bpy = mathutils = Matrix = Vector = Quaternion = None

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
                    idx = set(v for v in arr[1][0] if 0 <= v < len(mats))
                    used = sorted(idx) or [0]
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
    '1895bf16884f4064f8e9550e7493c205': 'OutfitRoot', '7e949680c0864ee7b441d9b2c93b890b': 'ReplaceObject'}
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
        gone, todo = set(), [self.root]  # what VRCFury deletes during the upload
        while todo:
            g = todo.pop()
            todo += g.children
            for c in g.comps:
                if is_vrcfury(c):
                    for f in vrcf_features(c):
                        if f['@class'] == 'DeleteDuringUpload':
                            t = f.get('@target', c.go)
                            gone.add(id(t if isinstance(t, Obj) and t.cls == 1 else getattr(t, 'go', None)))

        def walk(g, p):
            if g is not self.root and (g.data.get('m_TagString') == 'EditorOnly' or id(g) in gone):
                return
            self.gos.append(g)
            self.paths.setdefault(p, g)
            for c in g.children:
                walk(c, (p + '/' if p else '') + str(c.data.get('m_Name', '')))
        walk(self.root, '')
        if gone:
            log('VRCFury: Delete During Upload leaves out %d object(s)' % len(gone))
        self.inside = set(map(id, self.gos))
        self.renderers = [c for g in self.gos for c in g.comps if c.cls in RENDERERS]
        self._shapes = {}
        self.ma = None  # the ModularAvatar once it has run

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
        if k == 'e':
            return 1.0 if truthy(o.data.get('m_Enabled', '1')) else 0.0
        names = self.shape_names(o)
        if prop[2] in names:
            w = listof(o.data.get('m_BlendShapeWeights'))
            i = names.index(prop[2])
            return num(w[i]) if i < len(w) else 0.0
        return 0.0

    def prop(self, path, cls, attr):
        """what an animation curve drives: ('a', GameObject) active, ('e', renderer) enabled,
        ('s', renderer, shape) a blend shape weight; None for anything else"""
        go = self.paths.get(path)
        if go is None:
            return None
        if cls == 1 and attr == 'm_IsActive':
            return ('a', go)
        if cls in RENDERERS and attr == 'm_Enabled':
            c = self.comp(go, cls)
            return ('e', c) if c else None
        if cls == 137 and attr.startswith('blendShape.'):
            c = self.comp(go, 137)
            return ('s', c, attr[11:]) if c else None
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


def join_path(base, path):
    return base + '/' + path if base and path else base or path


class Clip:
    def __init__(self, av, body, base=''):
        self.name = str(body.get('m_Name', ''))
        st = dictof(body.get('m_AnimationClipSettings'))
        self.loop = truthy(st.get('m_LoopTime', '0'))
        self.curves = []  # (prop, [(time, value)])
        self.other = 0  # curves this tool does not carry over
        end = 0.0
        curves = listof(body.get('m_FloatCurves')) or listof(body.get('m_EditorCurves'))
        for c in curves:
            c = dictof(c)
            keys = sorted((num(k.get('time')), num(k.get('value')))
                          for k in listof(dictof(c.get('curve')).get('m_Curve')) if isinstance(k, dict))
            if not keys:
                continue
            path, cid, attr = str(c.get('path') or ''), inum(c.get('classID')), str(c.get('attribute') or '')
            if isinstance(base, tuple):  # VRCFury's: the first of these places that has it ("/...": the root)
                p = av.prop(path[1:], cid, attr) if path.startswith('/') else next(
                    (x for x in (av.prop(join_path(b, path), cid, attr) for b in base) if x is not None), None)
            else:
                p = av.prop(join_path(base, path), cid, attr)
            if p is None:
                self.other += 1
                continue
            self.curves.append((p, keys))
            end = max(end, keys[-1][0])
        self.other += len(listof(body.get('m_PPtrCurves')))
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
                            v = v0 if p[0] != 's' or t1 <= t0 else v0 + (v1 - v0) * (t - t0) / (t1 - t0)
                            break
            out[p] = v
        return out


class Anim:
    """clips, blend trees and animator controllers, read when first used; base: where their paths start (an
    animator MA merges with relative paths), or a tuple of places to look in turn (VRCFury's)"""

    def __init__(self, db, av, base=''):
        self.db, self.av, self.base = db, av, base
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
            self._clips[p] = Clip(self.av, b, self.base) if b is not None else None
        return self._clips[p]

    def mix(self, parts):
        """[(weight, values)] with weights adding up to one"""
        props = set()
        for _, d in parts:
            props.update(d)
        out = {}
        for pr in props:
            out[pr] = sum(w * (d[pr] if pr in d else self.av.default(pr)) for w, d in parts)
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
            out = {}
            for w, k in zip(ws, kids):
                if w == 0:
                    continue
                for pr, v in val(k).items():
                    d = self.av.default(pr)
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
        best = min(kids, key=lambda k: (vec3(k.get('m_Position'))[0] - x) ** 2 +
                   (vec3(k.get('m_Position'))[1] - y) ** 2)
        return val(best)


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
                vals.update(self.anim.motion(m, params, u, self.over, names))
            if not changed:
                break
        return vals, names, track, params


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

MA_REACTIVE = ('ObjectToggle', 'ShapeChanger', 'MaterialSetter', 'MaterialSwap')
MA_MENU = ('MenuItem', 'MenuGroup', 'MenuInstaller', 'ObjectToggle', 'ShapeChanger', 'MergeAnimator', 'Parameters')


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


def av_path(av, g):
    parts = []
    while g is not None and g is not av.root:
        parts.append(go_name(g))
        g = g.parent
    return '/'.join(reversed(parts))


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
        self.rules = self.reactions()
        self.merged = self.animators()

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
        """the menu item or group on a GameObject"""
        return next((c for c in g.comps if ma_kind(c) in ('MenuItem', 'MenuGroup')), None)

    def controls(self):
        """[(path of menu names, control)] MA adds to the avatar's menu (VirtualMenu)"""
        out, av = [], self.av
        by_target = {}
        for c in self.comps.get('MenuInstaller', []):
            if not truthy(c.data.get('m_Enabled', '1')):
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
            if ma_kind(src) == 'MenuGroup':
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
                    out.append((prefix, {'name': name, 'type': t, 'parameter': {'name': pn}, 'value': v}))
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
        props, deletes = {}, 0
        for c in self.comps.get('ShapeChanger', []):
            for e in listof(c.data.get('m_shapes', c.data.get('Shapes'))):
                e = dictof(e)
                g = ma_objref(self.av, e.get('Object'))
                smr = next((x for x in g.comps if x.cls == 137), None) if g is not None else None
                shape = _str(e.get('ShapeName'))
                if smr is None or shape not in self.av.shape_names(smr):
                    continue
                gone = inum(e.get('ChangeType')) == 0
                deletes += gone
                props.setdefault(('s', smr, shape), []).append(
                    (100.0 if gone else num(e.get('Value')), self.conditions(c), truthy(c.data.get('m_inverted', '0'))))
        for c in self.comps.get('ObjectToggle', []):
            for e in listof(c.data.get('m_objects')):
                e = dictof(e)
                g = ma_objref(self.av, e.get('Object'))
                if g is not None:
                    props.setdefault(('a', g), []).append((1.0 if truthy(e.get('Active', '0')) else 0.0,
                                                           self.conditions(c), truthy(c.data.get('m_inverted', '0'))))
        if deletes:
            warn('%d Shape Changer shape(s) delete the mesh they move; they are set to 100 instead' % deletes)
        return props

    def conditions(self, comp):
        """BuildConditions: the first menu item from the component up is on, and every object from it up is active"""
        out, item = [], False
        g = comp.go
        while g is not None and g is not self.av.root:
            if not item:
                mi = next((c for c in g.comps if ma_kind(c) == 'MenuItem'), None)
                if mi is not None:
                    item = True
                    pv = self.item.get(id(mi))
                    if pv is not None:
                        out.append(('p',) + pv)
            out.append(('a', g))
            g = g.parent
        return out

    def apply(self, params, vals):
        """what the reactive components set, over the animators' values"""
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
        out.sort(key=lambda x: (x[0], x[1]))
        return out


# ---------------------------------------------------------------- what the avatar does: toggles, faces, visemes

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
        self.ma = None  # the ModularAvatar once it has run

    def evaluate(self, params):
        runs = [(x[2], x[3]) for x in self.merged if x[0] < 0] + ([(self.fx, {})] if self.fx else []) + [
            (x[2], x[3]) for x in self.merged if x[0] >= 0]
        if not runs and self.mat is None and self.vrcf is None:
            return {}, {}, {}, params
        if len(runs) == 1 and not runs[0][1] and self.mat is None and self.vrcf is None:
            return runs[0][0].evaluate(params)
        vals, names, track, out = {}, {}, {}, dict(params)
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
        if self.vrcf:  # VRCFury's toggle layers come after everything MA made
            vals = self.vrcf.apply(out, vals)
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

    def toggles(self):
        """(toggles, the values the avatar rests at with every toggle off)"""
        av = self.av
        ctrls = []
        for prefix, c in self.menu:
            t = inum(c.get('type'))
            name = rich_text(str(c.get('name') or ''))
            pn = str(dictof(c.get('parameter')).get('name') or '')
            if t not in (101, 102):
                self.skipped.append((name, {201: 'a two-axis puppet', 202: 'a four-axis puppet',
                                            203: 'a radial puppet'}.get(t, 'not a toggle')))
                continue
            if not pn:
                continue
            ctrls.append((prefix, name or pn, pn, num(c.get('value'), 1.0), t, _str(c.get('group'))))
        values = {}
        for _, _, pn, v, _, _ in ctrls:
            values.setdefault(pn, set()).add(v)
        base_p = dict(self.params)
        for pn in values:
            base_p[pn] = 0.0
        base = self.evaluate(base_p)[0]
        vis0 = {id(r): av.visible(r, base) for r in av.renderers}
        out, used = [], set()
        for prefix, name, pn, v, t, grp in ctrls:
            p = dict(base_p)
            p[pn] = v
            vals = self.evaluate(p)[0]
            show = [r for r in av.renderers if av.visible(r, vals) and not vis0[id(r)]]
            hide = [r for r in av.renderers if not av.visible(r, vals) and vis0[id(r)]]
            shapes = self.shape_diff(vals, base)[0]
            ptype = self.eparams.get(pn, (None, 0))[0]
            if ptype is None and self.animator_param(pn):
                ptype = {1: 1, 3: 0, 4: 2}.get(self.animator_param(pn)[0], 2)
            group = grp or (pn if (len(values[pn]) > 1 or ptype in (0, 1)) else '')
            on = t == 102 and abs(self.params.get(pn, 0.0) - v) < 1e-4
            nm = name
            if norm_name(nm) in used and prefix:
                nm = '%s %s' % (prefix[-1], name)
            k, stem = 2, nm
            while norm_name(nm) in used or not norm_name(nm):
                nm = '%s %d' % (stem, k)
                k += 1
            used.add(norm_name(nm))
            out.append({'name': nm, 'group': group, 'on': on, 'show': show, 'hide': hide,
                        'shapes': shapes, 'param': pn, 'value': v})
        # one that changes nothing is kept only as the "none of these" choice of a group
        busy = {x['group'] for x in out if x['show'] or x['hide'] or x['shapes']}
        kept = []
        for x in out:
            if x['show'] or x['hide'] or x['shapes'] or (x['group'] and x['group'] in busy):
                kept.append(x)
            else:
                self.skipped.append((x['name'], 'it changes nothing this tool carries over'))
        return kept, base

    def gestures(self):
        """(the gesture map, the expressions it uses); (None, []) when FX has no gesture faces"""
        if self.fx is None and not self.merged:
            return None, []
        av = self.av
        base = self.evaluate(dict(self.params))[0]
        faces, exprs, neg = {}, {}, False
        for side, P in (('left', 'GestureLeft'), ('right', 'GestureRight')):
            for g in range(1, 8):
                p = dict(self.params)
                p[P] = float(g)
                p[P + 'Weight'] = 1.0
                vals, names, track, _ = self.evaluate(p)
                shapes, n = self.shape_diff(vals, base, positive=True)
                neg = neg or n
                if not shapes:
                    faces[(side, g)] = None
                    continue
                src = {}
                for (smr, sh), w in shapes.items():
                    c = names.get(('s', smr, sh))
                    if c:
                        src[c] = src.get(c, 0.0) + w
                key = (tuple(sorted(((id(k[0]), k[1]), round(w, 3)) for k, w in shapes.items())),
                       track.get('eyes') == 2, track.get('mouth') == 2)
                if key not in exprs:
                    exprs[key] = {'shapes': shapes, 'clip': max(src, key=src.get) if src else '',
                                  'eyes': key[1], 'mouth': key[2], 'first': (side, g)}
                faces[(side, g)] = key
        if not exprs:
            return None, []
        if neg:
            warn('some gesture faces turn shape keys down, which hypr3d does not do; only what they turn up is kept')
        shape_names = {norm_name(n) for r in av.renderers if r.cls == 137 for n in av.shape_names(r)}
        used, presets, out = set(), set(), []
        name_of = {}
        for key, e in exprs.items():
            side, g = e['first']
            nm = face_name(e['clip']) or '%s %s' % (side, GESTURES[g])
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
        return gmap, out

    def visemes(self):
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
                elif 'm_Bound' in d and 'm_Direction' in d and 'm_Radius' in d:
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


def to_linear(c):
    if c > 1.0:  # an HDR colour
        return c ** 2.2
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


class MatInfo:
    """a Unity material, as far as glTF carries it"""

    def __init__(self, name):
        self.name = name
        self.shader = ''
        self.tex = None  # image file
        self.tex_xf = ((1.0, 1.0), (0.0, 0.0))  # Unity's tiling and offset
        self.color = (1.0, 1.0, 1.0, 1.0)  # linear
        self.mode = 'OPAQUE'  # OPAQUE, MASK, BLEND
        self.cutoff = 0.5
        self.double = False
        self.emit = (0.0, 0.0, 0.0)  # linear, may go over 1
        self.emit_tex = None
        self.emit_xf = ((1.0, 1.0), (0.0, 0.0))

    def key(self):
        return (self.name, self.tex, self.tex_xf, self.color, self.mode, self.cutoff, self.double,
                self.emit, self.emit_tex, self.emit_xf)


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
                        m = re.search(r'^\s*Shader\s+"([^"]*)"', fh.read(1 << 16), re.M)
                    if m:
                        name = m.group(1)
                except OSError:
                    pass
            self._shaders[g] = name
        return self._shaders[g]

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
        if m.mode != 'OPAQUE' and m.tex and not self.tex_meta.get(m.tex, (0, True))[1] and m.color[3] >= 0.999:
            m.mode = 'OPAQUE'  # the texture's alpha is switched off and nothing else is see-through
        m.cutoff = min(max(fl.get('_Cutoff', fl.get('_Clipping_Level', fl.get('_AlphaCutoff', 0.5))), 0.0), 1.0)
        m.double = int(round(fl.get('_Cull', fl.get('_CullMode', fl.get('_Culling', 2.0))))) == 0
        on = '_EMISSION' in kw or 'mtoon' in low or any(
            fl.get(k, 0.0) > 0.5 for k in ('_EnableEmission', '_UseEmission', '_EmissionEnabled', '_UseEmissive'))
        c = col.get('_EmissionColor')
        if on and c and max(c[:3]) > 0.004:
            m.emit = tuple(to_linear(x) for x in c[:3])
            t = tex.get('_EmissionMap')
            if t and ref(t[0])[0]:
                m.emit_tex = self.image(t[0])
                if m.emit_tex:
                    m.emit_xf = (t[1], t[2])
        return m

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

MA_NOT_CONVERTED = (('ReplaceObject', 'Replace Object'), ('BlendshapeSync', 'Blendshape Sync'),
                    ('MaterialSetter', 'Material Setter'), ('MaterialSwap', 'Material Swap'),
                    ('VisibleHeadAccessory', 'Visible Head Accessory'), ('MeshSettings', 'Mesh Settings'))


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
        self.db = {}  # MA's bone database: id(bone) -> merged (True) or kept (False), in the order added
        self.counts = {'merged': 0, 'removed': 0, 'proxies': 0, 'moves': 0, 'duplicates': 0}
        self._mangled = 0
        comps = {}
        for g in av.gos:
            for c in g.comps:
                k = ma_kind(c)
                if k:
                    comps.setdefault(k, []).append(c)
        self.comps = comps
        self.pbblock = {id(c.go) for c in comps.get('PBBlocker', [])}
        for k, name in MA_NOT_CONVERTED:
            if comps.get(k):
                warn('%d Modular Avatar %s component(s): not converted' % (len(comps[k]), name))
        # MA reads every object reference before it changes anything
        self.ref = {id(c): self.objref(c.data.get('mergeTarget' if k == 'MergeArmature' else 'target'))
                    for k in ('MergeArmature', 'MoveTo') for c in comps.get(k, [])}
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
        weighted to the bones their bones merged into, and the merged bones that went removed"""
        av = self.av
        moved = [g for g in av.gos if id(g) not in self.deleted and self.parent[id(g)] is not (
            g.parent if g is not self.root else None)]
        if not (moved or self.deleted or self.retarget):
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
                if g is None or id(g) not in self.retarget:
                    continue
                m = anchor(self.retarget[id(g)])
                if m < 0:
                    continue
                d = self.retarget[id(g)]
                # where MA's retargeting left the bone: moving with the bone it merged into since then
                V = self.bound.get(id(g))
                if V is None:
                    V = self.U[id(d)] @ self.U0[id(d)].inverted_safe() @ self.U0[id(g)]
                Lc = FLIP @ V @ self.U[id(g)].inverted_safe() @ FLIP
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
VRCF_DONE = {'ArmatureLink', 'Toggle', 'FullController', 'ApplyDuringUpload', 'DeleteDuringUpload', 'BlendShapeLink'}
# features that change nothing hypr3d shows: left out without a word
VRCF_QUIET = {'AnchorOverrideFix', 'AnchorOverrideFix2', 'BoundingBoxFix', 'BoundingBoxFix2', 'BlendshapeOptimizer',
              'DirectTreeOptimizer', 'FixWriteDefaults', 'MakeWriteDefaultsOff', 'MakeWriteDefaultsOff2', 'Slot4Fix',
              'UnlimitedParameters', 'DescriptorDebug', 'Gizmo', 'SetIcon', 'MoveMenuItem', 'ReorderMenuItem',
              'OverrideMenuSettings', 'MmdCompatibility', 'CrossEyeFix', 'CrossEyeFix2', 'TpsScaleFix',
              'ShowInFirstPerson', 'HeadChopHead', 'SpsOptions', 'SecurityLock'}
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

    def actions(x, depth=0):  # an Object Toggle action from before modes: it flips the object
        if isinstance(x, dict):
            if x.get('@class') == 'ObjectToggleAction' and inum(x.get('version'), -1) < 1:
                x['mode'] = 2
            for y in x.values():
                if depth < 20:
                    actions(y, depth + 1)
        elif isinstance(x, list):
            for y in x:
                actions(y, depth + 1)
    actions(f)
    return [f]


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
        self.rules = []  # [(parameter, test, {property: value})]: test 'on' (a bool at 1), 'nonzero' or 'slider'
        self.menu = []  # [(path of menu names, control)]
        self.merged = []  # [(priority, order, Controller, {its parameter names: the avatar's}, False)]
        self.links = [(c, f) for c, f in self.feats if f['@class'] == 'ArmatureLink']
        self.skipped = []
        self.missed = {}  # actions and features that are not converted: kind -> count
        others = {}
        for c, f in self.feats:
            k = f['@class']
            if k not in VRCF_DONE and k not in VRCF_QUIET:
                others[k] = others.get(k, 0) + 1
        self.upload()
        self.rest()
        self.shape_links = self.blendshape_links()
        for base, sk, m in self.shape_links:  # the linked meshes rest as the base does
            for a, bs in m.items():
                for b in bs:
                    self.set_data(('s', sk, b), self.av.default(('s', base, a)))
        self.toggles()
        self.full_controllers()
        for k, n in sorted(others.items()):
            warn('%d VRCFury %s feature(s): not converted' % (n, re.sub(r'(?<=[a-z])(?=[A-Z])', ' ', k)))
        for k, n in sorted(self.missed.items()):
            warn('VRCFury: %d %s: not converted' % (n, k))
        n = {k: sum(1 for c, f in self.feats if f['@class'] == k) for k in VRCF_DONE}
        said = ['%d %s' % (n[k], t) for k, t in (('ArmatureLink', 'Armature Link'), ('Toggle', 'Toggle'),
                                                 ('FullController', 'Full Controller'),
                                                 ('BlendShapeLink', 'Blend Shape Link'),
                                                 ('ApplyDuringUpload', 'Apply During Upload'),
                                                 ('DeleteDuringUpload', 'Delete During Upload')) if n[k]]
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
                        self.miss('toggle clip(s) with curves other than objects and shape keys')
                    out.update(clip.sample(None))
            elif count:
                self.miss('%s action(s) in toggles' % re.sub(r'(?<=[a-z])(?=[A-Z])', ' ', k[:-6] if k.endswith(
                    'Action') else k))
        return out

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
        if pr[0] == 'a':
            pr[1].data['m_IsActive'] = '1' if v >= 0.5 else '0'
        elif pr[0] == 's':
            names = self.av.shape_names(pr[1])
            if pr[2] in names:
                w = pr[1].data.get('m_BlendShapeWeights')
                if not isinstance(w, list):
                    w = pr[1].data['m_BlendShapeWeights'] = []
                i = names.index(pr[2])
                while len(w) <= i:
                    w.append('0')
                w[i] = v

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
            props = self.props(f.get('localState' if local else 'state'), c)
            if not props and truthy(f.get('hasTransition', '0')):  # an empty main state holds the end of the in one
                props = self.props(f.get('localTransitionStateIn' if local else 'transitionStateIn'), c)
            g = _str(f.get('globalParam')).strip() if truthy(f.get('useGlobalParam', '0')) else ''
            param = g or 'VF%d_%s' % (k, name or 'Toggle')
            if truthy(f.get('slider', '0')):
                self.declared.setdefault(param, (1, num(f.get('defaultSliderValue'))))
                self.rules.append((param, 'slider', props))
                if path:
                    self.skipped.append((path[-1], 'a slider'))
                continue
            dflt = 1.0 if truthy(f.get('defaultOn', '0')) else 0.0
            self.declared.setdefault(param, (2, dflt))
            self.rules.append((param, 'on', props))
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
            group = next((t for t in tags if count[t] > 1), '')
            if group and dflt:
                group_on[group] = True
        for path, param, dflt, tags, hold, off in entries:
            group = next((t for t in tags if count[t] > 1), '')
            if off and group and not group_on.get(group):  # on while the others of its tag are off: at first
                self.declared[param] = (2, 1.0)
                group_on[group] = True
            if not path:  # no menu item
                continue
            ctl = {'name': path[-1], 'type': 101 if hold else 102, 'parameter': {'name': param}, 'value': 1.0}
            if group:
                ctl['group'] = group
            self.menu.append((tuple(path[:-1]), ctl))

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

    def apply(self, params, vals):
        """what the toggles set, over the animators' values (their FX layers come last); then what linked shape keys
        follow"""
        if not self.rules and not self.shape_links:
            return vals
        vals = dict(vals)
        for pn, test, props in self.rules:
            x = params.get(pn, 0.0)
            if test == 'slider':  # from where it rests to the full values
                if x <= 0:
                    continue
                w = min(x, 1.0)
                for pr, t in props.items():
                    if pr[0] == 's':
                        d = vals[pr] if pr in vals else self.av.default(pr)
                        vals[pr] = d + (t - d) * w
                    elif w >= 0.5:
                        vals[pr] = t
            elif (abs(x) > 1e-6) if test == 'nonzero' else (abs(x - 1.0) < 0.5):
                vals.update(props)
        for base, sk, m in self.shape_links:
            for a, bs in m.items():
                pr = ('s', base, a)
                if pr in vals:
                    for b in bs:
                        vals[('s', sk, b)] = vals[pr]
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
            if listof(f.get('rewriteBindings')):
                warn('%s: its Full Controller rewrites animation paths, which is not converted' % go_name(c.go))
            for e in listof(f.get('controllers')):
                e = dictof(e)
                p = vrcf_asset(e.get('controller'))
                if not p:
                    continue
                if inum(e.get('type'), 5) != 5:
                    self.miss('Full Controller controller(s) for layers other than FX')
                    continue
                if self.db.get(p[0]) is None:
                    warn('%s: the controller its Full Controller merges is not in the input' % go_name(c.go))
                    continue
                ctl = Controller(Anim(self.db, self.av, self.bases(root)), p[0])
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
                self.rules.append((rn(tp), 'nonzero', {('a', root): 1.0}))


def link_armatures(h, vf, human):
    """VRCFury's Armature Links, on the hierarchy Modular Avatar left (h)"""
    av, U = h.av, h.U
    humans = {id(g) for g in human.values()}
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
        stay = []  # bones left where they are, and so all under them
        for p, q in reversed(pairs):  # the ones found last first, as VRCFury does
            if p is not prop and id(q) not in humans and (id(p) in pb_kids or any(h.under(p, s) for s in stay)):
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
            if id(p) not in pb_roots and id(p) not in pb_kids:
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


# ---------------------------------------------------------------- the Blender scene

class Build:
    """the avatar in Blender: its models imported, placed as Unity has them, and exported as a GLB"""

    def __init__(self, db, av, opts):
        self.db, self.av, self.opts = db, av, opts
        self.mats = Materials(db)
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
        self._images = {}
        self._relinked = set()
        self._tmp = 0

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
        drop = []
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
        for o in drop:
            bpy.data.objects.remove(o, do_unlink=True)
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
        if m.tex:
            im = self.load_image(m.tex)
            if im is not None:
                tn = nt.nodes.new('ShaderNodeTexImage')
                tn.image = im
                nt.links.new(tn.outputs['Color'], bsdf.inputs['Base Color'])
                if m.mode != 'OPAQUE' and self.mats.tex_meta.get(m.tex, (0, True))[1]:
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

    def materials(self):
        made = {}
        for r in self.av.renderers:
            b = self.counterpart(r.go) if r.go else None
            if b is None or b[0] != 'obj' or b[1].type != 'MESH':
                continue
            o = b[1]
            shared = o.data.users > 1
            for i, m in self.mats.slots(r):
                if m is None or i >= len(o.material_slots):
                    continue
                bm = made.get(m.key())
                if bm is None:
                    bm = made[m.key()] = self.blender_material(m)
                    self.post[bm.name] = m
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

    def shapes(self, d):
        out = {}
        for (smr, sh), w in sorted(d.items(), key=lambda x: (str(x[0][0].gname), x[0][1])):
            k = self.shape(smr, sh)
            if k is None:
                warn('the shape key %s of %s is not in the GLB' % (sh, smr.gname))
            elif w > 1e-4:
                out[k] = _r(w, 4)
        return out

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
        for t in self.kept:
            x = {'name': t['name']}
            if t['group']:
                x['group'] = t['group']
            if t['on']:
                x['on'] = True
            show, hide, sh = self.parts(t['show']), self.parts(t['hide']), self.shapes(t['shapes'])
            if show:
                x['show'] = show
                shown |= set(show)
            if hide:
                x['hide'] = hide
            if sh:
                x['shapes'] = sh
            x['_busy'] = bool(show or hide or sh)
            toggles.append(x)
        busy = {x.get('group') for x in toggles if x['_busy']}
        toggles = [x for x in toggles if x.pop('_busy') or (x.get('group') and x['group'] in busy)]
        hidden = [p for p in self.parts([r for r in av.renderers if not av.visible(r, self.base)])
                  if p not in shown]
        return hidden, toggles

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
        planes = inside = 0
        for c in cols:
            d, go = c.data, c.go
            if go is None or id(go) not in av.inside:
                continue
            if 'shapeType' in d:  # VRC PhysBone Collider
                t = d.get('rootTransform')
                T = t.go if isinstance(t, Obj) and t.go is not None and id(t.go) in av.inside else go
                shape = inum(d.get('shapeType'))
                if shape == 2:
                    planes += 1
                    continue
                if truthy(d.get('insideBounds', '0')):
                    inside += 1
                    continue
                r = num(d.get('radius'), 0.5)
                half = max(num(d.get('height'), 1.0) / 2 - r, 0.0) if shape == 1 else 0.0
                axis = Quaternion(quat(d.get('rotation'))) @ Vector((0.0, 1.0, 0.0))
                pos = Vector(vec3(d.get('position')))
                grow = max_scale(U[id(T)]) if id(T) in U else 1.0
            else:  # Dynamic Bone Collider
                T = go
                if inum(d.get('m_Bound')) == 1:
                    inside += 1
                    continue
                r = num(d.get('m_Radius'), 0.5)
                half = max(num(d.get('m_Height')) / 2 - r, 0.0)
                axis = Vector([(1.0, 0.0, 0.0), (0.0, 1.0, 0.0), (0.0, 0.0, 1.0)][min(max(inum(d.get('m_Direction')), 0), 2)])
                pos = Vector(vec3(d.get('m_Center')))
                grow = U[id(T)].col[0].xyz.length if id(T) in U else 1.0
            n = self.near(T)
            if n is None or id(T) not in U:
                continue
            Gi = self.world[n].inverted_safe()
            ends = [pos - axis * half, pos + axis * half] if half > 1e-6 else [pos]
            pts = [Gi @ (FLIP @ (U[id(T)] @ p)) for p in ends]
            x = {'name': unique(str(go.data.get('m_Name', '')), used), 'node': self.nodes[n]['name'],
                 'offset': _v(pts[0])}
            if len(pts) > 1:
                x['tail'] = _v(pts[1])
            x['radius'] = _r(r * grow / max(avg_scale(self.world[n]), 1e-9))
            name_of[id(c)] = x['name']
            out_c.append(x)
        if planes:
            warn('%d plane collider(s) are not converted' % planes)
        if inside:
            warn('%d collider(s) that keep bones inside them are not converted' % inside)
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

    def build(self, info):
        out = {}
        h = self.humanoid()
        if h:
            out['humanoid'] = h
        exprs, gestures = self.expressions()
        if exprs:
            out['expressions'] = exprs
        if gestures:
            out['gestures'] = gestures
        hidden, toggles = self.outfit()
        if hidden:
            out['hidden'] = hidden
        if toggles:
            out['toggles'] = toggles
        cols, springs = self.dynamics()
        if cols:
            out['colliders'] = cols
        if springs:
            out['springs'] = springs
        out['converter'] = info
        return out


# ---------------------------------------------------------------- running it

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
    human = an.humanoid()
    setups = [OutfitSetup(db, av, human, o) for o in outfits]
    b = Build(db, av, opts)
    b.import_models()
    b.compute(human)
    for x in setups:
        x.fix(b.U)
    ma = ModularAvatar(av, dict(human), b.U, [m for x in setups for m in x.specs])
    an.ma = av.ma = ma
    if an.vrcf is not None and an.vrcf.links:
        link_armatures(ma, an.vrcf, human)
    jaw = human.get('Jaw')
    if jaw is not None and id(jaw) in an.chained():  # Unity's guess, which the avatar does not use as a jaw
        warn('the Jaw bone %s swings with a PhysBone, so it is left out of the humanoid map' % jaw.data.get('m_Name'))
        del human['Jaw']
    kept, base = an.toggles()
    b.arrange()
    b.place()
    b.materials()
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
    binc = ma.apply(js, binc)
    info = {'tool': 'unity2hypr3d', 'input': [os.path.basename(p.rstrip('/')) for p in opts.inputs],
            'prefab': found.asset.path, 'avatar': av.name, 'date': time.strftime('%Y-%m-%d %H:%M:%S')}
    st = Settings(b, an, js, human, kept, base, ma).build(info)
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
