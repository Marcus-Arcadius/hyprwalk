# cs2mats.py OUTDIR: Cs2Mats.glb, a map for cs2mat_check.sh with what tools/cs2map.py writes for CS2's material details
# (HYPR3D_materials_source2's tintMask, decal, texture2, blendMode "add" and fog false; plain python3, built with
# tools/test/vm/litmap.py's pieces). A grey wall 4.5 m north of the eye, and in front of it, each 1.2 m wide, split down
# the middle by 2x1 textures drawn nearest:
#   A  a tint mask on the first uv set: the tint (red) only on its left half
#   B  a decal multiplied, on the second uv set, which runs the other way: its dark half on the right
#   C  a decal mixed in by its alpha, on the first uv set: blue on the left, nothing on the right
#   D  unlit, times a second color texture: green on the left
#   E  unlit and added to the wall (east of D), which it brightens
# and 60 m out, over the wall, two red unlit quads past the fog's end: F (east) with its fog off stays red, G (west) is
# all fog. A second row above the first:
#   H  vertex paint in the tint (csgo_complex's, "vertexColor": "tint"): blue, only where the tint mask is (its left half)
#   I  the same with all-0 paint, which is none
#   J  unlit mod2x in linear light (csgo_unlitgeneric's "mod2xLinear"): its left half, sRGB 188 (linear 0.5), leaves the
#      wall as it is; its right half, sRGB 128, darkens it
# and past the fog, in front of G, L: unlitgeneric's grey light added, which fades out in the fog. One uniform lightmap lights
# everything, and the sun is black, so the halves differ by their textures alone.
import os, sys
sys.dont_write_bytecode = True
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'vm'))
import litmap  # noqa: E402
from litmap import Lightmap, Map, png, rect, source2  # noqa: E402


def halves(left, right):
    """a 2x1 RGBA image"""
    return png(2, 1, 4, bytearray(list(left) + list(right)))


def cs2mats(path):
    g = Map()
    g.js['samplers'].append({'magFilter': 9728, 'minFilter': 9728, 'wrapS': 33071, 'wrapT': 33071})  # nearest
    NEAREST = len(g.js['samplers']) - 1
    img = {'white': g.image('white', png(4, 4, 4, bytearray([255] * 64))),
           'mask': g.image('tint_mask', halves((255, 255, 255, 255), (0, 0, 0, 255))),
           'dark': g.image('decal_dark', halves((60, 60, 60, 255), (255, 255, 255, 255))),
           'blue': g.image('decal_blue', halves((40, 60, 230, 255), (255, 255, 255, 0))),
           'green': g.image('color2_green', halves((40, 200, 60, 255), (255, 255, 255, 255))),
           'mod2x': g.image('mod2x_linear', halves((188, 188, 188, 255), (128, 128, 128, 255)))}
    tex = {k: g.texture(i, 0 if k == 'white' else NEAREST) for k, i in img.items()}

    lm = Lightmap(32, 32)
    flat = lm.chart(6, 6, 20, 20, lambda s, t: ((0.6, 0.6, 0.6), (0.5, 0.5, 1.0, 1.0), 0.0))

    def lit(**kw):
        return source2(specular=[False, False], vertexColor='none', **kw)

    def unlit(**kw):
        return {'KHR_materials_unlit': {}, 'HYPR3D_materials_source2': source2(specular=[False, False], **kw)}

    white = {'baseColorTexture': tex['white'], 'metallicFactor': 0.0, 'roughnessFactor': 1.0}
    wall = g.mat({'name': 'wall', 'pbrMetallicRoughness': dict(white, baseColorFactor=[0.5, 0.5, 0.5, 1.0]),
                  'extensions': {'HYPR3D_materials_source2': lit()}})
    mats = {
        'A': g.mat({'name': 'tinted', 'pbrMetallicRoughness': dict(white, baseColorFactor=[0.8, 0.08, 0.08, 1.0]),
                    'extensions': {'HYPR3D_materials_source2': lit(tintMask={'texture': tex['mask'], 'uv': 0})}}),
        'B': g.mat({'name': 'decal_multiplied', 'pbrMetallicRoughness': white,
                    'extensions': {'HYPR3D_materials_source2': lit(decal={'texture': tex['dark'], 'uv': 1, 'mode': 'multiply'})}}),
        'C': g.mat({'name': 'decal_mixed', 'pbrMetallicRoughness': white,
                    'extensions': {'HYPR3D_materials_source2': lit(decal={'texture': tex['blue'], 'uv': 0, 'mode': 'mix'})}}),
        'D': g.mat({'name': 'unlit_two', 'pbrMetallicRoughness': white,
                    'extensions': unlit(texture2={'texture': tex['green'], 'transform': [1.0, 0.0, 0.0, 1.0, 0.0, 0.0]})}),
        'E': g.mat({'name': 'unlit_added', 'alphaMode': 'BLEND', 'pbrMetallicRoughness': dict(white, baseColorFactor=[0.3, 0.3, 0.3, 1.0]),
                    'extensions': unlit(blendMode='add')}),
        'F': g.mat({'name': 'far_fog_off', 'pbrMetallicRoughness': dict(white, baseColorFactor=[0.9, 0.05, 0.05, 1.0]),
                    'extensions': unlit(fog=False)}),
        'G': g.mat({'name': 'far_fogged', 'pbrMetallicRoughness': dict(white, baseColorFactor=[0.9, 0.05, 0.05, 1.0]),
                    'extensions': unlit()}),
        'H': g.mat({'name': 'painted', 'pbrMetallicRoughness': white,
                    'extensions': {'HYPR3D_materials_source2': source2(specular=[False, False], vertexColor='tint',
                                                                       tintMask={'texture': tex['mask'], 'uv': 0})}}),
        'I': g.mat({'name': 'unpainted', 'pbrMetallicRoughness': white,
                    'extensions': {'HYPR3D_materials_source2': source2(specular=[False, False], vertexColor='tint')}}),
        'J': g.mat({'name': 'mod2x_linear', 'alphaMode': 'BLEND', 'pbrMetallicRoughness': dict(white, baseColorTexture=tex['mod2x']),
                    'extensions': unlit(blendMode='mod2x', mod2xLinear=True)}),
        'L': g.mat({'name': 'unlit_added_far', 'alphaMode': 'BLEND', 'pbrMetallicRoughness': dict(white, baseColorFactor=[0.5, 0.5, 0.5, 1.0]),
                    'extensions': unlit(blendMode='add')}),
    }

    def quad(x0, x1, y0, y1, z, mat, name, color=None):
        def attrs(s, t, p):
            # the second uv set runs the other way; every surface on the one flat chart
            return dict({'TEXCOORD_0': (s, 1.0 - t), 'TEXCOORD_1': (1.0 - s, 1.0 - t), '_LIGHTMAP_UV': flat(s, t)},
                        **({'COLOR_0': color} if color else {}))
        part = rect((x0, y0, z), (x1 - x0, 0.0, 0.0), (0.0, y1 - y0, 0.0), (0, 1), (0, 1), attrs)
        g.place(name, mesh=g.add_mesh(name, [g.prim(part, mat)]))

    quad(-6.0, 6.0, 0.0, 3.5, -4.5, wall, 'node000_wall')
    for k, (x0, x1) in zip('ABCD', ((-2.7, -1.5), (-1.3, -0.1), (0.1, 1.3), (1.5, 2.7))):
        quad(x0, x1, 0.9, 2.1, -4.4, mats[k], 'node001_panel_' + k)
    quad(3.0, 4.2, 0.9, 2.1, -4.4, mats['E'], 'node002_added')
    quad(5.0, 30.0, 10.0, 40.0, -60.0, mats['F'], 'node003_far_fog_off')
    quad(-30.0, -5.0, 10.0, 40.0, -60.0, mats['G'], 'node004_far_fogged')
    quad(-2.7, -1.5, 2.3, 3.3, -4.4, mats['H'], 'node005_panel_H', color=(0.15, 0.3, 1.0, 1.0))
    quad(-1.3, -0.1, 2.3, 3.3, -4.4, mats['I'], 'node005_panel_I', color=(0.0, 0.0, 0.0, 0.0))
    quad(0.1, 1.3, 2.3, 3.3, -4.4, mats['J'], 'node005_panel_J')
    quad(-25.0, -12.0, 28.0, 38.0, -55.0, mats['L'], 'node007_far_unlit_added')  # (in front of G, over the wall)
    g.place('hypr3d_spawn', translation=[0.0, 0.0, 0.0])

    ims = [g.image('map_' + k, d) for k, d in zip(('irradiance', 'directional', 'shadows'), lm.images())]
    g.js['extensions'] = {'HYPR3D_lighting': {
        'sets': [{'name': 'map', 'lightmaps': {k: {'image': i} for k, i in zip(('irradiance', 'directional', 'shadows'), ims)}}],
        'sun': {'color': [0.0, 0.0, 0.0], 'direction': [0.3, 0.8, 0.5]},
        'fog': {'start': 20.0, 'end': 40.0, 'exponent': 1.0, 'maxOpacity': 1.0, 'lodBias': 0.0},
        'exposure': {'min': 1.0, 'max': 1.0, 'speedUp': 1.0, 'speedDown': 1.0},
        'tonemap': {'shoulderStrength': 0.0, 'linearStrength': 0.0009, 'linearAngle': 0.0009, 'toeStrength': 1.0, 'toeNum': 1.0,
                    'toeDenom': 1.0, 'whitePoint': 1.648926, 'exposureBias': 0.0}}}
    g.js['extensionsUsed'] = ['HYPR3D_lighting', 'HYPR3D_materials_source2', 'KHR_materials_unlit']
    g.write(path)


if __name__ == '__main__':
    out = sys.argv[1] if len(sys.argv) > 1 else '.'
    os.makedirs(out, exist_ok=True)
    cs2mats(os.path.join(out, 'Cs2Mats.glb'))
    print('wrote', os.path.join(out, 'Cs2Mats.glb'))
