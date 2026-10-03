# tools/blender: the attack and walk animations

The built-in punch (a left click on nothing) is a right hook made in Blender on Hatsune Miku NT and exported as two
VRM animations: `assets/attack.vrma` (third person) and `assets/attack-first-person.vrma`. The plugin builds them in,
plays them on any humanoid and mirrors them for the left arm.

- `h3d_rig.py`: the rig. Each arm has an IK target at the wrist (`CTL.wrist.R`) and an elbow pole (`CTL.elbow.R`);
  the spine, chest, neck, head and collarbones are keyed directly. Key poses are given in the avatar's terms: trunk
  turns in degrees, wrists in metres from the chest joint, and `mix(A, B, trunk=…, R=…, L=…)` for in-between keys.
  `BONES` names Miku's bones.
- `h3d_hook.py`: the hook's key poses and timing: third person at frames 0–45, first person at 100–130. Timeline
  markers time it: `ready` (fists up), `hit`, `next` (the other arm may start) and `out` (letting go of the body).
- `h3d_scene.py`: builds the scene from nothing (`h3d_scene.build()`).
- `h3d_export.py`: exports both clips with `tools/blend2vrma.py`.
- `h3d_shots.py`: quick renders, including from the plugin's third person camera.

The Blender file is `~/Documents/3D-extras/attack/attack.blend` (Miku is in it, so it's not in the repo). To change
the punch, edit the controls or keys (or `h3d_hook.py`, then rebuild the scene) and export from Blender's Python
console:

```python
import sys; sys.path.insert(0, "/home/monero/Documents/3D/tools/blender")
import h3d_export; h3d_export.run()                     # the built-in ones: then ./build.sh
h3d_export.run("/home/monero/.local/share/hypr3d/avatars/Miku", prefix="Miku.")
```

The second writes `Miku.attack.vrma` and `Miku.attack-first-person.vrma` for one avatar. Its settings file's
`"attack": {"file": "Miku.attack.vrma", "firstPerson": "Miku.attack-first-person.vrma"}` uses them the next time
it loads, with no rebuild. Check a change with `tools/test/harness/attack_check.sh`, or try files with
`build/test/shot --attackclip FILE [FIRSTPERSON] --avatar …`.

## The walk and the run

`h3d_walk.py` makes the built-in walk and run (`assets/walk.vrma`, `assets/run.vrma`): how the body moves over the
steps. The plugin plants the feet itself and plays these above them in phase with the steps. Each is one stride in
place at 60 fps, the left heel landing on the first and last frames. They were made on the Magical Mirai 2019 Miku
(`~/Documents/3D-extras/miku-mm2019`) and play on any humanoid.

The numbers are in `CYCLES` at the top of `h3d_walk.py` (degrees, metres, fractions of the stride). The legs step as
the plugin's do (`CURVES`), so a cycle reads as a whole in Blender. With the avatar's armature in the scene:

```python
import sys; sys.path.insert(0, "/home/monero/Documents/3D/tools/blender")
import h3d_walk
SETTINGS = "/home/monero/.local/share/hypr3d/avatars/MikuMM2019/MikuMM2019.hypr3d.json"  # which bone is which
h3d_walk.build(SETTINGS)          # the "Walk" and "Run" actions, keyed from CYCLES
h3d_walk.show("Run")              # that one on the armature
h3d_walk.export(settings=SETTINGS)                    # into assets/: then ./build.sh
h3d_walk.export("/somewhere", prefix="Miku.", settings=SETTINGS)  # /somewhere/Miku.walk.vrma, Miku.run.vrma
```

The second export is for one avatar: its settings file's `"walk": {"walk": "Miku.walk.vrma", "run": "Miku.run.vrma"}`
uses them with no rebuild (`"walk": "none"` walks it procedurally). Keys edited by hand export as they are
(`build()` would key them anew). `build/test/shot --gaitstyle 0|1` walks without or with the clips, for comparison.
