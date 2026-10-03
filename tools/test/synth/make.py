# make.py: a synthetic VRChat avatar project, to test unity2hypr3d on what the robot sample lacks
#   blender -b --factory-startup --python-exit-code 1 -P make.py -- PROJ
# SynthAvatar.prefab: unpacked (every object written out); SynthVariant.prefab: a variant of the FBX
import bpy, bmesh, sys, os, math, hashlib, shutil, struct, json
import numpy as np
from mathutils import Matrix, Vector, Quaternion

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))  # tools/
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import unity2hypr3d as u
import unitygen
from unitygen import F, R, V, Q, C, emit, HEAD, doc, base, native, write_psd

argv = sys.argv[sys.argv.index('--') + 1:] if '--' in sys.argv else []
PROJ = os.path.abspath(argv[0] if argv else 'proj')
if os.path.isdir(PROJ):
    shutil.rmtree(PROJ)
for d in ('ProjectSettings', 'Packages', 'Assets/Synth/Materials', 'Assets/Synth/Textures', 'Assets/Synth/Shaders',
          'Assets/Synth/Animations', 'Assets/Synth/Scripts'):
    os.makedirs(os.path.join(PROJ, d), exist_ok=True)
open(os.path.join(PROJ, 'ProjectSettings/ProjectVersion.txt'), 'w').write(
    'm_EditorVersion: 2022.3.22f1\nm_EditorVersionWithRevision: 2022.3.22f1 (887be4894c44)\n')
open(os.path.join(PROJ, 'Packages/manifest.json'), 'w').write('{"dependencies": {}}\n')


def G(key):
    return hashlib.md5(('synth/' + key).encode()).hexdigest()


newid = unitygen.Ids(7)  # fileIDs like Unity's: big, of either sign


def write(rel, data, guid, importer, imp=None):
    path = os.path.join(PROJ, 'Assets/Synth', rel)
    with open(path, 'wb' if isinstance(data, bytes) else 'w') as f:
        f.write(data)
    if imp is None:
        imp = {'externalObjects': {}, 'userData': '', 'assetBundleName': '', 'assetBundleVariant': ''}
    meta = {'fileFormatVersion': 2, 'guid': guid, importer: imp}
    with open(path + '.meta', 'w') as f:
        f.write('\n'.join(emit(meta)) + '\n')
    return path


# ---------------------------------------------------------------- the model, in Blender

for o in list(bpy.data.objects):
    bpy.data.objects.remove(o, do_unlink=True)
MATS = ('Skin', 'Face', 'Hat', 'Glasses', 'Jacket', 'Skirt', 'Hair', 'Badge')
MAT = {n: bpy.data.materials.new(n) for n in MATS}

ad = bpy.data.armatures.new('Armature')
arm = bpy.data.objects.new('Armature', ad)
bpy.context.scene.collection.objects.link(arm)
bpy.context.view_layer.objects.active = arm
arm.select_set(True)
bpy.ops.object.mode_set(mode='EDIT')
BONES = []


def bone(name, head, tail, parent=None, connect=False):
    b = ad.edit_bones.new(name)
    b.head, b.tail, b.roll = Vector(head), Vector(tail), 0.0
    if parent:
        b.parent = ad.edit_bones[parent]
        b.use_connect = connect
    assert b.name == name, b.name
    BONES.append(name)


# facing -Y, its left at +X
bone('Hips', (0, 0, 0.95), (0, 0, 1.05))
bone('Spine', (0, 0, 1.05), (0, 0, 1.2), 'Hips', True)
bone('Chest', (0, 0, 1.2), (0, 0, 1.38), 'Spine', True)
bone('Neck', (0, 0, 1.42), (0, 0, 1.5), 'Chest')
bone('Head', (0, 0, 1.5), (0, 0, 1.72), 'Neck', True)
bone('Eye_L', (0.035, -0.07, 1.61), (0.035, -0.1, 1.61), 'Head')
bone('Eye_R', (-0.035, -0.07, 1.61), (-0.035, -0.1, 1.61), 'Head')
bone('Hair_1', (0, 0.08, 1.68), (0, 0.11, 1.52), 'Head')
bone('Hair_1.001', (0, 0.11, 1.52), (0, 0.12, 1.38), 'Hair_1', True)
bone('Hair_1.002', (0, 0.12, 1.38), (0, 0.12, 1.26), 'Hair_1.001', True)
for s, x in (('L', 1), ('R', -1)):
    bone('Thigh_' + s, (0.09 * x, 0, 0.93), (0.09 * x, 0, 0.52), 'Hips')
    bone('Knee_' + s, (0.09 * x, 0, 0.52), (0.09 * x, 0.01, 0.1), 'Thigh_' + s, True)
    bone('Ankle_' + s, (0.09 * x, 0.01, 0.1), (0.09 * x, -0.1, 0.02), 'Knee_' + s, True)
    bone('Shoulder_' + s, (0.03 * x, 0, 1.36), (0.13 * x, 0, 1.38), 'Chest')
    bone('UpperArm_' + s, (0.13 * x, 0, 1.38), (0.4 * x, 0, 1.38), 'Shoulder_' + s, True)
    bone('Elbow_' + s, (0.4 * x, 0, 1.38), (0.64 * x, 0, 1.38), 'UpperArm_' + s, True)
    bone('Wrist_' + s, (0.64 * x, 0, 1.38), (0.74 * x, 0, 1.38), 'Elbow_' + s, True)
bone('Skirt', (0, -0.11, 0.92), (0, -0.15, 0.72), 'Hips')
bone('Skirt.001', (0, -0.15, 0.72), (0, -0.18, 0.52), 'Skirt', True)
bpy.ops.object.mode_set(mode='OBJECT')

HUMAN = {'Hips': 'Hips', 'Spine': 'Spine', 'Chest': 'Chest', 'Neck': 'Neck', 'Head': 'Head',
         'LeftEye': 'Eye_L', 'RightEye': 'Eye_R'}
for s, side in (('L', 'Left'), ('R', 'Right')):
    HUMAN.update({side + 'UpperLeg': 'Thigh_' + s, side + 'LowerLeg': 'Knee_' + s, side + 'Foot': 'Ankle_' + s,
                  side + 'Shoulder': 'Shoulder_' + s, side + 'UpperArm': 'UpperArm_' + s,
                  side + 'LowerArm': 'Elbow_' + s, side + 'Hand': 'Wrist_' + s})


def seg_dist(p, a, b):
    ab = b - a
    t = max(0.0, min(1.0, (p - a).dot(ab) / ab.length_squared))
    return (a + ab * t - p).length


def along(a, b, sx=1.0, sy=1.0):
    a, b = Vector(a), Vector(b)
    d = b - a
    rot = Vector((0, 0, 1)).rotation_difference(d.normalized()).to_matrix().to_4x4()
    return Matrix.Translation((a + b) / 2) @ rot @ Matrix.Diagonal((sx, sy, 1, 1)), d.length


class Parts:
    def __init__(self):
        self.bm = bmesh.new()
        self.bm.loops.layers.uv.new('UVMap')
        self.parts = {}

    def add(self, name, kind, M, mi=0, **kw):
        bm = self.bm
        if kind == 'cyl':
            vs = bmesh.ops.create_cone(bm, cap_ends=kw.get('caps', True), cap_tris=False, segments=kw.get('seg', 12),
                                       radius1=kw['r1'], radius2=kw.get('r2', kw['r1']), depth=kw['depth'],
                                       matrix=M, calc_uvs=True)['verts']
        elif kind == 'sphere':
            vs = bmesh.ops.create_uvsphere(bm, u_segments=kw.get('us', 16), v_segments=kw.get('vs', 12),
                                           radius=kw['r'], matrix=M, calc_uvs=True)['verts']
        else:
            vs = bmesh.ops.create_cube(bm, size=1.0, matrix=M, calc_uvs=True)['verts']
        for f in {f for v in vs for f in v.link_faces}:
            f.material_index = mi
        self.parts.setdefault(name, []).extend(vs)
        return vs

    def make(self, name, mats, faces_mi=None):
        bm = self.bm
        if faces_mi:
            for f in bm.faces:
                m = faces_mi(f)
                if m is not None:
                    f.material_index = m
        bm.verts.index_update()
        idx = {k: sorted({v.index for v in vs if v.is_valid}) for k, vs in self.parts.items()}
        me = bpy.data.meshes.new(name)
        bm.to_mesh(me)
        bm.free()
        ob = bpy.data.objects.new(name, me)
        bpy.context.scene.collection.objects.link(ob)
        for m in mats:
            me.materials.append(MAT[m])
        return ob, idx


def skin(ob, bones):
    ob.parent = arm
    ob.modifiers.new('Armature', 'ARMATURE').object = arm
    groups = {b: ob.vertex_groups.new(name=b) for b in bones}
    segs = {b: (ad.bones[b].head_local, ad.bones[b].tail_local) for b in bones}
    for v in ob.data.vertices:
        best = min(bones, key=lambda b: seg_dist(v.co, *segs[b]))
        groups[best].add([v.index], 1.0, 'REPLACE')


def attach(ob, bone_name, at):
    ob.parent = arm
    ob.parent_type = 'BONE'
    ob.parent_bone = bone_name
    bpy.context.view_layer.update()
    ob.matrix_world = Matrix.Translation(at)


def shape(ob, name, fn):
    if ob.data.shape_keys is None:
        ob.shape_key_add(name='Basis')
    k = ob.shape_key_add(name=name, from_mix=False)
    n = 0
    for i, v in enumerate(ob.data.vertices):
        co = fn(v.co.copy())
        if co is not None and (co - v.co).length > 1e-7:
            k.data[i].co = co
            n += 1
    assert n, name
    return k


# the body: skin, and a face on the front of the head
P = Parts()
M, d = along((0, 0, 0.9), (0, 0, 1.42), 1.0, 0.7)
P.add('torso', 'cyl', M, r1=0.13, depth=d)
M, d = along((0, 0, 1.4), (0, 0, 1.53))
P.add('neck', 'cyl', M, r1=0.045, depth=d)
P.add('head', 'sphere', Matrix.Translation((0, 0, 1.62)), r=0.11, us=20, vs=14)
for x in (1, -1):
    M, d = along((0.1 * x, 0, 1.38), (0.64 * x, 0, 1.38))
    P.add('arm', 'cyl', M, r1=0.045, depth=d)
    P.add('hand', 'cube', Matrix.Translation((0.7 * x, 0, 1.38)) @ Matrix.Diagonal((0.1, 0.03, 0.07, 1)))
    M, d = along((0.09 * x, 0, 0.95), (0.09 * x, 0, 0.08))
    P.add('leg', 'cyl', M, r1=0.06, depth=d)
    P.add('foot', 'cube', Matrix.Translation((0.09 * x, -0.05, 0.04)) @ Matrix.Diagonal((0.08, 0.2, 0.08, 1)))
body, idx = P.make('Body', ['Skin', 'Face'])
hv = set(idx['head'])
for p in body.data.polygons:
    if all(i in hv for i in p.vertices) and p.center.y < -0.05:
        p.material_index = 1
skin(body, ['Hips', 'Spine', 'Chest', 'Neck', 'Head'] +
     [b + s for s in ('_L', '_R') for b in ('Thigh', 'Knee', 'Ankle', 'Shoulder', 'UpperArm', 'Elbow', 'Wrist')])
torso = set(idx['torso'])


def region(y1, z0, z1, xs=None):
    def ok(i, co):
        return i in hv and co.y < y1 and z0 <= co.z < z1 and (xs is None or xs(co.x))
    return ok



def body_shape(name, ok, move):
    moved = {i: move(v.co.copy()) for i, v in enumerate(body.data.vertices) if ok(i, v.co)}
    if body.data.shape_keys is None:
        body.shape_key_add(name='Basis')
    k = body.shape_key_add(name=name, from_mix=False)
    for i, co in moved.items():
        k.data[i].co = co
    assert any((k.data[i].co - body.data.vertices[i].co).length > 1e-7 for i in moved), name


mouth = region(-0.06, 1.545, 1.6)
eyes = region(-0.06, 1.6, 1.655)
brow = region(-0.05, 1.655, 1.7)
VIS = ['sil', 'pp', 'ff', 'th', 'dd', 'kk', 'ch', 'ss', 'nn', 'rr', 'aa', 'ee', 'ih', 'oh', 'ou']
VMOVE = {'aa': lambda c: c + Vector((0, 0, -0.015)) if c.z < 1.575 else c,
         'ee': lambda c: Vector((c.x * 1.15, c.y, c.z)), 'ih': lambda c: c + Vector((0, 0, -0.008)),
         'oh': lambda c: Vector((c.x * 0.85, c.y, c.z - 0.01)), 'ou': lambda c: Vector((c.x * 0.75, c.y - 0.01, c.z)),
         'pp': lambda c: Vector((c.x * 0.9, c.y, c.z))}
for k, v in enumerate(VIS):
    body_shape('vrc.v_' + v, mouth, VMOVE.get(v, lambda c, k=k: c + Vector((0, 0, -0.001 * (k + 1)))))
body_shape('Blink', eyes, lambda c: c + Vector((0, 0.004, -0.008)))
body_shape('Smile', region(-0.06, 1.545, 1.6, lambda x: abs(x) > 0.015), lambda c: c + Vector((0, 0, 0.01)))
body_shape('Angry', brow, lambda c: c + Vector((0, 0, -0.01)))
body_shape('Surprised', brow, lambda c: c + Vector((0, 0, 0.012)))
body_shape('Wink_L', region(-0.06, 1.6, 1.655, lambda x: x > 0), lambda c: c + Vector((0, 0.004, -0.01)))
body_shape('Shrink', lambda i, co: i in torso, lambda c: Vector((c.x * 0.93, c.y * 0.93, c.z)))
BODY_SHAPES = [k.name for k in body.data.shape_keys.key_blocks[1:]]

# a hat on the head bone and a badge on the chest bone: rigid, not skinned
P = Parts()
P.add('crown', 'cyl', Matrix.Translation((0, 0, 0.1)), r1=0.12, r2=0.02, depth=0.2, seg=16)
P.add('brim', 'cyl', Matrix.Identity(4), r1=0.19, depth=0.012, seg=24)
hat, _ = P.make('Hat', ['Hat'])
attach(hat, 'Head', (0, 0, 1.72))
P = Parts()
P.add('badge', 'cube', Matrix.Diagonal((0.04, 0.012, 0.04, 1)))
badge, _ = P.make('Badge', ['Badge'])
attach(badge, 'Chest', (0.06, -0.1, 1.28))

P = Parts()
P.add('bar', 'cube', Matrix.Translation((0, -0.112, 1.61)) @ Matrix.Diagonal((0.16, 0.008, 0.012, 1)))
for x in (1, -1):
    P.add('lens', 'cube', Matrix.Translation((0.035 * x, -0.115, 1.6)) @ Matrix.Diagonal((0.05, 0.004, 0.035, 1)))
glasses, _ = P.make('Glasses', ['Glasses'])
skin(glasses, ['Head'])

P = Parts()
M, d = along((0, 0, 1.0), (0, 0, 1.43), 1.0, 0.72)
P.add('coat', 'cyl', M, r1=0.142, depth=d)
for x in (1, -1):
    M, d = along((0.11 * x, 0, 1.38), (0.42 * x, 0, 1.38))
    P.add('sleeve', 'cyl', M, r1=0.055, depth=d)
jacket, _ = P.make('Jacket', ['Jacket'])
skin(jacket, ['Hips', 'Spine', 'Chest', 'Shoulder_L', 'Shoulder_R', 'UpperArm_L', 'UpperArm_R'])

P = Parts()
P.add('skirt', 'cyl', Matrix.Translation((0, 0, 0.785)) @ Matrix.Diagonal((1, 0.8, 1, 1)), r1=0.22, r2=0.145,
      depth=0.33, seg=20, caps=False)
skirt, _ = P.make('Skirt', ['Skirt'])
skin(skirt, ['Hips', 'Skirt', 'Skirt.001', 'Thigh_L', 'Thigh_R'])
shape(skirt, 'Flare', lambda c: Vector((c.x * 1.2, c.y * 1.2, c.z)) if c.z < 0.7 else None)

P = Parts()
P.add('hair', 'cyl', Matrix.Translation((0, 0.02, 1.5)), r1=0.125, depth=0.42, seg=16, caps=False)
hairm, _ = P.make('HairMesh', ['Hair'])
bm = bmesh.new()
bm.from_mesh(hairm.data)
bmesh.ops.delete(bm, geom=[f for f in bm.faces if f.calc_center_median().y < 0.0], context='FACES')
bm.to_mesh(hairm.data)
bm.free()
skin(hairm, ['Head', 'Hair_1', 'Hair_1.001', 'Hair_1.002'])

# ---------------------------------------------------------------- textures

TEX = {}


def png(name, w, h, fn, ext='png', **meta):
    arr = np.zeros((h, w, 4), dtype=np.float32)
    yy, xx = np.mgrid[0:h, 0:w]
    fn(arr, xx / max(w - 1, 1), yy / max(h - 1, 1))
    path = os.path.join(PROJ, 'Assets/Synth/Textures', name + '.' + ext)
    if ext == 'psd':
        write_psd(path, arr)
    else:
        im = bpy.data.images.new(name, w, h, alpha=True)
        im.pixels.foreach_set(arr.ravel())
        im.filepath_raw = path
        im.file_format = {'png': 'PNG', 'tga': 'TARGA'}[ext]
        im.save()
        bpy.data.images.remove(im)
    g = G('tex/' + name)
    plat = []
    for target, size, over in (('DefaultTexturePlatform', meta.get('default', 2048), 0),
                               ('Standalone', meta.get('standalone', 2048), 1 if 'standalone' in meta else 0),
                               ('Android', 512, 1)):
        plat.append({'serializedVersion': 3, 'buildTarget': target, 'maxTextureSize': size, 'resizeAlgorithm': 0,
                     'textureFormat': -1, 'textureCompression': 1, 'compressionQuality': 50,
                     'crunchedCompression': 0, 'allowsAlphaSplitting': 0, 'overridden': over,
                     'ignorePlatformSupport': 0, 'androidETC2FallbackOverride': 0,
                     'forceMaximumCompressionQuality_BC6H_BC7': 0})
    imp = {'internalIDToNameTable': [], 'externalObjects': {}, 'serializedVersion': 12,
           'mipmaps': {'mipMapMode': 0, 'enableMipMap': 1, 'sRGBTexture': 1, 'linearTexture': 0},
           'isReadable': 0, 'maxTextureSize': meta.get('default', 2048),
           'textureSettings': {'serializedVersion': 2, 'filterMode': 1, 'aniso': 1, 'mipBias': 0,
                               'wrapU': 0, 'wrapV': 0, 'wrapW': 0},
           'alphaUsage': meta.get('alpha', 1), 'alphaIsTransparency': meta.get('alpha', 1),
           'textureType': 0, 'textureShape': 1, 'platformSettings': plat,
           'userData': '', 'assetBundleName': '', 'assetBundleVariant': ''}
    with open(path + '.meta', 'w') as f:
        f.write('\n'.join(emit({'fileFormatVersion': 2, 'guid': g, 'TextureImporter': imp})) + '\n')
    TEX[name] = g
    return g


def checker(c1, c2, n=8, alpha=None):
    def fn(a, x, y):
        k = ((np.floor(x * n) + np.floor(y * n)) % 2)[..., None]
        a[...] = np.where(k > 0, np.array(c1 + (1.0,)), np.array(c2 + (1.0,)))
        if alpha is not None:
            a[..., 3] = alpha(x, y)
    return fn


png('skin', 64, 64, checker((1.0, 0.85, 0.75), (0.95, 0.78, 0.68)), ext='psd')
png('face', 128, 128, checker((1.0, 0.9, 0.85), (0.3, 0.2, 0.2), 4, lambda x, y: (x > 0.5) * 1.0), alpha=0)
png('hat', 64, 64, checker((0.2, 0.3, 0.9), (0.9, 0.9, 0.2), 4, lambda x, y: (y < 0.8) * 1.0))
png('big', 4096, 64, checker((0.2, 0.6, 0.3), (0.1, 0.4, 0.2), 16), default=8192)
png('emit', 64, 64, checker((1.0, 1.0, 1.0), (0.0, 0.0, 0.0), 2), standalone=32)
png('hair', 512, 512, checker((0.35, 0.2, 0.1), (0.3, 0.15, 0.08), 16, lambda x, y: (y < 0.9) * 1.0), ext='tga', default=256)

# ---------------------------------------------------------------- the FBX

FBX = os.path.join(PROJ, 'Assets/Synth/Synth.fbx')
FBX_GUID = G('Synth.fbx')
bpy.ops.object.select_all(action='DESELECT')
bpy.ops.export_scene.fbx(filepath=FBX, use_selection=False, object_types={'ARMATURE', 'MESH'},
                         apply_unit_scale=True, apply_scale_options='FBX_SCALE_NONE', global_scale=1.0,
                         axis_forward='-Z', axis_up='Y', add_leaf_bones=False, bake_anim=False,
                         primary_bone_axis='Y', secondary_bone_axis='X', use_mesh_modifiers=False,
                         mesh_smooth_type='FACE', use_armature_deform_only=False, path_mode='AUTO',
                         embed_textures=False)

# read it back the way the converter does: the node tree, and every node's matrix as Unity has it
for coll in (bpy.data.objects, bpy.data.meshes, bpy.data.materials, bpy.data.images, bpy.data.armatures):
    for x in list(coll):
        coll.remove(x)
info = u.FBXInfo(FBX, 'Assets/Synth/Synth.fbx')
fi = u.FBXInst('synth', FBX_GUID, info, False, None)
fi.u = info.unit / 100.0
bpy.ops.import_scene.fbx(
    filepath=FBX, global_scale=1.0, use_custom_normals=True, use_image_search=True,
    ignore_leaf_bones=False, automatic_bone_orientation=False, force_connect_children=False,
    use_anim=False, use_custom_props=False, use_prepost_rot=True, use_manual_orientation=False,
    bake_space_transform=False, primary_bone_axis='Y', secondary_bone_axis='X', mtl_name_collision_mode='MAKE_UNIQUE')
b = u.Build.__new__(u.Build)
b.owner = {}
b.map_models(fi, list(bpy.data.objects))
bpy.context.view_layer.update()
orig = {}
for mid, x in fi.bl.items():
    Mw = x[1].matrix_world @ x[1].pose.bones[x[2]].matrix if x[0] == 'bone' else x[1].matrix_world
    orig[mid] = u.to_unity(Mw, fi.u)
lost = [info.name(m) for m in info.order if m not in orig]
assert not lost, lost
print('synth: FBX unit %g, %d models' % (info.unit, len(info.order)))

top = info.kids.get(0, [])
collapsed = len(top) == 1 and top[0] not in info.mesh and info.kind(top[0]) in ('Null', 'Root', '')
root_mid = top[0] if collapsed else 0
MID = {}
for m in info.order:
    MID.setdefault(info.name(m), m)
# the Skirt bone shares its name with the Skirt mesh, which comes first in the file
SKIRT_BONE = next(m for m in info.order if info.name(m) == 'Skirt' and m not in info.mesh)


def trs(M):
    t, q, s = M.decompose()
    return V(t.x, t.y, t.z), Q(q.x, q.y, q.z, q.w), V(s.x, s.y, s.z)


def local(mid):
    if mid == root_mid:
        return orig[mid] if mid else Matrix.Identity(4)
    p = info.models[mid][2]
    return (orig[p].inverted() if p else Matrix.Identity(4)) @ orig[mid]


def lossy(mid):
    Mw = orig[mid]
    return max(Mw.col[0].xyz.length, Mw.col[1].xyz.length, Mw.col[2].xyz.length)


# ---------------------------------------------------------------- the animations, controller, menus

CLIP = {}


def clip(name, curves):
    fc = []
    for path, cid, attr, val in curves:
        keys = [{'serializedVersion': 3, 'time': t, 'value': val, 'inSlope': 0, 'outSlope': 0,
                 'tangentMode': 136 if cid == 137 else 103, 'weightedMode': 0,
                 'inWeight': 0.33333334, 'outWeight': 0.33333334} for t in (0.0, 0.016666668)]
        fc.append({'curve': {'serializedVersion': 2, 'm_Curve': keys, 'm_PreInfinity': 2, 'm_PostInfinity': 2,
                             'm_RotationOrder': 4},
                   'attribute': attr, 'path': path, 'classID': cid, 'script': R(0), 'flags': 0})
    body = base(m_Name=name, serializedVersion=7, m_Legacy=0, m_Compressed=0, m_UseHighQualityCurve=1,
                m_RotationCurves=[], m_CompressedRotationCurves=[], m_EulerCurves=[], m_PositionCurves=[],
                m_ScaleCurves=[], m_FloatCurves=fc, m_PPtrCurves=[], m_SampleRate=60, m_WrapMode=0,
                m_Bounds={'m_Center': V(0, 0, 0), 'm_Extent': V(0, 0, 0)},
                m_ClipBindingConstant={'genericBindings': [], 'pptrCurveMapping': []},
                m_AnimationClipSettings={'serializedVersion': 2, 'm_AdditiveReferencePoseClip': R(0),
                                         'm_AdditiveReferencePoseTime': 0, 'm_StartTime': 0,
                                         'm_StopTime': 0.016666668, 'm_OrientationOffsetY': 0, 'm_Level': 0,
                                         'm_CycleOffset': 0, 'm_HasAdditiveReferencePose': 0, 'm_LoopTime': 0,
                                         'm_LoopBlend': 0, 'm_LoopBlendOrientation': 0,
                                         'm_LoopBlendPositionY': 0, 'm_LoopBlendPositionXZ': 0,
                                         'm_KeepOriginalOrientation': 0, 'm_KeepOriginalPositionY': 1,
                                         'm_KeepOriginalPositionXZ': 0, 'm_HeightFromFeet': 0, 'm_Mirror': 0},
                m_EditorCurves=fc, m_EulerEditorCurves=[], m_HasGenericRootTransform=0,
                m_HasMotionFloatCurves=0, m_Events=[])
    g = G('anim/' + name)
    write('Animations/%s.anim' % name, HEAD + doc(74, 7400000, 'AnimationClip', body), g, 'NativeFormatImporter',
          native(7400000))
    CLIP[name] = g


CTL = []


def state(name, motion=None, behaviours=()):
    sid = newid()
    body = base(serializedVersion=6, m_Name=name, m_Speed=1, m_CycleOffset=0, m_Transitions=[],
                m_StateMachineBehaviours=[R(x) for x in behaviours], m_Position=V(50, 50, 0), m_IKOnFeet=0,
                m_WriteDefaultValues=0, m_Mirror=0, m_SpeedParameterActive=0, m_MirrorParameterActive=0,
                m_CycleOffsetParameterActive=0, m_TimeParameterActive=0,
                m_Motion=R(7400000, CLIP[motion], 2) if motion else R(0), m_Tag='', m_SpeedParameter='',
                m_MirrorParameter='', m_CycleOffsetParameter='', m_TimeParameter='')
    CTL.append((1102, sid, 'AnimatorState', body))
    return sid, body


def transition(dst, conds, self_ok=1):
    tid = newid()
    body = base(m_Name='', m_Conditions=[{'m_ConditionMode': m, 'm_ConditionEvent': p, 'm_EventTreshold': t}
                                         for m, p, t in conds],
                m_DstStateMachine=R(0), m_DstState=R(dst), m_Solo=0, m_Mute=0, m_IsExit=0, serializedVersion=3,
                m_TransitionDuration=0.1, m_TransitionOffset=0, m_ExitTime=0.75, m_HasExitTime=0,
                m_HasFixedDuration=1, m_InterruptionSource=0, m_OrderedInterruption=1,
                m_CanTransitionToSelf=self_ok)
    CTL.append((1101, tid, 'AnimatorStateTransition', body))
    return tid


def machine(name, states, default, anys=()):
    mid = newid()
    body = base(serializedVersion=6, m_Name=name,
                m_ChildStates=[{'serializedVersion': 1, 'm_State': R(s), 'm_Position': V(300, 60 * i, 0)}
                               for i, s in enumerate(states)],
                m_ChildStateMachines=[], m_AnyStateTransitions=[R(t) for t in anys], m_EntryTransitions=[],
                m_StateMachineTransitions={}, m_StateMachineBehaviours=[], m_AnyStatePosition=V(50, 20, 0),
                m_EntryPosition=V(50, 120, 0), m_ExitPosition=V(800, 120, 0),
                m_ParentStateMachinePosition=V(800, 20, 0), m_DefaultState=R(default))
    CTL.append((1107, mid, 'AnimatorStateMachine', body))
    return mid


def tracking(eyes=2, mouth=0):
    tid = newid()
    body = base(m_GameObject=R(0), m_Enabled=1, m_EditorHideFlags=0, m_Script=R(u.TRACKING_FID, u.DESC_GUID, 3),
                m_Name='', m_EditorClassIdentifier='', trackingHead=0, trackingLeftHand=0, trackingRightHand=0,
                trackingHip=0, trackingLeftFoot=0, trackingRightFoot=0, trackingLeftFingers=0,
                trackingRightFingers=0, trackingEyes=eyes, trackingMouth=mouth, debugString='')
    body['m_ObjectHideFlags'] = 1
    CTL.append((114, tid, 'MonoBehaviour', body))
    return tid


LAYERS = []


def layer(name, sm, weight=1.0):
    LAYERS.append({'serializedVersion': 5, 'm_Name': name, 'm_StateMachine': R(sm), 'm_Mask': R(0),
                   'm_Motions': [], 'm_Behaviours': [], 'm_BlendingMode': 0, 'm_SyncedLayerIndex': -1,
                   'm_DefaultWeight': weight, 'm_IKPass': 0, 'm_SyncedLayerAffectsTiming': 0,
                   'm_Controller': R(9100000)})


GESTURE_STATES = ['Fist', 'Open', 'Point', 'Victory', 'RockNRoll', 'HandGun', 'ThumbsUp']


def hand(side, faces, self_ok):
    p = 'Gesture' + side
    idle, _ = state('Idle')
    sts, anys = [idle], [transition(idle, [(6, p, 0)], self_ok)]
    for g, st in enumerate(GESTURE_STATES, 1):
        clip_name, track = faces.get(st, (None, False))
        s, _ = state(st, clip_name, [tracking()] if track else [])
        sts.append(s)
        anys.append(transition(s, [(6, p, g)], self_ok))
    layer(side + ' Hand', machine(side + ' Hand', sts, idle, anys))


def toggle_layer(name, param, on_clip, off_clip):
    off, offb = state(name + ' Off', off_clip)
    on, onb = state(name + ' On', on_clip)
    offb['m_Transitions'].append(R(transition(on, [(1, param, 0)])))
    onb['m_Transitions'].append(R(transition(off, [(2, param, 0)])))
    layer(name, machine(name, [off, on], off))


# ---------------------------------------------------------------- the Unity objects of the avatar

class UO:
    def __init__(self, key, name, parent, tr, active=True):
        self.key, self.name, self.parent = key, name, parent
        self.pos, self.rot, self.scl = tr
        self.active = active
        self.go, self.tf = newid(), newid()
        self.comps = []  # [(cls, fid, kind, body)]
        self.children = []
        if parent is not None:
            parent.children.append(self)

    def add(self, cls, kind, body):
        fid = newid()
        body = dict(body)
        body['m_GameObject'] = R(self.go)
        self.comps.append((cls, fid, kind, body))
        return fid

    @property
    def path(self):
        parts, o = [], self
        while o.parent is not None:
            parts.append(o.name)
            o = o.parent
        return '/'.join(reversed(parts))


BY = {}  # FBX node name -> UO (the first one)
UOS = {}  # FBX model id -> UO
MESH_OF = {n: MID[n] for n in ('Body', 'Hat', 'Badge', 'Glasses', 'Jacket', 'Skirt', 'HairMesh')}
for n, m in MESH_OF.items():
    assert m in info.mesh, n
RENAME = {MESH_OF['Skirt']: 'Skirt Outfit'}
INACTIVE = {MESH_OF['Badge'], MESH_OF['Glasses'], MESH_OF['Skirt']}


def tree(mid, parent):
    name = 'SynthAvatar' if mid == root_mid else RENAME.get(mid, info.name(mid))
    o = UO(mid, name, parent, trs(local(mid)), mid not in INACTIVE)
    if mid:
        BY.setdefault(info.name(mid), o)
        UOS[mid] = o
    for k in (top if mid == 0 else info.kids.get(mid, [])):
        tree(k, o)
    return o


ROOT = tree(root_mid, None)
hat_o = UOS[MESH_OF['Hat']]
hat_o.scl = V(hat_o.scl['x'] * 1.2, hat_o.scl['y'] * 1.2, hat_o.scl['z'] * 1.2)
print('synth: FBX root %s, %s' % ('collapsed' if collapsed else 'kept', ', '.join(o.name for o in ROOT.children)))
print('synth: bone scale in Unity %.4g' % lossy(MID['Head']))

# clips, now that the paths are known
P_HAT, P_GL, P_JK, P_SK, P_BODY = (UOS[MESH_OF[n]].path for n in ('Hat', 'Glasses', 'Jacket', 'Skirt', 'Body'))
assert BY['Skirt'] is not UOS[MESH_OF['Skirt']] and P_SK == 'Skirt Outfit', P_SK
clip('Angry', [(P_BODY, 137, 'blendShape.Angry', 100)])
clip('Surprised', [(P_BODY, 137, 'blendShape.Surprised', 100)])
clip('Smile', [(P_BODY, 137, 'blendShape.Smile', 100)])
clip('Victory', [(P_BODY, 137, 'blendShape.Smile', 100), (P_BODY, 137, 'blendShape.Wink_L', 100)])
clip('HatOn', [(P_HAT, 1, 'm_IsActive', 1)])
clip('HatOff', [(P_HAT, 1, 'm_IsActive', 0)])
clip('GlassesOn', [(P_GL, 1, 'm_IsActive', 1)])
clip('GlassesOff', [(P_GL, 1, 'm_IsActive', 0)])
clip('Outfit0', [(P_JK, 1, 'm_IsActive', 0), (P_SK, 1, 'm_IsActive', 0), (P_BODY, 137, 'blendShape.Shrink', 0)])
clip('Outfit1', [(P_JK, 1, 'm_IsActive', 1), (P_SK, 1, 'm_IsActive', 0), (P_BODY, 137, 'blendShape.Shrink', 0)])
clip('Outfit2', [(P_JK, 1, 'm_IsActive', 0), (P_SK, 1, 'm_IsActive', 1), (P_BODY, 137, 'blendShape.Shrink', 100)])

empty, _ = state('Empty')
layer('Base', machine('Base', [empty], empty), 0.0)
hand('Left', {'Fist': ('Angry', True), 'Open': ('Surprised', False), 'Point': ('Smile', False),
              'Victory': ('Victory', False), 'HandGun': ('Angry', False), 'ThumbsUp': ('Smile', False)}, 0)
hand('Right', {'Fist': ('Angry', True), 'Open': ('Surprised', False), 'Victory': ('Victory', False),
               'RockNRoll': ('Smile', False), 'ThumbsUp': ('Smile', False)}, 1)
toggle_layer('Hat', 'Hat', 'HatOn', 'HatOff')
toggle_layer('Glasses', 'Glasses', 'GlassesOn', 'GlassesOff')
outs = [state('Outfit%d' % k, 'Outfit%d' % k)[0] for k in range(3)]
layer('Outfit', machine('Outfit', outs, outs[0], [transition(s, [(6, 'Outfit', k)], 0) for k, s in enumerate(outs)]))
PARAMS = [('GestureLeft', 3, 0), ('GestureRight', 3, 0), ('GestureLeftWeight', 1, 0), ('GestureRightWeight', 1, 0),
          ('Hat', 4, 1), ('Glasses', 4, 0), ('Outfit', 3, 1), ('HairColor', 1, 0)]
ctl = base(m_Name='FX', serializedVersion=5,
           m_AnimatorParameters=[{'m_Name': n, 'm_Type': t, 'm_DefaultFloat': float(v) if t == 1 else 0,
                                  'm_DefaultInt': v if t == 3 else 0, 'm_DefaultBool': v if t == 4 else 0,
                                  'm_Controller': R(9100000)} for n, t, v in PARAMS],
           m_AnimatorLayers=LAYERS)
FX_GUID = G('FX.controller')
write('FX.controller', HEAD + doc(91, 9100000, 'AnimatorController', ctl) +
      ''.join(doc(c, f, k, bd) for c, f, k, bd in CTL), FX_GUID, 'NativeFormatImporter', native(9100000))


def control(name, typ, param='', value=1, sub=None, subparams=()):
    return {'name': name, 'icon': R(0), 'type': typ, 'parameter': {'name': param}, 'value': value, 'style': 0,
            'subMenu': R(11400000, sub, 2) if sub else R(0), 'subParameters': [{'name': p} for p in subparams],
            'labels': []}


def asset114(name, script, **kw):
    return HEAD + doc(114, 11400000, 'MonoBehaviour', base(
        m_GameObject=R(0), m_Enabled=1, m_EditorHideFlags=0, m_Script=R(script, u.DESC_GUID, 3), m_Name=name,
        m_EditorClassIdentifier='', **kw))


SUB_GUID, MENU_GUID, PARAMS_GUID = G('OutfitMenu.asset'), G('Menu.asset'), G('Params.asset')
write('OutfitMenu.asset', asset114('OutfitMenu', u.MENU_FID, controls=[
    control('Jacket', 102, 'Outfit', 1), control('Skirt', 102, 'Outfit', 2), control('Nothing', 102, 'Outfit', 0)]),
    SUB_GUID, 'NativeFormatImporter', native(11400000))
write('Menu.asset', asset114('Menu', u.MENU_FID, controls=[
    control('<b>Hat</b>', 102, 'Hat', 1), control('Glasses', 102, 'Glasses', 1),
    control('Outfits', 103, '', 1, SUB_GUID), control('Hair Color', 203, '', 1, None, ['HairColor'])]),
    MENU_GUID, 'NativeFormatImporter', native(11400000))
write('Params.asset', asset114('Params', u.PARAMS_FID, isEmpty=0, parameters=[
    {'name': n, 'valueType': t, 'saved': 1, 'defaultValue': v, 'networkSynced': 1}
    for n, t, v in (('Hat', 2, 1), ('Glasses', 2, 0), ('Outfit', 0, 1), ('HairColor', 1, 0))]),
    PARAMS_GUID, 'NativeFormatImporter', native(11400000))

# ---------------------------------------------------------------- shaders, scripts, materials

SHADER = {}
for name, sh in (('lilToon', 'lilToon'), ('MToon', 'VRM/MToon')):
    SHADER[name] = G('shader/' + name)
    write('Shaders/%s.shader' % name, 'Shader "%s"\n{\n    Properties\n    {\n        _MainTex ("Texture", 2D) = '
          '"white" {}\n    }\n    SubShader { Pass { } }\n}\n' % sh, SHADER[name], 'ShaderImporter',
          {'externalObjects': {}, 'defaultTextures': [], 'nonModifiableTextures': [], 'userData': '',
           'assetBundleName': '', 'assetBundleVariant': ''})
SCRIPT = {}
for name in ('DynamicBone', 'DynamicBoneCollider'):
    SCRIPT[name] = G('script/' + name)
    write('Scripts/%s.cs' % name, 'public class %s : UnityEngine.MonoBehaviour {}\n' % name, SCRIPT[name],
          'MonoImporter', {'externalObjects': {}, 'serializedVersion': 2, 'defaultReferences': [],
                           'executionOrder': 0, 'icon': R(0), 'userData': '', 'assetBundleName': '',
                           'assetBundleVariant': ''})
STANDARD = R(46, u.BUILTIN_GUID, 0)
MATG = {}


def material(name, shader, tex=None, xf=((1, 1), (0, 0)), floats=(), colors=(), valid=(), old_kw='', rq=-1,
             emit_tex=None):
    envs = []
    for k, t, x in (('_MainTex', tex, xf), ('_EmissionMap', emit_tex, ((1, 1), (0, 0))),
                    ('_BumpMap', None, ((1, 1), (0, 0)))):
        envs.append({k: {'m_Texture': R(2800000, TEX[t], 3) if t else R(0), 'm_Scale': F(x=x[0][0], y=x[0][1]),
                         'm_Offset': F(x=x[1][0], y=x[1][1])}})
    body = base(serializedVersion=8, m_Name=name, m_Shader=shader, m_Parent=R(0), m_ModifiedSerializedProperties=0,
                m_ValidKeywords=list(valid), m_InvalidKeywords=[], m_LightmapFlags=4,
                m_EnableInstancingVariants=0, m_DoubleSidedGI=0, m_CustomRenderQueue=rq, stringTagMap={},
                disabledShaderPasses=[], m_LockedProperties='',
                m_SavedProperties={'serializedVersion': 3, 'm_TexEnvs': envs, 'm_Ints': [],
                                   'm_Floats': [{k: v} for k, v in floats],
                                   'm_Colors': [{k: C(*v)} for k, v in colors]},
                m_BuildTextureStacks=[])
    if old_kw:
        body['m_ShaderKeywords'] = old_kw
    g = MATG[name] = G('mat/' + name)
    write('Materials/%s.mat' % name, HEAD + doc(21, 2100000, 'Material', body), g, 'NativeFormatImporter',
          native(2100000))


material('Skin', STANDARD, 'skin', floats=[('_Mode', 0), ('_Glossiness', 0.2)], colors=[('_Color', (1, 0.85, 0.75, 1))])
material('Face', STANDARD, 'face', floats=[('_Mode', 1), ('_Cutoff', 0.5)], colors=[('_Color', (1, 1, 1, 1))],
         old_kw='_ALPHATEST_ON')
material('Hat', STANDARD, 'hat', ((2, 2), (0.25, 0.5)), floats=[('_Mode', 1), ('_Cutoff', 0.3)],
         colors=[('_Color', (1, 1, 1, 1))], old_kw='_ALPHATEST_ON', rq=2450)
material('Glasses', STANDARD, floats=[('_Mode', 2)], colors=[('_Color', (0.6, 0.8, 1.0, 0.35))],
         old_kw='_ALPHABLEND_ON', rq=3000)
material('Jacket', STANDARD, 'big', floats=[('_Mode', 0)], colors=[('_Color', (1, 1, 1, 1)), ('_EmissionColor', (2, 0.5, 0, 1))],
         valid=['_EMISSION'], emit_tex='emit')
material('Skirt', R(4800000, SHADER['lilToon'], 3), 'skin', floats=[('_Cull', 0), ('_TransparentMode', 0)],
         colors=[('_Color', (0.9, 0.2, 0.3, 1))])
material('Hair', R(4800000, SHADER['MToon'], 3), 'hair', floats=[('_BlendMode', 1), ('_Cutoff', 0.5), ('_CullMode', 2)],
         colors=[('_Color', (1, 1, 1, 1)), ('_ShadeColor', (0.3, 0.2, 0.2, 1)), ('_EmissionColor', (0, 0, 0, 1))])
material('Badge', STANDARD, floats=[('_Mode', 0)], colors=[('_Color', (1, 0.8, 0, 1))])

# ---------------------------------------------------------------- the FBX's import settings

human = [{'boneName': b, 'humanName': h, 'limit': {'min': V(0, 0, 0), 'max': V(0, 0, 0), 'value': V(0, 0, 0),
                                                   'length': 0, 'modified': 0}} for h, b in HUMAN.items()]
fbx_imp = {
    'serializedVersion': 22200,
    'internalIDToNameTable': [{'first': {'43': 4300000}, 'second': 'Body'}],
    'externalObjects': [{'first': {'type': 'UnityEngine:Material', 'assembly': 'UnityEngine.CoreModule', 'name': n},
                         'second': R(2100000, MATG[n], 2)} for n in MATS],
    'materials': {'materialImportMode': 1, 'materialName': 0, 'materialSearch': 1, 'materialLocation': 1},
    'animations': {'legacyGenerateAnimations': 4, 'bakeSimulation': 0, 'resampleCurves': 1, 'optimizeGameObjects': 0,
                   'removeConstantScaleCurves': 0, 'motionNodeName': '', 'rigImportErrors': '',
                   'rigImportWarnings': '', 'animationImportErrors': '', 'animationImportWarnings': '',
                   'animationRetargetingWarnings': '', 'animationDoRetargetingWarnings': 0,
                   'importAnimatedCustomProperties': 0, 'importConstraints': 0, 'animationCompression': 1,
                   'animationRotationError': 0.5, 'animationPositionError': 0.5, 'animationScaleError': 0.5,
                   'animationWrapMode': 0, 'extraExposedTransformPaths': [], 'extraUserProperties': [],
                   'clipAnimations': [], 'isReadable': 0},
    'meshes': {'lODScreenPercentages': [], 'globalScale': 1, 'meshCompression': 0, 'addColliders': 0,
               'useSRGBMaterialColor': 1, 'sortHierarchyByName': 1, 'importVisibility': 1, 'importBlendShapes': 1,
               'importCameras': 1, 'importLights': 1, 'nodeNameCollisionStrategy': 1, 'fileIdsGeneration': 2,
               'swapUVChannels': 0, 'generateSecondaryUV': 0, 'useFileUnits': 1, 'keepQuads': 0,
               'weldVertices': 1, 'bakeAxisConversion': 0, 'preserveHierarchy': 0, 'skinWeightsMode': 0,
               'maxBonesPerVertex': 4, 'minBoneWeight': 0.001, 'optimizeBones': 1, 'meshOptimizationFlags': -1,
               'indexFormat': 0, 'useFileScale': 1, 'strictVertexDataChecks': 0},
    'tangentSpace': {'normalSmoothAngle': 60, 'normalImportMode': 0, 'tangentImportMode': 3,
                     'normalCalculationMode': 4, 'legacyComputeAllNormalsFromSmoothingGroupsWhenMeshHasBlendShapes': 0,
                     'blendShapeNormalImportMode': 1, 'normalSmoothingSource': 0},
    'referencedClips': [], 'importAnimation': 0,
    'humanDescription': {'serializedVersion': 3, 'human': human, 'skeleton': [], 'armTwist': 0.5,
                         'foreArmTwist': 0.5, 'upperLegTwist': 0.5, 'legTwist': 0.5, 'armStretch': 0.05,
                         'legStretch': 0.05, 'feetSpacing': 0, 'globalScale': 1, 'rootMotionBoneName': '',
                         'hasTranslationDoF': 0, 'hasExtraRoot': 1, 'skeletonHasParents': 1},
    'lastHumanDescriptionAvatarSource': R(0), 'autoGenerateAvatarMappingIfUnspecified': 1, 'animationType': 3,
    'humanoidOversampling': 1, 'avatarSetup': 1, 'addHumanoidExtraRootOnlyWhenUsingAvatar': 1,
    'importBlendShapeDeformPercent': 1, 'remapMaterialsIfMaterialImportModeIsNone': 0, 'additionalBone': 0,
    'userData': '', 'assetBundleName': '', 'assetBundleVariant': ''}
with open(FBX + '.meta', 'w') as f:
    f.write('\n'.join(emit({'fileFormatVersion': 2, 'guid': FBX_GUID, 'ModelImporter': fbx_imp})) + '\n')

# ---------------------------------------------------------------- the avatar's components

S_HEAD, S_HAIR, S_HAND, S_THIGH = (lossy(MID[n]) for n in ('Head', 'Hair_1', 'Wrist_L', 'Thigh_L'))
body_shapes = info.mesh[MESH_OF['Body']]['shapes']
assert body_shapes == BODY_SHAPES, (body_shapes, BODY_SHAPES)
BLINK = body_shapes.index('Blink')


def descriptor(body_smr, eye_l, eye_r):
    layers = [{'isEnabled': 0, 'type': t, 'animatorController': R(0), 'mask': R(0), 'isDefault': 1}
              for t in (0, 2, 3, 4)]
    layers.append({'isEnabled': 0, 'type': 5, 'animatorController': R(9100000, FX_GUID, 2), 'mask': R(0),
                   'isDefault': 0})
    return dict(m_Enabled=1, m_EditorHideFlags=0, m_Script=R(u.DESC_FID, u.DESC_GUID, 3), m_Name='',
                m_EditorClassIdentifier='', Name='', ViewPosition=V(0, 1.61, 0.1), Animations=0, ScaleIPD=1,
                lipSync=3, lipSyncJawBone=R(0), lipSyncJawClosed=Q(0, 0, 0, 1), lipSyncJawOpen=Q(0, 0, 0, 1),
                VisemeSkinnedMesh=R(body_smr), MouthOpenBlendShapeName='Facial_Blends.Jaw_Down',
                VisemeBlendShapes=['vrc.v_' + v for v in VIS], unityVersion='',
                portraitCameraPositionOffset=V(0, 0, 0), portraitCameraRotationOffset=Q(0, 1, 0, 0),
                networkIDs=[], customExpressions=1, expressionsMenu=R(11400000, MENU_GUID, 2),
                expressionParameters=R(11400000, PARAMS_GUID, 2), enableEyeLook=1,
                customEyeLookSettings={'eyeMovement': {'confidence': 0.5, 'excitement': 0.5},
                                       'leftEye': R(eye_l), 'rightEye': R(eye_r),
                                       'eyesLookingStraight': {'linked': 1, 'left': Q(0, 0, 0, 1),
                                                               'right': Q(0, 0, 0, 1)},
                                       'eyelidType': 2, 'upperLeftEyelid': R(0), 'upperRightEyelid': R(0),
                                       'lowerLeftEyelid': R(0), 'lowerRightEyelid': R(0),
                                       'eyelidsSkinnedMesh': R(body_smr),
                                       'eyelidsBlendshapes': struct.pack('<iii', BLINK, -1, -1).hex()},
                customizeAnimationLayers=1, baseAnimationLayers=layers,
                specialAnimationLayers=[{'isEnabled': 0, 'type': t, 'animatorController': R(0), 'mask': R(0),
                                         'isDefault': 1} for t in (6, 7, 8)],
                AnimationPreset=R(0), animationHashSet=[], autoFootsteps=1, autoLocomotion=1)


def physbone(root_tf, colliders):
    return dict(m_Enabled=1, m_EditorHideFlags=0, m_Script=R(u.PHYSBONE_FID, u.DYN_GUID, 3), m_Name='',
                m_EditorClassIdentifier='', foldout_transforms=1, foldout_forces=1, foldout_collision=1,
                foldout_stretchsquish=1, foldout_limits=1, foldout_grabpose=1, foldout_options=1,
                foldout_gizmos=0, version=0, integrationType=0, rootTransform=R(root_tf), ignoreTransforms=[],
                ignoreOtherPhysBones=1, endpointPosition=V(0, 0, 0), multiChildType=0, pull=0.3, spring=0.5,
                stiffness=0.2, gravity=0.2, gravityFalloff=0, immobileType=0, immobile=0, allowCollision=1,
                collisionFilter={'allowSelf': 1, 'allowOthers': 1}, radius=0.03 / S_HAIR,
                colliders=[R(c) for c in colliders], limitType=1, maxAngleX=45, maxAngleZ=45,
                limitRotation=V(0, 0, 0), allowGrabbing=1, allowPosing=1, grabMovement=0.5, maxStretch=0,
                maxSquish=0, stretchMotion=0, snapToHand=0, parameter='', isAnimated=0, resetWhenDisabled=0)


def vrc_collider(shape, radius, height, pos):
    return dict(m_Enabled=1, m_EditorHideFlags=0, m_Script=R(u.COLLIDER_FID, u.DYN_GUID, 3), m_Name='',
                m_EditorClassIdentifier='', rootTransform=R(0), shapeType=shape, insideBounds=0, radius=radius,
                height=height, position=pos, rotation=Q(0, 0, 0, 1), bonesAsSpheres=0)


def dynbone(root_tf, colliders):
    return dict(m_Enabled=1, m_EditorHideFlags=0, m_Script=R(11500000, SCRIPT['DynamicBone'], 3), m_Name='',
                m_EditorClassIdentifier='', m_Root=R(root_tf), m_Roots=[], m_UpdateRate=60, m_UpdateMode=0,
                m_Damping=0.2, m_Elasticity=0.05, m_Stiffness=0.3, m_Inert=0, m_Friction=0,
                m_Radius=0.02 / S_THIGH, m_EndLength=0, m_EndOffset=V(0, 0, 0), m_Gravity=V(0, -0.01, 0),
                m_Force=V(0, 0, 0), m_BlendWeight=1, m_Colliders=[R(c) for c in colliders], m_Exclusions=[],
                m_FreezeAxis=0, m_DistantDisable=0, m_ReferenceObject=R(0), m_DistanceToObject=20)


def db_collider():
    return dict(m_Enabled=1, m_EditorHideFlags=0, m_Script=R(11500000, SCRIPT['DynamicBoneCollider'], 3),
                m_Name='', m_EditorClassIdentifier='', m_Direction=1, m_Center=V(0, 0.2 / S_THIGH, 0), m_Bound=0,
                m_Radius=0.06 / S_THIGH, m_Height=0.3 / S_THIGH, m_Radius2=0)


RENDERER = dict(m_Enabled=1, m_CastShadows=1, m_ReceiveShadows=1, m_DynamicOccludee=1, m_StaticShadowCaster=0,
                m_MotionVectors=1, m_LightProbeUsage=1, m_ReflectionProbeUsage=1, m_RayTracingMode=2,
                m_RayTraceProcedural=0, m_RenderingLayerMask=1, m_RendererPriority=0)


def mesh_mats(mid, fbx_own=()):
    m = info.mesh[mid]
    out = []
    for i in m['used']:
        n = m['materials'][i]
        out.append(R(-876546973899608171, FBX_GUID, 3) if n in fbx_own else R(2100000, MATG[n], 2))
    return out


def weights(mid):
    w = [0] * len(info.mesh[mid]['shapes'])
    if mid == MESH_OF['Body']:
        w[info.mesh[mid]['shapes'].index('Wink_L')] = 30
    return w


# ---------------------------------------------------------------- style B: every object written out

UP = {}  # ids in SynthAvatar.prefab that SynthMA.prefab points at


def unpacked():
    docs = []
    all_bones = [BY[n] for n in BONES]
    hips = BY['Hips']
    mesh_fid = {MESH_OF['Body']: 4300000}
    body_smr = None
    for o in list(walk(ROOT)):
        mid = o.key if isinstance(o.key, int) else None
        if mid and mid in info.mesh:
            m = info.mesh[mid]
            mesh = R(mesh_fid.get(mid, newid()), FBX_GUID, 3)
            if m['skinned'] or m['shapes']:
                bd = base(**RENDERER)
                bd.update(m_Materials=mesh_mats(mid, ('Glasses',)), serializedVersion=2, m_Quality=0,
                          m_UpdateWhenOffscreen=0, m_SkinnedMotionVectors=1, m_Mesh=mesh,
                          m_Bones=[R(b.tf) for b in all_bones], m_BlendShapeWeights=weights(mid),
                          m_RootBone=R(hips.tf), m_AABB={'m_Center': V(0, 0, 0), 'm_Extent': V(1, 1, 1)},
                          m_DirtyAABB=0)
                fid = o.add(137, 'SkinnedMeshRenderer', bd)
                if mid == MESH_OF['Body']:
                    body_smr = fid
            else:
                o.add(33, 'MeshFilter', base(m_Mesh=mesh))
                bd = base(**RENDERER)
                bd.update(m_Materials=mesh_mats(mid))
                o.add(23, 'MeshRenderer', bd)
    ident = (V(0, 0, 0), Q(0, 0, 0, 1), V(1, 1, 1))
    ROOT.add(95, 'Animator', base(m_Enabled=1, m_Avatar=R(9000000, FBX_GUID, 3), m_Controller=R(0),
                                  m_CullingMode=0, m_UpdateMode=0, m_ApplyRootMotion=0, m_LinearVelocityBlending=0,
                                  m_StabilizeFeet=0, m_WarningMessage='', m_HasTransformHierarchy=1,
                                  m_AllowConstantClipSamplingOptimization=1, m_KeepAnimatorStateOnDisable=0,
                                  m_WriteDefaultValuesOnDisable=0))
    ROOT.add(114, 'MonoBehaviour', base(**descriptor(body_smr, BY['Eye_L'].tf, BY['Eye_R'].tf)))
    hc = UO('HeadCollider', 'HeadCollider', BY['Head'], ident)
    head_col = hc.add(114, 'MonoBehaviour', base(**vrc_collider(0, 0.1 / S_HEAD, 0, V(0, 0.08 / S_HEAD, 0))))
    hand_col = BY['Wrist_L'].add(114, 'MonoBehaviour', base(**vrc_collider(
        1, 0.04 / S_HAND, 0.2 / S_HAND, V(0, 0.05 / S_HAND, 0))))
    hp = UO('HairPhysBone', 'HairPhysBone', ROOT, ident)
    UP['hair_pb'] = hp.add(114, 'MonoBehaviour', base(**physbone(BY['Hair_1'].tf, [head_col, hand_col])))
    UP['head_col'], UP['hand_col'] = head_col, hand_col
    leg_col = BY['Thigh_L'].add(114, 'MonoBehaviour', base(**db_collider()))
    BY['Skirt'].add(114, 'MonoBehaviour', base(**dynbone(BY['Skirt'].tf, [leg_col])))
    # something only the editor has: skipped
    ed = UO('EditorOnly', 'Notes', ROOT, ident)
    ed.tag = 'EditorOnly'
    ed.add(114, 'MonoBehaviour', base(**physbone(BY['Hair_1'].tf, [])))
    for o in walk(ROOT):
        comps = [(4, o.tf, 'Transform', None)] + o.comps
        docs.append(doc(1, o.go, 'GameObject', base(
            serializedVersion=6, m_Component=[{'component': R(c[1])} for c in comps], m_Layer=0, m_Name=o.name,
            m_TagString=getattr(o, 'tag', 'Untagged'), m_Icon=R(0), m_NavMeshLayer=0, m_StaticEditorFlags=0,
            m_IsActive=1 if o.active else 0)))
        docs.append(doc(4, o.tf, 'Transform', base(
            m_GameObject=R(o.go), serializedVersion=2, m_LocalRotation=o.rot, m_LocalPosition=o.pos,
            m_LocalScale=o.scl, m_ConstrainProportionsScale=0, m_Children=[R(c.tf) for c in o.children],
            m_Father=R(o.parent.tf if o.parent else 0), m_LocalEulerAnglesHint=V(0, 0, 0))))
        for cls, fid, kind, bd in o.comps:
            docs.append(doc(cls, fid, kind, bd))
    return HEAD + ''.join(docs)


def walk(o):
    yield o
    for c in o.children:
        yield from walk(c)


# ---------------------------------------------------------------- style A: a variant of the model

def variant():
    """a prefab variant as Unity keeps it: a PrefabInstance, its modifications, stubs of the model's objects the added
    ones point at, and the added objects"""
    imp = fbx_imp
    ids = {}
    for mid, cls, fid in u.fbx_ids(FBX_GUID, info, imp):
        ids.setdefault((mid, cls), fid)
    root = root_mid
    PI = newid() & u.MASK63
    docs, strip = [], {}
    kinds = {1: 'GameObject', 4: 'Transform', 137: 'SkinnedMeshRenderer', 33: 'MeshFilter', 23: 'MeshRenderer'}

    def src(mid, cls):
        return R(ids[(mid, cls)], FBX_GUID, 3)

    def stub(mid, cls):
        if (mid, cls) not in strip:
            fid = (ids[(mid, cls)] ^ PI) & u.MASK63
            strip[(mid, cls)] = fid
            docs.append(doc(cls, fid, kinds[cls], {'m_CorrespondingSourceObject': src(mid, cls),
                                                   'm_PrefabInstance': R(PI), 'm_PrefabAsset': R(0)}, stripped=True))
        return strip[(mid, cls)]

    mods, added_go, added_comp = [], [], []

    def mod(mid, cls, path, value=None, obj=None):
        mods.append({'target': src(mid, cls), 'propertyPath': path, 'value': '' if value is None else value,
                     'objectReference': obj if obj is not None else R(0)})

    def component(mid, cls, kind, body):
        fid = newid()
        body = dict(body)
        body['m_GameObject'] = R(stub(mid, 1))
        docs.append(doc(cls, fid, kind, base(**body)))
        added_comp.append({'targetCorrespondingSourceObject': src(mid, 1), 'insertIndex': -1,
                           'addedObject': R(fid)})
        return fid

    def gameobject(name, parent_mid, comps, tag='Untagged'):
        go, tf = newid(), newid()
        cids = [newid() for _ in comps]
        docs.append(doc(1, go, 'GameObject', base(
            serializedVersion=6, m_Component=[{'component': R(c)} for c in [tf] + cids], m_Layer=0, m_Name=name,
            m_TagString=tag, m_Icon=R(0), m_NavMeshLayer=0, m_StaticEditorFlags=0, m_IsActive=1)))
        docs.append(doc(4, tf, 'Transform', base(
            m_GameObject=R(go), serializedVersion=2, m_LocalRotation=Q(0, 0, 0, 1), m_LocalPosition=V(0, 0, 0),
            m_LocalScale=V(1, 1, 1), m_ConstrainProportionsScale=0, m_Children=[], m_Father=R(stub(parent_mid, 4)),
            m_LocalEulerAnglesHint=V(0, 0, 0))))
        for c, (cls, kind, body) in zip(cids, comps):
            body = dict(body)
            body['m_GameObject'] = R(go)
            docs.append(doc(cls, c, kind, base(**body)))
        added_go.append({'targetCorrespondingSourceObject': src(parent_mid, 4), 'insertIndex': -1,
                         'addedObject': R(tf)})
        return cids

    # what every instance has: the root's place, and its name
    for k, v in (('m_LocalPosition.x', 0), ('m_LocalPosition.y', 0), ('m_LocalPosition.z', 0),
                 ('m_LocalRotation.w', 1), ('m_LocalRotation.x', 0), ('m_LocalRotation.y', 0),
                 ('m_LocalRotation.z', 0), ('m_LocalEulerAnglesHint.x', 0), ('m_LocalEulerAnglesHint.y', 0),
                 ('m_LocalEulerAnglesHint.z', 0)):
        mod(root, 4, k, v)
    mod(root, 1, 'm_Name', 'SynthVariant')
    sk, hat, gl, body_m = MESH_OF['Skirt'], MESH_OF['Hat'], MESH_OF['Glasses'], MESH_OF['Body']
    mod(sk, 1, 'm_Name', 'Skirt Outfit')
    mod(sk, 1, 'm_IsActive', 0)
    mod(gl, 1, 'm_IsActive', 0)
    for a in 'xyz':
        mod(hat, 4, 'm_LocalScale.' + a, UOS[hat].scl[a])
    mod(body_m, 137, 'm_BlendShapeWeights.Array.data[%d]' % body_shapes.index('Wink_L'), 30)
    mod(gl, 137, 'm_Materials.Array.data[0]', obj=R(2100000, MATG['GlassesRed'], 2))
    # added to the model's objects
    desc = component(root, 114, 'MonoBehaviour', descriptor(stub(body_m, 137), stub(MID['Eye_L'], 4),
                                                             stub(MID['Eye_R'], 4)))
    hand_col = component(MID['Wrist_L'], 114, 'MonoBehaviour', vrc_collider(
        1, 0.04 / S_HAND, 0.2 / S_HAND, V(0, 0.05 / S_HAND, 0)))
    leg_col = component(MID['Thigh_L'], 114, 'MonoBehaviour', db_collider())
    component(SKIRT_BONE, 114, 'MonoBehaviour', dynbone(stub(SKIRT_BONE, 4), [leg_col]))
    head_col, = gameobject('HeadCollider', MID['Head'], [(114, 'MonoBehaviour', vrc_collider(
        0, 0.1 / S_HEAD, 0, V(0, 0.08 / S_HEAD, 0)))])
    gameobject('HairPhysBone', root, [(114, 'MonoBehaviour', physbone(stub(MID['Hair_1'], 4), [head_col, hand_col]))])
    gameobject('Notes', root, [(114, 'MonoBehaviour', physbone(stub(MID['Hair_1'], 4), []))], tag='EditorOnly')
    pi = {'m_ObjectHideFlags': 0, 'serializedVersion': 2,
          'm_Modification': {'serializedVersion': 3, 'm_TransformParent': R(0), 'm_Modifications': mods,
                             'm_RemovedComponents': [],
                             'm_RemovedGameObjects': [src(MID['Badge'], 1)],
                             'm_AddedGameObjects': added_go, 'm_AddedComponents': added_comp},
          'm_SourcePrefab': R(100100000, FBX_GUID, 3)}
    assert SKIRT_BONE != sk and info.kind(SKIRT_BONE) == 'LimbNode'
    return HEAD + doc(1001, PI, 'PrefabInstance', pi) + ''.join(docs)


material('GlassesRed', STANDARD, floats=[('_Mode', 3)], colors=[('_Color', (1, 0.1, 0.1, 0.5))],
         old_kw='_ALPHAPREMULTIPLY_ON', rq=3000)
text_a = variant()  # before unpacked(), which adds objects to the tree
write('SynthVariant.prefab', text_a, G('SynthVariant.prefab'), 'PrefabImporter')
write('SynthAvatar.prefab', unpacked(), G('SynthAvatar.prefab'), 'PrefabImporter')
json.dump({'collapsed': collapsed, 'bone_scale': lossy(MID['Head']),
           'paths': {o.name: o.path for o in walk(ROOT)}}, open(os.path.join(PROJ, 'synth.json'), 'w'), indent=1)
print('synth: wrote %s: SynthAvatar.prefab (unpacked), SynthVariant.prefab (a variant of the model)' % PROJ)

# ---------------------------------------------------------------- Modular Avatar: an outfit that merges into the avatar
#
# Outfit.fbx: its own Armature with the avatar's bone names at its places, plus bones of its own (Frill, HairClip).
# OutfitMA.prefab: Merge Armature, PhysBones, a collider and a script MA doesn't know (their bones stay, though the
# Dress follows the avatar's Neck), Bone Proxies, Move To and a menu toggle; MA drops its copy of the avatar's hair
# PhysBone. SynthMA.prefab: SynthAvatar.prefab with the hair PhysBone on Hair_1 and OutfitMA inside. Outfit.prefab: no
# MA setup, for --outfit.

import copy as _copy
MA_GUID = unitygen.MA_GUID


def ma(kind, **kw):
    return unitygen.ma(kind, **kw)


for coll in (bpy.data.objects, bpy.data.meshes, bpy.data.armatures):
    for x in list(coll):
        coll.remove(x)
OMATS = ('Dress', 'Bow', 'Headband', 'Charm', 'Tag', 'HairTie')
for n in OMATS:
    MAT[n] = bpy.data.materials.new(n)
ad = bpy.data.armatures.new('Armature')
arm = bpy.data.objects.new('Armature', ad)
bpy.context.scene.collection.objects.link(arm)
bpy.context.view_layer.objects.active = arm
arm.select_set(True)
bpy.ops.object.mode_set(mode='EDIT')
BONES = []
bone('Hips', (0, 0, 0.95), (0, 0, 1.05))
bone('Spine', (0, 0, 1.05), (0, 0, 1.2), 'Hips', True)
bone('Chest', (0, 0, 1.2), (0, 0, 1.38), 'Spine', True)
bone('Neck', (0, 0, 1.42), (0, 0, 1.5), 'Chest')
bone('Head', (0, 0, 1.5), (0, 0, 1.72), 'Neck', True)
bone('Hair_1', (0, 0.08, 1.68), (0, 0.11, 1.52), 'Head')
bone('Hair_1.001', (0, 0.11, 1.52), (0, 0.12, 1.38), 'Hair_1', True)
bone('Hair_1.002', (0, 0.12, 1.38), (0, 0.12, 1.26), 'Hair_1.001', True)
bone('HairClip', (0.04, 0.1, 1.62), (0.07, 0.1, 1.62), 'Hair_1')
for s_, x in (('L', 1), ('R', -1)):
    bone('Thigh_' + s_, (0.09 * x, 0, 0.93), (0.09 * x, 0, 0.52), 'Hips')
    bone('Knee_' + s_, (0.09 * x, 0, 0.52), (0.09 * x, 0.01, 0.1), 'Thigh_' + s_, True)
    bone('Shoulder_' + s_, (0.03 * x, 0, 1.36), (0.13 * x, 0, 1.38), 'Chest')
    bone('UpperArm_' + s_, (0.13 * x, 0, 1.38), (0.4 * x, 0, 1.38), 'Shoulder_' + s_, True)
bone('Frill', (0, -0.15, 0.9), (0, -0.19, 0.74), 'Hips')
bone('Frill.001', (0, -0.19, 0.74), (0, -0.22, 0.58), 'Frill', True)
bpy.ops.object.mode_set(mode='OBJECT')

P = Parts()
M, d = along((0, 0, 0.62), (0, 0, 1.44), 1.0, 0.75)
P.add('dress', 'cyl', M, r1=0.2, r2=0.155, depth=d, seg=20, caps=False)
dress, _ = P.make('Dress', ['Dress'])
skin(dress, ['Hips', 'Spine', 'Chest', 'Neck', 'Thigh_L', 'Thigh_R', 'Frill', 'Frill.001', 'Shoulder_L', 'Shoulder_R'])
shape(dress, 'Flare', lambda c: Vector((c.x * 1.3, c.y * 1.3, c.z)) if c.z < 0.8 else None)
P = Parts()
P.add('bow', 'cube', Matrix.Diagonal((0.09, 0.02, 0.05, 1)))
bow, _ = P.make('Bow', ['Bow'])
attach(bow, 'Chest', (0, -0.13, 1.3))
P = Parts()
P.add('tie', 'cube', Matrix.Diagonal((0.03, 0.03, 0.03, 1)))
tie, _ = P.make('HairTie', ['HairTie'])
attach(tie, 'HairClip', (0.055, 0.1, 1.62))


def loose(name, at, parts):
    """an object at the top of the file with its origin at `at`, its mesh wherever the parts are"""
    P = Parts()
    for kind, M, kw in parts:
        P.add(name.lower(), kind, M, **kw)
    ob, _ = P.make(name, [name])
    ob.data.transform(Matrix.Translation(-Vector(at)))
    ob.location = at
    return ob


loose('Headband', (0, 0, 1.66), [('cyl', Matrix.Translation((0, 0, 1.66)), dict(r1=0.118, depth=0.025, seg=24, caps=False))])
loose('Charm', (0.35, -0.25, 1.05), [('sphere', Matrix.Translation((0.35, -0.41, 1.12)), dict(r=0.025))])
loose('Tag', (0.3, 0.1, 1.0), [('cube', Matrix.Translation((0.3, 0.02, 1.0)) @ Matrix.Diagonal((0.04, 0.01, 0.03, 1)), {})])

OFBX = os.path.join(PROJ, 'Assets/Synth/Outfit.fbx')
OUT_GUID = G('Outfit.fbx')
bpy.ops.object.select_all(action='DESELECT')
bpy.ops.export_scene.fbx(filepath=OFBX, use_selection=False, object_types={'ARMATURE', 'MESH'},
                         apply_unit_scale=True, apply_scale_options='FBX_SCALE_NONE', global_scale=1.0,
                         axis_forward='-Z', axis_up='Y', add_leaf_bones=False, bake_anim=False,
                         primary_bone_axis='Y', secondary_bone_axis='X', use_mesh_modifiers=False,
                         mesh_smooth_type='FACE', use_armature_deform_only=False, path_mode='AUTO',
                         embed_textures=False)
oinfo = u.FBXInfo(OFBX, 'Assets/Synth/Outfit.fbx')
OMID = {}
for m in oinfo.order:
    OMID.setdefault(oinfo.name(m), m)
assert len(oinfo.kids.get(0, [])) > 1  # not collapsed: the root is the file's own
for n, (sh, co) in (('Dress', ('Skin', 'Dress')),):
    pass
material('Dress', STANDARD, floats=[('_Mode', 0)], colors=[('_Color', (0.25, 0.3, 0.8, 1))])
material('Bow', STANDARD, floats=[('_Mode', 0)], colors=[('_Color', (0.9, 0.1, 0.2, 1))])
material('Headband', STANDARD, floats=[('_Mode', 0), ('_Cull', 0)], colors=[('_Color', (0.95, 0.85, 0.1, 1))])
material('Charm', STANDARD, floats=[('_Mode', 0)], colors=[('_Color', (0.1, 0.9, 0.9, 1))])
material('Tag', STANDARD, floats=[('_Mode', 0)], colors=[('_Color', (0.95, 0.5, 0.1, 1))])
material('HairTie', STANDARD, floats=[('_Mode', 0)], colors=[('_Color', (0.9, 0.2, 0.9, 1))])
oimp = _copy.deepcopy(fbx_imp)
oimp['internalIDToNameTable'] = []
oimp['externalObjects'] = [{'first': {'type': 'UnityEngine:Material', 'assembly': 'UnityEngine.CoreModule', 'name': n},
                            'second': R(2100000, MATG[n], 2)} for n in OMATS]
oimp['animationType'] = 2
oimp['humanDescription']['human'] = []
with open(OFBX + '.meta', 'w') as f:
    f.write('\n'.join(emit({'fileFormatVersion': 2, 'guid': OUT_GUID, 'ModelImporter': oimp})) + '\n')
ids_o = {}
for mid, cls, fid in u.fbx_ids(OUT_GUID, oinfo, oimp):
    ids_o.setdefault((mid, cls), fid)
SCRIPT['OutfitNote'] = G('script/OutfitNote')
write('Scripts/OutfitNote.cs', 'public class OutfitNote : UnityEngine.MonoBehaviour { public string note; }\n',
      SCRIPT['OutfitNote'], 'MonoImporter', {'externalObjects': {}, 'serializedVersion': 2, 'defaultReferences': [],
                                             'executionOrder': 0, 'icon': R(0), 'userData': '',
                                             'assetBundleName': '', 'assetBundleVariant': ''})


def Variant(src_guid, parent_tf=0):
    return unitygen.Variant(newid, src_guid, parent_tf)


def oid(name, cls=1):
    return ids_o[(OMID[name], cls)]


O_GO, O_TF = ids_o[(0, 1)], ids_o[(0, 4)]
S_O = lossy_o = None


def outfit_prefab(with_ma):
    v = Variant(OUT_GUID)
    v.root(O_GO, O_TF, 'Outfit')
    thigh_col = v.component(oid('Thigh_L'), 114, 'MonoBehaviour', vrc_collider(
        1, 0.06 / S_THIGH, 0.34 / S_THIGH, V(0, 0.17 / S_THIGH, 0)))
    v.component(oid('Frill'), 114, 'MonoBehaviour', physbone(0, [thigh_col]))
    if not with_ma:
        return v
    v.component(oid('Armature'), 114, 'MonoBehaviour', ma(
        'MergeArmature', mergeTarget={'referencePath': 'Armature', 'targetObject': R(0)}, prefix='', suffix='',
        legacyLocked=0, LockMode=1, mangleNames=1))
    v.component(oid('Hair_1'), 114, 'MonoBehaviour', physbone(0, []))
    v.component(oid('Neck'), 114, 'MonoBehaviour', dict(
        m_Enabled=1, m_EditorHideFlags=0, m_Script=R(11500000, SCRIPT['OutfitNote'], 3), m_Name='',
        m_EditorClassIdentifier='', note='made for SynthAvatar'))
    v.component(oid('Headband'), 114, 'MonoBehaviour', ma('BoneProxy', boneReference=10, subPath='', attachmentMode=2,
                                                          matchScale=0))
    v.component(oid('Charm'), 114, 'MonoBehaviour', ma('BoneProxy', boneReference=55,
                                                       subPath='Armature/Hips/Spine/Chest', attachmentMode=1,
                                                       matchScale=0))
    v.component(oid('Tag'), 114, 'MonoBehaviour', ma('MoveTo', target={
        'referencePath': 'Armature/Hips/Spine/Chest/Neck', 'targetObject': R(0)}, matchPosition=1, matchRotation=1,
        matchScale=0))
    def item(name, param='', value=1, default=0, auto=1, kind=102, source=1):
        return (114, 'MonoBehaviour', ma('MenuItem', Control={
            'name': '', 'icon': R(0), 'type': kind, 'parameter': {'name': param}, 'value': value, 'style': 0,
            'subMenu': R(0), 'subParameters': [], 'labels': []}, MenuSource=source,
            menuSource_otherObjectChildren=R(0), isSynced=1, isSaved=1, isDefault=default, automaticValue=auto,
            label=''))

    def toggle(obj, active):
        return (114, 'MonoBehaviour', ma('ObjectToggle', m_inverted=0, m_objects=[
            {'Object': {'referencePath': 'Outfit/' + obj, 'targetObject': R(v.stub(oid(obj), 1))}, 'Active': active}]))
    for n in ('Headband', 'Charm', 'Tag'):  # shown by their toggles
        v.mod(oid(n), 'm_IsActive', 0)
    _, menu_tf, _ = v.gameobject('Outfit Menu', O_TF, [item('', kind=103, auto=0),
                                                        (114, 'MonoBehaviour', ma('MenuInstaller', menuToAppend=R(0),
                                                                                  installTargetMenu=R(0)))])
    v.gameobject('Hide Dress', O_TF, [item('Hide Dress'), toggle('Dress', 0)], own_parent=menu_tf)
    v.gameobject('Headband On', O_TF, [item('Headband On', 'Accessory', 1, default=1, auto=0), toggle('Headband', 1)],
                 own_parent=menu_tf)
    v.gameobject('Charm On', O_TF, [item('Charm On', 'Accessory', 2, auto=0), toggle('Charm', 1)], own_parent=menu_tf)
    v.gameobject('Flare Dress', O_TF, [item('Flare Dress'), (114, 'MonoBehaviour', ma('ShapeChanger', m_inverted=0, m_shapes=[
        {'Object': {'referencePath': 'Outfit/Dress', 'targetObject': R(v.stub(oid('Dress'), 1))}, 'ShapeName': 'Flare',
         'ChangeType': 1, 'Value': 100}], m_threshold=0.01))], own_parent=menu_tf)
    v.gameobject('Not In Menu', O_TF, [item('Not In Menu'), toggle('Bow', 0)])  # no installer: stays at its default
    v.component(O_GO, 114, 'MonoBehaviour', ma(
        'MergeAnimator', animator=R(9100000, G('OutfitFX.controller'), 2), layerType=5, deleteAttachedAnimator=1,
        pathMode=0, matchAvatarWriteDefaults=1, relativePathRoot={'referencePath': '', 'targetObject': R(0)},
        layerPriority=0, mergeAnimatorMode=0))
    v.component(O_GO, 114, 'MonoBehaviour', ma('MenuInstaller', menuToAppend=R(11400000, G('OutfitExtra.asset'), 2),
                                               installTargetMenu=R(0)))
    v.component(O_GO, 114, 'MonoBehaviour', ma('Parameters', parameters=[
        {'nameOrPrefix': 'TagOn', 'remapTo': '', 'internalParameter': 0, 'isPrefix': 0, 'syncType': 3, 'localOnly': 0,
         'defaultValue': 1, 'saved': 1, 'hasExplicitDefaultValue': 1, 'm_overrideAnimatorDefaults': 0}]))
    return v


CTL, LAYERS = [], []
clip('TagOn', [('Tag', 1, 'm_IsActive', 1)])
clip('TagOff', [('Tag', 1, 'm_IsActive', 0)])
toggle_layer('Tag', 'TagOn', 'TagOn', 'TagOff')
write('OutfitFX.controller', HEAD + doc(91, 9100000, 'AnimatorController', base(
    m_Name='OutfitFX', serializedVersion=5,
    m_AnimatorParameters=[{'m_Name': 'TagOn', 'm_Type': 4, 'm_DefaultFloat': 0, 'm_DefaultInt': 0,
                           'm_DefaultBool': 0, 'm_Controller': R(9100000)}],
    m_AnimatorLayers=LAYERS)) + ''.join(doc(c, f, k, bd) for c, f, k, bd in CTL), G('OutfitFX.controller'),
      'NativeFormatImporter', native(9100000))
write('OutfitExtra.asset', asset114('OutfitExtra', u.MENU_FID, controls=[control('Show Tag', 102, 'TagOn', 1)]),
      G('OutfitExtra.asset'), 'NativeFormatImporter', native(11400000))
OMA = outfit_prefab(True)
write('OutfitMA.prefab', HEAD + OMA.text(), G('OutfitMA.prefab'), 'PrefabImporter')
write('Outfit.prefab', HEAD + outfit_prefab(False).text(), G('Outfit.prefab'), 'PrefabImporter')

SA_GUID = G('SynthAvatar.prefab')
v = Variant(SA_GUID)
v.root(ROOT.go, ROOT.tf, 'SynthMA')
v.removed_c.append(UP['hair_pb'])
v.component(BY['Hair_1'].go, 114, 'MonoBehaviour', physbone(0, [v.stub(UP['head_col'], 114),
                                                                v.stub(UP['hand_col'], 114)]))
n = Variant(G('OutfitMA.prefab'), parent_tf=v.stub(ROOT.tf, 4))
n.root(OMA.own(O_GO), OMA.own(O_TF), 'Outfit')
v.added_go.append({'targetCorrespondingSourceObject': v.src(ROOT.tf), 'insertIndex': -1,
                   'addedObject': R(n.stub(OMA.own(O_TF), 4))})
write('SynthMA.prefab', HEAD + v.text() + n.text(), G('SynthMA.prefab'), 'PrefabImporter')
print('synth: wrote Outfit.fbx, OutfitMA.prefab, Outfit.prefab and SynthMA.prefab (%d outfit models)' % len(oinfo.order))

# ---------------------------------------------------------------- an outfit named the VRM way, in an A pose, for --outfit
# OutfitVRM.fbx: J_Bip_C_Hips and so on, a Cape bone of its own, the arms 35 degrees down; OutfitVRM.prefab, a variant

for coll in (bpy.data.objects, bpy.data.meshes, bpy.data.armatures):
    for x in list(coll):
        coll.remove(x)
for n in ('Top', 'Cape'):
    MAT[n] = bpy.data.materials.new(n)
ad = bpy.data.armatures.new('Armature')
arm = bpy.data.objects.new('Armature', ad)
bpy.context.scene.collection.objects.link(arm)
bpy.context.view_layer.objects.active = arm
arm.select_set(True)
bpy.ops.object.mode_set(mode='EDIT')
BONES = []
bone('J_Bip_C_Hips', (0, 0, 0.95), (0, 0, 1.05))
bone('J_Bip_C_Spine', (0, 0, 1.05), (0, 0, 1.2), 'J_Bip_C_Hips', True)
bone('J_Bip_C_Chest', (0, 0, 1.2), (0, 0, 1.38), 'J_Bip_C_Spine', True)
bone('J_Bip_C_Neck', (0, 0, 1.42), (0, 0, 1.5), 'J_Bip_C_Chest')
bone('J_Bip_C_Head', (0, 0, 1.5), (0, 0, 1.72), 'J_Bip_C_Neck', True)
bone('Cape_1', (0, 0.12, 1.36), (0, 0.16, 1.05), 'J_Bip_C_Chest')
bone('Cape_1.001', (0, 0.16, 1.05), (0, 0.19, 0.8), 'Cape_1', True)
A = math.radians(35)
for s_, x in (('L', 1), ('R', -1)):
    bone('J_Bip_%s_UpperLeg' % s_, (0.09 * x, 0, 0.93), (0.09 * x, 0, 0.52), 'J_Bip_C_Hips')
    bone('J_Bip_%s_LowerLeg' % s_, (0.09 * x, 0, 0.52), (0.09 * x, 0.01, 0.1), 'J_Bip_%s_UpperLeg' % s_, True)
    bone('J_Bip_%s_Shoulder' % s_, (0.03 * x, 0, 1.36), (0.13 * x, 0, 1.38), 'J_Bip_C_Chest')
    d = Vector((math.cos(A) * x, 0, -math.sin(A)))
    p0 = Vector((0.13 * x, 0, 1.38))
    bone('J_Bip_%s_UpperArm' % s_, p0, p0 + d * 0.27, 'J_Bip_%s_Shoulder' % s_, True)
    bone('J_Bip_%s_LowerArm' % s_, p0 + d * 0.27, p0 + d * 0.51, 'J_Bip_%s_UpperArm' % s_, True)
    bone('J_Bip_%s_Hand' % s_, p0 + d * 0.51, p0 + d * 0.61, 'J_Bip_%s_LowerArm' % s_, True)
bpy.ops.object.mode_set(mode='OBJECT')
P = Parts()
M, d = along((0, 0, 1.05), (0, 0, 1.44), 1.0, 0.75)
P.add('top', 'cyl', M, r1=0.16, depth=d, seg=20, caps=False)
for x in (1, -1):
    dd = Vector((math.cos(A) * x, 0, -math.sin(A)))
    p0 = Vector((0.12 * x, 0, 1.38))
    M, d = along(p0, p0 + dd * 0.45)
    P.add('sleeve', 'cyl', M, r1=0.06, depth=d, seg=12)
top, _ = P.make('Top', ['Top'])
skin(top, ['J_Bip_C_Spine', 'J_Bip_C_Chest', 'J_Bip_L_Shoulder', 'J_Bip_R_Shoulder', 'J_Bip_L_UpperArm',
           'J_Bip_R_UpperArm', 'J_Bip_L_LowerArm', 'J_Bip_R_LowerArm'])
P = Parts()
P.add('cape', 'cube', Matrix.Translation((0, 0.2, 1.08)) @ Matrix.Diagonal((0.3, 0.01, 0.55, 1)))
cape, _ = P.make('Cape', ['Cape'])
skin(cape, ['J_Bip_C_Chest', 'Cape_1', 'Cape_1.001'])
VFBX = os.path.join(PROJ, 'Assets/Synth/OutfitVRM.fbx')
VRM_GUID = G('OutfitVRM.fbx')
bpy.ops.object.select_all(action='DESELECT')
bpy.ops.export_scene.fbx(filepath=VFBX, use_selection=False, object_types={'ARMATURE', 'MESH'},
                         apply_unit_scale=True, apply_scale_options='FBX_SCALE_NONE', global_scale=1.0,
                         axis_forward='-Z', axis_up='Y', add_leaf_bones=False, bake_anim=False,
                         primary_bone_axis='Y', secondary_bone_axis='X', use_mesh_modifiers=False,
                         mesh_smooth_type='FACE', use_armature_deform_only=False, path_mode='AUTO',
                         embed_textures=False)
vinfo = u.FBXInfo(VFBX, 'Assets/Synth/OutfitVRM.fbx')
material('Top', STANDARD, floats=[('_Mode', 0)], colors=[('_Color', (0.1, 0.6, 0.3, 1))])
material('Cape', STANDARD, floats=[('_Mode', 0), ('_Cull', 0)], colors=[('_Color', (0.5, 0.1, 0.6, 1))])
vimp = _copy.deepcopy(oimp)
vimp['externalObjects'] = [{'first': {'type': 'UnityEngine:Material', 'assembly': 'UnityEngine.CoreModule',
                                      'name': n}, 'second': R(2100000, MATG[n], 2)} for n in ('Top', 'Cape')]
with open(VFBX + '.meta', 'w') as f:
    f.write('\n'.join(emit({'fileFormatVersion': 2, 'guid': VRM_GUID, 'ModelImporter': vimp})) + '\n')
ids_v = {}
for mid, cls, fid in u.fbx_ids(VRM_GUID, vinfo, vimp):
    ids_v.setdefault((mid, cls), fid)
VMID = {vinfo.name(m): m for m in vinfo.order}
v = Variant(VRM_GUID)
v.root(ids_v[(0, 1)], ids_v[(0, 4)], 'OutfitVRM')
v.component(ids_v[(VMID['Cape_1'], 1)], 114, 'MonoBehaviour', physbone(0, []))
write('OutfitVRM.prefab', HEAD + v.text(), G('OutfitVRM.prefab'), 'PrefabImporter')
print('synth: wrote OutfitVRM.fbx and OutfitVRM.prefab')

# ---------------------------------------------------------------- VRCFury: an outfit linked the VRCFury way, and toggles
# OutfitVF.prefab (Outfit.fbx): Armature Link, a Dress Toggle setting Shrink, Bow and Headband toggles sharing an
# exclusive tag, a PhysBone. SynthVF.prefab: SynthAvatar.prefab with OutfitVF inside (Bow renamed by a
# [SerializeReference] override), an old Unity 2019 version-0 Toggle, Apply During Upload and Delete During Upload.

OVF = Variant(OUT_GUID)
OVF.root(O_GO, O_TF, 'OutfitVF')
refs = unitygen.Refs(newid)
OVF.component(O_GO, 114, 'MonoBehaviour', unitygen.vrcfury(refs, refs.add(
    'ArmatureLink', unitygen.vf_armature_link(OVF.stub(oid('Hips'), 1)))))
refs = unitygen.Refs(newid)
OVF.component(O_GO, 114, 'MonoBehaviour', unitygen.vrcfury(refs, refs.add('Toggle', unitygen.vf_toggle(
    'Outfit/Dress', [refs.action('ObjectToggleAction', obj=R(OVF.stub(oid('Dress'), 1)), mode=0),
                     refs.action('BlendShapeAction', blendShape='Shrink', blendShapeValue=100, renderer=R(0),
                                 allRenderers=1)], on=1))))
DECO = {}  # name -> (component, toggle reference id)
for name, off in (('Bow', 1), ('Headband', 0)):
    refs = unitygen.Refs(newid)
    t = refs.add('Toggle', unitygen.vf_toggle(
        'Outfit/Deco/' + name, [refs.action('ObjectToggleAction', obj=R(OVF.stub(oid(name), 1)), mode=0)],
        tag='deco', off_state=off))
    DECO[name] = (OVF.component(O_GO, 114, 'MonoBehaviour', unitygen.vrcfury(refs, t)), t['rid'])
OVF.component(oid('Frill'), 114, 'MonoBehaviour', physbone(0, []))
write('OutfitVF.prefab', HEAD + OVF.text(), G('OutfitVF.prefab'), 'PrefabImporter')

SV = Variant(SA_GUID)
SV.root(ROOT.go, ROOT.tf, 'SynthVF')
refs = unitygen.Refs(newid, version=1)
old = dict(unitygen.vf_toggle('Extras/Badge', [refs.action('ObjectToggleAction', version=0, obj=R(SV.stub(
    UOS[MESH_OF['Badge']].go, 1)), mode=0)]), version=0)
SV.component(ROOT.go, 114, 'MonoBehaviour', unitygen.vrcfury(refs, None, [refs.add('Toggle', old)]))
refs = unitygen.Refs(newid)
SV.component(ROOT.go, 114, 'MonoBehaviour', unitygen.vrcfury(refs, refs.add('ApplyDuringUpload', {
    'version': 0, 'action': unitygen.vf_state([refs.action('BlendShapeAction', blendShape='Smile', blendShapeValue=30,
                                                           renderer=R(0), allRenderers=1)])})))
refs = unitygen.Refs(newid)
SV.component(UOS[MESH_OF['Glasses']].go, 114, 'MonoBehaviour', unitygen.vrcfury(refs, refs.add(
    'DeleteDuringUpload', {'version': 0})))
n = Variant(G('OutfitVF.prefab'), parent_tf=SV.stub(ROOT.tf, 4))
n.root(OVF.own(O_GO), OVF.own(O_TF), 'OutfitVF')
comp, rid = DECO['Bow']  # rename the Bow toggle via a [SerializeReference] override
n.mod(comp, 'managedReferences[%d].name' % rid, 'Outfit/Deco/Ribbon Bow')
SV.added_go.append({'targetCorrespondingSourceObject': SV.src(ROOT.tf), 'insertIndex': -1,
                    'addedObject': R(n.stub(OVF.own(O_TF), 4))})
write('SynthVF.prefab', HEAD + SV.text() + n.text(), G('SynthVF.prefab'), 'PrefabImporter')
print('synth: wrote OutfitVF.prefab and SynthVF.prefab (VRCFury)')
