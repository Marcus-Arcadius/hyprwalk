# findmat.py GLB x0 y0 z0 x1 y1 z1 [alpha]: primitives whose world bounds meet the box (glTF metres)
import json, struct, sys
d = open(sys.argv[1], 'rb').read(64 << 20)
n = struct.unpack('<I', d[12:16])[0]
j = json.loads(d[20:20 + n])
box = [float(x) for x in sys.argv[2:8]]
only = sys.argv[8] if len(sys.argv) > 8 else None
mats = j['materials']; meshes = j['meshes']; nodes = j['nodes']; acc = j['accessors']
def mul(a, b):
    return [sum(a[r + 4 * k] * b[k + 4 * c] for k in range(4)) for c in range(4) for r in range(4)]
def local(nd):
    if 'matrix' in nd: return nd['matrix']
    t = nd.get('translation', [0, 0, 0]); x, y, z, w = nd.get('rotation', [0, 0, 0, 1]); s = nd.get('scale', [1, 1, 1])
    R = [1-2*(y*y+z*z), 2*(x*y+z*w), 2*(x*z-y*w), 0, 2*(x*y-z*w), 1-2*(x*x+z*z), 2*(y*z+x*w), 0, 2*(x*z+y*w), 2*(y*z-x*w), 1-2*(x*x+y*y), 0, 0, 0, 0, 1]
    return mul([1,0,0,0, 0,1,0,0, 0,0,1,0, t[0],t[1],t[2],1], mul(R, [s[0],0,0,0, 0,s[1],0,0, 0,0,s[2],0, 0,0,0,1]))
stack = [(r, [1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1]) for r in j['scenes'][0]['nodes']]
seen = set()
while stack:
    ni, parent = stack.pop()
    nd = nodes[ni]; w = mul(parent, local(nd))
    stack += [(c, w) for c in nd.get('children', [])]
    if 'mesh' not in nd: continue
    for p in meshes[nd['mesh']]['primitives']:
        m = mats[p['material']] if 'material' in p else {'name': '?'}
        if only and m.get('alphaMode', 'OPAQUE') != only: continue
        a = acc[p['attributes']['POSITION']]
        lo, hi = [1e9]*3, [-1e9]*3
        for c in range(8):
            q = [a['max'][k] if (c >> k) & 1 else a['min'][k] for k in range(3)]
            wq = [sum(w[r + 4 * k] * q[k] for k in range(3)) + w[12 + r] for r in range(3)]
            lo = [min(lo[k], wq[k]) for k in range(3)]; hi = [max(hi[k], wq[k]) for k in range(3)]
        if all(lo[k] <= box[3 + k] and hi[k] >= box[k] for k in range(3)):
            key = (m['name'], nd.get('name', '')[:50])
            if key in seen: continue
            seen.add(key)
            print(m['name'], m.get('alphaMode', 'OPAQUE'), 'ext:', list(m.get('extensions', {}).keys()), nd.get('name', '')[:50], [round(x, 1) for x in lo], [round(x, 1) for x in hi])
