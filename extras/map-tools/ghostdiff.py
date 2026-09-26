import bpy, sys, numpy as np, glob, os
d = sys.argv[sys.argv.index('--') + 1]
def load(f):
    img = bpy.data.images.load(f); w, h = img.size; px = np.empty(w*h*4, np.float32); img.pixels.foreach_get(px); bpy.data.images.remove(img); return px.reshape(h, w, 4)[::-1, :, :3]
base = load(os.path.join(d, 'base.png'))
res = []
for f in glob.glob(os.path.join(d, '*.png')):
    if f.endswith('base.png'): continue
    x = load(f)
    diff = np.abs(x[100:330, 40:170] - base[100:330, 40:170]).mean()
    res.append((diff, os.path.basename(f)))
for r in sorted(res, reverse=True)[:5]: print('%.4f %s' % r)
