import bpy, sys, numpy as np
a = sys.argv[sys.argv.index('--') + 1:]
exps = [float(x) for x in a[0].split(',')]
for f, e in zip(a[1:], exps):
    img = bpy.data.images.load(f); img.colorspace_settings.name = 'Non-Color'
    w, h = img.size; px = np.empty(w*h*4, np.float32); img.pixels.foreach_get(px); px = px.reshape(h, w, 4)[..., :3]
    lin = np.where(px <= 0.04045, px / 12.92, ((px + 0.055) / 1.055) ** 2.4)
    scene = lin * 1.5 / e
    lum = scene @ np.array([0.2125, 0.7154, 0.0721], np.float32)
    lum = np.maximum(lum, 0.005)
    geo = float(np.exp(np.log(lum).mean())); ari = float(lum.mean())
    print(f"{f.split('/')[-1]:14s} exp {e:.2f} scene log-avg {geo:.3f} mean {ari:.3f} -> CS2 exposure {min(max(0.27/geo, 0.8), 1.0):.2f} (unclamped {0.27/geo:.2f}); clipped {float((lin >= 0.999).any(-1).mean())*100:.1f}%")
