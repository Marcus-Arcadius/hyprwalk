"""hyprwalk_export: the attack's two clips (hyprwalk_hook.CLIPS) from the open Blender scene as VRM animations, via
tools/blend2vrma.py: into the repo's assets/ (built in: rebuild the plugin), or a folder an avatar's settings file's
"attack" points at (reload the avatar). In Blender:
    import hyprwalk_export; hyprwalk_export.run()               # -> assets/attack.vrma, assets/attack-first-person.vrma
    hyprwalk_export.run("/somewhere", prefix="Miku.")      # -> /somewhere/Miku.attack.vrma, Miku.attack-first-person.vrma
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
if os.path.join(HERE, "..") not in sys.path:
    sys.path.insert(0, os.path.join(HERE, ".."))  # tools/ has blend2vrma

import blend2vrma
import hyprwalk_hook

ASSETS = os.path.normpath(os.path.join(HERE, "..", "..", "assets"))
# its "humanoid" says which armature bone is which
SETTINGS = os.path.expanduser("~/.local/share/hyprwalk/avatars/Miku/Miku.hyprwalk.json")


def run(folder=ASSETS, prefix="", settings=SETTINGS, armature="Armature"):
    out = []
    for clip, name, title in (("third", "attack.vrma", "Hook"), ("first", "attack-first-person.vrma", "Hook (first person)")):
        out.append(blend2vrma.export(os.path.join(folder, prefix + name), armature=armature, humanoid=settings,
                                     frames=hyprwalk_hook.CLIPS[clip], bones="upper", name=title))
    return out
