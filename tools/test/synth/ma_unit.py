# ma_unit.py: ModularAvatar on small hand-made hierarchies, against what MA 1.18.7's code does
import sys, os, math
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))  # tools/
import unity2hypr3d as u
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

print('\n%d failure(s)' % len(FAILS) if FAILS else '\nall passed')
sys.exit(1 if FAILS else 0)
