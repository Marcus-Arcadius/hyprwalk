# check.py OUT.glb: what the converter wrote, next to what the synthetic avatar should give
import sys, os, json, struct, math
import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))  # tools/
import unity2hypr3d as u

glb = (sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else sys.argv[1:])[0]
js, binc = u.read_glb(glb)
st = json.load(open(glb[:-4] + '.hypr3d.json'))
nodes = js['nodes']
parent = {}
for i, n in enumerate(nodes):
    for c in n.get('children', []):
        parent[c] = i


def local(n):
    t = np.array(n.get('translation', [0, 0, 0]), float)
    x, y, z, w = n.get('rotation', [0, 0, 0, 1])
    s = np.array(n.get('scale', [1, 1, 1]), float)
    R = np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                  [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                  [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]])
    M = np.eye(4)
    if 'matrix' in n:
        return np.array(n['matrix'], float).reshape(4, 4).T
    M[:3, :3] = R * s
    M[:3, 3] = t
    return M


def world(i):
    M = local(nodes[i])
    while i in parent:
        i = parent[i]
        M = local(nodes[i]) @ M
    return M


by = {}
for i, n in enumerate(nodes):
    by.setdefault(n.get('name', ''), i)


def tree(i, d=0):
    n = nodes[i]
    extra = []
    if 'mesh' in n:
        extra.append('mesh %s' % js['meshes'][n['mesh']].get('name'))
    if 'skin' in n:
        extra.append('skin')
    s = n.get('scale')
    if s and max(abs(x - 1) for x in s) > 1e-4:
        extra.append('scale %s' % [round(x, 4) for x in s])
    print('  ' * d + n.get('name', '?'), ('(' + ', '.join(extra) + ')') if extra else '')
    for c in n.get('children', []):
        tree(c, d + 1)


print('== nodes')
for r in js['scenes'][js.get('scene', 0)]['nodes']:
    tree(r)

print('== world positions (glTF: +Y up, the avatar faces +Z)')
for nm in ('Hips', 'Head', 'Eye_L', 'Wrist_L', 'Thigh_L', 'Hair_1', 'Skirt', 'Hat', 'Badge'):
    if nm in by:
        M = world(by[nm])
        print('  %-8s %s  axis-Y %s' % (nm, np.round(M[:3, 3], 4), np.round(M[:3, 1] / np.linalg.norm(M[:3, 1]), 3)))

print('== colliders, world')
for c in st.get('colliders', []):
    M = world(by[c['node']])
    p = M @ np.array(list(c['offset']) + [1.0])
    s = '  %-12s at %s r %.3f' % (c['name'], np.round(p[:3], 4), c['radius'] * np.linalg.norm(M[:3, 0]))
    if 'tail' in c:
        q = M @ np.array(list(c['tail']) + [1.0])
        s += ' to %s' % np.round(q[:3], 4)
    print(s)

print('== meshes')
for m in js['meshes']:
    names = m.get('extras', {}).get('targetNames', [])
    w = m.get('weights', [])
    nz = {names[i] if i < len(names) else i: round(x, 3) for i, x in enumerate(w) if abs(x) > 1e-6}
    mats = [js['materials'][p['material']]['name'] if 'material' in p else None for p in m['primitives']]
    print('  %-12s %d prims %s, %d shapes, weights %s' % (m.get('name'), len(m['primitives']), mats, len(names), nz))

print('== materials')
for mt in js['materials']:
    pbr = mt.get('pbrMetallicRoughness', {})
    d = {k: mt[k] for k in ('alphaMode', 'alphaCutoff', 'doubleSided', 'emissiveFactor') if k in mt}
    d['color'] = [round(x, 3) for x in pbr.get('baseColorFactor', [1, 1, 1, 1])]
    t = pbr.get('baseColorTexture')
    if t:
        d['tex'] = js['images'][js['textures'][t['index']]['source']].get('name')
        if 'extensions' in t:
            d['xf'] = t['extensions']
    if 'emissiveTexture' in mt:
        d['emitTex'] = js['images'][js['textures'][mt['emissiveTexture']['index']]['source']].get('name')
    if 'extensions' in mt:
        d['ext'] = mt['extensions']
    print('  %-10s %s' % (mt.get('name'), json.dumps(d)))

print('== images')
for im in js.get('images', []):
    bv = js['bufferViews'][im['bufferView']]
    data = binc[bv.get('byteOffset', 0): bv.get('byteOffset', 0) + bv['byteLength']]
    if data[:8] == b'\x89PNG\r\n\x1a\n':
        w, h = struct.unpack('>II', data[16:24])
        print('  %-10s png %dx%d' % (im.get('name'), w, h))
    else:
        print('  %-10s %s %d bytes' % (im.get('name'), im.get('mimeType'), len(data)))
print('== extensionsUsed', js.get('extensionsUsed'))
