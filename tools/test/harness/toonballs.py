# toonballs.py OUTDIR: ToonBalls.glb for toon_check.sh: six 0.3 m balls in a row along +X, no skeleton: Plain, MToon
# (VRMC_materials_mtoon), Toon (unity2hypr3d's hypr3d_toon extras), Matcap (Toon plus an added hypr3d_matcap),
# PlainMatcap and MToon0 (VRM 0.x MToon). The base color is dark enough for the tone curve.
import math, os, struct, sys, zlib

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'vm'))
from assets import GLB, box  # noqa: E402

BASE = (0.25, 0.16, 0.1, 1.0)
X = (-1.0, -0.6, -0.2, 0.2, 0.6, 1.0)  # centred on the avatar's middle, as the camera is


def sphere(cx, cy, cz, r, rings=48, segs=64):
    """positions, normals, uvs, indices: a UV sphere, counter-clockwise from outside"""
    pos, nrm, uv, idx = [], [], [], []
    for i in range(rings + 1):
        th = math.pi * i / rings
        for j in range(segs + 1):
            ph = 2 * math.pi * j / segs
            n = (math.sin(th) * math.cos(ph), math.cos(th), -math.sin(th) * math.sin(ph))
            pos.append((cx + r * n[0], cy + r * n[1], cz + r * n[2]))
            nrm.append(n)
            uv.append((j / segs, i / rings))
    for i in range(rings):
        for j in range(segs):
            a, b = i * (segs + 1) + j, (i + 1) * (segs + 1) + j
            idx += [a, b, a + 1, a + 1, b, b + 1]
    return pos, nrm, uv, idx


def png(w, h, px):
    """an RGB PNG from px(x, y) -> (r, g, b) bytes"""
    rows = b''.join(b'\0' + bytes(c for x in range(w) for c in px(x, y)) for y in range(h))
    chunk = lambda t, d: struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    return b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) + \
        chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b'')


def matcap_png(size=64):
    """matcap texture: white where the normal faces the eye, black at the rim, v down"""
    def px(x, y):
        u, v = (x + 0.5) / size * 2 - 1, (y + 0.5) / size * 2 - 1
        k = max(0.0, 1.0 - math.sqrt(u * u + v * v))
        return (round(255 * k),) * 3
    return png(size, size, px)


def image(g, data):
    g.js.setdefault('images', []).append({'bufferView': g.view(data, None), 'mimeType': 'image/png'})
    g.js['bufferViews'][-1].pop('target')
    g.js.setdefault('samplers', [{'magFilter': 9729, 'minFilter': 9729, 'wrapS': 33071, 'wrapT': 33071}])
    g.js.setdefault('textures', []).append({'source': len(g.js['images']) - 1, 'sampler': 0})
    return len(g.js['textures']) - 1


def main(out):
    g = GLB()
    g.js['asset']['generator'] = 'hypr3d tools/test/harness/toonballs.py'
    cap = image(g, matcap_png())
    step = {'shade': [0.3, 0.3, 0.3], 'base': True, 'lo': -0.05, 'hi': 0.05, 'strength': 1.0}
    mats = [g.material('Plain', BASE)]
    mats.append(g.material('MToon', BASE))
    g.js['materials'][-1]['extensions'] = {'VRMC_materials_mtoon': {
        'specVersion': '1.0', 'shadeColorFactor': [0.1, 0.03, 0.01], 'shadingToonyFactor': 0.95, 'shadingShiftFactor': 0.0}}
    g.js['extensionsUsed'] = ['VRMC_materials_mtoon']
    mats.append(g.material('Toon', BASE, extras={'hypr3d_toon': step}))
    mats.append(g.material('Matcap', BASE, extras={
        'hypr3d_toon': step, 'hypr3d_matcap': {'index': cap, 'color': [1, 1, 1, 1], 'mode': 'add', 'lit': 0}}))
    mats.append(g.material('PlainMatcap', BASE, extras={
        'hypr3d_matcap': {'index': cap, 'color': [1, 1, 1, 1], 'mode': 'add', 'lit': 0}}))
    mats.append(g.material('MToon0', BASE))
    props = [{'name': m['name'], 'shader': 'VRM_USE_GLTFSHADER', 'floatProperties': {}, 'vectorProperties': {},
              'textureProperties': {}} for m in g.js['materials']]
    props[-1].update(shader='VRM/MToon', floatProperties={'_ShadeToony': 0.95, '_ShadeShift': 0.0},
                     vectorProperties={'_ShadeColor': [0.1, 0.4, 0.1, 1.0], '_Color': list(BASE)})
    g.js['extensions'] = {'VRM': {'specVersion': '0.0', 'materialProperties': props}}
    g.js['extensionsUsed'].append('VRM')
    for name, x, m in zip(('Plain', 'MToon', 'Toon', 'Matcap', 'PlainMatcap', 'MToon0'), X, mats):
        pos, nrm, uv, idx = sphere(x, 1.2, 0.0, 0.15)
        prim = {'attributes': {'POSITION': g.accessor(pos, 'VEC3'), 'NORMAL': g.accessor(nrm, 'VEC3'),
                               'TEXCOORD_0': g.accessor(uv, 'VEC2')}, 'indices': g.accessor(idx, 'SCALAR'), 'material': m}
        g.js['meshes'].append({'name': name, 'primitives': [prim]})
        g.node(name, mesh=len(g.js['meshes']) - 1)
    # a stand under them, so the avatar is as tall as a person's head
    g.mesh('Stand', [(mats[0], box(-0.25, 0.0, -0.05, -0.15, 1.0, 0.05))])
    os.makedirs(out, exist_ok=True)
    g.write(os.path.join(out, 'ToonBalls.glb'))
    print('wrote', os.path.join(out, 'ToonBalls.glb'))


if __name__ == '__main__':
    main(sys.argv[1] if len(sys.argv) > 1 else '.')
