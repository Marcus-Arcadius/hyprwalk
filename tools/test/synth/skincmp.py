# skincmp.py A.glb [--pose NODE AXIS DEG]... B.glb [--pose ...]...: every mesh's vertices where the GLB puts them
# (skinned meshes through their joints), compared by mesh name; --pose turns a node of the file before it
import sys, os, json, struct
import numpy as np
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))  # tools/
import unity2hypr3d as u

argv = sys.argv[sys.argv.index('--') + 1:] if '--' in sys.argv else sys.argv[1:]
files, poses = [], []  # poses[k]: what --pose turns in file k (they follow the file)
i = 0
while i < len(argv):
    if argv[i] == '--pose':
        poses[-1].append((argv[i + 1], argv[i + 2], float(argv[i + 3])))
        i += 4
    else:
        files.append(argv[i])
        poses.append([])
        i += 1

CT = {5126: ('f', 4), 5123: ('H', 2), 5121: ('B', 1), 5125: ('I', 4)}
NC = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4, 'MAT4': 16}


def acc(js, binc, i):
    a = js['accessors'][i]
    bv = js['bufferViews'][a['bufferView']]
    fmt, size = CT[a['componentType']]
    n = NC[a['type']]
    stride = bv.get('byteStride', size * n)
    off = bv.get('byteOffset', 0) + a.get('byteOffset', 0)
    out = np.array([struct.unpack_from('<%d%s' % (n, fmt), binc, off + k * stride) for k in range(a['count'])], float)
    if a.get('normalized') and fmt in 'HB':
        out /= 65535.0 if fmt == 'H' else 255.0
    return out


def local(n):
    if 'matrix' in n:
        return np.array(n['matrix'], float).reshape(4, 4).T
    t = np.array(n.get('translation', [0, 0, 0]), float)
    x, y, z, w = n.get('rotation', [0, 0, 0, 1])
    s = np.array(n.get('scale', [1, 1, 1]), float)
    R = np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                  [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                  [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]])
    M = np.eye(4)
    M[:3, :3] = R * s
    M[:3, 3] = t
    return M


def rot(axis, deg):
    a = np.radians(deg)
    c, s = np.cos(a), np.sin(a)
    k = 'xyz'.index(axis)
    R = np.eye(4)
    i, j = [(1, 2), (2, 0), (0, 1)][k]
    R[i, i] = R[j, j] = c
    R[i, j], R[j, i] = -s, s
    return R


def verts(path, poses):
    js, binc = u.read_glb(path)
    nodes = js['nodes']
    parent = {}
    for i, n in enumerate(nodes):
        for c in n.get('children', []):
            parent[c] = i
    L = [local(n) for n in nodes]
    by = {n.get('name'): i for i, n in enumerate(nodes)}
    for nm, axis, deg in poses:
        if nm in by:
            L[by[nm]] = L[by[nm]] @ rot(axis, deg)
    W = [None] * len(nodes)

    def world(i):
        if W[i] is None:
            W[i] = L[i] if i not in parent else world(parent[i]) @ L[i]
        return W[i]
    out = {}
    for i, n in enumerate(nodes):
        if 'mesh' not in n:
            continue
        m = js['meshes'][n['mesh']]
        pts = []
        for p in m['primitives']:
            P = acc(js, binc, p['attributes']['POSITION'])
            P4 = np.c_[P, np.ones(len(P))]
            if 'skin' in n and 'JOINTS_0' in p['attributes']:
                sk = js['skins'][n['skin']]
                ibm = acc(js, binc, sk['inverseBindMatrices']).reshape(-1, 4, 4).transpose(0, 2, 1)
                Jm = np.array([world(j) @ ibm[k] for k, j in enumerate(sk['joints'])])
                J = acc(js, binc, p['attributes']['JOINTS_0']).astype(int)
                Wt = acc(js, binc, p['attributes']['WEIGHTS_0'])
                Wt = Wt / Wt.sum(1, keepdims=True)
                V = np.zeros((len(P), 4))
                for k in range(4):
                    V += Wt[:, k:k + 1] * np.einsum('nij,nj->ni', Jm[J[:, k]], P4)
                pts.append(V[:, :3])
            else:
                pts.append((world(i) @ P4.T).T[:, :3])
        out[m.get('name') or n.get('name')] = np.concatenate(pts)
    return out, {nm: world(i) for nm, i in by.items()}


A, WA = verts(files[0], poses[0])
B, WB = verts(files[1], poses[1])
for nm in sorted(set(A) | set(B)):
    if nm not in A or nm not in B:
        print('  %-10s only in %s' % (nm, 'A' if nm in A else 'B'))
        continue
    if A[nm].shape != B[nm].shape:
        print('  %-10s %d vs %d vertices' % (nm, len(A[nm]), len(B[nm])))
        continue
    d = np.linalg.norm(A[nm] - B[nm], axis=1)
    moved = (d > 1e-4).sum()
    print('  %-10s max %.6f m, %d of %d vertices moved%s' % (nm, d.max(), moved, len(d),
                                                            ', centre %s -> %s' % (np.round(A[nm].mean(0), 4), np.round(B[nm].mean(0), 4)) if moved else ''))
if len(files) > 2:
    for nm in files[2:]:
        pass
