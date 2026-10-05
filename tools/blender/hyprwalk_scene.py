"""hyprwalk_scene: builds the attack's Blender scene from scratch: Hatsune Miku NT in a scene of her own, rigged (hyprwalk_rig),
the hook keyed (hyprwalk_hook), hands in fists. In Blender: import hyprwalk_scene; hyprwalk_scene.build() (or build(avatar_path))"""
import math
import os

import bpy
from mathutils import Euler

import hyprwalk_rig
import hyprwalk_hook

AVATAR = os.path.expanduser("~/.local/share/hyprwalk/avatars/Miku/Miku.glb")
SCENE = "hyprwalk attack"


def build(avatar=AVATAR):
    sc = bpy.data.scenes.get(SCENE) or bpy.data.scenes.new(SCENE)
    bpy.context.window.scene = sc
    if "Armature" not in sc.objects:
        bpy.ops.import_scene.gltf(filepath=avatar, bone_heuristic='BLENDER')
    arm = bpy.data.objects["Armature"]
    # show the attack's bones in front, minus the importer's spheres; hide the hundreds of others (hair, skirt, fingers)
    for pb in arm.pose.bones:
        pb.custom_shape = None
    keyed = {hyprwalk_rig.BONES[k] for k in hyprwalk_rig.BONES}
    shown = arm.data.collections.get("hyprwalk attack") or arm.data.collections.new("hyprwalk attack")
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
    # Workbench's texture mode shows the active image node: pick the one feeding the BSDF's base color
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
    hyprwalk_hook.build()
    hyprwalk_rig.fist()
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
