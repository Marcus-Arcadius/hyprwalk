"""h3d_shots: renders of the attack from a few cameras round her and from the plugin's third person camera (Workbench:
quick, the shapes clear): render(folder, views, frames)"""
import math
import os

import bpy
from mathutils import Euler, Vector

VIEWS = {  # name: (degrees round her from straight ahead, to her right > 0; height of the camera; distance; looking at; lens)
    "front": (0, 1.35, 2.4, (0, -0.15, 1.15), 70),
    "frontR": (40, 1.4, 2.4, (0, -0.15, 1.15), 70),
    "frontL": (-40, 1.4, 2.4, (0, -0.15, 1.15), 70),
    "side": (90, 1.3, 2.4, (0, -0.2, 1.15), 70),
    "behindR": (150, 1.55, 2.2, (0, -0.1, 1.15), 70),
    "behind": (180, 1.6, 2.6, (0.0, -0.1, 1.15), 70),
    "top": (20, 3.2, 1.4, (0, -0.2, 1.1), 50),
}
# the plugin's third person camera: 2.6 m behind her head's height plus 15 cm (0.95 of her height), 0.4 m to her right,
# looking level ahead; 70 degrees of view up and down
TP = ((-0.4, 2.6, 1.66), 70)


def camera(name):
    if name == "tp":
        cam = camera("front")
        (x, y, z), fov = TP
        cam.location = Vector((x, y, z))
        cam.rotation_euler = Vector((0, -1, 0)).to_track_quat('-Z', 'Y').to_euler()
        cam.data.sensor_fit = 'VERTICAL'
        cam.data.sensor_height = 24
        cam.data.lens = 12 / math.tan(math.radians(fov / 2))
        return cam
    deg, h, d, at, lens = VIEWS[name]
    cam = bpy.data.objects.get("CAM.shots")
    if cam is None:
        cam = bpy.data.objects.new("CAM.shots", bpy.data.cameras.new("CAM.shots"))
        bpy.context.scene.collection.objects.link(cam)
    a = math.radians(deg)
    # ahead of her is -Y; her right is -X: round to her right
    pos = Vector((-math.sin(a) * d, -math.cos(a) * d, h))
    look = Vector(at)
    cam.location = pos
    cam.rotation_euler = (look - pos).to_track_quat('-Z', 'Y').to_euler()
    cam.data.sensor_fit = 'AUTO'
    cam.data.lens = lens
    bpy.context.scene.camera = cam
    return cam


def setup_render(w=360, h=480):
    sc = bpy.context.scene
    sc.render.engine = 'BLENDER_WORKBENCH'
    sc.render.resolution_x, sc.render.resolution_y = w, h
    sc.render.resolution_percentage = 100
    sc.render.film_transparent = False
    sh = sc.display.shading
    sh.light = 'STUDIO'
    sh.color_type = 'TEXTURE'
    sh.show_object_outline = True
    sh.show_cavity = False
    sc.render.image_settings.file_format = 'PNG'


def render(out_dir, views, frames, prefix="", size=(360, 480)):
    os.makedirs(out_dir, exist_ok=True)
    setup_render(*size)
    sc = bpy.context.scene
    arm = bpy.data.objects.get("Armature")
    hidden = arm.hide_render if arm else None
    if arm:
        arm.hide_render = True
    for o in bpy.data.objects:
        if o.name.startswith("CTL."):
            o.hide_render = True
    files = []
    for v in views:
        camera(v)
        for f in frames:
            sc.frame_set(f)
            p = os.path.join(out_dir, f"{prefix}{v}_{f:03d}.png")
            sc.render.filepath = p
            bpy.ops.render.render(write_still=True)
            files.append(p)
    if arm:
        arm.hide_render = hidden
    return files
