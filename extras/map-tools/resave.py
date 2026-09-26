import bpy, sys
a = sys.argv[sys.argv.index("--") + 1:]
for src, dst in zip(a[0::2], a[1::2]):
    img = bpy.data.images.load(src)
    s = bpy.context.scene.render.image_settings
    s.file_format = "PNG"; s.color_mode = "RGB"; s.compression = 90
    img.save_render(dst)
