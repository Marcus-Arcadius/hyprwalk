# booth.py: a stand-in for a VRChat avatar bought on Booth and outfits sold for it, as .unitypackage files
#   blender -b --factory-startup --python-exit-code 1 -P booth.py -- OUTDIR
#
# OUTDIR/proj: the shop's Unity project the packages are exported from
# OUTDIR/SynthChan_v1.0.unitypackage: the avatar, laid out as Booth avatars are, under Assets/しんせ工房/シンセちゃん/:
#   FBX/ (a humanoid model with Japanese shape keys and vrc.v_* visemes), Materials/ (lilToon materials; lilToon
#   itself is not in the package, as on Booth), Textures/ (PNG and PSD), Animation/ (an FX controller with gesture
#   faces, toggles and a face lock), Expressions/ (menus with Japanese labels) and Prefab/ (a PC prefab, a variant
#   of the model with PhysBones on the hair, ears, tail and skirt, colliders and a head-pat contact; and a Quest
#   prefab, a variant of that with Quest materials and fewer PhysBones)
# OUTDIR/SynthChan_OnePiece_v1.0.unitypackage: a dress for her, set up for Modular Avatar (Merge Armature, a menu of
#   MA toggles, a Shape Changer that shrinks her body under it)
# OUTDIR/Parka_v1.0.unitypackage: a parka with no MA setup, for --outfit; its bones are named Hips_Parka and so on,
#   and it is exported at the 100x bone scale many Blender exports have
# OUTDIR/SynthChan_Cardigan_VRCFury_v1.0.unitypackage: a cardigan set up for VRCFury (Armature Link, toggles, one of
#   them saved by an old VRCFury in Unity 2019, and a Full Controller with its own FX, menu and parameters)
# OUTDIR/Hairpin_v1.0.unitypackage: a hair pin put on the head by an Armature Link from VRCFury 1.x (version 5)
# OUTDIR/シンセちゃん_v1.0.zip: the avatar package as Booth hands it out: a zip with Shift-JIS names and no UTF-8 flag
import sys, os, math, shutil
import numpy as np
import bpy
from mathutils import Matrix, Vector, Quaternion

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', '..'))  # tools/
sys.path.insert(0, HERE)
import unitygen as g
from unitygen import R, C, F, Esc, doc, base, HEAD

argv = sys.argv[sys.argv.index('--') + 1:] if '--' in sys.argv else []
OUT = os.path.abspath(argv[0] if argv else 'booth')
PROJ = os.path.join(OUT, 'proj')
if os.path.isdir(PROJ):
    shutil.rmtree(PROJ)
os.makedirs(os.path.join(PROJ, 'ProjectSettings'))
os.makedirs(os.path.join(PROJ, 'Packages'))
with open(os.path.join(PROJ, 'ProjectSettings/ProjectVersion.txt'), 'w') as f:
    f.write('m_EditorVersion: 2022.3.22f1\nm_EditorVersionWithRevision: 2022.3.22f1 (887be4894c44)\n')
with open(os.path.join(PROJ, 'Packages/manifest.json'), 'w') as f:
    f.write('{"dependencies": {"com.vrchat.avatars": "3.7.0", "jp.lilxyzw.liltoon": "1.8.3"}}\n')

SHOP = 'Assets/しんせ工房'
AV = SHOP + '/シンセちゃん'
ids = g.Ids(24)


def G(key):
    return g.guid_of('booth/' + key)


def P_(rel):
    return os.path.join(PROJ, rel)


# lilToon's shaders, by their GUIDs (from lilToon's own .meta files); the packages do not include them
LIL = {'opaque': 'df12117ecd77c31469c224178886498e', 'cutout': '85d6126cae43b6847aff4b13f4adb8ec',
       'trans': '165365ab7100a044ca85fc8c33548a62'}
QUEST_TOON_LIT = '0b7113dea2069fc4e8943843eff19f70'  # stands in for VRChat/Mobile/Toon Lit (in the SDK, not here)

# ---------------------------------------------------------------- the shape of her (Blender: Z up, facing -Y, her
# left at +X), 1.45 m tall

HC = Vector((0.0, -0.005, 1.275))  # the head's centre, radius and vertical stretch
HR, HSZ = 0.118, 1.06
SIDES = (('L', 1), ('R', -1))
FINGERS = (('Index', -0.021, (0.028, 0.018, 0.016)), ('Middle', -0.007, (0.030, 0.020, 0.017)),
           ('Ring', 0.007, (0.028, 0.018, 0.015)), ('Little', 0.020, (0.022, 0.015, 0.013)))
PHAL = ('Proximal', 'Intermediate', 'Distal')
TWIN = [(0.105, 0.045, 1.34), (0.14, 0.065, 1.24), (0.155, 0.075, 1.12), (0.16, 0.075, 1.0), (0.152, 0.068, 0.88)]
TAIL = [(0, 0.085, 0.72), (0, 0.16, 0.68), (0, 0.23, 0.665), (0, 0.29, 0.70), (0, 0.325, 0.78), (0, 0.335, 0.87)]
HAIRBACK = [(0, 0.1, 1.33), (0, 0.125, 1.22), (0, 0.13, 1.11), (0, 0.125, 1.02)]
EAR = [(0.07, 0.005, 1.37), (0.09, 0.005, 1.43), (0.103, 0.005, 1.475)]
SKIRT = {'F': [(0, -0.1, 0.79), (0, -0.15, 0.67), (0, -0.19, 0.56)],
         'B': [(0, 0.09, 0.79), (0, 0.14, 0.67), (0, 0.18, 0.56)],
         'L': [(0.11, 0, 0.79), (0.165, 0, 0.67), (0.21, 0, 0.56)],
         'R': [(-0.11, 0, 0.79), (-0.165, 0, 0.67), (-0.21, 0, 0.56)]}


def head_shape(co):
    """a point of the round head moved to the head's shape: a narrower chin"""
    t = max(0.0, (HC.z - co.z) / (HR * HSZ))
    k = 1 - 0.30 * t ** 1.5
    return Vector((HC.x + (co.x - HC.x) * k, HC.y + (co.y - HC.y) * (1 - 0.08 * t), co.z))


def face_y(x, z):
    """where the front of the face is at (x, z)"""
    t = max(0.0, (HC.z - z) / (HR * HSZ))
    k = 1 - 0.30 * t ** 1.5
    dz = (z - HC.z) / HSZ
    return HC.y - math.sqrt(max(HR * HR - (x / k) ** 2 - dz * dz, 1e-6)) * (1 - 0.08 * t)


def skeleton(rig, sfx='', own=True):
    """her bones; sfx is added to every name (an outfit's copy); own: the ones outfits leave out too"""
    b = rig.bone
    b('Hips' + sfx, (0, 0, 0.70), (0, 0, 0.80))
    b('Spine' + sfx, (0, 0, 0.80), (0, 0, 0.92), 'Hips' + sfx, True)
    b('Chest' + sfx, (0, 0, 0.92), (0, 0, 1.06), 'Spine' + sfx, True)
    b('Neck' + sfx, (0, 0, 1.08), (0, 0, 1.16), 'Chest' + sfx)
    b('Head' + sfx, (0, 0, 1.16), (0, 0, 1.40), 'Neck' + sfx, True)
    for s, x in SIDES:
        n = lambda k: '%s_%s%s' % (k, s, sfx)
        b(n('Shoulder'), (0.02 * x, 0, 1.045), (0.09 * x, 0, 1.05), 'Chest' + sfx)
        b(n('UpperArm'), (0.09 * x, 0, 1.05), (0.30 * x, 0, 1.05), n('Shoulder'), True)
        b(n('LowerArm'), (0.30 * x, 0, 1.05), (0.50 * x, 0, 1.05), n('UpperArm'), True)
        b(n('Hand'), (0.50 * x, 0, 1.05), (0.56 * x, 0, 1.047), n('LowerArm'), True)
        b(n('UpperLeg'), (0.07 * x, 0, 0.70), (0.07 * x, 0.005, 0.39), 'Hips' + sfx)
        b(n('LowerLeg'), (0.07 * x, 0.005, 0.39), (0.07 * x, 0.015, 0.075), n('UpperLeg'), True)
        b(n('Foot'), (0.07 * x, 0.015, 0.075), (0.07 * x, -0.07, 0.025), n('LowerLeg'), True)
        b(n('Toe'), (0.07 * x, -0.07, 0.025), (0.07 * x, -0.115, 0.02), n('Foot'), True)
        if not own:
            continue
        b(n('Eye'), (0.043 * x, -0.07, 1.262), (0.043 * x, -0.1, 1.262), 'Head' + sfx)
        th = [(0.515 * x, -0.02, 1.04), (0.535 * x, -0.04, 1.035), (0.55 * x, -0.055, 1.032), (0.562 * x, -0.066, 1.03),
              (0.572 * x, -0.074, 1.029)]
        for k in range(3):
            b('Thumb_%s_%s%s' % (PHAL[k], s, sfx), th[k], th[k + 1],
              n('Hand') if k == 0 else 'Thumb_%s_%s%s' % (PHAL[k - 1], s, sfx), k > 0)
        for fn, y, ls in FINGERS:
            p = 0.56
            for k in range(3):
                b('%s_%s_%s%s' % (fn, PHAL[k], s, sfx), (p * x, y, 1.045), ((p + ls[k]) * x, y, 1.044),
                  n('Hand') if k == 0 else '%s_%s_%s%s' % (fn, PHAL[k - 1], s, sfx), k > 0)
                p += ls[k]
        rig.chain(n('Ear'), [(px * x, py, pz) for px, py, pz in EAR], 'Head' + sfx)
        rig.chain(n('TwinTail'), [(px * x, py, pz) for px, py, pz in TWIN], 'Head' + sfx)
    if own:
        rig.chain('HairBack' + sfx, HAIRBACK, 'Head' + sfx)
        rig.chain('Tail' + sfx, TAIL, 'Hips' + sfx)
        b('Skirt_Root' + sfx, (0, 0, 0.8), (0, 0, 0.76), 'Hips' + sfx)
        for k, pts in SKIRT.items():
            rig.chain('Skirt_%s%s' % (k, sfx), pts, 'Skirt_Root' + sfx)


HUMAN = {'Hips': 'Hips', 'Spine': 'Spine', 'Chest': 'Chest', 'Neck': 'Neck', 'Head': 'Head',
         'LeftEye': 'Eye_L', 'RightEye': 'Eye_R'}
for s, side in (('L', 'Left'), ('R', 'Right')):
    HUMAN.update({side + 'Shoulder': 'Shoulder_' + s, side + 'UpperArm': 'UpperArm_' + s,
                  side + 'LowerArm': 'LowerArm_' + s, side + 'Hand': 'Hand_' + s, side + 'UpperLeg': 'UpperLeg_' + s,
                  side + 'LowerLeg': 'LowerLeg_' + s, side + 'Foot': 'Foot_' + s, side + 'Toes': 'Toe_' + s})
    for fn in ('Thumb',) + tuple(f[0] for f in FINGERS):
        for k in range(3):
            HUMAN['%s %s %s' % (side, fn, PHAL[k])] = '%s_%s_%s' % (fn, PHAL[k], s)


# ---------------------------------------------------------------- mesh pieces

def tube(P, name, points, radii, seg=12, mi=0, squash=1.0, caps=(True, True), v_range=(0.0, 1.0)):
    """a tube through the points, radii[k] thick at each, its rings square to the path (u round, v along)"""
    bm, uv = P.bm, P.uv
    pts = [Vector(p) for p in points]
    t0 = (pts[1] - pts[0]).normalized()
    ref = Vector((0, 1, 0)) if abs(t0.y) < 0.9 else Vector((1, 0, 0))
    n = t0.cross(ref).normalized()
    prev, rows = t0, []
    for k, p in enumerate(pts):
        if k == 0:
            t = t0
        elif k == len(pts) - 1:
            t = (pts[k] - pts[k - 1]).normalized()
        else:
            t = ((pts[k] - pts[k - 1]).normalized() + (pts[k + 1] - pts[k]).normalized()).normalized()
        ax = prev.cross(t)
        if ax.length > 1e-8:
            n = Quaternion(ax.normalized(), prev.angle(t)) @ n
        prev = t
        bb = t.cross(n).normalized()
        nn = bb.cross(t).normalized()
        rows.append([bm.verts.new(p + nn * (math.cos(2 * math.pi * i / seg) * radii[k]) +
                                  bb * (math.sin(2 * math.pi * i / seg) * radii[k] * squash)) for i in range(seg)])
    vs = [v_range[0] + (v_range[1] - v_range[0]) * k / (len(pts) - 1) for k in range(len(pts))]
    faces = []
    for k in range(len(rows) - 1):
        for i in range(seg):
            j = (i + 1) % seg
            f = bm.faces.new((rows[k][i], rows[k][j], rows[k + 1][j], rows[k + 1][i]))
            for loop, c in zip(f.loops, ((i / seg, vs[k]), ((i + 1) / seg, vs[k]), ((i + 1) / seg, vs[k + 1]),
                                         (i / seg, vs[k + 1]))):
                loop[uv].uv = c
            faces.append(f)
    for end, row in ((0, rows[0]), (1, rows[-1])):
        if caps[end]:
            f = bm.faces.new(row if end else list(reversed(row)))
            for loop in f.loops:
                loop[uv].uv = (0.5, vs[0] if end == 0 else vs[-1])
            faces.append(f)
    for f in faces:
        f.material_index = mi
    out = [v for row in rows for v in row]
    P.parts.setdefault(name, []).extend(out)
    return out


def blade(P, name, top, tip, width, facing, mi=0, bend=0.0):
    """a flat strand from a top edge `width` wide to a point, bent out along `facing` in the middle"""
    bm, uv = P.bm, P.uv
    top, tip, facing = Vector(top), Vector(tip), Vector(facing).normalized()
    across = (tip - top).cross(facing).normalized() * (width / 2)
    mid = (top + tip) / 2 + facing * bend
    vs = [bm.verts.new(x) for x in (top - across, top + across, mid + across * 0.75, mid - across * 0.75, tip)]
    f1 = bm.faces.new((vs[0], vs[1], vs[2], vs[3]))
    f2 = bm.faces.new((vs[3], vs[2], vs[4]))
    for f, cs in ((f1, ((0, 1), (1, 1), (0.9, 0.5), (0.1, 0.5))), (f2, ((0.1, 0.5), (0.9, 0.5), (0.5, 0)))):
        f.material_index = mi
        for loop, c in zip(f.loops, cs):
            loop[uv].uv = c
    P.parts.setdefault(name, []).extend(vs)
    P.flat.add(name)
    return vs


def decal(P, name, center, size, nx=6, nz=6, mi=0, lift=0.003, shape=None):
    """a grid lying on the face, `size` (width, height) around center (x, z), lift in front of it; UVs 0..1 over
    it. shape(s, t) -> (dx, dz) bends it (s, t in 0..1)"""
    bm, uv = P.bm, P.uv
    cx, cz = center
    w, h = size
    grid = []
    for j in range(nz + 1):
        row = []
        for i in range(nx + 1):
            s, t = i / nx, j / nz
            dx, dz = shape(s, t) if shape else (0.0, 0.0)
            x, z = cx + (s - 0.5) * w + dx, cz + (t - 0.5) * h + dz
            row.append(bm.verts.new((x, face_y(x, z) - lift, z)))
        grid.append(row)
    vs = [v for row in grid for v in row]
    for j in range(nz):
        for i in range(nx):
            f = bm.faces.new((grid[j][i], grid[j][i + 1], grid[j + 1][i + 1], grid[j + 1][i]))
            f.material_index = mi
            for loop, (a, b) in zip(f.loops, ((i, j), (i + 1, j), (i + 1, j + 1), (i, j + 1))):
                loop[uv].uv = (a / nx, b / nz)
            front(f)
    P.parts.setdefault(name, []).extend(vs)
    P.flat.add(name)
    return vs


def front(f):
    """a face lying on the face turned to look out of it (-Y)"""
    f.normal_update()
    if f.normal.y > 0:
        f.normal_flip()


def disc(P, name, center, r, sz=1.0, seg=16, mi=0, lift=0.003):
    """a filled ellipse on the face"""
    bm, uv = P.bm, P.uv
    cx, cz = center
    ring = []
    for i in range(seg):
        a = 2 * math.pi * i / seg
        x, z = cx + r * math.cos(a), cz + r * sz * math.sin(a)
        ring.append(bm.verts.new((x, face_y(x, z) - lift, z)))
    c = bm.verts.new((cx, face_y(cx, cz) - lift - 0.0005, cz))
    for i in range(seg):
        f = bm.faces.new((c, ring[(i + 1) % seg], ring[i]))
        f.material_index = mi
        for loop in f.loops:
            loop[uv].uv = (0.5, 0.5)
        front(f)
    P.parts.setdefault(name, []).extend(ring + [c])
    P.flat.add(name)
    return ring + [c]


# where the palette texture (顔.png) keeps its colours
SWATCH = {'lash': (0.125, 0.875), 'brow': (0.375, 0.875), 'mouth': (0.625, 0.875), 'blush': (0.875, 0.875),
          'inner_ear': (0.125, 0.625), 'white': (0.375, 0.625), 'shoe': (0.625, 0.625), 'frame': (0.875, 0.625),
          'sock': (0.375, 0.625), 'sole': (0.125, 0.375), 'tongue': (0.375, 0.375)}
PALETTE = {'lash': (0.16, 0.09, 0.12), 'brow': (0.36, 0.22, 0.28), 'mouth': (0.45, 0.12, 0.16),
           'blush': (1.0, 0.55, 0.6), 'inner_ear': (1.0, 0.72, 0.76), 'white': (0.97, 0.97, 0.98),
           'shoe': (0.24, 0.14, 0.1), 'frame': (0.1, 0.08, 0.1), 'sole': (0.9, 0.88, 0.85),
           'tongue': (0.85, 0.35, 0.4)}


def swatch(P, part, key):
    P.uv_swatch(part, SWATCH[key])


# ---------------------------------------------------------------- SynthChan in Blender

def mats(*names):
    return [bpy.data.materials.get(n) or bpy.data.materials.new(n) for n in names]


def build_avatar():
    g.clear_scene()
    rig = g.Rig('Armature')
    skeleton(rig)
    rig.done()
    SKIN, FACE, EYE = 0, 1, 2
    P = g.Parts()
    # the body: loft for the torso, tubes for the limbs
    P.add('torso', 'loft', Matrix.Identity(4), mi=SKIN, seg=24, rings=[
        (0.63, 0.085, 0.07), (0.68, 0.11, 0.08), (0.72, 0.118, 0.083), (0.78, 0.105, 0.075), (0.84, 0.093, 0.068),
        (0.90, 0.10, 0.074), (0.96, 0.108, 0.082, -0.004), (1.01, 0.108, 0.077), (1.05, 0.095, 0.065),
        (1.08, 0.06, 0.045), (1.10, 0.036, 0.032)])
    tube(P, 'neck', [(0, 0, 1.07), (0, 0, 1.13), (0, -0.005, 1.19)], [0.034, 0.031, 0.03], seg=16, mi=SKIN)
    head = P.add('head', 'sphere', Matrix.Translation(HC) @ Matrix.Diagonal((1, 1, HSZ, 1)), mi=SKIN, r=HR, us=32, vs=24)
    for v in head:
        v.co = head_shape(v.co)
    for s, x in SIDES:
        tube(P, 'arm_' + s, [(0.07 * x, 0, 1.05), (0.19 * x, 0, 1.05), (0.30 * x, 0, 1.05), (0.40 * x, 0, 1.05),
                             (0.495 * x, 0, 1.05)], [0.036, 0.031, 0.027, 0.024, 0.021], seg=14, mi=SKIN)
        P.add('palm_' + s, 'cube', Matrix.Translation((0.532 * x, -0.001, 1.046)) @
              Matrix.Diagonal((0.065, 0.058, 0.021, 1)), mi=SKIN)
        th = [(0.515 * x, -0.02, 1.04), (0.535 * x, -0.04, 1.035), (0.55 * x, -0.055, 1.032),
              (0.562 * x, -0.066, 1.03), (0.572 * x, -0.074, 1.029)]
        for k in range(3):
            tube(P, 'Thumb_%s_%s' % (PHAL[k], s), [th[k], th[k + 1]], [0.0095, 0.0085], seg=8, mi=SKIN)
        for fn, y, ls in FINGERS:
            p = 0.556
            for k in range(3):
                q = p + ls[k] + (0.002 if k == 2 else 0.0)
                tube(P, '%s_%s_%s' % (fn, PHAL[k], s), [(p * x, y, 1.045), (q * x, y, 1.044)],
                     [0.0075, 0.007 if k < 2 else 0.0055], seg=8, mi=SKIN)
                p += ls[k]
        tube(P, 'leg_' + s, [(0.068 * x, 0, 0.73), (0.07 * x, 0.002, 0.56), (0.07 * x, 0.005, 0.39),
                             (0.07 * x, 0.01, 0.23), (0.07 * x, 0.015, 0.075)], [0.064, 0.055, 0.042, 0.036, 0.028],
             seg=16, mi=SKIN)
    # the face: eyes (their own texture, cut out), lashes, brows, mouth and blush from the palette
    for s, x in SIDES:
        decal(P, 'eye_' + s, (0.043 * x, 1.262), (0.05, 0.058), mi=EYE, lift=0.0025)
    for s, x in SIDES:
        ex = 0.043 * x

        def lash(sp, t, ex=ex, x=x):
            xx = ex + (sp - 0.5) * 0.062 * x
            th = 0.0035 + 0.0045 * sp
            zz = 1.262 + 0.0285 + 0.005 * math.sin(math.pi * sp) - 0.005 * sp + (t - 0.5) * th
            return xx, zz
        strip(P, 'lash_' + s, lash, mi=FACE, lift=0.0035)

        def brow(sp, t, x=x):
            xx = (0.02 + 0.05 * sp) * x
            zz = 1.318 + 0.004 * math.sin(math.pi * sp) - 0.002 * sp + (t - 0.5) * 0.0045
            return xx, zz
        strip(P, 'brow_' + s, brow, mi=FACE, lift=0.004)
        disc(P, 'blush_' + s, (0.062 * x, 1.222), 0.013, sz=0.5, mi=FACE, lift=0.002)
    disc(P, 'mouth', (0, 1.188), 0.0095, sz=0.38, mi=FACE, lift=0.0025)
    for part, key in (('lash_L', 'lash'), ('lash_R', 'lash'), ('brow_L', 'brow'), ('brow_R', 'brow'),
                      ('blush_L', 'blush'), ('blush_R', 'blush'), ('mouth', 'mouth')):
        swatch(P, part, key)
    body, idx = P.make('Body', mats('肌', '顔', '目'))
    bones = {'torso': ['Hips', 'Spine', 'Chest', 'UpperLeg_L', 'UpperLeg_R'], 'neck': ['Neck', 'Chest', 'Head'],
             'head': ['Head', 'Neck'], 'mouth': ['Head'], 'eye_L': ['Eye_L'], 'eye_R': ['Eye_R']}
    for s, x in SIDES:
        bones['arm_' + s] = ['Shoulder_' + s, 'UpperArm_' + s, 'LowerArm_' + s, 'Hand_' + s, 'Chest']
        bones['palm_' + s] = ['Hand_' + s]
        bones['leg_' + s] = ['Hips', 'UpperLeg_' + s, 'LowerLeg_' + s, 'Foot_' + s]
        for p in ('lash_', 'brow_', 'blush_'):
            bones[p + s] = ['Head']
        for fn in ('Thumb',) + tuple(f[0] for f in FINGERS):
            for k in range(3):
                bones['%s_%s_%s' % (fn, PHAL[k], s)] = ['%s_%s_%s' % (fn, PHAL[k], s)]
    rig.skin(body, (idx, bones), blend=0.035)
    face_keys(body, idx)

    # hair: a cap, bangs, side locks, the back and two twin tails; cat ears and a tail
    P = g.Parts()
    cap = P.add('cap', 'sphere', Matrix.Translation(HC + Vector((0, 0.006, 0.006))) @
                Matrix.Diagonal((1.075, 1.07, HSZ * 1.06, 1)), r=HR, us=32, vs=24)
    for v in cap:
        v.co = head_shape(v.co)
    P.delete('cap', lambda c: (c.y < -0.03 and c.z < 1.35) or c.z < 1.2 or (c.y < 0.03 and c.z < 1.24))
    for k in range(9):
        a = (k - 4) / 4.0  # -1 .. 1 across the forehead
        x0 = 0.085 * a
        top_z = 1.39 - 0.02 * abs(a)
        tip_z = 1.305 + 0.018 * abs(a) + (0.012 if k % 2 else 0.0)
        top = (x0, face_y(x0, top_z) - 0.02, top_z)
        tip = (x0 * 1.12, face_y(x0 * 1.1, tip_z) - 0.012, tip_z)
        blade(P, 'bangs', top, tip, 0.034, (0.25 * a, -1, 0.1), bend=0.004)
    for s, x in SIDES:
        blade(P, 'side_' + s, (0.1 * x, -0.05, 1.34), (0.105 * x, -0.07, 1.16), 0.032, (x, -0.6, 0), bend=0.004)
        tube(P, 'twin_' + s, [(px * x, py, pz) for px, py, pz in TWIN] + [(0.14 * x, 0.06, 0.8)],
             [0.03, 0.042, 0.04, 0.034, 0.024, 0.006], seg=12, squash=0.8)
    P.add('back', 'loft', Matrix.Identity(4), seg=24, caps=(False, False), rings=[
        (1.02, 0.1, 0.1, 0.035), (1.12, 0.115, 0.115, 0.03), (1.22, 0.125, 0.12, 0.02), (1.31, 0.128, 0.126, 0.01)])
    P.delete('back', lambda c: c.y < 0.0)
    hair, hidx = P.make('髪', mats('髪'))
    hb = {'cap': ['Head'], 'bangs': ['Head'], 'back': ['Head'] + ['HairBack', 'HairBack.001', 'HairBack.002']}
    for s, x in SIDES:
        hb['side_' + s] = ['Head']
        hb['twin_' + s] = ['Head'] + ['TwinTail_%s%s' % (s, '' if k == 0 else '.%03d' % k) for k in range(4)]
    rig.skin(hair, (hidx, hb), blend=0.03)

    P = g.Parts()
    for s, x in SIDES:
        M, d = g.along((0.066 * x, 0.004, 1.355), (0.108 * x, 0.004, 1.485), 1.0, 0.42)
        P.add('ear_' + s, 'cyl', M, mi=0, r1=0.036, r2=0.004, depth=d, seg=4)
        M, d = g.along((0.068 * x, -0.008, 1.37), (0.1 * x, -0.008, 1.465), 1.0, 0.25)
        P.add('inner_' + s, 'cyl', M, mi=1, r1=0.024, r2=0.003, depth=d, seg=4)
        swatch(P, 'inner_' + s, 'inner_ear')
    ears, eidx = P.make('ネコミミ', mats('髪', '顔'))
    rig.skin(ears, ['Ear_L', 'Ear_L.001', 'Ear_R', 'Ear_R.001'], blend=0.01)

    P = g.Parts()
    tube(P, 'tail', TAIL + [(0, 0.33, 0.93)], [0.026, 0.028, 0.026, 0.024, 0.021, 0.016, 0.004], seg=12)
    tail, _ = P.make('しっぽ', mats('髪'))
    rig.skin(tail, ['Hips', 'Tail', 'Tail.001', 'Tail.002', 'Tail.003', 'Tail.004'], blend=0.04)

    # clothes: a sailor top with a collar and a bow, and a pleated skirt; knee socks and shoes
    P = g.Parts()
    P.add('top', 'loft', Matrix.Identity(4), seg=24, caps=(False, False), rings=[
        (0.79, 0.108, 0.084), (0.84, 0.106, 0.081), (0.90, 0.113, 0.086), (0.96, 0.121, 0.095, -0.004),
        (1.01, 0.121, 0.09), (1.05, 0.109, 0.079), (1.075, 0.077, 0.059), (1.092, 0.05, 0.045)],
        uv_v=[0.52, 0.58, 0.66, 0.74, 0.82, 0.88, 0.94, 0.98])
    for s, x in SIDES:
        tube(P, 'sleeve_' + s, [(0.06 * x, 0, 1.05), (0.13 * x, 0, 1.052), (0.2 * x, 0, 1.05)],
             [0.052, 0.047, 0.041], seg=14, caps=(False, False), v_range=(0.6, 0.7))
    P.add('collar', 'cyl', Matrix.Translation((0, 0.012, 1.072)), r1=0.118, r2=0.058, depth=0.03, seg=24, caps=False)
    P.uv_swatch('collar', (0.5, 0.97))
    for x in (1, -1):
        M = Matrix.Translation((0.022 * x, -0.101, 1.018)) @ Matrix.Rotation(math.radians(20 * x), 4, 'Y')
        P.add('bow', 'sphere', M @ Matrix.Diagonal((1.6, 0.5, 1.0, 1)), r=0.016, us=10, vs=8)
    P.add('bow', 'sphere', Matrix.Translation((0, -0.103, 1.018)), r=0.009, us=8, vs=6)
    P.uv_swatch('bow', (0.5, 0.505))
    pleats = []
    for z, rx, ry, pl in ((0.82, 0.104, 0.082, 0.0), (0.76, 0.14, 0.115, 0.3), (0.68, 0.19, 0.165, 0.7),
                          (0.575, 0.24, 0.215, 1.0)):
        pleats.append((z, rx, ry, pl))
    skirt_loft(P, 'skirt', pleats, seg=32)
    clothes, cidx = P.make('服', mats('服'))
    cb = {'top': ['Hips', 'Spine', 'Chest', 'Neck'], 'collar': ['Chest', 'Neck'], 'bow': ['Chest'],
          'skirt': ['Hips', 'Skirt_Root'] + ['Skirt_%s%s' % (k, '' if i == 0 else '.001') for k in 'FBLR' for i in (0, 1)]}
    for s, x in SIDES:
        cb['sleeve_' + s] = ['Shoulder_' + s, 'UpperArm_' + s, 'Chest']
    rig.skin(clothes, (cidx, cb), blend=0.045)

    P = g.Parts()
    for s, x in SIDES:
        tube(P, 'sock_' + s, [(0.07 * x, 0.012, 0.09), (0.07 * x, 0.01, 0.23), (0.07 * x, 0.006, 0.345)],
             [0.031, 0.039, 0.044], seg=16, caps=(False, False))
        swatch(P, 'sock_' + s, 'white')
        P.add('shoe_' + s, 'sphere', Matrix.Translation((0.07 * x, -0.028, 0.038)) @
              Matrix.Diagonal((0.8, 1.9, 0.72, 1)), r=0.05, us=16, vs=10)
        swatch(P, 'shoe_' + s, 'shoe')
        P.add('sole_' + s, 'cube', Matrix.Translation((0.07 * x, -0.028, 0.006)) @
              Matrix.Diagonal((0.08, 0.19, 0.012, 1)))
        swatch(P, 'sole_' + s, 'sole')
    shoes, sidx = P.make('靴', mats('顔'))
    sb = {}
    for s, x in SIDES:
        sb['sock_' + s] = ['LowerLeg_' + s, 'Foot_' + s]
        sb['shoe_' + s] = sb['sole_' + s] = ['Foot_' + s, 'Toe_' + s]
    rig.skin(shoes, (sidx, sb), blend=0.02)

    # glasses (off at first) and ribbons on the twin tails
    P = g.Parts()
    for s, x in SIDES:
        ex = 0.043 * x
        for (a, b) in (((ex - 0.03, 1.292), (ex + 0.03, 1.292)), ((ex - 0.03, 1.234), (ex + 0.03, 1.234)),
                       ((ex - 0.031, 1.232), (ex - 0.031, 1.294)), ((ex + 0.031, 1.232), (ex + 0.031, 1.294))):
            ya, yb = face_y(a[0], a[1]) - 0.016, face_y(b[0], b[1]) - 0.016
            M, d = g.along((a[0], ya, a[1]), (b[0], yb, b[1]))
            P.add('frame', 'cyl', M, mi=0, r1=0.0022, depth=d + 0.004, seg=6)
        decal(P, 'lens_' + s, (ex, 1.263), (0.058, 0.056), nx=3, nz=3, mi=1, lift=0.016)
    M, d = g.along((0.012, face_y(0.012, 1.268) - 0.017, 1.268), (-0.012, face_y(-0.012, 1.268) - 0.017, 1.268))
    P.add('frame', 'cyl', M, mi=0, r1=0.002, depth=d, seg=6)
    for x in (1, -1):
        M, d = g.along((0.074 * x, face_y(0.072, 1.27) - 0.014, 1.27), (0.12 * x, 0.02, 1.285))
        P.add('frame', 'cyl', M, mi=0, r1=0.002, depth=d, seg=6)
    swatch(P, 'frame', 'frame')
    glasses, _ = P.make('メガネ', mats('顔', 'メガネ'))
    rig.skin(glasses, ['Head'])

    P = g.Parts()
    for s, x in SIDES:
        c = Vector(TWIN[0]) * 1
        c.x *= x
        c += Vector((0.012 * x, 0.004, -0.02))
        P.add('knot_' + s, 'sphere', Matrix.Translation(c), r=0.018, us=10, vs=8)
        for side in (1, -1):
            M = Matrix.Translation(c + Vector((0, 0.022 * side, 0.012))) @ Matrix.Rotation(
                math.radians(25 * side), 4, 'X')
            P.add('loop_' + s, 'sphere', M @ Matrix.Diagonal((0.45, 1.5, 0.9, 1)), r=0.022, us=12, vs=8)
    ribbons, ridx = P.make('リボン', mats('リボン'))
    rig.skin(ribbons, (ridx, {'knot_L': ['TwinTail_L'], 'loop_L': ['TwinTail_L'],
                              'knot_R': ['TwinTail_R'], 'loop_R': ['TwinTail_R']}))
    return rig


def strip(P, name, fn, mi=0, lift=0.003, n=10):
    """a ribbon lying on the face: fn(s, t) -> (x, z) for s along it and t across, both 0..1"""
    bm, uv = P.bm, P.uv
    rows = []
    for t in (0.0, 1.0):
        row = []
        for i in range(n + 1):
            x, z = fn(i / n, t)
            row.append(bm.verts.new((x, face_y(x, z) - lift, z)))
        rows.append(row)
    for i in range(n):
        f = bm.faces.new((rows[0][i], rows[0][i + 1], rows[1][i + 1], rows[1][i]))
        f.material_index = mi
        for loop in f.loops:
            loop[uv].uv = (0.5, 0.5)
        front(f)
    P.parts.setdefault(name, []).extend(rows[0] + rows[1])
    P.flat.add(name)


def skirt_loft(P, name, rings, seg=32):
    """a pleated skirt: rings (z, rx, ry, pleat depth 0..1), the pleats a zigzag of the radius"""
    bm, uv = P.bm, P.uv
    rows = []
    for z, rx, ry, pl in rings:
        row = []
        for i in range(seg):
            a = 2 * math.pi * i / seg
            k = 1 + 0.045 * pl * (1 if i % 2 else -1)
            row.append(bm.verts.new((rx * k * math.sin(a), -ry * k * math.cos(a), z)))
        rows.append(row)
    for j in range(len(rows) - 1):
        for i in range(seg):
            k = (i + 1) % seg
            f = bm.faces.new((rows[j][i], rows[j][k], rows[j + 1][k], rows[j + 1][i]))
            v0, v1 = 0.48 - 0.46 * j / (len(rows) - 1), 0.48 - 0.46 * (j + 1) / (len(rows) - 1)
            for loop, c in zip(f.loops, ((i / seg, v0), ((i + 1) / seg, v0), ((i + 1) / seg, v1), (i / seg, v1))):
                loop[uv].uv = c
    P.parts.setdefault(name, []).extend(v for row in rows for v in row)


VISEMES = ['sil', 'pp', 'ff', 'th', 'dd', 'kk', 'ch', 'ss', 'nn', 'rr', 'aa', 'ee', 'ih', 'oh', 'ou']
MOUTH = {'sil': (0.97, 1.0), 'pp': (0.9, 0.45), 'ff': (1.0, 0.6), 'th': (1.05, 1.3), 'dd': (1.1, 1.6),
         'kk': (1.15, 1.8), 'ch': (0.9, 1.6), 'ss': (1.25, 1.1), 'nn': (1.0, 1.3), 'rr': (0.85, 1.8),
         'aa': (1.35, 3.6), 'ee': (1.7, 1.6), 'ih': (1.4, 2.4), 'oh': (1.05, 3.0), 'ou': (0.7, 2.4)}
FACES = ['まばたき', 'ウィンク', '笑い', 'にっこり', '怒り', '困り', 'びっくり', 'ジト目', '照れ']


def face_keys(body, idx):
    """the visemes, the faces (Japanese names, as Booth avatars have them) and a shrink key for outfits"""
    V_ = {k: set(v) for k, v in idx.items()}
    verts = body.data.vertices
    MZ, EZ = 1.188, 1.262  # the mouth's and the eyes' heights
    LASH = EZ + 0.029
    closed = EZ - 0.012  # where the eyes close to

    def mouth(sx, sz, lift=0.0):
        return lambda c: Vector((c.x * sx, c.y, MZ + (c.z - MZ) * sz + lift))

    for v in VISEMES:
        g.shape_key(body, 'vrc.v_' + v, mouth(*MOUTH[v]), V_['mouth'])

    part_of = {}
    for s in 'LR':
        for p in ('eye', 'lash', 'brow'):
            for i in V_['%s_%s' % (p, s)]:
                part_of[i] = (p, s)

    def key(name, move=None, sides='LR', mouth_fn=None, extra=None):
        """move(co, part, the eye's x) for the eyes, lashes and brows of `sides`; mouth_fn(co); extra: (vertices, fn)"""
        new = {}
        if move:
            for i, (p, s) in part_of.items():
                if s in sides:
                    new[i] = move(verts[i].co.copy(), p, 0.043 if s == 'L' else -0.043)
        for vs, fn in ([(V_['mouth'], mouth_fn)] if mouth_fn else []) + ([extra] if extra else []):
            for i in vs:
                new[i] = fn(verts[i].co.copy())
        g.shape_key_to(body, name, new)

    def blink(co, part, ex):
        if part == 'eye':
            return Vector((co.x, co.y, closed + (co.z - closed) * 0.04))
        if part == 'lash':
            return co + Vector((0, 0, closed - LASH))
        return co + Vector((0, 0, -0.004))

    def smile_eyes(co, part, ex):  # ^ ^
        if part == 'eye':
            return Vector((co.x, co.y, EZ - 0.004 + (co.z - EZ) * 0.04))
        if part == 'lash':
            s = (co.x - ex) / (0.062 if ex > 0 else -0.062) + 0.5
            return Vector((co.x, co.y, EZ - 0.006 + 0.016 * (1 - abs(2 * s - 1)) + (co.z - LASH) * 0.6))
        return co + Vector((0, 0, 0.004))

    def happy(co, part, ex):
        if part == 'eye':
            return Vector((co.x, co.y, EZ + (co.z - EZ) * 0.85))
        return co + Vector((0, 0, 0.003 if part == 'brow' else -0.004))

    def brows(inner_dz, outer_dz, eye_sz=1.0):
        def mv(co, part, ex):
            if part == 'brow':
                s = (abs(co.x) - 0.02) / 0.05  # 0 at the inner end
                return co + Vector((0, 0, inner_dz * (1 - s) + outer_dz * s))
            if part == 'eye':
                return Vector((co.x, co.y, EZ + (co.z - EZ) * eye_sz))
            return co + Vector((0, 0, (LASH - EZ) * (eye_sz - 1)))
        return mv

    def surprised(co, part, ex):
        if part == 'eye':
            return Vector((ex + (co.x - ex) * 1.12, co.y, EZ + (co.z - EZ) * 1.15))
        return co + Vector((0, 0, 0.009 if part == 'brow' else 0.004))

    def half(co, part, ex):
        if part == 'eye':
            return Vector((co.x, co.y, min(co.z, EZ + 0.004)))
        return co + Vector((0, 0, -0.025 if part == 'lash' else -0.003))

    def bigger_blush(co):
        cx = 0.062 if co.x > 0 else -0.062
        return Vector((cx + (co.x - cx) * 1.5, co.y, 1.222 + (co.z - 1.222) * 1.5))

    key('まばたき', blink)
    key('ウィンク', blink, 'L')
    key('笑い', smile_eyes, mouth_fn=mouth(1.5, 1.6, 0.001))
    key('にっこり', happy, mouth_fn=mouth(1.4, 2.8, -0.001))
    key('怒り', brows(-0.009, 0.003, 0.8))
    key('困り', brows(0.008, -0.004))
    key('びっくり', surprised, mouth_fn=mouth(0.8, 2.6))
    key('ジト目', half)
    key('照れ', extra=(V_['blush_L'] | V_['blush_R'], bigger_blush))
    g.shape_key(body, 'Body_Shrink', lambda c: Vector((c.x * 0.93, c.y * 0.93, c.z)) if c.z < 1.06 else None,
                V_['torso'])
    g.shape_key(body, 'Chest_Big', chest_big, V_['torso'])


def chest_big(c, out=0.018):
    """a fuller chest: the front of the torso around 0.97 m pushed forward"""
    if c.y >= 0 or not 0.88 < c.z < 1.06:
        return None
    k = math.cos((c.z - 0.97) / 0.09 * math.pi / 2) * min(1.0, -c.y / 0.06)
    return c + Vector((0, -out * max(k, 0.0), 0.004 * max(k, 0.0)))




# ---------------------------------------------------------------- textures (numpy, rows bottom up)

def grid(w, h):
    yy, xx = np.mgrid[0:h, 0:w]
    return (xx + 0.5) / w, (yy + 0.5) / h


def ellipse(x, y, cx, cy, rx, ry):
    return ((x - cx) / rx) ** 2 + ((y - cy) / ry) ** 2


def tex_skin():
    x, y = grid(64, 64)
    a = np.ones((64, 64, 4), np.float32)
    k = 0.96 + 0.04 * y
    a[..., 0], a[..., 1], a[..., 2] = 1.0 * k, 0.87 * k, 0.8 * k
    return a


def tex_palette():
    a = np.ones((64, 64, 4), np.float32)
    for key, (su, sv) in SWATCH.items():
        c, r = int(su * 4), int(sv * 4)
        a[r * 16:(r + 1) * 16, c * 16:(c + 1) * 16, :3] = PALETTE.get(key, PALETTE['white'])
    return a


def tex_eye():
    x, y = grid(128, 128)
    a = np.zeros((128, 128, 4), np.float32)
    sclera = ellipse(x, y, 0.5, 0.46, 0.47, 0.45) <= 1
    a[sclera] = (0.98, 0.97, 0.99, 1.0)
    shade = sclera & (y > 0.78)  # the lid's shadow on the white
    a[shade, :3] *= 0.82
    iris = ellipse(x, y, 0.5, 0.44, 0.3, 0.38) <= 1
    t = np.clip((y - 0.06) / 0.76, 0, 1)
    top, bottom = np.array((0.28, 0.16, 0.45)), np.array((0.55, 0.78, 0.9))
    col = bottom[None, None, :] * (1 - t[..., None]) + top[None, None, :] * t[..., None]
    a[iris, :3] = col[iris]
    pupil = ellipse(x, y, 0.5, 0.47, 0.12, 0.18) <= 1
    a[pupil, :3] = (0.12, 0.06, 0.2)
    for cx, cy, r in ((0.37, 0.64, 0.085), (0.62, 0.3, 0.045)):
        a[ellipse(x, y, cx, cy, r, r) <= 1, :3] = 1.0
    return a


def tex_hair():
    x, y = grid(256, 256)
    a = np.ones((256, 256, 4), np.float32)
    streak = 0.06 * np.sin(x * 2 * math.pi * 23) + 0.03 * np.sin(x * 2 * math.pi * 61 + 1.3)
    ring = 0.12 * np.exp(-((y - 0.78) / 0.03) ** 2)  # the shine band
    v = 0.9 + 0.08 * y + streak + ring
    a[..., 0], a[..., 1], a[..., 2] = 0.98 * v, 0.74 * v, 0.85 * v
    return np.clip(a, 0, 1)


def tex_clothes():
    x, y = grid(256, 256)
    a = np.ones((256, 256, 4), np.float32)
    navy, red, white = (0.13, 0.15, 0.33), (0.85, 0.12, 0.2), (0.97, 0.97, 0.98)
    a[..., :3] = white
    a[y > 0.96, :3] = navy
    a[(y > 0.975) & (y < 0.982), :3] = white
    a[(y > 0.52) & (y < 0.545), :3] = navy
    a[(y > 0.685) & (y < 0.7), :3] = navy
    a[(y >= 0.5) & (y <= 0.52), :3] = red
    skirt = y < 0.5
    a[skirt, :3] = navy
    pleat = (np.floor(x * 32) % 2 == 0) & skirt
    a[pleat, :3] *= 0.85
    a[(y > 0.04) & (y < 0.058), :3] = white
    return a


def tex_dress():
    x, y = grid(256, 256)
    a = np.ones((256, 256, 4), np.float32)
    a[..., :3] = (0.62, 0.8, 0.95)
    dots = ellipse((x * 16) % 1, (y * 16) % 1, 0.5, 0.5, 0.18, 0.18) <= 1
    a[dots, :3] = (0.92, 0.96, 1.0)
    a[y < 0.05, :3] = (0.98, 0.98, 1.0)  # the lace hem
    a[(y < 0.05) & (np.sin(x * 2 * math.pi * 40) > 0.3), :3] = (0.85, 0.9, 0.97)
    return a


def tex_parka():
    x, y = grid(128, 128)
    rng = np.random.default_rng(3)
    a = np.ones((128, 128, 4), np.float32)
    v = 0.56 + 0.05 * rng.random((128, 128))
    a[..., 0], a[..., 1], a[..., 2] = v, v, v * 1.03
    return np.clip(a, 0, 1)


# ---------------------------------------------------------------- materials

def lil(path, guid, name, mode='opaque', tex=None, color=(1, 1, 1, 1), cull=2, emission=None):
    """a lilToon material: mode opaque, cutout or trans (lilToon's own shaders by GUID; not in the package)"""
    tm = {'opaque': 0, 'cutout': 1, 'trans': 2}[mode]
    envs = [{k: {'m_Texture': R(2800000, t, 3) if t else R(0), 'm_Scale': F(x=1, y=1), 'm_Offset': F(x=0, y=0)}}
            for k, t in (('_MainTex', tex), ('_EmissionMap', None), ('_ShadowColorTex', None), ('_BumpMap', None),
                         ('_OutlineTex', None), ('_MatCapTex', None))]
    floats = [('_lilToonVersion', 43), ('_TransparentMode', tm), ('_Cull', cull), ('_Cutoff', 0.5),
              ('_UseEmission', 1 if emission else 0), ('_UseShadow', 1), ('_ShadowBorder', 0.5),
              ('_ShadowBlur', 0.1), ('_ShadowStrength', 1), ('_AsUnlit', 0), ('_ZWrite', 0 if mode == 'trans' else 1),
              ('_SrcBlend', 5 if mode == 'trans' else 1), ('_DstBlend', 10 if mode == 'trans' else 0),
              ('_AlphaMaskMode', 0), ('_OutlineWidth', 0.05), ('_UseOutline', 0)]
    colors = [('_Color', color), ('_EmissionColor', tuple(emission or (1, 1, 1)) + (1,)),
              ('_ShadowColor', (0.82, 0.76, 0.85, 1)), ('_OutlineColor', (0.6, 0.56, 0.73, 1)),
              ('_MainTexHSVG', (0, 1, 1, 1))]
    rq, tags = {'opaque': (-1, {}), 'cutout': (2450, {'RenderType': 'TransparentCutout'}),
                'trans': (3000, {'RenderType': 'Transparent'})}[mode]
    body = base(serializedVersion=8, m_Name=name, m_Shader=R(4800000, LIL[mode], 3), m_Parent=R(0),
                m_ModifiedSerializedProperties=0, m_ValidKeywords=[], m_InvalidKeywords=[], m_LightmapFlags=4,
                m_EnableInstancingVariants=0, m_DoubleSidedGI=0, m_CustomRenderQueue=rq, stringTagMap=tags,
                disabledShaderPasses=[], m_LockedProperties='',
                m_SavedProperties={'serializedVersion': 3, 'm_TexEnvs': envs, 'm_Ints': [],
                                   'm_Floats': [{k: v} for k, v in floats],
                                   'm_Colors': [{k: C(*v)} for k, v in colors]},
                m_BuildTextureStacks=[])
    g.write_asset(path, HEAD + doc(21, 2100000, 'Material', body), guid, 'NativeFormatImporter', g.native(2100000))
    return guid


def toon_lit(path, guid, name, tex=None):
    """a Quest material: VRChat/Mobile/Toon Lit, a shader of the SDK's"""
    envs = [{'_MainTex': {'m_Texture': R(2800000, tex, 3) if tex else R(0), 'm_Scale': F(x=1, y=1),
                          'm_Offset': F(x=0, y=0)}}]
    body = base(serializedVersion=8, m_Name=name, m_Shader=R(4800000, QUEST_TOON_LIT, 3), m_Parent=R(0),
                m_ModifiedSerializedProperties=0, m_ValidKeywords=[], m_InvalidKeywords=[], m_LightmapFlags=4,
                m_EnableInstancingVariants=0, m_DoubleSidedGI=0, m_CustomRenderQueue=-1, stringTagMap={},
                disabledShaderPasses=[], m_LockedProperties='',
                m_SavedProperties={'serializedVersion': 3, 'm_TexEnvs': envs, 'm_Ints': [], 'm_Floats': [],
                                   'm_Colors': []}, m_BuildTextureStacks=[])
    g.write_asset(path, HEAD + doc(21, 2100000, 'Material', body), guid, 'NativeFormatImporter', g.native(2100000))


# ---------------------------------------------------------------- Unity space

def unity_point(p):
    """a Blender world point (metres) as Unity has it: x mirrored, Y up, Z forward"""
    return Vector((-p[0], p[2], -p[1]))


def local(model, bone, p):
    """a Blender world point in the model's node's own space, as Unity has it (in the node's units)"""
    Wm = model.world[model.node(bone)]
    q = Wm.inverted() @ unity_point(p).to_4d()
    return (round(q.x, 6), round(q.y, 6), round(q.z, 6))


def local_len(model, bone, d):
    return d / model.lossy(bone)


# ---------------------------------------------------------------- the avatar's assets

def avatar_assets():
    rig = build_avatar()
    fbx_rel = AV + '/FBX/シンセちゃん.fbx'
    FBX_GUID = G('avatar.fbx')
    g.export_fbx(P_(fbx_rel), 'FBX_SCALE_ALL')

    T = AV + '/Textures/'
    tex = {'肌': g.write_texture(P_(T + '肌.png'), tex_skin(), G('tex/skin')),
           '顔': g.write_texture(P_(T + '顔.png'), tex_palette(), G('tex/palette'), default=256, alpha=0),
           '目': g.write_texture(P_(T + '目.png'), tex_eye(), G('tex/eye'), default=512),
           '髪': g.write_texture(P_(T + '髪.psd'), tex_hair(), G('tex/hair'), default=1024, alpha=0),
           '服': g.write_texture(P_(T + '服.png'), tex_clothes(), G('tex/clothes'), default=1024, alpha=0)}
    M = AV + '/Materials/'
    mats = {'肌': lil(P_(M + '肌.mat'), G('mat/skin'), '肌', tex=tex['肌']),
            '顔': lil(P_(M + '顔.mat'), G('mat/face'), '顔', tex=tex['顔']),
            '目': lil(P_(M + '目.mat'), G('mat/eye'), '目', 'cutout', tex=tex['目'], emission=(0.08, 0.06, 0.1)),
            '髪': lil(P_(M + '髪.mat'), G('mat/hair'), '髪', tex=tex['髪'], cull=0),
            '服': lil(P_(M + '服.mat'), G('mat/clothes'), '服', tex=tex['服'], cull=0),
            'メガネ': lil(P_(M + 'メガネ.mat'), G('mat/glasses'), 'メガネ', 'trans', color=(0.75, 0.9, 1.0, 0.25)),
            'リボン': lil(P_(M + 'リボン.mat'), G('mat/ribbon'), 'リボン', color=(0.9, 0.15, 0.25, 1),
                       emission=(0.2, 0.01, 0.04))}
    quest = {}
    for n, gd in mats.items():
        quest[n] = G('mat/quest/' + n)
        toon_lit(P_(M + 'Quest/%s_Quest.mat' % n), quest[n], n + '_Quest', tex.get(n))

    model = g.Model(P_(fbx_rel), fbx_rel, FBX_GUID)
    imp = g.model_importer(mats, HUMAN)
    g.write_meta(P_(fbx_rel), FBX_GUID, 'ModelImporter', imp)
    model.fileids(imp)
    print('booth: %s: %d nodes, bone scale %g, %d shape keys on Body' % (
        fbx_rel, len(model.info.order), model.lossy('Head'), len(model.shapes('Body'))))

    # animations: gesture faces, toggles, a face lock and a head-pat smile
    A = AV + '/Animation/'
    clips = {}

    def clip(name, curves):
        clips[name] = G('anim/' + name)
        g.write_asset(P_(A + name + '.anim'), g.clip_yaml(name, curves), clips[name], 'NativeFormatImporter',
                      g.native(7400000))
    for f in FACES:
        clip(f, [('Body', 137, 'blendShape.' + f, 100)])
    TOGGLES = [('ネコミミ', 'Ears', 1), ('しっぽ', 'Tail', 1), ('メガネ', 'Glasses', 0), ('リボン', 'Ribbon', 1)]
    for obj, param, _ in TOGGLES:
        clip(obj + '_ON', [(obj, 1, 'm_IsActive', 1)])
        clip(obj + '_OFF', [(obj, 1, 'm_IsActive', 0)])
    fx = g.Controller(ids, clips)
    for n, t, d in (('GestureLeft', 3, 0), ('GestureRight', 3, 0), ('GestureLeftWeight', 1, 0),
                    ('GestureRightWeight', 1, 0), ('Ears', 4, 1), ('Tail', 4, 1), ('Glasses', 4, 0), ('Ribbon', 4, 1),
                    ('FaceLock', 3, 0), ('HeadPat', 4, 0), ('HairHue', 1, 0)):
        fx.param(n, t, d)
    empty, _ = fx.state('Empty')
    fx.layer('Base', fx.machine('Base', [empty], empty), 0.0)
    fx.hand('Left', {'Fist': ('怒り', True), 'Open': ('びっくり', False), 'Point': ('ジト目', False),
                     'Victory': ('にっこり', False), 'RockNRoll': ('ウィンク', False), 'HandGun': ('困り', False),
                     'ThumbsUp': ('笑い', False)})
    fx.hand('Right', {'Fist': ('怒り', True), 'Open': ('びっくり', False), 'Point': ('ジト目', False),
                      'Victory': ('にっこり', False), 'RockNRoll': ('照れ', False), 'HandGun': ('困り', False),
                      'ThumbsUp': ('笑い', False)}, 1)
    for obj, param, _ in TOGGLES:
        fx.toggle(obj, param, obj + '_ON', obj + '_OFF')
    fx.choice('表情固定', 'FaceLock', [None, 'にっこり', 'ジト目', '照れ'])
    fx.toggle('なでなで', 'HeadPat', '笑い', None)
    FX_GUID = G('fx')
    g.write_asset(P_(A + 'FX_シンセちゃん.controller'), fx.yaml('FX_シンセちゃん'), FX_GUID, 'NativeFormatImporter',
                  g.native(9100000))

    E = AV + '/Expressions/'
    SUB, MENU, PARAMS = G('menu/faces'), G('menu/main'), G('params')
    g.write_asset(P_(E + '表情メニュー.asset'), g.menu_asset('表情メニュー', [
        g.control('にっこり', 102, 'FaceLock', 1), g.control('ジト目', 102, 'FaceLock', 2),
        g.control('照れ', 102, 'FaceLock', 3)]), SUB, 'NativeFormatImporter', g.native(11400000))
    g.write_asset(P_(E + 'ExMenu.asset'), g.menu_asset('ExMenu', [
        g.control('ネコミミ', 102, 'Ears'), g.control('しっぽ', 102, 'Tail'), g.control('メガネ', 102, 'Glasses'),
        g.control(Esc('🎀リボン'), 102, 'Ribbon'), g.control('表情', 103, sub=SUB),
        g.control('髪色', 203, subparams=['HairHue'])]), MENU, 'NativeFormatImporter', g.native(11400000))
    g.write_asset(P_(E + 'ExParams.asset'), g.params_asset('ExParams', [
        ('Ears', 2, 1, 1), ('Tail', 2, 1, 1), ('Glasses', 2, 0, 1), ('Ribbon', 2, 1, 1), ('FaceLock', 0, 0, 0),
        ('HairHue', 1, 0, 1)]), PARAMS, 'NativeFormatImporter', g.native(11400000))

    # the PC prefab: a variant of the model
    v = g.Variant(ids, FBX_GUID)
    ROOT_GO, ROOT_TF = model.fid('', 1), model.fid('', 4)
    v.root(ROOT_GO, ROOT_TF, 'シンセちゃん')
    v.mod(model.fid('メガネ', 1), 'm_IsActive', 0)
    shapes = model.shapes('Body')
    v.mod(model.fid('Body', 137), 'm_BlendShapeWeights.Array.data[%d]' % shapes.index('Chest_Big'), 40)
    v.component(ROOT_GO, 114, 'MonoBehaviour', g.descriptor(
        v.stub(model.fid('Body', 137), 137), v.stub(model.fid('Eye_L', 4), 4), v.stub(model.fid('Eye_R', 4), 4),
        FX_GUID, MENU, PARAMS, ['vrc.v_' + x for x in VISEMES], shapes.index('まばたき'),
        view=tuple(unity_point((0, -0.08, 1.262)))))

    def col(bone, shape, r, h, a, b=None):
        """a PhysBone collider on a bone: a sphere at a (Blender world), or a capsule from a to b"""
        pa = Vector(local(model, bone, a))
        if b is None:
            return v.component(model.fid(bone, 1), 114, 'MonoBehaviour', g.pb_collider(
                shape, local_len(model, bone, r), 0, tuple(pa)))
        pb = Vector(local(model, bone, b))
        axis = (pb - pa).normalized()
        rot = Vector((0, 1, 0)).rotation_difference(axis)
        return v.component(model.fid(bone, 1), 114, 'MonoBehaviour', g.pb_collider(
            1, local_len(model, bone, r), (pb - pa).length + 2 * local_len(model, bone, r), tuple((pa + pb) / 2),
            (rot.x, rot.y, rot.z, rot.w)))
    head = col('Head', 0, 0.125, 0, (0, -0.005, 1.28))
    chest = col('Chest', 0, 0.105, 0, (0, 0.0, 0.97))
    arms = [col('UpperArm_' + s, 1, 0.04, 0, (0.1 * x, 0, 1.05), (0.29 * x, 0, 1.05)) for s, x in SIDES]
    legs = [col('UpperLeg_' + s, 1, 0.065, 0, (0.07 * x, 0.0, 0.66), (0.07 * x, 0.005, 0.42)) for s, x in SIDES]
    hands = [col('Hand_' + s, 0, 0.045, 0, (0.54 * x, 0, 1.045)) for s, x in SIDES]
    hair = [(114, 'MonoBehaviour', g.physbone(v.stub(model.fid('TwinTail_' + s, 4), 4), [head, chest] + arms,
                                               radius=0.03, pull=0.15, spring=0.35, stiffness=0.1, gravity=0.15,
                                               immobile=0.3)) for s, x in SIDES]
    hair.append((114, 'MonoBehaviour', g.physbone(v.stub(model.fid('HairBack', 4), 4), [head, chest], radius=0.03,
                                                  pull=0.2, spring=0.3, stiffness=0.2, gravity=0.1)))
    v.gameobject('PhysBone_髪', ROOT_TF, hair)
    ear_pb = [v.component(model.fid('Ear_' + s, 1), 114, 'MonoBehaviour', g.physbone(
        0, [], radius=0.01, pull=0.5, spring=0.4, stiffness=0.6, max_angle=25)) for s, x in SIDES]
    tail_pb = v.component(model.fid('Tail', 1), 114, 'MonoBehaviour', g.physbone(
        0, legs, radius=0.025, pull=0.12, spring=0.45, stiffness=0.05, gravity=0.08, max_angle=0))
    v.component(model.fid('Skirt_Root', 1), 114, 'MonoBehaviour', g.physbone(
        0, legs + hands, radius=0.02, pull=0.25, spring=0.2, stiffness=0.3, gravity=0.1, multi=0, max_angle=60))
    v.gameobject('HeadPat', model.fid('Head', 4), [(114, 'MonoBehaviour', g.contact_receiver(
        'HeadPat', local_len(model, 'Head', 0.13)))], pos=local(model, 'Head', (0, 0.01, 1.42)))
    PC_GUID = G('prefab/pc')
    g.write_asset(P_(AV + '/Prefab/シンセちゃん_PC.prefab'), HEAD + v.text(), PC_GUID, 'PrefabImporter')

    # the Quest prefab: a variant of the PC one, with Quest materials and without the ear and tail PhysBones
    q = g.Variant(ids, PC_GUID)
    q.root(v.own(ROOT_GO), v.own(ROOT_TF), Esc('シンセちゃん_Quest'))
    for mesh in ('Body', '髪', 'ネコミミ', 'しっぽ', '服', '靴', 'メガネ', 'リボン'):
        m = model.info.mesh[model.node(mesh)]
        for k, i in enumerate(m['used']):
            q.mod(v.own(model.fid(mesh, 137)), 'm_Materials.Array.data[%d]' % k,
                  obj=R(2100000, quest[m['materials'][i]], 2))
    q.removed_c += ear_pb + [tail_pb]
    g.write_asset(P_(AV + '/Prefab/シンセちゃん_Quest.prefab'), HEAD + q.text(), G('prefab/quest'), 'PrefabImporter')

    readme = ('シンセちゃん v1.0\n\nご購入ありがとうございます。\n'
              '・lilToon と VRChat SDK を先に導入してください。\n'
              '・Prefab フォルダの シンセちゃん_PC.prefab をシーンに置いてアップロードしてください。\n')
    g.write_asset(P_(AV + '/はじめにお読みください.txt'), readme, G('readme'), 'TextScriptImporter')
    return model, readme


# ---------------------------------------------------------------- the dress, set up for Modular Avatar

DRESS = SHOP + '/シンセちゃん用ワンピース'


def dress_assets():
    g.clear_scene()
    rig = g.Rig('Armature')
    skeleton(rig, own=False)
    rig.bone('OP_Skirt_Root', (0, 0, 0.8), (0, 0, 0.76), 'Hips')
    chains = []
    for k in range(6):
        a = 2 * math.pi * k / 6
        d = Vector((math.sin(a), -math.cos(a), 0))
        pts = [Vector((0, 0, 0.79)) + d * 0.105, Vector((0, 0, 0.64)) + d * 0.19, Vector((0, 0, 0.47)) + d * 0.26]
        chains.append(rig.chain('OP_Skirt_%d' % k, pts, 'OP_Skirt_Root'))
    rig.bone('OP_Ribbon', (0, -0.1, 1.0), (0, -0.125, 1.0), 'Chest')
    for s, x in SIDES:
        rig.chain('OP_RibbonTail_' + s, [(0.012 * x, -0.112, 0.99), (0.03 * x, -0.118, 0.93), (0.04 * x, -0.12, 0.87)],
                  'OP_Ribbon')
    rig.done()
    P = g.Parts()
    P.add('bodice', 'loft', Matrix.Identity(4), seg=24, caps=(False, False), rings=[
        (0.78, 0.112, 0.088), (0.84, 0.109, 0.084), (0.90, 0.116, 0.089), (0.96, 0.124, 0.098, -0.004),
        (1.01, 0.124, 0.093), (1.05, 0.112, 0.082), (1.075, 0.08, 0.062), (1.09, 0.054, 0.048)],
        uv_v=[0.55, 0.6, 0.66, 0.72, 0.8, 0.88, 0.94, 0.99])
    for s, x in SIDES:
        tube(P, 'sleeve_' + s, [(0.06 * x, 0, 1.05), (0.12 * x, 0, 1.055), (0.17 * x, 0, 1.05)],
             [0.055, 0.058, 0.043], seg=14, caps=(False, False), v_range=(0.7, 0.8))
    skirt_loft(P, 'skirt', [(0.8, 0.11, 0.086, 0.0), (0.72, 0.17, 0.15, 0.2), (0.6, 0.23, 0.21, 0.5),
                            (0.46, 0.28, 0.265, 0.8)], seg=36)
    ob, idx = P.make('ワンピース', mats('ワンピース'))
    bones = {'bodice': ['Hips', 'Spine', 'Chest', 'Neck'],
             'skirt': ['Hips', 'OP_Skirt_Root'] + [b for c in chains for b in c]}
    for s, x in SIDES:
        bones['sleeve_' + s] = ['Shoulder_' + s, 'UpperArm_' + s, 'Chest']
    rig.skin(ob, (idx, bones), blend=0.045)
    P = g.Parts()
    for x in (1, -1):
        M = Matrix.Translation((0.03 * x, -0.118, 1.0)) @ Matrix.Rotation(math.radians(15 * x), 4, 'Y')
        P.add('bow', 'sphere', M @ Matrix.Diagonal((1.7, 0.5, 1.0, 1)), r=0.022, us=12, vs=8)
    P.add('bow', 'sphere', Matrix.Translation((0, -0.12, 1.0)), r=0.012, us=8, vs=6)
    for s, x in SIDES:
        tube(P, 'tail_' + s, [(0.012 * x, -0.114, 0.99), (0.03 * x, -0.12, 0.93), (0.04 * x, -0.122, 0.86)],
             [0.012, 0.014, 0.016], seg=8, squash=0.3)
    rb, ridx = P.make('胸リボン', mats('胸リボン'))
    rig.skin(rb, (ridx, {'bow': ['OP_Ribbon'], 'tail_L': ['OP_RibbonTail_L', 'OP_RibbonTail_L.001'],
                         'tail_R': ['OP_RibbonTail_R', 'OP_RibbonTail_R.001']}), blend=0.02)
    fbx_rel = DRESS + '/FBX/ワンピース.fbx'
    FBX_GUID = G('dress.fbx')
    g.export_fbx(P_(fbx_rel), 'FBX_SCALE_ALL')
    t = g.write_texture(P_(DRESS + '/Textures/ワンピース.png'), tex_dress(), G('tex/dress'), default=1024, alpha=0)
    mats_ = {'ワンピース': lil(P_(DRESS + '/Materials/ワンピース.mat'), G('mat/dress'), 'ワンピース', tex=t, cull=0),
             '胸リボン': lil(P_(DRESS + '/Materials/胸リボン.mat'), G('mat/dress_ribbon'), '胸リボン',
                          color=(0.98, 0.98, 1.0, 1))}
    model = g.Model(P_(fbx_rel), fbx_rel, FBX_GUID)
    imp = g.model_importer(mats_)
    g.write_meta(P_(fbx_rel), FBX_GUID, 'ModelImporter', imp)
    model.fileids(imp)

    v = g.Variant(ids, FBX_GUID)
    ROOT_GO, ROOT_TF = model.fid('', 1), model.fid('', 4)
    v.root(ROOT_GO, ROOT_TF, 'ワンピース')
    v.component(model.fid('Armature', 1), 114, 'MonoBehaviour', g.ma(
        'MergeArmature', mergeTarget=g.ma_ref('Armature'), prefix='', suffix='', legacyLocked=0, LockMode=1,
        mangleNames=1))
    legs = []
    for s, x in SIDES:
        a, b = Vector(local(model, 'UpperLeg_' + s, (0.07 * x, 0, 0.66))), Vector(local(model, 'UpperLeg_' + s,
                                                                                         (0.07 * x, 0.005, 0.42)))
        rot = Vector((0, 1, 0)).rotation_difference((b - a).normalized())
        legs.append(v.component(model.fid('UpperLeg_' + s, 1), 114, 'MonoBehaviour', g.pb_collider(
            1, 0.068, (b - a).length + 0.136, tuple((a + b) / 2), (rot.x, rot.y, rot.z, rot.w))))
    v.component(model.fid('OP_Skirt_Root', 1), 114, 'MonoBehaviour', g.physbone(
        0, legs, radius=0.025, pull=0.2, spring=0.2, stiffness=0.25, gravity=0.12, max_angle=60))
    for s, x in SIDES:
        v.component(model.fid('OP_RibbonTail_' + s, 1), 114, 'MonoBehaviour', g.physbone(
            0, [], radius=0.01, pull=0.2, spring=0.3, gravity=0.2))
    # shown by the menu's toggles
    v.mod(model.fid('ワンピース', 1), 'm_IsActive', 0)
    v.mod(model.fid('胸リボン', 1), 'm_IsActive', 0)

    def toggle(objs):
        return (114, 'MonoBehaviour', g.ma('ObjectToggle', m_inverted=0, m_objects=[
            {'Object': g.ma_ref(p, t), 'Active': a} for p, t, a in objs]))
    _, menu_tf, _ = v.gameobject('ワンピース Menu', ROOT_TF, [
        (114, 'MonoBehaviour', g.ma_item('ワンピース', kind=103, auto=0)),
        (114, 'MonoBehaviour', g.ma('MenuInstaller', menuToAppend=R(0), installTargetMenu=R(0)))])
    v.gameobject('ワンピース', ROOT_TF, [
        (114, 'MonoBehaviour', g.ma_item('ワンピース', default=1)),
        toggle([('ワンピース/ワンピース', v.stub(model.fid('ワンピース', 1), 1), 1), ('服', 0, 0)]),
        (114, 'MonoBehaviour', g.ma('ShapeChanger', m_inverted=0, m_threshold=0.01, m_shapes=[
            {'Object': g.ma_ref('Body'), 'ShapeName': 'Body_Shrink', 'ChangeType': 1, 'Value': 100}]))],
        own_parent=menu_tf)
    v.gameobject('胸リボン', ROOT_TF, [
        (114, 'MonoBehaviour', g.ma_item('胸リボン', default=1)),
        toggle([('ワンピース/胸リボン', v.stub(model.fid('胸リボン', 1), 1), 1)])], own_parent=menu_tf)
    g.write_asset(P_(DRESS + '/Prefab/ワンピース_MA.prefab'), HEAD + v.text(), G('prefab/dress'), 'PrefabImporter')
    g.write_asset(P_(DRESS + '/はじめにお読みください.txt'),
                  'シンセちゃん用ワンピース\nModular Avatar 対応です。Prefab をアバターにドラッグ＆ドロップしてください。\n',
                  G('dress/readme'), 'TextScriptImporter')


# ---------------------------------------------------------------- the parka, with no MA setup

PARKA = SHOP + '/パーカー'
SFX = '_Parka'


def parka_assets():
    g.clear_scene()
    rig = g.Rig('Armature')
    skeleton(rig, SFX, own=False)
    hood = rig.chain('Hood' + SFX, [(0, 0.09, 1.1), (0, 0.13, 1.03), (0, 0.14, 0.95)], 'Neck' + SFX)
    strings = {s: rig.chain('String_%s%s' % (s, SFX), [(0.03 * x, -0.11, 1.07), (0.035 * x, -0.125, 0.99),
                                                      (0.035 * x, -0.13, 0.92)], 'Chest' + SFX) for s, x in SIDES}
    rig.done()
    P = g.Parts()
    P.add('body', 'loft', Matrix.Identity(4), seg=24, caps=(False, False), rings=[
        (0.7, 0.13, 0.105), (0.78, 0.128, 0.102), (0.86, 0.127, 0.104), (0.94, 0.134, 0.116, -0.008),
        (1.01, 0.135, 0.121, -0.01), (1.05, 0.123, 0.1, -0.004), (1.08, 0.088, 0.072), (1.10, 0.058, 0.052)])
    for s, x in SIDES:
        tube(P, 'sleeve_' + s, [(0.06 * x, 0, 1.05), (0.19 * x, 0, 1.05), (0.3 * x, 0, 1.05), (0.4 * x, 0, 1.05),
                                (0.49 * x, 0, 1.05)], [0.058, 0.05, 0.044, 0.04, 0.036], seg=14, caps=(False, False))
    P.add('hood', 'sphere', Matrix.Translation((0, 0.07, 1.1)) @ Matrix.Diagonal((1.0, 0.9, 0.75, 1)), r=0.12,
          us=20, vs=12)
    P.delete('hood', lambda c: c.y < 0.035 or c.z > 1.16)
    P.add('pocket', 'cube', Matrix.Translation((0, -0.107, 0.8)) @ Matrix.Diagonal((0.16, 0.012, 0.09, 1)))
    ob, idx = P.make('パーカー', mats('パーカー'))
    n = lambda b: b + SFX
    bones = {'body': [n('Hips'), n('Spine'), n('Chest')], 'pocket': [n('Hips'), n('Spine')],
             'hood': [n('Neck'), n('Chest')] + hood}
    for s, x in SIDES:
        bones['sleeve_' + s] = [n('Shoulder_' + s), n('UpperArm_' + s), n('LowerArm_' + s), n('Chest')]
    rig.skin(ob, (idx, bones), blend=0.045)
    P = g.Parts()
    for s, x in SIDES:
        tube(P, 'string_' + s, [(0.03 * x, -0.11, 1.07), (0.035 * x, -0.125, 0.99), (0.035 * x, -0.13, 0.9)],
             [0.004, 0.004, 0.006], seg=6)
    st, sidx = P.make('紐', mats('紐'))
    rig.skin(st, (sidx, {'string_' + s: strings[s] for s, x in SIDES}), blend=0.02)
    fbx_rel = PARKA + '/FBX/パーカー.fbx'
    FBX_GUID = G('parka.fbx')
    g.export_fbx(P_(fbx_rel))  # the 100x bone scale
    t = g.write_texture(P_(PARKA + '/Textures/パーカー.png'), tex_parka(), G('tex/parka'), default=512, alpha=0)
    mats_ = {'パーカー': lil(P_(PARKA + '/Materials/パーカー.mat'), G('mat/parka'), 'パーカー', tex=t, cull=0),
             '紐': lil(P_(PARKA + '/Materials/紐.mat'), G('mat/string'), '紐', color=(0.95, 0.95, 0.95, 1))}
    model = g.Model(P_(fbx_rel), fbx_rel, FBX_GUID)
    imp = g.model_importer(mats_)
    g.write_meta(P_(fbx_rel), FBX_GUID, 'ModelImporter', imp)
    model.fileids(imp)
    v = g.Variant(ids, FBX_GUID)
    v.root(model.fid('', 1), model.fid('', 4), 'パーカー')
    for s, x in SIDES:
        v.component(model.fid('String_%s%s' % (s, SFX), 1), 114, 'MonoBehaviour', g.physbone(
            0, [], radius=local_len(model, 'Chest' + SFX, 0.005), pull=0.2, spring=0.2, gravity=0.3))
    v.component(model.fid('Hood' + SFX, 1), 114, 'MonoBehaviour', g.physbone(
        0, [], radius=local_len(model, 'Chest' + SFX, 0.02), pull=0.3, spring=0.2, stiffness=0.4, gravity=0.1))
    g.write_asset(P_(PARKA + '/Prefab/パーカー.prefab'), HEAD + v.text(), G('prefab/parka'), 'PrefabImporter')
    print('booth: %s: bone scale %g' % (fbx_rel, model.lossy('Chest' + SFX)))


# ---------------------------------------------------------------- the cardigan, set up for VRCFury

CARD = SHOP + '/シンセちゃん用カーディガン'


def tex_knit():
    x, y = grid(256, 256)
    a = np.ones((256, 256, 4), np.float32)
    a[..., :3] = (0.98, 0.94, 0.86)
    rib = np.sin(x * 2 * math.pi * 48) > 0.2
    a[rib, :3] *= 0.93
    a[(np.floor(y * 12) % 4 == 0), :3] = (0.96, 0.68, 0.78)  # pink stripes
    return a


def cardigan_assets():
    """a cardigan for her, with VRCFury: an Armature Link, a toggle (on at first; it also shrinks her body), a toggle
    saved by an old VRCFury in Unity 2019 (rolled-up sleeves, a shape key), and a Full Controller whose FX shows a bow
    at the back by a parameter called Ribbon, as hers is (VRCFury keeps them apart)"""
    g.clear_scene()
    rig = g.Rig('Armature')
    skeleton(rig, own=False)
    bow = rig.chain('Cardigan_Ribbon', [(0, 0.115, 0.86), (0, 0.14, 0.78), (0, 0.15, 0.69)], 'Spine')
    rig.done()
    P = g.Parts()
    P.add('body', 'loft', Matrix.Identity(4), seg=28, caps=(False, False), rings=[
        (0.76, 0.126, 0.1), (0.84, 0.121, 0.096), (0.90, 0.127, 0.102), (0.96, 0.135, 0.112, -0.008),
        (1.01, 0.136, 0.116, -0.01), (1.05, 0.124, 0.094, -0.004), (1.078, 0.086, 0.068), (1.094, 0.057, 0.051)])
    P.delete('body', lambda c: c.y < -0.08 and abs(c.x) < 0.03)  # it is open at the front
    for s, x in SIDES:
        tube(P, 'sleeve_' + s, [(0.06 * x, 0, 1.05), (0.19 * x, 0, 1.05), (0.3 * x, 0, 1.05), (0.4 * x, 0, 1.05),
                                (0.47 * x, 0, 1.05)], [0.055, 0.048, 0.042, 0.038, 0.036], seg=14, caps=(False, False))
    ob, idx = P.make('カーディガン', mats('カーディガン'))
    bones = {'body': ['Hips', 'Spine', 'Chest']}
    for s, x in SIDES:
        bones['sleeve_' + s] = ['Shoulder_' + s, 'UpperArm_' + s, 'LowerArm_' + s, 'Chest']
    rig.skin(ob, (idx, bones), blend=0.045)
    sleeves = set(idx['sleeve_L']) | set(idx['sleeve_R'])
    g.shape_key(ob, '袖まくり', lambda c: Vector((math.copysign(min(abs(c.x), 0.3 + (abs(c.x) - 0.3) * 0.2), c.x),
                                                  c.y, c.z)) if abs(c.x) > 0.3 else None, sleeves)
    g.shape_key(ob, 'chest_big', chest_big, set(idx['body']))  # the body's Chest_Big, named loosely
    P = g.Parts()
    for z in (0.8, 0.88, 0.96):
        for x in (1, -1):
            P.add('button', 'sphere', Matrix.Translation((0.04 * x, face_body_y(z) - 0.004, z)), r=0.009, us=8, vs=6)
    buttons, _ = P.make('ボタン', mats('ボタン'))
    rig.skin(buttons, ['Spine', 'Chest'])
    P = g.Parts()
    for x in (1, -1):
        M = Matrix.Translation((0.028 * x, 0.128, 0.865)) @ Matrix.Rotation(math.radians(30 * x), 4, 'Y')
        P.add('loop', 'sphere', M @ Matrix.Diagonal((1.6, 0.45, 1.0, 1)), r=0.02, us=12, vs=8)
        tube(P, 'tail_' + ('L' if x > 0 else 'R'), [(0.01 * x, 0.126, 0.85), (0.025 * x, 0.145, 0.77),
                                                    (0.035 * x, 0.155, 0.69)], [0.01, 0.012, 0.014], seg=8, squash=0.35)
    rb, ridx = P.make('リボン_背中', mats('リボン_背中'))
    rig.skin(rb, (ridx, {'loop': ['Spine'], 'tail_L': bow, 'tail_R': bow}), blend=0.02)
    fbx_rel = CARD + '/FBX/カーディガン.fbx'
    FBX_GUID = G('cardigan.fbx')
    g.export_fbx(P_(fbx_rel), 'FBX_SCALE_ALL')
    t = g.write_texture(P_(CARD + '/Textures/カーディガン.png'), tex_knit(), G('tex/knit'), default=1024, alpha=0)
    M = CARD + '/Materials/'
    mats_ = {'カーディガン': lil(P_(M + 'カーディガン.mat'), G('mat/cardigan'), 'カーディガン', tex=t, cull=0),
             'ボタン': lil(P_(M + 'ボタン.mat'), G('mat/button'), 'ボタン', color=(0.95, 0.78, 0.35, 1)),
             'リボン_背中': lil(P_(M + 'リボン_背中.mat'), G('mat/back_ribbon'), 'リボン_背中', color=(0.95, 0.5, 0.65, 1))}
    model = g.Model(P_(fbx_rel), fbx_rel, FBX_GUID)
    imp = g.model_importer(mats_)
    g.write_meta(P_(fbx_rel), FBX_GUID, 'ModelImporter', imp)
    model.fileids(imp)

    # its FX, menu and parameters, for the Full Controller
    A = CARD + '/VRCFury/'
    clips = {}
    for name, v in (('背中リボン_ON', 1), ('背中リボン_OFF', 0)):
        clips[name] = G('card/anim/' + name)
        g.write_asset(P_(A + name + '.anim'), g.clip_yaml(name, [('リボン_背中', 1, 'm_IsActive', v)]), clips[name],
                      'NativeFormatImporter', g.native(7400000))
    fx = g.Controller(ids, clips)
    fx.param('Ribbon', 4, 1)
    fx.toggle('背中リボン', 'Ribbon', '背中リボン_ON', '背中リボン_OFF')
    FXG, MENU, PRMS = G('card/fx'), G('card/menu'), G('card/params')
    g.write_asset(P_(A + 'FX_カーディガン.controller'), fx.yaml('FX_カーディガン'), FXG, 'NativeFormatImporter',
                  g.native(9100000))
    g.write_asset(P_(A + 'カーディガンメニュー.asset'), g.menu_asset('カーディガンメニュー', [
        g.control('背中リボン', 102, 'Ribbon')]), MENU, 'NativeFormatImporter', g.native(11400000))
    g.write_asset(P_(A + 'カーディガンパラメータ.asset'), g.params_asset('カーディガンパラメータ', [('Ribbon', 2, 1, 1)]), PRMS,
                  'NativeFormatImporter', g.native(11400000))

    v = g.Variant(ids, FBX_GUID)
    ROOT_GO, ROOT_TF = model.fid('', 1), model.fid('', 4)
    v.root(ROOT_GO, ROOT_TF, 'カーディガン')
    refs = g.Refs(ids)
    v.component(ROOT_GO, 114, 'MonoBehaviour', g.vrcfury(refs, refs.add(
        'ArmatureLink', g.vf_armature_link(v.stub(model.fid('Hips', 1), 1)))))
    refs = g.Refs(ids)
    v.component(ROOT_GO, 114, 'MonoBehaviour', g.vrcfury(refs, refs.add('Toggle', g.vf_toggle(
        'Clothes/カーディガン', [refs.action('ObjectToggleAction', obj=R(v.stub(model.fid('カーディガン', 1), 1)), mode=0),
                            refs.action('ObjectToggleAction', obj=R(v.stub(model.fid('ボタン', 1), 1)), mode=0),
                            refs.action('BlendShapeAction', blendShape='Body_Shrink', blendShapeValue=100,
                                        renderer=R(0), allRenderers=1)], on=1))))
    refs = g.Refs(ids, version=1)  # saved long ago
    old = dict(g.vf_toggle('Clothes/袖まくり', [refs.action(
        'BlendShapeAction', blendShape='袖まくり', blendShapeValue=100, renderer=R(v.stub(model.fid('カーディガン', 137), 137)),
        allRenderers=0)]), version=0)
    v.component(ROOT_GO, 114, 'MonoBehaviour', g.vrcfury(refs, None, [refs.add('Toggle', old)]))
    refs = g.Refs(ids)
    v.component(ROOT_GO, 114, 'MonoBehaviour', g.vrcfury(refs, refs.add('FullController', g.vf_full_controller(
        controllers=[(g.guid_asset(FXG, 9100000, A + 'FX_カーディガン.controller'), 5)],
        menus=[(g.guid_asset(MENU, 11400000, A + 'カーディガンメニュー.asset'), 'Clothes/カーディガン設定')],
        prms=[g.guid_asset(PRMS, 11400000, A + 'カーディガンパラメータ.asset')]))))
    refs = g.Refs(ids)
    v.component(ROOT_GO, 114, 'MonoBehaviour', g.vrcfury(refs, refs.add('BlendShapeLink', {
        'version': 1, 'objs': [], 'linkSkins': [{'renderer': R(v.stub(model.fid('カーディガン', 137), 137))}],
        'baseObj': 'Body', 'includeAll': 1, 'exactMatch': 0, 'excludes': [], 'includes': []})))
    v.component(model.fid('Cardigan_Ribbon', 1), 114, 'MonoBehaviour', g.physbone(
        0, [], radius=0.01, pull=0.2, spring=0.3, gravity=0.2))
    g.write_asset(P_(CARD + '/Prefab/カーディガン_VRCFury.prefab'), HEAD + v.text(), G('prefab/cardigan'),
                  'PrefabImporter')


def face_body_y(z):
    """the front of her cardigan at height z, near the middle"""
    return -0.115 if z > 0.9 else -0.107


# ---------------------------------------------------------------- a hair pin, linked by an old VRCFury

PIN = SHOP + '/ヘアピン'


def hairpin_assets():
    """a hair pin: two meshes at the top of their FBX, where they go on her head, and a VRCFury Armature Link saved
    by VRCFury 1.x (version 5: link mode Reparent Root to the Head, bone offsets kept)"""
    g.clear_scene()
    P = g.Parts()
    M, d = g.along((0.07, -0.075, 1.345), (0.115, -0.03, 1.325))
    P.add('pin', 'cyl', M, r1=0.005, depth=d, seg=8)
    pin, _ = P.make('ヘアピン', mats('ヘアピン'))
    P = g.Parts()
    for k in range(5):
        a = 2 * math.pi * k / 5
        c = Vector((0.08, -0.07, 1.345))
        M = Matrix.Translation(c + Vector((0.012 * math.cos(a), -0.004, 0.012 * math.sin(a))))
        P.add('star', 'sphere', M, r=0.008, us=8, vs=6)
    deco, _ = P.make('ヘアピン_飾り', mats('ヘアピン_飾り'))
    fbx_rel = PIN + '/FBX/ヘアピン.fbx'
    FBX_GUID = G('hairpin.fbx')
    g.export_fbx(P_(fbx_rel), 'FBX_SCALE_ALL')
    mats_ = {'ヘアピン': lil(P_(PIN + '/Materials/ヘアピン.mat'), G('mat/pin'), 'ヘアピン', color=(0.85, 0.85, 0.9, 1)),
             'ヘアピン_飾り': lil(P_(PIN + '/Materials/ヘアピン_飾り.mat'), G('mat/pin_deco'), 'ヘアピン_飾り',
                            color=(1.0, 0.85, 0.2, 1), emission=(0.3, 0.24, 0.05))}
    model = g.Model(P_(fbx_rel), fbx_rel, FBX_GUID)
    imp = g.model_importer(mats_)
    g.write_meta(P_(fbx_rel), FBX_GUID, 'ModelImporter', imp)
    model.fileids(imp)
    v = g.Variant(ids, FBX_GUID)
    ROOT_GO, ROOT_TF = model.fid('', 1), model.fid('', 4)
    v.root(ROOT_GO, ROOT_TF, 'ヘアピン')
    refs = g.Refs(ids)
    v.component(ROOT_GO, 114, 'MonoBehaviour', g.vrcfury(refs, refs.add('ArmatureLink', g.vf_armature_link(
        v.stub(ROOT_GO, 1), legacy={'version': 5, 'linkMode': 3, 'boneOnAvatar': 10, 'keepBoneOffsets2': 1,
                                    'skinRewriteScalingFactor': 0}))))
    g.write_asset(P_(PIN + '/Prefab/ヘアピン.prefab'), HEAD + v.text(), G('prefab/hairpin'), 'PrefabImporter')


# ---------------------------------------------------------------- packing

avatar_model, README = avatar_assets()
dress_assets()
parka_assets()
cardigan_assets()
hairpin_assets()
g.folder_metas(PROJ, 'booth')
packs = []
for name, prefix, extra in (('SynthChan_v1.0.unitypackage', AV, ''),
                            ('SynthChan_OnePiece_v1.0.unitypackage', DRESS, '\n00'),
                            ('Parka_v1.0.unitypackage', PARKA, ''),
                            ('SynthChan_Cardigan_VRCFury_v1.0.unitypackage', CARD, ''),
                            ('Hairpin_v1.0.unitypackage', PIN, '')):
    n = g.unitypackage(PROJ, [prefix], os.path.join(OUT, name), pathname_extra=extra)
    packs.append('%s (%d entries)' % (name, n))
terms = ('利用規約\n\n本データは VRChat 等での個人利用に限ります。再配布は禁止です。\n'
         '（これはテスト用の架空の商品です）\n')
g.booth_zip(os.path.join(OUT, 'シンセちゃん_v1.0.zip'), [
    ('シンセちゃん_v1.0/SynthChan_v1.0.unitypackage', os.path.join(OUT, 'SynthChan_v1.0.unitypackage')),
    ('シンセちゃん_v1.0/はじめにお読みください.txt', README.encode('cp932')),
    ('シンセちゃん_v1.0/利用規約.txt', terms.encode('cp932')),
    ('シンセちゃん_v1.0/表情一覧.txt', ('、'.join(FACES) + '\n').encode('cp932'))])
print('booth: wrote %s: %s, シンセちゃん_v1.0.zip' % (OUT, ', '.join(packs)))
