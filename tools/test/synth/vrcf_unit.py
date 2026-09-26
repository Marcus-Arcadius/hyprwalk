# vrcf_unit.py: the VRCFury emulation of tools/unity2hypr3d.py on small hand-made cases: its serialized format (both
# of Unity's [SerializeReference] layouts), the upgrades of old features, Armature Link on small hierarchies, and
# toggles with the resting state they give the avatar
#   blender -b --factory-startup --python-exit-code 1 -P vrcf_unit.py
import sys, os
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
    print('%s %s: %s%s' % ('ok  ' if ok else 'FAIL', what, tuple(round(x, 4) for x in a),
                           '' if ok else ' (want %s)' % (tuple(round(x, 4) for x in b),)))
    if not ok:
        FAILS.append(what)


def T(cls, ns='VF.Model.Feature'):
    return {'class': cls, 'ns': ns, 'asm': 'VRCFury'}


def component(data, go=None):
    d = dict(data)
    d['m_Script'] = {'fileID': '11500000', 'guid': u.VRCF_GUID, 'type': '3'}
    d['m_GameObject'] = go
    c = u.Obj(114, 'MonoBehaviour', d)
    if go is not None:
        go.comps.append(c)
    return c


class Scene:
    """GameObjects with world matrices given directly"""

    def __init__(self):
        self.gos, self.U = [], {}
        self.root = self.go('Avatar', None, (0, 0, 0))

    def go(self, name, parent, pos, active=True):
        g = u.Obj(1, 'GameObject', {'m_Name': name, 'm_IsActive': '1' if active else '0', 'm_TagString': 'Untagged'})
        tf = u.Obj(4, 'Transform', {'m_GameObject': g})
        g.tf, g.comps, g.children, g.parent = tf, [tf], [], parent
        if parent is not None:
            parent.children.append(g)
        self.U[id(g)] = Matrix.Translation(Vector(pos))
        self.gos.append(g)
        return g

    def smr(self, g, bones, root=None, names=()):
        c = u.Obj(137, 'SkinnedMeshRenderer', {'m_GameObject': g, 'm_Bones': [b.tf for b in bones],
                                               'm_RootBone': root.tf if root else None, '_names': list(names)})
        g.comps.append(c)
        return c

    def pb(self, g, root=None):
        c = u.Obj(114, 'MonoBehaviour', {'m_GameObject': g, 'pull': '0.2', 'multiChildType': '0',
                                         'rootTransform': root.tf if root else None, 'ignoreTransforms': []})
        g.comps.append(c)
        return c

    def av(self):
        order = []

        def walk(g):
            order.append(g)
            for c in g.children:
                walk(c)
        walk(self.root)
        return AV(self.root, order)


class AV:
    """what the VRCFury code needs of the converter's Avatar"""
    default = u.Avatar.default

    def __init__(self, root, gos):
        self.root, self.gos = root, gos
        self.inside = set(map(id, gos))
        self.renderers = [c for g in gos for c in g.comps if c.cls in u.RENDERERS]
        self.ma = None

    def shape_names(self, smr):
        return smr.data.get('_names', [])


class VF:
    """the links an Analysis' VRCFury would hand link_armatures"""

    def __init__(self, av, links):
        self.av, self.links = av, links

    go = u.VRCFury.go


def link(prop, bone=0, **kw):
    f = {'@class': 'ArmatureLink', 'version': 7, 'propBone': prop, 'linkTo': [{'useBone': '1', 'bone': bone}],
         'recursive': '1', 'alignPosition': '1', 'alignRotation': '1', 'alignScale': '1', 'autoScaleFactor': '1',
         'scalingFactorPowersOf10Only': '1', 'removeBoneSuffix': ''}
    f.update(kw)
    return f


HB = {n: i for i, n in enumerate(u.HUMAN_BONES)}

print('== [SerializeReference]: the RefIds layout, a null reference, actions inside a State')
g = u.Obj(1, 'GameObject', {'m_Name': 'Coat'})
c = component({'content': {'rid': '111'}, 'config': {'features': []}, 'references': {'version': '2', 'RefIds': [
    {'rid': '111', 'type': T('Toggle'), 'data': {'version': '3', 'name': 'A/B', 'defaultOn': '1',
                                                 'state': {'actions': [{'rid': '222'}, {'rid': '-2'}]}}},
    {'rid': '222', 'type': T('ObjectToggleAction', 'VF.Model.StateAction'),
     'data': {'version': '1', 'obj': g, 'mode': '1'}}]}})
fs = u.vrcf_features(c)
check('one Toggle', [f['@class'] for f in fs], ['Toggle'])
check('its actions, the null one None', [a and a['@class'] for a in fs[0]['state']['actions']],
      ['ObjectToggleAction', None])
check('the action\'s object and mode', (fs[0]['state']['actions'][0]['obj'] is g, fs[0]['state']['actions'][0]['mode']),
      (True, '1'))
check('is_vrcfury by its script', u.is_vrcfury(c), True)
check('is_vrcfury by its fields', u.is_vrcfury(u.Obj(114, 'MonoBehaviour', {'somethingIsBroken': '0', 'content': {}})),
      True)
check('not an MA component', u.is_vrcfury(u.Obj(114, 'MonoBehaviour', {'m_Script': {
    'fileID': '11500000', 'guid': '2df373bf91cf30b4bbd495e11cb1a2ec', 'type': '3'}})), False)

print('== Unity 2019\'s layout (ids as keys) in config.features, and the upgrades of old saves')
old = component({'config': {'features': [{'id': '0'}, {'id': '1'}, {'id': '3'}]}, 'references': {
    'version': '1',
    '00000000': {'type': T('Toggle'), 'data': {'version': '0', 'name': 'Old', 'defaultOn': '0',
                                              'defaultSliderValue': '0.5', 'state': {'actions': [{'id': '2'}]}}},
    '00000001': {'type': T('ArmatureLink'), 'data': {'version': '0', 'propBone': g, 'useBoneMerging': '1',
                                                    'keepBoneOffsets': '0', 'boneOnAvatar': '0'}},
    '00000002': {'type': T('ObjectToggleAction', 'VF.Model.StateAction'), 'data': {'version': '0', 'obj': g,
                                                                                  'mode': '0'}},
    '00000003': {'type': T('ArmatureLink'), 'data': {'version': '5', 'propBone': g, 'linkMode': '3',
                                                    'keepBoneOffsets2': '1', 'boneOnAvatar': '10',
                                                    'fallbackBones': ['9'], 'skinRewriteScalingFactor': '0'}}}})
fs = u.vrcf_features(old)
check('three features', [f['@class'] for f in fs], ['Toggle', 'ArmatureLink', 'ArmatureLink'])
t, a0, a5 = fs
check('Toggle v0: default slider value cleared, inactive at zero', (t['defaultSliderValue'], t['sliderInactiveAtZero']),
      (0, '1'))
check('Object Toggle v0: flips the object', t['state']['actions'][0]['mode'], 2)
check('Armature Link v0 bone merging: recursive, snapped, auto scale off',
      (a0['recursive'], a0['@align'], a0['autoScaleFactor'], a0['skinRewriteScalingFactor']), ('1', True, '0', 1.0))
check('Armature Link v0: links to its boneOnAvatar', [x['bone'] for x in a0['linkTo']], ['0'])
check('Armature Link v5 Reparent Root, offsets kept: not recursive, not snapped',
      (a5['recursive'], a5['@align']), ('0', False))
check('Armature Link v5: boneOnAvatar then fallbackBones', [x['bone'] for x in a5['linkTo']], ['10', '9'])
check('Armature Link v5: no multiplier saved, so an automatic one (as recursive)',
      (a5['autoScaleFactor'], a5['skinRewriteScalingFactor']), (None, 1.0))
auto = u.vrcf_upgrade({'@class': 'ArmatureLink', 'version': 4, 'linkMode': '4', 'keepBoneOffsets2': '0',
                       'skinRewriteScalingFactor': '1', 'boneOnAvatar': '0', 'bonePathOnAvatar': 'Armature/Hips'})[0]
check('Armature Link v4 Auto: decided when linked', (auto['recursive'], auto['@align']), (None, None))
check('Armature Link v4 with a path: links to the path under the root',
      [(x['useBone'], x['useObj'], x['offset']) for x in auto['linkTo']], [('0', '0', 'Armature/Hips')])

modes = u.vrcf_upgrade({'@class': 'Modes', 'version': '0', 'name': 'My Outfit', 'saved': '1', 'modes': [
    {'state': {'actions': []}}, {'state': {'actions': []}}]})
check('Modes: a toggle each, one exclusive tag', [(m['name'], m['exclusiveTag'], m['enableExclusiveTag']) for m in modes],
      [('My Outfit/Mode 1', 'mode_MyOutfit', '1'), ('My Outfit/Mode 2', 'mode_MyOutfit', '1')])
a, b, cc = (u.Obj(1, 'GameObject', {'m_Name': n}) for n in 'ABC')
st = u.vrcf_upgrade({'@class': 'ObjectState', 'version': '0', 'states': [
    {'obj': a, 'action': '1'}, {'obj': b, 'action': '0'}, {'obj': cc, 'action': '2'}]})
check('Object State: a delete and the rest applied during the upload', [f['@class'] for f in st],
      ['DeleteDuringUpload', 'ApplyDuringUpload'])
check('... what it deletes', st[0]['@target'] is cc, True)
check('... what it turns on and off', [(x['obj'].data['m_Name'], x['mode']) for x in st[1]['action']['actions']],
      [('A', 0), ('B', 1)])
bc = u.vrcf_upgrade({'@class': 'BoneConstraint', 'version': '0', 'obj': a, 'bone': '10'})[0]
check('Bone Constraint: an Armature Link, snapped, not recursive',
      (bc['@class'], bc['recursive'], bc['alignPosition'], bc['linkTo'][0]['bone']), ('ArmatureLink', '0', '1', '10'))
X = {'fileID': '9100000', 'guid': 'a' * 32, 'type': '2'}
fc = u.vrcf_upgrade({'@class': 'FullController', 'version': '0', 'controller': {'objRef': X}, 'menu': {
    'id': 'b' * 32 + '|Assets/m.asset'}, 'submenu': 'Props', 'parameters': {'id': 'c' * 32 + ':11400000|x'},
    'removePrefixes': ['Old/'], 'addPrefix': 'New'})[0]
check('Full Controller v0: every unsynced parameter global', fc['allNonsyncedAreGlobal'], '1')
check('... its controller, menu (under its submenu) and parameters in the lists',
      (len(fc['controllers']), fc['menus'][0]['prefix'], len(fc['prms'])), (1, 'Props', 1))
check('... removePrefixes and addPrefix as path rewrites', fc['rewriteBindings'],
      [{'from': 'Old/', 'to': ''}, {'from': '', 'to': 'New'}])

print('== asset fields, menu paths, the humanoid fallbacks')
check('asset by its reference', u.vrcf_asset({'objRef': X, 'id': ''}), ('a' * 32, 9100000))
check('asset by its id', u.vrcf_asset({'objRef': None, 'id': 'b' * 32 + ':123|Assets/x.anim|name'}), ('b' * 32, 123))
check('a main asset\'s id', u.vrcf_asset({'id': 'c' * 32 + '|Assets/x.anim'}), ('c' * 32, 0))
check('the old guid and fileID', u.vrcf_asset({'guid': 'D' * 32, 'fileID': '7400000', 'id': ''}), ('d' * 32, 7400000))
check('no asset', u.vrcf_asset({'id': '', 'objRef': None}), None)
check('menu path', u.vrcf_path('Clothes/Jacket'), ['Clothes', 'Jacket'])
check('menu path with an escaped slash', u.vrcf_path('A\\/B/C'), ['A/B', 'C'])
check('menu path with empty parts', u.vrcf_path('/X//Y/'), ['X', 'Y'])
check('UpperChest falls back to', [u.HUMAN_BONES[b] for b in u.human_fallbacks(HB['UpperChest'])],
      ['Chest', 'Spine', 'Hips'])
check('Jaw falls back to', [u.HUMAN_BONES[b] for b in u.human_fallbacks(HB['Jaw'])],
      ['Head', 'Neck', 'UpperChest', 'Chest', 'Spine', 'Hips'])
fb = [u.HUMAN_BONES[b] for b in u.human_fallbacks(HB['Left Index Distal'])]
check('a finger falls back to its lower segments first', fb[:2], ['Left Index Intermediate', 'Left Index Proximal'])
check('... then the fingers beside it', fb[2:5], ['Left Middle Distal', 'Left Middle Intermediate',
                                                  'Left Middle Proximal'])
check('... then the hand and up', fb[-8:], ['LeftHand', 'LeftLowerArm', 'LeftUpperArm', 'LeftShoulder', 'UpperChest',
                                           'Chest', 'Spine', 'Hips'])

print('== prefab overrides of [SerializeReference] fields: managedReferences[id].field')
d = {'references': {'version': '2', 'RefIds': [{'rid': '5', 'type': T('Toggle'), 'data': {
    'defaultOn': '0', 'state': {'actions': []}}}]}}
u.set_prop(d, 'managedReferences[5].defaultOn', '1')
u.set_prop(d, 'managedReferences[5].name', 'Coat')
u.set_prop(d, 'managedReferences[6].name', 'nothing there')
check('RefIds entry changed', d['references']['RefIds'][0]['data'], {'defaultOn': '1', 'state': {'actions': []},
                                                                     'name': 'Coat'})
d = {'references': {'version': '1', '00000002': {'type': T('Toggle'), 'data': {'name': 'x'}}}}
u.set_prop(d, 'managedReferences[2].name', 'y')
check('Unity 2019 entry changed', d['references']['00000002']['data']['name'], 'y')


def hierarchy(sc, human):
    av = sc.av()
    h = u.ModularAvatar(av, dict(human), sc.U)
    return av, h


def avatar(sc):
    arm = sc.go('Armature', sc.root, (0, 0, 0))
    hips = sc.go('Hips', arm, (0, 1, 0))
    spine = sc.go('Spine', hips, (0, 1.1, 0))
    chest = sc.go('Chest', spine, (0, 1.3, 0))
    neck = sc.go('Neck', chest, (0, 1.5, 0))
    head = sc.go('Head', neck, (0, 1.6, 0))
    return {'Hips': hips, 'Spine': spine, 'Chest': chest, 'Neck': neck, 'Head': head}


print('== Armature Link: a suffix worked out, bones snapped on and linked, a PhysBone root kept, the unused removed')
sc = Scene()
human = avatar(sc)
tail = sc.go('Tail', human['Hips'], (0, 0.95, 0.1))
coat = sc.go('Coat', sc.root, (0, 0, 0))
oa = sc.go('Armature', coat, (0, 0, 0))
oh = sc.go('Hips_Coat', oa, (0, 1.02, 0))
osp = sc.go('Spine_Coat', oh, (0, 1.12, 0))
pocket = sc.go('Pocket', osp, (0.1, 1.05, -0.1))
oc = sc.go('Chest_Coat', osp, (0, 1.32, 0))
frill = sc.go('Frill', oc, (0, 1.3, 0.1))
on = sc.go('Neck_Coat', oc, (0, 1.52, 0))
ot = sc.go('Tail_Coat', oh, (0, 0.97, 0.1))
ot2 = sc.go('Tail_Coat.001', ot, (0, 0.9, 0.2))
mesh = sc.go('CoatMesh', coat, (0, 0, 0))
sc.smr(mesh, [oh, osp, oc, on, ot, ot2], root=oh)
sc.smr(pocket, [], root=None)  # something the pocket draws
sc.pb(frill)
sc.pb(ot)
comp = component({}, coat)
av, h = hierarchy(sc, human)
u.link_armatures(h, VF(av, [(comp, link(oh))]), human)
check('linked under the avatar\'s bones', [h.up(x) is human[n] for x, n in ((oh, 'Hips'), (osp, 'Spine'),
                                                                             (oc, 'Chest'))], [True, True, True])
near('Hips_Coat snapped onto Hips', h.U[id(oh)].translation, (0, 1, 0))
near('Spine_Coat snapped onto Spine', h.U[id(osp)].translation, (0, 1.1, 0))
near('the pocket went with its bone (an object with no match stays under it)', h.U[id(pocket)].translation,
     (0.1, 1.03, -0.1))
check('the pocket still under Spine_Coat', h.up(pocket) is osp, True)
check('an unmatched PhysBone root stays under its bone', h.up(frill) is oc, True)
check('meshes weighted to the linked bones go to the avatar\'s',
      sorted((h.name(g), h.name(t)) for g, t in ((h.byid[k], v) for k, v in h.retarget.items())),
      [('Chest_Coat', 'Chest'), ('Hips_Coat', 'Hips'), ('Neck_Coat', 'Neck'), ('Spine_Coat', 'Spine')])
check('a PhysBone\'s root is linked but keeps its own weights', (h.up(ot) is tail, id(ot) in h.retarget),
      (True, False))
check('... and its chain stays under it', h.up(ot2) is ot, True)
near('where Hips_Coat was when its weights moved', h.bound[id(oh)].translation, (0, 1, 0))
check('removed: what nothing uses', sorted(h.name(h.byid[k]) for k in h.deleted), ['Neck_Coat'])

print('== an avatar PhysBone above where a bone goes leaves it out')
sc = Scene()
human = avatar(sc)
hair = sc.go('Hair', human['Head'], (0, 1.7, 0.05))
hair2 = sc.go('Hair.001', hair, (0, 1.6, 0.1))
sc.pb(hair)
acc = sc.go('Acc', sc.root, (0, 0, 0))
ah = sc.go('Hair', acc, (0, 1.7, 0.05))
charm = sc.go('Charm', ah, (0, 1.72, 0.08))
sc.go('Deco', charm, (0, 1.73, 0.09)).comps.append(u.Obj(23, 'MeshRenderer', {}))
comp = component({}, acc)
av, h = hierarchy(sc, human)
u.link_armatures(h, VF(av, [(comp, link(ah, bone=HB['Head'], recursive='0', alignPosition='0',
                                         alignRotation='0', alignScale='0', linkTo=[
                                             {'useBone': '0', 'useObj': '1', 'obj': hair, 'offset': ''}]))]), human)
check('linked to the object Link To names', h.up(ah) is hair, True)
check('the avatar\'s hair PhysBone leaves it out', [h.name(x) for x in h.blocks.get(id(hair), [])], ['Hair'])
near('not snapped: where it was', h.U[id(ah)].translation, (0, 1.7, 0.05))

print('== Link To a bone the avatar does not have: the one above it; Link From holding avatar bones: not linked')
sc = Scene()
human = avatar(sc)
p = sc.go('Pin', sc.root, (0.1, 1.35, 0))
comp = component({}, p)
av, h = hierarchy(sc, human)
u.link_armatures(h, VF(av, [(comp, link(p, bone=HB['UpperChest'], recursive='0', alignPosition='0',
                                        alignRotation='0', alignScale='0'))]), human)
check('UpperChest missing: linked to Chest', h.up(p) is human['Chest'], True)
near('kept where it was', h.U[id(p)].translation, (0.1, 1.35, 0))
before = u.WARNINGS[:]
av, h = hierarchy(sc, human)
u.link_armatures(h, VF(av, [(comp, link(human['Spine'], bone=HB['Head']))]), human)
check('an avatar bone as Link From: left alone', h.up(human['Spine']) is human['Hips'], True)
check('... and said', len(u.WARNINGS) > len(before), True)

print('== a scale multiplier from the bones\' scales, in powers of ten (clothes exported in centimetres)')
sc = Scene()
human = avatar(sc)
big = sc.go('Big', sc.root, (0, 0, 0))
bh = sc.go('Hips', big, (0, 1, 0))
sc.U[id(bh)] = Matrix.Translation((0, 1, 0)) @ Matrix.Scale(100.0, 4)
comp = component({}, big)
av, h = hierarchy(sc, human)
u.link_armatures(h, VF(av, [(comp, link(bh))]), human)
near('Hips scaled 100 times the avatar\'s', h.U[id(bh)].to_scale(), (100, 100, 100), 1e-3)

print('== toggles: menu entries, exclusive tags, the resting state')
sc = Scene()
human = avatar(sc)
coat = sc.go('Coat', sc.root, (0, 0, 0))
jacket = sc.go('Jacket', sc.root, (0, 0, 0))
hat = sc.go('Hat', sc.root, (0, 0, 0), active=False)
extra = sc.go('Extra', sc.root, (0, 0, 0))
body = sc.go('Body', sc.root, (0, 0, 0))
bsmr = sc.smr(body, [], names=['Shrink', 'Smile'])
bsmr.data['m_BlendShapeWeights'] = ['0', '0']


def toggle(name, actions, **kw):
    f = {'@class': 'Toggle', 'version': 3, 'name': name, 'state': {'actions': actions}}
    f.update(kw)
    return f


def turn(g, mode):
    return {'@class': 'ObjectToggleAction', 'version': 1, 'obj': g, 'mode': mode}


feats = [toggle('Clothes/Coat', [turn(coat, 0), {'@class': 'BlendShapeAction', 'blendShape': 'Shrink',
                                                 'blendShapeValue': '100', 'allRenderers': '1'}],
                defaultOn='1', enableExclusiveTag='1', exclusiveTag='top'),
         toggle('Clothes/Jacket', [turn(jacket, 0)], enableExclusiveTag='1', exclusiveTag='top, outer'),
         toggle('Clothes/Nothing', [], enableExclusiveTag='1', exclusiveTag='top', exclusiveOffState='1'),
         toggle('Hide Hat', [turn(hat, 1)]),
         toggle('', [turn(extra, 0)], defaultOn='1'),
         toggle('Big', [turn(extra, 0)], slider='1', defaultSliderValue='0.5'),
         {'@class': 'ApplyDuringUpload', 'action': {'actions': [
             {'@class': 'BlendShapeAction', 'blendShape': 'Smile', 'blendShapeValue': '30', 'allRenderers': '1'}]}},
         {'@class': 'ZawooIntegration'}]
comp = component({}, sc.root)
u._VRCF_FEATURES[id(comp)] = feats


class AN:
    pass


an = AN()
an.av, an.db = sc.av(), None
before = len(u.WARNINGS)
vf = u.VRCFury(an, [comp])
check('resting state: what a toggle turns on is off, what one turns off is on',
      [g.data['m_IsActive'] for g in (coat, jacket, extra, hat)], ['0', '0', '0', '1'])
check('Apply During Upload set the shape key', bsmr.data['m_BlendShapeWeights'], ['0', 30.0])
check('menu entries: path, name, group', [(p, c['name'], c.get('group', '')) for p, c in vf.menu],
      [(('Clothes',), 'Coat', 'top'), (('Clothes',), 'Jacket', 'top'), (('Clothes',), 'Nothing', 'top'),
       ((), 'Hide Hat', ''), ((), 'Big', '')])
check('parameters: bool, on as defaultOn; the off state stays off (Coat is on at first); the slider a float',
      [vf.declared[c['parameter']['name'] or c['subParameters'][0]['name']] for p, c in vf.menu],
      [(2, 1.0), (2, 0.0), (2, 0.0), (2, 0.0), (1, 0.5)])
check('a slider: a radial puppet in the menu, of its float', (vf.menu[-1][1]['type'], vf.skipped), (203, []))
check('a feature it does not convert: said, with why', any('Zawoo' in w and 'contacts' in w
                                                            for w in u.WARNINGS[before:]), True)
params = {k: v for k, (t, v) in vf.declared.items()}
vals = vf.apply(params, {})
check('with the defaults: the coat and the nameless toggle\'s object on, the shape key set',
      (vals.get(('a', coat)), vals.get(('a', extra)), vals.get(('s', bsmr, 'Shrink'))), (1.0, 1.0, 100.0))
params[vf.menu[3][1]['parameter']['name']] = 1.0
check('Hide Hat on: hat off', vf.apply(params, {}).get(('a', hat)), 0.0)

print('== Blend Shape Link: which shape keys follow which')
f = {'exactMatch': '0', 'includeAll': '1', 'excludes': [{'name': 'Skip'}], 'includes': [
    {'nameOnBase': 'Smile', 'nameOnLinked': 'Grin'}, {'nameOnBase': '', 'nameOnLinked': 'Solo'}]}
m = u.VRCFury.shape_map(f, ['Chest_Big', 'Smile', 'Skip', 'Solo', 'DUPA'],
                        ['chest_big', 'Grin', 'Smile', 'Skip', 'Solo', 'DupA', 'dup a'])
check('loose names, an include (no name-alike match after it), excludes, an ambiguous loose name left out', m,
      {'Chest_Big': ['chest_big'], 'Smile': ['Grin'], 'Solo': ['Solo']})
f['exactMatch'] = '1'
check('exact names only', u.VRCFury.shape_map(f, ['Chest_Big', 'Smile'], ['chest_big', 'Smile']), {'Smile': ['Smile']})
sc = Scene()
body = sc.go('Body', sc.root, (0, 0, 0))
b1 = sc.smr(body, [], names=['Chest_Big', 'Smile'])
b1.data['m_BlendShapeWeights'] = ['40', '0']
top = sc.go('Top', sc.root, (0, 0, 0))
t1 = sc.smr(top, [], names=['chest_big'])
t1.data['m_BlendShapeWeights'] = ['0']
comp = component({}, top)
u._VRCF_FEATURES[id(comp)] = [{'@class': 'BlendShapeLink', 'version': 1, 'baseObj': 'Body', 'includeAll': '1',
                               'exactMatch': '0', 'linkSkins': [{'renderer': t1}], 'excludes': [], 'includes': []},
                              toggle('Big', [{'@class': 'BlendShapeAction', 'blendShape': 'Chest_Big',
                                              'blendShapeValue': '100', 'allRenderers': '1'}])]
an = AN()
an.av, an.db = sc.av(), None
vf = u.VRCFury(an, [comp])
check('the linked mesh rests as the base does', t1.data['m_BlendShapeWeights'], [40.0])
p = {vf.menu[0][1]['parameter']['name']: 1.0}
check('and follows it when a toggle sets the base\'s', vf.apply(p, {}).get(('s', t1, 'chest_big')), 100.0)

print('== an exclusive off state with no toggle of its tag on at first: on at first')
sc = Scene()
a, b = sc.go('A', sc.root, (0, 0, 0)), sc.go('B', sc.root, (0, 0, 0))
comp = component({}, sc.root)
u._VRCF_FEATURES[id(comp)] = [toggle('A', [turn(a, 0)], enableExclusiveTag='1', exclusiveTag='t'),
                              toggle('None', [], enableExclusiveTag='1', exclusiveTag='t', exclusiveOffState='1')]
an = AN()
an.av, an.db = sc.av(), None
vf = u.VRCFury(an, [comp])
check('the off state is on at first', [vf.declared[c['parameter']['name']][1] for p, c in vf.menu], [0.0, 1.0])

def vrcf(sc, feats, names=None):
    comp = component({}, sc.root)
    u._VRCF_FEATURES[id(comp)] = [x for f in feats for x in u.vrcf_upgrade(f)]  # as vrcf_features() upgrades them
    an = AN()
    an.av, an.db = sc.av(), None
    if names:
        an.av.shape_names = lambda r: names.get(id(r), [])
    return u.VRCFury(an, [comp])


def bs(key, value=100):
    return {'@class': 'BlendShapeAction', 'blendShape': key, 'blendShapeValue': value, 'allRenderers': '1'}


print('== sliders: from where the property rests to the state\'s, smoothly as VRCFury\'s flat keys go; a puppet')
sc = Scene()
body = sc.go('Body', sc.root, (0, 0, 0))
b1 = sc.smr(body, [], names=['Chest', 'Squint'])
b1.data['m_BlendShapeWeights'] = ['40', '0']
vf = vrcf(sc, [dict(toggle('Menu/Chest', [bs('Chest')]), slider='1', defaultSliderValue='0.3'),
               {'@class': 'Puppet', 'name': 'Menu/Squint', 'slider': '1', 'defaultX': '0', 'stops': [
                   {'x': '0.5', 'y': '0', 'state': {'actions': [bs('Squint', 50)]}},
                   {'x': '1', 'y': '0', 'state': {'actions': [bs('Squint', 100)]}}]},
               {'@class': 'Puppet', 'name': 'Two axes', 'slider': '0', 'stops': [
                   {'x': '0', 'y': '1', 'state': {'actions': [bs('Squint', 100)]}}]}])
radials = [(p, c) for p, c in vf.menu if c['type'] == 203]
check('radials in the menu, of their floats', [(p, c['name'], c['type'], c['subParameters'][0]['name']) for p, c in radials],
      [(('Menu',), 'Chest', 203, 'VF0_Menu/Chest'), (('Menu',), 'Squint', 203, 'Menu/Squint_x')])
check('the floats\' defaults', [vf.declared[c['subParameters'][0]['name']] for p, c in radials], [(1, 0.3), (1, 0.0)])
check('a puppet with two axes (y only): a two-axis puppet in the menu', [(c['name'], c['type'], [
    x['name'] for x in c['subParameters']]) for p, c in vf.menu if c['type'] == 201], [('Two axes', 201, ['', 'Two axes_y'])])
check('... a 2D freeform directional tree: all of the stop at it, half of it half way, none at the middle',
      [round(vf.apply({'Two axes_y': y}, {}).get(('s', b1, 'Squint'), -1), 3) for y in (1.0, 0.5, 0.0)], [100.0, 50.0, 0.0])
at = lambda x, key='Chest', pn='VF0_Menu/Chest': round(vf.apply({pn: x}, {})[('s', b1, key)], 3)
check('the slider at 0, 0.25, 0.5, 1: 40 to 100 along smoothstep', [at(x) for x in (0.001, 0.25, 0.5, 1)],
      [40.0, 49.375, 70.0, 100.0])
check('the puppet between its stops (0 where it rests)', [at(x, 'Squint', 'Menu/Squint_x') for x in (0.25, 0.5, 0.75, 1)],
      [25.0, 50.0, 75.0, 100.0])

print('== Gesture Drivers (and Senky\'s): a hand\'s sign shows a state, a lock toggle too; faces blocking blinks')
sc = Scene()
body = sc.go('Body', sc.root, (0, 0, 0))
b1 = sc.smr(body, [], names=['Wink', 'Smile', 'Grr', 'Blep'])
b1.data['m_BlendShapeWeights'] = ['0', '0', '0', '0']
vf = vrcf(sc, [{'@class': 'GestureDriver', 'version': '0', 'gestures': [
    {'hand': '1', 'sign': '2', 'comboSign': '0', 'state': {'actions': [bs('Wink')]}, 'disableBlinking': '1'},
    {'hand': '0', 'sign': '7', 'comboSign': '0', 'state': {'actions': [bs('Smile')]}, 'enableLockMenuItem': '1',
     'lockMenuItem': 'Faces/Smile'},
    {'hand': '3', 'sign': '1', 'comboSign': '1', 'state': {'actions': [bs('Grr')]}}]}])
check('the lock toggle in the menu', [(p, c['name'], c['type']) for p, c in vf.menu], [(('Faces',), 'Smile', 102)])
lock = vf.menu[0][1]['parameter']['name']
names, track = {}, {}
v = vf.apply({'GestureLeft': 2.0}, {}, names, track)
check('left open: the wink, blinking blocked (an old save\'s disableBlinking)', (v.get(('s', b1, 'Wink')), track.get('eyes')),
      (100.0, 2))
check('... not by the right hand', ('s', b1, 'Wink') in vf.apply({'GestureRight': 2.0}, {}), False)
check('thumbs up on either hand smiles, named for its lock item', ([('s', b1, 'Smile') in vf.apply({h: 7.0}, {}) for h in (
    'GestureLeft', 'GestureRight')], names.get(('s', b1, 'Smile')) or vf.apply({'GestureLeft': 7.0}, {}, names) and
    names.get(('s', b1, 'Smile'))), ([True, True], 'Smile'))
check('the lock toggle shows it with the hands neutral', vf.apply({lock: 1.0}, {}).get(('s', b1, 'Smile')), 100.0)
check('a combo of both hands: the left\'s sign and the right\'s combo sign, not one of them alone',
      [('s', b1, 'Grr') in vf.apply(p, {}) for p in ({'GestureLeft': 1.0, 'GestureRight': 1.0}, {'GestureLeft': 1.0},
                                                     {'GestureRight': 1.0})], [True, False, False])
sen = u.vrcf_upgrade({'@class': 'SenkyGestureDriver', 'eyesHappy': {'actions': [bs('Smile')]},
                      'mouthBlep': {'actions': [bs('Blep')]}, 'earsBack': {'actions': []}})[0]
gs = sen['gestures']
check('Senky\'s: a Gesture Driver, either hand, lock items in Emote Lock',
      (sen['@class'], sorted({(g['hand'], g['lockMenuItem']) for g in gs})),
      ('GestureDriver', [(0, 'Emote Lock/Angry'), (0, 'Emote Lock/Happy'), (0, 'Emote Lock/Sad'), (0, 'Emote Lock/Tongue')]))
check('... happy eyes by thumbs up, stopping the blinks; the tongue by victory',
      [([a['@class'] for a in g['state']['actions']], g['sign']) for g in gs if g['exclusiveTag'] in ('eyes', 'mouth') and
       g['state']['actions'] and g['state']['actions'][0].get('blendShape') in ('Smile', 'Blep')],
      [(['BlendShapeAction', 'BlockBlinkingAction'], 7), (['BlendShapeAction'], 4)])

print('== Blinking and Visemes: faces of their own')
vf = vrcf(sc, [{'@class': 'Blinking', 'state': {'actions': [bs('Wink')]}},
               {'@class': 'Visemes', 'state_aa': {'actions': [bs('Blep')]}, 'state_O': {'actions': [bs('Grr', 60)]},
                'state_PP': {'actions': [bs('Smile')]}}])
check('the blink', vf.blink, {(b1, 'Wink'): 1.0})
check('the visemes hypr3d has (aa..ou), at their weights', vf.visemes, {'aa': {(b1, 'Blep'): 1.0}, 'oh': {(b1, 'Grr'): 0.6}})
check('... and the consonants (PP..RR) apart', vf.consonants, {'pp': {(b1, 'Smile'): 1.0}})

print('== material actions: a slot\'s material, a property\'s value; Set an FX Float; exclusive tags of several groups')
sc = Scene()
hat = sc.go('Hat', sc.root, (0, 0, 0))
hmr = u.Obj(23, 'MeshRenderer', {'m_GameObject': hat, 'm_Materials': [{'fileID': '2100000', 'guid': 'a' * 32}]})
hat.comps.append(hmr)
star, moon, sun = (sc.go(n, sc.root, (0, 0, 0)) for n in ('Star', 'Moon', 'Sun'))
feats = [toggle('Gold', [{'@class': 'MaterialAction', 'version': 1, 'renderer': hmr, 'materialIndex': '0',
                          'mat': {'id': 'b' * 32 + '|Assets/Gold.mat'}}]),
         toggle('Old', [{'@class': 'MaterialAction', 'version': 0, 'obj': hat, 'materialIndex': '0',
                         'mat': {'id': 'c' * 32 + ':2100000|Assets/Old.mat'}}]),
         toggle('Tint', [{'@class': 'MaterialPropertyAction', 'version': 2, 'renderer2': hat, 'affectAllMeshes': '0',
                          'propertyName': '_Color', 'propertyType': '1', 'valueColor': {'r': '1', 'g': '0.5', 'b': '0.5',
                                                                                        'a': '1'}}]),
         toggle('Shine', [{'@class': 'MaterialPropertyAction', 'version': 2, 'renderer2': hat, 'affectAllMeshes': '0',
                           'propertyName': '_Glossiness', 'propertyType': '0', 'value': '1'}]),
         toggle('Glow', [{'@class': 'FxFloatAction', 'name': 'Glow', 'value': '0.7'}]),
         toggle('Star', [turn(star, 0)], enableExclusiveTag='1', exclusiveTag='deco, left'),
         toggle('Moon', [turn(moon, 0)], enableExclusiveTag='1', exclusiveTag='deco'),
         toggle('Sun', [turn(sun, 0)], enableExclusiveTag='1', exclusiveTag='left')]
before = len(u.WARNINGS)
vf = vrcf(sc, feats)
p = {c['name']: c['parameter']['name'] for _, c in vf.menu}
check('the material in the slot', vf.apply({p['Gold']: 1.0}, {}).get(('m', hmr, 0)), ('b' * 32, 2100000))
check('an old save names the object: its renderer\'s slot', vf.apply({p['Old']: 1.0}, {}).get(('m', hmr, 0)),
      ('c' * 32, 2100000))
check('a colour property, channel by channel', sorted((k[2], v) for k, v in vf.apply({p['Tint']: 1.0}, {}).items()),
      [('_Color.a', 1.0), ('_Color.b', 0.5), ('_Color.g', 0.5), ('_Color.r', 1.0)])
check('a property hypr3d does not carry: said', any('_Glossiness' in w for w in u.WARNINGS[before:]), True)
check('an FX float while it is on, for the animators to read', (vf.drive({p['Glow']: 1.0}).get('Glow'),
                                                                 vf.drive({p['Glow']: 0.0}).get('Glow')), (0.7, None))
check('a toggle of two tags is in both groups', [(c['name'], c.get('group'), c.get('groups')) for _, c in vf.menu[-3:]],
      [('Star', 'deco', ['deco', 'left']), ('Moon', 'deco', None), ('Sun', 'left', None)])

print('== Move Menu Item and Reorder Menu Item; old features upgraded into toggles')
m = [((), {'name': 'A'}), (('Sub',), {'name': 'B'}), (('Sub',), {'name': 'C'}), (('Sub', 'Deep'), {'name': 'D'}),
     ((), {'name': 'E'})]
vf.moves = [{'@class': 'MoveMenuItem', 'fromPath': 'Sub/B', 'toPath': 'Top/Renamed'},
            {'@class': 'MoveMenuItem', 'fromPath': 'Sub/Deep', 'toPath': 'Deeper'},
            {'@class': 'ReorderMenuItem', 'path': 'E', 'position': '0'}]
check('moved (renamed), a submenu moved whole, one put first', [('/'.join(p + (c['name'],))) for p, c in vf.menu_moved(m)],
      ['E', 'A', 'Sub/C', 'Top/Renamed', 'Deeper/D'])
br = u.vrcf_upgrade({'@class': 'Breathing', 'version': '0', 'inState': {'actions': []}, 'outState': {'actions': []},
                     'blendshape': 'Chest', 'obj': None})[0]
check('Breathing: a toggle, on at first, of a loop between breathing out and in',
      (br['@class'], br['name'], br['defaultOn'], br['state']['actions'][0]['@class'],
       [a['blendShapeValue'] for a in br['state']['actions'][0]['state1']['actions']]),
      ('Toggle', 'Breathing', '1', 'SmoothLoopAction', [100]))
wc = u.vrcf_upgrade({'@class': 'WorldConstraint', 'menuPath': 'Props/Drop', '@go': star})[0]
check('World Constraint: a toggle that drops its object', (wc['name'], wc['state']['actions'][0]['@class'],
                                                           wc['state']['actions'][0]['obj'] is star),
      ('Props/Drop', 'WorldDropAction', True))

print('== Full Controller rewriteBindings: path rewrites in turn, deletes, a prefix for every path')
rw = u.vrcf_rewrite([{'from': 'Armature/', 'to': 'Coat/Armature'}, {'from': 'Coat/Armature/Old', 'to': '', 'delete': '1'}])
check('a prefix swapped, only on whole names', [rw(x) for x in ('Armature/Hips', 'Armature', 'ArmatureX/Hips', 'Body')],
      ['Coat/Armature/Hips', 'Coat/Armature', 'ArmatureX/Hips', 'Body'])
check('a later rule sees an earlier one\'s path; a delete drops the path', (rw('Armature/Old/Bone'), rw('Armature/New')),
      (None, 'Coat/Armature/New'))
rw = u.vrcf_rewrite([{'from': '', 'to': 'Prefix/'}])
check('an empty "from" puts every path under "to", but not one from the root', [rw(x) for x in ('Body', '', '/Root')],
      ['Prefix/Body', 'Prefix', '/Root'])
rw = u.vrcf_rewrite([{'from': 'Deep/Down', 'to': '/'}])
check('"to" the root', rw('Deep/Down/Mesh'), '/Mesh')
check('no rules: nothing to rewrite', u.vrcf_rewrite([]), None)

print('== Armature Link and animated bones: one an animation moves is linked but keeps its weights, and its children')
sc = Scene()
human = avatar(sc)
tail = sc.go('Tail', human['Hips'], (0, 0.95, 0.1))
tail2 = sc.go('Tail.001', tail, (0, 0.9, 0.2))
coat = sc.go('Coat', sc.root, (0, 0, 0))
oa = sc.go('Armature', coat, (0, 0, 0))
oh = sc.go('Hips', oa, (0, 1.0, 0))
osp = sc.go('Spine', oh, (0, 1.1, 0))
ot = sc.go('Tail', oh, (0, 0.95, 0.1))
ot2 = sc.go('Tail.001', ot, (0, 0.9, 0.2))
bag = sc.go('Bag', osp, (0.1, 1.0, 0.1))
sc.smr(sc.go('CoatMesh', coat, (0, 0, 0)), [oh, osp, ot, ot2], root=oh)
sc.smr(bag, [], root=None)
comp = component({}, coat)
av, h = hierarchy(sc, human)
u.link_armatures(h, VF(av, [(comp, link(oh))]), human, ({id(ot), id(osp)}, {id(oh)}))
check('the turned tail: linked, keeps its own weights', (h.up(ot) is tail, id(ot) in h.retarget), (True, False))
check('... its child stays under it, not linked to the avatar\'s', (h.up(ot2) is ot, id(ot2) in h.retarget), (True, False))
check('a humanoid bone is linked all the same, but an animated one keeps its weights',
      (h.up(osp) is human['Spine'], id(osp) in h.retarget), (True, False))
check('a scaled bone is linked, its weights kept', (h.up(oh) is human['Hips'], id(oh) in h.retarget), (True, False))

print('== the transforms animations move: every controller, a Full Controller\'s rewritten paths, toggle clips')


class UFile:
    def __init__(self, docs):
        self.docs, self.order, self.binary = docs, list(docs), False

    def cls(self, f):
        return self.docs[f][0]

    def get(self, f):
        return (None, self.docs[f][1])


class Asset:
    ext = '.asset'


class DB:
    def __init__(self, files):
        self.files = files

    def get(self, g):
        return Asset() if g in self.files else None

    def yaml(self, g):
        return self.files.get(g)


def R(guid, fid):
    return {'fileID': str(fid), 'guid': guid}


sc = Scene()
arm = sc.go('Armature', sc.root, (0, 0, 0))
hips = sc.go('Hips', arm, (0, 1, 0))
ear, wing, horn, fin, halo = (sc.go(n, hips, (0, 1, 0)) for n in ('Ear', 'Wing', 'Horn', 'Fin', 'Halo'))
acc = sc.go('Acc', sc.root, (0, 0, 0))
bow = sc.go('Bow', sc.go('Armature', acc, (0, 0, 0)), (0, 1, 0))
FX, CLIP, TREE, FULL, BOW, HALO = ('%02x' % k * 16 for k in (0xf0, 0xc1, 0xb2, 0xf1, 0xb0, 0xa0))
an = AN()
an.db = DB({
    FX: UFile({1: (1102, {'m_Motion': R(CLIP, 7400000)}), 2: (1102, {'m_Motion': {'fileID': '3'}}),
               3: (206, {'m_Childs': [{'m_Motion': R(TREE, 7400000)}]})}),
    CLIP: UFile({7400000: (74, {'m_EulerCurves': [{'path': 'Armature/Hips/Ear'}],
                                'm_RotationCurves': [{'path': 'Armature/Hips/Horn'}],
                                'm_EditorCurves': [{'classID': '4', 'path': 'Armature/Hips/Fin',
                                                    'attribute': 'm_LocalPosition.y'}]})}),
    TREE: UFile({7400000: (74, {'m_ScaleCurves': [{'path': 'Armature/Hips/Wing'}]})}),
    FULL: UFile({1: (1102, {'m_Motion': R(BOW, 7400000)})}),
    BOW: UFile({7400000: (74, {'m_PositionCurves': [{'path': 'Old/Bow'}]})}),
    HALO: UFile({7400000: (74, {'m_EulerCurves': [{'path': 'Armature/Hips/Halo'}]})})})
an.av, an.mat = sc.av(), None
an.av.paths = {u.av_path(an.av, g): g for g in an.av.gos}
an.av.desc = {'customizeAnimationLayers': '1', 'baseAnimationLayers': [
    {'type': '5', 'isDefault': '0', 'animatorController': R(FX, 9100000)}]}
c_root, c_acc = component({}, sc.root), component({}, acc)
u._VRCF_FEATURES[id(c_root)] = u.vrcf_upgrade(toggle('Halo', [
    {'@class': 'AnimationClipAction', 'clip': {'id': HALO + ':7400000'}}]))
an.vrcf = vf = u.VRCFury(an, [c_root])
vf.feats.append((c_acc, {'@class': 'FullController', 'version': 3, 'controllers': [
    {'controller': {'id': FULL}, 'type': '5'}], 'rewriteBindings': [{'from': 'Old', 'to': 'Armature'}]}))
moved, scaled = u.animated_transforms(an)


def names(ids):
    return sorted(u.av_path(an.av, g) for g in an.av.gos if id(g) in ids)
check('moved or turned: euler and position curves (not a quaternion one), a rewritten path found from where the Full '
      'Controller is, a toggle\'s clip', names(moved), ['Acc/Armature/Bow', 'Armature/Hips/Ear', 'Armature/Hips/Fin',
                                                        'Armature/Hips/Halo'])
check('scaled: a blend tree\'s clip', names(scaled), ['Armature/Hips/Wing'])

print('all passed' if not FAILS else '%d FAILED: %s' % (len(FAILS), ', '.join(FAILS)))
sys.exit(1 if FAILS else 0)
