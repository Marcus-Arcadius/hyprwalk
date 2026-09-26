import bpy, sys, numpy as np, re
a = sys.argv[sys.argv.index("--") + 1:]
raw = open(a[0], 'rb').read()
print('raw bytes', len(raw))
txt = open(a[1]).read()
m = re.search(r'm_lut\s*=\s*\[(.*?)\]', txt, re.S)
vals = np.array([float(x) for x in re.findall(r'-?[\d.]+(?:e-?\d+)?', m.group(1))], dtype=np.float32)
print('lut floats', vals.size, 'cube', round((vals.size / 4) ** (1/3), 3), round((vals.size / 3) ** (1/3), 3))
n = 32
lut = vals.reshape(n, n, n, 3)
ident = np.stack(np.meshgrid(np.arange(n), np.arange(n), np.arange(n), indexing='ij'), -1).astype(np.float32) / (n - 1)
# try orders
for name, idn in [('b,g,r', ident[..., ::-1]), ('r,g,b', ident)]:
    d = np.abs(lut[..., :3] - idn)
    print(name, 'mean abs diff from identity', d.mean(), 'max', d.max())

for idx in [(0,0,0),(n-1,n-1,n-1),(n//2,n//2,n//2),(0,0,n-1),(0,n-1,0),(n-1,0,0),(8,16,24)]:
    print(idx, lut[idx])
rawlut = np.frombuffer(raw, dtype=np.uint8).reshape(n, n, n, 3).astype(np.float32) / 255
print('raw vs text max diff', np.abs(rawlut - lut).max())
# how it maps greys and some colours (index [b][g][r] if the first diff line wins)
for v in [0.1, 0.25, 0.5, 0.75, 0.9]:
    i = round(v * (n - 1))
    print('grey', v, '->', lut[i, i, i])
