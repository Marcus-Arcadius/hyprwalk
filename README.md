# hypr3d

A [Hyprland](https://hyprland.org) plugin that turns your desktop into a place you walk around in. In 3D mode your
windows hang on a wall in a small courtyard (or in any glTF map, CS2 maps included). You walk up to them in first
person, and the crosshair clicks, scrolls and types into whatever it points at. You can carry windows anywhere, tile
them in a ring around you, pin one to your view, launch apps into the world and play games where they hang.

Load an avatar (VRM, glTF, or a VRChat avatar converted with `tools/unity2hypr3d.py`) and you see the world through
its eyes, with its hands in view. It works much like a VRChat avatar: faces, hand gestures, emotes and dances, a
radial Action Menu, outfit toggles and sliders, hair and clothes that swing, toon outlines, and lip sync from your
microphone.

![Your windows on the courtyard wall](screenshots/desktop-wall.png)

![Third person, with the Action Menu's outfit page open](screenshots/avatar-action-menu.png)

![CS2's de_mirage, converted from a local install with tools/cs2map.py](screenshots/mirage-mid-cs2.png)

## Install

A plugin has to be built against the exact Hyprland it runs in, so you build hypr3d yourself. It supports Hyprland
0.55 and 0.56 and needs GCC 15 or newer. `./build.sh` finds the running Hyprland and its headers, warns if their
versions differ, and runs `make` (extra arguments go to `make`, e.g. `./build.sh clean`). The result is
`hypr3d.so`.

PipeWire's development files are optional. Without them, lip sync has no microphone and emotes play without sound.

### Arch Linux

```sh
sudo pacman -S --needed base-devel git hyprland pango libpipewire
git clone https://github.com/Marcus-Arcadius/hypr3d && cd hypr3d
./build.sh
```

### Fedora

Hyprland comes from the [sdegler/hyprland](https://copr.fedorainfracloud.org/coprs/sdegler/hyprland/) COPR (tested on
Fedora 44):

```sh
sudo dnf copr enable sdegler/hyprland
sudo dnf install gcc-c++ make pkgconf git hyprland-devel pango-devel pixman-devel pipewire-devel
git clone https://github.com/Marcus-Arcadius/hypr3d && cd hypr3d
./build.sh
```

### openSUSE Tumbleweed

```sh
sudo zypper install gcc-c++ make pkgconf git hyprland-devel glslang-devel pango-devel libpixman-1-0-devel pipewire-devel
git clone https://github.com/Marcus-Arcadius/hypr3d && cd hypr3d
./build.sh
```

`glslang-devel` is needed by Hyprland's headers, but `hyprland-devel` doesn't pull it in.

### Debian sid, Ubuntu 26.10

```sh
sudo apt install build-essential pkgconf git hyprland-dev libpango1.0-dev libpixman-1-dev libpipewire-0.3-dev
git clone https://github.com/Marcus-Arcadius/hypr3d && cd hypr3d
./build.sh
```

Older releases ship a Hyprland that is too old (Ubuntu 26.04 LTS has 0.53.3).

### NixOS

Nothing to install. `./build.sh` asks Nix for the derivation of the running Hyprland, builds its `dev` output (the
headers) and compiles inside that derivation's build shell, so the compiler and flags match. It does the same for
the running PipeWire. If Hyprland isn't running, name its binary:

```sh
HYPR_BIN=/nix/store/…-hyprland-…/bin/Hyprland ./build.sh
```

### Any distribution, with hyprpm

hyprpm, Hyprland's plugin manager, builds the headers for the Hyprland you run from its source. That makes it work
without a development package, and with a Hyprland you built yourself. It needs `git`, `cmake`, `cpio`,
`pkg-config`, `gcc`/`g++` and Hyprland's own build dependencies.

```sh
hyprpm update
hyprpm add https://github.com/Marcus-Arcadius/hypr3d
hyprpm enable hypr3d
hyprpm reload
```

To load it at login, add `exec-once = hyprpm reload -n` to `hyprland.conf` (Lua:
`hl.on("hyprland.start", function() hl.exec_cmd("hyprpm reload -n") end)`). After a Hyprland update, run
`hyprpm update` again. The repository is private, so git needs your GitHub login (e.g. `gh auth setup-git`).

### Other distributions

Install your distribution's Hyprland development package (`pkg-config --modversion hyprland` must find it), plus
pkg-config, make, GCC 15+ and the pango, pixman and (optionally) PipeWire development packages. Then run
`./build.sh`. If there's no development package, use hyprpm.

After updating Hyprland, log out and back in before you rebuild. Until then the old Hyprland is still running, and
it won't load a plugin built for the new one.

## Load

```sh
hyprctl plugin load "$PWD/hypr3d.so"      # needs an absolute path
hyprctl plugin unload "$PWD/hypr3d.so"
```

To load it from your config, with a Lua config:

```lua
hl.plugin.load("/path/to/hypr3d/hypr3d.so")
hl.config({ plugin = { hypr3d = {
    avatar = "~/avatars/me.glb",
} } })
hl.bind("SUPER + grave", function() hl.plugin.hypr3d.toggle() end)
```

or with `hyprland.conf`:

```ini
plugin = /path/to/hypr3d/hypr3d.so
plugin {
    hypr3d {
        avatar = ~/avatars/me.glb
    }
}
bind = SUPER, grave, hypr3d:toggle
```

The plugin's config values exist only once it's loaded; Hyprland reloads the config after loading a plugin. A
plugin runs inside the compositor, so if it crashes, your session goes down with it.

## Controls

Enter and leave 3D with the `hypr3d:toggle` dispatcher, `hyprctl hypr3d toggle` or `hl.plugin.hypr3d.toggle()`.
Keys held with Super or Ctrl+Alt still go to Hyprland. They act on the window under the crosshair: Super+Q closes
the window you look at, or nothing when you look at no window.

| Input | In 3D |
|---|---|
| mouse, arrow keys | look around |
| W A S D, Left Shift | walk (`walk_speed`), run (`run_speed`); steps up to 0.5 m are climbed |
| Space, Left Ctrl or C | jump, crouch (flying: up, down) |
| F | fly on/off |
| R | back to the start |
| clicks | click whatever the crosshair points at; a left click on nothing makes the avatar punch |
| wheel | scroll the window; on nothing in third person, zoom the camera |
| E or Enter | type into the window under the crosshair (every key goes to it); Super+Esc stops |
| P, Shift+P | play the window under the crosshair where it is, or filling the view; Super+Esc stops |
| Super+wheel | while playing a window in the tiling ring: bigger or smaller |
| G or H | pick up the window under the crosshair. G/H again or a left click puts it down where you look; Esc or a right click puts it back. While carrying: wheel = scale, Ctrl+wheel = distance, Shift+wheel = the window's real size |
| Shift+H | pin the window (or the one you carry) to the top right of your view; again takes it back |
| X | send a window back to the desktop wall |
| T, Shift+T | tiling mode: every window in a ring around you; Shift+T recentres the ring where you look |
| Y | the tiling ring follows you, or stays where it is |
| Q, B | the Apps page (launch into the world), the Windows page |
| Tab | the Action Menu |
| V | first / third person (needs an avatar) |
| F1–F8 | hand gestures; hold Left or Right Shift for one hand |
| Super+Esc | with another monitor: hand the mouse and keyboard to it (again: back) |
| Esc | leave 3D |

**The Action Menu** has pages for emotes, expressions, gestures, the outfit, apps, windows, options, maps and avatars.
The mouse points, a left click picks, a right click goes back, a middle click closes. The wheel, 1–9, Enter,
Backspace and Esc work too. Sliders (VRChat's radial puppets) open a dial, and two-axis puppets open a stick.

**Launchers and shell menus** (rofi, fuzzel, a power menu: any layer surface that takes the keyboard) show over the
3D view and work as on the desktop. An app you start from one opens in front of you.

### Play mode

P gives the window what a game expects: keyboard focus and every key (Super and Ctrl+Alt combinations still go to
Hyprland), the buttons and wheel, and the mouse as the app asks for it (relative motion under a pointer lock, a
confined pointer, or a plain pointer with the app's own cursor). P plays the window where it is. Shift+P turns the
camera to fill the view with it (`play_view = fill` swaps the two). In the tiling ring a played window takes
`play_size` of the view, and Super+wheel resizes it.

A window that goes fullscreen in 3D (a video, a game) is played automatically. Play mode ends with Super+Esc, when
you leave 3D, when the screen locks or the window closes, or when another window or a launcher takes the keyboard.
In that last case the game's keys are held back, so stray keys don't trigger hypr3d actions: P plays the game again,
and Super+Esc walks. Controllers are read by the game itself (SDL), and only while it has the keyboard focus.

### Windows in the world

- Apps launched from 3D (Q, or `hyprctl hypr3d launch`) open in front of you, as big as on your screen.
  `app_rules` sets where by window class:
  `CLASS: DISTANCE [HEIGHT|auto] [left|right|SIDE]`, e.g.
  `steam_app_.*: 2.4 1.6, discord: 1.2 auto left` (metres from your eye, the height or `auto`, and a side). Built-in
  rules put games and video players 2 m away, chat apps 1.3 m away and to the left, and anything else 1.5 m away.
- Where you put a window is remembered per class and map in `$XDG_STATE_HOME/hypr3d/windows/`; X forgets it.
- Other windows that open on the 3D monitor (a terminal, the portal's picker) open in front of you too. Dialogs
  follow their window.
- The Windows page (B) lists every window: focus, bring here, to the wall, pin/unpin, bigger/smaller (its real
  size), play, close.
- Tiling mode (T) is hypr3d's own layout: Hyprland's tiling doesn't change.
- X11 apps work through XWayland, with their menus and tooltips drawn as popups. Drag and drop works, and so do
  input method popups (fcitx5).

### Two monitors

3D goes on the focused monitor, or the one `monitor` names. The others stay your normal desktop. Super+Esc, a
keybind that moves the focus to another monitor, or moving the mouse across hands the mouse and keyboard over; the
3D view keeps running. Coming back works the same way. For your own keybinds there's `hyprctl hypr3d away`, the
`hypr3d:away` dispatcher and `hl.plugin.hypr3d.away()`.

### Lip sync

Off until you turn it on: `lipsync = true`, `hyprctl hypr3d avatar lipsync on`, or the Action Menu's options.
While you're in 3D with an avatar it listens to your default microphone (or `lipsync_source`) through PipeWire, and a
red badge shows it's listening. Nothing is recorded or sent. It shows five vowels (a, i, u, e, o) and the consonants
pp, ff, ss and ch where the avatar has those visemes. The gain is automatic (`lipsync_gain` sets a fixed one), and
the badge tells you when the microphone is muted, silent or missing. `hyprctl hypr3d avatar lipsync` reports the
details.

## Config

All values are `plugin:hypr3d:…`. Paths may start with `~/`. A config reload applies changes at once; a value
changed at run time (`hyprctl keyword`) takes effect within a second.

| Value | Default | |
|---|---|---|
| `layer_spacing` | `0.02` | metres between stacked layers (bars, notifications) on the desktop wall, 0–0.5 |
| `wallpaper` | `false` | keep the wallpaper on the desktop wall |
| `map` | `""` | a glTF/GLB map instead of the courtyard |
| `map_scale` | `0` | metres per map unit; 0 guesses |
| `avatar` | `""` | a glTF, GLB or VRM avatar |
| `avatar_height` | `0` | scale the avatar to this height in metres; 0 keeps its own |
| `avatar_physics` | `true` | spring bones (hair, skirts) |
| `first_person_body` | `true` | first person from the avatar's eyes with its body and hands; `false`: from 1.65 m, no body |
| `avatar_emotes` | `""` | more emotes: `.vrma` or glTF clips, files or folders, comma-separated |
| `emote_volume` | `0.5` | volume of emotes' sounds, 0–1 |
| `lipsync` | `false` | lip sync from the microphone |
| `lipsync_gain` | `auto` | microphone gain in dB (−20 to 60), or `auto` |
| `lipsync_source` | `""` | the microphone, by name or description as `wpctl status` lists it; `""` = default |
| `apps` | `""` | the Apps page's favourites: desktop ids, app names or commands, comma-separated |
| `app_rules` | `""` | where launched apps open (see Windows in the world), comma-separated |
| `pin_size` | `0.3` | share of the view's height a pinned window takes, 0.05–1 |
| `monitor` | `""` | the monitor for 3D: its name (`DP-1`) or `desc:` and the start of its description; `""` = focused |
| `walk_speed` | `1.6` | m/s, 0.3–10 |
| `run_speed` | `4.5` | m/s, 0.5–15 |
| `tiling` | `false` | start in tiling mode |
| `tiling_follow` | `true` | the tiling ring follows you; `false`: it stays where you turned it on |
| `play_view` | `here` | what P does: `here` (play in place) or `fill` (fill the view); Shift+P does the other |
| `play_size` | `0.5` | share of the view a window played in the tiling ring takes, 0.25–0.94 |

## hyprctl

`hyprctl hypr3d` with no arguments prints the state as JSON.

| Command | |
|---|---|
| `status`, `toggle`, `on [MONITOR]`, `off [now]` | the state; enter or leave 3D |
| `away [on\|off\|toggle]` | hand the mouse and keyboard to another monitor, or take them back |
| `type [on\|off]` | type into the window under the crosshair |
| `play [on\|off\|toggle] [here\|fill]` | play mode; without arguments, what's played |
| `launch WHAT`, `apps` | launch an app into the world; list the desktop entries |
| `window SEL focus\|bring\|wall\|pin\|unpin\|bigger\|smaller\|size W H\|play\|close` | act on a window by address, class or title |
| `tile [on\|off\|toggle\|here\|follow [on\|off\|toggle]]` | tiling mode (T, Shift+T, Y) |
| `grab`, `place`, `hold DIST [SCALE]`, `pin`, `reset-windows [forget]` | carry, place and pin windows; put them all back on the wall |
| `windows`, `panels`, `camera`, `log [LINES]` | windows in the world, everything drawn, the camera, the plugin's recent log |
| `look DX DY`, `turn YAW PITCH`, `tp X Y Z`, `aim [WINDOW]` | move the view |
| `walk SECS [forward\|back\|left\|right]`, `jump`, `fly`, `click [left\|right\|middle]`, `sens [VALUE]` | input from a script |
| `map [PATH\|none\|reload\|forget\|scale S]`, `spawn [here]`, `desktop [here [HEIGHT]]` | the map, its start and where the desktop hangs |
| `view [first\|third\|toggle] [DIST] [SIDE]`, `view body [on\|off\|toggle]` | the camera |
| `avatar [PATH\|none\|reload\|height M]` | load an avatar, or print its state |
| `avatar expression [NAME [WEIGHT]\|none]`, `avatar gesture [left\|right\|both GESTURE]` | faces and hand gestures |
| `avatar parts [reset]`, `avatar toggle NAME [on\|off\|reset]`, `avatar slider NAME [VALUE\|NN%\|reset]`, `avatar slider NAME X Y`, `avatar shape KEY [WEIGHT\|reset]` | the outfit |
| `avatar physics [on\|off\|toggle]` | spring bones |
| `avatar lipsync [on\|off\|toggle\|gain DB\|auto\|source NAME\|default]` | lip sync; without arguments, what it hears |
| `avatar emote [NAME\|NUMBER\|FILE\|FOLDER [once\|loop]\|stop]`, `avatar attack [left\|right]` | emotes and punches |
| `menu [open [PAGE]\|close\|toggle\|back\|pick [N]\|move DX DY\|scroll N]` | the Action Menu |

Dispatchers: `hypr3d:toggle`, `hypr3d:menu [PAGE]`, `hypr3d:play [here|fill|on|off|toggle]`, `hypr3d:away` and
`hypr3d:tile [here|follow]`. Lua functions: `hl.plugin.hypr3d.toggle()`, `enter()`, `exit()`, `type()`,
`play([VIEW])`, `away()`, `tile(["here"|"follow"])` and `menu([PAGE])`.

## Avatars

`plugin:hypr3d:avatar`, `hyprctl hypr3d avatar FILE` or the Action Menu's Avatars page (it lists
`~/.local/share/hypr3d/avatars/`) loads:

- **VRM 0.x and 1.0**: the humanoid, expressions, look-at, spring bones (with `VRMC_springBone_extended_collider`
  and `VRMC_springBone_limit`), node constraints, and MToon's shading, matcaps and outlines.
- **A plain glTF/GLB**: humanoid bones are guessed from their names (Mixamo, VRoid, Blender and the like),
  expressions from shape key names (VRoid, VRChat, MMD, ARKit) and spring bones from bone names (hair, skirt,
  tail…). Clips named idle, walk, run, jump or fall are used for those.
- **What `tools/unity2hypr3d.py` writes**: a GLB and a settings file.

The avatar blinks, looks where you look, walks, runs, jumps, crouches and flies. Without clips of its own, a humanoid
walks procedurally: its feet stay planted, stride and cadence follow human gait data, and it handles stairs, slopes
and turns. Its upper body follows built-in walk and run clips (`assets/*.vrma`, made with
`tools/blender/h3d_walk.py`). Spring bones step at 60 Hz and are interpolated between steps, so they stay smooth on
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

`AVATAR.hypr3d.json` next to the avatar adds to what the model says, or overrides it. The converter writes one, and
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

The converter also writes glTF extras that hypr3d reads: `hypr3d_part` on primitives, and `hypr3d_queue`,
`hypr3d_stencil`, `hypr3d_outline`, `hypr3d_back`, `hypr3d_light`, `hypr3d_toon` and `hypr3d_matcap` on materials.

## Maps

`plugin:hypr3d:map`, `hyprctl hypr3d map FILE` or the Action Menu's Maps page (it lists `~/.local/share/hypr3d/maps/`)
loads a glTF/GLB. A node named `hypr3d_spawn` marks the start (facing −Z), `hypr3d_desktop` where the desktop hangs
(facing +Z), and nodes under `hypr3d_backdrop` are scenery without collision. Without them hypr3d uses a game's
`info_player_*` start and finds a flat wall itself. A directional light becomes the sun. `spawn here` and
`desktop here` are saved in `$XDG_STATE_HOME/hypr3d/maps/`.

## Tools

The converters need [Blender](https://www.blender.org) (5.2 was used) and re-run themselves inside it.

**`tools/unity2hypr3d.py`** converts a VRChat avatar into a GLB and a settings file, without Unity:

```sh
python3 tools/unity2hypr3d.py Avatar.unitypackage [Outfit.unitypackage…] -o ~/avatars/me.glb
python3 tools/unity2hypr3d.py Avatar.zip --outfit "Some Dress" --emote Dance.zip -o me.glb
python3 tools/unity2hypr3d.py ~/UnityProjects/MyAvatar --list
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
python3 tools/cs2map.py de_mirage        # → ~/.local/share/hypr3d/maps/de_mirage.glb
```

It keeps the 3D skybox, CS2's baked lighting (lightmaps, light probes, the sun, fog, exposure and tone curve) and the
materials' details (tints, decals, blended layers). The maps are Valve's: this reads your copy of the game, for your
own use.

**`tools/blend2vrma.py`** writes a humanoid animation from Blender as a VRM animation (`.vrma`) that plays on any
humanoid: an emote, an attack or a walk cycle. `tools/blender/` has the scripts the built-in punches and walk were
made with (see its README).

```sh
blender -b FILE.blend --python tools/blend2vrma.py -- OUT.vrma --humanoid AVATAR.hypr3d.json [--frames A B] [--bones upper]
```

## Tests

Everything is in `tools/test`. Each script's header (or `--help`) explains it.

- `harness/`: `shot`, an offscreen renderer that drives the plugin's own renderer, animator and Action Menu
  (`tools/test/harness/build.sh` builds `build/test/shot`; `grep 'a == "--' tools/test/harness/shot.cpp` lists
  its options). The checks built on it: `ctl_check.sh`, `toon_check.sh`, `fp_check.sh`, `attack_check.sh`,
  `spring_check.sh`, `disc_check.sh`, `cs2mat_check.sh` and `lipsync_check.sh`.
- `vm/run.sh OUTDIR [--only ITEMS] [--gpu virgl]`: the plugin in a real Hyprland in NixOS VMs (needs Nix). About
  950 checks in about an hour, driven by real input devices, with PipeWire, XWayland and real apps (Chromium,
  Firefox, Electron, OBS, fcitx5, Chocolate Doom, SuperTux).
- `live/check.sh OUTDIR [--mic] [--app CMD]…`: a guided check on your own desktop, GPU, microphone and apps.
- `tiling/run.sh` and `sound/sound_check.sh`: the tiling ring and emote sound playback on their own.
- `fuzz/fuzz.py map|avatar OUTDIR`: fuzzes the map and avatar loaders under AddressSanitizer and UBSan.
- `synth/`: synthetic Unity projects and Booth-style packages (`make.py`, `booth.py`) and the converter's unit tests
  (`*_unit.py`). `regress.sh` compares the working copy's converter with HEAD's.

Scripts that use numpy run under Blender's Python, e.g.
`blender -b --factory-startup --python-exit-code 1 -P tools/test/synth/ma_unit.py`.

## Known limits

- Only keys held with Super or Ctrl+Alt reach Hyprland in 3D, so Alt+Tab doesn't, and a game never gets Super
  combinations.
- Direct scanout is off while 3D is up, on every monitor. A fullscreen game is composited into the 3D view, with up to
  a frame of latency; `hyprctl hypr3d status` shows `fps`, `updateMs` and `renderMs`.
- With Hyprland 0.56, windows and layers that close vanish in 3D without fading out.
- A dragged item's icon isn't drawn in 3D (the cursor shows "grabbing" instead).
- Changing a window's real size makes a tiled window floating. Window placement is remembered per class, so an app
  with several windows of one class is put back only when it has one.
- The tiling ring is one row: when crowded, its windows shrink instead of wrapping.
- Lip sync knows five Japanese vowels and four consonants. Other voices and languages can pick the wrong shape, and
  the automatic gain also hears other voices near you (a fixed `lipsync_gain` is steadier).
- Emote sounds are Ogg Vorbis only, come only from the settings file, and aren't positioned in the world.
- First person's hands are posed the same way for every avatar. The punches were made on Hatsune Miku NT and land a
  little differently on other builds.
- The built-in emotes are procedural look-alikes, since VRChat's own are proprietary. Load real ones as `.vrma` or
  glTF clips.
- The converter handles the Modular Avatar and VRCFury features avatars use most, not all of them. PhysBone curves
  and Gravity Falloff aren't carried over, and toon shading uses only the first shade step. It was tested on
  synthetic packages and free Booth items, not on paid avatars.
- Hyprland 0.55.x crashes when it quits with windows open, with or without hypr3d (fixed for dwindle in 0.56.0). A
  patch is in `extras/hyprland-exit-crash`. hypr3d works around aquamarine's headless-output bug (fixed in 0.12.1).

## Credits

- [cgltf](https://github.com/jkuhlmann/cgltf) (MIT) and [stb_image, stb_dxt and stb_vorbis](https://github.com/nothings/stb)
  (public domain or MIT), vendored in `src/third_party/`.
- The bone-name table in `tools/unity2hypr3d.py` is from [Modular Avatar](https://github.com/bdunderscore/modular-avatar)
  (MIT, © 2022 bd_), which took it from HhotateA's AvatarModifyTools (MIT, © 2021 @HhotateA_xR) and Azukimochi's
  BoneRenamer (MIT, © 2023 Azukimochi). The rest of the converter's Modular Avatar support reimplements MA's
  behaviour in Python, written from reading MA's source.
- The humanoid muscle maths follow lox9973's [ShaderMotion](https://gitlab.com/lox9973/ShaderMotion) (MIT,
  © 2020-2021 lox9973) and [uvw.js](https://gitlab.com/lox9973/uvw.js) (Apache 2.0, © 2022-2023 lox9973).
- The converter knows UnlitWF's and lilToon's shaders by their GUIDs (from
  [Unlit_WF_ShaderSuite](https://github.com/whiteflare/Unlit_WF_ShaderSuite), zlib, and
  [lilToon](https://github.com/lilxyzw/lilToon), MIT, © 2020-2024 lilxyzw). Their settings, and
  [Poiyomi Toon](https://github.com/poiyomi/PoiyomiToonShader)'s (MIT), were read from those shaders' sources. None of
  their code is in this repo.
- The VRCFury support reimplements [VRCFury](https://github.com/VRCFury/VRCFury)'s build behaviour (© 2022 Senky,
  under its own license), written from reading its source. It contains none of VRCFury's code.
- The vowel recordings in `extras/speech` are from Wikimedia Commons, Lingua Libre and Tofugu/WaniKani (public
  domain, CC0, CC BY and CC BY-SA 4.0), and the formant data in `extras/speech/ref` is Kakeru Yazawa's (Zenodo
  15227304, CC BY 4.0); `extras/speech/LICENSES.md` credits each. The patches in `extras/` change Hyprland's and
  aquamarine's code (BSD 3-Clause).
- License texts are in [THIRD_PARTY.md](THIRD_PARTY.md). VRChat, Modular Avatar, Counter-Strike 2 and Blender belong
  to their owners. This project has no connection with any of them and ships none of their assets.
