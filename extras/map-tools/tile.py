# blender -b -P tile.py -- out.png cols tileW tileH in1.png in2.png ...  (row-major, top-left first)
import bpy, sys, numpy as np
a = sys.argv[sys.argv.index("--") + 1:]
out, cols, tw, th, files = a[0], int(a[1]), int(a[2]), int(a[3]), a[4:]
rows = (len(files) + cols - 1) // cols
sheet = np.zeros((rows * th, cols * tw, 4), dtype=np.float32); sheet[..., 3] = 1
for k, f in enumerate(files):
    img = bpy.data.images.load(f); w, h = img.size
    px = np.empty(w * h * 4, dtype=np.float32); img.pixels.foreach_get(px); px = px.reshape(h, w, 4)
    ys = np.arange(th) * h // th; xs = np.arange(tw) * w // tw
    t = px[ys][:, xs]
    r, c = k // cols, k % cols
    y0 = (rows - 1 - r) * th  # blender rows go bottom-up
    sheet[y0:y0 + th, c * tw:(c + 1) * tw] = t
o = bpy.data.images.new("o", cols * tw, rows * th); o.pixels.foreach_set(sheet.ravel())
o.filepath_raw = out; o.file_format = "PNG"; o.save()
