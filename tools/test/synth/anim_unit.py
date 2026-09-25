# anim_unit.py: the animation side of tools/unity2hypr3d.py on small hand-made cases: Transform curves and where they
# put the GLB's nodes, VRCFury's Scale, Smooth Loop and World Drop, 2D blend trees and puppets, avatar masks and the
# Gesture layer's hand poses
#   blender -b --factory-startup --python-exit-code 1 -P anim_unit.py
import sys, os, math
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))  # tools/
import unity2hypr3d as u
from types import SimpleNamespace as NS
from mathutils import Matrix, Vector, Quaternion, Euler

FAILS = []


def check(what, got, want):
    ok = got == want
    print('%s %s: %r%s' % ('ok  ' if ok else 'FAIL', what, got, '' if ok else ' (want %r)' % (want,)))
    if not ok:
        FAILS.append(what)


def near(what, a, b, eps=1e-4):
    d = max(abs(x - y) for x, y in zip(a, b)) if len(a) == len(b) else 1e9
    ok = d < eps
    print('%s %s: %s%s' % ('ok  ' if ok else 'FAIL', what, tuple(round(x, 4) for x in a),
                           '' if ok else ' (want %s)' % (tuple(round(x, 4) for x in b),)))
    if not ok:
        FAILS.append(what)


def go(name, parent=None, active=True):
    g = u.Obj(1, 'GameObject', {'m_Name': name, 'm_IsActive': '1' if active else '0', 'm_TagString': 'Untagged'})
    tf = u.Obj(4, 'Transform', {'m_GameObject': g})
    g.tf, g.comps, g.children, g.parent = tf, [tf], [], parent
    if parent is not None:
        parent.children.append(g)
    return g


class AV:
    """the parts of the converter's Avatar that clips and the VRCFury code need"""
    default = u.Avatar.default
    prop = u.Avatar.prop
    comp = u.Avatar.comp

    def __init__(self, root, gos):
        self.root, self.gos = root, gos
        self.inside = set(map(id, gos))
        self.renderers = [c for g in gos for c in g.comps if c.cls in u.RENDERERS]
        self.paths, self.tf_rest, self.ma = {}, {}, None
        for g in gos:
            parts, x = [], g
            while x is not None and x is not root:
                parts.append(u.go_name(x))
                x = x.parent
            self.paths.setdefault('/'.join(reversed(parts)), g)

    def shape_names(self, smr):
        return smr.data.get('_names', [])


def vec_curve(path, keys):
    return {'curve': {'m_Curve': [{'time': t, 'value': dict(zip('xyzw', v))} for t, v in keys]}, 'path': path}


def float_curve(path, cid, attr, keys):
    return {'curve': {'m_Curve': [{'time': t, 'value': v} for t, v in keys]}, 'path': path, 'classID': cid,
            'attribute': attr}


print('== Transform curves: vectors of position, rotation, Euler angles and scale; the editor\'s copies left out')
root = go('Avatar')
arm = go('Armature', root)
tail = go('Tail', arm)
av = AV(root, [root, arm, tail])
av.tf_rest[id(tail)] = {'p': (0.0, 0.1, 0.0), 'q': (0.0, 0.0, 0.0, 1.0), 'e': (0.0, 0.0, 0.0), 's': (1.0, 1.0, 1.0)}
body = {'m_Name': 'c', 'm_AnimationClipSettings': {'m_StopTime': '1'},
        'm_PositionCurves': [vec_curve('Armature/Tail', [(0, (0, 0.1, 0)), (1, (0, 0.3, 0))])],
        'm_EulerCurves': [vec_curve('Armature/Tail', [(0, (0, 0, 0)), (1, (40, 0, 0))])],
        'm_ScaleCurves': [vec_curve('Armature/Tail', [(0, (1, 1, 1)), (1, (2, 2, 2))])],
        'm_RotationCurves': [vec_curve('Nowhere', [(0, (0, 0, 0, 1))])],
        'm_EditorCurves': [float_curve('Armature/Tail', 4, 'm_LocalPosition.y', [(0, 0.1), (1, 0.3)])]}
clip = u.Clip(av, body)
check('the curves: each axis a property; the editor\'s copy of one is not counted twice, one to nowhere is "other"',
      (sorted(p[2] for p, _ in clip.curves), clip.other), (['ex', 'ey', 'ez', 'px', 'py', 'pz', 'sx', 'sy', 'sz'], 4))
v = clip.sample(0.5)
check('half way: straight lines', (round(v[('t', tail, 'py')], 4), round(v[('t', tail, 'ex')], 4), v[('t', tail, 'sx')]),
      (0.2, 20.0, 1.5))
clip2 = u.Clip(av, {'m_Name': 'd', 'm_AnimationClipSettings': {}, 'm_EditorCurves': [
    float_curve('Armature/Tail', 4, 'm_LocalScale.x', [(0, 3.0)]),
    float_curve('Armature/Tail', 4, 'localEulerAnglesRaw.y', [(0, 15.0)])]})
check('editor curves alone (no vectors): used', sorted((p[2], k[0][1]) for p, k in clip2.curves),
      [('ey', 15.0), ('sx', 3.0)])
check('a Transform\'s value at rest (Avatar.default)', [av.default(('t', tail, c)) for c in ('py', 'qw', 'sx', 'ez', 'm')],
      [0.1, 1.0, 1.0, 0.0, 1.0])

print('== a Transform\'s local matrix from its components: Unity\'s Euler order (Z, then X, then Y), a scale multiplier')
rest = {'p': (1.0, 2.0, 3.0), 'q': (0.0, 0.0, 0.0, 1.0), 'e': (0.0, 0.0, 0.0), 's': (1.0, 1.0, 1.0)}
R = u.tf_local({'ex': 10.0, 'ey': 20.0, 'ez': 30.0}, rest).to_quaternion()
want = Quaternion((0, 1, 0), math.radians(20)) @ Quaternion((1, 0, 0), math.radians(10)) @ Quaternion((0, 0, 1),
                                                                                                        math.radians(30))
check('Quaternion.Euler(10, 20, 30) = y @ x @ z', abs(R.dot(want)) > 1 - 1e-9, True)
M = u.tf_local({'sx': 2.0, 'm': 1.5, 'py': 5.0}, rest)
near('the rest\'s own where none is set; a multiplier over the scale', tuple(M.translation) + tuple(M.to_scale()),
     (1.0, 5.0, 3.0, 3.0, 1.5, 1.5))
check('the same place, to within the tolerances', (u.tf_same({'px': 1.00005}, {}, rest), u.tf_same({'px': 1.001}, {}, rest),
                                                   u.tf_same({'qw': -1.0}, {}, rest)), (True, False, True))

print('== where the GLB\'s node goes: a bone (the GLB\'s axes are Unity\'s) and a mesh object (turned 90 degrees)')
FLIP = u.FLIP
Ub = Matrix.LocRotScale(Vector((0.2, 1.0, 0.1)), Quaternion((1, 0, 0), math.radians(30)), None)
Um = Matrix.Translation(Vector((0.0, 1.2, 0.0)))
bone, mesh = go('Bone', root), go('Mesh', root)
bone.name, mesh.name = 'Bone', 'Mesh'
C = Matrix.Rotation(math.radians(-90), 4, 'X')  # how Blender's exporter turns an object
W = [Matrix.Identity(4), FLIP @ Ub @ FLIP, FLIP @ Um @ FLIP @ C]
st = u.Settings.__new__(u.Settings)
st.nodes = [{'name': 'Root', 'children': [1, 2]}, {'name': 'Bone'}, {'name': 'Mesh'}]
st.index = {'Root': 0, 'Bone': 1, 'Mesh': 2}
st.parent, st.world, st.world0 = [-1, 0, 0], list(W), list(W)
st.b = NS(U={id(root): Matrix.Identity(4), id(bone): Ub, id(mesh): Um})
st.ma = NS(up=lambda g: root)
st.av = NS(tf_rest={})
got = st.transforms({bone: {'sx': 2.0, 'sy': 2.0, 'sz': 2.0}, mesh: {'sx': 1.0, 'sy': 3.0, 'sz': 1.0}})
check('a bone scaled: just its scale', got['Bone'], {'s': [2.0, 2.0, 2.0]})
check('the object: Unity\'s y is the node\'s z (only the scale changes)', (sorted(got['Mesh']), got['Mesh']['s']),
      (['s'], [1.0, 1.0, 3.0]))
got = st.transforms({bone: {'ex': 30.0, 'ey': 45.0, 'ez': 0.0}})
q = Quaternion(got['Bone']['r'][3:] + got['Bone']['r'][:3])
wq = (FLIP @ Euler((math.radians(30), math.radians(45), 0), 'ZXY').to_matrix().to_4x4() @ FLIP).to_quaternion()
check('a turn: mirrored in x as glTF has it', abs(q.dot(wq)) > 1 - 1e-6, True)

print('== VRCFury: Scale (a multiplier), Smooth Loop and World Drop in toggles')


class AN:
    pass


def vrcf(av, feats):
    comp = u.Obj(114, 'MonoBehaviour', {'m_Script': {'fileID': '11500000', 'guid': u.VRCF_GUID, 'type': '3'},
                                        'm_GameObject': av.root})
    u._VRCF_FEATURES[id(comp)] = [x for f in feats for x in u.vrcf_upgrade(f)]
    an = AN()
    an.av, an.db = av, None
    return u.VRCFury(an, [comp])


face = go('Face', root)
smr = u.Obj(137, 'SkinnedMeshRenderer', {'m_GameObject': face, '_names': ['Breath'], 'm_BlendShapeWeights': ['0']})
face.comps.append(smr)
heart = go('Heart', root)
av = AV(root, [root, arm, tail, face, heart])
bs = lambda v: {'@class': 'BlendShapeAction', 'blendShape': 'Breath', 'blendShapeValue': v, 'allRenderers': '1'}
vf = vrcf(av, [{'@class': 'Toggle', 'version': 3, 'name': 'Big', 'state': {'actions': [
    {'@class': 'ScaleAction', 'obj': heart, 'scale': 2.0}]}},
    {'@class': 'Toggle', 'version': 3, 'name': 'Drop', 'state': {'actions': [{'@class': 'WorldDropAction', 'obj': heart}]}},
    {'@class': 'Breathing', 'version': 0, 'inState': {'actions': []}, 'outState': {'actions': []}, 'obj': tail,
     'scaleMin': 0.9, 'scaleMax': 1.1, 'blendshape': 'Breath'}])
check('Scale: the object\'s scale times this', vf.rules[0][2], {('t', heart, 'm'): 2.0})
check('... and as a slider, straight from 1 (smoothly, as VRCFury\'s keys go)', round(vf.apply({'VF0_Big': 1.0}, {})[
    ('t', heart, 'm')], 4), 2.0)
check('World Drop: the object, while its toggle is on', [(k, [u.go_name(g) for g in v]) for k, v in vf.drops.items()],
      [('VF1_Drop', ['Heart'])])
secs, a, b = vf.loops['VF2_Breathing']
check('the old Breathing: a Smooth Loop of 5 s, out (at 100, 1.1 x) and back in (0, 0.9 x)', (secs, a, b), (
    5.0, {('t', tail, 'm'): 1.1, ('s', smr, 'Breath'): 100}, {('t', tail, 'm'): 0.9, ('s', smr, 'Breath'): 0}))
check('nothing of them missed', vf.missed, {})

print('== 2D blend trees: gradient band weights, polar for the directional kinds')
pts = [(0, 0), (1, 0), (0, 1), (-1, 0), (0, -1)]
r = lambda ws: [round(w, 3) for w in ws]
for polar in (False, True):
    kind = 'polar' if polar else 'cartesian'
    check('%s: at a child, all of it' % kind, r(u.blend2d(pts, (0, 1), polar)), [0.0, 0.0, 1.0, 0.0, 0.0])
    check('%s: half way out, half of it and half of the middle' % kind, r(u.blend2d(pts, (0.5, 0), polar)),
          [0.5, 0.5, 0.0, 0.0, 0.0])
w = u.blend2d(pts, (0.5, 0.5), True)
check('polar, between two: as much of each, and the rest the middle\'s', (round(w[1], 3) == round(w[2], 3),
                                                                        round(sum(w), 6), w[3] < 1e-6), (True, 1.0, True))
check('past the ends: the nearest', r(u.blend2d([(0, 0), (1, 0)], (-2, 0), False)), [1.0, 0.0])

print('== a four-axis puppet as a 2D slider: how far up, right, down and left')
an = u.Analysis.__new__(u.Analysis)
an.av = NS(renderers=[], default=lambda pr: 0.0, visible=lambda r, v: True, tf_rest={})
an.params = {}
UP, RT, DN, LF = ('s', smr, 'Up'), ('s', smr, 'Right'), ('s', smr, 'Down'), ('s', smr, 'Left')
an.evaluate = lambda p: ({UP: 100 * p.get('U', 0), RT: 100 * p.get('R', 0), DN: 100 * p.get('D', 0),
                          LF: 100 * p.get('L', 0)}, {}, {}, p)
an.material_diff = lambda vals, base: {}
s = an.slider2('Four', ['U', 'R', 'D', 'L'], 202, {}, {}, {})
key = lambda x, y: next(k for k in s['keys'] if abs(k['at'][0] - x) < 1e-6 and abs(k['at'][1] - y) < 1e-6)
check('a 9 x 9 grid, row by row from the bottom', (s['grid'], len(s['keys']), s['keys'][0]['at'], s['keys'][1]['at']),
      (9, 81, (-1.0, -1.0), (-0.75, -1.0)))
check('up, left, and half way down right', [sorted((k[1], v) for k, v in key(x, y)['shapes'].items() if v) for x, y in
                                            ((0, 1), (-1, 0), (0.5, -0.5))],
      [[('Up', 1.0)], [('Left', 1.0)], [('Down', 0.5), ('Right', 0.5)]])

print('== avatar masks: VRChat\'s SDK ones by guid, others read from their m_Mask; the humanoid parts muscles move')
check('the SDK\'s left hand mask', u.mask_parts(None, ('7ff0199655202a04eb175de45a6e078a', 31900000)), {7})


class DB:
    def __init__(self, text):
        self.uf = u.UFile.__new__(u.UFile)
        self.text = text

    def get(self, guid):
        return NS(ext='.mask')

    def yaml(self, guid):
        return NS(binary=False, cls=lambda fid: 319, main=lambda cls: 31900000,
                  get=lambda fid: (319, {'m_Mask': self.text}))
hexmask = ''.join('%08x' % int.from_bytes(bytes([v, 0, 0, 0]), 'big') for v in (0, 1, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0))
check('a mask of its own: the body and the right fingers', u.mask_parts(DB(hexmask), ('a' * 32, 31900000)), {1, 8})
check('no mask: none', u.mask_parts(None, None), None)
check('the parts of a few muscles', [u.muscle_part(u.MUSCLE_OF[m]) for m in (
    'LeftHand.Index.1 Stretched', 'RightHand.Thumb.Spread', 'Spine Front-Back', 'Head Nod Down-Up',
    'Left Arm Down-Up', 'Right Upper Leg Front-Back')], [7, 8, 1, 2, 5, 4])

print('== the hand poses as a humanoid clip: each sign at its second, both hands')
hp = u.HandPoses({(0, 1): {3: -1.0}, (1, 1): {50: -0.5}, (1, 4): {60: 1.0}})
check('at 1 s: both hands\' fists; at 4 s the right hand\'s victory; from t = 0 to 7 by seconds',
      (hp.at(1.0)[0], hp.at(4.2)[0], hp.length, hp.rate), ({3: -1.0, 50: -0.5}, {60: 1.0}, 7.0, 1.0))

print('\n%d failure(s)' % len(FAILS) if FAILS else '\nall passed')
sys.exit(1 if FAILS else 0)
