"""h3d_export: the attack's two clips (h3d_hook.CLIPS: third person's, first person's) from the scene open in Blender as
VRM animations, with tools/blend2vrma.py: into the repo's assets/ (the built in attacks: rebuild the plugin), or into a
folder of yours that an avatar's settings file's "attack" points at (reload the avatar, no rebuild). In Blender:
    import h3d_export; h3d_export.run()               # -> assets/attack.vrma, assets/attack-first-person.vrma
    h3d_export.run("/somewhere", prefix="Miku.")      # -> /somewhere/Miku.attack.vrma, Miku.attack-first-person.vrma
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
if os.path.join(HERE, "..") not in sys.path:
    sys.path.insert(0, os.path.join(HERE, ".."))  # (tools/: blend2vrma)

import blend2vrma
import h3d_hook

ASSETS = os.path.normpath(os.path.join(HERE, "..", "..", "assets"))
# which of the armature's bones is which: the avatar's settings file's "humanoid"
SETTINGS = os.path.expanduser("~/.local/share/hypr3d/avatars/Miku/Miku.hypr3d.json")


def run(folder=ASSETS, prefix="", settings=SETTINGS, armature="Armature"):
    out = []
    for clip, name, title in (("third", "attack.vrma", "Hook"), ("first", "attack-first-person.vrma", "Hook (first person)")):
        out.append(blend2vrma.export(os.path.join(folder, prefix + name), armature=armature, humanoid=settings,
                                     frames=h3d_hook.CLIPS[clip], bones="upper", name=title))
    return out
