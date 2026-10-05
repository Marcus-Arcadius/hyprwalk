# Avatars and maps

Back to the [README](../README.md).

## Avatars

`plugin:hyprwalk:avatar`, `hyprctl hyprwalk avatar FILE` or the Action Menu's Avatars page (it lists
`~/.local/share/hyprwalk/avatars/`) loads:

- **VRM 0.x and 1.0**: the humanoid, expressions, look-at, spring bones (with `VRMC_springBone_extended_collider`
  and `VRMC_springBone_limit`), node constraints, and MToon's shading, matcaps and outlines.
- **A plain glTF/GLB**: humanoid bones are guessed from their names (Mixamo, VRoid, Blender and the like),
  expressions from shape key names (VRoid, VRChat, MMD, ARKit) and spring bones from bone names (hair, skirt,
  tail…). Clips named idle, walk, run, jump or fall are used for those.
- **What `tools/unity2hyprwalk.py` writes**: a GLB and a settings file.

The avatar blinks, looks where you look, walks, runs, jumps, crouches and flies. Without clips of its own, a humanoid
walks procedurally: its feet stay planted, stride and cadence follow human gait data, and it handles stairs, slopes
and turns. Its upper body follows built-in walk and run clips (`assets/*.vrma`, made with
`tools/blender/hyprwalk_walk.py`). Spring bones step at 60 Hz and are interpolated between steps, so they stay smooth on
fast monitors. Materials are drawn in Unity's render queue order, with its stencil test, toon outlines (inverted
hull), and MToon-style toon shading and matcaps.

**First person**: with a humanoid avatar, the camera sits in its eyes, its head hidden. Its hands are in view and
follow what you do (a finger to what you click, tapping as you type, reaching for a window you carry, down while you
play), and looking down you see its body. `first_person_body = false` goes back to a plain camera at 1.65 m.

**Emotes**: the built-in ones are Wave, Clap, Point, Cheer, Dance, Backflip, Sad Kick and Die. `avatar_emotes` and
the settings file add more. An emote can have a sound (Ogg Vorbis, the settings file's `"sound"`). It plays through
PipeWire at `emote_volume`, and the dance keeps time with it.

**Attacks**: a left click on nothing throws a punch, alternating arms if you keep clicking. The punches were made in
Blender (`assets/attack.vrma`, `assets/attack-first-person.vrma`); a settings file can bring its own.

### The settings file

`AVATAR.hyprwalk.json` next to the avatar adds to what the model says, or overrides it. The converter writes one, and
you can write one by hand. Every key is optional:

| Key | |
|---|---|
| `humanoid` | `{"LeftUpperArm": "Arm_L", …}`: humanoid bone (Unity's or VRM's name) → node |
| `expressions` | `[{"name", "preset", "shapes": {"key" or "mesh/key": 0..1}, "binary", "blink"/"lookAt"/"mouth": "block"\|"blend"\|"none"}]` |
| `gestures` | the face each hand sign sets: `{"left"/"right"/"both": {"fist": "expression" or "none", …}, "combos": {"fist+open": …}}` |
| `hands` | finger poses: `{"file": "x.vrma", "left"/"right": {"fist": SECONDS, …}}` |
| `floor` | the height it stands on, in the GLB's units (MA's Floor Adjuster) |
| `hidden` | `["mesh node", …]` hidden at the start |
| `visemes` | consonant shapes: `{"pp"/"ff"/"ss"/"ch": {"key": 0..1}}` |
| `fixed` | `["node", …]` held still in the world (MA's World Fixed Object) |
| `toggles` | `[{"name", "group"/"groups", "on", "show", "hide", "shapes", "variants", "transforms", "loop", "drop"}]`: outfit toggles. A group's toggles are exclusive; `transforms` is `{"node": {"t", "r", "s"}}`; `loop` goes from `a` to `b` and back every `seconds`; `drop` leaves nodes in the world |
| `sliders` | `[{"name", "value", "keys": [{"at", "shapes", "transforms", "show", "hide", "variants"}]}]`: radial puppets, blended between keys. Two-axis: `"axes": 2`, `"grid": N`, keys `"at": [x, y]` |
| `walk` | `{"walk": FILE, "run": FILE}`: its own walk and run cycles, or `"none"` for the procedural body |
| `attack` | `FILE` or `{"file", "firstPerson"}`: its own punch; the animation's markers `ready`, `hit`, `next` and `out` time it |
| `emotes` | `[{"file" or "clip", "name", "loop", "hold", "grounded", "speed", "sound"}]` |
| `springs` | `[{"name", "bones", "ignore", "stiffness", "drag", "gravity", "gravityDir", "radius", "center", "immobile", "parentImmobile", "colliders", "limit"}]`. `limit` is a `VRMC_springBone_limit` cone, hinge or spherical limit; `radius` can be a list, one per bone |
| `colliders` | spheres and capsules `{"name", "node", "offset", "tail", "radius", "inside"}`, planes (`"normal"`) and discs (`"disc": {"normal", "radius"}`) |
| `immobile` | 0..1, default 0.9: how much of the air the avatar carries along as it moves |

The converter also writes glTF extras that hyprwalk reads: `hyprwalk_part` on primitives, and `hyprwalk_queue`,
`hyprwalk_stencil`, `hyprwalk_outline`, `hyprwalk_back`, `hyprwalk_light`, `hyprwalk_toon` and `hyprwalk_matcap` on
materials. Avatars converted when hyprwalk was called hypr3d (`hypr3d_*` extras, `AVATAR.hypr3d.json`) still load.

## Maps

`plugin:hyprwalk:map`, `hyprctl hyprwalk map FILE` or the Action Menu's Maps page (it lists
`~/.local/share/hyprwalk/maps/`) loads a glTF/GLB. A node named `hyprwalk_spawn` marks the start (facing −Z),
`hyprwalk_desktop` where the desktop hangs (facing +Z), and nodes under `hyprwalk_backdrop` are scenery without
collision. Without them hyprwalk uses a game's `info_player_*` start and finds a flat wall itself. A directional light
becomes the sun. `spawn here` and `desktop here` are saved in `$XDG_STATE_HOME/hyprwalk/maps/`. Maps exported when
hyprwalk was called hypr3d (`hypr3d_*` nodes, `HYPR3D_*` extensions) still load.

## Tools

The converters need [Blender](https://www.blender.org) (5.2 was used) and re-run themselves inside it.

**`tools/unity2hyprwalk.py`** converts a VRChat avatar into a GLB and a settings file, without Unity:

```sh
python3 tools/unity2hyprwalk.py Avatar.unitypackage [Outfit.unitypackage…] -o ~/avatars/me.glb
python3 tools/unity2hyprwalk.py Avatar.zip --outfit "Some Dress" --emote Dance.zip -o me.glb
python3 tools/unity2hyprwalk.py ~/UnityProjects/MyAvatar --list
```

It reads `.unitypackage` files, Booth `.zip`s and Unity projects. It carries over the humanoid map, visemes, blink
and eye bones, gesture faces and hand poses, menu toggles and radial puppets, PhysBones and Dynamic Bones (limits and
Immobile too), and materials (Standard, lilToon, Poiyomi, MToon and UnlitWF, with stencils, outlines and toon
shading). Modular Avatar and VRCFury setups are built the way they build for VRChat, and the Action layer's dances
become `.vrma` emotes with their songs. `--outfit` dresses the avatar the way MA's Setup Outfit does, and `--emote`
adds dances sold as bare clips. Other shader effects, constraints, particles and contacts aren't converted.
`--help` has the rest.

**`tools/cs2map.py`** exports a Counter-Strike 2 map from your own install with
[Source 2 Viewer](https://github.com/ValveResourceFormat/ValveResourceFormat)'s CLI (it downloads or builds a newer
one if your CS2 needs it):

```sh
python3 tools/cs2map.py --list
python3 tools/cs2map.py de_mirage        # → ~/.local/share/hyprwalk/maps/de_mirage.glb
```

It keeps the 3D skybox, CS2's baked lighting (lightmaps, light probes, the sun, fog, exposure and tone curve) and the
materials' details (tints, decals, blended layers). The maps are Valve's: this reads your copy of the game, for your
own use.

**`tools/blend2vrma.py`** writes a humanoid animation from Blender as a VRM animation (`.vrma`) that plays on any
humanoid: an emote, an attack or a walk cycle. `tools/blender/` has the scripts the built-in punches and walk were
made with (see [its README](../tools/blender/README.md)).

```sh
blender -b FILE.blend --python tools/blend2vrma.py -- OUT.vrma --humanoid AVATAR.hyprwalk.json [--frames A B] [--bones upper]
```
