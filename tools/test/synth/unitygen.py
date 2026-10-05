# unitygen.py: Unity assets from Blender for the converter's test generators (make.py, booth.py): YAML, .meta files,
# textures, FBX models read back as the converter reads them, animator controllers, VRChat, Modular Avatar and VRCFury
# components, prefab variants, .unitypackage and Booth .zip packing. Runs inside Blender; the caller puts tools/ on
# sys.path first.
import bpy, bmesh, os, io, math, hashlib, random, struct, tarfile, gzip, zipfile
import numpy as np
from mathutils import Matrix, Vector

import unity2hyprwalk as u


# ---------------------------------------------------------------- Unity YAML out

class F(dict):
    """a flow map: {fileID: 0}"""


def R(fid, g=None, t=None):
    return F(fileID=fid) if g is None else F(fileID=fid, guid=g, type=t)


def V(x, y, z):
    return F(x=x, y=y, z=z)


def Q(x, y, z, w):
    return F(x=x, y=y, z=z, w=w)


def C(r, g, b, a=1.0):
    return F(r=r, g=g, b=b, a=a)


class Esc(str):
    """a string Unity writes double-quoted, with \\u escapes (UTF-16 pairs beyond the BMP)"""


def fmt(v):
    if isinstance(v, bool):
        return '1' if v else '0'
    if isinstance(v, int):
        return str(v)
    if isinstance(v, float):
        if v == int(v) and abs(v) < 1e15:
            return str(int(v))
        return '%.9g' % v
    if isinstance(v, Esc):
        out = []
        for ch in v:
            c = ord(ch)
            if c < 0x80 and ch not in '"\\':
                out.append(ch)
            elif c > 0xffff:
                c -= 0x10000
                out.append('\\u%04X\\u%04X' % (0xd800 + (c >> 10), 0xdc00 + (c & 0x3ff)))
            else:
                out.append('\\u%04X' % c)
        return '"%s"' % ''.join(out)
    return str(v)


def flow(d):
    return '{' + ', '.join('%s: %s' % (k, flow(v) if isinstance(v, dict) else fmt(v)) for k, v in d.items()) + '}'


def emit(d, ind=0):
    out, pad = [], ' ' * ind
    for k, v in d.items():
        if isinstance(v, F):
            out.append('%s%s: %s' % (pad, k, flow(v)))
        elif isinstance(v, dict):
            if not v:
                out.append('%s%s: {}' % (pad, k))
            else:
                out.append('%s%s:' % (pad, k))
                out += emit(v, ind + 2)
        elif isinstance(v, list):
            if not v:
                out.append('%s%s: []' % (pad, k))
                continue
            out.append('%s%s:' % (pad, k))
            for it in v:
                if isinstance(it, F):
                    out.append('%s- %s' % (pad, flow(it)))
                elif isinstance(it, dict):
                    sub = emit(it, ind + 2)
                    sub[0] = pad + '- ' + sub[0][ind + 2:]
                    out += sub
                else:
                    out.append('%s- %s' % (pad, fmt(it)))
        else:
            out.append('%s%s: %s' % (pad, k, fmt(v)))
    return out


HEAD = '%YAML 1.1\n%TAG !u! tag:unity3d.com,2011:\n'


def doc(cls, fid, kind, body, stripped=False):
    return '--- !u!%d &%d%s\n%s:\n%s\n' % (cls, fid, ' stripped' if stripped else '', kind, '\n'.join(emit(body, 2)))


def base(**kw):
    d = {'m_ObjectHideFlags': 0, 'm_CorrespondingSourceObject': R(0), 'm_PrefabInstance': R(0), 'm_PrefabAsset': R(0)}
    d.update(kw)
    return d


# ---------------------------------------------------------------- ids, files and .meta files

def guid_of(key):
    return hashlib.md5(key.encode()).hexdigest()


class Ids:
    """fileIDs like Unity's (big, of either sign), from a seeded generator"""

    def __init__(self, seed):
        self.rnd = random.Random(seed)

    def __call__(self):
        return self.rnd.getrandbits(63) - (1 << 62)


def native(main):
    return {'externalObjects': {}, 'mainObjectFileID': main, 'userData': '', 'assetBundleName': '',
            'assetBundleVariant': ''}


def plain_importer():
    return {'externalObjects': {}, 'userData': '', 'assetBundleName': '', 'assetBundleVariant': ''}


def mono_importer():
    return {'externalObjects': {}, 'serializedVersion': 2, 'defaultReferences': [], 'executionOrder': 0,
            'icon': R(0), 'userData': '', 'assetBundleName': '', 'assetBundleVariant': ''}


def write_meta(path, guid, importer, imp=None, folder=False):
    meta = {'fileFormatVersion': 2, 'guid': guid}
    if folder:
        meta['folderAsset'] = 'yes'
    meta[importer] = plain_importer() if imp is None else imp
    with open(path + '.meta', 'w', encoding='utf-8') as f:
        f.write('\n'.join(emit(meta)) + '\n')


def write_asset(path, data, guid, importer, imp=None):
    """an asset and its .meta; data is text (written as UTF-8) or bytes"""
    os.makedirs(os.path.dirname(path), exist_ok=True)
    if isinstance(data, bytes):
        with open(path, 'wb') as f:
            f.write(data)
    else:
        with open(path, 'w', encoding='utf-8') as f:
            f.write(data)
    write_meta(path, guid, importer, imp)
    return path


def folder_metas(root, key):
    """a .meta for every folder under root/Assets that has none, as Unity makes them"""
    for dp, dns, fns in os.walk(os.path.join(root, 'Assets')):
        for d in dns:
            p = os.path.join(dp, d)
            if not os.path.exists(p + '.meta'):
                rel = os.path.relpath(p, root).replace(os.sep, '/')
                write_meta(p, guid_of(key + '/folder/' + rel), 'DefaultImporter', folder=True)


# ---------------------------------------------------------------- textures

def write_psd(path, arr):
    """a flat Photoshop file, as Blender cannot write one: RGB, or RGBA when there is transparency"""
    h, w = arr.shape[:2]
    ch = 4 if (arr[..., 3] < 1).any() else 3
    data = (np.clip(arr[::-1, :, :ch], 0, 1) * 255 + 0.5).astype(np.uint8)  # rows top down
    with open(path, 'wb') as f:
        f.write(b'8BPS' + struct.pack('>H6xHIIHH', 1, ch, h, w, 8, 3))
        f.write(struct.pack('>IIIH', 0, 0, 0, 0))  # no palette, resources or layers; raw data
        for c in range(ch):
            f.write(np.ascontiguousarray(data[..., c]).tobytes())


def texture_importer(default=2048, standalone=None, alpha=1, srgb=1):
    plat = []
    for target, size, over in (('DefaultTexturePlatform', default, 0),
                               ('Standalone', standalone or default, 1 if standalone else 0),
                               ('Android', 512, 1)):
        plat.append({'serializedVersion': 3, 'buildTarget': target, 'maxTextureSize': size, 'resizeAlgorithm': 0,
                     'textureFormat': -1, 'textureCompression': 1, 'compressionQuality': 50,
                     'crunchedCompression': 0, 'allowsAlphaSplitting': 0, 'overridden': over,
                     'ignorePlatformSupport': 0, 'androidETC2FallbackOverride': 0,
                     'forceMaximumCompressionQuality_BC6H_BC7': 0})
    return {'internalIDToNameTable': [], 'externalObjects': {}, 'serializedVersion': 12,
            'mipmaps': {'mipMapMode': 0, 'enableMipMap': 1, 'sRGBTexture': srgb, 'linearTexture': 0},
            'isReadable': 0, 'maxTextureSize': default,
            'textureSettings': {'serializedVersion': 2, 'filterMode': 1, 'aniso': 1, 'mipBias': 0,
                                'wrapU': 0, 'wrapV': 0, 'wrapW': 0},
            'alphaUsage': alpha, 'alphaIsTransparency': alpha, 'textureType': 0, 'textureShape': 1,
            'platformSettings': plat, 'userData': '', 'assetBundleName': '', 'assetBundleVariant': ''}


def write_texture(path, arr, guid, **imp):
    """arr: float RGBA, rows bottom up (Blender's order); .png, .tga or .psd by the path's extension"""
    os.makedirs(os.path.dirname(path), exist_ok=True)
    ext = os.path.splitext(path)[1].lower()
    if ext == '.psd':
        write_psd(path, arr)
    else:
        h, w = arr.shape[:2]
        im = bpy.data.images.new('tex', w, h, alpha=True)
        im.pixels.foreach_set(np.ascontiguousarray(arr, dtype=np.float32).ravel())
        im.filepath_raw = path
        im.file_format = {'.png': 'PNG', '.tga': 'TARGA'}[ext]
        im.save()
        bpy.data.images.remove(im)
    write_meta(path, guid, 'TextureImporter', texture_importer(**imp))
    return guid


# ---------------------------------------------------------------- the model, in Blender

def clear_scene():
    for coll in (bpy.data.objects, bpy.data.meshes, bpy.data.materials, bpy.data.images, bpy.data.armatures):
        for x in list(coll):
            coll.remove(x)


def seg_dist(p, a, b):
    ab = b - a
    t = max(0.0, min(1.0, (p - a).dot(ab) / ab.length_squared))
    return (a + ab * t - p).length


def along(a, b, sx=1.0, sy=1.0):
    """a matrix putting a primitive made along Z between the points a and b, and its length"""
    a, b = Vector(a), Vector(b)
    d = b - a
    rot = Vector((0, 0, 1)).rotation_difference(d.normalized()).to_matrix().to_4x4()
    return Matrix.Translation((a + b) / 2) @ rot @ Matrix.Diagonal((sx, sy, 1, 1)), d.length


class Rig:
    """an armature being built, and skinning to it"""

    def __init__(self, name='Armature'):
        self.data = bpy.data.armatures.new(name)
        self.ob = bpy.data.objects.new(name, self.data)
        bpy.context.scene.collection.objects.link(self.ob)
        bpy.context.view_layer.objects.active = self.ob
        self.ob.select_set(True)
        bpy.ops.object.mode_set(mode='EDIT')
        self.bones = []

    def bone(self, name, head, tail, parent=None, connect=False):
        b = self.data.edit_bones.new(name)
        b.head, b.tail, b.roll = Vector(head), Vector(tail), 0.0
        if parent:
            b.parent = self.data.edit_bones[parent]
            b.use_connect = connect
        assert b.name == name, b.name
        self.bones.append(name)
        return name

    def chain(self, name, points, parent, connect=True):
        """bones name, name.001, ... through the points"""
        names = []
        for i in range(len(points) - 1):
            nm = name if i == 0 else '%s.%03d' % (name, i)
            self.bone(nm, points[i], points[i + 1], parent if i == 0 else names[-1], connect and i > 0)
            names.append(nm)
        return names

    def done(self):
        bpy.ops.object.mode_set(mode='OBJECT')

    def seg(self, b):
        bb = self.data.bones[b]
        return bb.head_local, bb.tail_local

    def skin(self, ob, bones, blend=0.0):
        """each vertex to its nearest bone of `bones`, shared with those up to `blend` m further (at most four) for
        smooth joints"""
        ob.parent = self.ob
        ob.modifiers.new('Armature', 'ARMATURE').object = self.ob
        per_vertex = None
        if isinstance(bones, tuple):  # (index {part: [vertex]}, {part: [bones]})
            idx, by_part = bones
            per_vertex = {}
            for part, vs in idx.items():
                for v in vs:
                    per_vertex[v] = by_part.get(part, by_part.get('*'))
            names = sorted({b for bl in by_part.values() for b in bl})
        else:
            names = list(bones)
        groups = {b: ob.vertex_groups.new(name=b) for b in names}
        segs = {b: self.seg(b) for b in names}
        for v in ob.data.vertices:
            cand = (per_vertex.get(v.index) if per_vertex is not None else None) or names
            ds = sorted((seg_dist(v.co, *segs[b]), b) for b in cand)
            if blend <= 0:
                groups[ds[0][1]].add([v.index], 1.0, 'REPLACE')
                continue
            d0 = ds[0][0]
            ws = [(1.0 - (d - d0) / blend, b) for d, b in ds[:4] if d - d0 < blend]
            tot = sum(w for w, _ in ws)
            for w, b in ws:
                groups[b].add([v.index], w / tot, 'REPLACE')

    def attach(self, ob, bone_name, at):
        """a rigid object on a bone, its origin at `at`"""
        ob.parent = self.ob
        ob.parent_type = 'BONE'
        ob.parent_bone = bone_name
        bpy.context.view_layer.update()
        ob.matrix_world = Matrix.Translation(at)


class Parts:
    """a mesh made of primitives; each named part remembers its vertices"""

    def __init__(self):
        self.bm = bmesh.new()
        self.uv = self.bm.loops.layers.uv.new('UVMap')
        self.parts = {}
        self.flat = set()  # parts whose faces keep their winding (decals)

    def add(self, name, kind, M, mi=0, **kw):
        bm = self.bm
        if kind == 'cyl':
            vs = bmesh.ops.create_cone(bm, cap_ends=kw.get('caps', True), cap_tris=False, segments=kw.get('seg', 12),
                                       radius1=kw['r1'], radius2=kw.get('r2', kw['r1']), depth=kw['depth'],
                                       matrix=M, calc_uvs=True)['verts']
        elif kind == 'sphere':
            vs = bmesh.ops.create_uvsphere(bm, u_segments=kw.get('us', 16), v_segments=kw.get('vs', 12),
                                           radius=kw['r'], matrix=M, calc_uvs=True)['verts']
        elif kind == 'loft':
            vs = self.loft(kw['rings'], kw.get('seg', 16), M, kw.get('caps', (True, True)), kw.get('uv_v', None))
        else:
            vs = bmesh.ops.create_cube(bm, size=1.0, matrix=M, calc_uvs=True)['verts']
        for f in {f for v in vs for f in v.link_faces}:
            f.material_index = mi
        self.parts.setdefault(name, []).extend(vs)
        return vs

    def loft(self, rings, seg, M, caps, uv_v):
        """a tube through elliptic rings [(z, rx, ry[, dy])] around Z, centred at y = dy; u round, v along"""
        bm, uv = self.bm, self.uv
        rows = []
        for r in rings:
            z, rx, ry = r[:3]
            dy = r[3] if len(r) > 3 else 0.0
            row = []
            for i in range(seg):
                a = 2 * math.pi * i / seg
                row.append(bm.verts.new(M @ Vector((rx * math.sin(a), dy - ry * math.cos(a), z))))
            rows.append(row)
        zs = [r[0] for r in rings]
        vv = uv_v or [(z - zs[0]) / ((zs[-1] - zs[0]) or 1) for z in zs]
        for k in range(len(rows) - 1):
            for i in range(seg):
                j = (i + 1) % seg
                f = bm.faces.new((rows[k][i], rows[k][j], rows[k + 1][j], rows[k + 1][i]))
                for loop, (uu, v) in zip(f.loops, ((i / seg, vv[k]), ((i + 1) / seg, vv[k]),
                                                   ((i + 1) / seg, vv[k + 1]), (i / seg, vv[k + 1]))):
                    loop[uv].uv = (uu, v)
        for end, row in ((0, rows[0]), (1, rows[-1])):
            if caps[end]:
                f = bm.faces.new(row if end else list(reversed(row)))
                for loop in f.loops:
                    loop[uv].uv = (0.5, vv[0] if end == 0 else vv[-1])
        return [v for row in rows for v in row]

    def uv_swatch(self, name, uv):
        """every loop of a part's faces to one spot of a palette texture"""
        vs = set(self.parts[name])
        for f in self.bm.faces:
            if all(v in vs for v in f.verts):
                for loop in f.loops:
                    loop[self.uv].uv = uv

    def uv_planar(self, name, center, size, axis='y', back=None):
        """a part's UVs projected along Y (front) or X onto a `size` square around `center`; faces facing +axis get
        `back` if given"""
        vs = set(self.parts[name])
        k = 'xyz'.index(axis)
        a, b = [i for i in range(3) if i != k]
        for f in self.bm.faces:
            if not all(v in vs for v in f.verts):
                continue
            away = back is not None and f.normal[k] > 0.2
            for loop in f.loops:
                if away:
                    loop[self.uv].uv = back
                else:
                    co = loop.vert.co
                    loop[self.uv].uv = (0.5 + (co[a] - center[a]) / size, 0.5 + (co[b] - center[b]) / size)

    def delete(self, name, test):
        """drop a part's faces whose centre passes test(centre)"""
        vs = set(self.parts[name])
        bmesh.ops.delete(self.bm, geom=[f for f in self.bm.faces if all(v in vs for v in f.verts) and
                                       test(f.calc_center_median())], context='FACES')
        self.parts[name] = [v for v in self.parts[name] if v.is_valid]

    def make(self, name, mats, smooth=True):
        bm = self.bm
        keep = {v for p in self.flat for v in self.parts.get(p, [])}
        bmesh.ops.recalc_face_normals(bm, faces=[f for f in bm.faces if not all(v in keep for v in f.verts)])
        bm.verts.index_update()
        idx = {k: sorted({v.index for v in vs if v.is_valid}) for k, vs in self.parts.items()}
        me = bpy.data.meshes.new(name)
        bm.to_mesh(me)
        bm.free()
        if smooth:
            for p in me.polygons:
                p.use_smooth = True
        ob = bpy.data.objects.new(name, me)
        bpy.context.scene.collection.objects.link(ob)
        for m in mats:
            me.materials.append(m if isinstance(m, bpy.types.Material) else
                                bpy.data.materials.get(m) or bpy.data.materials.new(m))
        return ob, idx


def shape_key(ob, name, fn, only=None):
    """a shape key moving each vertex (of `only`, a set of indices, if given) to fn(co), or leaving it (None)"""
    if ob.data.shape_keys is None:
        ob.shape_key_add(name='Basis')
    k = ob.shape_key_add(name=name, from_mix=False)
    n = 0
    for i, v in enumerate(ob.data.vertices):
        if only is not None and i not in only:
            continue
        co = fn(v.co.copy())
        if co is not None and (co - v.co).length > 1e-7:
            k.data[i].co = co
            n += 1
    assert n, name
    return k


def shape_key_to(ob, name, moved):
    """a shape key moving the vertices {index: new position}"""
    if ob.data.shape_keys is None:
        ob.shape_key_add(name='Basis')
    k = ob.shape_key_add(name=name, from_mix=False)
    n = 0
    for i, co in moved.items():
        if (co - ob.data.vertices[i].co).length > 1e-7:
            k.data[i].co = co
            n += 1
    assert n, name
    return k


def export_fbx(path, scale='FBX_SCALE_NONE'):
    """scale: apply_scale_options; FBX_SCALE_NONE gives the 100x bone scale of many Blender exports, FBX_SCALE_ALL scale
    1 throughout"""
    os.makedirs(os.path.dirname(path), exist_ok=True)
    bpy.ops.object.select_all(action='DESELECT')
    bpy.ops.export_scene.fbx(filepath=path, use_selection=False, object_types={'ARMATURE', 'MESH'},
                             apply_unit_scale=True, apply_scale_options=scale, global_scale=1.0,
                             axis_forward='-Z', axis_up='Y', add_leaf_bones=False, bake_anim=False,
                             primary_bone_axis='Y', secondary_bone_axis='X', use_mesh_modifiers=False,
                             mesh_smooth_type='FACE', use_armature_deform_only=False, path_mode='AUTO',
                             embed_textures=False)


class Model:
    """an exported FBX read back the way the converter reads it: its tree, and each node's matrix as Unity has it"""

    def __init__(self, path, upath, guid):
        clear_scene()
        self.path, self.upath, self.guid = path, upath, guid
        self.info = info = u.FBXInfo(path, upath)
        fi = u.FBXInst('gen', guid, info, False, None)
        fi.u = info.unit / 100.0
        bpy.ops.import_scene.fbx(
            filepath=path, global_scale=1.0, use_custom_normals=True, use_image_search=True,
            ignore_leaf_bones=False, automatic_bone_orientation=False, force_connect_children=False,
            use_anim=False, use_custom_props=False, use_prepost_rot=True, use_manual_orientation=False,
            bake_space_transform=False, primary_bone_axis='Y', secondary_bone_axis='X',
            mtl_name_collision_mode='MAKE_UNIQUE')
        b = u.Build.__new__(u.Build)
        b.owner = {}
        b.map_models(fi, list(bpy.data.objects))
        bpy.context.view_layer.update()
        self.world = {}
        for mid, x in fi.bl.items():
            Mw = x[1].matrix_world @ x[1].pose.bones[x[2]].matrix if x[0] == 'bone' else x[1].matrix_world
            self.world[mid] = u.to_unity(Mw, fi.u)
        lost = [info.name(m) for m in info.order if m not in self.world]
        assert not lost, lost
        clear_scene()
        self.mid = {}
        for m in info.order:
            self.mid.setdefault(info.name(m), m)
        self.ids = {}

    def node(self, name):
        return self.mid[name]

    def lossy(self, name):
        Mw = self.world[self.mid[name]]
        return max(Mw.col[0].xyz.length, Mw.col[1].xyz.length, Mw.col[2].xyz.length)

    def shapes(self, mesh_name):
        return self.info.mesh[self.mid[mesh_name]]['shapes']

    def fileids(self, imp):
        """the fileIDs Unity gives the model's objects, with these import settings: {(node, class id): fileID}"""
        self.ids = {}
        for mid, cls, fid in u.fbx_ids(self.guid, self.info, imp):
            self.ids.setdefault((mid, cls), fid)
        return self.ids

    def fid(self, name, cls=1):
        return self.ids[(self.mid[name] if name else 0, cls)]


def humanoid(mapping):
    return [{'boneName': b, 'humanName': h, 'limit': {'min': V(0, 0, 0), 'max': V(0, 0, 0), 'value': V(0, 0, 0),
                                                      'length': 0, 'modified': 0}} for h, b in mapping.items()]


def model_importer(materials, human=None, internal=()):
    """a ModelImporter block: material remaps {name in the model: .mat guid}; human: {Unity bone: node}"""
    return {
        'serializedVersion': 22200,
        'internalIDToNameTable': list(internal),
        'externalObjects': [{'first': {'type': 'UnityEngine:Material', 'assembly': 'UnityEngine.CoreModule',
                                       'name': n}, 'second': R(2100000, g, 2)} for n, g in materials.items()],
        'materials': {'materialImportMode': 1, 'materialName': 0, 'materialSearch': 1, 'materialLocation': 1},
        'animations': {'legacyGenerateAnimations': 4, 'bakeSimulation': 0, 'resampleCurves': 1,
                       'optimizeGameObjects': 0, 'removeConstantScaleCurves': 0, 'motionNodeName': '',
                       'rigImportErrors': '', 'rigImportWarnings': '', 'animationImportErrors': '',
                       'animationImportWarnings': '', 'animationRetargetingWarnings': '',
                       'animationDoRetargetingWarnings': 0, 'importAnimatedCustomProperties': 0,
                       'importConstraints': 0, 'animationCompression': 1, 'animationRotationError': 0.5,
                       'animationPositionError': 0.5, 'animationScaleError': 0.5, 'animationWrapMode': 0,
                       'extraExposedTransformPaths': [], 'extraUserProperties': [], 'clipAnimations': [],
                       'isReadable': 0},
        'meshes': {'lODScreenPercentages': [], 'globalScale': 1, 'meshCompression': 0, 'addColliders': 0,
                   'useSRGBMaterialColor': 1, 'sortHierarchyByName': 1, 'importVisibility': 1,
                   'importBlendShapes': 1, 'importCameras': 1, 'importLights': 1, 'nodeNameCollisionStrategy': 1,
                   'fileIdsGeneration': 2, 'swapUVChannels': 0, 'generateSecondaryUV': 0, 'useFileUnits': 1,
                   'keepQuads': 0, 'weldVertices': 1, 'bakeAxisConversion': 0, 'preserveHierarchy': 0,
                   'skinWeightsMode': 0, 'maxBonesPerVertex': 4, 'minBoneWeight': 0.001, 'optimizeBones': 1,
                   'meshOptimizationFlags': -1, 'indexFormat': 0, 'useFileScale': 1, 'strictVertexDataChecks': 0},
        'tangentSpace': {'normalSmoothAngle': 60, 'normalImportMode': 0, 'tangentImportMode': 3,
                         'normalCalculationMode': 4,
                         'legacyComputeAllNormalsFromSmoothingGroupsWhenMeshHasBlendShapes': 0,
                         'blendShapeNormalImportMode': 1, 'normalSmoothingSource': 0},
        'referencedClips': [], 'importAnimation': 0,
        'humanDescription': {'serializedVersion': 3, 'human': humanoid(human or {}), 'skeleton': [],
                             'armTwist': 0.5, 'foreArmTwist': 0.5, 'upperLegTwist': 0.5, 'legTwist': 0.5,
                             'armStretch': 0.05, 'legStretch': 0.05, 'feetSpacing': 0, 'globalScale': 1,
                             'rootMotionBoneName': '', 'hasTranslationDoF': 0, 'hasExtraRoot': 1,
                             'skeletonHasParents': 1},
        'lastHumanDescriptionAvatarSource': R(0), 'autoGenerateAvatarMappingIfUnspecified': 1,
        'animationType': 3 if human else 2, 'humanoidOversampling': 1, 'avatarSetup': 1 if human else 0,
        'addHumanoidExtraRootOnlyWhenUsingAvatar': 1, 'importBlendShapeDeformPercent': 1,
        'remapMaterialsIfMaterialImportModeIsNone': 0, 'additionalBone': 0,
        'userData': '', 'assetBundleName': '', 'assetBundleVariant': ''}


# ---------------------------------------------------------------- animation clips and controllers

def clip_yaml(name, curves, pptr=(), vectors=()):
    """a clip, one frame unless keyed: curves [(path, class id, attribute, value or [(time, value)] keys, linear)]; pptr
    [(path, class id, attribute, [(time, reference)])] for material slots; vectors [(kind, path, [(time, (x, y, z[,
    w]))])], Transform 'position', 'rotation' (quaternion), 'euler' (degrees) or 'scale', kept by Unity as vector and
    editor float curves"""
    fc = []
    stop = 0.016666668
    vec = {'position': [], 'rotation': [], 'euler': [], 'scale': []}
    editor = []
    for kind, path, ks in vectors:
        axes = 'xyzw' if kind == 'rotation' else 'xyz'
        zero = {a: 0 for a in axes}
        keys = [{'serializedVersion': 3, 'time': t, 'value': {a: v[i] for i, a in enumerate(axes)}, 'inSlope': zero,
                 'outSlope': zero, 'tangentMode': 0, 'weightedMode': 0, 'inWeight': zero, 'outWeight': zero}
                for t, v in ks]
        vec[kind].append({'curve': {'serializedVersion': 2, 'm_Curve': keys, 'm_PreInfinity': 2, 'm_PostInfinity': 2,
                                    'm_RotationOrder': 4}, 'path': path})
        attr = {'position': 'm_LocalPosition', 'rotation': 'm_LocalRotation', 'euler': 'localEulerAnglesRaw',
                'scale': 'm_LocalScale'}[kind]
        for i, a in enumerate(axes):
            editor.append({'curve': {'serializedVersion': 2, 'm_Curve': [
                {'serializedVersion': 3, 'time': t, 'value': v[i], 'inSlope': 0, 'outSlope': 0, 'tangentMode': 0,
                 'weightedMode': 0, 'inWeight': 0.33333334, 'outWeight': 0.33333334} for t, v in ks],
                'm_PreInfinity': 2, 'm_PostInfinity': 2, 'm_RotationOrder': 4}, 'attribute': '%s.%s' % (attr, a),
                'path': path, 'classID': 4, 'script': R(0), 'flags': 0})
        stop = max(stop, ks[-1][0])
    for path, cid, attr, val in curves:
        if isinstance(val, (list, tuple)):
            ks = list(val)
            keys = []
            for i, (t, v) in enumerate(ks):
                a = ks[max(i - 1, 0)]
                b = ks[min(i + 1, len(ks) - 1)]
                sin = (v - a[1]) / (t - a[0]) if t > a[0] else 0
                sout = (b[1] - v) / (b[0] - t) if b[0] > t else 0
                keys.append({'serializedVersion': 3, 'time': t, 'value': v, 'inSlope': sin, 'outSlope': sout,
                             'tangentMode': 69, 'weightedMode': 0, 'inWeight': 0.33333334, 'outWeight': 0.33333334})
            stop = max(stop, ks[-1][0])
        else:
            keys = [{'serializedVersion': 3, 'time': t, 'value': val, 'inSlope': 0, 'outSlope': 0,
                     'tangentMode': 136 if cid == 137 else 103, 'weightedMode': 0,
                     'inWeight': 0.33333334, 'outWeight': 0.33333334} for t in (0.0, 0.016666668)]
        fc.append({'curve': {'serializedVersion': 2, 'm_Curve': keys, 'm_PreInfinity': 2, 'm_PostInfinity': 2,
                             'm_RotationOrder': 4},
                   'attribute': attr, 'path': path, 'classID': cid, 'script': R(0), 'flags': 0})
    pc = [{'curve': [{'time': t, 'value': r} for t, r in keys], 'attribute': attr, 'path': path, 'classID': cid,
           'script': R(0), 'flags': 2} for path, cid, attr, keys in pptr]
    body = base(m_Name=name, serializedVersion=7, m_Legacy=0, m_Compressed=0, m_UseHighQualityCurve=1,
                m_RotationCurves=vec['rotation'], m_CompressedRotationCurves=[], m_EulerCurves=vec['euler'],
                m_PositionCurves=vec['position'], m_ScaleCurves=vec['scale'], m_FloatCurves=fc, m_PPtrCurves=pc,
                m_SampleRate=60, m_WrapMode=0,
                m_Bounds={'m_Center': V(0, 0, 0), 'm_Extent': V(0, 0, 0)},
                m_ClipBindingConstant={'genericBindings': [], 'pptrCurveMapping': []},
                m_AnimationClipSettings={'serializedVersion': 2, 'm_AdditiveReferencePoseClip': R(0),
                                         'm_AdditiveReferencePoseTime': 0, 'm_StartTime': 0,
                                         'm_StopTime': stop, 'm_OrientationOffsetY': 0, 'm_Level': 0,
                                         'm_CycleOffset': 0, 'm_HasAdditiveReferencePose': 0, 'm_LoopTime': 0,
                                         'm_LoopBlend': 0, 'm_LoopBlendOrientation': 0,
                                         'm_LoopBlendPositionY': 0, 'm_LoopBlendPositionXZ': 0,
                                         'm_KeepOriginalOrientation': 0, 'm_KeepOriginalPositionY': 1,
                                         'm_KeepOriginalPositionXZ': 0, 'm_HeightFromFeet': 0, 'm_Mirror': 0},
                m_EditorCurves=fc + [e for e in editor if not e['attribute'].startswith('localEuler')],
                m_EulerEditorCurves=[e for e in editor if e['attribute'].startswith('localEuler')],
                m_HasGenericRootTransform=0,
                m_HasMotionFloatCurves=0, m_Events=[])
    return HEAD + doc(74, 7400000, 'AnimationClip', body)


def blend_tree_yaml(name, param, children, typ=0, param_y=''):
    """a blend tree asset: children [(clip guid, threshold, or (x, y) in a 2D tree)]; typ 0 1D, 1 2D simple
    directional, 2 2D freeform directional, 3 2D freeform cartesian, 4 direct (param: the children's parameter)"""
    kids = []
    for guid, at in children:
        pos = at if isinstance(at, tuple) else (0, 0)
        kids.append({'serializedVersion': 2, 'm_Motion': R(7400000, guid, 2),
                     'm_Threshold': 0 if isinstance(at, tuple) else at, 'm_Position': {'x': pos[0], 'y': pos[1]},
                     'm_TimeScale': 1, 'm_CycleOffset': 0, 'm_DirectBlendParameter': param, 'm_Mirror': 0})
    th = [k['m_Threshold'] for k in kids] or [0]
    body = base(m_Name=name, m_Childs=kids, m_BlendParameter=param, m_BlendParameterY=param_y or param,
                m_MinThreshold=min(th), m_MaxThreshold=max(th), m_UseAutomaticThresholds=0,
                m_NormalizedBlendValues=0, m_BlendType=typ)
    return HEAD + doc(206, 20600000, 'BlendTree', body)


GESTURE_STATES = ['Fist', 'Open', 'Point', 'Victory', 'RockNRoll', 'HandGun', 'ThumbsUp']


class Controller:
    """an AnimatorController being built: layers of state machines; clips are {name: guid}"""

    def __init__(self, ids, clips):
        self.ids, self.clips = ids, clips
        self.docs, self.layers, self.params = [], [], []

    def param(self, name, typ, default=0):
        """typ: 1 float, 3 int, 4 bool"""
        self.params.append({'m_Name': name, 'm_Type': typ, 'm_DefaultFloat': float(default) if typ == 1 else 0,
                            'm_DefaultInt': default if typ == 3 else 0, 'm_DefaultBool': default if typ == 4 else 0,
                            'm_Controller': R(9100000)})

    def state(self, name, motion=None, behaviours=()):
        sid = self.ids()
        body = base(serializedVersion=6, m_Name=name, m_Speed=1, m_CycleOffset=0, m_Transitions=[],
                    m_StateMachineBehaviours=[R(x) for x in behaviours], m_Position=V(50, 50, 0), m_IKOnFeet=0,
                    m_WriteDefaultValues=0, m_Mirror=0, m_SpeedParameterActive=0, m_MirrorParameterActive=0,
                    m_CycleOffsetParameterActive=0, m_TimeParameterActive=0,
                    m_Motion=R(7400000, self.clips[motion], 2) if motion else R(0), m_Tag='', m_SpeedParameter='',
                    m_MirrorParameter='', m_CycleOffsetParameter='', m_TimeParameter='')
        self.docs.append((1102, sid, 'AnimatorState', body))
        return sid, body

    def transition(self, dst, conds, self_ok=1):
        """conds [(mode, parameter, threshold)]: mode 1 If, 2 IfNot, 3 Greater, 4 Less, 6 Equals, 7 NotEqual"""
        tid = self.ids()
        body = base(m_Name='', m_Conditions=[{'m_ConditionMode': m, 'm_ConditionEvent': p, 'm_EventTreshold': t}
                                             for m, p, t in conds],
                    m_DstStateMachine=R(0), m_DstState=R(dst), m_Solo=0, m_Mute=0, m_IsExit=0, serializedVersion=3,
                    m_TransitionDuration=0.1, m_TransitionOffset=0, m_ExitTime=0.75, m_HasExitTime=0,
                    m_HasFixedDuration=1, m_InterruptionSource=0, m_OrderedInterruption=1,
                    m_CanTransitionToSelf=self_ok)
        self.docs.append((1101, tid, 'AnimatorStateTransition', body))
        return tid

    def machine(self, name, states, default, anys=()):
        mid = self.ids()
        body = base(serializedVersion=6, m_Name=name,
                    m_ChildStates=[{'serializedVersion': 1, 'm_State': R(s), 'm_Position': V(300, 60 * i, 0)}
                                   for i, s in enumerate(states)],
                    m_ChildStateMachines=[], m_AnyStateTransitions=[R(t) for t in anys], m_EntryTransitions=[],
                    m_StateMachineTransitions={}, m_StateMachineBehaviours=[], m_AnyStatePosition=V(50, 20, 0),
                    m_EntryPosition=V(50, 120, 0), m_ExitPosition=V(800, 120, 0),
                    m_ParentStateMachinePosition=V(800, 20, 0), m_DefaultState=R(default))
        self.docs.append((1107, mid, 'AnimatorStateMachine', body))
        return mid

    def tracking(self, eyes=2, mouth=0):
        """a VRC Animator Tracking Control: 2 = animation drives the eyes (while this face shows)"""
        tid = self.ids()
        body = base(m_GameObject=R(0), m_Enabled=1, m_EditorHideFlags=0,
                    m_Script=R(u.TRACKING_FID, u.DESC_GUID, 3), m_Name='', m_EditorClassIdentifier='',
                    trackingHead=0, trackingLeftHand=0, trackingRightHand=0, trackingHip=0, trackingLeftFoot=0,
                    trackingRightFoot=0, trackingLeftFingers=0, trackingRightFingers=0, trackingEyes=eyes,
                    trackingMouth=mouth, debugString='')
        body['m_ObjectHideFlags'] = 1
        self.docs.append((114, tid, 'MonoBehaviour', body))
        return tid

    def layer(self, name, sm, weight=1.0, mask=None):
        """mask: an AvatarMask's guid (VRChat's SDK's, say)"""
        self.layers.append({'serializedVersion': 5, 'm_Name': name, 'm_StateMachine': R(sm),
                            'm_Mask': R(31900000, mask, 2) if mask else R(0),
                            'm_Motions': [], 'm_Behaviours': [], 'm_BlendingMode': 0, 'm_SyncedLayerIndex': -1,
                            'm_DefaultWeight': weight, 'm_IKPass': 0, 'm_SyncedLayerAffectsTiming': 0,
                            'm_Controller': R(9100000)})

    def hand(self, side, faces, self_ok=0):
        """a gesture layer: faces {gesture state: (clip, stop eye tracking)}"""
        p = 'Gesture' + side
        idle, _ = self.state('Idle')
        sts, anys = [idle], [self.transition(idle, [(6, p, 0)], self_ok)]
        for g, st in enumerate(GESTURE_STATES, 1):
            clip, track = faces.get(st, (None, False))
            s, _ = self.state(st, clip, [self.tracking()] if track else [])
            sts.append(s)
            anys.append(self.transition(s, [(6, p, g)], self_ok))
        self.layer(side + ' Hand', self.machine(side + ' Hand', sts, idle, anys))

    def motion_time(self, name, param, clip):
        """a layer playing a clip at the time a float parameter gives (a radial puppet's)"""
        s, b = self.state(name, clip)
        b['m_TimeParameterActive'] = 1
        b['m_TimeParameter'] = param
        self.layer(name, self.machine(name, [s], s))

    def toggle(self, name, param, on_clip, off_clip):
        off, offb = self.state(name + ' Off', off_clip)
        on, onb = self.state(name + ' On', on_clip)
        offb['m_Transitions'].append(R(self.transition(on, [(1, param, 0)])))
        onb['m_Transitions'].append(R(self.transition(off, [(2, param, 0)])))
        self.layer(name, self.machine(name, [off, on], off))

    def choice(self, name, param, clips):
        """an int parameter picking one of clips (by value)"""
        sts = [self.state('%s %d' % (name, k), c)[0] for k, c in enumerate(clips)]
        self.layer(name, self.machine(name, sts, sts[0], [self.transition(s, [(6, param, k)], 0)
                                                         for k, s in enumerate(sts)]))

    def yaml(self, name):
        ctl = base(m_Name=name, serializedVersion=5, m_AnimatorParameters=self.params, m_AnimatorLayers=self.layers)
        return HEAD + doc(91, 9100000, 'AnimatorController', ctl) + ''.join(doc(c, f, k, b) for c, f, k, b in self.docs)


# ---------------------------------------------------------------- VRChat's assets and components

def control(name, typ, param='', value=1, sub=None, subparams=()):
    """an Expressions Menu control: typ 101 button, 102 toggle, 103 sub menu, 201/202/203 puppets"""
    return {'name': name, 'icon': R(0), 'type': typ, 'parameter': {'name': param}, 'value': value, 'style': 0,
            'subMenu': R(11400000, sub, 2) if sub else R(0), 'subParameters': [{'name': p} for p in subparams],
            'labels': []}


def asset114(name, script, **kw):
    return HEAD + doc(114, 11400000, 'MonoBehaviour', base(
        m_GameObject=R(0), m_Enabled=1, m_EditorHideFlags=0, m_Script=R(script, u.DESC_GUID, 3), m_Name=name,
        m_EditorClassIdentifier='', **kw))


def menu_asset(name, controls):
    return asset114(name, u.MENU_FID, controls=controls)


def params_asset(name, params):
    """params [(name, valueType 0 int / 1 float / 2 bool, default, saved)]"""
    return asset114(name, u.PARAMS_FID, isEmpty=0, parameters=[
        {'name': n, 'valueType': t, 'saved': s, 'defaultValue': v, 'networkSynced': 1} for n, t, v, s in params])


def descriptor(body_smr, eye_l, eye_r, fx_guid, menu_guid, params_guid, visemes, blink, view=(0, 1.6, 0.1)):
    """a VRC Avatar Descriptor: visemes (the 15 shape key names), blink (the index of the blink shape key)"""
    layers = [{'isEnabled': 0, 'type': t, 'animatorController': R(0), 'mask': R(0), 'isDefault': 1}
              for t in (0, 2, 3, 4)]
    layers.append({'isEnabled': 0, 'type': 5, 'animatorController': R(9100000, fx_guid, 2), 'mask': R(0),
                   'isDefault': 0})
    return dict(m_Enabled=1, m_EditorHideFlags=0, m_Script=R(u.DESC_FID, u.DESC_GUID, 3), m_Name='',
                m_EditorClassIdentifier='', Name='', ViewPosition=V(*view), Animations=0, ScaleIPD=1,
                lipSync=3, lipSyncJawBone=R(0), lipSyncJawClosed=Q(0, 0, 0, 1), lipSyncJawOpen=Q(0, 0, 0, 1),
                VisemeSkinnedMesh=R(body_smr), MouthOpenBlendShapeName='Facial_Blends.Jaw_Down',
                VisemeBlendShapes=list(visemes), unityVersion='',
                portraitCameraPositionOffset=V(0, 0, 0), portraitCameraRotationOffset=Q(0, 1, 0, 0),
                networkIDs=[], customExpressions=1, expressionsMenu=R(11400000, menu_guid, 2),
                expressionParameters=R(11400000, params_guid, 2), enableEyeLook=1,
                customEyeLookSettings={'eyeMovement': {'confidence': 0.5, 'excitement': 0.5},
                                       'leftEye': R(eye_l), 'rightEye': R(eye_r),
                                       'eyesLookingStraight': {'linked': 1, 'left': Q(0, 0, 0, 1),
                                                               'right': Q(0, 0, 0, 1)},
                                       'eyelidType': 2, 'upperLeftEyelid': R(0), 'upperRightEyelid': R(0),
                                       'lowerLeftEyelid': R(0), 'lowerRightEyelid': R(0),
                                       'eyelidsSkinnedMesh': R(body_smr),
                                       'eyelidsBlendshapes': struct.pack('<iii', blink, -1, -1).hex()},
                customizeAnimationLayers=1, baseAnimationLayers=layers,
                specialAnimationLayers=[{'isEnabled': 0, 'type': t, 'animatorController': R(0), 'mask': R(0),
                                         'isDefault': 1} for t in (6, 7, 8)],
                AnimationPreset=R(0), animationHashSet=[], autoFootsteps=1, autoLocomotion=1)


def physbone(root_tf, colliders, radius=0.0, pull=0.2, spring=0.2, stiffness=0.2, gravity=0.0, immobile=0.0,
             multi=0, max_angle=45, ignore=(), limit=None, max_angle_z=None, limit_rot=(0, 0, 0), immobile_type=0):
    """a VRC PhysBone; root_tf 0 = the object it is on. Lengths are in the root's own units. limit: 1 Angle, 2 Hinge,
    3 Polar (max_angle its pitch, max_angle_z its yaw), None: Angle if max_angle; limit_rot: its Rotation (Unity's
    Euler angles); immobile_type 0 All Motion, 1 World"""
    return dict(m_Enabled=1, m_EditorHideFlags=0, m_Script=R(u.PHYSBONE_FID, u.DYN_GUID, 3), m_Name='',
                m_EditorClassIdentifier='', foldout_transforms=1, foldout_forces=1, foldout_collision=1,
                foldout_stretchsquish=1, foldout_limits=1, foldout_grabpose=1, foldout_options=1,
                foldout_gizmos=0, version=1, integrationType=0, rootTransform=R(root_tf),
                ignoreTransforms=[R(x) for x in ignore], ignoreOtherPhysBones=1, endpointPosition=V(0, 0, 0),
                multiChildType=multi, pull=pull, spring=spring, stiffness=stiffness, gravity=gravity,
                gravityFalloff=0, immobileType=immobile_type, immobile=immobile, allowCollision=1,
                collisionFilter={'allowSelf': 1, 'allowOthers': 1}, radius=radius,
                colliders=[R(c) for c in colliders], limitType=(1 if max_angle else 0) if limit is None else limit,
                maxAngleX=max_angle, maxAngleZ=max_angle if max_angle_z is None else max_angle_z,
                limitRotation=V(*limit_rot), allowGrabbing=1, allowPosing=1, grabMovement=0.5,
                maxStretch=0, maxSquish=0, stretchMotion=0, snapToHand=0, parameter='', isAnimated=0,
                resetWhenDisabled=0)


def pb_collider(shape, radius, height, pos, rot=(0, 0, 0, 1), inside=0):
    """a VRC PhysBone Collider: shape 0 sphere, 1 capsule, 2 plane (its normal its Y); inside 1: it keeps bones in"""
    return dict(m_Enabled=1, m_EditorHideFlags=0, m_Script=R(u.COLLIDER_FID, u.DYN_GUID, 3), m_Name='',
                m_EditorClassIdentifier='', rootTransform=R(0), shapeType=shape, insideBounds=inside, radius=radius,
                height=height, position=V(*pos), rotation=Q(*rot), bonesAsSpheres=0)


CONTACT_GUID = '80f1b8067b0760e4bb45023bc2e9de66'  # VRC.SDK3.Dynamics.Contact.dll
RECEIVER_FID = -1450912254


def contact_receiver(param, radius, tags=('Hand', 'Finger'), pos=(0, 0, 0), kind=0):
    """a VRC Contact Receiver: kind 0 constant, 1 on enter, 2 proximity"""
    return dict(m_Enabled=1, m_EditorHideFlags=0, m_Script=R(RECEIVER_FID, CONTACT_GUID, 3), m_Name='',
                m_EditorClassIdentifier='', rootTransform=R(0), shapeType=0, radius=radius, height=0,
                position=V(*pos), rotation=Q(0, 0, 0, 1), collisionTags=list(tags), allowSelf=0, allowOthers=1,
                localOnly=0, receiverType=kind, parameter=param, minVelocity=0.05)


MA_GUID = {'MergeArmature': '2df373bf91cf30b4bbd495e11cb1a2ec', 'BoneProxy': '42581d8044b64899834d3d515ab3a144',
           'MoveTo': '4e6bb6a99e499d2489ccf296662fa3cd', 'MenuItem': '3b29d45007c5493d926d2cd45a489529',
           'ObjectToggle': 'a162bb8ec7e24a5abcf457887f1df3fa', 'MergeAnimator': '1bb122659f724ebf85fe095ac02dc339',
           'MenuInstaller': '7ef83cb0c23d4d7c9d41021e544a1978', 'Parameters': '71a96d4ea0c344f39e277d82035bf9bd',
           'ShapeChanger': '2db441f589c3407bb6fb5f02ff8ab541', 'MenuGroup': '97e46a47dd8a425eb4ce9411defe313d',
           'MaterialSetter': '0adf335711644e34b6c635e94ae61fa7', 'MaterialSwap': 'b259b73280ead4e4fbbdafc5e29175d1',
           'BlendshapeSync': '6fd7cab7d93b403280f2f9da978d8a4f', 'ReplaceObject': '7e949680c0864ee7b441d9b2c93b890b',
           'VisibleHeadAccessory': '33dac8cfeaeb4c399ddd90597f849f70',
           'MeshSettings': '560fdafd46c74b2db6422fdf0e7f2363', 'PlatformFilter': '8c8a67d5c01849629fa90c3b2eded93f',
           'ScaleAdjuster': '09a660aa9d4e47d992adcac5a05dd808', 'MeshCutter': '762726b8618cac7419e39bdc2b572b3d',
           'VertexFilterByAxis': '660848d04d7443b5b6fcfb627e6be5ea', 'VertexFilterByBone': 'f8e2c9a1b3d44c6d9a7e5f2c1b8d3e4f',
           'VertexFilterByMask': '96a7b00b1dae4a02b61b29bf02241063', 'VertexFilterByShape': 'da7788c69fae9ff4abae088a0dc92c5b',
           'VertexFilterByUVTile': '8c38d6a064dbe9b91f24ee30e85c3c4f', 'MergeBlendTree': '229dd561ca024a6588e388160921a70f',
           'MenuInstallTarget': '1fad1419b52a42ae89b0df52eb861e47', 'FloorAdjuster': 'ba18e6eae93342fd8774b3f3f132928a',
           'GlobalCollider': '49bb23f95a7baca4186efa68bc5891b6', 'WorldFixedObject': '0e2d9f1d69e34b92a96e6cc162770fad'}


def ma(kind, script_guid=None, **kw):
    """a Modular Avatar component (script_guid for the kinds not in MA_GUID)"""
    return dict(m_Enabled=1, m_EditorHideFlags=0, m_Script=R(11500000, script_guid or MA_GUID[kind], 3), m_Name='',
                m_EditorClassIdentifier='', **kw)


def ma_ref(path, target=0):
    return {'referencePath': path, 'targetObject': R(target)}


def ma_item(name='', param='', value=1, default=0, auto=1, kind=102, saved=1, synced=1, subparams=()):
    """an MA Menu Item: kind 102 toggle, 101 button, 103 sub menu (of its children: MenuSource 1), 203 a radial
    puppet (of the float subparams[0])"""
    return ma('MenuItem', Control={
        'name': name, 'icon': R(0), 'type': kind, 'parameter': {'name': param}, 'value': value, 'style': 0,
        'subMenu': R(0), 'subParameters': [{'name': p} for p in subparams], 'labels': []}, MenuSource=1,
        menuSource_otherObjectChildren=R(0), isSynced=synced, isSaved=saved, isDefault=default,
        automaticValue=auto, label='')


# ---------------------------------------------------------------- prefab variants

class Variant:
    """a prefab variant of another prefab or a model: PrefabInstance, modifications, stubs of the objects its own point
    at, and its own objects"""
    KINDS = {1: 'GameObject', 4: 'Transform', 137: 'SkinnedMeshRenderer', 33: 'MeshFilter', 23: 'MeshRenderer',
             114: 'MonoBehaviour', 95: 'Animator'}

    def __init__(self, ids, src_guid, parent_tf=0):
        self.ids = ids
        self.guid, self.parent_tf = src_guid, parent_tf
        self.pi = ids() & u.MASK63
        self.docs, self.strip = [], {}
        self.mods, self.added_go, self.added_c, self.removed_c, self.removed_go = [], [], [], [], []

    def src(self, fid):
        return R(fid, self.guid, 3)

    def own(self, fid):
        """the fileID the source's object fid has in the file holding this instance"""
        return (fid ^ self.pi) & u.MASK63

    def stub(self, fid, cls):
        if fid not in self.strip:
            self.strip[fid] = self.own(fid)
            self.docs.append(doc(cls, self.own(fid), self.KINDS[cls], {
                'm_CorrespondingSourceObject': self.src(fid), 'm_PrefabInstance': R(self.pi), 'm_PrefabAsset': R(0)},
                stripped=True))
        return self.strip[fid]

    def mod(self, fid, path, value=None, obj=None):
        self.mods.append({'target': self.src(fid), 'propertyPath': path, 'value': '' if value is None else value,
                          'objectReference': obj if obj is not None else R(0)})

    def root(self, go, tf, name, pos=(0, 0, 0)):
        for k, v in (('m_LocalPosition.x', pos[0]), ('m_LocalPosition.y', pos[1]), ('m_LocalPosition.z', pos[2]),
                     ('m_LocalRotation.w', 1), ('m_LocalRotation.x', 0), ('m_LocalRotation.y', 0),
                     ('m_LocalRotation.z', 0), ('m_LocalEulerAnglesHint.x', 0), ('m_LocalEulerAnglesHint.y', 0),
                     ('m_LocalEulerAnglesHint.z', 0)):
            self.mod(tf, k, v)
        self.mod(go, 'm_Name', name)

    def component(self, go, cls, kind, body):
        fid = self.ids()
        body = dict(body)
        body['m_GameObject'] = R(self.stub(go, 1))
        self.docs.append(doc(cls, fid, kind, base(**body)))
        self.added_c.append({'targetCorrespondingSourceObject': self.src(go), 'insertIndex': -1, 'addedObject': R(fid)})
        return fid

    def gameobject(self, name, parent_tf, comps, active=1, own_parent=None, pos=(0, 0, 0), tag='Untagged'):
        """an added GameObject under the source's parent_tf, or under one of the variant's own (own_parent)"""
        go, tf = self.ids(), self.ids()
        cids = [self.ids() for _ in comps]
        self.docs.append(doc(1, go, 'GameObject', base(
            serializedVersion=6, m_Component=[{'component': R(c)} for c in [tf] + cids], m_Layer=0, m_Name=name,
            m_TagString=tag, m_Icon=R(0), m_NavMeshLayer=0, m_StaticEditorFlags=0, m_IsActive=active)))
        self.docs.append(doc(4, tf, 'Transform', base(
            m_GameObject=R(go), serializedVersion=2, m_LocalRotation=Q(0, 0, 0, 1), m_LocalPosition=V(*pos),
            m_LocalScale=V(1, 1, 1), m_ConstrainProportionsScale=0, m_Children=[],
            m_Father=R(own_parent if own_parent else self.stub(parent_tf, 4)), m_LocalEulerAnglesHint=V(0, 0, 0))))
        for c, (cls, kind, body) in zip(cids, comps):
            body = dict(body)
            body['m_GameObject'] = R(go)
            self.docs.append(doc(cls, c, kind, base(**body)))
        if not own_parent:
            self.added_go.append({'targetCorrespondingSourceObject': self.src(parent_tf), 'insertIndex': -1,
                                  'addedObject': R(tf)})
        return go, tf, cids

    def text(self):
        pi = {'m_ObjectHideFlags': 0, 'serializedVersion': 2,
              'm_Modification': {'serializedVersion': 3, 'm_TransformParent': R(self.parent_tf),
                                 'm_Modifications': self.mods,
                                 'm_RemovedComponents': [self.src(f) for f in self.removed_c],
                                 'm_RemovedGameObjects': [self.src(f) for f in self.removed_go],
                                 'm_AddedGameObjects': self.added_go, 'm_AddedComponents': self.added_c},
              'm_SourcePrefab': R(100100000, self.guid, 3)}
        return doc(1001, self.pi, 'PrefabInstance', pi) + ''.join(self.docs)


# ---------------------------------------------------------------- packages

def unitypackage(root, prefixes, out, pathname_extra=''):
    """a .unitypackage of the assets under root within prefixes (folders included): a gzipped tar of <guid>/asset (none
    for folders), <guid>/asset.meta and <guid>/pathname; pathname_extra is what some Unity versions append (a line
    with 00)"""
    entries = []
    for dp, dns, fns in os.walk(os.path.join(root, 'Assets')):
        for n in dns + fns:
            p = os.path.join(dp, n)
            rel = os.path.relpath(p, root).replace(os.sep, '/')
            if n.endswith('.meta') or not any(rel == x or rel.startswith(x + '/') or x.startswith(rel + '/')
                                              for x in prefixes):
                continue
            if not os.path.exists(p + '.meta'):
                continue
            with open(p + '.meta', encoding='utf-8') as f:
                g = next(l.split(':', 1)[1].strip() for l in f if l.startswith('guid:'))
            entries.append((g, rel, p))
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode='w', format=tarfile.GNU_FORMAT) as tf:
        def add(name, data):
            ti = tarfile.TarInfo(name)
            ti.size, ti.mtime, ti.mode = len(data), 1727136000, 0o644
            tf.addfile(ti, io.BytesIO(data))
        for g, rel, p in sorted(entries):
            if os.path.isfile(p):
                with open(p, 'rb') as f:
                    add(g + '/asset', f.read())
            with open(p + '.meta', 'rb') as f:
                add(g + '/asset.meta', f.read())
            add(g + '/pathname', (rel + pathname_extra).encode('utf-8'))
    with open(out, 'wb') as f:
        with gzip.GzipFile(filename='archtemp.tar', fileobj=f, mode='wb', mtime=0) as gz:
            gz.write(buf.getvalue())
    return len(entries)


class SJISInfo(zipfile.ZipInfo):
    """a zip entry whose name is written in Shift-JIS, without the UTF-8 flag (zipfile always sets it)"""

    def _encodeFilenameFlags(self):
        return self.filename.encode('cp932'), self.flag_bits & ~0x800


def booth_zip(out, files):
    """a zip as Booth downloads made on Windows are: names in Shift-JIS, without the UTF-8 flag.
    files: [(name in the zip, bytes or a file path)]"""
    with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED) as z:
        for name, data in files:
            if not isinstance(data, bytes):
                with open(data, 'rb') as f:
                    data = f.read()
            zi = SJISInfo(name, date_time=(2024, 9, 24, 12, 0, 0))
            zi.compress_type = zipfile.ZIP_DEFLATED
            z.writestr(zi, data)
    with zipfile.ZipFile(out) as z:  # the names really are Shift-JIS, with no UTF-8 flag
        for info, (name, _) in zip(z.infolist(), files):
            assert not info.flag_bits & 0x800 and info.filename.encode('cp437').decode('cp932') == name, info.filename


# ---------------------------------------------------------------- VRCFury's components

VRCF_GUID = 'd9e94e501a2d4c95bff3d5601013d923'  # VF.Model.VRCFury


class Refs:
    """a MonoBehaviour's [SerializeReference] objects, as Unity writes them: version 2 (newer Unity: RefIds with
    64-bit ids) or version 1 (Unity 2019: keyed by 8-digit ids)"""

    def __init__(self, ids, version=2):
        self.ids, self.version = ids, version
        self.items = []

    def add(self, cls, data, ns='VF.Model.Feature', asm='VRCFury'):
        rid = (self.ids() & ((1 << 62) - 1)) if self.version == 2 else len(self.items)
        self.items.append((rid, cls, ns, asm, data))
        return F(rid=rid) if self.version == 2 else F(id=rid)

    def action(self, cls, **kw):
        d = {'version': kw.pop('version', 1 if cls in ('ObjectToggleAction', 'MaterialAction') else 0),
             'desktopActive': 0, 'androidActive': 0, 'localOnly': 0, 'remoteOnly': 0}
        d.update(kw)
        return self.add(cls, d, ns='VF.Model.StateAction')

    def block(self):
        if self.version == 2:
            return {'version': 2, 'RefIds': [{'rid': r, 'type': F(**{'class': c, 'ns': ns, 'asm': asm}), 'data': d}
                                             for r, c, ns, asm, d in self.items]}
        out = {'version': 1}
        for r, c, ns, asm, d in self.items:
            out['%08d' % r] = {'type': F(**{'class': c, 'ns': ns, 'asm': asm}), 'data': d}
        return out


def vrcfury(refs, content=None, features=()):
    """a VRCFury component: its feature in content (VRCFury since 2023), or a list in config.features (older)"""
    d = dict(m_Enabled=1, m_EditorHideFlags=0, m_Script=R(11500000, VRCF_GUID, 3), m_Name='',
             m_EditorClassIdentifier='', version=3 if content is not None else 2,
             unityVersion='2022.3.22f1' if refs.version == 2 else '2019.4.31f1', vrcfuryVersion='1.1206.0',
             somethingIsBroken=0, config={'features': list(features)})
    if content is not None:
        d['content'] = content
    d['references'] = refs.block()
    return d


def guid_asset(guid, fid, path, main=True):
    """one of VRCFury's asset fields (GuidAnimationClip, GuidController...): the reference, and its id"""
    return {'version': 1, 'fileID': 0, 'guid': '', 'id': ('%s|%s' % (guid, path)) if main else
            ('%s:%d|%s' % (guid, fid, path)), 'objRef': R(fid, guid, 2)}


NO_ASSET = {'version': 1, 'fileID': 0, 'guid': '', 'id': '', 'objRef': R(0)}


def vf_state(actions):
    return {'actions': list(actions)}


def vf_armature_link(prop_bone, bone=0, recursive=1, align=1, suffix='', legacy=None):
    """an Armature Link (version 7); legacy: the fields of an older one ({'version': 5, 'linkMode': 3, ...})"""
    d = {'version': 7, 'propBone': R(prop_bone),
         'linkTo': [{'useBone': 1, 'bone': bone, 'useObj': 0, 'obj': R(0), 'offset': ''}],
         'removeBoneSuffix': suffix, 'removeParentConstraints': 1, 'forceMergedName': '', 'forceOneWorldScale': 0,
         'recursive': recursive, 'alignPosition': align, 'alignRotation': align, 'alignScale': align,
         'autoScaleFactor': 1, 'scalingFactorPowersOf10Only': 1, 'skinRewriteScalingFactor': 1,
         'useOptimizedUpload': 0, 'useBoneMerging': 0, 'keepBoneOffsets': 0, 'physbonesOnAvatarBones': 0,
         'boneOnAvatar': 0, 'bonePathOnAvatar': '', 'fallbackBones': [], 'keepBoneOffsets2': 0, 'linkMode': 4}
    if legacy:  # an old save: the newer fields are not there yet
        for k in ('linkTo', 'recursive', 'alignPosition', 'alignRotation', 'alignScale', 'autoScaleFactor',
                  'scalingFactorPowersOf10Only'):
            d.pop(k)
        d.update(legacy)
    return d


def vf_toggle(name, actions, on=0, saved=1, tag='', version=3, off_state=0, hold=0):
    return {'version': version, 'name': name, 'state': vf_state(actions), 'saved': saved, 'slider': 0,
            'sliderInactiveAtZero': 1, 'securityEnabled': 0, 'defaultOn': on, 'includeInRest': 0,
            'exclusiveOffState': off_state, 'enableExclusiveTag': 1 if tag else 0, 'exclusiveTag': tag,
            'resetPhysbones': [], 'hasExitTime': 0, 'enableIcon': 0, 'icon': NO_ASSET, 'enableDriveGlobalParam': 0,
            'driveGlobalParam': '', 'separateLocal': 0, 'localState': vf_state([]), 'hasTransition': 0,
            'transitionStateIn': vf_state([]), 'transitionStateOut': vf_state([]), 'transitionTimeIn': 0,
            'transitionTimeOut': 0, 'localTransitionStateIn': vf_state([]), 'localTransitionStateOut': vf_state([]),
            'localTransitionTimeIn': 0, 'localTransitionTimeOut': 0, 'simpleOutTransition': 1,
            'defaultSliderValue': 0, 'useGlobalParam': 0, 'globalParam': '', 'holdButton': hold,
            'invertRestLogic': 0, 'expandIntoTransition': 1}


def vf_full_controller(controllers=(), menus=(), prms=(), global_params=(), toggle_param=''):
    """a Full Controller (version 4): controllers [(asset, layer type)], menus [(asset, prefix)], prms [asset]"""
    return {'version': 4, 'controllers': [{'controller': c, 'type': t} for c, t in controllers],
            'menus': [{'menu': m, 'prefix': p} for m, p in menus], 'prms': [{'parameters': p} for p in prms],
            'smoothedPrms': [], 'globalParams': list(global_params), 'allNonsyncedAreGlobal': 0, 'ignoreSaved': 0,
            'toggleParam': toggle_param, 'rootObjOverride': R(0), 'rootBindingsApplyToAvatar': 0,
            'rewriteBindings': [], 'allowMissingAssets': 0, 'injectParams': [], 'injectSpsDepthParam': '',
            'injectSpsVelocityParam': '', 'controller': NO_ASSET, 'menu': NO_ASSET, 'parameters': NO_ASSET,
            'submenu': '', 'removePrefixes': [], 'addPrefix': '', 'useSecurityForToggle': 0}
