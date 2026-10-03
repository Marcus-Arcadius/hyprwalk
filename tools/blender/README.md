# tools/blender: the attack's and the walk's animations

The avatar's attack (a left click on nothing, see the main README's Attacks) is a right hook made in
Blender on Hatsune Miku NT, written out as two VRM animations: `assets/attack.vrma` (third person) and
`assets/attack-first-person.vrma` (seen from the eyes). The plugin builds them in, puts them on any
humanoid as it does emotes, and mirrors them for the left arm.

- `h3d_rig.py`: the rig. Per arm an IK target at the wrist (`CTL.wrist.R`, its rotation the hand's) and
  a pole the elbow points to (`CTL.elbow.R`); the spine, chest, neck, head and collarbones keyed as
  they are. Key poses are written in her own terms: the trunk's turns in degrees (yaw to her left, pitch
  bending ahead), the wrists in meters from the chest's joint (out to that arm's side, up, ahead), and
  `mix(A, B, trunk=…, R=…, L=…)` for a key part way between two, each part at its own pace (the trunk
  leads a strike, the fist snaps after it). `BONES` names Miku's bones.
- `h3d_hook.py`: the hook's key poses and timing. Third person's at frames 0–45: the fists up, the right
  wound up out past her shoulder ahead of her twin tails (where the camera behind her sees it), swept round
  in front of her face as the trunk turns, held, back to a guard and down. First person's at frames
  100–130: from the hands as first person holds them, the fist drawn back to the bottom right of the
  view and struck up to the crosshair (side on: her bell sleeves hide a fist that points away). Timeline
  markers say when the fists are up (`ready`: a punch after another starts there), when it strikes
  (`hit`), when the other arm's may start (`next`) and when it lets go of the body (`out`).
- `h3d_scene.py`: the scene from nothing: Miku imported, the rig, the keys (`h3d_scene.build()`).
- `h3d_export.py`: both clips out as VRM animations with `tools/blend2vrma.py`.
- `h3d_shots.py`: quick renders round her, and from the plugin's third person camera.

The Blender file is `~/Documents/3D-extras/attack/attack.blend` (Miku is in it, so not in the repo).
To change the punch, open it and move the controls or the trunk's keys (or change `h3d_hook.py` and
rebuild the scene), then in Blender's Python console or the file's `hypr3d export` text:

```python
import sys; sys.path.insert(0, "/home/monero/Documents/3D/tools/blender")
import h3d_export; h3d_export.run()                     # the built in ones: then ./build.sh
h3d_export.run("/home/monero/.local/share/hypr3d/avatars/Miku", prefix="Miku.")
```

The second writes `Miku.attack.vrma` and `Miku.attack-first-person.vrma` next to Miku; her settings
file's `"attack": {"file": "Miku.attack.vrma", "firstPerson": "Miku.attack-first-person.vrma"}` then
uses them when the avatar loads again, with no rebuild. Check it with
`tools/test/harness/attack_check.sh` (the hair out of the arms, the fist in view, nothing jumping), and
`build/test/shot --attackclip FILE [FIRSTPERSON] --avatar …` to try files before they're built in.

## The walk and the run

`h3d_walk.py` makes the walk and the run every humanoid goes by (`assets/walk.vrma`, `assets/run.vrma`, built in):
how the body goes over the steps. The plugin steps the feet itself (planted where they land, the stride and cadence
from the legs and the speed) and plays these above them at the steps' phase: the hips' turn, drop and sway, the
spine and chest turning against the hips and leaning ahead, the head's nod and tilt, the collarbones, and the arms
(swung a little after the legs, the elbow bending as the arm comes ahead, the forearm and the hand following through).
Each is one stride in place, 60 frames a second: the left heel lands on its first frame and its last, the right half
way. They were made on the Magical Mirai 2019 Miku (`~/Documents/3D-extras/miku-mm2019`, a T-posed rip; her Blender
file has the two actions in it), and play on any humanoid as VRM animations do.

The numbers are in `CYCLES` at the top of `h3d_walk.py`, in her terms (degrees, meters, parts of the stride); the legs
step as the plugin's do (its foot roll, knee and lift curves, in `CURVES`), so a cycle reads as a whole here. In
Blender's Python console or a text block, with the avatar's armature in the scene:

```python
import sys; sys.path.insert(0, "/home/monero/Documents/3D/tools/blender")
import h3d_walk
SETTINGS = "/home/monero/.local/share/hypr3d/avatars/MikuMM2019/MikuMM2019.hypr3d.json"  # which bone is which
h3d_walk.build(SETTINGS)          # the actions "Walk" and "Run", keyed anew from CYCLES (both loop)
h3d_walk.show("Run")              # that one on the armature, the scene's frames its stride
h3d_walk.export(settings=SETTINGS)                    # into assets/: then ./build.sh
h3d_walk.export("/somewhere", prefix="Miku.", settings=SETTINGS)  # /somewhere/Miku.walk.vrma, Miku.run.vrma
```

The second export is for one avatar: its settings file's `"walk": {"walk": "Miku.walk.vrma", "run": "Miku.run.vrma"}`
uses them when it loads again, with no rebuild (`"walk": "none"` walks it procedurally). Keys edited by hand in the
Graph Editor export as they are (`build()` would key them anew). Check a change with the harness: `build/test/shot
--gaitstyle 0|1` walks without or with them, and `~/Documents/3D-extras/gait-tools` measures it (`snaps.py` for how
smoothly the arms go, `clipsum.sh` for arms in the body or a skirt, `taps.py` for keys tapped fast, `sweep.py` for
turns). The plugin keeps the arms clear of the body itself, out from the clip's as far as each avatar needs.
