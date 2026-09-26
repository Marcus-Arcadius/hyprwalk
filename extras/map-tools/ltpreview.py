import bpy, sys, numpy as np
a = sys.argv[sys.argv.index('--') + 1:]
out, files = a[0], a[1:]
tiles = []
for f in files:
    img = bpy.data.images.load(f); img.colorspace_settings.name = 'Non-Color'
    w, h = img.size; px = np.empty(w*h*4, np.float32); img.pixels.foreach_get(px); px = px.reshape(h, w, 4)
    b = np.round(px * 255)
    if 'irradiance' in f or 'probes.png' in f:
        e = b[..., 3]; v = (b[..., :3] + 0.5) * np.ldexp(1.0, (e - 136).astype(np.int32))[..., None]; v[e == 0] = 0
        v = 1 - np.exp(-v * 1.5)
        rgb = np.where(v <= 0.0031308, v * 12.92, 1.055 * np.power(np.maximum(v, 1e-9), 1/2.4) - 0.055)
    else:
        rgb = np.repeat(px[..., :1], 3, -1)
    s = 768; ys = np.arange(s) * h // s; xs = np.arange(s) * w // s
    t = np.ones((s, s, 4), np.float32); t[..., :3] = rgb[ys][:, xs]
    tiles.append(t)
sheet = np.concatenate(tiles, 1)
o = bpy.data.images.new('o', sheet.shape[1], sheet.shape[0]); o.pixels = sheet.ravel(); o.filepath_raw = out; o.file_format = 'PNG'; o.save()
