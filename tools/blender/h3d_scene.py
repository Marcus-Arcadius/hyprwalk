"""h3d_scene: the attack's Blender scene from nothing: Hatsune Miku NT imported into a scene of her own, the rig on her
(h3d_rig), the hook keyed (h3d_hook: third person's at frames 0-45, first person's at 100-130), the hands in fists to
look at. In Blender: import h3d_scene; h3d_scene.build() (the avatar: h3d_scene.AVATAR, or build(path))"""
import math
import os

import bpy
from mathutils import Euler

import h3d_rig
import h3d_hook

AVATAR = os.path.expanduser("~/.local/share/hypr3d/avatars/Miku/Miku.glb")
SCENE = "hypr3d attack"


def build(avatar=AVATAR):
    sc = bpy.data.scenes.get(SCENE) or bpy.data.scenes.new(SCENE)
    bpy.context.window.scene = sc
    if "Armature" not in sc.objects:
        bpy.ops.import_scene.gltf(filepath=avatar, bone_heuristic='BLENDER')
    arm = bpy.data.objects["Armature"]
    # the bones the attack keys shown (not the importer's spheres for them), in front; the rest (her hair's, her skirt's,
    # her fingers': hundreds) in a bone collection of their own, hidden
    for pb in arm.pose.bones:
        pb.custom_shape = None
    keyed = {h3d_rig.BONES[k] for k in h3d_rig.BONES}
    shown = arm.data.collections.get("hypr3d attack") or arm.data.collections.new("hypr3d attack")
    rest = arm.data.collections.get("the rest") or arm.data.collections.new("the rest")
    for b in arm.data.bones:
        for c in list(b.collections):
            c.unassign(b)
        (shown if b.name in keyed else rest).assign(b)
    rest.is_visible = False
    arm.data.display_type = 'OCTAHEDRAL'
    arm.show_in_front = True
    ico = bpy.data.objects.get("Icosphere")
    if ico:
        ico.hide_set(True)
        ico.hide_render = True
    # (Workbench shows each material's base color texture: the image node feeding the BSDF's base color)
    for m in bpy.data.materials:
        if not m.node_tree:
            continue
        bsdf = next((n for n in m.node_tree.nodes if n.type == 'BSDF_PRINCIPLED'), None)
        pick, stack, seen = None, [bsdf.inputs['Base Color']] if bsdf else [], set()
        while stack and not pick:
            for link in stack.pop().links:
                n = link.from_node
                if n.name in seen:
                    continue
                seen.add(n.name)
                if n.type == 'TEX_IMAGE':
                    pick = n
                    break
                stack += list(n.inputs)
        if pick:
            m.node_tree.nodes.active = pick
    h3d_hook.build()
    h3d_rig.fist()
    sc.frame_set(0)
    # the view: her front right, the whole of her
    for area in bpy.context.screen.areas:
        if area.type == 'VIEW_3D':
            sp = area.spaces[0]
            sp.shading.type = 'MATERIAL'
            r3d = sp.region_3d
            r3d.view_location = (0, -0.15, 1.05)
            r3d.view_distance = 3.0
            r3d.view_rotation = Euler((math.radians(80), 0, math.radians(-35)), 'XYZ').to_quaternion()
            r3d.view_perspective = 'PERSP'
    return sc
