import bpy, sys, numpy as np
argv = sys.argv[sys.argv.index("--") + 1:]
img = bpy.data.images.load(argv[0])
w, h = img.size
print("size", w, h, "channels", img.channels, "depth", img.depth, "float", img.is_float, "colorspace", img.colorspace_settings.name)
px = np.array(img.pixels[:], dtype=np.float32).reshape(h, w, 4)
lum = px[..., :3] @ np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)
print("lum min/mean/max", lum.min(), lum.mean(), lum.max(), "p99", np.percentile(lum, 99), "p50", np.percentile(lum, 50))
y, x = np.unravel_index(np.argmax(lum), lum.shape)
print("brightest at x=%d y=%d (row from bottom), value" % (x, y), px[y, x])
# tone-mapped preview
out = px.copy()
out[..., :3] = 1 - np.exp(-out[..., :3] * float(argv[2]))
out[..., :3] = np.where(out[..., :3] <= 0.0031308, out[..., :3] * 12.92, 1.055 * np.power(np.clip(out[..., :3], 0.0031308, None), 1 / 2.4) - 0.055)
out[..., 3] = 1
step = max(1, w // 1024)
out = out[::step, ::step]
o = bpy.data.images.new("o", out.shape[1], out.shape[0])
o.pixels = out.ravel()
o.filepath_raw = argv[1]; o.file_format = "PNG"; o.save()
