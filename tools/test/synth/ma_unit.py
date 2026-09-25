# ma_unit.py: ModularAvatar on small hand-made hierarchies, against what MA 1.18.7's code does
import sys, os, math
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))  # tools/
import unity2hypr3d as u
import bpy
from mathutils import Matrix, Vector, Quaternion

FAILS = []


def check(what, got, want):
    ok = got == want
    print('%s %s: %r%s' % ('ok  ' if ok else 'FAIL', what, got, '' if ok else ' (want %r)' % (want,)))
    if not ok:
        FAILS.append(what)


def near(what, a, b, eps=1e-5):
    d = (Vector(a) - Vector(b)).length
    ok = d < eps
    print('%s %s: %s%s' % ('ok  ' if ok else 'FAIL', what, tuple(round(x, 4) for x in a), '' if ok else ' (want %s)' % (tuple(round(x, 4) for x in b),)))
    if not ok:
        FAILS.append(what)


class Scene:
    def __init__(self):
        self.gos, self.U = [], {}
        self.root = self.go('Avatar', None, (0, 0, 0))

    def go(self, name, parent, pos, rot=None, scale=None):
        g = u.Obj(1, 'GameObject', {'m_Name': name, 'm_IsActive': '1', 'm_TagString': 'Untagged'})
        tf = u.Obj(4, 'Transform', {'m_GameObject': g})
        g.tf, g.comps, g.children, g.parent = tf, [tf], [], parent
        if parent is not None:
            parent.children.append(g)
        L = Matrix.LocRotScale(Vector(pos), rot or Quaternion(), Vector(scale or (1, 1, 1)))
        self.U[id(g)] = L if parent is None else self.U[id(parent)] @ Matrix.Translation(
            self.U[id(parent)].inverted() @ Vector(pos)) if False else L  # world matrices given directly
        self.gos.append(g)
        return g

    def comp(self, g, data, cls=114, script=None):
        c = u.Obj(cls, 'MonoBehaviour', dict(data))
        c.data['m_GameObject'] = g
        if script:
            c.data['m_Script'] = {'fileID': '11500000', 'guid': script, 'type': '3'}
        g.comps.append(c)
        return c

    def run(self, human=(), outfits=()):
        order = []

        def walk(g):
            order.append(g)
            for c in g.children:
                walk(c)
        walk(self.root)

        class AV:
            pass
        av = AV()
        av.root, av.gos, av.inside = self.root, order, set(map(id, order))
        av.renderers = [c for g in order for c in g.comps if c.cls in u.RENDERERS]
        return u.ModularAvatar(av, dict(human), self.U, outfits)


MERGE = '2df373bf91cf30b4bbd495e11cb1a2ec'
PROXY = '42581d8044b64899834d3d515ab3a144'
MOVETO = '4e6bb6a99e499d2489ccf296662fa3cd'
BLOCKER = 'a5bf908a199a4648845ebe2fd3b5a4bd'


def merge_comp(sc, g, path, prefix='', suffix='', mangle='1', target=None):
    return sc.comp(g, {'mergeTarget': {'referencePath': path, 'targetObject': target}, 'prefix': prefix,
                       'suffix': suffix, 'mangleNames': mangle}, script=MERGE)


def pb(sc, g, root=None, ignore=()):
    return sc.comp(g, {'pull': '0.2', 'multiChildType': '0', 'rootTransform': root.tf if root else None,
                       'ignoreTransforms': [x.tf for x in ignore]})


def names(ma, g):
    return [ma.nm[id(c)].split('$')[0] for c in ma.kids[id(g)]]


def avatar(sc):
    arm = sc.go('Armature', sc.root, (0, 0, 0))
    hips = sc.go('Hips', arm, (0, 1, 0))
    spine = sc.go('Spine', hips, (0, 1.1, 0))
    chest = sc.go('Chest', spine, (0, 1.3, 0))
    head = sc.go('Head', chest, (0, 1.6, 0))
    return arm, hips, spine, chest, head


print('== prefix and suffix, names kept unmangled, a bone the avatar lacks')
sc = Scene()
arm, hips, spine, chest, head = avatar(sc)
o = sc.go('Outfit', sc.root, (0, 0, 0))
oa = sc.go('Armature', o, (0, 0, 0))
oh = sc.go('O.Hips.x', oa, (0, 1, 0))
os_ = sc.go('O.Spine.x', oh, (0, 1.1, 0))
ot = sc.go('Tail', oh, (0, 1, -0.1))
merge_comp(sc, oa, 'Armature', 'O.', '.x', mangle='0')
ma = sc.run({'Hips': hips, 'Spine': spine, 'Chest': chest, 'Head': head})
check('outfit Hips and Spine removed', sorted(ma.nm[k] for k in ma.deleted), ['Armature', 'O.Hips.x', 'O.Spine.x'])
check('Tail moved to Hips', ma.up(ot) is hips, True)
check('names stay when mangleNames is off', ma.nm[id(ot)], 'Tail')
check('retargets', sorted((ma.nm[k], ma.nm[id(d)]) for k, d in ma.retarget.items()),
      [('Armature', 'Armature'), ('O.Hips.x', 'Hips'), ('O.Spine.x', 'Spine')])
check('Tail has a blocker', id(ot) in ma.pbblock, True)

print('== an outfit PhysBone on a bone that is not where the avatar\'s is: its chain stays with it')
sc = Scene()
arm, hips, spine, chest, head = avatar(sc)
o = sc.go('Outfit', sc.root, (0, 0, 0))
oa = sc.go('Armature', o, (0, 0, 0))
oh = sc.go('Hips', oa, (0, 1, 0))
os_ = sc.go('Spine', oh, (0, 1.15, 0))  # 5 cm off the avatar's Spine
oc = sc.go('Chest', os_, (0, 1.3, 0))
ign = sc.go('Chest', oc, (0, 1.4, 0))  # a Chest under Chest, ignored by the PhysBone
merge_comp(sc, oa, 'Armature')
pb(sc, os_, ignore=[oc])
ma = sc.run({'Hips': hips, 'Spine': spine, 'Chest': chest, 'Head': head})
check('outfit Spine kept (its PhysBone)', id(os_) in ma.deleted, False)
check('outfit Spine now under the avatar Spine', ma.up(os_) is spine, True)
check('outfit Chest (the PhysBone ignores it, so it is kept) moved under the avatar Chest', (ma.up(oc) is chest, id(oc) in ma.deleted), (True, False))
check('outfit Hips merged', id(oh) in ma.deleted, True)
check('outfit Spine not retargeted', id(os_) in ma.retarget, False)

print('== the same, but the chain is where the avatar\'s is: MA drops the outfit\'s copy of the PhysBone')
sc = Scene()
arm, hips, spine, chest, head = avatar(sc)
pbA = pb(sc, spine)
o = sc.go('Outfit', sc.root, (0, 0, 0))
oa = sc.go('Armature', o, (0, 0, 0))
oh = sc.go('Hips', oa, (0, 1, 0))
os_ = sc.go('Spine', oh, (0, 1.1, 0))
oc = sc.go('Chest', os_, (0, 1.3, 0))
merge_comp(sc, oa, 'Armature')
pbO = pb(sc, os_)
pbO2 = pb(sc, os_, root=oc)  # another on the same bone, for Chest: kept, and so the bone stays
ma = sc.run({'Hips': hips, 'Spine': spine, 'Chest': chest, 'Head': head})
check('the copy dropped', (id(pbO) in ma.dead, id(pbO2) in ma.dead, id(pbA) in ma.dead), (True, False, False))
check('outfit Spine kept: a live PhysBone on it', id(os_) in ma.deleted, False)
check('outfit Spine not retargeted (retained)', id(os_) in ma.retarget, False)
check('outfit Chest kept: that PhysBone roots there', id(oc) in ma.deleted, False)

print('== nested merges: the inner outfit merges into the outer outfit first')
sc = Scene()
arm, hips, spine, chest, head = avatar(sc)
o = sc.go('Outfit', sc.root, (0, 0, 0))
oa = sc.go('Armature', o, (0, 0, 0))
oh = sc.go('Hips', oa, (0, 1, 0))
ob = sc.go('Belt', oh, (0, 1, 0.1))
a2 = sc.go('Acc', sc.root, (0, 0, 0))
aa = sc.go('Armature', a2, (0, 0, 0))
ah = sc.go('Hips', aa, (0, 1, 0))
ab = sc.go('Belt', ah, (0, 1, 0.1))
buckle = sc.go('Buckle', ab, (0, 1, 0.12))
merge_comp(sc, oa, 'Armature')
merge_comp(sc, aa, 'Outfit/Armature')
ma = sc.run({'Hips': hips, 'Spine': spine, 'Chest': chest, 'Head': head})
check('Buckle ends under the outfit Belt, which is under the avatar Hips', (ma.up(buckle) is ob, ma.up(ob) is hips), (True, True))
check('both Hips merged away', (id(oh) in ma.deleted, id(ah) in ma.deleted), (True, True))

print('== Bone Proxy: the attachment modes and match scale')
sc = Scene()
arm, hips, spine, chest, head = avatar(sc)
q = Quaternion((0, 0, 1), math.radians(90))
sc.U[id(head)] = Matrix.LocRotScale(Vector((0, 1.6, 0)), q, Vector((2, 2, 2)))
p1 = sc.go('AtRoot', sc.root, (0.5, 1, 0))
p2 = sc.go('KeepWorld', sc.root, (0.5, 1, 0))
p3 = sc.go('KeepRot', sc.root, (0.5, 1, 0), Quaternion((1, 0, 0), 0.5))
p4 = sc.go('KeepPos', sc.root, (0.5, 1, 0), Quaternion((1, 0, 0), 0.5))
p5 = sc.go('Scaled', sc.root, (0.5, 1, 0))
kid = sc.go('Kid', p2, (0.6, 1, 0))
for g, mode, ms in ((p1, 1, '0'), (p2, 2, '0'), (p3, 3, '0'), (p4, 4, '0'), (p5, 1, '1')):
    sc.comp(g, {'boneReference': '10', 'subPath': '', 'attachmentMode': str(mode), 'matchScale': ms}, script=PROXY)
ma = sc.run({'Hips': hips, 'Spine': spine, 'Chest': chest, 'Head': head})
U = ma.U
near('AtRoot at the Head', U[id(p1)].translation, (0, 1.6, 0))
near('AtRoot turned as the Head', U[id(p1)].to_quaternion() @ Vector((1, 0, 0)), q @ Vector((1, 0, 0)))
check('AtRoot keeps its world scale', tuple(round(x, 4) for x in U[id(p1)].to_scale()), (1.0, 1.0, 1.0))
near('KeepWorld stays', U[id(p2)].translation, (0.5, 1, 0))
near('its child stays too', U[id(kid)].translation, (0.6, 1, 0))
near('KeepRot at the Head', U[id(p3)].translation, (0, 1.6, 0))
near('KeepRot keeps its turn', U[id(p3)].to_quaternion() @ Vector((0, 1, 0)), Quaternion((1, 0, 0), 0.5) @ Vector((0, 1, 0)))
near('KeepPos stays', U[id(p4)].translation, (0.5, 1, 0))
near('KeepPos turned as the Head', U[id(p4)].to_quaternion() @ Vector((1, 0, 0)), q @ Vector((1, 0, 0)))
check('match scale: the Head\'s', tuple(round(x, 4) for x in U[id(p5)].to_scale()), (2.0, 2.0, 2.0))
check('names under the Head', names(ma, head), ['AtRoot', 'KeepWorld', 'KeepRot', 'KeepPos', 'Scaled'])

print('== Bone Proxy names: one already there gets " (1)"; a path target; $$AVATAR; no target')
sc = Scene()
arm, hips, spine, chest, head = avatar(sc)
sc.go('Hat', head, (0, 1.7, 0))
hat = sc.go('Hat', sc.root, (0, 1.8, 0))
sc.comp(hat, {'boneReference': '10', 'subPath': '', 'attachmentMode': '2', 'matchScale': '0'}, script=PROXY)
pp = sc.go('ByPath', sc.root, (0, 0, 0))
sc.comp(pp, {'boneReference': '55', 'subPath': 'Armature/Hips/Spine', 'attachmentMode': '1'}, script=PROXY)
pa = sc.go('ToRoot', hips, (0, 1, 0))
sc.comp(pa, {'boneReference': '55', 'subPath': '$$AVATAR', 'attachmentMode': '2'}, script=PROXY)
pn = sc.go('Lost', chest, (0.1, 1.3, 0))
sc.comp(pn, {'boneReference': '55', 'subPath': 'No/Such', 'attachmentMode': '1'}, script=PROXY)
ma = sc.run({'Hips': hips, 'Spine': spine, 'Chest': chest, 'Head': head})
check('renamed', ma.nm[id(hat)], 'Hat (1)')
check('by path under Spine', ma.up(pp) is spine, True)
check('to the avatar root', ma.up(pa) is sc.root, True)
check('no target: stays under Chest', ma.up(pn) is chest, True)
near('... but AtRoot still zeroes it (MA does that for every proxy)', ma.U[id(pn)].translation, (0, 1.3, 0))

print('== Move To: position, rotation, scale; no reparenting')
sc = Scene()
arm, hips, spine, chest, head = avatar(sc)
sc.U[id(head)] = Matrix.LocRotScale(Vector((0, 1.6, 0)), Quaternion((0, 1, 0), 1.0), Vector((3, 3, 3)))
m = sc.go('Mover', sc.root, (1, 1, 1))
mk = sc.go('MoverKid', m, (1, 1.5, 1))
sc.comp(m, {'target': {'referencePath': 'Armature/Hips/Spine/Chest/Head', 'targetObject': None},
            'matchPosition': '1', 'matchRotation': '0', 'matchScale': '1'}, script=MOVETO)
ma = sc.run({'Hips': hips})
near('moved to the Head', ma.U[id(m)].translation, (0, 1.6, 0))
near('its child with it, scaled too', ma.U[id(mk)].translation, (0, 1.6 + 3 * 0.5, 0))
check('not turned', tuple(round(x, 4) for x in ma.U[id(m)].to_quaternion()), (1.0, 0.0, 0.0, 0.0))
check('scaled as the Head', tuple(round(x, 4) for x in ma.U[id(m)].to_scale()), (3.0, 3.0, 3.0))
check('parent unchanged', ma.up(m) is sc.root, True)

print('== PhysBone Blocker placed by hand; the empty "Armature" beside the real one')
sc = Scene()
fake = sc.go('Armature', sc.root, (0, 0, 0))
arm, hips, spine, chest, head = avatar(sc)
t = sc.go('Tip', chest, (0, 1.4, 0.1))
sc.comp(t, {}, script=BLOCKER)
o = sc.go('Outfit', sc.root, (0, 0, 0))
oa = sc.go('Armature', o, (0, 0, 0))
oh = sc.go('Hips', oa, (0, 1, 0))
merge_comp(sc, oa, 'Armature')
ma = sc.run({'Hips': hips})
check('merged into the real Armature', ma.retarget.get(id(oa)) is arm, True)
check('blocks at Chest, Spine, Hips, Armature, root', [ma.nm[id(x)] for x in (chest, spine, hips, arm, sc.root)
                                                        if t in ma.blocks.get(id(x), [])],
      ['Chest', 'Spine', 'Hips', 'Armature', 'Avatar'])
check('not at Tip itself', t in ma.blocks.get(id(t), []), False)

print('== a merge that targets nothing, and one into itself')
sc = Scene()
arm, hips, spine, chest, head = avatar(sc)
o = sc.go('Outfit', sc.root, (0, 0, 0))
oa = sc.go('Armature', o, (0, 0, 0))
merge_comp(sc, oa, 'Nope')
o2 = sc.go('Loop', sc.root, (0, 0, 0))
l2 = sc.go('Inner', o2, (0, 0, 0))
merge_comp(sc, o2, 'Loop/Inner')
u.WARNINGS.clear()
ma = sc.run({'Hips': hips})
check('warnings', [w for w in u.WARNINGS], ['Armature: its Merge Armature has no target in the avatar, so it is not merged',
                                           'the Merge Armatures of Loop merge into each other',
                                           'Loop cannot go under Inner, which is under it'])

print('== Setup Outfit: VRM names, an UpperChest the avatar lacks, a bone of its own')
sc = Scene()
arm, hips, spine, chest, head = avatar(sc)
neck = sc.go('Neck', chest, (0, 1.45, 0))
head.parent.children.remove(head)
head.parent = neck
neck.children.append(head)
sh = sc.go('Shoulder_L', chest, (0.05, 1.4, 0))
ua = sc.go('UpperArm_L', sh, (0.15, 1.4, 0))
o = sc.go('OutfitVRM', sc.root, (0, 0, 0))
oa = sc.go('Armature', o, (0, 0, 0))
b = {}
for name, parent, pos in (('J_Bip_C_Hips', oa, (0, 1, 0)), ('J_Bip_C_Spine', 'J_Bip_C_Hips', (0, 1.1, 0)),
                          ('J_Bip_C_Chest', 'J_Bip_C_Spine', (0, 1.3, 0)), ('J_Bip_C_UpperChest', 'J_Bip_C_Chest', (0, 1.38, 0)),
                          ('J_Bip_C_Neck', 'J_Bip_C_UpperChest', (0, 1.45, 0)), ('J_Bip_C_Head', 'J_Bip_C_Neck', (0, 1.6, 0)),
                          ('J_Bip_L_Shoulder', 'J_Bip_C_UpperChest', (0.05, 1.4, 0)),
                          ('J_Bip_L_UpperArm', 'J_Bip_L_Shoulder', (0.15, 1.4, 0)), ('Ribbon', 'J_Bip_C_Neck', (0, 1.45, 0.1))):
    b[name] = sc.go(name, b.get(parent, parent) if isinstance(parent, str) else parent, pos)
human = {'Hips': hips, 'Spine': spine, 'Chest': chest, 'Neck': neck, 'Head': head, 'LeftShoulder': sh, 'LeftUpperArm': ua}
order = []


def walk(g):
    order.append(g)
    for c in g.children:
        walk(c)
walk(sc.root)


class AV:
    pass
av = AV()
av.root, av.gos, av.inside = sc.root, order, set(map(id, order))
av.renderers = []
setup = u.OutfitSetup(None, av, human, o)
check('specs', [(u.go_name(m.go), u.go_name(m.target), m.prefix, m.suffix, m.mangle) for m in setup.specs],
      [('Armature', 'Armature', 'J_Bip_', '', True), ('J_Bip_C_UpperChest', 'Chest', 'J_Bip_', '', False)])
check('renamed', [u.go_name(b[n]) for n in ('J_Bip_C_Hips', 'J_Bip_C_Neck', 'J_Bip_L_Shoulder', 'J_Bip_L_UpperArm', 'Ribbon')],
      ['J_Bip_Hips', 'J_Bip_Neck', 'J_Bip_Shoulder_L', 'J_Bip_UpperArm_L', 'Ribbon'])
check('matched', sorted((u.go_name(k), u.go_name(v)) for k, v in setup.pairs),
      sorted([('J_Bip_Hips', 'Hips'), ('J_Bip_Spine', 'Spine'), ('J_Bip_Chest', 'Chest'), ('J_Bip_Neck', 'Neck'),
              ('J_Bip_Head', 'Head'), ('J_Bip_Shoulder_L', 'Shoulder_L'), ('J_Bip_UpperArm_L', 'UpperArm_L')]))
ma = u.ModularAvatar(av, human, sc.U, setup.specs)
check('the UpperChest merged into the Chest', (id(b['J_Bip_C_UpperChest']) in ma.deleted,
                                              ma.retarget.get(id(b['J_Bip_C_UpperChest'])) is chest), (True, True))
check('Ribbon under the avatar Neck', ma.up(b['Ribbon']) is neck, True)
check('every outfit bone but Ribbon gone', sorted(u.go_name(ma.byid[k]) for k in ma.deleted),
      sorted(['Armature', 'J_Bip_Hips', 'J_Bip_Spine', 'J_Bip_Chest', 'J_Bip_C_UpperChest', 'J_Bip_Neck', 'J_Bip_Head',
              'J_Bip_Shoulder_L', 'J_Bip_UpperArm_L']))

print('== Setup Outfit: an A-pose arm turned; one that does not start where the avatar\'s does left alone')
sc = Scene()
arm, hips, spine, chest, head = avatar(sc)
sh = sc.go('Shoulder_L', chest, (0.05, 1.4, 0))
ua = sc.go('UpperArm_L', sh, (0.15, 1.4, 0))
la = sc.go('LowerArm_L', ua, (0.45, 1.4, 0))
sr = sc.go('Shoulder_R', chest, (-0.05, 1.4, 0))
ur = sc.go('UpperArm_R', sr, (-0.15, 1.4, 0))
lr = sc.go('LowerArm_R', ur, (-0.45, 1.4, 0))
o = sc.go('Outfit', sc.root, (0, 0, 0))
oa = sc.go('Armature', o, (0, 0, 0))
oh = sc.go('Hips', oa, (0, 1, 0))
osp = sc.go('Spine', oh, (0, 1.1, 0))
oc = sc.go('Chest', osp, (0, 1.3, 0))
osh = sc.go('Shoulder_L', oc, (0.05, 1.4, 0))
c, s_ = math.cos(math.radians(35)), math.sin(math.radians(35))
oua = sc.go('UpperArm_L', osh, (0.15, 1.4, 0))
ola = sc.go('LowerArm_L', oua, (0.15 + 0.3 * c, 1.4 - 0.3 * s_, 0))
osr = sc.go('Shoulder_R', oc, (-0.05, 1.4, 0))
our = sc.go('UpperArm_R', osr, (-0.17, 1.4, 0))  # 2 cm off
olr = sc.go('LowerArm_R', our, (-0.17 - 0.3 * c, 1.4 - 0.3 * s_, 0))
human = {'Hips': hips, 'Spine': spine, 'Chest': chest, 'Head': head, 'LeftShoulder': sh, 'LeftUpperArm': ua,
         'LeftLowerArm': la, 'RightShoulder': sr, 'RightUpperArm': ur, 'RightLowerArm': lr}
order = []
walk(sc.root)
av = AV()
av.root, av.gos, av.inside = sc.root, order, set(map(id, order))
av.renderers = []
u.WARNINGS.clear()
setup = u.OutfitSetup(None, av, human, o)
setup.fix(sc.U)
near('left lower arm turned onto the avatar\'s', sc.U[id(ola)].translation, (0.45, 1.4, 0))
near('right one left alone', sc.U[id(olr)].translation, (-0.17 - 0.3 * c, 1.4 - 0.3 * s_, 0))
check('the misfit warned about', [w.split(' (')[0] for w in u.WARNINGS],
      ['Outfit: 2 of its bones are more than 1 cm from the avatar\'s they merge into'])

print('== MA menu items: the parameters and values MA gives them')
ITEM, TOGGLE = '3b29d45007c5493d926d2cd45a489529', 'a162bb8ec7e24a5abcf457887f1df3fa'
sc = Scene()
arm, hips, spine, chest, head = avatar(sc)


def mitem(g, param='', value=1, default=0, auto=1, kind=102):
    return sc.comp(g, {'Control': {'type': str(kind), 'parameter': {'name': param}, 'value': str(value)},
                       'isDefault': str(default), 'automaticValue': str(auto), 'label': ''}, script=ITEM)


def otoggle(g, target):
    return sc.comp(g, {'m_inverted': '0', 'm_objects': [{'Object': {'referencePath': u.go_name(target),
                                                                    'targetObject': target}, 'Active': '1'}]},
                   script=TOGGLE)
things = [sc.go('Thing%d' % i, sc.root, (0, 0, 0)) for i in range(8)]
a1, a2 = sc.go('Toggle', sc.root, (0, 0, 0)), sc.go('Toggle', things[0], (0, 0, 0))  # one name: one parameter
i1, i2 = mitem(a1), mitem(a2)
otoggle(a1, things[1])
otoggle(a2, things[2])
b = sc.go('Solo', sc.root, (0, 0, 0))
i3 = mitem(b, default=1)
otoggle(b, things[3])
c1, c2 = sc.go('C1', sc.root, (0, 0, 0)), sc.go('C2', sc.root, (0, 0, 0))
i4, i5 = mitem(c1, 'Declared', default=1), mitem(c2, 'Declared')
d1, d2 = sc.go('D1', sc.root, (0, 0, 0)), sc.go('D2', sc.root, (0, 0, 0))
i6, i7 = mitem(d1, 'Fixed', 2, auto=0), mitem(d2, 'Fixed', 3, default=1, auto=0)
e1 = sc.go('E1', sc.root, (0, 0, 0))
i8 = mitem(e1, 'BoolParam')
f1 = sc.go('F1', sc.root, (0, 0, 0))
i9 = mitem(f1)  # no reactive component, no parameter: nothing
order = []
walk(sc.root)


class AN:
    pass
an = AN()
an.av = AV()
an.av.root, an.av.gos, an.av.inside, an.av.renderers = sc.root, order, set(map(id, order)), []
an.av.desc = {}
an.av.default = lambda p: 1.0 if u.truthy(p[1].data.get('m_IsActive', '1')) else 0.0
an.db = None
an.eparams = {'Declared': (0, 3.0), 'BoolParam': (2, 0.0)}
comps = {}
for g in order:
    for c in g.comps:
        k = u.ma_kind(c)
        if k:
            comps.setdefault(k, []).append(c)
mt = u.MAToggles(an, comps)
check('two "Toggle" items share one parameter (values in hierarchy order: a2 comes first)', (mt.item[id(i2)], mt.item[id(i1)]),
      (('__MA/AutoParam/Toggle', 1.0), ('__MA/AutoParam/Toggle', 2.0)))
check('... an Int, 0 by default', mt.declared['__MA/AutoParam/Toggle'], (0, 0.0))
check('one default item alone: 1, on by default', (mt.item[id(i4 if False else i3)], mt.declared['__MA/AutoParam/Solo']),
      (('__MA/AutoParam/Solo', 1.0), (2, 1.0)))
check('a declared Int with default 3: the default item takes 3, the other 1', (mt.item[id(i4)][1], mt.item[id(i5)][1]), (3.0, 1.0))
check('fixed values kept', (mt.item[id(i6)][1], mt.item[id(i7)][1]), (2.0, 3.0))
check('... and the default is the default item\'s', mt.declared['Fixed'], (0, 3.0))
check('a declared Bool: value 1', mt.item[id(i8)][1], 1.0)
check('an item with nothing to do gets no parameter', id(i9) in mt.item, False)
check('the rules', sorted((u.go_name(k[1]), [c[0] for c in r[0][1]]) for k, r in mt.rules.items()),
      [('Thing1', ['p', 'a']), ('Thing2', ['p', 'a', 'a']), ('Thing3', ['p', 'a'])])
vals = mt.apply({'__MA/AutoParam/Toggle': 2.0, '__MA/AutoParam/Solo': 0.0}, {})
check('Toggle = 2 shows Thing1 only; Solo off leaves Thing3 as it is', sorted(u.go_name(k[1]) for k in vals), ['Thing1'])
vals = mt.apply({'__MA/AutoParam/Toggle': 1.0}, {('a', things[0]): 0.0})
check('... but not while Thing0 above it is hidden', sorted(u.go_name(k[1]) for k in vals if k[1] is not things[0]), [])

print('== Replace Object: the replacement takes the place of its target, and what named the target names it')
REPLACE = '7e949680c0864ee7b441d9b2c93b890b'
sc = Scene()
arm, hips, spine, chest, head = avatar(sc)
hair = sc.go('Hair', head, (0, 1.7, 0))
tip = sc.go('HairTip', hair, (0, 1.75, 0))
bangs = sc.go('Bangs', head, (0, 1.65, 0.05))
ring = sc.go('Ring', head, (0, 1.8, 0))
hsmr = sc.comp(hair, {'m_Enabled': '1', 'm_Bones': [hair.tf]}, cls=137)
acc = sc.go('Acc', sc.root, (0, 0, 0))
newhair = sc.go('NewHair', acc, (0, 1.72, 0))
nsmr = sc.comp(newhair, {'m_Enabled': '1'}, cls=137)
body = sc.go('Body', sc.root, (0, 0, 0))
bsmr = sc.comp(body, {'m_Enabled': '1', 'm_Bones': [hair.tf, head.tf], 'm_RootBone': hair.tf}, cls=137)
pbc = pb(sc, head, root=hair)
sc.comp(newhair, {'targetObject': {'referencePath': 'Armature/Hips/Spine/Chest/Head/Hair', 'targetObject': None}},
        script=REPLACE)
under = sc.go('Under', ring, (0, 1.8, 0))
sc.comp(under, {'targetObject': {'referencePath': 'Armature/Hips/Spine/Chest/Head/Ring', 'targetObject': ring}},
        script=REPLACE)
before = len(u.WARNINGS)
ma = sc.run({'Hips': hips, 'Spine': spine, 'Chest': chest, 'Head': head, 'Jaw': hair})
check('the replacement is where its target was among its siblings', names(ma, head), ['NewHair', 'Bangs', 'Ring'])
check('the target\'s children are under it', ma.up(tip) is newhair, True)
check('the target goes', (id(hair) in ma.deleted, ma.replaced.get(id(hair)) is newhair), (True, True))
check('its renderer\'s place goes to the replacement\'s', ma.replaced.get(id(hsmr)) is nsmr, True)
check('meshes weighted to it: the replacement, bound where it is', (ma.retarget[id(hair)] is newhair,
                                                                   ma.bound[id(hair)] == ma.U[id(newhair)]), (True, True))
check('references to it and its components now name the replacement\'s',
      (bsmr.data['m_Bones'][0] is newhair.tf, bsmr.data['m_RootBone'] is newhair.tf, pbc.data['rootTransform'] is
       newhair.tf), (True, True, True))
check('the humanoid map follows', ma.human['Jaw'] is newhair, True)
check('animations of the target reach the replacement', (ma.swap(('a', hair))[1] is newhair,
                                                       ma.swap(('e', hsmr))[1] is nsmr, ma.swap(('a', bangs))[1] is bangs),
      (True, True, True))
check('one under its own target is refused, and said', (ma.up(under) is ring, any('Under' in w for w in u.WARNINGS[before:])),
      (True, True))
near('where the world has the replacement is kept', ma.U[id(newhair)].translation, (0, 1.72, 0))

print('== Blendshape Sync: a mesh\'s shape keys follow another\'s, straight or through a remap curve')
check('remap: straight lines, and on past the ends', [u.remap(v, [(0, 0), (50, 80), (100, 100)]) for v in (25, 50, 75,
                                                                                                       150, -10)],
      [40.0, 80, 90.0, 120.0, -16.0])
check('remap without a curve', u.remap(33.0, None), 33.0)
SYNC = '6fd7cab7d93b403280f2f9da978d8a4f'
sc = Scene()
face = sc.go('Face', sc.root, (0, 0, 0))
fsmr = sc.comp(face, {'m_Enabled': '1', 'm_BlendShapeWeights': ['40', '0']}, cls=137)
mask = sc.go('Mask', sc.root, (0, 0, 0))
msmr = sc.comp(mask, {'m_Enabled': '1', 'm_BlendShapeWeights': ['0', '0', '0']}, cls=137)
curve = {'m_Curve': [{'time': '0', 'value': '0'}, {'time': '100', 'value': '50'}]}
sc.comp(mask, {'Bindings': [
    {'ReferenceMesh': {'referencePath': 'Face', 'targetObject': None}, 'Blendshape': 'Smile', 'LocalBlendshape': '',
     'RemapCurveIsValid': '0'},
    {'ReferenceMesh': {'referencePath': 'Face', 'targetObject': None}, 'Blendshape': 'Angry',
     'LocalBlendshape': 'Grr', 'RemapCurveIsValid': '1', 'RemapCurve': curve},
    {'ReferenceMesh': {'referencePath': 'Face', 'targetObject': None}, 'Blendshape': 'Missing'}]}, script=SYNC)
order = []
walk(sc.root)
an = AN()
an.av = AV()
an.av.root, an.av.gos, an.av.inside = sc.root, order, set(map(id, order))
an.av.renderers = [fsmr, msmr]
names_of = {id(fsmr): ['Smile', 'Angry'], id(msmr): ['Smile', 'Grr', 'Other']}
an.av.shape_names = lambda r: names_of[id(r)]
an.av.default = lambda p: u.Avatar.default(an.av, p)
an.db, an.eparams = None, {}
mt = u.MAToggles(an, {'BlendshapeSync': [c for c in mask.comps if u.ma_kind(c) == 'BlendshapeSync']})
check('the bindings: the local name, else the same; a missing shape key left out',
      [(a, b, p) for s, a, d, b, p in mt.syncs], [('Smile', 'Smile', None), ('Angry', 'Grr', [(0.0, 0.0), (100.0, 50.0)])])
check('the followers rest as their sources do (remapped)', msmr.data['m_BlendShapeWeights'][:2], [40.0, 0.0])
vals = mt.apply({}, {('s', fsmr, 'Angry'): 100.0, ('s', fsmr, 'Smile'): 20.0})
check('what animates the source animates them', (vals[('s', msmr, 'Smile')], vals[('s', msmr, 'Grr')]), (20.0, 50.0))

print('== Material Setter and Material Swap: rules that put materials in renderers\' slots')
SETTER, SWAP = '0adf335711644e34b6c635e94ae61fa7', 'b259b73280ead4e4fbbdafc5e29175d1'
RED, BLUE, CLOTH = ({'fileID': '2100000', 'guid': c * 32, 'type': '2'} for c in 'abc')
sc = Scene()
hat = sc.go('Hat', sc.root, (0, 0, 0))
hmr = sc.comp(hat, {'m_Enabled': '1', 'm_Materials': [CLOTH, RED]}, cls=23)
shirt = sc.go('Shirt', sc.root, (0, 0, 0))
smr2 = sc.comp(shirt, {'m_Enabled': '1', 'm_Materials': [CLOTH]}, cls=137)
outfit = sc.go('Outfit', sc.root, (0, 0, 0))
omr = sc.comp(sc.go('Coat', outfit, (0, 0, 0)), {'m_Enabled': '1', 'm_Materials': [CLOTH]}, cls=137)
item = sc.go('Blue hat', sc.root, (0, 0, 0))
mi = mitem(item)
sc.comp(item, {'m_inverted': '0', 'm_objects': [{'Object': {'referencePath': 'Hat', 'targetObject': None},
                                                 'Material': BLUE, 'MaterialIndex': '1'},
                                                {'Object': {'referencePath': 'Hat', 'targetObject': None},
                                                 'Material': BLUE, 'MaterialIndex': '5'}]}, script=SETTER)
sw = sc.go('Recolour', sc.root, (0, 0, 0))
sc.comp(sw, {'m_inverted': '0', 'm_root': {'referencePath': 'Outfit', 'targetObject': None},
             'm_swaps': [{'From': CLOTH, 'To': RED}]}, script=SWAP)
order = []
walk(sc.root)
an = AN()
an.av = AV()
an.av.root, an.av.gos, an.av.inside = sc.root, order, set(map(id, order))
an.av.renderers = [hmr, smr2, omr]
an.av.default = lambda p: u.Avatar.default(an.av, p)
an.db, an.eparams = None, {}
comps = {}
for g in order:
    for c in g.comps:
        if u.ma_kind(c):
            comps.setdefault(u.ma_kind(c), []).append(c)
mt = u.MAToggles(an, comps)
check('the setter\'s slot (a slot the renderer lacks left out), the swap under its root only',
      sorted((u.go_name(k[1].go), k[2]) for k in mt.rules if k[0] == 'm'), [('Coat', 0), ('Hat', 1)])
pn = mt.item[id(mi)][0]
vals = mt.apply({pn: 1.0}, {})
check('with its menu item on: the blue hat, and the coat swapped (no menu item: always)',
      (vals.get(('m', hmr, 1)), vals.get(('m', omr, 0)), ('m', smr2, 0) in vals), (('b' * 32, 2100000), ('a' * 32, 2100000),
                                                                                   False))
check('with it off: only the swap', sorted(k[1].go.data['m_Name'] for k in mt.apply({pn: 0.0}, {})), ['Coat'])

print('== Platform Filter: what is not for VRChat is left out')
sc = Scene()
PF = '8c8a67d5c01849629fa90c3b2eded93f'
objs = {}
for name, filters in (('resonite only', [(0, 'nadena.dev.ndmf.resonite')]),
                      ('not on resonite', [(1, 'nadena.dev.ndmf.resonite')]),
                      ('vrchat or resonite', [(0, 'nadena.dev.ndmf.resonite'), (0, u.NDMF_VRCHAT)]),
                      ('not on vrchat', [(1, u.NDMF_VRCHAT)]), ('no filter', [])):
    objs[name] = g = sc.go(name, sc.root, (0, 0, 0))
    for exclude, platform in filters:
        sc.comp(g, {'m_excludePlatform': str(exclude), 'm_platform': platform}, script=PF)
out = u.ma_platform_out(sc.gos)
check('left out', sorted(n for n, g in objs.items() if id(g) in out), ['not on vrchat', 'resonite only'])


print('== Scale Adjuster: the meshes weighted to its bone scale in the bone\'s axes; its children and others do not')
import numpy as np
from types import SimpleNamespace as NS
SCALEADJ = '09a660aa9d4e47d992adcac5a05dd808'
FLIP = u.FLIP


def gltf_nodes(sc, gos):
    """GLB nodes for these GameObjects, where Unity has them (glTF = FLIP @ Unity @ FLIP), named as they are"""
    js = {'nodes': [], 'scenes': [{'nodes': []}], 'scene': 0, 'skins': [], 'accessors': [], 'bufferViews': [],
          'buffers': [{'byteLength': 0}]}
    at = {}
    for g in gos:
        g.name = u.go_name(g)
        at[id(g)] = len(js['nodes'])
        par = g.parent if g.parent is not None and id(g.parent) in at else None
        W = FLIP @ sc.U[id(g)] @ FLIP
        L = (FLIP @ sc.U[id(par)] @ FLIP).inverted() @ W if par is not None else W
        js['nodes'].append({'name': g.name, 'matrix': [L[r][c] for c in range(4) for r in range(4)]})
        if par is None:
            js['scenes'][0]['nodes'].append(at[id(g)])
        else:
            js['nodes'][at[id(par)]].setdefault('children', []).append(at[id(g)])
    return js, at


def add_ibm(js, binc, mats):
    data = b''.join(np.array([[M[r][c] for c in range(4) for r in range(4)] for M in mats], '<f4').tobytes() for _ in [0])
    off = len(binc)
    js['bufferViews'].append({'buffer': 0, 'byteOffset': off, 'byteLength': len(data)})
    js['accessors'].append({'bufferView': len(js['bufferViews']) - 1, 'componentType': 5126, 'count': len(mats),
                            'type': 'MAT4'})
    js['buffers'][0]['byteLength'] = off + len(data)
    return len(js['accessors']) - 1, binc + data


sc = Scene()
arm, hips, spine, chest, head = avatar(sc)
rot = Quaternion((0, 0, 1), math.radians(30))
cuff = sc.go('Cuff', hips, (0.1, 0.8, 0), rot)
kid = sc.go('CuffKid', cuff, (0.1, 0.9, 0), rot)
lone = sc.go('Lone', hips, (0, 0.9, 0))
sc.comp(cuff, {'m_Scale': {'x': '1.2', 'y': '1', 'z': '0.5'}}, script=SCALEADJ)
sc.comp(lone, {'m_Scale': {'x': '2', 'y': '2', 'z': '2'}}, script=SCALEADJ)
ma = sc.run({'Hips': hips, 'Spine': spine, 'Chest': chest, 'Head': head})
check('the adjusted bones', sorted(u.go_name(ma.byid[k]) for k in ma.scales), ['Cuff', 'Lone'])
order = []
walk(sc.root)
js, at = gltf_nodes(sc, order)
_, W = u.gltf_tree(js)
joints = [at[id(cuff)], at[id(kid)], at[id(hips)]]
ibm0 = [W[j].inverted() for j in joints]
acc, binc = add_ibm(js, b'', ibm0)
js['skins'].append({'joints': list(joints), 'inverseBindMatrices': acc})
binc2 = ma.apply(js, binc)
ibm = u.ModularAvatar.read_mat4(js, binc2, js['skins'][0]['inverseBindMatrices'], 3)
p_local = Vector((0.1, 0.05, 0.2))  # a point in the cuff's own (Unity) axes
got = W[joints[0]] @ ibm[0] @ (FLIP @ (sc.U[id(cuff)] @ p_local))
near('a vertex weighted to it: scaled (1.2, 1, 0.5) about the bone, in its axes', got,
     FLIP @ (sc.U[id(cuff)] @ Vector((0.12, 0.05, 0.1))))
near('one weighted to its child: where it was', W[joints[1]] @ ibm[1] @ Vector((0.3, 0.2, 0.1)), (0.3, 0.2, 0.1))
check('the other joints\' bind matrices are as they were', [(ibm[k] - ibm0[k]).to_quaternion().angle < 1e-6 and
                                                          max(abs(x) for r in (ibm[k] - ibm0[k]) for x in r) < 1e-6
                                                          for k in (1, 2)], [True, True])
sc = Scene()
arm, hips, spine, chest, head = avatar(sc)
lone = sc.go('Lone', hips, (0, 0.9, 0))
sc.comp(lone, {'m_Scale': {'x': '2', 'y': '2', 'z': '2'}}, script=SCALEADJ)
ma = sc.run({'Hips': hips, 'Spine': spine, 'Chest': chest, 'Head': head})
order = []
walk(sc.root)
js, at = gltf_nodes(sc, order)
acc, binc = add_ibm(js, b'', [Matrix.Identity(4)])
js['skins'].append({'joints': [at[id(hips)]], 'inverseBindMatrices': acc, 'skeleton': at[id(hips)]})
check('one on a bone no mesh is weighted to changes nothing (the GLB is left as it is)',
      (ma.apply(js, binc) is binc, 'skeleton' in js['skins'][0]), (True, True))

print('== Merge Motion (Blend Tree): its motions play at full weight, in one layer, their parameters renamed')
SMR = u.Obj(137, 'SkinnedMeshRenderer', {})
TREE, CA, CB, OTHER = ('t' * 32, 1), ('a' * 32, 2), ('b' * 32, 3), ('o' * 32, 4)


class FakeAnim:
    av = NS(default=lambda pr: 10.0)

    def cls(self, p):
        return 206 if p == TREE else 74 if p in (CA, CB, OTHER) else None

    def body(self, p):
        return {'m_BlendType': '0', 'm_BlendParameter': 'Size', 'm_BlendParameterY': 'Size',
                'm_Childs': [{'m_Motion': {'fileID': CA[1], 'guid': CA[0]}},
                             {'m_Motion': {'fileID': CB[1], 'guid': CB[0]}}]} if p == TREE else None

    def motion(self, p, params, t, over, names):
        if p == TREE:
            return {('s', SMR, 'Big'): 10.0 + 90.0 * params.get('Size', 0.0)}
        return {('s', SMR, 'Big'): 40.0}  # OTHER: 30 up from the default
fa = FakeAnim()
ml = u.MotionLayer([(fa, TREE, {'Size': 'Renamed'}), (fa, OTHER, {})])
check('the parameters its trees blend by: floats, 0 by default, as the avatar names them', ml.params,
      {'Renamed': (1, 0.0)})
check('both motions at once: their changes from the default add up (a direct blend tree)',
      ml.evaluate({'Renamed': 0.5})[0], {('s', SMR, 'Big'): 10.0 + 45.0 + 30.0})

print('== Menu Install Target: an installer\'s menu goes where an install target names it, and only there')
TARGET, INSTALLER = '1fad1419b52a42ae89b0df52eb861e47', '7ef83cb0c23d4d7c9d41021e544a1978'
sc = Scene()


def submenu(g):
    return sc.comp(g, {'Control': {'type': '103', 'parameter': {'name': ''}, 'value': '1'}, 'MenuSource': '1',
                       'isDefault': '0', 'automaticValue': '1', 'label': ''}, script=ITEM)
m = sc.go('Menu', sc.root, (0, 0, 0))
submenu(m)
sc.comp(m, {'m_Enabled': '1', 'menuToAppend': {'fileID': '0'}, 'installTargetMenu': {'fileID': '0'}}, script=INSTALLER)
ia = sc.go('A', m, (0, 0, 0))
mitem(ia, 'PA')
slot = sc.go('Slot', m, (0, 0, 0))
other = sc.go('Other', sc.root, (0, 0, 0))
submenu(other)
oi = sc.comp(other, {'m_Enabled': '1', 'menuToAppend': {'fileID': '0'}, 'installTargetMenu': {'fileID': '0'}},
             script=INSTALLER)
ib = sc.go('B', other, (0, 0, 0))
mitem(ib, 'PB')
sc.comp(slot, {'installer': oi}, script=TARGET)
ic = sc.go('C', m, (0, 0, 0))
mitem(ic, 'PC')
order = []
walk(sc.root)
an = AN()
an.av = AV()
an.av.root, an.av.gos, an.av.inside, an.av.renderers = sc.root, order, set(map(id, order)), []
an.av.desc = {}
an.db, an.eparams = None, {}
comps = {}
for g in order:
    for c in g.comps:
        if u.ma_kind(c):
            comps.setdefault(u.ma_kind(c), []).append(c)
mt = u.MAToggles(an, comps)
check('the menu: B in Other, at the install target\'s place in Menu, and not at the top as well',
      ['/'.join(p + (e['name'],)) for p, e in mt.controls()], ['Menu/A', 'Menu/Other/B', 'Menu/C'])

print('== Mesh Cutter and Shape Changer delete: the cuts, and when they are in effect')
CUTTER, AXIS, BONE, SHAPEF, CHANGER = ('762726b8618cac7419e39bdc2b572b3d', '660848d04d7443b5b6fcfb627e6be5ea',
                                       'f8e2c9a1b3d44c6d9a7e5f2c1b8d3e4f', 'da7788c69fae9ff4abae088a0dc92c5b',
                                       '2db441f589c3407bb6fb5f02ff8ab541')
sc = Scene()
body = sc.go('Body', sc.root, (0, 0, 0))
bsmr = sc.comp(body, {'m_Enabled': '1', 'm_BlendShapeWeights': ['0', '0']}, cls=137)
grp = sc.go('Group', sc.root, (0, 0, 0))
it = sc.go('Item', grp, (0, 0, 0))
mi = mitem(it)
cut = sc.go('Cut', it, (0, 0, 0))
sc.comp(cut, {'m_inverted': '0', 'm_object': {'referencePath': 'Body', 'targetObject': None}, 'm_multiMode': '0'},
        script=CUTTER)
sc.comp(cut, {'m_center': {'x': '0', 'y': '0.5', 'z': '0'}, 'm_axis': {'x': '0', 'y': '-1', 'z': '0'},
              'm_selectionMode': '1'}, script=AXIS)
sc.comp(cut, {'m_bone': {'referencePath': 'Body', 'targetObject': None}, 'm_threshold': '0.3', 'm_selectionMode': '2'},
        script=BONE)
under = sc.go('UnderBody', body, (0, 0, 0))  # a cutter under its own mesh: the mesh's being on is no condition
sc.comp(under, {'m_inverted': '1', 'm_object': {'referencePath': 'Body', 'targetObject': None}}, script=CUTTER)
sc.comp(under, {'m_shapes': ['Shrink'], 'm_threshold': '0.002', 'm_selectionMode': '2'}, script=SHAPEF)
sc.comp(under, {'m_shapes': ['Shrink']}, script=SHAPEF)  # the same filter twice: one key
nof = sc.go('NoFilters', sc.root, (0, 0, 0))
sc.comp(nof, {'m_object': {'referencePath': 'Body', 'targetObject': None}}, script=CUTTER)
for th in ('0.05', '0.02'):
    sc.comp(sc.go('Del' + th, sc.root, (0, 0, 0)), {'m_inverted': '0', 'm_threshold': th, 'm_shapes': [
        {'Object': {'referencePath': 'Body', 'targetObject': None}, 'ShapeName': 'Shrink', 'ChangeType': '0',
         'Value': '0'}]}, script=CHANGER)
order = []
walk(sc.root)
an = AN()
an.av = AV()
an.av.root, an.av.gos, an.av.inside = sc.root, order, set(map(id, order))
an.av.renderers = [bsmr]
an.av.shape_names = lambda r: ['Shrink', 'Other']
an.av.default = lambda p: u.Avatar.default(an.av, p)
an.db, an.eparams = None, {}
comps = {}
for g in order:
    for c in g.comps:
        if u.ma_kind(c):
            comps.setdefault(u.ma_kind(c), []).append(c)
mt = u.MAToggles(an, comps)
cuts = {k[2][0]: v for k, v in mt.rules.items() if k[0] == 'c'}
check('the cuts: the two cutters with filters and the Shape Changers\' delete (no shape key set)',
      (sorted(cuts), any(k[0] == 's' for k in mt.rules)), (['cutter', 'shape'], False))
ck = next(k for k in mt.rules if k[0] == 'c' and k[2][0] == 'cutter' and k[2][1] == 0)
check('a cutter\'s conditions: its menu item, and its objects up to the root', [
    c[0] if c[0] == 'p' else u.go_name(c[1]) for c in mt.rules[ck][0][1]], ['Cut', 'p', 'Item', 'Group'])
uk = next(k for k in mt.rules if k[0] == 'c' and k[2][0] == 'cutter' and k[2][1] != 0 or (
    k[0] == 'c' and k[2][0] == 'cutter' and len(k[2][2]) == 2 and k is not ck))
f = mt.cutters[(id(bsmr), ck[2])]['filters']
check('its filters: by axis (all corners), by bone (a centroid is taken as any corner)',
      [(x['kind'], x['mode']) for x in f], [('axis', 1), ('bone', 0)])
under_key = next(k for k in mt.rules if k[0] == 'c' and k is not ck and k[2][0] == 'cutter')
check('one under its mesh: not conditional on the mesh or what is above it; inverted', (
    [u.go_name(c[1]) for c in mt.rules[under_key][0][1]], mt.rules[under_key][0][2]), (['UnderBody'], True))
check('... its two filters are the same selector; a mode or threshold makes another', len(
    mt.cutters[(id(bsmr), under_key[2])]['filters']), 2)
sk = next(k for k in mt.rules if k[0] == 'c' and k[2][0] == 'shape')
check('the Shape Changers\' delete: one cut, at the smaller threshold', (
    len(mt.rules[sk]), mt.cutters[(id(bsmr), sk[2])]['filters'][0]['threshold']), (2, 0.02))
vals = mt.apply({mt.item[id(mi)][0]: 1.0}, {})
check('with the item on: its cut is in effect; the inverted one is not, the delete always', sorted(
    k[2][0] + ('+%d' % k[2][1] if k[2][0] == 'cutter' else '') for k in u.cuts_in(vals)), ['cutter+0', 'shape'])

print('== cut_meshes: what the vertex filters pick, cut away for good or made a part toggles hide')
# a 4 x 2 grid of quads in the GLB's XY plane, x = 0..4, y = 0..2, skinned to two bones (x <= 1: the first, x = 2:
# both, x >= 3: the second), with a shape key moving the top row 5 mm and the middle one 0.5 mm, UVs (x / 4, y / 2)
# as Unity has them. The renderer stands at Unity's origin, so in its space a vertex is at (-x, y, 0)


def grid_glb():
    js = {'asset': {'version': '2.0'}, 'buffers': [{'byteLength': 0}], 'bufferViews': [], 'accessors': [],
          'materials': [{'name': 'M0'}], 'scene': 0, 'scenes': [{'nodes': [0]}], 'skins': [], 'meshes': []}
    binc = bytearray()

    def acc(arr, ctype, typ, **kw):
        arr = np.ascontiguousarray(arr)
        while len(binc) % 4:
            binc.append(0)
        js['bufferViews'].append({'buffer': 0, 'byteOffset': len(binc), 'byteLength': arr.nbytes})
        binc.extend(arr.tobytes())
        js['accessors'].append(dict(bufferView=len(js['bufferViews']) - 1, componentType=ctype, count=len(arr),
                                    type=typ, **kw))
        return len(js['accessors']) - 1
    xy = [(x, y) for y in range(3) for x in range(5)]
    pos = np.array([(x, y, 0) for x, y in xy], '<f4')
    uv = np.array([(x / 4, 1 - y / 2) for x, y in xy], '<f4')
    jn = np.array([(0, 1, 0, 0)] * 15, 'u1')
    wt = np.array([(1, 0, 0, 0) if x <= 1 else (0.5, 0.5, 0, 0) if x == 2 else (0, 1, 0, 0) for x, y in xy], '<f4')
    tris = []
    for j in range(2):
        for i in range(4):
            a = j * 5 + i
            tris += [(a, a + 1, a + 6), (a, a + 6, a + 5)]
    rows = np.arange(5, 15, dtype='<u2')
    deltas = np.array([(0, 0.005 if y == 2 else 0.0005, 0) for x, y in xy[5:]], '<f4')
    # the shape key's deltas as a sparse accessor, as Blender writes them
    iv = acc(rows, 5123, 'SCALAR')
    vv = acc(deltas, 5126, 'VEC3')
    js['accessors'].append({'componentType': 5126, 'count': 15, 'type': 'VEC3', 'min': [0, 0, 0], 'max': [0, .005, 0],
                            'sparse': {'count': 10, 'indices': {'bufferView': js['accessors'][iv]['bufferView'],
                                                                'componentType': 5123},
                                       'values': {'bufferView': js['accessors'][vv]['bufferView']}}})
    tgt = len(js['accessors']) - 1
    prim = {'attributes': {'POSITION': acc(pos, 5126, 'VEC3', min=[0, 0, 0], max=[4, 2, 0]),
                           'TEXCOORD_0': acc(uv, 5126, 'VEC2'), 'JOINTS_0': acc(jn, 5121, 'VEC4'),
                           'WEIGHTS_0': acc(wt, 5126, 'VEC4')},
            'indices': acc(np.array(tris, '<u2').reshape(-1), 5123, 'SCALAR'), 'material': 0,
            'targets': [{'POSITION': tgt}]}
    ibm = acc(np.tile(np.eye(4, dtype='<f4').reshape(1, 16), (2, 1)), 5126, 'MAT4')
    js['meshes'].append({'primitives': [prim], 'extras': {'targetNames': ['Shrink']}})
    js['skins'].append({'joints': [2, 3], 'inverseBindMatrices': ibm})
    js['nodes'] = [{'name': 'Root', 'children': [1, 2, 3]}, {'name': 'Grid', 'mesh': 0, 'skin': 0},
                   {'name': 'Bone0'}, {'name': 'Bone1'}]
    js['buffers'][0]['byteLength'] = len(binc)
    return js, bytes(binc)


gsc = Scene()
grid_go, bone0, bone1 = (gsc.go(n, gsc.root, (0, 0, 0)) for n in ('Grid', 'Bone0', 'Bone1'))
for g in (gsc.root, grid_go, bone0, bone1):
    g.name = u.go_name(g)
gsc.root.name = 'Root'
grid_r = gsc.comp(grid_go, {'m_Enabled': '1'}, cls=137)
mask_img = bpy.data.images.new('mask', 4, 4, alpha=True)  # black where u >= 0.5
mask_img.pixels[:] = [v for y in range(4) for x in range(4) for v in ((0, 0, 0, 1) if x >= 2 else (1, 1, 1, 1))]
wraps = {'r' * 32: 0, 'c' * 32: 1}
fb = NS(av=NS(gos=[gsc.root, grid_go, bone0, bone1], ma=None), U={id(grid_go): Matrix.Identity(4)},
        keys={id(grid_r): {'Shrink': 'Shrink'}},
        counterpart=lambda g: ('obj', NS(material_slots=[NS(material=NS(name='M0'))])) if g is grid_go else None,
        db=NS(get=lambda guid: NS(file=guid, path=guid) if guid in wraps else None,
              importer=lambda guid: {'textureSettings': {'wrapU': wraps[guid], 'wrapV': wraps[guid]}}),
        load_image=lambda path: mask_img)


def cut(cutters, cuts0, kept):
    """run cut_meshes with these cutters {name: (multi, [filters])}, the cuts in effect at rest and per toggle (by
    name): (the triangles left, as sets of their corners' (x, y); the parts made {name: triangles}; the GLB)"""
    js, binc = grid_glb()
    fb._masks = {}
    keys = {n: ('cutter', m, n) for n, (m, fs) in cutters.items()}
    pr = lambda n: ('c', grid_r, keys[n])
    an = NS(mat=NS(cutters={(id(grid_r), keys[n]): {'r': grid_r, 'multi': m, 'filters': fs, 'labels': [n]}
                            for n, (m, fs) in cutters.items()}),
            cuts0=frozenset(map(pr, cuts0)), kept=[{'cuts': frozenset(map(pr, k))} for k in kept], sliders=[])
    out, pieces = u.cut_meshes(js, binc, fb, an)
    left, parts = set(), {}
    for p in js['meshes'][0]['primitives']:
        P = u.gltf_array(js, out, p['attributes']['POSITION'])
        t = u.gltf_array(js, out, p['indices'])[:, 0].astype(int).reshape(-1, 3)
        tris = {frozenset((int(round(P[i][0])), int(round(P[i][1]))) for i in tri) for tri in t}
        name = p.get('extras', {}).get('hypr3d_part')
        if name:
            parts[name] = tris
        else:
            left |= tris
    return left, parts, (js, out, pieces)


ALL = cut({}, [], [])[0]
cols = lambda *cs: {t for t in ALL if min(x for x, y in t) in cs}
rows_ = lambda *rs: {t for t in ALL if min(y for x, y in t) in rs}
check('the grid: 16 triangles', len(ALL), 16)
right = lambda mode: {'kind': 'axis', 'center': (-2.5, 0, 0), 'axis': (-1, 0, 0), 'mode': mode}  # x > 2.5
# the centroid: past x = 2.5 are the right column's and the triangles of the next with two corners at x = 3
for mode, gone in ((0, cols(2, 3)), (1, cols(3)), (2, cols(3) | {t for t in cols(2) if sum(x for x, y in t) / 3 > 2.5})):
    left = cut({'A': (0, [right(mode)])}, ['A'], [])[0]
    want = ALL - gone
    check('by axis, %s: %d cut away' % (('any corner', 'all corners', 'the centroid')[mode], len(ALL - want)),
          left, want)
bone = lambda th: {'kind': 'bone', 'bone': bone1, 'threshold': th, 'mode': 0}
check('by bone, 60% of the weight: the corners at x >= 3', cut({'A': (0, [bone(0.6)])}, ['A'], [])[0], ALL - cols(2, 3))
check('... 50%: x = 2 too', cut({'A': (0, [bone(0.5)])}, ['A'], [])[0], ALL - cols(1, 2, 3))
shp = lambda th: {'kind': 'shape', 'shapes': ['Shrink'], 'threshold': th, 'mode': 0}
check('by shape key, moved more than 1 mm: the top row\'s triangles', cut({'A': (0, [shp(0.001)])}, ['A'], [])[0],
      ALL - rows_(1))
check('... more than 0.1 mm: all (one empty triangle is left, so that the mesh stays one)',
      cut({'A': (0, [shp(0.0001)])}, ['A'], [])[0], {frozenset({(0, 0)})})
tile = lambda incl, inv=False: {'kind': 'uvtile', 'uv': 0, 'umin': (True, incl, 0.5), 'umax': (False, False, 1.0),
                                'vmin': (False, False, 0.0), 'vmax': (False, False, 1.0), 'invert': inv, 'mode': 1}
check('by UV tile, u >= 0.5, all corners', cut({'A': (0, [tile(True)])}, ['A'], [])[0], ALL - cols(2, 3))
check('... u > 0.5', cut({'A': (0, [tile(False)])}, ['A'], [])[0], ALL - cols(3))
check('... u >= 0.5 inverted', cut({'A': (0, [tile(True, True)])}, ['A'], [])[0], ALL - cols(0))
mask = lambda guid: {'kind': 'mask', 'slot': 0, 'texture': guid, 'white': False, 'uv': 0, 'mode': 1}
check('by mask, black, all corners, the texture repeating (u = 1 reads its first column: white)',
      cut({'A': (0, [mask('r' * 32)])}, ['A'], [])[0], ALL - cols(2))
check('... clamped (u = 1 reads its last: black)', cut({'A': (0, [mask('c' * 32)])}, ['A'], [])[0], ALL - cols(2, 3))
check('two filters, both (VertexIntersection)', cut({'A': (1, [right(0), shp(0.001)])}, ['A'], [])[0],
      ALL - (cols(2, 3) & rows_(1)))
check('... either (VertexUnion)', cut({'A': (0, [right(0), shp(0.001)])}, ['A'], [])[0],
      ALL - (cols(2, 3) | rows_(1)))
left_ = {'kind': 'axis', 'center': (-0.5, 0, 0), 'axis': (1, 0, 0), 'mode': 0}  # x < 0.5
left, parts, (js, out, pieces) = cut({'A': (0, [right(0)]), 'B': (0, [left_])}, ['A'], [['A'], ['A', 'B']])
check('A in effect in every state: cut away; B only in one: its triangles a part of their own',
      (left, parts), (cols(1), {'Grid (B)': cols(0)}))
check('... what the settings file needs to know of it', [(r is grid_r, sorted(k[2][2] for k in ks), n)
                                                        for r, ks, n in pieces], [(True, ['B'], 'Grid (B)')])
pp = next(p for p in js['meshes'][0]['primitives'] if 'extras' in p)
pa = js['accessors'][pp['attributes']['POSITION']]
check('the part has only the vertices it uses, its shape key\'s too, and the bounds of its positions',
      (pa['count'], js['accessors'][pp['targets'][0]['POSITION']]['count'], pa['min'], pa['max']),
      (6, 6, [0.0, 0.0, 0.0], [1.0, 2.0, 0.0]))
check('... its shape key moves the same vertices',
      sorted(round(float(v), 4) for v in u.gltf_array(js, out, pp['targets'][0]['POSITION'])[:, 1]),
      [0.0, 0.0, 0.0005, 0.0005, 0.005, 0.005])
js0 = grid_glb()[0]
left, parts, (js, out, pieces) = cut({'A': (0, [right(0)])}, [], [])
check('a cut never in effect changes nothing', (left, parts, js['meshes'] == js0['meshes'], pieces),
      (ALL, {}, True, []))

print('== MA Global Collider: which of the avatar\'s colliders it takes, and so whether PhysBones meet it')
GC = '49bb23f95a7baca4186efa68bc5891b6'
sc = Scene()
hand = sc.go('Hand', sc.root, (0.5, 1, 0))


def gcol(name, manual='0', target='14', low='0', copy='0'):
    return sc.comp(sc.go(name, hand, (0.5, 1, 0)), {'m_manualRemap': manual, 'm_colliderToHijack': target,
                                                     'm_lowPriority': low, 'm_copyHijackedShape': copy,
                                                     'm_radius': '0.05', 'm_height': '0.2'}, script=GC)
auto1 = gcol('auto1')
lowring = gcol('lowring', '1', '8', '1')
handr = gcol('handr', '1', '3', copy='1')
headc = gcol('headc', '1', '0')
auto2 = gcol('auto2')
st = u.Settings.__new__(u.Settings)
st.ma = NS(comps={'GlobalCollider': [auto1, lowring, handr, headc, auto2]})
st.av = NS(desc={'collider_handR': {'radius': '0.07', 'height': '0.3'}})
got = st.global_colliders()
check('manual ones first (a low priority one first of them), then the others the ring, middle, little, index fingers '
      'left; a head one does not count', [u.go_name(c.go) for c in got], ['handr', 'auto2', 'lowring', 'auto1'])
check('one that copies the shape of the collider it takes', (got[0].data['m_radius'], got[0].data['m_height']),
      ('0.07', '0.3'))

print('== MA Floor Adjuster: the height the active one is at')
FLOOR = 'ba18e6eae93342fd8774b3f3f132928a'
sc = Scene()
arm, hips, spine, chest, head = avatar(sc)
f1 = sc.go('Floor', sc.root, (0, -0.03, 0))
f2 = sc.go('Off', sc.root, (0, 0.2, 0))
f2.data['m_IsActive'] = '0'
fc1, fc2 = sc.comp(f1, {}, script=FLOOR), sc.comp(f2, {}, script=FLOOR)
ma = sc.run({'Hips': hips})
order = []
walk(sc.root)
st = u.Settings.__new__(u.Settings)
st.ma, st.human, st.b = ma, {'Hips': hips}, NS(U=sc.U)
st.av = NS(inside=set(map(id, order)), root=sc.root, default=lambda p: 1.0 if u.truthy(p[1].data.get('m_IsActive', '1'))
           else 0.0)
near('the floor', (st.floor(), 0, 0), (-0.03, 0, 0))
f2.data['m_IsActive'] = '1'
before = len(u.WARNINGS)
check('two active: none, and said', (st.floor(), len(u.WARNINGS) - before), (None, 1))

print('\n%d failure(s)' % len(FAILS) if FAILS else '\nall passed')
sys.exit(1 if FAILS else 0)
