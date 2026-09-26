# usage: blender -b --factory-startup -P thumb.py -- out.png size in1.png in2.png ...  (contact sheet: rgb on top, alpha below)
import bpy, sys, numpy as np
argv = sys.argv[sys.argv.index("--") + 1:]
out, size, files = argv[0], int(argv[1]), argv[2:]
tiles = []
for f in files:
    img = bpy.data.images.load(f)
    w, h = img.size
    px = np.array(img.pixels[:], dtype=np.float32).reshape(h, w, 4)
    ys = (np.arange(size) * h // size); xs = (np.arange(size) * w // size)
    t = px[ys][:, xs]
    rgb = t.copy(); rgb[..., 3] = 1
    a = np.repeat(t[..., 3:4], 4, axis=2); a[..., 3] = 1
    tiles.append(np.concatenate([a, rgb], axis=0))  # blender rows go bottom-up: rgb ends on top
    print(f.split("/")[-1], w, h, "alpha min/max %.2f %.2f" % (px[..., 3].min(), px[..., 3].max()), "mean rgb", px[..., :3].reshape(-1, 3).mean(0).round(3))
sheet = np.concatenate(tiles, axis=1)
H, W = sheet.shape[:2]
o = bpy.data.images.new("o", W, H, alpha=True)
o.pixels = sheet.ravel()
o.filepath_raw = out; o.file_format = "PNG"; o.save()
