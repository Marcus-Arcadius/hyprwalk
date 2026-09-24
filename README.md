# hypr3d

A [Hyprland](https://hyprland.org) plugin that turns the desktop into a place you can walk around in.
While 3D mode is on, your windows hang on a wall in a small courtyard, or in any glTF map, and you
walk up to them in first person. The crosshair clicks, scrolls and types into whatever it points at.
You can take a window off the wall and put it anywhere.

With an avatar loaded you can switch to third person. The avatar works much like one in VRChat: it
has faces, hand gestures, emotes, a radial Action Menu, outfit toggles, and hair and clothes that
swing. It can be a VRM, a plain GLB, or a VRChat avatar converted with `tools/unity2hypr3d.py`.

![Your windows on the courtyard wall](screenshots/desktop-wall.png)

![Third person, with the Action Menu's outfit page open](screenshots/avatar-action-menu.png)

![CS2's de_mirage, converted from a local install with tools/cs2map.py](screenshots/mirage-mid-cs2.png)

## Building

hypr3d is built against the Hyprland it runs in: a plugin has to be compiled with the headers and the
compiler of that exact Hyprland build. It was written for Hyprland 0.55.2 on NixOS.

```sh
./build.sh          # builds hypr3d.so
./build.sh clean
```

`build.sh` finds the running Hyprland binary (`pgrep`, then `/proc/<pid>/exe`) and asks Nix for the
derivation that built it (`nix-store --query --deriver`). It builds that derivation's `dev` output,
which holds the headers, and then runs `make` inside the derivation's own build shell
(`nix develop "<drv>^*"`). That gives the same compiler and flags Hyprland was built with. If
Hyprland isn't running, name its binary:
`HYPR_BIN=/nix/store/…-hyprland-…/bin/Hyprland ./build.sh`. Any arguments go on to `make`.

On other distributions, `make` should work anywhere `pkg-config` finds the headers of the Hyprland
you run (`hyprland`, `pixman-1`, `libdrm`, `glesv2`, `egl`, `cairo` and `pangocairo`). This is
untested.

## Loading

```sh
hyprctl plugin load "$PWD/hypr3d.so"      # an absolute path
hyprctl plugin unload "$PWD/hypr3d.so"
```

To keep it, load it from your config. With a Lua config (Hyprland's `hl.*` API, which is also what
home-manager can write), use this:

```lua
hl.plugin.load("/home/you/Documents/3D/hypr3d.so")
hl.config({ plugin = { hypr3d = {
    avatar = "~/avatars/me.glb",
    avatar_physics = true,
} } })
hl.bind("SUPER + grave", function() hl.plugin.hypr3d.toggle() end)
```

`hl.config` flattens nested tables, so `plugin = { hypr3d = { map = … } }` sets
`plugin:hypr3d:map`. The plugin's values only exist once it is loaded, and Hyprland reloads the
config after loading a plugin. With a classic `hyprland.conf`, use
`plugin { hypr3d { avatar = ~/avatars/me.glb } }` and `bind = SUPER, grave, hypr3d:toggle`.

A plugin runs inside the compositor, so a crash in it takes your session down with it.

## Config

| Value | Default | What it does |
|---|---|---|
| `plugin:hypr3d:layer_spacing` | `0.02` | metres between stacked layers (bars, notifications) on the desktop wall, 0–0.5 |
| `plugin:hypr3d:wallpaper` | `false` | keep the wallpaper on the desktop wall in 3D |
| `plugin:hypr3d:map` | `""` | a glTF/GLB map to walk around in, instead of the courtyard |
| `plugin:hypr3d:map_scale` | `0` | metres per map unit; 0 guesses it from the map |
| `plugin:hypr3d:avatar` | `""` | a glTF, GLB or VRM avatar, seen in third person |
| `plugin:hypr3d:avatar_height` | `0` | scale the avatar to this height in metres; 0 keeps its own |
| `plugin:hypr3d:avatar_physics` | `true` | hair, skirts and the like swing (spring bones) |
| `plugin:hypr3d:avatar_emotes` | `""` | more emotes: VRM animations (`.vrma`) or glTF clips; files or folders, separated by commas |

Paths may start with `~/`. Maps and avatars load in the background, and a failure shows up as a
notification.

## Controls

Enter and leave 3D with the `hypr3d:toggle` dispatcher, `hyprctl hypr3d toggle` or
`hl.plugin.hypr3d.toggle()`. Keys held with Super, or with Ctrl+Alt, still go to Hyprland, so your
compositor shortcuts keep working in 3D.

| Input | In 3D |
|---|---|
| mouse, arrow keys | look around |
| W A S D | walk; Left Shift runs |
| Space | jump (flying: up) |
| Left Ctrl or C | crouch (flying: down) |
| F | fly on/off |
| R | back to the start |
| left/right/middle click | click whatever the crosshair points at |
| wheel | scroll it; in third person, pointing at nothing, zoom the camera |
| E or Enter | type into the window under the crosshair; Super+Esc goes back to walking |
| G | pick up the window under the crosshair; G or a left click puts it down where it is, a right click or Esc puts it back where it was; the wheel moves it nearer or further, Ctrl+wheel resizes it |
| X | send a window you've placed back to the wall |
| V | first / third person (needs an avatar) |
| Tab | the Action Menu |
| F1–F8 | hand gestures (Neutral, Fist, Open, Point, Victory, Rock'n'roll, Handgun, Thumbs up), as in VRChat's desktop mode: with Left Shift held only the left hand, with Right Shift only the right, otherwise both |
| Esc | leave 3D |

The Action Menu has pages for emotes, expressions, gestures, the outfit and options (view, physics,
fly, respawn, reset face, stop emote). With it open, the mouse moves its cursor, a left click
picks, a right click goes back and a middle click closes it. The wheel goes round it, 1–8 pick an
item, Enter picks, Backspace goes back and Esc closes it. WASD still walks.

## hyprctl

`hyprctl hypr3d` with no arguments prints the state as JSON. Commands:

| Command | |
|---|---|
| `status`, `toggle`, `on`, `off [now]` | the state as JSON; enter and leave 3D |
| `type [on\|off]` | type into the window under the crosshair, or go back to walking |
| `look dx dy`, `turn yaw pitch`, `tp x y z` | turn by mouse counts, turn to angles in degrees, teleport |
| `walk secs [forward\|back\|left\|right]`, `jump`, `fly` | move from a script |
| `click [left\|right\|middle]` | click where the crosshair is |
| `sens [value]` | mouse sensitivity |
| `grab`, `place`, `hold dist [scale]`, `reset-windows` | carry windows, and put them all back |
| `map [path\|none\|reload\|forget\|scale s]` | load a map; `forget` drops the start and desktop place saved for it |
| `spawn [here]` | go back to the start, or make where you stand the start |
| `desktop [here [height]]` | where the desktop hangs, or hang it where the crosshair points |
| `view [first\|third\|toggle] [distance] [side]` | the camera |
| `avatar [path\|none\|reload\|height m]` | load an avatar, or print its state |
| `avatar expression [name [weight]\|none]` | set a face |
| `avatar gesture [left\|right\|both gesture]` | set a hand gesture |
| `avatar parts [reset]`, `avatar toggle name [on\|off\|reset]`, `avatar shape key [weight\|reset]` | the outfit |
| `avatar physics [on\|off\|toggle]` | spring bones |
| `avatar emote [name\|number\|file\|folder [once\|loop]\|stop]` | play an emote, or load emotes from files |
| `menu [open [page]\|close\|toggle\|back\|pick [n]\|move dx dy\|scroll n]` | drive the Action Menu |

The `hypr3d:menu` dispatcher toggles the Action Menu. `hypr3d:menu emotes` opens a page, and any
other argument does what `hyprctl hypr3d menu` does. The Lua functions are
`hl.plugin.hypr3d.toggle()`, `enter()`, `exit()`, `type()` and `menu([page or command])`.

## Avatars

`plugin:hypr3d:avatar` (or `hyprctl hypr3d avatar FILE`) takes one of these:

- **VRM 0.x and 1.0**: the humanoid map, expressions (blend shapes, material colours and texture
  transforms), look-at, spring bones (VRM 0.x `secondaryAnimation` and `VRMC_springBone`) and node
  constraints (`VRMC_node_constraint`).
- **A plain glTF/GLB**: the humanoid bones are guessed from their names (Mixamo, VRoid, Blender's
  rigs and most other naming styles). Expressions come from shape key names (VRoid, VRChat, MMD and
  ARKit), and spring bones from bone names (hair, skirt, cape, sleeves, ribbons, tail and ears, in
  Japanese too). Clips named idle, walk, run, jump or fall play for those; otherwise the avatar is
  walked procedurally.
- **What `tools/unity2hypr3d.py` writes**: a GLB with a settings file next to it (below).

It blinks, its eyes wander, it turns its head where you look, and walks, runs, jumps, falls, crouches
and flies. Gestures curl its fingers and can set a face. The built-in emotes are Wave, Clap, Point,
Cheer, Dance, Backflip, Sad Kick and Die, and `avatar_emotes` adds your own `.vrma` or glTF clips.

### The settings file

`AVATAR.hypr3d.json` (or `AVATAR.glb.hypr3d.json`) next to the avatar adds to what the model says
or overrides it. The converter writes it; you can also write one by hand. Every key is optional:

| Key | |
|---|---|
| `humanoid` | `{"LeftUpperArm": "Arm_L", …}`: humanoid bone (Unity's or VRM's names) → node name |
| `expressions` | `[{"name", "preset", "shapes": {"shape key" or "mesh/shape key": 0..1}, "binary", "blink"/"lookAt"/"mouth": "block"\|"blend"\|"none"}]` |
| `gestures` | `{"left"/"right"/"both": {"fist": "expression name" or "none", …}}`: the face each hand gesture sets |
| `hidden` | `["mesh node", …]`: parts that start hidden |
| `toggles` | `[{"name", "group", "on", "show": [parts], "hide": [parts], "shapes": {…}}]`: outfit toggles for the Action Menu; the toggles of a group are exclusive |
| `springs` | `[{"name", "bones": [roots], "ignore": [bones], "stiffness", "drag", "gravity", "gravityDir": [x,y,z], "radius", "center", "immobile", "colliders": [names, or "body"]}]`: each root and everything under it swings |
| `colliders` | `[{"name", "node", "offset": [x,y,z], "tail": [x,y,z], "radius"}]`: spheres, or capsules with a tail, in the node's units |
| `immobile` | 0..1, default 0.9: how much of the air the avatar carries along as it moves. With 0, walking at 4.5 m/s blows long hair out level behind it |

## Maps

`plugin:hypr3d:map` loads a glTF/GLB. The scale is guessed unless `map_scale` gives it. A node named
`hypr3d_spawn` marks the start and faces its −Z. A node named `hypr3d_desktop` marks where the desktop
hangs, facing its +Z. Without them hypr3d uses a game's `info_player_*` start and looks for a flat wall
itself. What you change with `spawn here` and `desktop here` is saved in
`$XDG_STATE_HOME/hypr3d/maps/`. A directional light becomes the sun, and nodes under a
`hypr3d_backdrop` node are scenery with no collision.

## Tools

The converters are Python scripts. They need [Blender](https://www.blender.org) (5.2 was used), and
re-run themselves inside it.

### tools/unity2hypr3d.py: VRChat avatars

```sh
python3 tools/unity2hypr3d.py Avatar.unitypackage [Outfit.unitypackage…] -o ~/avatars/me.glb
python3 tools/unity2hypr3d.py ~/UnityProjects/MyAvatar --list
python3 tools/unity2hypr3d.py MyAvatar.unitypackage --outfit "Some Dress" -o me.glb
```

It takes a `.unitypackage`, a Booth-style `.zip` (Shift-JIS names too), a Unity project or its
`Assets` folder, or a `.prefab`/`.unity` inside a project. Unity isn't needed. It writes `OUT.glb`
and `OUT.hypr3d.json` for hypr3d, carrying over:

- the humanoid bone map, the visemes, the blink shape and the eye bones
- faces set by hand gestures in the FX controller
- Expressions Menu toggles that show or hide objects or set shape keys, and objects that start hidden
- PhysBones and Dynamic Bones, as springs and colliders
- materials: colour, texture, cutout or transparent, emission and culling (Standard, lilToon,
  Poiyomi and MToon settings, and the common property names of other shaders)
- Modular Avatar setups, built the way MA builds them for VRChat: Merge Armature (an outfit's bones
  join the avatar's and its meshes follow them), Bone Proxy, Move To, PhysBone Blocker, and MA's
  menus and toggles (Menu Item, Menu Installer, Menu Group, Object Toggle, Shape Changer, Merge
  Animator for FX, Parameters)

`--outfit NAME|PATH` puts an outfit on that the avatar's prefab doesn't have yet, the way dragging
it onto the avatar and running MA's *Setup Outfit* would. It works whether or not the outfit is set
up for MA. It finds the outfit's hips, works out the prefix and suffix of its bone names, and matches
bones with MA's name table. It turns A-pose arms to the avatar's pose and warns when bones are more
than 1 cm off. `--outfit` can be given more than once. `--list` lists the avatars found, `--avatar NAME`
picks one, and `--max-texture N` caps the texture size (default 2048).

Not converted: shader effects beyond the above, other animations, material swaps, MA's Replace
Object, Blendshape Sync, Material Setter/Swap, Visible Head Accessory and Mesh Settings, VRCFury,
constraints, particles, audio and contacts. Blender imports only binary FBX files.

### tools/cs2map.py: Counter-Strike 2 maps

```sh
python3 tools/cs2map.py --list
python3 tools/cs2map.py de_mirage        # → ~/.local/share/hypr3d/maps/de_mirage.glb
hyprctl hypr3d map ~/.local/share/hypr3d/maps/de_mirage.glb
```

It exports a map from your own CS2 install with
[Source 2 Viewer](https://github.com/ValveResourceFormat/ValveResourceFormat)'s CLI. When your CS2 build's
shaders are too new for the installed Source 2 Viewer, it downloads or builds a newer one. It keeps
the 3D skybox, the sky and CS2's baked lighting: lightmaps, light probes, the sun, fog, the exposure
range and the tone curve. It also keeps the materials' detail textures, self-illumination and
blended layers. See `python3 tools/cs2map.py --help` for `--spawn`, `--desktop` and the rest. The maps are
Valve's; this reads your copy of the game for your own use.

### tools/test: the tests

- `tools/test/harness/`: `shot`, an offscreen render harness. It drives the plugin's real renderer,
  avatar animator and Action Menu on a surfaceless EGL context and writes PNGs, with no compositor
  involved. `tools/test/harness/build.sh` builds it into `build/test/shot` through `./build.sh`,
  linking the plugin's objects except `main.o` and `panels.o`. Its arguments run in order:

  ```sh
  build/test/shot --size 960x720 --avatar me.glb --frames 30 --out front.png --view 90 --out side.png
  build/test/shot --avatar me.glb --physics 1 --accel 40 --move 0 3 --frames 60 --view 90 --out walk.png --swing
  build/test/shot --avatar me.glb --menu outfit --toggle Hat off --expr Smile --out menu.png
  build/test/shot --map ~/.local/share/hypr3d/maps/de_mirage.glb --stand 0 0 0 90 0 --autoexp 1 --out mid.png
  ```

  `grep 'a == "--' tools/test/harness/shot.cpp` lists every option. There are options for the camera,
  motion, faces, gestures, toggles, emotes, the menu, maps, timing (`--bench`) and debugging
  (`--hide`, `--show`, `--glinfo`).
- `tools/test/synth/make.py PROJ`: writes a synthetic Unity project for the converter's tests (run it
  under Blender, see below). It holds an unpacked avatar prefab, a variant of an FBX with overrides,
  PSD and TGA textures, and outfits with and without Modular Avatar, including one with VRM bone
  names in an A pose.
- `tools/test/synth/check.py OUT.glb`: what a conversion wrote (the node tree, world positions,
  colliders, meshes, materials and images).
- `tools/test/synth/skincmp.py A.glb [--pose NODE AXIS DEG]… B.glb`: compares where two GLBs put
  every mesh's skinned vertices.
- `tools/test/synth/ma_unit.py`: unit tests of the Modular Avatar code on small hand-made
  hierarchies.
- `tools/test/synth/fbxread.py FILE.fbx`: prints a binary FBX's model tree (plain python3).
- `tools/test/regress.sh [--base REV|FILE] [--robot PATH]`: converts the synthetic avatars (and
  VRChat's robot sample, if you give its path) with the working copy's converter and with HEAD's,
  then compares the results. Avatars without MA must come out byte-identical.

Scripts that use numpy run under Blender's Python:

```sh
blender -b --factory-startup --python-exit-code 1 -P tools/test/synth/make.py -- /tmp/synth
blender -b --factory-startup --python-exit-code 1 -P tools/test/synth/ma_unit.py
```

## Known limits

- VRChat's own emote animations are proprietary, so the built-in emotes are procedural look-alikes.
  Load real ones as `.vrma` or glTF clips with `avatar_emotes`.
- In 3D mode, Alt+Tab doesn't reach Hyprland: Tab opens the Action Menu even with Alt held. Only keys
  with Super or Ctrl+Alt are passed through.
- No lip sync. The visemes are carried over, but nothing drives them yet.
- `tools/unity2hypr3d.py` doesn't read VRCFury setups yet (see its "not converted" list above).

## Credits

- [cgltf](https://github.com/jkuhlmann/cgltf) (MIT) and [stb_image and stb_dxt](https://github.com/nothings/stb)
  (public domain or MIT), vendored in `src/third_party/`.
- The bone-name table in `tools/unity2hypr3d.py` is from
  [Modular Avatar](https://github.com/bdunderscore/modular-avatar) (MIT, © 2022 bd_). MA took it from
  HhotateA's AvatarModifyTools (MIT, © 2021 @HhotateA_xR) and Azukimochi's BoneRenamer (MIT,
  © 2023 Azukimochi). The converter's other Modular Avatar support reimplements MA's behaviour in
  Python, written from reading MA's source. The license texts are in [THIRD_PARTY.md](THIRD_PARTY.md).
- VRChat, Modular Avatar, Counter-Strike 2 and Blender belong to their owners. This project has no
  connection with any of them, and it ships none of their assets.
