# cs2mat_unit.py: tools/cs2map.py's CS2 material details on a small hand-made glTF scene, as CS2's own shaders have
# them (decompiled from the game's csgo_vertexlitgeneric and csgo_unlitgeneric): the unlit shader's blend modes (4 adds:
# de_dust2's clouds), its second color texture and that texture's uv transform, the DynamicParams that tile and move the
# clouds (and g_vTexCoordScrollSpeed), g_bFogEnabled, F_NOTINT (no tint at all,
# though Source 2 Viewer bakes it into the base color), the tint mask and the decal texture with the uv set each is read
# with, and the lightmap's uv set coming after the ones a material reads itself (de_dust2's tower edges have no lightmap
# uvs: their second set is their decal's). The result is written as a GLB and read back.
#   python3 tools/test/synth/cs2mat_unit.py
import sys, os, json, math, struct, zlib, tempfile
sys.dont_write_bytecode = True  # (no __pycache__ left in tools/)
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))  # tools/
import cs2map

FAILS = []
TMP = tempfile.mkdtemp(prefix='cs2mat_unit')
EX = object.__new__(cs2map.Export)  # its material methods need no game
EX.work = TMP


def check(what, got, want):
    ok = got == want
    print('%s %s: %r%s' % ('ok  ' if ok else 'FAIL', what, got, '' if ok else ' (want %r)' % (want,)))
    if not ok:
        FAILS.append(what)


def near(what, got, want, tol=1e-6):
    ok = len(got) == len(want) and all(abs(g - w) <= tol for g, w in zip(got, want))
    print('%s %s: %s%s' % ('ok  ' if ok else 'FAIL', what, [round(g, 6) for g in got], '' if ok else ' (want %s)' % (want,)))
    if not ok:
        FAILS.append(what)


def png(path, rgba):
    """a 1x1 PNG"""
    def chunk(tag, data):
        return struct.pack('>I', len(data)) + tag + data + struct.pack('>I', zlib.crc32(tag + data) & 0xffffffff)
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 1, 1, 8, 6, 0, 0, 0)) +
                chunk(b'IDAT', zlib.compress(b'\0' + bytes(rgba))) + chunk(b'IEND', b''))
    return path


# the textures Source 2 Viewer would decompile: each vtex as its own 1x1 PNG
FILES = {}


VMATS = {  # decompiled .vmat files' DynamicParams, by material
    'materials/sky/clouds.vmat': {'g_vTexCoordScale': 'return float2(3.5,3.5);', 'g_vTexCoordOffset': 'return frac(float2(0,.002)*time());'},
    'materials/test/mist.vmat': {'g_vTexCoordOffset': 'return frac(float2(-.006,-0)*time());'},
    'materials/odd/pulse.vmat': {'g_flOpacityScale': 'return sin(time());'},
}


def fake_vrf_files(paths, outdir, logname):
    for p in paths:
        if p.endswith('.vmat_c'):
            text = '"Layer0"\n{\n\t"shader"\t"x.vfx"\n'
            if p[:-2] in VMATS:
                text += '\t"DynamicParams"\n\t{\n' + ''.join(f'\t\t"{k}"\t"{v}"\n' for k, v in VMATS[p[:-2]].items()) + '\t}\n'
            path = os.path.join(TMP, os.path.basename(p[:-2]))
            open(path, 'w').write(text + '}\n')
            FILES.setdefault(p, path)
        else:
            FILES.setdefault(p, png(os.path.join(TMP, os.path.basename(p[:-len('.vtex_c')]) + '.png'), (len(FILES), 0, 0, 255)))
    return {p: FILES[p] for p in paths}


EX.vrf_files = fake_vrf_files
EX.vrf_vmats = fake_vrf_files

doc = cs2map.Doc()
base_png = png(os.path.join(TMP, 'base.png'), (200, 200, 200, 255))
base_tex = doc.add_texture(doc.add_image(base_png))


def material(name, shader, ints=None, floats=None, vectors=None, textures=None, factor=None, vmat=None):
    m = {'name': name, 'pbrMetallicRoughness': {'baseColorTexture': {'index': base_tex}},
         'extras': {'vmat': {'Name': vmat or f'materials/test/{name.replace(" ", "_")}.vmat', 'ShaderName': shader, 'IntParams': ints or {},
                             'FloatParams': floats or {}, 'VectorParams': vectors or {}, 'TextureParams': textures or {}}}}
    if factor:
        m['pbrMetallicRoughness']['baseColorFactor'] = factor
    return doc.add('materials', m)


VLG, UNLIT, LMG = 'csgo_vertexlitgeneric.vfx', 'csgo_unlitgeneric.vfx', 'csgo_lightmappedgeneric.vfx'
M = {
    'clouds': material('clouds', UNLIT, {'F_BLEND_MODE': 4, 'F_TWOTEXTURE': 1, 'g_bFogEnabled': 0},
                       {'g_flTex2CoordRotation': 30}, {'g_vTex2CoordScale': [2, 3, 0, 0], 'g_vTex2CoordOffset': [0.1, 0.2, 0, 0],
                                                       'g_vTex2CoordCenter': [0.5, 0.25, 0, 0]},
                       {'g_tColor2': 'materials/sky/clouds_b.vtex'}, vmat='materials/sky/clouds.vmat'),
    'drifting': material('drifting', VLG, vectors={'g_vTexCoordScrollSpeed': [0.25, 0, 0, 0]}),
    'mist': material('mist', UNLIT, {'F_BLEND_MODE': 4}, vectors={'g_vTexCoordScrollSpeed': [-0.02, 0, 0, 0], 'g_vTexCoordScale': [3, 3, 0, 0],
                                                                  'g_vTexCoordOffset': [0.5, 0, 0, 0]}),
    'bags': material('bags', VLG, vectors={'g_vTexCoordScale': [3, 2, 0, 0], 'g_vTexCoordOffset': [0.25, 0, 0, 0]}),
    'layer scale': material('layer scale', LMG, vectors={'g_vTexCoordScale': [3, 3, 0, 0]}),
    'pulse': material('pulse', UNLIT, {'F_BLEND_MODE': 1}, vmat='materials/odd/pulse.vmat'),
    'cutout': material('cutout', UNLIT, {'F_BLEND_MODE': 2}, {'g_flAlphaTestReference': 0.3}),
    'stain': material('stain', UNLIT, {'F_BLEND_MODE': 3}),
    'multiplied': material('multiplied', UNLIT, {'F_BLEND_MODE': 5}),
    'frame': material('frame', VLG, {'F_NOTINT': 1}, factor=[0.5, 0.2, 0.1, 0.8]),
    'door': material('door', VLG, {'F_TINT_MASK': 1}, textures={'g_tTintMask': 'materials/door_tint.vtex'}, factor=[0.3, 0.6, 0.6, 1]),
    'door uv2': material('door uv2', VLG, {'F_TINT_MASK': 1, 'F_FORCE_UV2': 1, 'g_bUseSecondaryUvForTintMask': 1},
                         textures={'g_tTintMask': 'materials/door_tint.vtex'}),
    'crate': material('crate', VLG, {'F_TINT_MASK': 1, 'F_DECAL_TEXTURE': 1, 'F_DECAL_BLEND_MODE': 1, 'F_FORCE_UV2': 1,
                                     'g_bUseSecondaryUvForTintMask': 1},
                      textures={'g_tTintMask': 'materials/crate_tint.vtex', 'g_tDecal': 'materials/crate_decal.vtex'}),
    'poster': material('poster', VLG, {'F_DECAL_TEXTURE': 1, 'g_bUseSecondaryUvForDecal': 0},
                       textures={'g_tDecal': 'materials/poster_decal.vtex'}),
    'no mask': material('no mask', VLG, {'F_TINT_MASK': 1}, textures={'g_tTintMask': 'materials/default/default_mask.vtex'}),
    'layered': material('layered', LMG, {'F_LAYERS': 1, 'F_TINT_MASK': 1},
                        textures={'g_tLayer2Color': 'materials/ground_b.vtex', 'g_tTintMask': 'materials/ground_tint.vtex'}),
    'glow overlay': material('glow overlay', 'csgo_static_overlay.vfx', {'F_BLEND_MODE': 4}),
    'complex decal': material('complex decal', 'csgo_complex.vfx', {'F_DECAL_TEXTURE': 1}, textures={'g_tDecal': 'materials/complex_decal.vtex'}),
    'complex uv2': material('complex uv2', 'csgo_complex.vfx', {'F_SECONDARY_UV': 1, 'F_DETAIL_TEXTURE': 2, 'F_PAINT_VERTEX_COLORS': 1},
                           textures={'g_tDetail': 'materials/complex_detail.vtex'}),
    'complex uv2 first': material('complex uv2 first', 'csgo_complex.vfx', {'F_SECONDARY_UV': 1, 'F_DETAIL_TEXTURE': 2,
                                                                             'g_bUseSecondaryUvForDetailTexture': 0},
                                  textures={'g_tDetail': 'materials/complex_detail.vtex'}),
    'plaster': material('plaster', LMG, vectors={'g_vLayer1Tint': [0.98, 0.933, 0.882, 0]}, factor=[0.5, 1, 1, 1]),
    'plaster turned': material('plaster turned', LMG, {'F_TEXTURETRANSFORMS': 1, 'F_DETAILTEXTURE': 1}, {'g_flLayer1TexCoordRotation': 90},
                               {'g_vLayer1TexCoordScale': [2, 3, 0, 0], 'g_vLayer1TexCoordOffset': [0.1, 0.2, 0, 0],
                                'g_vLayer1DetailScale': [4, 4, 0, 0]}, {'g_tLayer1Detail': 'materials/plaster_detail.vtex'}),
    'glowing sign': material('glowing sign', 'generic.vfx', {'F_SELF_ILLUM': 1}, {'g_flSelfIllumBrightness': 2},
                             {'g_vSelfIllumTint': [0.5, 0.5, 0.5, 0]}),
}


def mesh(mat, uvs):
    """a quad with the given uv sets (lists of 4 (u, v)), TEXCOORD_0 up"""
    attrs = {'POSITION': doc.add_accessor('3f', [(0, 0, 0), (1, 0, 0), (1, 0, 1), (0, 0, 1)], 'VEC3', 5126, minmax=True)}
    for k, uv in enumerate(uvs):
        attrs[f'TEXCOORD_{k}'] = doc.add_accessor('2f', uv, 'VEC2', 5126, minmax=True)
    idx = doc.add_accessor('H', [(i,) for i in (0, 1, 2, 0, 2, 3)], 'SCALAR', 5123)
    m = doc.add('meshes', {'primitives': [{'attributes': attrs, 'indices': idx, 'material': mat}]})
    n = doc.add('nodes', {'name': f'n0_lr0_mesh{m}', 'mesh': m})
    doc.roots.append(n)
    return m


TILED = [(0, 0), (4, 0), (4, 2), (0, 2)]  # the material's own, repeating
INSIDE = [(0.1, 0.1), (0.3, 0.1), (0.3, 0.2), (0.1, 0.2)]  # inside 0..1: a lightmap's, or a second set's
plain = mesh(M['door'], [TILED, INSIDE])
own_two = mesh(M['door uv2'], [TILED, INSIDE])
own_two_lit = mesh(M['crate'], [TILED, INSIDE, [(0.5, 0.5), (0.6, 0.5), (0.6, 0.6), (0.5, 0.6)]])
decal_two = mesh(M['poster'], [TILED, INSIDE])
complex_decal = mesh(M['complex decal'], [TILED, INSIDE])
for name in ('clouds', 'cutout', 'stain', 'multiplied', 'frame', 'no mask', 'layered', 'drifting', 'pulse', 'glow overlay', 'complex uv2',
             'complex uv2 first', 'plaster', 'plaster turned', 'glowing sign', 'mist', 'bags', 'layer scale'):
    mesh(M[name], [TILED])

cs2map.WARNINGS.clear()
EX.fix_materials(doc)
EX.moving_textures(doc)
mats = doc.j['materials']
s2 = lambda k: mats[M[k]].get('extensions', {}).get(cs2map.EXT_S2, {})

# the unlit shader's blend modes
check('F_BLEND_MODE 4 (added) is blended', mats[M['clouds']].get('alphaMode'), 'BLEND')
check('and added', s2('clouds').get('blendMode'), 'add')
check('g_bFogEnabled 0: no fog', s2('clouds').get('fog'), False)
check('F_BLEND_MODE 2 is alpha tested', (mats[M['cutout']].get('alphaMode'), mats[M['cutout']].get('alphaCutoff')), ('MASK', 0.3))
check('F_BLEND_MODE 3 multiplies by twice its color', (mats[M['stain']].get('alphaMode'), s2('stain').get('blendMode')), ('BLEND', 'mod2x'))
check('its color linear (read as sRGB: neutral at linear 0.5)', s2('stain').get('mod2xLinear'), True)
check("csgo_static_overlay's F_BLEND_MODE 4 blends by alpha, as its 1 does (CS2's DstBlend for it is INV_SRC_ALPHA)",
      (mats[M['glow overlay']].get('alphaMode'), 'blendMode' in s2('glow overlay')), ('BLEND', False))
check('F_BLEND_MODE 5 (multiplied) stays opaque', mats[M['multiplied']].get('alphaMode', 'OPAQUE'), 'OPAQUE')
check('with a warning', any('multiply what is behind them' in w for w in cs2map.WARNINGS), True)
check('fog stays on by default', 'fog' in s2('door'), False)

# the second color texture and its uvs: CS2's g_vTex2CoordXform0/1 (scaled and turned about the center, then moved)
t2 = s2('clouds').get('texture2', {})
check('F_TWOTEXTURE: the second texture comes along', 'texture' in t2, True)
xf = t2.get('transform', [1, 0, 0, 1, 0, 0])
a, (sx, sy), (ox, oy), (cx, cy) = math.radians(30), (2, 3), (0.1, 0.2), (0.5, 0.25)
c, s = math.cos(a), math.sin(a)
x0 = (c * sx, -s * sy, cx + ox + s * sy * cy - c * sx * cx)  # g_vTex2CoordXform0's x, y, w
x1 = (s * sx, c * sy, cy + oy + s * sx * cx - c * sy * cy)
for u, v in ((0, 0), (1, 0), (0.3, 0.7)):
    near(f'its uv transform at ({u}, {v})', [xf[0] * u + xf[2] * v + xf[4], xf[1] * u + xf[3] * v + xf[5]],
         [x0[0] * u + x0[1] * v + x0[2], x1[0] * u + x1[1] * v + x1[2]])

# moving textures: the DynamicParams CS2 evaluates each frame (in the decompiled .vmat), and g_vTexCoordScrollSpeed
base_xf = lambda k: mats[M[k]]['pbrMetallicRoughness']['baseColorTexture'].get('extensions', {}).get('KHR_texture_transform')
check("DynamicParams: the clouds' g_vTexCoordScale, on the base color", base_xf('clouds'), {'scale': [3.5, 3.5]})
check('and their offset moving with time: a scroll', s2('clouds').get('scroll'), [0.0, 0.002])
check('KHR_texture_transform is used', 'KHR_texture_transform' in doc.j.get('extensionsUsed', []), True)
check('g_vTexCoordScrollSpeed scrolls too', s2('drifting').get('scroll'), [0.25, 0.0])
check("a static g_vTexCoordScale/Offset: the base color's transform", base_xf('bags'), {'scale': [3.0, 2.0], 'offset': [0.25, 0.0]})
check("with a moving offset: that in place of the static one, its scroll on top of g_vTexCoordScrollSpeed",
      (base_xf('mist'), [round(x, 6) for x in s2('mist').get('scroll', [])]), ({'scale': [3.0, 3.0]}, [-0.026, 0.0]))
check("csgo_lightmappedgeneric's color takes its first layer's transform, not g_vTexCoordScale", base_xf('layer scale'), None)
check('a dynamic parameter hypr3d does not do is left out', ('scroll' in s2('pulse'), base_xf('pulse')), (False, None))
check('with a warning', any("dynamic parameters aren't done" in w and 'g_flOpacityScale of pulse' in w for w in cs2map.WARNINGS), True)

# tints
check('F_NOTINT: no tint (the alpha stays)', mats[M['frame']]['pbrMetallicRoughness']['baseColorFactor'], [1.0, 1.0, 1.0, 0.8])
check('a tint mask keeps its tint as the base color', mats[M['door']]['pbrMetallicRoughness']['baseColorFactor'], [0.3, 0.6, 0.6, 1])
check('the tint mask, on the first uvs', s2('door').get('tintMask', {}).get('uv'), 0)
check('with F_FORCE_UV2 but no decal still the first (only decal variants read it with the second)', s2('door uv2').get('tintMask', {}).get('uv'), 0)
check('a decal variant reads it with the second when told to', s2('crate').get('tintMask', {}).get('uv'), 1)
check('its decal: the second uvs by default, multiplied (F_DECAL_BLEND_MODE 1)', {k: v for k, v in s2('crate').get('decal', {}).items() if k != 'texture'},
      {'uv': 1, 'mode': 'multiply'})
check('a decal told to use the first uvs, mixed in by its alpha', {k: v for k, v in s2('poster').get('decal', {}).items() if k != 'texture'},
      {'uv': 0, 'mode': 'mix'})
check('a default (white) tint mask is left out', 'tintMask' in s2('no mask'), False)
check('a layered material keeps its layer, no tint mask', ('tintMask' in s2('layered'), cs2map.EXT in mats[M['layered']].get('extensions', {})), (False, True))

# the lightmap's uv set comes after the material's own
EX.lightmap_uvs(doc)
attrs = lambda m: doc.j['meshes'][m]['primitives'][0]['attributes']
check('one set of its own: the second is the lightmap\'s', sorted(attrs(plain)), ['POSITION', 'TEXCOORD_0', '_LIGHTMAP_UV'])
check('two of its own and no third: no lightmap uvs (lit by the probes)', sorted(attrs(own_two)), ['POSITION', 'TEXCOORD_0', 'TEXCOORD_1'])
check('two of its own and a third: the third', sorted(attrs(own_two_lit)), ['POSITION', 'TEXCOORD_0', 'TEXCOORD_1', '_LIGHTMAP_UV'])
check('a decal texture makes two of its own', sorted(attrs(decal_two)), ['POSITION', 'TEXCOORD_0', 'TEXCOORD_1'])
check("csgo_complex's doesn't (its decal is on the first): the second is the lightmap's", sorted(attrs(complex_decal)),
      ['POSITION', 'TEXCOORD_0', '_LIGHTMAP_UV'])

# csgo_complex with a second uv set reads its detail texture with it (g_bUseSecondaryUvForDetailTexture, 1 by default)
check('csgo_complex F_SECONDARY_UV: the detail texture on the second uvs', s2('complex uv2').get('detail', {}).get('uv'), 1)
check('unless told not to', 'uv' in s2('complex uv2 first').get('detail', {}), False)
check("csgo_complex's vertex paint is part of its tint", s2('complex uv2').get('vertexColor'), 'tint')
check("generic's self-illumination: its tint as it is, no 2^brightness, the albedo in full",
      (mats[M['glowing sign']].get('emissiveFactor'), mats[M['glowing sign']]['extensions']['KHR_materials_emissive_strength']['emissiveStrength'],
       s2('glowing sign').get('selfIllumAlbedo')), ([0.5, 0.5, 0.5], 1.0, 1.0))

# one-layer csgo_lightmappedgeneric: its color still gets the first layer's tint and uv transform
lin = cs2map.srgb_to_linear
near("one layer: g_vLayer1Tint (linear) on the base color", mats[M['plaster']]['pbrMetallicRoughness']['baseColorFactor'],
     [0.5 * lin(0.98), lin(0.933), lin(0.882), 1])
one = cs2map.Export.uv_transform(mats[M['plaster turned']]['extras']['vmat'], 'Layer1TexCoord')
kt = base_xf('plaster turned') or {}
r, (sx, sy), (ox, oy) = kt.get('rotation', 0), kt.get('scale', [1, 1]), kt.get('offset', [0, 0])
c, s = math.cos(r), math.sin(r)
for u, v in ((0, 0), (1, 0), (0.3, 0.7)):
    near(f'F_TEXTURETRANSFORMS, turned 90 degrees: KHR_texture_transform (T * R * S) at ({u}, {v}) as CS2 has it',
         [c * sx * u + s * sy * v + ox, -s * sx * u + c * sy * v + oy], [one[0] * u + one[2] * v + one[4], one[1] * u + one[3] * v + one[5]])
near('its detail texture at its scale of the transformed uvs', s2('plaster turned').get('detail', {}).get('transform', []),
     cs2map.compose([4, 0, 0, 4, 0, 0], one))

# draw calls' tints: linear as the compiler stores them, linearized again by Source 2 Viewer's glTF export
MDAT = '''--- Files in package:
[1/2] maps/x/worldnodes/n0_lr0_c0_s_cb_nomerge1.vmdl_c
\t\t\t\t\tm_vTintColor = [ 0.982826, 0.932277, 0.7593 ]
\t\t\t\t\tm_flAlpha = 1.0
\t\t\t\t\tm_material = resource:"materials/sky/clouds.vmat"
[2/2] maps/prefabs/x_skybox/worldnodes/node000_world_lr0_agg3_2_windows.vmdl_c
\t\t\t\t\tm_vTintColor = [ 0.84337, 0.835528, 0.812242 ]
\t\t\t\t\tm_material = resource:"materials/test/windows.vmat"
\t\t\t\t\tm_vTintColor = [ 0.45908, 0.598942, 0.541798 ]
\t\t\t\t\tm_material = resource:"materials/test/windows.vmat"
\t\t\t\t\tm_material = resource:"materials/test/windows.vmat"
'''
calls = cs2map.parse_draw_calls(MDAT)
check('draw calls in order, with their tints (white without one)', calls,
      {'n0_lr0_c0_s_cb_nomerge1': [('materials/sky/clouds.vmat', (0.982826, 0.932277, 0.7593))],
       'node000_world_lr0_agg3_2_windows': [('materials/test/windows.vmat', (0.84337, 0.835528, 0.812242)),
                                            ('materials/test/windows.vmat', (0.45908, 0.598942, 0.541798)),
                                            ('materials/test/windows.vmat', (1.0, 1.0, 1.0))]})
NODES = '''\t\t{
\t\t\tm_aggregateMeshes =
\t\t\t[
\t\t\t\t{
\t\t\t\t\tm_nDrawCallIndex = 1
\t\t\t\t\tm_vTintColor = [ 255, 102, 255 ]
\t\t\t\t},
\t\t\t\t{
\t\t\t\t\tm_nDrawCallIndex = 0
\t\t\t\t\tm_vTintColor = [ 255, 255, 255 ]
\t\t\t\t},
\t\t\t\t{
\t\t\t\t\tm_nDrawCallIndex = 2
\t\t\t\t},
\t\t\t]
\t\t\tm_renderableModel = resource:"maps/prefabs/x_skybox/worldnodes/node000_world_lr0_agg3_2_windows.vmdl"
\t\t},
\t\t{
\t\t\tm_vTintColor = [ 1.0, 1.0, 1.0, 1.0 ]
\t\t\tm_renderableModel = resource:"maps/x/worldnodes/n0_lr0_c0_s_cb_nomerge1.vmdl"
\t\t},
'''
frags = cs2map.parse_fragments(NODES)
check("an aggregate's fragments: draw call and tint (0-1 gamma), in order", frags,
      {'node000_world_lr0_agg3_2_windows': [(1, (1.0, 0.4, 1.0)), (0, (1.0, 1.0, 1.0)), (2, None)]})

l2s = cs2map.linear_to_srgb
vrf = lambda inst, draw, ct: [lin(min(i * d * t, 1.0)) for i, d, t in zip(inst, draw, ct)] + [1.0]  # Source 2 Viewer's
ct, dT = [0.56, 0.58, 0.62], (0.982826, 0.932277, 0.7593)
near("the clouds: the draw call's tint as it is, times g_vColorTint linearized", cs2map.tinted_base_color(vrf((1, 1, 1), dT, ct), dT, None, 1.0, ct),
     [d * lin(t) for d, t in zip(dT, ct)])
inst = (1.0, 0.4, 1.0)
near("a fragment's own tint (gamma) with the draw call's put back to gamma",
     cs2map.tinted_base_color(vrf(inst, dT, ct), dT, inst, 1.0, ct), [lin(i * l2s(d)) * lin(t) for i, d, t in zip(inst, dT, ct)])
check("left alone when Source 2 Viewer's color isn't its formula's (a fixed Source 2 Viewer)",
      cs2map.tinted_base_color([d * lin(t) for d, t in zip(dT, ct)] + [1.0], dT, (1.0, 1.0, 1.0), 1.0, ct), None)

tdoc = cs2map.Doc()
tmat = lambda name, vmat, factor, vectors=None, ints=None: tdoc.add('materials', {
    'name': name, 'pbrMetallicRoughness': {'baseColorFactor': factor},
    'extras': {'vmat': {'Name': vmat, 'ShaderName': UNLIT, 'IntParams': ints or {}, 'FloatParams': {}, 'VectorParams': vectors or {}, 'TextureParams': {}}}})
clouds = tmat('clouds', 'materials/sky/clouds.vmat', vrf((1, 1, 1), dT, ct), {'g_vColorTint': ct + [0]})
wA, wB = (0.84337, 0.835528, 0.812242), (0.45908, 0.598942, 0.541798)
win_a = tmat('windows', 'materials/test/windows.vmat', vrf((1, 1, 1), wA, (1, 1, 1)))
win_b = tmat('windows', 'materials/test/windows.vmat', vrf(inst, wB, (1, 1, 1)))
win_c = tmat('windows', 'materials/test/windows.vmat', [1.0, 1.0, 1.0, 1.0])
bars = tmat('bars', 'materials/sky/clouds.vmat', vrf((1, 1, 1), dT, (1, 1, 1)), ints={'F_NOTINT': 1})


def tnode(name, mesh_name, mat, parent=None):
    me = tdoc.add('meshes', {'name': mesh_name, 'primitives': [{'attributes': {}, 'material': mat}]})
    n = tdoc.add('nodes', {'name': name, 'mesh': me})
    if parent is None:
        tdoc.roots.append(n)
    else:
        tdoc.j['nodes'][parent].setdefault('children', []).append(n)
    return me


m_clouds = tnode('n0_lr0_c0_s_cb_nomerge1.meshset_0', 'n0_lr0_c0_s_cb_nomerge1.meshset_0', clouds)
m_other = tnode('some_prop.meshset_0', 'some_prop.meshset_0', clouds)  # the same material where no draw call is tinted
backdrop = tdoc.add('nodes', {'name': 'hypr3d_backdrop'})
tdoc.roots.append(backdrop)
agg = 'node000_world_lr0_agg3_2_windows'
f1, f2, f3 = (tnode(agg, f'{agg}_fragment{k}', m, backdrop) for k, m in ((1, win_b), (2, win_a), (3, win_c)))
n_changed, n_unsure = cs2map.Export.fix_tints(tdoc, [(backdrop, {agg: calls[agg]}, frags), (None, {'n0_lr0_c0_s_cb_nomerge1': calls['n0_lr0_c0_s_cb_nomerge1']}, {})])
tm = tdoc.j['materials']
prim_mat = lambda me: tm[tdoc.j['meshes'][me]['primitives'][0]['material']]
near("fix_tints: the clouds get CS2's color", prim_mat(m_clouds)['pbrMetallicRoughness']['baseColorFactor'], [d * lin(t) for d, t in zip(dT, ct)] + [1.0])
check('in a copy of the material, the other mesh keeping the original', (prim_mat(m_other) is tm[clouds], prim_mat(m_clouds) is tm[clouds]), (True, False))
near("an aggregate's fragment by its draw call, with its own tint", prim_mat(f1)['pbrMetallicRoughness']['baseColorFactor'],
     [lin(i * l2s(d)) for i, d in zip(inst, wB)] + [1.0])
near('and one with a white tint', prim_mat(f2)['pbrMetallicRoughness']['baseColorFactor'], list(wA) + [1.0])
check('a draw call without a tint is left as it is', prim_mat(f3)['pbrMetallicRoughness']['baseColorFactor'], [1.0, 1.0, 1.0, 1.0])
check('how many changed', (n_changed, n_unsure), (3, 0))

# written as a GLB and read back: the new textures are kept and renumbered with the others
for m in doc.list('materials'):
    m.pop('extras', None)
out = os.path.join(TMP, 'materials.glb')
doc.write_glb(out)
data = open(out, 'rb').read()
n = struct.unpack('<I', data[12:16])[0]
gj = json.loads(data[20:20 + n])


def image_of(mat, key):
    ext = next(m for m in gj['materials'] if m['name'] == mat)['extensions'][cs2map.EXT_S2][key]
    return gj['images'][gj['textures'][ext['texture']['index']]['source']]['name']


check("the GLB: the door's tint mask", image_of('door', 'tintMask'), 'door_tint')
check("the crate's decal", image_of('crate', 'decal'), 'crate_decal')
check("the clouds' second texture", image_of('clouds', 'texture2'), 'clouds_b')

print('all ok' if not FAILS else f'{len(FAILS)} FAILED: {", ".join(FAILS)}')
sys.exit(1 if FAILS else 0)
