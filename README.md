# hypr3d

A [Hyprland](https://hyprland.org) plugin that turns the desktop into a place you can walk around in.
While 3D mode is on, your windows hang on a wall in a small courtyard, or in any glTF map, and you
walk up to them in first person. The crosshair clicks, scrolls and types into whatever it points at.
You can take a window off the wall and put it anywhere, pin one to your view, and launch apps
straight into the world. Games are played there too: play mode gives a window the keyboard, the
mouse (a locked pointer's relative motion, as games want it) and the buttons, where it is among your
other windows, or, with Shift+P, filling the view: the camera turns to face it, you as you were.

With an avatar loaded, first person is from its eyes: its hands are up in front of you as in a first
person game, doing what you do (a finger on the window you click, tapping as you type, reaching out to a
window you carry), and looking down you see its body. V switches to third person. The avatar works much
like one in VRChat: it has faces, hand gestures, emotes and dances, a radial Action Menu, outfit toggles
and sliders, hair and clothes that swing, toon outlines, and lip sync from your microphone if you turn it
on. A left click with the crosshair on no window has it throw a punch at what's ahead (a hook animated in
Blender), one arm and then the other as you go on clicking. It can be a VRM, a plain GLB, or a VRChat
avatar converted with `tools/unity2hypr3d.py`.

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

Hyprland's build shell has no audio library, so for lip sync's microphone `build.sh` does the same
for the PipeWire that runs: it finds its binary, builds its derivation's `dev` output and adds that
to `pkg-config`'s path. The plugin then links `libpipewire-0.3` from that PipeWire. Without it (no
PipeWire running, or its derivation gone), hypr3d builds without a microphone, and lip sync says so
when you turn it on. The `dev` output may not be in the binary cache; then Nix builds PipeWire once.

On other distributions, `make` should work anywhere `pkg-config` finds the headers of the Hyprland
you run (`hyprland`, `pixman-1`, `libdrm`, `glesv2`, `egl`, `cairo` and `pangocairo`, and
`libpipewire-0.3` for the microphone if it is there). This is untested.

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
| `plugin:hypr3d:first_person_body` | `true` | first person from the avatar's eyes, its body below and its hands in view, doing what you do (below); `false`: from 1.65 m up, only its shadow, as before |
| `plugin:hypr3d:avatar_emotes` | `""` | more emotes: VRM animations (`.vrma`) or glTF clips; files or folders, separated by commas |
| `plugin:hypr3d:emote_volume` | `0.5` | how loud an emote's sound (a dance's song, the settings file's `"sound"`) plays, 0–1: 1 as loud as the file is, 0.5 half that (6 dB down), 0 not at all (see Avatars) |
| `plugin:hypr3d:lipsync` | `false` | lip sync: the microphone moves the avatar's mouth while you are in 3D (below) |
| `plugin:hypr3d:lipsync_gain` | `auto` | lip sync: how much louder the microphone counts, in dB (−20 to 60); `auto` goes by your voice (below) |
| `plugin:hypr3d:lipsync_source` | `""` | lip sync: the microphone, by its name or its description as `wpctl status` lists it; `""` is the default one |
| `plugin:hypr3d:apps` | `""` | the Action Menu's Apps page: desktop ids, app names or commands, separated by commas (below) |
| `plugin:hypr3d:app_rules` | `""` | where apps launched from 3D open: `CLASS: DISTANCE [HEIGHT\|auto] [left\|right\|SIDE]`, separated by commas (below) |
| `plugin:hypr3d:pin_size` | `0.3` | how much of the view's height a window pinned to it takes, 0.05–1 |
| `plugin:hypr3d:monitor` | `""` | the monitor 3D goes on: its name as `hyprctl monitors` lists it (`DP-1`), or `desc:` and the start of its description; `""` is the focused one. The others stay your desktop (below) |
| `plugin:hypr3d:walk_speed` | `1.6` | how fast you walk (W), m/s, 0.3–10: an easy walk |
| `plugin:hypr3d:run_speed` | `4.5` | how fast you run (Shift+W), m/s, 0.5–15 |
| `plugin:hypr3d:tiling` | `false` | tiling mode (T) from the start: your windows side by side round you, going with you or staying where it's turned on (`tiling_follow`, below) |
| `plugin:hypr3d:tiling_follow` | `true` | tiling mode's row goes with you; `false`: it stays where you turn it on, and you walk up to it (Y switches, below) |
| `plugin:hypr3d:play_view` | `here` | how P plays a window: `here`, where it is, the view as it was and your other windows round it; `fill`, the camera turning to face it, filling the view. Shift+P plays in the other (see Play mode). Another value plays here, and a notification says so |
| `plugin:hypr3d:play_size` | `0.5` | how much of the view a window played here in tiling mode's ring takes, 0.25–0.94 of its height or width (whichever it fills first; bigger than on your screen too, first person or third, as the view sees it), centred where you look, drawn over the world: the windows beside it show round it, and when the ring is crowded they get smaller to make room for it, not it. Super+wheel changes it while you play, till the plugin loads again or this changes (see Play mode). A value out of range is taken as the nearest it can be (not a number: 0.5), and a notification says so |

Paths may start with `~/`. Maps and avatars load in the background, and a failure shows up as a
notification. A config reload applies changed values at once; a value changed at run time without one
(`hyprctl keyword plugin:hypr3d:…`, or `hl.config()` through `hyprctl eval`) takes effect within a
second.

## Controls

Enter and leave 3D with the `hypr3d:toggle` dispatcher, `hyprctl hypr3d toggle` or
`hl.plugin.hypr3d.toggle()`. 3D goes on the focused monitor, or on the one `plugin:hypr3d:monitor`
names (see Two monitors, below). Keys held with Super, or with Ctrl+Alt, still go to Hyprland, so
your compositor shortcuts keep working in 3D, and a launcher one opens comes over the 3D view (below).
Walking, a shortcut is for the window the crosshair is on (or the one you carry), as on the 2D
desktop it's for the one under the mouse: Hyprland's keyboard focus goes there before the shortcut
runs, so Super+Q (`hl.dsp.window.close()`, "the focused window") closes the window you look at, and
nothing when you look at no window, never one out of sight that happened to have the focus. It's for
no window too when the one you look at is from a workspace that isn't shown (out in the world from
another: focusing it would switch the monitor to that workspace) or is under a special workspace
that's open (a scratchpad: focusing it would close the scratchpad, so the scratchpad's own toggle
opened it again; not with `input:special_fallthrough`), or won't take the keyboard (a `no_focus`
rule, a modal dialog of its open), and while the Action Menu is open. One aimed at an X11 menu or
tooltip is for the window it belongs to. One under a window fullscreen or maximized on its workspace
gets the keyboard without taking that one out of fullscreen, and when it closes, the keyboard goes to
no window (the next one Hyprland would give it to would take that one out of fullscreen). Right after
the window a shortcut went to closes (Super+Q; Alt+F4 in a game; however long it takes to close, up
to half a minute), with the keyboard, a shortcut is for no window till you turn or move (or 5 seconds
go by): Super+Q pressed again, for a window slow to close or a game you played, doesn't close the one
that slides under the crosshair as tiling mode's row closes up. A window that closes by itself (a
popup, a splash) holds nothing back. `hyprctl hypr3d log` says where a shortcut moved the focus ("a
shortcut: the keyboard to …").

| Input | In 3D |
|---|---|
| mouse, arrow keys | look around |
| W A S D | walk (1.6 m/s, `walk_speed`); Left Shift runs (4.5 m/s, `run_speed`). You walk up and down stairs and ledges up to half a metre, and the view goes up and down them smoothly, not a step at a time |
| Space | jump (flying: up) |
| Left Ctrl or C | crouch (flying: down) |
| F | fly on/off (going on as you were going: into the air at your speed, and out of it falling on ahead) |
| R | back to the start |
| left/right/middle click | click whatever the crosshair points at; with it on no window, a left click attacks: the avatar swings an arm at what's ahead, a punch (see Attacks) |
| wheel | scroll it; in third person, pointing at nothing, zoom the camera |
| E or Enter | type into the window under the crosshair (every key is its, W A S D and P too, and a notification says so); Super+Esc goes back to walking, and so does the window closing or another window taking the keyboard |
| P | play the window under the crosshair: it gets every key, the buttons, the wheel and the mouse, where it is, your view staying as it was (below); Super+Esc stops. After play mode ended by itself, P plays the game again |
| Shift+P | the same, filling the view: the camera turns to face the window (with `play_view = fill`, P and Shift+P swap) |
| Super+wheel | while you play a window here: bigger (up) or smaller. In tiling mode's ring 5% of the view a notch, from 25% up to 94% (it starts at `play_size`, half the view), first person or third, the row turning so it stays in the middle of your view; out in the world 5% a notch where it hangs. The game doesn't get it (below) |
| G or H | pick up the window under the crosshair: it goes where you look (flat on a wall within its reach, else in the air in front of you); G or H again, or a left click, puts it down there, a right click or Esc puts it back where it was; the wheel makes it bigger (up) or smaller, 5% a notch, and it stays that big wherever you put it; Ctrl+wheel moves it nearer (down) or further; Shift+wheel changes its real size (the app draws itself anew). In third person the avatar carries it: it stays out past the avatar, never between the camera and the avatar |
| X | send a window you've placed back to the wall (and forget the place its app had) |
| Shift+H | pin the window under the crosshair to your view (top right, over the world), or the one you carry; Shift+H again takes it back into your hands, as big as it was, to put down where you look |
| T | tiling mode: every window in the world and on the desktop wall side by side in a ring round you, each facing you, as big as on your screen (made smaller to fit your view), going with you wherever you walk or fly (or, with Y, staying where it is); T again puts them back where they were. Shift+T brings the ring round you, the row's middle where you look (below) |
| Y | tiling mode's row goes with you, or stays where it is: you walk up to your windows and away from them (below) |
| Q | the Action Menu's Apps page: launch an app into the world |
| B | the Action Menu's Windows page: every window, and what to do with it |
| V | first / third person (needs an avatar) |
| Tab | the Action Menu |
| F1–F8 | hand gestures (Neutral, Fist, Open, Point, Victory, Rock'n'roll, Handgun, Thumbs up), as in VRChat's desktop mode: with Left Shift held only the left hand, with Right Shift only the right, otherwise both |
| Esc | leave 3D |
| Super+Esc | with another monitor: the mouse and keyboard to it, the 3D view staying up; Super+Esc there comes back (below). With a game's keys held back (play mode ended by itself, below): walking again |

The Action Menu has pages for emotes, expressions, gestures, the outfit, apps, windows, options
(view, physics, fly, lip sync, mic gain, respawn, reset face, stop emote), maps (see Maps) and avatars
(see The Avatars page). With it open, the mouse moves its cursor, a left click picks, a right click
goes back and a middle click closes it. The wheel goes round it, 1–9 pick an item (the main page has
nine, Avatars the ninth; the others up to eight), Enter picks, Backspace goes back and Esc closes it.
WASD still walks.

A slider on the outfit page (🎚️, VRChat's radial puppet) opens a dial. The cursor sets it by going
round from the top, clockwise from 0% to 100%. It stops at the ends rather than jump across the top.
The wheel moves it in 5% steps, and 1–8 set 0%, 14%, … 100%. A click, Backspace or a right click
closes the dial.

A two-axis puppet (🕹️, VRChat's and VRCFury's two- and four-axis puppets) opens a stick instead:
where the cursor is in the disc sets x and y, −1 to 1 each, right and up positive, and 1–8 push it
all the way in the eight directions, from up clockwise.

**A launcher, a clipboard picker or your shell's menu**, anything a shortcut opens that takes the
keyboard (a layer surface: your Super+D launcher, rofi, fuzzel, wofi, a power menu), comes over the 3D
view when it opens on the 3D monitor: it's drawn as on the 2D desktop, where you see it, not on the
desktop wall. While it's there it has every key (Esc too: its own Esc closes it, not 3D), the wheel,
and a pointer of its own that starts where the crosshair was; the mouse moves that over it as over the
monitor, with its cursor, and clicks where it is. When it lets go of the keyboard (it closed, or you
picked something) you walk again, and an app you started opens in front of you. The status's
`"shell"` says which one is over the view.

**Lip sync** is off until you turn it on: with `lipsync = true` in the config,
`hyprctl hypr3d avatar lipsync on`, or the Action Menu's options. Then, while you are in 3D with an
avatar, hypr3d listens to your default microphone through PipeWire (as the "hypr3d lip sync"
stream), and a red "lip sync: listening" badge sits in the top right corner, below Hyprland's
notifications while any show. How loud you are opens the mouth, and the vowel you make (its first
two formants, from linear prediction; a window counts as voiced when it's periodic, or when its
formants stand out) picks the shape: the avatar's aa, ih, ou, ee and oh visemes. Consonants that
show use the avatar's own consonant visemes where it has them (VRChat's `vrc.v_pp`, `vrc.v_ff`,
`vrc.v_ss` and `vrc.v_ch`, or the settings file's `visemes`): an s, z or ts (and Japanese し and ち)
shows ss, a rounder sh ch, a flat, faint f ff, and a low murmur well under the voice (an m, or the
voice trailing off) pp. Otherwise a consonant keeps the last vowel's shape, less open, for a moment.
Hiss or noise that goes on shuts the mouth. It keeps a quarter of a second of sound at most, to look
at, and nothing is written anywhere or sent. Leaving 3D, or turning it off, closes the microphone.

How loud is loud depends on the microphone, so lip sync's gain is automatic: the loud part of your
voice over the last 15 seconds (the 90th percentile of its voiced moments) is brought to where a
microphone set up for speech has it (−12 dBFS), up to 50 dB louder, never quieter. A microphone 40 dB
quieter than that opens the mouth as wide once you've said a syllable, while a whisper right after
your normal voice stays shut. It goes by the room too: the noise in your pauses (half a second with
no voice in it) keeps the mouth shut, as does anything under −80 dBFS, and noise that looks voiced
(a hum, a rumble) doesn't count as your voice. A fixed gain instead: `lipsync_gain = 24` (dB), `hyprctl
hypr3d avatar lipsync gain 24` or the Mic gain dial in the Action Menu's options (at its start
automatic, then round to 60 dB); `auto` goes back.

The badge says what the microphone gives: "listening (Yeti X …)" with the one it's linked to, "no
sound from … (muted?)" when it has sent only exact zeros for 2 seconds (a microphone muted by its own
button does that, where PipeWire can't see it), "… is muted" when PipeWire has it muted, "no
microphone linked", or "no … (listening to …)" when the one `lipsync_source` names isn't there and
WirePlumber gave it the default one. The first of these each time it starts listening comes as a
notification too, with what to do about it. `hyprctl hypr3d avatar lipsync` has it all: the source
and its state, mute and volume, the links, how much came and how long it's been exact zeros, the last
second's peak and RMS, the gain, your voice's level, the room's, and the sources there are.

## Two monitors

3D goes on one monitor, and the others stay your desktop, drawn by Hyprland as always: your bar, your
windows, notifications. It's the focused monitor, unless `plugin:hypr3d:monitor` names one (or
`hyprctl hypr3d on MONITOR` does); for 3D always on the right one:

```lua
hl.config({ plugin = { hypr3d = { monitor = "DP-1" } } })
```

Entering 3D brings the mouse and keyboard to it, and the cursor if it was on another monitor. They
go to your desktop and back while the 3D view stays up and goes on (your avatar, its emotes and lip
sync, the windows out in the world):

- **Super+Esc**, walking: the cursor goes back where it was on the other monitor (else to the middle
  of the one nearest), and the focus with it. Super+Esc there comes back into 3D.
- a keybind that moves the focus to another monitor (Hyprland's `movefocus` or `focusmonitor`, as
  `hl.dsp.focus({ direction = "left" })` has it) goes there, and one that moves it to the 3D monitor
  comes back
- the mouse moved onto the 3D monitor comes back: the cursor stays where it came in, and the mouse
  turns the camera again
- `hyprctl hypr3d away [on|off|toggle]`, the `hypr3d:away` dispatcher and `hl.plugin.hypr3d.away()`
  (both toggle), for a keybind of your own

While the mouse is away, the other monitors work as on the 2D desktop: the cursor, clicks, the wheel
and every key (Esc too: it doesn't leave 3D there), and notifications show there. In 3D the crosshair
goes and you stand still; typing, play mode and a window you carried end. Opening the Action Menu or
playing a window from a keybind of yours (`hypr3d:menu`, `hypr3d:play` or their Lua functions) comes
back into 3D first, and your 3D keybind leaves 3D as ever. The cursor is never on the 3D monitor while it's away: going there comes
back. Coming into 3D takes the keyboard focus from a window on another monitor, so that a Super
shortcut (closing a window, say) doesn't act on it unseen; clicking, typing into or playing a window
in 3D gives it the focus.

## Apps and games in 3D

### Play mode

Point at a window and press P (or Shift+P, `hyprctl hypr3d play on`, the `hypr3d:play` dispatcher,
`hl.plugin.hypr3d.play()`, or Play on the Windows page). The window then gets everything, as a game
wants it:

- the keyboard focus (xdg_toplevel's "activated" state, and the focus SDL needs to read a controller)
  and every key, Esc and Tab included; keys held with Super or Ctrl+Alt still go to Hyprland
- the buttons and the wheel
- the mouse as the app asks for it: while it locks the pointer (`zwp_locked_pointer_v1`, SDL's
  relative mouse mode), only relative motion (`zwp_relative_pointer_v1`), as Hyprland gives it on the
  2D desktop; while it confines it, a pointer kept in its region; otherwise a pointer that moves over
  the window and its popups as over a monitor, with the app's own cursor drawn where it is. A tablet
  covers the window.

P plays the window where it is: your view stays as it was, first or third person, with your other
windows round it (in tiling mode, its neighbours in the ring), and it doesn't follow the window if
that moves (the ring making room for a window that opens, say). A window played that isn't in your
view (Play on the Windows page, or one going fullscreen, behind you or off to the side) turns you to
face it first, at once, where it is. **Shift+P** plays it filling the view instead: the camera leaves
you and turns to face the window, filling most of the view (94% of the way it fits) and following it
if it moves. `plugin:hypr3d:play_view = fill` makes that P's (and a window played by itself, going
fullscreen), and Shift+P then plays it where it is; the notification that says you're playing says
which key gives the other view next time. `hyprctl hypr3d play here` or `play fill` switches while
you play, the camera going to the window or back to you (turned to face it, if it isn't in your
view), and so do the `hypr3d:play` dispatcher and `hl.plugin.hypr3d.play()` with the same word.

In tiling mode's ring a window played here takes half the view (`plugin:hypr3d:play_size`): half its
height or its width, whichever the window fills first, however big it is on your screen (a small game
is made bigger), first person or third (where the ring's other windows stand on the ground and take
at most 60% of the view, it doesn't: it's drawn over the world while you play), as the view sees it:
from the third-person camera where it is (brought in by a wall behind you), from where you stand in a
ring that stays (Y), looking up or down at it. The row turns so it's in the middle of your view, and
it goes up or down to where you look (look a little up or down at it as you press P, and its middle
is there; your view doesn't move), so the windows beside it show round it. A window opening or
closing in the row meanwhile doesn't move it: the row turns round it. When the ring is crowded, the
others get smaller to make room for it, not it. Played away from a ring that stays, it's as the ring
has it. **Super+wheel** while you play makes it bigger (up) or smaller, 5% of the view a notch, from
25% up to 94%, the windows beside it moving round with it and the row turning so it stays in the
middle; a notification says what it takes when you stop turning the wheel, and when that's the most
or the least. The size you choose stays for every window you play till the plugin loads again or
`play_size` changes (a `play_size` set a moment before you press P is used). When its play ends by
itself (a popup or a launcher takes the keyboard: the game's keys held back, below), it keeps its
size and its place till that's over. Filling the view, and when play mode ends, it's as big as the
ring's other windows again, where they are, and the row stays turned where it is (Shift+T brings its
middle to where you look, as ever). A window played where it hangs out in the world gets 5% bigger or
smaller a notch there, as one you carry does with the wheel; one on the desktop wall or pinned to the
view stays as it is. The game never gets Super+wheel (nor do Hyprland's binds); the wheel alone is
its, as ever.

Either way the crosshair, the aimed window's outline and the Action Menu go (the lip sync badge stays
while the microphone listens), and the window, its popups and dialogs are drawn over the world and
the avatar, so nothing gets in front of them. Windows pinned to the view stay over it; one of them
played (Play on the Windows page) is played in its corner, and filling the view, the camera goes to
it there. You don't walk while playing; your avatar stays where you were.

Play mode ends with **Super+Esc** (the app doesn't get that Esc), when you leave 3D, when the screen
locks, when the window closes or leaves the 3D view (its workspace hidden), when another window takes
the keyboard, and when a layer surface does (a launcher, a lock screen). A notification says how to
stop when it starts. The window's dialogs are played with it: a file chooser (the portal's, or the
app's own) opens over it, as on the 2D desktop, and has the keyboard; the pointer goes over it, and
when it closes you're back in the window, still playing. An X11 menu of the app's own can take the
keyboard too. Pointing at a dialog and pressing P plays the window it belongs to (a fullscreen dialog
is played itself), and a dialog or menu of the window's that has the keyboard keeps it.

A window that goes fullscreen in 3D (a browser's video, a game), on the 3D monitor's workspace, is
played by itself as soon as it has the keyboard, or a dialog of its own has it (the dialog keeps it):
at once when it has it, when it opens fullscreen too, and later when the keyboard comes back to it (a
window over it that had it closed, or you clicked it). When another window of the game's had the
keyboard as it went fullscreen (Helldivers 2 through Proton does that), Hyprland gives the keyboard
to no window (one under a fullscreen window can't have it): the plugin gives it to the game then, and
only then (not when no window has it later, as when you come back into 3D from another monitor). Play
mode that ended by itself (another window took the keyboard, a launcher came over the view) starts
again the same way, in the view it was played in. A dialog that goes fullscreen is played itself, not
the window it belongs to. Not while you type into another window, carry one, have the Action Menu
open or the mouse is away, nor right after the window a shortcut went to closed, with the keyboard,
till you turn or move (or 5 seconds go by: Super+Q pressed twice on a window over the game doesn't
close the game); and not a window whose play you ended with Super+Esc (or `hyprctl hypr3d play off`,
the dispatcher, the Lua function) while it was fullscreen, nor one that was fullscreen already when
you came into 3D, until it's fullscreen again (P plays it meanwhile). Leaving fullscreen doesn't end
play mode: the app keeps the keys, as on the 2D desktop (Esc out of a video's fullscreen, a game
switched to windowed), and Super+Esc walks. One that maximizes isn't played.

On the 2D desktop a fullscreen window hides the other windows of its workspace and the bar on the
top layer (a maximized one the windows). In 3D it does that only on the desktop wall, while it's on
the wall itself: one out in the world (opened in front of you, or in tiling mode's row) hides
nothing, and the windows out in the world are never under one, so a game going fullscreen leaves
every other window round you where it was, drawn and getting its frames. Clicking one of them, or
typing into it, gives it the keyboard as Hyprland does on the 2D desktop: a tiled one takes the
fullscreen window out of fullscreen (`misc:on_focus_under_fullscreen`), a floating one comes over it.

Controllers are read by the games themselves, from `/dev/input` (SDL through udev), not through
Hyprland; SDL drops a controller's events while its window doesn't have the keyboard focus. In play
mode it has it. Walking, a game still reads its controller for as long as it has the keyboard focus
(you clicked it last, or typed into it); once another window has it, SDL ignores the controller.

What a game gets from the compositor in 3D: the "activated" state while it has the keyboard,
`wl_output` enter and the monitor's scale as on the desktop, frame callbacks, presentation feedback
and FIFO barriers at the pace of the 3D view (Hyprland reports what the 3D view covers as
"discarded": the plugin reports what it draws as presented instead), and its idle inhibitor keeps the
screen on even when its workspace is hidden. Direct scanout is off in 3D (see Performance).

A game that locks the pointer keeps the keyboard focus when a new window opens (Hyprland's rule, on
the 2D desktop too): click the new window, or leave play mode and point at it and press E.

**When play mode ends by itself** (the game quit and its window closed, it left the 3D view, another
window or a layer surface took the keyboard: a splash gave way to the game's real window, a popup
came up, a launcher opened over the view; or no window has had the keyboard for half a second), the
next keys you press for the game would be hypr3d's: B, 2, 8, 5 closing a window from the Windows
page, Tab and a click picking from the Action Menu, X sending one to the wall, T, Esc leaving 3D. So
they're held back, and a notification says so ("play mode ended (…): the keys are held back. P plays
… again, Super+Esc walks"). W A S D, Space, Left Shift and Left Ctrl or C still walk, the arrows and
the mouse still look, and Super and Ctrl+Alt shortcuts still reach Hyprland; every other key, click
and the wheel does nothing. Then:

- **Super+Esc** walks again (so does `hyprctl hypr3d play off`)
- **P** plays the game again: the window of it the crosshair is on, else the one played last, else
  another of its windows; with none, the window under the crosshair. So do `hyprctl hypr3d play on`
  (or `toggle`, or a view alone), the `hypr3d:play` dispatcher and `hl.plugin.hypr3d.play()`, in the
  view they ask for
- a **left click** on a window of the game plays it again (the click itself goes nowhere)
- a window of the game taking the keyboard (its real window after a splash, the focus back after a
  popup or a launcher closed) is played again at once: one of the same process, else of the same
  class, drawn in 3D, at least 100 px each way, and not under a fullscreen window. Not the one
  Hyprland gives the keyboard to as another of the game's windows closes (the game quitting or closed
  with Super+Q, and a second window of it, its launcher, left): P or a click plays that one, and
  Super+Q pressed twice doesn't close it too. Right after the window a shortcut went to closed (a
  popup over the game, closed with Super+Q) it's played once you turn or move, or 5 seconds go by, as
  shortcuts are held back till then (Controls, above): Super+Q pressed twice doesn't close the game,
  and the second, for no window, takes the keyboard from the game only till then (it's given it back
  when no window or launcher has it, and its workspace is shown). A popup that closes by itself gives
  the game back at once
- 4 seconds without a key, click or wheel held back, it's walking again ("walking again: the keys are
  hypr3d's"); while a launcher over the view has the keys, or the game waits for that hold on
  shortcuts (the keyboard back with it, or taken from it by the second Super+Q), those 4 seconds wait

The status's `"playHeld"` says whether keys are held back. A Steam game's window (`SteamAppId` in its
environment, or class `steam_app_ID`) that opens in 3D and isn't played a moment later says how:
"… : P plays it (Super+Esc gives the keys back)", once a minute for its class, naming the first
window of it that opened (not a helper window that came right after).

### The app's cursor

In 3D the pointer is where the crosshair is on a window, and that app's own cursor is drawn there, on
the window: the shape it asked for (`wp_cursor_shape_v1`, from Hyprland's cursor theme) or its own
cursor surface (an X11 app's comes from XWayland that way), animated ones too. The crosshair shrinks
to a dot at the cursor's hotspot. An app that hides its cursor (a game) shows none, and the whole
crosshair comes back while walking; in play mode the app's cursor is the only one. Hyprland's own
cursor stays off the monitors in 3D (its hardware cursor off, its software cursor not drawn), and
`cursor:invisible` hides the app's too. The resize arrow Hyprland puts on the cursor at a window's
edge (`general:resize_on_border` with `hover_icon_on_border`) is taken off as 3D comes in and as the
mouse comes back from another monitor: in 3D Hyprland's own mouse handling doesn't run to take it off,
and while it's on Hyprland refuses every app's cursor (a game's menu showed the arrow, never its own).

### X11 apps

X11 apps run through XWayland, which Hyprland starts when it finds the `Xwayland` binary. Their
windows are panels like the others, and get clicks, typing, the wheel and relative motion as on the 2D
desktop, at any scale. Their menus and tooltips, which X11 makes windows of their own at absolute
positions (override-redirect), are drawn as popups of the window they belong to, so they go along when
it's out in the world: found through `WM_TRANSIENT_FOR`, else the app's window that has the keyboard,
else the one they're over. (Hyprland 0.55.2's own lookup of an X11 window's parent returns a Wayland
window; the plugin walks `WM_TRANSIENT_FOR` itself.)

A move of the pointer over an X11 window that no relative motion of the mouse came with (walking, the
crosshair going over it; play mode's pointer put somewhere) goes with a relative motion of nothing, as
Hyprland sends one with a confined pointer's. XWayland gives a move without one to X clients from its
absolute pointer device, and SDL3 (CS2's, any SDL3 game through XWayland) takes the pointer's axes to
be absolute from the first such move it sees: a game opened in 3D then had a mouse-look that got only
how much each move differed from the last, nothing for a steady turn. Every other frame of pointer
events to an X11 window in 3D (a click, the wheel) has a relative motion of nothing in it too.
Hyprland's own moves of the cursor on the 2D desktop (a window closing, a game letting go of its
pointer lock, the pointer given back as 3D ends) end with no frame, so XWayland can still hold one from
before, and the frame Hyprland sends with the pointer's enter gave X clients that old move from the
absolute device. So a frame with a relative motion of nothing also goes right after the pointer comes
onto an X11 window (the plugin's doing or Hyprland's: a game that just opened gets it where the cursor
is), before the plugin takes it off one that Hyprland gave it to, and after the moves of Hyprland's
that the plugin asks for (3D ending, the mouse going to another monitor).

### Launching apps into the world

Q, or the Action Menu's Apps page, shows the favourites from `plugin:hypr3d:apps` (desktop ids such as
`org.mozilla.firefox`, app names such as `Discord`, or commands), then All apps: the XDG desktop
entries (`applications/*.desktop` in `$XDG_DATA_HOME` and `$XDG_DATA_DIRS`, which on NixOS the plugin
also looks for without the variable), minus hidden ones, with their icons (PNG or SVG from the icon
themes, read with hyprgraphics). `hyprctl hypr3d launch WHAT` does the same from a script or a keybind.

An app launched from 3D starts the way Hyprland's `exec` starts things, with `HYPR3D_LAUNCH` set in its
environment. Its window (one whose process is the one started or a child of it, or has that variable)
opens in front of you, not on the wall: where its class was put the last time in this world, else as
`plugin:hypr3d:app_rules` says, else as the built-in rules say. It comes as big as it is on your screen
(its text as big as on the 2D desktop), made smaller when that wouldn't fit in 85% of your view, and
where another window already is (two opened one after the other), 10 cm nearer, in front of it. In
third person the distance is past the avatar and the size is for the camera behind it: a window comes
several times bigger (1.5 m past the avatar, seen from 4.1 m), made smaller to fit 60% of the view
above the ground, and stands on the ground; a side keeps its angle from the camera:

| Apps | Distance | Side |
|---|---|---|
| games: `steam_app_*`, `gamescope`, `*.exe`, `steam_proton`, `retroarch`, Minecraft's | 2 m | |
| chat and calls: Discord, Vesktop, WebCord, Equibop, Signal, Telegram, Element, Slack, Zoom, Teams | 1.3 m | 1 m to the left, or nearer the middle, as far as it takes for all of it to show |
| video players: mpv, VLC, Celluloid, Showtime | 2 m | |
| anything else | 1.5 m | |

```lua
hl.config({ plugin = { hypr3d = {
    apps = "firefox, Discord, obs, steam",
    app_rules = "steam_app_.*: 2.4 1.6, discord: 1.2 auto left, org.telegram.desktop: 1.2 auto right",
} } })
```

A rule is `CLASS: DISTANCE [HEIGHT|auto] [left|right|SIDE]`: the class a regular expression (the whole
of it, any case), the distance from your eye (in third person past the avatar) in metres, the window's height in metres or `auto` (as big
as on your screen, as above; the same with no height), and to the side in metres (`left` and `right` are
1 m; a side in metres needs the height or `auto` before it). A height in metres makes the window that
tall whatever its size in pixels: a window as tall as your screen made 0.9 m tall 1.5 m away shows its
text less than half as big as on the screen. Something in the way (a wall you stand close to) brings a
window nearer and smaller, so it looks the same. A window whose app was already running (a second
Discord, a Firefox that hands the address to the one open) isn't the launched process's: for a minute
it's known by the class its desktop entry names (`StartupWMClass`, else its id, else the program's
name). A Steam game, `steam steam://rungameid/ID` (what Steam's own shortcuts run: picked on the Apps
page or by name, the same), is started by Steam, which may open windows of its
own first: its window is the one with `SteamAppId=ID` in its environment, as Steam starts every game
(native or Proton), or of class `steam_app_ID`, as Proton's are; Steam's windows stay where they'd be.

Where you put a window (G, then put it down) is kept for its class, for each map, in
`$XDG_STATE_HOME/hypr3d/windows/` (`courtyard.conf` for the courtyard): its windows launched from 3D
open there again, and so does one that opens in 3D, or that's there when you enter 3D, when it's the
only window of its class. A window opening while you're in 3D goes to its place only when you'd see it
open there (in your view, facing you, not tiny with distance, nothing in between); otherwise it opens
in front of you, as any other would, and the place stays kept for next time (out of sight it looked
as if it hadn't opened). X forgets its place; `hyprctl hypr3d reset-windows forget` forgets them all.

A dialog of a window out in the world (a file chooser from the portal, OBS's properties) opens in front
of it, and goes along with it when the window moves (carried, or in tiling mode's row); one already open
on the wall comes out with it. Any other window that opens while you're in 3D, on the monitor you're in 3D on (a terminal from
a keybind, the screen-share portal's picker, a splash screen), opens in front of you too, not out of
sight on the wall, as big as on your screen: a floating one 1.3 m away, a tiled one as the app rules
say. X sends it to the wall.

### The Windows page, pinning, real sizes

B, or the Action Menu's Windows page, lists every window and where it is (on the wall and on which
workspace, out in the world, or pinned), after Tiling, which turns tiling mode (below) on or off, and,
while it's on, Follow me, which has its row go with you or stay where it is (Y).
Picking a window gives: Focus (its workspace shown, and the
keyboard), Bring here (out in front of you), To the wall, Pin to view or Unpin, Bigger and Smaller (its
real size, a quarter more or a fifth less), Play, and Close…, which asks first: "Close it" is at the
bottom (5) and every other slot is "Keep it", so 8 again, or a click again where Close… was, keeps the
window (a game's keys and aim reach the menu when it isn't played). `hyprctl hypr3d window SEL ACTION`
does the same for a window by address, class or title (`close` at once). The log has what the page did
to which window ("the Action Menu: close …"), X sending one to the wall, and every window that closes
in 3D ("closed: …": its class and title, where it was, and whether it was under a fullscreen window,
had the keyboard, the crosshair was on it or it was played).

**Pinning** (Shift+H on the window under the crosshair or the one you carry, or the Windows page)
keeps a window in the top right corner of your view, `pin_size` of its height, drawn over the world: a
video, or a call, while you walk or play. Shift+H again, whatever the crosshair points at (it can't
point at a pinned window), takes it back into your hands, as big as it was before and as far off: it
goes where you look, H (or G, or a left click) puts it down there, and Esc or a right click puts it back
in the corner. With more than one pinned (the Windows page stacks them down the side), Shift+H takes
the last one pinned. Unpin on the Windows page (or `hyprctl hypr3d window SEL unpin`) puts any of them
down where it is in the world instead; standing close to a wall, in front of the wall rather than in
it, nearer and smaller so it looks the same. To click in a pinned window, take it back and put it
down, or play it from the Windows page: it's played in its corner (played filling the view, the
camera goes to it there), and it's pinned still after.

**The real size**: Shift+wheel while you carry a window, Bigger and Smaller, or
`hyprctl hypr3d window SEL size W H` change the window's size in pixels, and the app draws itself
anew at it, text as big as before (the wheel alone scales it instead). A tiled window becomes floating on the
2D desktop for it, as that's the only way a window can be any size.

### Tiling mode

T puts every window in the 3D view side by side in a ring round you: the ones out in the world (from
any monitor) and the ones on the desktop wall. Each stands upright, turned to face you, 2 m from your
eye, as big as it is on your screen, made smaller to fit 85% of your view, a little apart from the
next. The row is centred on where you look, in the order the windows were round you, left to right.
When there are so many that the row would go right round you, they all get smaller alike, and
something in the way (a wall you stand close to) brings a window nearer and smaller, so it looks the
same. In third person the ring is round the avatar, 1 m further out than the camera's boom (so the
camera is inside it and sees every window from the front), each window as big as the camera needs
(fitting 60% of the view) and standing on the ground. A window you play here (P) takes half the view
all the same, first person or third, as you see it (`play_size`, and Super+wheel while you play: 25%
to 94%), in the middle of your view, and the others make room for it (Play mode, above).

The ring goes with you, so your windows are there to use anywhere on the map: walk, run or fly (over
the rooftops too) and they stay round you, each the same way round you as before (turn to the one you
want), going along at once so none lags behind. A jump doesn't take them up with you: they stay on the
ground you jumped from, and come down with you when you drop off something. Flying they go up and down
with you, standing round you in the air, and landing on something higher they come up onto it a moment
after you. In third person the ring goes with the avatar, and fits the camera's boom as the wheel makes
it longer or shorter; V to first person puts it round your eye. Something in the way as you go (a wall
you pass close to, a doorway) brings a window nearer and smaller for as long as it's there. Y stops it
going with you.

With Y the ring stays where it is, and your windows stay where they are in the world: you walk up to
one to read it, round the ring and through it, off down the street or up into the air, and they're
still there when you come back. V and the wheel don't move it either. Shift+T brings it round you, its
middle where you look, and it stays there: that's how you take your row somewhere and leave it. Y
again brings it round you where you are, the row as it was round you, the windows flying over to it,
and from then on it goes with you again. Y works with tiling off too, saying what T will do, and
`plugin:hypr3d:tiling_follow = false` has the ring stay from the start. Which it does is kept when
tiling goes off and on, when you leave 3D and on another map (coming into 3D, the ring is round where
you come in either way).

While you're at a ring that stays (inside it: no further from its middle than its windows stand),
tiling works as below. Away from it, what you do works as it does without tiling, so nothing flies off
into a row streets away: a window that opens comes in front of you, one the Windows page brings comes
in front of you and leaves the row, and one you carry has no room made for it in the row; put down in
the air, it stays there, out of the row. One brought or put down out there stays where it is, out of
the row, when tiling ends too, and when you come into 3D again; one that opened there is like any other
window out in the world (T off and on, or coming into 3D again, takes it into the row). A window that
comes onto the desktop wall (another workspace shown) still joins the row, wherever you are.

While tiling is on:

- a window that opens (launched from 3D, or opening on the 3D monitor) goes into the row where you
  look, and the others make room (right of the one you play here, in the middle of your view: it moves
  over); so does one the Windows page's Bring here brings (with the ring staying, only at the ring)
- a window that comes onto the desktop wall (another workspace shown) joins the row where it is round
  you
- G or H carries a window out of the row, and room opens for it where you carry it: put down in the
  air, it goes there, so the row's order is yours. Put down flat on a wall, it stays there, out of the
  row (with the ring staying, so does one put down away from it). Esc or a right click puts it back
  where it was in the row
- X sends a window back to the wall, where it stays, and Shift+H pins one, out of the row
- when a window closes or leaves the row, the others close up
- a window going fullscreen (a game) stays in its place in the row, fitting your view, and is played
  (see Play mode); the others stay round you, drawn, and T works on all of them as ever
- Shift+T brings the ring round you, the row's middle where you look, the row as it is (going with
  you, that turns the row; staying, it brings it to you)
- Y has the ring stay where it is, or go with you again (above)

T again (or Tiling on the Windows page, or `hyprctl hypr3d tile off`) puts each window back where it
was before tiling: out in the world where it was, or on the wall. The ones that opened meanwhile, and
the ones you put on a wall, stay where they are. Tiling mode stays on when you leave 3D; coming back,
the ring is round where you come in. `plugin:hypr3d:tiling = true` has it on from the start.

The ring is its own layout, not Hyprland's: a window's tiling on the 2D desktop doesn't change, and it
is shown at the size in pixels that tiling gives it. A window Hyprland makes narrower (another one
opening next to it on the 2D desktop) gets narrower in the row too, and the row closes up round it.

### Drag and drop

Dragging works in 3D: text, files or a browser's tab go where the crosshair is, over whatever window
that is, and drop there when you let go; turn with the button held to take it to another window. In
play mode the pointer drags, within the window played. The dragged thing's icon isn't drawn in 3D
(Hyprland doesn't give it out); the cursor shows "grabbing" while it lasts.

### Input methods

An input method's candidate popup (`zwp_input_method_v2`'s popup surface, fcitx5's candidate list for
example) is drawn as a popup of the window being typed into, by its text cursor, and goes along with it.

### What works, app by app

The VM test (`tools/test/vm`, below) runs open-source stand-ins for the apps you'd use; Discord, Steam's
games and your own OBS setup are for `tools/test/live/check.sh --app` on your desktop.

| App | Runs as | Works in 3D | Doesn't |
|---|---|---|---|
| games: SDL2 (`h3dgame.c`), Chocolate Doom, SuperTux | Wayland, and X11 through XWayland | play mode: the pointer locked, relative motion (Doom turns with the mouse), keys, buttons, the wheel, fullscreen, a controller | a controller only while the game has the keyboard focus (SDL's rule) |
| Chromium | Wayland | `<select>` lists, tooltips and the context menu (popups), selecting text and the clipboard (`wl-paste` reads it), typing, drag and drop, touchpad scrolling and pinch zoom, fullscreen, the portal's file dialog (played along with the window) | |
| Firefox | Wayland | the same; its `<select>` lists and tooltips are subsurfaces of its window, drawn with it | |
| Electron (Discord's stack) | Wayland, and X11 | the page as in Chromium, the app's own context menu (a native one), notifications (mako) | pinch zoom (Electron turns it off unless the app turns it on); drag and drop inside an X11 Electron window, which doesn't work on the 2D desktop either |
| OBS Studio | Wayland (Qt) | its menus (popups), its dialogs by it in the world, screen capture through xdg-desktop-portal-hyprland (its picker in front of you, used in 3D; the capture is the 3D view), a placed window captured as a window | a PipeWire source made through obs-websocket, which waits for the portal forever (OBS's, not 3D's: made from OBS's window, or loaded when it starts, it works) |
| X11 apps: Tk, xterm | XWayland | clicks, typing, the wheel, menus and tooltips as popups (out in the world too), their own cursor, at scales 1, 1.5 and 2 | |
| input methods: fcitx5 | `zwp_input_method_v2` | its candidate popup, by the text | |

### Performance

In 3D the whole view is drawn every frame: the world, the avatar, and every window, drawn from its
client's own buffer (nothing is copied). A game in play mode is one of those windows, so its frames
can't be scanned out directly: Hyprland's direct scanout, which puts a fullscreen window's buffer on
the screen as it is and draws nothing, is blocked in 3D, and each of the game's frames is drawn into
the 3D view instead. What that costs the game is the 3D view's own frame (the world and the avatar
drawn around it) and up to a frame of latency, as with any compositing; `hyprctl hypr3d status` has
the plugin's own CPU time a frame (`updateMs`, `renderMs`) and `fps`. The test VM can't show the
difference: Hyprland blocks direct scanout there on the 2D desktop too (`hyprctl monitors` says
`directScanoutBlockedBy: SW`, its software cursor). Your NVIDIA and a 144 Hz monitor are for
`tools/test/live/check.sh`, which checks the 3D view keeps up with your monitor.

A game behind the 3D view gets its frame callbacks, presentation feedback and FIFO barriers at the 3D
view's frame rate, so with vsync it draws exactly as many frames as the 3D view does; without vsync
(mailbox, or immediate) it draws as fast as it likes, and the 3D view shows the newest each frame.
Tearing (`wp_tearing_control`) doesn't apply in 3D. In the test VM (a virtual 75 Hz monitor at
1280×800):

| | 3D view, frames a second | plugin's CPU time a frame | the game, frames a second |
|---|---|---|---|
| llvmpipe (software): 4 windows and the game on the wall | 17–21 | 43–51 ms (llvmpipe drawing) | 15–22 |
| ... in third person, the avatar drawn | 9–10 | 86–100 ms | 8–13 |
| ... the game played, filling the view (Shift+P) | 21 | 41 ms | 19–21 |
| ... the game fullscreen and played, filling the view | 23–29 | 29–38 ms | 18–30 |
| Intel iGPU (`--gpu virgl`): 4 windows and the game on the wall | 79–83 | 0.2 ms | 76–83 |
| ... in third person | 79–82 | 0.3 ms | 79–82 |
| ... the game played, filling the view | 96–102 | 0.2 ms | 95–103 |
| ... the game fullscreen and played, filling the view | 94–99 | 0.5 ms | 90–98 |
| ... Chocolate Doom played (Wayland or X11) | 92–96 | | |

(Two runs each; the llvmpipe numbers move with the host's load.)

On the 2D desktop the same game drew 150–670 frames a second: in the VM nothing is held to the
virtual monitor's 75 Hz, the 3D view included.

## hyprctl

`hyprctl hypr3d` with no arguments prints the state as JSON. Commands:

| Command | |
|---|---|
| `status`, `toggle`, `on [MONITOR]`, `off [now]` | the state as JSON; enter 3D (on that monitor: its name, or `desc:` and its description) and leave it |
| `away [on\|off\|toggle]` | the mouse and keyboard to your desktop on another monitor, the 3D view staying up (Super+Esc), or back into 3D; `toggle` without an argument. The status's `"away"` says which |
| `type [on\|off]` | type into the window under the crosshair (none: the one that has the keyboard, when it's in the 3D view; else an error), or go back to walking |
| `play [on\|off\|toggle] [here\|fill]` | play the window under the crosshair (P), or stop (and walk again with a game's keys held back); `here` plays it where it is, the view as it was, `fill` filling the view (Shift+P), neither as `play_view` says. A view alone plays in it, and while playing switches to it (the camera going to the window or back). Without an argument, what's played: its class, the pointer, whether it's locked or confined, `fill` (the view it's played in), `view` (how far the camera has gone to face it: 0 played here, 1 filling the view) and `size` (how much of the view it takes played here in tiling mode's ring: `play_size`, or what Super+wheel made it) |
| `camera` | the camera as it's drawn: its eye, yaw and pitch (degrees) and up. The status's `yaw` and `pitch` are yours, which a window played filling the view leaves as they are |
| `launch WHAT` | start an app into the world: a desktop id, an app's name or a command |
| `apps` | the desktop entries: id, name, what it runs, its class, whether its icon was found |
| `window SEL focus\|bring\|wall\|pin\|unpin\|bigger\|smaller\|size W H\|play\|close` | do that to a window, by its address (`0x…`, as `hyprctl clients` has it), class or title |
| `panels` | everything drawn in 3D, in drawing order: its kind (window, popup, layer), window, box on the desktop, placed, drawn over the world, whether its middle is in the view (`inView`), and its surfaces' boxes (subsurfaces too) |
| `look dx dy`, `turn yaw pitch`, `tp x y z` | turn by mouse counts, turn to angles in degrees, teleport |
| `walk secs [forward\|back\|left\|right]`, `jump`, `fly` | move from a script |
| `click [left\|right\|middle]` | click where the crosshair is |
| `sens [value]` | mouse sensitivity |
| `grab`, `place`, `hold dist [scale]`, `pin`, `reset-windows [forget]` | carry windows (`grab` is G or H, `pin` is Shift+H; `hold`'s distance counts from you, in third person from the avatar), and put them all back on the wall, tiling mode off (`forget`: nor where their classes were put) |
| `aim [window]` | turn to face a window's middle: that one (address, class or title), else the one nearest to where you look |
| `log [lines]` | what the plugin logged lately, its notifications too (the last 400 lines; Hyprland's own log has them only with `debug:disable_logs = false`) |
| `windows` | the windows off the wall: their address, where, which way they face (`normal`), how far from your eye, how big (1 = as on the wall), how tall and wide, how big it looks from your eye (`apparent`, 1 = as on the 2D desktop), held, pinned or `tiled` (in tiling mode's row), the window a dialog goes along with (`follows`), how far and big the one you carry is held (and whether it's `onWall`), how many places are remembered, and tiling mode as `tile` has it |
| `tile [on\|off\|toggle\|here\|follow [on\|off\|toggle]]` | tiling mode (T); `here` brings the ring round you, the row's middle where you look (Shift+T), or turns it on; `follow` has the ring go with you (`on`) or stay where it is (`off`), or switches (`toggle`, or no word: Y), and says `following` or `staying`. Without an argument, how it is: on, `follow`, whether you're at the ring (`atRing`: tiling and going with you, always), the ring's middle, the height it stands on (`ground`, where you stand, not up with a jump; staying, where you stood when it was put there), where the row's middle is (`yaw`), how far out, how far behind the middle the view is (`back`, the camera's boom in third person), how much of the view a window takes (`fit`), the row's windows left to right, where the one you carry would go in it (`holdSlot`, −1: nowhere), and the window played here in it (`played`: its address, `fit`, how much of the view it's given, and `share`, what it takes as it's laid out: the same, unless the row has no room left for it even with the others at nothing; null: none) |
| `map [path\|none\|reload\|forget\|scale s]` | load a map; `forget` drops the start and desktop place saved for it |
| `spawn [here]` | go back to the start, or make where you stand the start |
| `desktop [here [height]]` | where the desktop hangs, or hang it where the crosshair points |
| `view [first\|third\|toggle] [distance] [side]` | the camera |
| `view body [on\|off\|toggle]` | first person from the avatar's eyes with its body and hands, or as without them (1.65 m up), till `first_person_body` changes; without a word, `on` or `off` |
| `avatar [path\|none\|reload\|height m]` | load an avatar, or print its state |
| `avatar expression [name [weight]\|none]` | set a face |
| `avatar gesture [left\|right\|both gesture]` | set a hand gesture |
| `avatar parts [reset]`, `avatar toggle name [on\|off\|reset]`, `avatar slider name [0..1\|NN%\|reset]`, `avatar slider name x y`, `avatar shape key [weight\|reset]` | the outfit; `parts` lists the toggles, sliders and material variants. A two-axis slider takes x and y, −1..1 or NN% each |
| `avatar physics [on\|off\|toggle]` | spring bones |
| `avatar lipsync [on\|off\|toggle\|gain dB\|auto\|source name\|default]` | lip sync, its gain and its microphone; without an argument, what it hears and what the microphone gives: the level, formants and visemes, the badge's text and `"problem"` (none, starting, unlinked, muted, silent, nothing, missing, error), the source (name, description, state, muted, volume), `"linked"`, `"samples"`, `"buffers"`, `"silentFor"` (seconds of exact zeros), `"peak"` and `"rms"` (the last second, dBFS), `"gain"`, `"reference"` (your voice), `"room"`, `"marks"` (shut below, wide open from) and `"sources"` |
| `avatar emote [name\|number\|file\|folder [once\|loop]\|stop]` | play an emote, or load emotes from files; without a name, the list, with each one's speed and sound, and what's playing: how far into it, and its sound's stream (`"sound"`) |
| `avatar attack [left\|right]` | attack, as a left click on nothing does: that arm's swing, or whichever is next; it gives back the attacks, as `"attack"` in the avatar's state has them |
| `menu [open [page]\|close\|toggle\|back\|pick [n]\|move dx dy\|scroll n]` | drive the Action Menu; without an argument, what it shows (a dial's value, a stick's x and y) |

The `hypr3d:menu` dispatcher toggles the Action Menu. `hypr3d:menu emotes` opens a page (`apps`,
`windows`, `maps` and `avatars` too), and any other argument does what `hyprctl hypr3d menu` does. The `hypr3d:play`
dispatcher plays the window under the crosshair, or stops (`hypr3d:play here` or `fill`: in that view,
or switched to while playing; `on`, `off` and `toggle` as `hyprctl hypr3d play` takes them),
`hypr3d:away` sends the mouse and keyboard to another monitor, or brings them back, and
`hypr3d:tile` turns tiling mode on or off (`hypr3d:tile here`: the ring round you, the row's middle
where you look; `hypr3d:tile follow`: the ring going with you or staying, as Y). The Lua functions
are `hl.plugin.hypr3d.toggle()`, `enter()`, `exit()`, `type()`, `play(["here" or "fill"])` (P, or in
that view, or switching to it while playing; `on`, `off` or `toggle` too, with a view or not, as
`hyprctl hypr3d play` takes them; it gives back what that says), `away()`, `tile(["here" or
"follow"])` (T, Shift+T or Y; it gives back what `hyprctl hypr3d tile toggle`, `tile here` or `tile
follow` says) and `menu([page or command])`.

The status's `"cursor"` is the app's cursor as it's drawn (where on the window, its size and
hotspot, or null), `"playing"` what's played (as `hyprctl hypr3d play` has it), `"playHeld"` whether a game's keys are held back after
play mode ended by itself, `"shell"` the layer
surface over the view (its namespace, whether it has the keyboard, where its pointer is, and whether that's over it), and `"updateMs"` and `"renderMs"` the plugin's own
time a frame on the CPU (its update, and its drawing's GL calls), averaged.

## Avatars

`plugin:hypr3d:avatar` (or `hyprctl hypr3d avatar FILE`, or the Action Menu's Avatars page, below)
takes one of these:

- **VRM 0.x and 1.0**: the humanoid map, expressions (blend shapes, material colours and texture
  transforms), look-at, spring bones (VRM 0.x `secondaryAnimation` and `VRMC_springBone`, with
  `VRMC_springBone_extended_collider`'s inside and plane colliders and `VRMC_springBone_limit`'s cone,
  hinge and spherical limits), node constraints
  (`VRMC_node_constraint`), and MToon's shading, matcap, outlines and render queue.
- **A plain glTF/GLB**: the humanoid bones are guessed from their names (Mixamo, VRoid, Blender's
  rigs and most other naming styles). Expressions come from shape key names (VRoid, VRChat, MMD and
  ARKit), and spring bones from bone names (hair, skirt, cape, sleeves, ribbons, tail and ears, in
  Japanese too). Clips named idle, walk, run, jump or fall play for those; otherwise the avatar is
  walked procedurally.
- **What `tools/unity2hypr3d.py` writes**: a GLB with a settings file next to it (below).

It blinks, its eyes wander, it turns its head where you look, and walks, runs, jumps, falls, crouches
and flies. Gestures curl its fingers and can set a face.

Its hair, skirt, sleeves and the like swing (spring bones, `avatar_physics`). They're stepped 60 times
a second whatever the frame rate, each step with the avatar and what they hang from as they were by
then, and a frame between two steps shows them as far on into the next as it is, from where what they
hang from is then: on a 144 Hz monitor they swing as smoothly as at 60, and the sleeves on the arms
first person holds up in view go with the arms rather than shaking on them.

Without clips of its own a humanoid walks as people do. Each foot stays where it lands until it
lifts, so the feet don't slide: it lands on its heel (flatter in high heels, as people do) and pushes
off over the ball of the foot (a shoe without toes of its own rolls over its tip), the foot easing into
and out of each roll, and the next step lands where the body will be by then,
also along a curve as it turns. Stride and cadence follow the speed and the legs' length as they do
in people (dynamic similarity: stride = 2.5 legs × Froude^0.3), so a smaller avatar takes shorter,
quicker steps, and one whose legs can't reach that far (high heels) shorter ones still. Walking, each
foot is down about 60% of the time; running 20–35%, the rest in the air. Walking, the knees bend as
gait studies measure them: giving a little as the foot takes the weight, bending as it pushes off,
folding to 60° early in the step so the foot clears the ground, and nearly straight again to land (a
leg near its full reach straightens softly rather than snapping straight). The pelvis rises over the
standing leg walking and sinks onto it running, 4–5 cm up and down in all at a brisk walk as people's
(the steps no longer than that allows), settling smoothly after each landing; it sways over the
standing leg, drops on the other side and turns with the stride. The trunk leans ahead a little and
turns against the pelvis, the arms swing against the legs (the elbows bent about 20° back and 40°
ahead walking, bent and pumping running), the hands hang relaxed, and the head stays level. The arms
keep clear of a skirt or a coat: at load it measures how far out the body goes round the hips (not
the arms, the hair or what the arms carry) and how thick the sleeves are, and the arms are held out
as far as the whole swing needs (a sleeve's cuff may press into the skirt a little, as cloth does).
It leans into starts, and a little into curves (not when it spins on the spot). In third person it
turns to where the keys take it as soon as they go down (running, as it slows enough to turn), and it
turns as people do, not all in one piece: the head first, looking ahead into the turn (not back where
the camera looks), then the trunk as far as the spine twists, then the pelvis and the feet with the
steps. The pelvis turns no faster than about 340° a second and no further from a planted foot than
the hip lets it; that foot pivots on its ball (a foot that will pivot is put down as much further out
as its heel will swing; on stairs, where its heel would swing into the step behind, it steps round
instead), and the next one lands under its hip, turned toward where it will face, on its own side of
the other foot. A leg in the air hangs under its hip whichever way the pelvis turns
meanwhile, rather than swinging out round the body. Turning right round it slows down, turns, then
goes, as people do: the legs don't set off the new way until the pelvis faces it near enough, trailing
your body by up to about half a metre meanwhile (running too, held back less: a run turns round in more
of a curve). So right round (A then D, W then S) takes a braking step and two steps round, about 0.6
seconds, the legs under it and never crossed; from standing, the foot on the side it turns to steps
first; round a tight curve (the mouse swung while walking) the steps quicken, though never into a flick
(at most 1.7 times as quick for a foot left out to the side). Going back and forth (a key back the other
way again within half a second, walking), it faces the way it has been going if the taps only slow it
down, else the way the camera looks, and steps from side to side or back and forth until one way is
held. The body it carries follows yours as a person's would: speeding up, slowing down and
turning back no harder than legs push (7 m/s² walking, 12 running), starting to slow as the key goes
down, trailing you a little as it sets off rather than hurrying to catch up (a few percent quicker at
most), and when you let go of the keys it comes to a stop a little past where you stop (about 20 cm
walking, more running; never into a wall) rather than stopping dead. Your own body goes back the other way at 7 m/s² (it starts at 10 and stops at 14). The steps
keep their pace through a turn back, a foot in the air lands where the body will be when you stop
short or turn back, and where it lands moves smoothly when you press another key. It takes quicker
steps when a leg is at its reach, steps back under itself when it stops or turns on the spot, and its
knees give as it lands from a jump. Up and down stairs and hills it stays on its feet: each foot comes down
on one tread, the whole foot (moved along itself a little if it would land across a stair's edge, toes in
the next step or heel over the edge; a foot too long for the tread leaves its heel over the edge), and
lies along a slope; a foot in the air goes up and over a step's edge before it comes down; the pelvis
goes along the line of the steps rather than a step at a time, lower going down, as people's is (the
leg behind bent, the one ahead reaching down to the next step), and the steps are sized for the slope,
landing further ahead going up and nearly under the body going down. Walking at 1.6 m/s it takes two
stairs a step. Tapping the keys doesn't jerk it about: a step in the air as
it stops (or starts again) eases from a walking step into a standing one (or back), the pelvis and
arms carry on their swing from where they were, and the body speeds up into a turn and slows out of
it (7 radians a second at most, reached in about a ninth of a second) rather than snapping round.
Walking into a run and back (Shift) the arms and the trunk ease from one to the other, and when the
steps hurry (turning sharply, stopping short from a run) the body's swing speeds up smoothly with them.

Above the legs the body goes as a walk and a run made in Blender have it (`assets/walk.vrma` and
`assets/run.vrma`, built in; made with `tools/blender/h3d_walk.py`, below): the hips' turn, drop and
sway, the trunk turning against them and leaning ahead (more running), the head level but nodding and
tilting a little with the steps, the collarbones, and the arms swinging with the elbows and hands
following through (walking relaxed, running bent and pumping, the hands in toward the middle as they
come ahead). They're played at the steps' own phase, so they stay in step at any speed, through turns,
hurried steps and stops; swung as far as the speed takes them (slower, less); walking into running as
the arms ease; and on them goes what the walking adds itself: the trunk twisting back after a turn,
leaning into starts, crouching, the head looking into turns, and the arms held out as far as this
avatar's hips and skirt need (the clips' own are a slim avatar's). The legs are always the stepping's,
the feet planted. Standing still, it eases back to its own stance. An avatar's settings file can give
it a walk and run of its own, or none (`walk`, below).

Off the ground the body goes on from how it was rather than snapping to an air pose: the legs, the arms
and the trunk go toward the pose for how it's going as limbs with weight do (springs), from where they
were. A jump from standing pushes the body up over a moment (the hips go on as they were going, not off
like a shot), the legs tuck, the arms swing up and then out, and the legs reach down to land; from a
run the leg that was stepping goes on ahead and the other trails behind, the arms against them, as a
stride in the air, and the leading leg reaches down to land first. Landing, the hips go on down a little
as the knees take it and come back up (the view gives a couple of centimetres and stops with you, as
the avatar does), and going on (a key down) the stride goes on from the foot that
lands first, the other on through the air when running, instead of stopping dead to start again.
Flying, the body lies along the way it goes, the faster the flatter (about 55° at 8 m/s, nearly flat
at 16), upright going straight up and feet first coming down; it leans into speeding up, flares back
upright past standing as it slows, banks into its turns and turns round more slowly than walking (4
radians a second), the legs trail along the body (one knee bent, fluttering a little, together flat
out), the arms swept back along it, the head up to see ahead. Hovering it floats upright, one knee bent,
bobbing gently and treading slowly, the arms held out a little; going into or out of flying it eases
from one to the other. Wherever the pose does change all at once (leaving the ground, landing, taking
to the air from it), what was shown goes on as it was going and settles into the new pose over a moment
(inertialization) rather than snapping to it.
`hyprctl hypr3d avatar`'s `"gait"` has its stride, cadence,
duty factor, the pelvis's yaw and each foot (its yaw, how far its knee bends, how much of the leg's
reach it uses), and off the ground `"air"`: the leg that went on ahead, how fast it was going, how much
it's flying, how the body lies (pitch and roll) and the legs' and arms' angles. The built-in emotes are Wave, Clap, Point,
Cheer, Dance, Backflip, Sad Kick and Die. `avatar_emotes` adds your own `.vrma` or glTF clips, and
so does the settings file's `emotes`, where the converter lists the dances and poses it finds.

An emote can have a sound, its `"sound"` in the settings file: an Ogg Vorbis file, mono or stereo, a
dance's song (the converter carries over the one a bare-clip dance comes with, as Freddy Fazbear Pump
It Up's). While the emote plays in 3D it plays through PipeWire to your default output, as the
"hypr3d emote sound" stream (the mixer shows the emote's name, and WirePlumber remembers the volume you
give it there), at `emote_volume` (0.5: half the file's own level, as a dance's song is often mastered
louder than the rest of what you hear), round with the emote as it loops, and it fades out as the emote
does when you move or stop it. It comes in where the dance is by the time it's heard, and from then on
the dance keeps time with what's heard of it (a slow or dropped frame would otherwise put the dance
behind the song). Played again it starts again; out of 3D it stops, and back in 3D in the middle of
the emote it comes in where the dance is. It plays at its own speed whatever the emote's `speed`.
`hyprctl hypr3d avatar emote` lists each emote's sound and has the stream's state (`"sound"`: what's
playing, where in it, the latency, an error). Built without PipeWire, emotes play with no sound. Toggles
and sliders can show and hide parts, set shape keys, move, turn and scale nodes, and switch
materials: the GLB's glTF material variants (`KHR_materials_variants`) that the settings file names.
A toggle can also loop shape keys and nodes back and forth (VRCFury's Breathing), or leave a part
where it is in the world while it is on (VRCFury's World Drop).

Materials are drawn in Unity's render queue order, with Unity's stencil test: the eyes of an UnlitWF
avatar show through its fringe, as in VRChat. Toon outlines are drawn as an inverted hull: the mesh
again, pushed out along its normals, its front faces culled (UnlitWF's, lilToon's, Poiyomi's and
MToon's; `hyprctl` has no switch for them, the harness's `--outlines 0` turns them off). It costs a
second draw of each outlined mesh: 0.24 → 0.28 ms a frame for Hatsune Miku NT at 1920×1080 on an RTX
4090. UnlitWF's back faces take their own colour or texture, and its light clamp keeps the light's
brightness between its minimum and one (its anti-glare). The converter writes these into the
material's glTF extras (below). UnlitWF's light has no N·L in it, so its materials are lit the same
all round unless their toon shade is on, as are Poiyomi's with its Flat lighting.

Toon shading follows MToon's: the sun lights a surface from its shade colour to its lit one as N·L
goes from one threshold to another, a sharp step in place of the gradual fall-off, and the shade side
is lit by the sun too (the sky and the bounce light it as any material). A shadow cast on it takes it
to its shade colour, as MToon folds the shadow into N·L; for that the shadow is looked up 10 cm
towards the sun (or three of its normal offsets), so the surface's own silhouette in the shadow map
doesn't draw a staircase along the step. In a map with the game's lighting the sun only gets as far
as its baked shadow lets it, so an avatar indoors is lit by the probes alone. A matcap, looked up by
the normal as the eye sees it, is added, multiplied, mixed in, or (UnlitWF's median) lightens and
darkens, lit as the surface is or as if in full light. VRM files bring MToon's own (`VRMC_materials_mtoon`,
or VRM 0.x's `materialProperties`); the converter writes lilToon's, Poiyomi's, UnlitWF's and MToon's
into the extras.

### First person

With a humanoid avatar (one with a head and both arms), first person is from its eyes: between its eye
bones, or without them where its face has them. So you see the world from its own height (Hatsune Miku
NT's eyes are 1.43 m up, not the 1.65 m you see from without an avatar; `avatar_height` makes it taller).
Its head and what hangs off it (its hair, a hat) aren't drawn, the rest is: looking down you see its body,
its skirt and its legs walking, and its shadow is whole. The camera goes with its eyes as it crouches,
bends over and lands, but holds still through what moves them only a little (up to 3 cm: breathing, the
weight going from foot to foot, most of a step's bob), so the windows in view don't wobble.

Its hands are up low in the view, as a first person game has them: posed there by the plugin, the arms
reaching from the shoulders, the elbows down and out, the fingers as its gestures have them (F1–F8). A
hand making a gesture is held up a little where it shows, turned so the gesture reads side on (pointing
away from you, a hand would hide behind its own forearm or a sleeve). The hands bob as you walk, pump as
you run, trail a little behind the camera as you turn or start and stop, and keep back from a wall or a
window just ahead (lower the nearer it is, out of the view up against it). The trunk turns toward where
you look, up to about 43° from the hips (stepping sideways the legs go where you go, the shoulders stay
square to the view), and looking down it bends over a little to see past itself; looking down past about
45° the hands let go (all the way by 70°), the arms hang as the walking has them, and the body is in view
below.
What you do moves them:

- a mouse button pressed on a window: the right hand's finger reaches to the crosshair (a little under
  it and to its right), to the window itself when it's within reach, as long as the button is held (a
  click: a quarter of a second, poking it);
- typing (E): lower and nearer together, the hand on each key's side of the keyboard tapping as keys go
  down (the space bar: one thumb, then the other);
- carrying a window (G, H): both reaching out to it, the palms open;
- playing a window (P): let down, out of the view;
- an emote: it moves the whole body, the head too, so the camera goes out behind the avatar as in third
  person while it plays, and back into its eyes when it's done (or you move).

`first_person_body = false`, or `hyprctl hypr3d view body off`, puts first person back as it is without
an avatar: from 1.65 m up, only the avatar's shadow. An avatar that isn't a humanoid with a head and
arms is seen that way anyway. `hyprctl hypr3d avatar`'s `"body"` says which it is, `"eyeHeight"` where
its eyes are at rest, `"emoteView"` how far out an emote's camera is (0 in the eyes, 1 out), and
`"hands"` what the hands do (`ready`, `touch`, `type`, `hold` or `down`), how much of the arms that is
(`"arms"`: less under an emote, none let go looking down) and where each wrist is in the view (`"at"`,
0..1 across and down from the top left).

### Attacks

With the crosshair on no window (on the map, the sky, the wall between windows), a left click attacks:
the avatar throws a right hook, the fists closed. Its fists come up (the left by its chin), it winds up
a little to its right with the right fist out past its shoulder (ahead of long hair: in third person you
see it past Miku's twin tails), then its trunk turns it left and the fist sweeps round in front of its
face, palm down; held a moment, back to a guard, and the hands down again: about 0.75 s in all. A click
while it swings is the other arm's (the same hook, mirrored), as soon as the first has struck (a quarter
of a second in): it takes over from where its own fists come up, one, two. Clicking on, they go one after
the other, no quicker than one every 0.2 s (a click while one is waiting to go is dropped), and the first
click after a pause (0.9 s since the last one started) is the right arm's again. The punch has the trunk
(spine to head, turned on top of what the body does) and the arms; the legs go on doing what they were
doing: standing, walking or running, crouching, in the air. In third person it punches the way the body
faces (walking, the way it walks), wherever the camera looks at it from. In first person it has a punch
of its own: from the hands as first person holds them, the fist drawn back to the bottom right of the
view and struck up to the crosshair, side on (Miku's bell sleeves hide a fist that points away), and back.
The hands go where that punch has them in the view (from the eyes, in arms' lengths, whatever the
avatar's build), where you look (up and down too, about 30° at most), and no further ahead than there's
room (they stop short of a wall or a window just ahead). An emote stops.

The punches are animations made in Blender (`tools/blender`): `assets/attack.vrma` and
`assets/attack-first-person.vrma`, VRM animations of the right arm's, built into the plugin and put on any
humanoid as emotes are; the left arm's is their mirror image. An avatar's settings file can bring its own
(`"attack"`, see below).

A click on a window is that window's, as ever, and so is every click while you type into a window (E)
or play one (P); with the Action Menu open it's the menu's, carrying a window it puts it down, and with a
launcher over the view it's the launcher's. With a window's menu open that holds onto the pointer (GTK's,
Qt's and weston's do; the browsers' don't), a click on nothing closes it, as a click beside it on the 2D
desktop does, and the next one attacks. An avatar that isn't a humanoid has no arms to swing: a click
on nothing does nothing.

`hyprctl hypr3d avatar attack [left|right]` attacks as a click does (with that arm), and `hyprctl
hypr3d avatar`'s `"attack"` says what the attacks are doing: each arm's punch (`"t"`, seconds into it,
`"weight"`, how much of the body is its, `"fist"`, and `"firstPerson"`: first person's punch) or null,
the arm waiting to go next (`"next"`), the last one, how many there have been (`"swings"`), how far the
chest is turned from the hips (`"turn"`, radians, right > 0) and how much of the body the punches have
(`"weight"`).

### The settings file

`AVATAR.hypr3d.json` (or `AVATAR.glb.hypr3d.json`) next to the avatar adds to what the model says
or overrides it. The converter writes it; you can also write one by hand. Every key is optional:

| Key | |
|---|---|
| `humanoid` | `{"LeftUpperArm": "Arm_L", …}`: humanoid bone (Unity's or VRM's names) → node name |
| `expressions` | `[{"name", "preset", "shapes": {"shape key" or "mesh/shape key": 0..1}, "binary", "blink"/"lookAt"/"mouth": "block"\|"blend"\|"none"}]` |
| `gestures` | `{"left"/"right"/"both": {"fist": "expression name" or "none", …}, "combos": {"fist+open": "expression name" or "none"}}`: the face each hand gesture sets; a combo's while the left hand makes one sign and the right the other |
| `hands` | `{"file": "a .vrma", "left"/"right": {"fist": seconds, …}}`: each sign's finger pose, the animation's at that time (the converter writes an avatar's Gesture layer's hand poses so); the others stay procedural |
| `floor` | the height the avatar stands on, in the GLB's units (MA's Floor Adjuster); otherwise its lowest point |
| `hidden` | `["mesh node", …]`: parts that start hidden |
| `visemes` | `{"pp": {"shape key" or "mesh/shape key": 0..1}, "ff": …, "ss": …, "ch": …}`: the shapes of the consonants lip sync shows (VRChat's PP, FF, SS and CH visemes; the converter also writes th, dd, kk, nn and rr, unused) |
| `fixed` | `["node", …]`: nodes held in the world where their rest pose was when the avatar appeared (MA's World Fixed Object), with everything under them |
| `toggles` | `[{"name", "group" or "groups": [names], "on", "show": [parts], "hide": [parts], "shapes": {…}, "variants": [material variants], "transforms": {…}, "loop": {"seconds", "a": {"shapes", "transforms"}, "b": {…}}, "drop": [nodes]}]`: outfit toggles for the Action Menu; the toggles of a group are exclusive, and a toggle in several groups turns off the others of each. A toggle that is on puts its material variants' materials on, sets its nodes' `transforms` (`{"node": {"t": [x,y,z], "r": [x,y,z,w], "s": [x,y,z]}}`, each part optional, in the node's own space), goes from `a` to `b` and back every `seconds` (smoothly), and leaves the `drop` nodes where they were in the world when it went on |
| `sliders` | `[{"name", "value": 0..1, "keys": [{"at": 0..1, "shapes": {…}, "transforms": {…}, "show": [parts], "hide": [parts], "variants": [material variants]}]}]`: dials for the Action Menu (VRChat's radial puppets). Between two keys the shape keys and transforms blend. Parts and material variants switch at a key, so each key has what holds from it up to the next. A two-axis one has `"axes": 2`, `"value": [x, y]` and a `"grid": n` of n×n keys, each `"at": [x, y]` (−1..1), blended between the four around the stick |
| `walk` | `{"walk": "FILE", "run": "FILE"}`: the avatar's own walk and run (how its body goes over the steps) instead of the built in ones, VRM animations of one stride each, the left heel landing at the start and the end, the right half way (next to the settings file unless absolute; one it leaves out is the built in one); `"none"`: the walking's own body, procedural. `tools/blender/h3d_walk.py` makes them |
| `attack` | `"FILE"` or `{"file", "firstPerson"}`: the avatar's own punch instead of the built in one, a VRM animation of the right arm's (next to the settings file unless absolute; the left's is its mirror image), its trunk and arms; first person's the built in one unless `firstPerson` names one. Its animation's extras' `"markers"` (seconds) say when the fists are up (`ready`: a punch after another starts there), when it strikes (`hit`), when the other arm's may start (`next`) and when it starts letting go of the body (`out`); `tools/blend2vrma.py` writes them from Blender's timeline markers |
| `emotes` | `[{"file" or "clip", "name", "loop", "hold", "grounded", "speed", "sound"}]`: more emotes. `file` is a `.vrma` or glTF file (next to the settings file unless absolute), `clip` a clip of the avatar's own. `hold` keeps the last frame until you move, `grounded` keeps the feet on the floor, and `speed` (default 1) plays it faster or slower. `sound` is an Ogg Vorbis file (next to the settings file unless absolute, mono or stereo, 10 minutes at most) played with it, round as it loops (see Avatars); one that can't be played is said in the log, and the emote plays without it |
| `springs` | `[{"name", "bones": [roots], "ignore": [bones], "stiffness", "drag", "gravity", "gravityDir": [x,y,z], "radius", "center", "immobile", "parentImmobile", "colliders": [names, or "body"], "limit"}]`: each root and everything under it swings. `limit` is a `VRMC_springBone_limit` limit for each of its bones: `{"cone": {"angle", "rotation": [x,y,z,w]}}`, `{"hinge": {"angle", "rotation"}}` or `{"spherical": {"pitch", "yaw", "rotation"}}`, in radians, turned by `rotation` from a frame whose y runs along the bone: how far a bone may turn from where the animation points it (a PhysBone's Angle, Hinge or Polar limit). A bone with a limit leaves out the colliders made for the body (`"body"`) that it starts inside of. `parentImmobile` (0..1) is how much of what the bone the spring hangs from does beyond where the avatar goes (a walk's bob and sway, a turn, a dance) carries the spring along instead of swinging it: a PhysBone's Immobile (All Motion). `radius` (how far its bones keep out of the colliders) can be a list, one for each bone down from a root and the last for the rest: a PhysBone's radius curve, as wide as long hair fans out lower down |
| `colliders` | `[{"name", "node", "offset": [x,y,z], "tail": [x,y,z], "radius", "inside"}]`: spheres, or capsules with a tail, in the node's units; `"inside": true` keeps the bones inside it (PhysBones' inside bounds). A plane is `{"name", "node", "offset", "normal": [x,y,z]}`: the bones keep to the side it faces. A disc is `{"name", "node", "offset", "disc": {"normal": [x,y,z], "radius"}, "radius"}`: flat and round round `offset`, out to the disc's `radius`, with the collider's `radius` round that (as thick as twice it, round at its edge): a tutu, a hat's brim. What gets on one is pushed off its nearest side, where a ring of capsules round a tutu pushes what gets inside the ring further in (an older hypr3d reads it as a sphere of the collider's radius round its middle) |
| `immobile` | 0..1, default 0.9: how much of the air the avatar carries along as it moves. With 0, running at 4.5 m/s blows long hair out level behind it |

The converter also leaves things for hypr3d in the GLB's glTF extras. A primitive's `hypr3d_part`
makes it a part of its own (what an MA Mesh Cutter hides while a toggle is on). A material's
`hypr3d_queue` is Unity's render queue, `hypr3d_stencil` its stencil test and write (`ref`, `read`,
`write`, `comp`, `pass`, `fail`, `zfail`, and `again` for UnlitWF's MaskOut_Blend, which draws what
its mask hides again, fainter), `hypr3d_outline` its toon outline (`width` in metres, `space`
world, object or screen, `color`, `base`, `tint`, `mask`, `shift`, `fix`, `lit`, and a colour
`texture`: `{"index", "transform", "blend"}`, the colour times it, or mixed `blend` of the way
towards it), `hypr3d_back` its
back faces' `color` and `texture`, `hypr3d_light` UnlitWF's light clamp (`min`, `max`, `chroma`),
`hypr3d_toon` its toon shading (`shade`: a linear colour, times the base colour if `base`, and times
a `texture`: `{"index"}`; `lo` and `hi`: N·L where it's all shade and where it's all lit, `lo = hi =
-1` lit all round; `strength`: how much of the shade shows) and `hypr3d_matcap` its matcap
(`{"index", "color": [r, g, b, amount], "mode": "add"|"multiply"|"mix"|"median", "lit"}`, `lit` 0
as if in full light).

### The Avatars page

The Action Menu's Avatars page (the main page's ninth: Tab, then 9) switches avatars in 3D. It lists
the `.glb`, `.gltf` and `.vrm` files in `$XDG_DATA_HOME/hypr3d/avatars/`
(`~/.local/share/hypr3d/avatars/`) by name, and those in its folders by the folder's name
(`folder/file` when a folder has more than one; `.vrma` emotes don't count), so an avatar in a folder
of its own, with its settings file and emotes beside it, is listed by the folder's name. Then come
the configured avatar and the one shown, wherever they are. The one shown is lit and says "here",
one on its way says "loading…", the others give their size, and the configured one says "default".
The main page's Avatars item says which avatar is shown. A pick closes the menu and loads that
avatar in the background at `avatar_height`, as `hyprctl hypr3d avatar` does. Picking the avatar
shown does nothing, and while another is loading it stops that one. The choice lasts until the
plugin loads again or `plugin:hypr3d:avatar` or `avatar_height` changes; at login the configured
avatar comes back.

## Maps

`plugin:hypr3d:map` loads a glTF/GLB. The scale is guessed unless `map_scale` gives it. A node named
`hypr3d_spawn` marks the start and faces its −Z. A node named `hypr3d_desktop` marks where the desktop
hangs, facing its +Z. Without them hypr3d uses a game's `info_player_*` start and looks for a flat wall
itself. What you change with `spawn here` and `desktop here` is saved in
`$XDG_STATE_HOME/hypr3d/maps/`. A directional light becomes the sun, and nodes under a
`hypr3d_backdrop` node are scenery with no collision.

The Action Menu's Maps page switches maps in 3D. It lists the `.glb` and `.gltf` files in
`$XDG_DATA_HOME/hypr3d/maps/` (`~/.local/share/hypr3d/maps/`) by name, and those in its folders by the
folder's name (`folder/file` when a folder has more than one). Then come the configured map and the
one you're on, wherever they are, and the courtyard. The one you're on is lit and says "here", one on
its way says "loading…", the others give their size, and the configured one says "default". A pick
closes the menu and loads that map in the background, as `hyprctl hypr3d map` does. The configured
map loads at `map_scale`, and the others at the scale saved for them, or guessed. Picking the map
you're on while another is loading stops that one. The choice lasts until the plugin loads again or
`plugin:hypr3d:map` changes; at login the configured map comes back.

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
- faces set by hand gestures in the FX controller, both hands' signs together too, and the finger
  poses of the avatar's Gesture layer (its own, an MA Merge Animator's or a VRCFury Full
  Controller's), as the settings file's `hands`
- Expressions Menu toggles that show or hide objects, set shape keys, change materials, or move, turn
  and scale objects (transform curves, in the FX controller's clips and blend trees), and objects
  that start hidden. Radial puppets become sliders, and two- and four-axis puppets two-axis sliders
- PhysBones and Dynamic Bones, as springs and colliders: spheres, capsules, planes, and the ones that
  keep bones inside them. A PhysBone's Angle, Hinge or Polar limit (its Rotation too) becomes the
  spring's `limit`, and its Immobile (All Motion) its `parentImmobile`: a necktie whose limit keeps it
  in front of the chest stays there walking, running, turning and dancing
- materials: colour, texture, cutout or transparent, emission and culling (Standard, lilToon,
  Poiyomi, MToon and UnlitWF settings, and the common property names of other shaders). A material
  that a toggle or slider puts in a slot is written as a glTF material variant, and so is one whose
  colour, emission, tiling or cutoff it changes. The toggle or slider names the variant. Changes in
  effect at rest go straight into the GLB. Also Unity's render queue, the stencil (UnlitWF's
  Mask/MaskOut/MaskOut_Blend shaders by their passes, or by their GUIDs when the shaders aren't in
  the input; lilToon's and Poiyomi's stencil settings), toon outlines (UnlitWF's `_TL_*`, lilToon's
  outline shaders, Poiyomi's and MToon's: width and its mask, colour, Z shift, fixed width near the
  eye), UnlitWF's back faces (`_BK_*`) and light clamp (`_GL_*`), and toon shading and matcaps
  (UnlitWF's `_TS_*` and `_HL_*`, lilToon's first shadow and matcap, Poiyomi's Multilayer Math and
  Flat lighting and its first matcap, MToon's and MToon10's), in the materials' extras. UnlitWF's
  alpha from a mask texture (`_AL_Source` 1 or 2) or inverted (`_AL_InvMaskVal`) is baked into the
  base texture's alpha
- the Action layer's humanoid clips, dances and poses, as emotes (below)
- Modular Avatar setups, built the way MA builds them for VRChat: Merge Armature (an outfit's bones
  join the avatar's and its meshes follow them), Bone Proxy, Move To, Replace Object, PhysBone
  Blocker, Platform Filter, Scale Adjuster (the meshes weighted to a bone scaled, not its children),
  Floor Adjuster (the settings file's `floor`), Global Collider (a collider every PhysBone that
  allows it meets), and MA's menus and reactive components: Menu Item (radial ones too), Menu
  Installer, Menu Install Target, Menu Group, Object Toggle, Shape Changer (its Delete too), Mesh
  Cutter (its vertex filters by axis, bone, mask, shape key and UV tile: cut away for good when it is
  always in effect, else a part of its own that the toggles hide while it is), Material Setter,
  Material Swap, Blendshape Sync, Merge Animator (FX, Gesture for hand poses, and Action for emotes),
  Merge Blend Tree and Parameters
- VRCFury setups, built after MA's as VRCFury builds them:
  - Armature Link: an outfit's bones are linked to the avatar's (snapped on if it says so), and its
    meshes follow the avatar's bones. A bone that an animation moves keeps its own weights, and what
    is under it stays with it.
  - Toggles: they turn objects on and off, set shape keys, swap materials, set material properties
    and FX floats, scale objects (the Scale action), loop (Smooth Loop), leave objects in the world
    (World Drop), and play clips (with transform curves too). Exclusive tags become groups (a toggle
    may have several), and the avatar starts in the resting state the toggles give it. Slider toggles
    and Puppets become sliders, two-axis Puppets two-axis ones.
  - Gesture Driver (and Senky's): faces for the hand signs, both hands' combos too, with their lock
    toggles. Blinking and Visemes give the blink and mouth shapes, and Block Blinking and Block
    Visemes stop them.
  - Full Controller: its FX controller (with its path rewrites), its Action controller (emotes), its
    Gesture controller (hand poses), menus and parameters are merged in. Its other layers (Base,
    Additive, Sitting, TPose, IKPose) are left out quietly: hypr3d walks, sits and stands by itself.
  - Blend Shape Link, Apply During Upload, Delete During Upload, Move Menu Item and Reorder Menu Item.
  - The older Modes, Object State, Bone Constraint, Breathing, World Constraint and Senky Gesture
    Driver, upgraded as VRCFury upgrades them. The old Unity 2019 save format is read too.

`--outfit NAME|PATH` puts an outfit on that the avatar's prefab doesn't have yet, the way dragging
it onto the avatar and running MA's *Setup Outfit* would. It works whether or not the outfit is set
up for MA. It finds the outfit's hips, works out the prefix and suffix of its bone names, and
matches bones with MA's name table. It turns A-pose arms to the avatar's pose and warns when bones
are more than 1 cm off. An outfit set up with VRCFury is put on as it is, and its Armature Link
does the rest. `--outfit` can be given more than once. `--list` lists the avatars found,
`--avatar NAME` picks one, and `--max-texture N` caps the texture size (default 2048).

**Emotes and dances.** Humanoid clips in the avatar's Action layer (its own, an MA Merge Animator's
or a VRCFury Full Controller's) become VRM animations, `OUT.<name>.vrma`, listed in the settings
file's `emotes` and named after the menu item that plays them. Unity keeps such clips as muscle
values. The converter turns them into bone turns for the avatar's own T pose, from its model's import
settings, following Unity's humanoid as lox9973's ShaderMotion and uvw.js describe it. The body's
motion moves the hips. Keys with weighted tangents are Unity's Bezier spans. A state with Foot IK on
(`m_IKOnFeet`) plants the feet where the clip's foot goals say, by two-bone IK on the legs. Clips'
translation curves (Translation DoF) only apply to avatars that enable it, as in Unity; the converter
warns for those. Shape-key curves such as MMD's faces (あ, まばたき, 笑い…) set the avatar's
shape keys of the same names, else the matching VRM expression. A motion sold for any avatar, set up
with MA like VRSuya's, goes on with `--outfit`. One sold as bare clips, for you to put in your Action
layer yourself (no prefab or menu), goes on with `--emote`: a `.anim` file, a clip by name, or a
package, zip or folder. For a package, every humanoid clip in it becomes an emote, named after the
clip with its words spaced out, and looping if the clip loops. A looping clip's "Loop" is left off,
so `FreddyFazbearPumpItUp_Loop` becomes Freddy Fazbear Pump It Up, unless the clip is one of a set
(Thumbs up Entry, Thumbs up Loop). A package's still poses (a "proxy" standing pose, say) are left
out when it also has clips that move. Name one with `--emote` to have it anyway. Both options can be
given more than once:

```sh
python3 tools/unity2hypr3d.py Avatar.zip --outfit VRSuya_Doodle_Dance_Released_260709.zip \
    --emote pHMToothlessDance.zip -o me.glb
```

A bare clip's song goes with it when the package puts one beside it: the sound file of the clip's
name, or its folder's one sound file when the clip is the folder's one clip. pHM's Freddy Fazbear Pump
It Up comes with `FreddyFazbearPumpItUp_Loop.ogg` (47.2 s, eight times round the 5.9 s dance), which
is copied next to the GLB as `OUT.Freddy Fazbear Pump It Up.ogg`, the emote's `"sound"`, and hypr3d
plays it with the dance (see Avatars). Only Ogg Vorbis is carried over (it's all hypr3d plays); another
kind is left out with a warning. VRSuya's dances come without their songs, so play those yourself.
VRSuya's Booth pages name them: "Doodle" by Zachz Winner for Doodle Dance, and しぐれうい's 「粛聖!!
ロリ神レクイエム☆」 for Loli Kami Requiem. A dance's `speed` in the settings file matches it to the
song's tempo. The Doodle Dance page suggests about 97.7%. At `"speed": 0.977`, the dance's 22-frame
bounce lasts 0.375 s, one beat at 160 BPM.

Not converted: shader effects beyond the above (rim lights, a toon shader's second and third shade
steps, shade and matcap masks, Poiyomi's other lighting types and matcaps, its other outline modes and
the like), constraints, particles, audio and contacts. Outlines take their colour
textures (UnlitWF's custom colour, lilToon's outline texture, Poiyomi's outline texture). MA's World
Fixed Object is held in the world where its rest pose was when the avatar appeared (MA fixes it to the
world's origin; hypr3d's worlds start you at theirs), the settings file's `"fixed"`. World Scale Object
and Convert Constraints are left out quietly (constraints aren't converted, and hypr3d doesn't scale the
avatar's world), and so are Visible Head Accessory, Mesh Settings and MA's VRChat-only settings,
since hypr3d has no use for them. VRCFury leaves out SPS, TPS and OGB (hypr3d has no contacts or
haptics) with a warning.

VRCFury features for VRChat's own systems change nothing hypr3d shows: Advanced Collider, Avatar
Scale, Toes, Talking, Cross Eye Fix and the like. Security locks are taken as unlocked. Blender
imports only binary FBX files.

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
blended layers, and puts decals back on what they're painted on (Source 2 Viewer 20's glTF export lifts
them 39 cm off it). Materials are tinted as CS2's shaders tint them: only where a tint mask says (de_dust2's
doors, shutters, curbs and awnings), not at all with `F_NOTINT`, with their decal textures (grime, stencils)
over them, and with Hammer's vertex paint inside the tint. A tint set on a mesh in Hammer is stored linear,
which Source 2 Viewer's glTF export takes for gamma and darkens a second time (de_dust2's dust sheets, clouds,
bombsite sprays and skybox windows; Mirage's clouds and sun glow): cs2map reads the draw calls' and the merged
props' own tints and puts them back as CS2 draws them. Walls and ground keep their first layer's tint (on one
layer too: Mirage's tan plaster), and blended ones each layer's, the band CS2 tints along the edge between the
layers, and their texture transforms. Unlit materials keep their blend mode and second texture (de_dust2's
clouds add to the sky), and textures that CS2 tiles or scrolls (the material's own transform, and its
DynamicParams at run time) do so too. Fog stays off where a material turns it off, and a mesh's lightmap uvs
are the set after those its material reads itself.
See `python3 tools/cs2map.py --help` for `--spawn`, `--desktop` and the rest. The maps are
Valve's; this reads your copy of the game for your own use.

### tools/blend2vrma.py and tools/blender: animations from Blender

```sh
blender -b FILE.blend --python tools/blend2vrma.py -- OUT.vrma --humanoid AVATAR.hypr3d.json [--frames A B] [--bones upper]
```

`blend2vrma.py` writes a humanoid armature's animation in Blender as a VRM animation (`.vrma`), which
hypr3d plays on any humanoid: an emote (`avatar_emotes`, a settings file's `emotes`) or an attack. It
goes through the scene's frames as Blender shows them (constraints and IK included), takes each
humanoid bone's turn from the armature's T pose as hypr3d works that out, and puts it on a T pose of the
armature's own proportions, straightened and rounded to the centimetre, with no turn at rest: the
animation, not the avatar it was made on. Which bone is which comes from a settings file's
`humanoid` (the converter writes one), else the bones' names. `--bones upper` leaves the hips and legs
out (players leave bones with no curves as they are: an attack goes over the walking); the fingers only
with `--fingers` (else the avatar's own gestures make the hands). The scene's markers go in the
animation's extras (`{"markers": {"hit": 0.2, …}}`, seconds). From Blender's own Python it's
`import blend2vrma; blend2vrma.export(OUT, armature=…, humanoid=…, frames=(A, B), bones="upper")`.

`tools/blender` has what the attack's punches were made with: a rig (IK targets for the wrists, poles for
the elbows, the trunk keyed, poses in the avatar's own terms), the hook's key poses, the scene built
from nothing and an exporter; its README says how to change the punch and see it in the plugin without
a rebuild. The Blender file itself, with Miku in it, is kept out of the repo.

`tools/blender/h3d_walk.py` makes the walk and the run (the built in `assets/walk.vrma` and
`assets/run.vrma`, or an avatar's own for its settings file's `walk`): two actions, Walk and Run, keyed
on a T-posed humanoid in Blender, one stride each, in place, looping. Its numbers (the hips' turn, drop
and sway, the trunk's lean and counter turn, the head's nod and tilt, the arms' swing, bend, follow
through and drag, in degrees and metres) are at the top of the script; the actions' keys can also be
edited by hand, then exported again. Their legs step as the walking's (the same foot roll and lift), so
a cycle reads as a whole in Blender, but the plugin keeps its own stepping.

### tools/test: the tests

- `tools/test/harness/`: `shot`, an offscreen render harness. It drives the plugin's real renderer,
  avatar animator and Action Menu on a surfaceless EGL context and writes PNGs, with no compositor
  involved. `tools/test/harness/build.sh` builds it into `build/test/shot` through `./build.sh`,
  linking the plugin's objects except `main.o`, `panels.o` and `mic.o`. Its arguments run in order:

  ```sh
  build/test/shot --size 960x720 --avatar me.glb --frames 30 --out front.png --view 90 --out side.png
  build/test/shot --avatar me.glb --physics 1 --accel 40 --move 0 3 --frames 60 --view 90 --out walk.png --swing
  build/test/shot --avatar me.glb --menu outfit --toggle Hat off --expr Smile --out menu.png
  build/test/shot --map ~/.local/share/hypr3d/maps/de_mirage.glb --stand 0 0 0 90 0 --autoexp 1 --out mid.png
  ```

  `grep 'a == "--' tools/test/harness/shot.cpp` lists every option. There are options for the camera,
  motion, faces, gestures, toggles, sliders (`--slider NAME X Y` for a two-axis one), emotes, the
  menu, lip sync from a WAV file (`--audio FILE`, `--visemes`, `--badge`, and `--lipsync-trace`: a
  line for each analysis window, its loudness, voicing, formants, fricative bands and visemes),
  outlines (`--outlines 0`), maps (`--no-dual`: glass without blending's second source, as where the
  GPU has none), timing (`--bench`) and debugging (`--hide`, `--show`, `--hide-avatar` (the avatar's
  materials by name: the hair, to see the legs), `--glinfo`, `--where NODE`), and
  for walking a line a frame of the gait (`--trace FILE`, with where what the legs carry is and how
  fast it goes: `carry`), every humanoid joint in the world (`--bones FILE`) and how deep each forearm
  and hand go into the body or a skirt (`--clip FILE`), how much of the hair (what springs swing under the head) is in
  each arm and how deep (`--hairclip FILE`: as round as the arm's skin goes out at rest, a sleeve with it; the
  deepest's node too), and `--wrists` (each wrist and shoulder from the feet); `--attackclip FILE [FILE]` gives
  the avatars loaded after it those attacks (a punch, and first person's) over the settings file's and the
  built in ones; for springs, each one's bones, colliders and limit
  (`--springs`), how far its bones are out of their limits (`--limits`, worked out on its own) and how much
  deeper than the animation has it physics puts each kind of spring into the body (`--springclip FILE`:
  round a vertical line through the hips; `--bodyclip FILE`: into each bone's skin as it is at rest, round
  a line through that bone, carried as the bone is now, so a dance's lean or a fall counts too, and which
  bone's), every spring bone and collider in the world (`--springdump FILE`: a line a frame),
  how each spring bone swings on what it hangs from (`--springtrace FILE PART`: a line a frame, the tail
  of each one with PART in its name in its parent's frame, after a line of what kind of spring each is),
  and `--view-chest DEG` puts the camera round where the chest faces (a dance turns it). `--gaitstyle 0` walks without the
  walk's and run's clips (the procedural body, to compare; `1`, the default, with them). `--face 1` turns the body to where it goes as
  third person does, and `--look-cam 1` turns the head as third person does too (a camera behind, looking
  -z: where it looks, back at it past 100°, into a turn while the body turns far).
  `--fly 1|0` flies as the plugin's F does (going on as it was going; out of it above the ground, it
  falls) and `--movey M` is the speed up or down the keys ask for flying (the plugin's Space and Ctrl,
  8 m/s). `--walk` moves the avatar through a map with the plugin's own body (src/walker.cpp: up its stairs,
  down off its ledges, the smoothed height the camera and the avatar go at), `--walklog FILE` writes a
  line a frame of that body, how high each heel and ball is over what's under it and the gait's state,
  and `--floors X0 Z0 X1 Z1 STEP FILE` every floor's height over a grid (to find a map's stairs). `--ctl`
  runs a `hyprctl hypr3d avatar …` or `menu …` request, and `--key`, `--click`, `--wheel` and
  `--mouse` give the Action Menu the plugin's input: the same code as the plugin's (`src/control.cpp`,
  which `main.cpp` hands its commands and menu input to).
- `tools/test/harness/ctl_check.sh DIR`: those, checked on the MA accessories and dances regress.sh
  converted (its `--keep`'s `OUT/new`): sliders by number and percent, a two-axis one, a toggle and
  its material variant, the menu's pages, a dial turned by the mouse and the wheel, a stick, the main
  page's nine (the ninth by the wheel and the mouse), an emote at twice its speed, and an emote's
  sound listed with it (one that isn't there said in the log; ffmpeg makes the test sound). It runs at the
  VM's 1280×800, and the harness lays its menu out for its `--size` before each option, as the plugin
  does every frame, so the dial and the stick come out where the VM test has them.
- `tools/test/harness/toon_check.sh [DIR]`: toon shading and matcaps on `toonballs.py`'s six balls
  (a plain one, MToon 1.0's, VRM 0.x MToon's, the converter's extras with and without a matcap, and a
  matcap alone), side on to the sun: the plain ball's light falls off with N·L, a toon ball's is flat
  on each side of a sharp step, each shade has its own colour, a matcap brightens where it's white,
  and a toon ball in a wall's shadow is all shade.
- `tools/test/harness/fp_check.sh AVATAR [DIR]`: first person with the avatar's body, through the
  harness's `--fpbody YAW PITCH` (the plugin's code: `--fphands ready|touch|type|hold|down`, `--fppress`,
  `--fptap left|right`, `--fproom M` for a wall that near, and `--fpstatus`, where the camera and each
  wrist are in the view; `--fpturn YAW PITCH` turns the camera that many degrees a second, as the mouse
  does, and `--fpfollow 1` turns the body after it as the plugin does), on any humanoid (Hatsune Miku NT,
  or regress.sh's BoothAccessories): the camera in its eyes and still standing, nothing of its head in
  the view (its top half as with no avatar), the hands up low in it, staying there walking and pumping
  running, letting go looking far down, the right one up to the crosshair touching, nearer together
  typing, up and out holding, gone let down, a gesture held up, and kept back from a wall.
- `tools/test/harness/attack_check.sh AVATAR [DIR]`: attacks, through the harness's `--attack
  left|right|next` (the plugin's left click on nothing) and `--attackstatus`, on any humanoid, measured in
  its own arms' lengths from its shoulders: a click swings the right arm, the fists closed, wound up out
  past the shoulder and ahead of it (seen from behind: two frames from third person's camera differ there),
  swept round in front of the face as the chest turns left, the left fist up by the chin, then both back
  where they hung; clicks a quarter of a second apart go one arm then the other (R L R L…), quicker ones
  one at a time, at least 0.18 s apart, after a pause the right again; no wrist faster than 28 arms'
  lengths a second or its speed changing by more than 1100 from frame to frame (one punch taking over from
  another too); the hair (`--hairclip`) no more than a strand's tip deeper than 3.5 cm in an arm while a
  punch has the body (the old swing sank 7 cm into Miku's twin tails), none deeper than 5; walking it
  goes on walking; in first person the fist drawn back to the bottom right of the view, struck to the
  crosshair and back to where it was held, the left held ready, and no further ahead than a wall 30 cm
  ahead allows; an emote stops; a model that isn't a humanoid has nothing to swing, and `--ctl "avatar
  attack …"` (hyprctl's) says so, and how it goes. Hatsune Miku NT, AliciaSolid, Seed-san, the VRM 1.0
  twist sample, Polydancer and BoothAccessories pass.
- `tools/test/harness/spring_check.sh AVATAR [DIR] [KIND...]`: the avatar's springs standing, walking and
  stopping, running and stopping, turning on the spot, jumping and dancing (and turning right round and
  running in first person, with a harness that has those), at the plugin's speeds and accelerations: no
  bone with a limit gets out of it, and the kinds of spring named (a spring's name up to its first `.`)
  never go more than 2 cm deeper into the body than the animation has them; and every kind swings as
  smoothly at 143.9 frames a second (a 144 Hz monitor's) as at 60, in first person turning too (its
  wobble about its own smooth path no more than half as much again: shown in between steps as they were
  going, Hatsune Miku NT's sleeves shook 0.2–0.3° as first person turned, over 100 times as much as at
  60). It prints how deep every kind went in each. Hatsune Miku NT's necktie:
  `spring_check.sh ~/.local/share/hypr3d/avatars/Miku/Miku.glb "" Necktie` (converted before PhysBone
  limits were carried over, it went 6.7 cm into her running).
- `tools/test/harness/disc_check.sh [DIR]`: the settings file's disc colliders and a spring's radius
  bone by bone, on a model it makes of short chains by level discs, each pulled into its disc by its own
  gravity: one lying over a disc stays on its top (a ring of capsules let a tail over a tutu through), one
  under one stays under it, one beside two is kept out of an edge, each joint out as far as its own radius
  from the first step on; the radii come out bone by bone as given, and a chain with no disc hangs on
  down. With the discs read as spheres, as an older hypr3d reads them, the chains go through and it fails.
- `tools/test/harness/cs2mat_check.sh [DIR]`: CS2's material details as `tools/cs2map.py` writes them,
  on `cs2mats.py`'s panels, each split by its textures: the tint only where the tint mask is, a decal
  multiplied on the second uv set and one mixed in by its alpha, an unlit colour times its second
  texture, an unlit one added to the wall, past the fog a quad with its fog off that stays red and
  added light that fades out; and vertex paint only where the tint mask is (all 0 is none), and unlit
  mod2x in linear light (sRGB 188 leaves the wall as it is, 128 darkens it).
- `tools/test/sound/sound_check.sh [--song FILE.ogg]... [WORKDIR]`: emotes' sounds on their own
  (`src/sound.cpp`'s decoding and `src/speaker.cpp`'s playing, built with `sound.mk` into
  `build/test/sound_test`), against a PipeWire of its own (`tools/test/sound/pipewire/`: no devices
  and no session manager, a timer driving it; nothing is heard and the desktop's PipeWire isn't
  touched). Test sounds made with ffmpeg decode to as many frames as the file says (ffmpeg's own
  decoding leaves a block off a mono one) and to ffmpeg's samples within one step of 16 bits; Opus,
  three channels, junk, an empty file and a missing one are refused, saying why. Played into
  `pw-record` (linked by hand), what comes is the decoded sound sample for sample: from where the dance
  would be by the time it's heard, at the volume asked (the 20 ms ramp up to it too), round its end
  as it loops, a fade over exactly the time asked when stopped, the sound's last sample when it ends
  by itself (the stream waits till it's heard before it closes), then silence; mono, and 44.1 kHz
  (resampled) as loud as it is. The clock the dance keeps time by never goes back, is where the
  dance is by its own time within a few ms and goes at the rate time does (0.2 ms off a straight
  line); one never linked has none. `--song` (Freddy's) is decoded against ffmpeg's and libvorbis's
  decodings and played round its end. Needs ffmpeg, pipewire, pw-record, pw-link and Blender.
- `tools/test/tiling/run.sh`: tiling mode's ring on its own (`src/tiling.cpp`, built with
  `tiling.mk` into `build/test/tile_unit`): the row left to right, the gaps, centred where you looked,
  each window facing the middle and as big as on the screen or made smaller to fit the view, many
  windows made smaller alike, third person's windows on the ground, where a window carried to a yaw
  goes in the row, where you're at a ring that stays where it is (within its radius of its middle, up
  and down too) and where you're away from it, and what the ring stands on as it goes with the
  plugin's body (`walker.cpp`, linked in): not up with a jump (at 144 frames a second and at the
  plugin's longest step, 50 ms), down with you walking off a ledge, up onto one you jump onto a moment
  after you, up and down stairs with you, with you flying up and falling out of the air, and at once
  where you're put.
- `tools/test/fuzz/fuzz.py map|avatar OUTDIR [-n N] [--avatars DIR]` (and `replay CASE`): feeds the
  map and avatar loaders broken files, and keeps what crashes them, trips AddressSanitizer or
  UndefinedBehaviorSanitizer, hangs or runs away with memory, with its input. It runs a harness built
  with the sanitizers (`./build.sh -f tools/test/fuzz/asan.mk` makes `build-asan/shot`) on llvmpipe.
  The seeds are `litmap.py`'s LitCourt for maps, and for avatars BoothAccessories as it is, as a VRM
  0.x and as a VRM 1.0, and `assets.py`'s ToonTest; a case mutates a seed's JSON, its binary data, its
  settings file or its emote file. See the script's header for the rest.
- `tools/test/harness/lipsync_check.sh AVATAR.glb [WORKDIR] [--real DIR [PERCENT]] [--levels "0 20 30 40"]`:
  lip sync on vowels `tools/test/synth/vowels.py` sings (a source-filter model of a man's and a
  woman's a, i, u, e, o), silence, hiss, a quiet voice with nothing heard before it, hiss right after
  a vowel, and an s, sh, f and m between two a's (each must show its consonant viseme and no other),
  and the avatar's own s viseme following an s. Then all of it as quieter microphones give it
  (`tools/test/synth/attenuate.py`: each file after a second of silence, `--levels` dB down): the
  automatic gain must open each vowel as wide as at its own level (0.05 less at most), and silence,
  hiss and the hiss after a vowel must stay shut. And a whisper (40 dB down): right after a normal
  voice it stays shut, while on its own, or 16 seconds after the normal voice, it opens once the gain
  has heard it. `--real DIR` adds recordings of real voices, named for their vowel (`a_*.wav` …
  `o_*.wav`): how often each one's viseme leads, how open it is, and how much of it counted as voiced;
  at least PERCENT (85) of them must lead with their own, at their own level and at each of the
  levels, where each must open as wide as at its own level. The ones used here are in `extras/speech/in`:
  from Wikimedia Commons, Lingua Libre and Tofugu/WaniKani, public domain, CC0, CC BY and CC BY-SA
  (`extras/speech/LICENSES.md` credits each).
- `tools/test/synth/attenuate.py OUT.wav DB PART... [--noise DBFS]`: WAVs and `silence:SECONDS` one
  after the other, DB decibels down, as a 32-bit float WAV, with white noise all along if asked (a
  microphone's own hiss): how a quieter microphone gives a recording.
- `tools/test/vm/run.sh [--only ITEMS] [--gpu virgl] OUTDIR`: the plugin in a real Hyprland, in NixOS
  VMs (`vm.nix`) built the way Hyprland's own CI tests Hyprland: QEMU with KVM and a virtio GPU that
  Mesa's llvmpipe draws for, the Hyprland you run (the one `build.sh` builds against, or `HYPR_BIN`'s)
  started as a user's login session on the VM's tty1, and PipeWire with a virtual microphone. QEMU
  opens no window. The NixOS test driver then runs `checks.py`, a checklist, in a VM at 1280×800 and then in
  one at 1920×1200 (a virtio GPU only takes the mode it's given):
  - loading the plugin with `hyprctl plugin load`, with `hl.plugin.load` in a Lua config and with
    `plugin =` in a `hyprland.conf`, unloading it (in 3D too) and loading it again, and that
    Hyprland exits cleanly with it
  - the config values, set in the config, at run time, and through `hyprctl keyword`; the Maps page
    (a maps folder's files and folders, the configured map at its `map_scale`, the courtyard, the map
    you're on picked while another loads); the Avatars page (the main page's ninth, and 9 opening it,
    an avatars folder's files and folders but not its `.vrma` emotes, the configured avatar,
    `avatar_height`, the avatar shown picked again and while another loads)
  - input from the VM's own keyboard, PS/2 mouse, USB tablet and wheel (QMP input events, so they
    pass through libinput and Hyprland's input stack to the plugin's hooks), and from a mouse with a
    high-resolution wheel (`wheel.py`, through uinput): the Action Menu, a slider's dial and a
    stick, F1–F8 with and without the Shifts, Super and Ctrl+Alt shortcuts, and clicking and typing
    into a window in 3D
  - what a window gets in 3D, seen by `wev`: the pointer entering, moving (surface-local, where the
    crosshair is), leaving, the buttons, keys only while you type, and the wheel exactly as on the
    2D desktop: whole notches and half ones, both wheels, with a mouse's own `scroll_factor`
    (`hl.device`), a window rule's (`scroll_mouse`, `scroll_touchpad`) and
    `input:emulate_discrete_scroll` at 0 and 2, and a touchpad's two-finger scrolling (`touchpad.py`,
    through uinput: finger scrolling that stops, both axes in one frame)
  - carrying windows: G, a left or right click, Esc and X, the wheel and Ctrl+wheel while holding
    one, `hyprctl hypr3d grab`, `place`, `hold`, `reset-windows` and `windows`, and a placed window
    that keeps drawing when its workspace is hidden; in third person: carried past the avatar (made
    bigger, pulled in, one that was nearer, onto a wall), as big on the wall as it was carried, and a
    window opening in third person as big as the camera needs, standing on the ground
  - with a shell's see-through overlay over the whole screen (as quickshell's): the crosshair and
    clicks going through it where it takes no input; H picking a window up and putting it down where
    you look, out in the air and flat on the courtyard's gate; Shift+H pinning one and taking it back
    into your hands (Esc: back in the corner), the one carried, two pinned, the Action Menu open, and
    `hyprctl hypr3d grab` and `pin`
  - a second monitor (Hyprland's own headless output): 3D on one while the other stays 2D, then on
    the other; the mouse, the focus, notifications on the focused one, and a monitor going away in 3D
    (and the plugin holding its output for aquamarine's queued frame, below)
  - 3D on the monitor `plugin:hypr3d:monitor` names (or `desc:`, or `hyprctl hypr3d on MONITOR`)
    from the other, the cursor and the focus with it; the other monitor used meanwhile: Super+Esc
    and back, a `movefocus` keybind each way (to a monitor with a window, and to one without), the
    mouse moved over and back, `hyprctl hypr3d away`, the Action Menu's keybind coming back; what a
    window there gets meanwhile (wev: the pointer, a click, keys and Esc, the wheel), Hyprland's
    cursor and notifications there, 3D drawing on; leaving 3D, and the 3D monitor going, while away
  - scales 1.5 and 2: the frame, the crosshair, the Action Menu and the badge drawn at the monitor's
    scale, the dial's mouse counts, aiming, clicking and typing, and the cursor hidden
  - sliders, a material variant and an emote's speed through hyprctl; the Lua functions, the
    `hypr3d:toggle` and `hypr3d:menu` dispatchers, from hyprctl and from keybinds, and `hypr3d:tile`
    (and `hypr3d:tile follow`) from hyprctl
  - stencil eyes and outlines on `assets.py`'s ToonTest.glb, and its TestRoom.glb as a map
  - a map with a game's own lighting, as `tools/cs2map.py` writes one: `litmap.py`'s LitCourt.glb, made
    up here, with two lightmap sets, light probes, the sun's baked shadow, fog, a sky, an exposure range,
    a tone curve, Source 2 materials (glass, decals, detail textures, self-illumination), a blend layer,
    a backdrop and block compressed textures, each checked in a frame, and the sun glinting off its
    glass; and LitCourtRuntimeSun, the same with a sun that has no baked shadow channel, which lights
    the lightmapped floor as the probe-lit props (only the realtime shadow map shadows it)
  - that Hyprland draws its notifications over the 3D view, that the lip sync badge moves below them,
    and that Hyprland draws its windows (rounding, blur, borders) as before once 3D is left
  - lip sync through PipeWire: `pw-cat` sings the vowels into the virtual microphone (over its own
    hiss, white noise at −75 dBFS), at their own level and 30 dB down, and the visemes, the gain
    (automatic, 0 dB, 30 dB, and the Mic gain dial), the badge, the "hypr3d lip sync" stream in
    `pw-dump` and its going away when you leave 3D or turn lip sync off are checked. So is what the
    report and the badge say when the microphone is muted in PipeWire, sends only exact zeros (as one
    muted by its own button does), is suspended, goes away while it's the default, isn't there when
    `lipsync_source` names it, or when the stream is left unlinked (WirePlumber's metadata)
  - an emote's sound through PipeWire: a looping emote with a song (two tones made with ffmpeg) plays
    it as the "hypr3d emote sound" stream, named for the emote and linked by WirePlumber to the default
    output (the virtual microphone's sink, recorded from the microphone): from the start, the dance's
    time the song's clock (a frame behind at most), round the song's end with no gap, the two tones as
    loud as each other, `emote_volume` 1 twice as loud as 0.5 and 0 no stream; it fades out with the
    emote, starts again when it's played again, stops at once out of 3D and comes back in where the
    dance is
  - play mode (`h3dgame.c`, a small SDL2 game that prints what it gets and draws it): its pointer
    lock, relative motion to the count, keys (Esc and Tab too), buttons and the wheel reaching it and
    not the player, the camera facing it and back (Shift+P), the pointer over it without a lock and
    kept on it, its own cursor, fullscreen, Super+Esc, leaving 3D, a screen lock (swaylock, unlocked by
    typing into it), and a game controller (`gamepad.py`, an Xbox 360 pad through uinput) read by SDL
    while it has the keyboard focus, and not once it hasn't
  - play mode in place (P): two games among two terminals on the wall, the camera not moving at all
    (its eye, yaw, pitch and up over two seconds, `hyprctl hypr3d camera`), the other windows in the
    view (`panels`' `inView`) and the frame as it was before P, no crosshair, the notification naming
    Shift+P; keys, buttons, the wheel and relative motion reaching the game that locks the pointer, a
    pointer over the other one kept on it with its own cursor and a click where it is; Super+Esc
    walking on at once from where you were; the same in third person; in tiling mode's ring with its
    neighbours in the view, and a window opening going into the row beside it, the game and the view
    staying where they are; Shift+P filling the view, `play_view = fill` swapping P and Shift+P,
    `hyprctl hypr3d play on here|fill`, switching while playing, `hl.plugin.hypr3d.play("fill")` and
    `play("here")`, and the `hypr3d:play` dispatcher with a word (in a hyprland.conf session). A
    window played here the moment one filling the view stops: the camera coming back the way it went,
    never off towards it, and then play fill going straight to it; Play on the Windows page on a game
    behind you, first and third person, and a game going fullscreen behind you: you turned to face
    it, its middle in the view, the eye where it was (out of fullscreen, still played, the pointer
    still on it, no mouse leave); a window pinned to the view played in its corner, and filling the
    view the camera staying at it (not going on after it), pinned in the corner again after;
    `play_view` set to a word it doesn't know said once, P playing here
  - a game played here in tiling mode's ring (`play_size`): a 1200×750 game among four small
    terminals, P with the crosshair off its middle taking half the view (from `hyprctl hypr3d
    windows` and `camera`, and in the frame), the row turned so its middle is the view's, up and down
    too, the camera still and the terminals beside it in the view; Super+wheel up and down (the game
    getting none of it, a log line a notch and a notification a turn of the wheel), 94% of the view
    at the most (more than the ring gives a window, its middle the frame's) and a quarter of it at
    the least, a high-resolution wheel's half notches; the wheel alone the game's; Super+Esc its size
    back with the row left turned; Shift+P filling the view as ever; `play_size` set with `hyprctl
    eval` and P at once, and out of range (1.5, −inf, NaN); looking 8° up and 10° down as P plays it,
    its middle where you look; a ring that stays (Y) with you off its middle; a window opening while
    a game is played going right of it, the game staying in the middle; a crowded ring (seven
    terminals as big as the game), the game half the view all the same and the others smaller round
    it; third person, half the view (not held to what a window standing on the ground there gets) and
    Super+wheel up to 80%, and looking down; and a game played out in the world (Super+wheel scaling
    it where it hangs) and on the desktop wall (nothing, logged once)
  - the game played in the ring as the camera draws it (`play_size`): half the view looking level,
    25° up and 25° down; in a ring that stays (Y) with you 1 m from its middle towards it (made
    smaller: it's nearer); third person with the avatar's back to a wall, the camera's boom pulled
    in, half the view and 94% at the most (not more); after V; away from a ring that stays, played as
    the ring has it (Super+wheel nothing there, logged); and its play ending by itself as a terminal
    opens and takes the keyboard, the game keeping its size and place, the terminal going in beside
    it, and half the view again when the keyboard's back with it
  - the hold on shortcuts and the game's keyboard given back: a window 3.6 s slow to close, Super+Q
    pressed again just after it went (walking, and with a game played), and twice on it over a game
    played with the game's keys pressed meanwhile, only it closing; Super+Q twice on a popup over the
    game, then an empty workspace (the monitor staying there) or a launcher open past the hold's end
    (on demand and exclusive: it keeps the keys, and after Esc the game's played); Alt+1, a game's
    key held back, not holding a popup closing by itself; the game fullscreen with its keys' hold run
    out, Super+Q twice on a popup then a turn; and a game played in and out of fullscreen with the
    mouse still, a pointer lock too, off its middle and with Hyprland's animations on, never getting
    a mouse leave
  - what a game needs from the compositor: the activated state (`wev`), `wl_output` enter and
    presentation feedback presented, not discarded (`weston-presentation-shm`'s own protocol log),
    its idle inhibitor on a hidden workspace (`swayidle`), and its frame rate following the 3D view's
  - X11 apps through XWayland: a Tk app (`tkapp.py`) clicked, typed into and scrolled, its menu bar's
    menu, right-click menu and tooltip (override-redirect windows) as popups, placed in the world too;
    SDL's x11 driver in relative mode getting in 3D what it gets on the 2D desktop; xterm's own cursor;
    at scales 1, 1.5 and 2
  - an X11 game the way CS2 runs (SDL3's x11 driver, fullscreen, opened in 3D after a move of Hyprland's
    that no frame followed was left on an xterm on the 2D desktop, its menu's pointer, then mouse-look
    turned on): every move of the mouse reaching its mouse-look, played here and filling the view; and
    xterm's own cursor drawn, not Hyprland's resize arrow, when 3D comes in with the cursor on a window's
    edge
  - apps: the Apps page (desktop entries written for the test, their icons), launching into the world
    (the built-in rules for a game and a chat app, as big as on the screen; config rules for commands,
    with a height and with `auto`; a terminal opening in 3D on its own), places remembered by
    class and restored (and not used while out of sight: in front of you then), X forgetting one, the
    Windows page, pinning, real sizes (Bigger, Shift+wheel), and fullscreen from an app starting play
    mode, which leaving fullscreen doesn't end (Super+Esc does)
  - flying and jumping: F then Space up into the air and hovering upright, W flying lying along the
    way it goes, letting go leaning back as it slows then hovering again, F flying ahead falling on
    ahead (not stopping dead) and landing on its feet, and a jump from a run: a stride in the air and
    landing running on
  - typing (E): into the terminal under the crosshair, said so in a notification; walking again when
    it closes while typed into and when a new window takes the keyboard; E with no window to type into
    stays walking
  - first person with the avatar's body: the camera in its eyes at its own height and still standing,
    the hands up low in the view (where the status's `"hands"` says they are), staying up walking and
    pumping running, letting go looking far down and coming back; a terminal pressed on (the right
    hand's finger to the crosshair), typed into (the hands nearer together), carried (both out to it)
    and played here (the hands gone from where they were in the frame, the camera not moving); an emote
    (the camera out behind the avatar and back into its eyes); `hyprctl hypr3d view body off` and
    `first_person_body` (1.65 m up, no body, as the other sections have it: they turn it off)
  - attacks: in third person a left click on nothing swings the right arm with the fist closed and the
    avatar standing where it was; three clicks a third of a second apart go R L R, and after a pause the
    right again; with the Action Menu open, on a window (wev gets the button), typing into it with the
    crosshair turned onto nothing, carrying it and playing it, no swing; in first person the fist drawn
    back at the right of the view, then across its middle (the status's `"hands"`); `hyprctl hypr3d
    avatar attack left` and a wrong word; weston-terminal's menu open (a popup that grabs the pointer), a click on nothing closes it
    with no swing and the next swings; ToonTest.glb (no skeleton): nothing, Hyprland fine
  - tiling mode: T with three terminals opened in 3D and one on the wall (all four in the row, in the
    order they were round you, facing you, as big as on the screen or fitting the view), one opening
    where you look, one carried past the row's end and put down there, Esc, one put on a wall and one
    sent back with X (both out of the row, it closing up), put somewhere else (the ring there at once)
    and Shift+T, the ring going with you (walking: each window the same way round you, in the frames
    while walking too, none lagging; a jump: it stays down; flying up over the yard's walls: round you
    in the air; a window opening up there and one carried and put down in the air: into the row round
    you; landing: down with you), the Windows page's Tiling (each going back where it was before, the
    new one staying), `hyprctl hypr3d tile`, `hl.plugin.hypr3d.tile()`, the `hypr3d:tile` dispatcher,
    `plugin:hypr3d:tiling` set at run time, 3D left and entered again tiling, and third person's ring
    (going with the avatar, fitting a longer boom, V to first person round your eye)
  - tiling mode's ring staying where it is (Y): said so, with you turned away from the row's middle
    and nothing moving, the ring and every window staying put as you walk up to the row, a window
    opening inside the ring going into the row where you look; out of the ring a window opening in
    front of you, one Bring here brings and one played from another workspace coming in front of you,
    and one carried out of the row with no room made for it and put down in the air staying there, all
    out of the row, the ring not moving (and one carried out there and let go of with Esc going back in
    its place in the row); Shift+T there bringing the ring round you, still staying; Y again taking it
    with you (round your eye at once, the row the same way round you, the windows flying over to it
    rather than jumping) and walking taking it along; tiling off leaving those out of the row where
    they are; `hyprctl hypr3d tile follow`, the JSON's `follow` and `atRing`,
    `hl.plugin.hypr3d.tile("follow")` and `tile("here")`, `plugin:hypr3d:tiling_follow` set at run
    time and in the config (`hl.plugin.load`'s section), Y with tiling off and T then staying, the
    Windows page's Follow me, 3D left and entered again staying, third person's V and wheel not moving
    it, and `reset-windows` leaving it staying
  - a game going fullscreen in 3D, with tiling on and off: the other windows staying where they are
    and drawn as they were (the row's, the wall's, a floating one, the world's, the bar on the top
    layer, and another game drawing on at the 3D view's pace), a maximized window too, and one
    fullscreen on the wall hiding only the wall's (a surface on the top layer mapped after it drawn
    over it, as on the 2D desktop); the game played by itself (opening fullscreen; when a window over
    it that took the keyboard closes by itself, at once, and with the game's keys' hold run out first
    too; a click on it while that window has the keyboard; Helldivers 2's way through Proton, an SDL
    game through XWayland whose second window had the keyboard as it went fullscreen, which Hyprland
    then gives to no window; and Wine's way with Tk, a client message after its dialog took the
    keyboard), its keys reaching it; Super+Esc keeping it unplayed while it stays fullscreen, T twice
    meanwhile, and out of fullscreen and back played again; a dialog of the game's own opening over
    it after play mode ended by itself keeping the keyboard, the game played with it and the keys the
    dialog's; a dialog going fullscreen played itself and staying fullscreen
    (`misc:on_focus_under_fullscreen` 2 and 0), Super+Esc, and `play off` after P on it, keeping it
    unplayed; with two monitors, the mouse away and back not playing the game (a click does); a
    launcher over the game played ending play mode, and the game played again when it closes; 3D left
    and entered again with it fullscreen (not played, the others drawn round you); and an X11 app out
    in the world keeping its tooltip (an override-redirect window) drawn over it when a window on the
    wall goes fullscreen
  - everyday apps, open-source stand-ins: Chromium and Firefox with `page.html` (a `<select>`'s list,
    a tooltip and the context menu drawn over the window, as popups or, Firefox's list and tooltip,
    subsurfaces; text selected by dragging copied to the clipboard and read back with `wl-paste`,
    typing, drag and drop in the page and, walking, out of it with the crosshair onto another window, a
    touchpad's scrolling and pinch, fullscreen, a file dialog opening by the browser, played along with
    it and closed with Esc); Electron (`electron/`, Discord's stack) as a
    Wayland and an X11 client, with its own context menu (a native one, clicked) and a notification
    (mako), and drag and drop through XWayland compared with the 2D desktop's, in a Hyprland of its
    own (0.55.2's XWM can keep an X window it never saw go, section 19's SDL game leaves one, and a
    tooltip given its id later is mapped as that window: centred, managed, not a popup); OBS (driven
    through obs-websocket by `obsws.py`): its menu, a dialog by it in the world, screen capture
    through xdg-desktop-portal-hyprland with its picker used in 3D, the capture showing the 3D view,
    and a placed window captured as a window
  - an input method: fcitx5's clipboard list (Ctrl+;) shown while typing into a terminal in 3D, its
    popup drawn over the window by the text, and gone with Esc
  - real open-source games: Chocolate Doom with Freedoom's levels (the pointer locked, turned by the
    mouse, walked by the keys; as a Wayland and an X11 client) and SuperTux (its menu by the keys and
    by the controller)
  - frame rates: the 3D view's with four windows and a game, in first and third person, playing,
    with a window pinned, the plugin's own time a frame, and the game's own frame rate behind it; and
    a fullscreen game with direct scanout allowed on the 2D desktop against the same game played in 3D
  - closing in 3D and a game's keys: Super+Q (bound to `hl.dsp.window.close()`, as a user's is) with
    tiling mode's row round you closing the window the crosshair is on, not the one opened or clicked
    last, and nothing with the crosshair on the sky; pressed again and again (a Tk window that takes
    1.3 s to close, as OBS does; a game played) closing only that one, and after turning onto
    another, that one; the Windows page's Close… asking first (B, a window's digit, 8, and 8 again;
    Tab and aimed clicks, and a click again), 5 closing, `hyprctl hypr3d window SEL close` at once;
    the log's lines for closes, the Action Menu, X and a shortcut's focus; play mode ending by itself
    (the game quitting, another window taking the keyboard) holding a gamer's keys back (B 2 8, Tab 6
    2 8, X, T and Esc doing nothing, W still walking), Super+Esc and 4 s without a key walking again;
    P (and `hyprctl hypr3d play on`), a click on the game and its window taking the keyboard back
    playing it again; a splash then the game's real window (the same class) played; a Steam game's
    window saying how to play it, with a helper window of it coming and going right after too;
    Super+Q with the crosshair on a window that won't take the keyboard (a `no_focus` rule) closing
    nothing; a game's two windows (one class): the one played closed by Super+Q pressed twice, or
    quitting, and the other not played nor closed for having the keyboard then; a popup of another
    app's over a game played taking the keyboard, Super+Q pressed twice on it closing only the popup
    (the game windowed, and fullscreen with its keys' hold run out), the game played again as you
    turn, as after Super+Q once, or with nothing pressed once the hold's 5 s are over (its keys held
    back till then), and at once when the popup closes by itself, a key typed half a second after
    reaching it; a launcher (Super+D) over the view for 5 s while playing, and the game played again
    after it; a game fullscreen with the ring's windows drawn under it (where they are), a shortcut
    on one leaving the game fullscreen; a window from another workspace out in the world, a shortcut
    on it switching no workspace and Super+Q closing nothing; and a scratchpad (a special workspace)
    open over the view, Super+Space, its toggle, with the crosshair on a window under it hiding it
  - `tools/test/live/check.sh`, below, run in the VM with its microphone prompts sung into the test
    microphone and `--app`'s steps done as they're asked for, and stopped with Ctrl+C halfway through
  - that Hyprland exits cleanly with windows open, in the dwindle and the master layout. Hyprland
    0.55.x doesn't: it crashes in both (with or without hypr3d), dwindle's fixed upstream in 0.56.0
    (commit 338bdbb3) and master's not yet, so these checks say "known" there instead of failing, and
    the other checks close the terminals before they stop Hyprland

  `OUTDIR` gets `results.txt` (a line per check), `results.json`, `frames/` (grim's frames from inside
  the VM), `logs/` (Hyprland's logs, the VM's journal, `lipsync.json`, `pw-dump.json`; `logs/hidpi`
  for the second VM), `live/` (the live check's results and frames) and `driver.log`. `--only 14,15`
  runs only those sections (after section 0, which starts Hyprland). Only synthetic things go into the
  VM: BoothAccessories converted from `booth.py`'s packages (or taken from `--avatars DIR`, as
  `regress.sh --keep` leaves them), `assets.py`'s two files, `litmap.py`'s two courts, the vowels,
  `wheel.py`, `touchpad.py` and `gamepad.py`, the test apps written here (`h3dgame.c`, `tkapp.py`,
  `tkfs.py`, `page.html`, `electron/`, `obsws.py`), the live check script and `hypr3d.so`; the apps it
  runs are open-source ones from nixpkgs (Chromium, Firefox, Electron, OBS Studio, Chocolate Doom with
  Freedoom's levels, SuperTux, fcitx5, xterm, Tk, weston's demo clients, swayidle, swaylock, mako and the
  portals).
  A run has about 900 checks and takes about 65 minutes (50 with `--gpu virgl`), nearly all
  of it in the VMs: Mesa draws in software there, at 6 to 22 frames a second (the lit map's first
  frames take a while more, as llvmpipe compiles its shaders). With `--gpu virgl` a GPU of yours draws
  instead, through virglrenderer on the render node
  `H3D_RENDERNODE` (`/dev/dri/renderD129` by default, an Intel iGPU here): QEMU's egl-headless
  display, which opens no window either. The first run builds the VMs in a few minutes and fetches
  about 410 MiB for them (1.3 GiB unpacked, mostly QEMU and a kernel) and about 890 MiB for the apps
  (2.2 GiB unpacked, most of it Chromium, Firefox, Electron and OBS), `--gpu virgl` about 200 MiB
  more (the full QEMU, 1 GiB unpacked); `OUTDIR/driver` keeps the VMs' closure (9.5 GiB, much of it
  already in a NixOS store) alive until `OUTDIR` is deleted. While it runs, the VMs' disks and the
  driver's sockets are in a folder under `/tmp` (`H3D_VM_TMP` picks another), deleted afterwards: the
  test driver puts them in `XDG_RUNTIME_DIR`, a small tmpfs that a core dump fills.
- `tools/test/live/check.sh OUTDIR [--mic] [--avatar FILE] [--map FILE|--no-map] [--app CMD]...`: for
  what only your own desktop can check: your GPU and monitor, your voice, and your own apps. You run it, in your Hyprland session,
  and don't touch the mouse or keyboard while it runs. It loads `hypr3d.so` and compares your desktop
  before and after, enters 3D on the focused monitor (and checks it keeps up with your monitor's
  refresh rate), loads an avatar (`--avatar`, else `assets.py`'s ToonTest) and looks at it, opens the
  Action Menu and plays an emote, shows a notification over the 3D view, picks up the window the
  crosshair starts on and puts it back, walks into your map (`--map`, else
  `~/.local/share/hypr3d/maps/de_mirage.glb` if you have it), leaves 3D and unloads the plugin. With
  `--mic` it turns lip sync on, writes what PipeWire says about your microphone (which one it's linked
  to, muted or not, what came from it) to `results.txt` and saves `wpctl status`, `wpctl inspect` and
  `pw-dump` in `audio/` (no sound), then asks you, in notifications over the 3D view, to hold a, i, u, e
  and o, then "sss", then nothing, and says which vowel it heard each time, at its loudest; exact
  zeros throughout get "your microphone sent only silence: is it muted?". Each `--app` (a desktop id, an
  app's name or a command: `--app discord`, `--app "steam steam://rungameid/APPID"`) is launched into 3D,
  and notifications ask you to play it (P) and stop (Super+Esc), type into it (E), point at it (its own
  cursor), carry it somewhere (H, and H again where it should go), pin it (Shift+H) and take it back
  (Shift+H again, then H), then to try what matters to you in it (P again, Super+Esc when you're done: a
  call, a screen share, OBS capturing the 3D view, a controller); frames of each step are saved, and
  its window is closed afterwards, as its close button does (a chat app goes to its tray, a game
  quits). A Steam game is the window Steam
  starts for it, not Steam's own. When the crosshair starts off any window (on the wallpaper between
  two), it turns to the nearest one first (`hyprctl hypr3d aim`). Each run goes into a folder of its
  own, `OUTDIR/run-1`, `run-2` … (`OUTDIR/latest` is the last), with `results.txt`, `frames/` (grim's,
  and `diff-*.png` showing in red what changed between two and in blue what was left out: what
  changed on its own between two frames a second apart, a clock or an animated wallpaper, and the
  terminal it runs in), `status/` (hyprctl's answers), `lipsync.jsonl`, `audio/` and `hypr3d.log`
  (`hyprctl hypr3d log`, the plugin's own lines, saved before it's unloaded). Whatever happens, Ctrl+C
  included (Esc leaves 3D first, so the terminal gets it), it leaves 3D, turns lip sync off and
  unloads the plugin. It won't start while hypr3d is loaded already. `tools/test/live/util_check.py`
  checks its desktop comparison on made-up frames.
- `tools/test/live/record_voice.sh [DIR]`: records you holding a, i, u, e and o, then "sss", then
  silence, 4 seconds each, from your default microphone into `DIR` (`~/h3d-live/voice`), and prints
  each one's level: for tuning lip sync to your voice offscreen (`lipsync_check.sh --real DIR`). You
  run it; the files stay where they are.
- `tools/test/synth/make.py PROJ`: writes a synthetic Unity project for the converter's tests (run it
  under Blender, see below). It holds an unpacked avatar prefab, a variant of an FBX with overrides,
  PSD and TGA textures, and outfits with and without Modular Avatar, including one with VRM bone
  names in an A pose.
- `tools/test/synth/booth.py OUTDIR`: a stand-in for an avatar bought on Booth, with outfits sold
  for it, as `.unitypackage` files. It makes `SynthChan_v1.0.unitypackage`, laid out like a Booth
  avatar: a humanoid FBX with Japanese shape keys and visemes, lilToon materials (the shader itself
  isn't included, as on Booth), PNG and PSD textures, an FX controller, menus with Japanese labels,
  PC and Quest prefabs, and PhysBones, colliders (a floor plane for the twin tails, a sphere the
  skirt keeps inside) and a head-pat contact. It also makes several outfits: a Modular Avatar dress,
  a plain parka for `--outfit`, a VRCFury cardigan, a hair pin from an old VRCFury, Modular Avatar
  accessories (thigh socks with a Mesh Cutter and Scale Adjusters, a Merge Blend Tree slider, a Menu
  Install Target, a Floor Adjuster, a Global Collider, a tail that sliders scale and turn), and
  VRCFury gimmicks (a World Drop heart, gesture combos, a Gesture controller's hand poses, a
  two-axis Puppet). Last, it puts the avatar package inside a Booth-style `.zip` with Shift-JIS
  names. `unitygen.py` holds what `make.py` and `booth.py` share: Unity's YAML, `.meta` files, prefab
  variants, controllers, VRChat, MA and VRCFury components, and the packing.
- `tools/test/synth/check.py OUT.glb`: what a conversion wrote (the node tree, world positions,
  colliders, meshes, materials and images).
- `tools/test/synth/skincmp.py A.glb [--pose NODE AXIS DEG]… B.glb`: compares where two GLBs put
  every mesh's skinned vertices.
- `tools/test/synth/ma_unit.py` and `vrcf_unit.py`: unit tests of the Modular Avatar and VRCFury
  code on small hand-made hierarchies and features (VRCFury's two save formats and its upgrades).
- `tools/test/synth/mat_unit.py`: the material reader on hand-made UnlitWF, lilToon, Poiyomi and
  MToon materials (alpha sources and masks, inverted alpha, which faces its shaders draw, emission,
  stencils, render queues, outlines, back faces, the light clamp, toon shading and matcaps), and a
  GLB exported and read back (the alpha baked into the base texture, the extras and their textures).
- `tools/test/synth/anim_unit.py`: transform curves, VRCFury's Scale, World Drop and Breathing, 2D
  blend trees, four-axis puppets, avatar masks and hand poses.
- `tools/test/synth/emote_unit.py`: `--emote` on a hand-made package of bare clips, read as a
  package, inside a zip, as a folder, as loose `.anim` files and by name. Still poses are left out
  of a package but kept when named, and a clip with no muscle or body curves isn't an emote. A
  clip's song: the sound file of its name beside it, or its folder's one when it's the folder's one
  clip (none when two clips share a folder and its sound file); copied for the settings file only
  when it's Ogg Vorbis (a WAV is left out, with a warning).
- `tools/test/synth/limit_unit.py [BOOTHDIR]`: PhysBone limits and Immobile on `booth.py`'s SynthChan
  (plain python3): the ears' Hinge, the tail's Polar and the skirt's and twin tails' Angle limits come out
  as hinge, spherical and cone limits; the back hair's hemisphere turned by Unity's Euler angles (90, 15,
  0) comes out mirrored for the GLB and lies behind it, off her back; an All Motion Immobile becomes
  `parentImmobile`, a World one doesn't.
- `tools/test/synth/human_unit.py [-- T_POSE.anim…]`: the muscle-to-bone maths on a small T-posed
  skeleton. Unity's T-pose muscle values must give the T pose back, left and right must mirror, and
  the signs, twists, body motion, curves (weighted keys too) and Foot IK must behave. Given Unity's
  own T pose clips (VRChat's SDK has `proxy_tpose.anim`), it poses them too.
- `tools/test/synth/goal_check.py -- AVATAR CLIP.anim…`: how far the converter puts an avatar's feet
  and hands from where a humanoid clip's own IK goal curves say they were. Unity writes those goals
  from the motion when it imports the clip, so they are Unity's own record of it.
- `tools/test/synth/fbxread.py FILE.fbx`: prints a binary FBX's model tree (plain python3).
- `tools/test/synth/decal_unit.py`: `tools/cs2map.py`'s decal fix on small hand-made scenes. Decals
  lifted 39 cm (on a floor, on a wall, over a curb, and under a node 16 times bigger, as the 3D skybox
  is) come back to 1 cm off their surface. A material that isn't a decal, decals already 1 cm off,
  decals with nothing behind them and a lone lifted decal among right ones stay where they are.
- `tools/test/synth/cs2mat_unit.py`: `tools/cs2map.py`'s CS2 material details, as the game's own
  shaders (decompiled) have them: the unlit shader's blend modes, its second texture and that
  texture's uv transform, fog left off, `F_NOTINT`, the tint mask and decal texture with the uv set
  each is read with, the lightmap's uv set coming after the material's own, the first layer's tint and
  transform on one-layer walls, a base colour's own uv transform and scroll with DynamicParams on top,
  and draw calls' linear tints (a model's, and a merged prop's fragment with its own tint) put back
  where Source 2 Viewer's glTF export had them linearized twice.
- `tools/test/regress.sh [--base REV|FILE] [--robot PATH] [--items DIR] [--shots]`: converts the
  synthetic avatars, the Booth-style packages (alone, with each outfit and from the zip), VRChat's
  robot sample (if you give its path) and free Booth items (if you give the folder you downloaded
  them to: 止丸式初音ミクNT, and as emotes on it and on SynthChan VRSuya's Doodle Dance, Loli Kami
  Requiem, INTERNET YAMERO and Reino Dance with `--outfit` and pHM's Toothless Dance with
  `--emote`) with the working copy's converter and with HEAD's. Then it compares the results, emote
  files included. Avatars with neither MA nor VRCFury must come out byte-identical, the zip must
  give the same GLB as the package, and bare clips must leave the GLB as it was. `--shots` also
  renders every result, front, side and walking, with the harness. The robot and the Booth items
  belong to their makers, so they are not in this repo.

Scripts that use numpy run under Blender's Python:

```sh
blender -b --factory-startup --python-exit-code 1 -P tools/test/synth/make.py -- /tmp/synth
blender -b --factory-startup --python-exit-code 1 -P tools/test/synth/booth.py -- /tmp/booth
blender -b --factory-startup --python-exit-code 1 -P tools/test/synth/ma_unit.py
blender -b --factory-startup --python-exit-code 1 -P tools/test/synth/vrcf_unit.py
blender -b --factory-startup --python-exit-code 1 -P tools/test/synth/mat_unit.py
blender -b --factory-startup --python-exit-code 1 -P tools/test/synth/human_unit.py
blender -b --factory-startup --python-exit-code 1 -P tools/test/synth/anim_unit.py
blender -b --factory-startup --python-exit-code 1 -P tools/test/synth/emote_unit.py
blender -b --factory-startup --python-exit-code 1 -P tools/test/synth/goal_check.py -- Avatar.zip Dance.anim
```

`vowels.py`, `fbxread.py`, `decal_unit.py`, `limit_unit.py` and the shell scripts run with plain `python3`
or `bash`.

## Known limits

- The attack's punches were made on Hatsune Miku NT. Another humanoid gets the same turns of its bones
  (in first person, its hands where hers were in the view): on a very different build the fist lands a
  little elsewhere, and long hair hanging in front of the shoulders can be brushed by the arms.
- VRChat's own emote animations are proprietary, so the built-in emotes are procedural look-alikes.
  Load real ones as `.vrma` or glTF clips with `avatar_emotes`.
- An emote's sound is Ogg Vorbis only (mono or stereo; not Opus, WAV or MP3), comes only through the
  settings file's `"sound"` (not with `avatar_emotes` or a file given to `hyprctl hypr3d avatar emote`),
  plays at its own speed whatever the emote's `speed` (as Unity's audio does), and isn't placed in the
  world: it's as loud in first person as in third, wherever the camera is. An Action layer's audio
  (an AudioSource a dance's prefab turns on) isn't carried over by the converter.
- A PhysBone's curves (a value that changes along its chain) aren't carried over: the chain gets the
  value set for all of it. Nor is Gravity Falloff, so gravity also pulls on bones hanging as modelled. A
  limit keeps a bone where its avatar's maker meant, as in VRChat: a fast spin can swing a necktie out
  sideways over a shoulder, and Hatsune Miku NT's skirt chain, which may only swing forward and back,
  hangs into her thigh when she sits.
- First person's hands are posed by the plugin, the same way for every avatar (to the length of its arms):
  an avatar's own first person animations aren't used, nor are its hands kept out of things at the sides
  (only of a wall or the window straight ahead). A big cuff (Hatsune Miku NT's) covers a good part of the
  bottom of the view.
- Walked procedurally, a foot lies along a slope up to about 26° and stands level on a stair; the toes
  don't bend to a stair's edge. Running down stairs it bounds down several at a time, the pelvis low. In
  first person the body turns its hips toward where you step sideways (A, D), and a
  sidestep straight across shuffles the trailing foot a little. Turning right round (a walk turns in
  about 0.6 s, people take longer) it trails your body by up to half a metre and catches up over the
  next second or two, up to a tenth quicker than you; running right round it can plant its feet wide
  for a moment, and running zigzags can slide a planted foot a few cm.
- In 3D mode, Alt+Tab doesn't reach Hyprland: Tab opens the Action Menu even with Alt held. Only keys
  with Super or Ctrl+Alt are passed through. That holds in play mode too, so a game never gets
  Super+anything: that's what keeps Super+Esc (and your own Super shortcuts) working while you play.
- Direct scanout is off in 3D: a fullscreen game's frames are drawn into the 3D view, the scene around
  them, instead of going to the screen as they are. See Performance. It's off on your other monitors
  too while 3D is up (Hyprland has one switch for all), so a fullscreen game there is composited.
- A window placed in the world from another monitor's workspace keeps that monitor's `wl_output`, scale
  and presentation timing; its FIFO barriers are released as that monitor presents.
- A window is known as launched from 3D by its process (the one started, or a child of it), by
  `HYPR3D_LAUNCH` in its environment, or, a Steam game's, by `SteamAppId` (or class `steam_app_ID`).
  An app that hands the launch to an instance already running (a second Discord, a second Firefox
  without `--new-instance`) is known only by its class, for a minute: its desktop entry's
  `StartupWMClass` (or id). Otherwise it's any other window that opens in 3D, which comes in front of
  you all the same (as big as on your screen); the Windows page's "Bring here" brings any
  window.
- Drag and drop works in 3D, but the dragged thing's icon isn't drawn there (Hyprland keeps it
  private); the "grabbing" cursor shows instead.
- Changing a window's real size (Shift+wheel, Bigger, Smaller) makes a tiled window floating on the 2D
  desktop.
- Where windows were put is remembered by class, so an app with several windows of one class is put
  back only when it has one.
- Tiling mode's ring is one row: once the windows would go further round you than about 340°, they
  all get smaller alike rather than going into a second row. Windows half your screen wide fill it at
  about five in first person, and at three or four in third person, where they're sized for the camera
  behind the avatar but stand round the avatar, so each takes a wider angle round it.
- Lip sync knows five vowels, how loud you are, and four consonants (pp, ff, ss, ch) where the
  avatar has their visemes; not th, dd, kk, nn or rr, and an n's murmur shows as pp, as an m's does.
  Its vowels are Japanese ones, between a man's and a woman's voice (Tokyo speakers' measurements and
  28 recordings of 11 speakers); other voices and languages may pick the wrong shape now and then.
  It was tested on sung vowels and consonants, through the harness and through PipeWire in a VM
  (`tools/test/vm`), and on those real recordings (26 of 28 lead with their vowel; the consonants
  in their words were looked at, not scored); `tools/test/live/check.sh --mic` tries it on your
  voice.
- The automatic gain goes by the voice it hears once it has heard a pause (the room), and only up to
  50 dB. Another voice near you (a TV, a call on speakers) counts as yours when you're quiet, and a
  voice less than about 10 dB over the room's noise opens the mouth only now and then; a fixed
  `lipsync_gain` is steadier there. A microphone muted by its own button sends exact zeros, which the
  badge tells from silence; one that sends its own quiet noise instead can't be told from a quiet
  room.
- The plugin's Hyprland code (its hooks, dispatchers, hyprctl command, Lua functions and config
  values) is tested in a real Hyprland in a VM (`tools/test/vm`), on a virtual GPU that Mesa draws for
  in software, at scales 1, 1.5 and 2 and with a second (headless) monitor; `tools/test/live/check.sh`
  goes through it on your own monitor and GPU.
- aquamarine before 0.12.1 (Hyprland 0.55.x has 0.11.0) runs a late frame of a headless output
  (`hyprctl output create headless`) after the output is removed: Hyprland crashes, or its heap is
  corrupted and it aborts later (fixed upstream by 1699271 and 6ecde03). In 3D the plugin asks for
  every frame, so a slow one is nearly always waiting when the monitor it's on goes. While the plugin
  is loaded, it holds a removed headless output until the frames queued for it have run.
- Hyprland 0.55.x crashes when it quits with windows open, with or without hypr3d: its dwindle and
  master layouts call a window that's gone. Hyprland 0.56.0 fixed dwindle's (commit 338bdbb3); master's
  still crashes on Hyprland's main branch (e368c13c, September 2026). Guarding master's calls the way
  338bdbb3 guards dwindle's fixes it (tested in the VM on 0.55.2 and on main). That patch, applied nowhere,
  is in `extras/hyprland-exit-crash`, with how to build Hyprland with it on NixOS.
- Toon shading takes a toon shader's first shade step only, not its second and third, nor its shade
  and matcap masks. A shadow on a toon surface is looked up 10 cm towards the sun, so shadows cast from
  closer than that (a fringe's on the forehead) don't show on it.
- `tools/unity2hypr3d.py` covers the MA and VRCFury features avatars and outfits use most, not all
  of them (see its "not converted" list above). It has been tested on synthetic packages and on
  free Booth items (an UnlitWF avatar, four MA dance motions and one sold as bare clips), not on paid
  avatars or outfits.
- The dance emotes are Unity's humanoid worked out without Unity: within a few degrees of Unity's own
  T pose, and their feet and hands within a few centimetres of where the clips' own IK goals (Unity's
  record of the motion) say (`goal_check.py`), but not compared frame by frame with Unity. The
  exception is Toothless Dance's hands, 13 cm from its goals on 止丸式初音ミクNT. They are nearer the
  body there, the elbows more bent than the goals have them, with Unity's default arm limits. That
  may be the avatar the clip was made on, whose limits the goals keep and the muscle values don't.

## Credits

- [cgltf](https://github.com/jkuhlmann/cgltf) (MIT) and [stb_image and stb_dxt](https://github.com/nothings/stb)
  (public domain or MIT), vendored in `src/third_party/`.
- The bone-name table in `tools/unity2hypr3d.py` is from
  [Modular Avatar](https://github.com/bdunderscore/modular-avatar) (MIT, © 2022 bd_). MA took it from
  HhotateA's AvatarModifyTools (MIT, © 2021 @HhotateA_xR) and Azukimochi's BoneRenamer (MIT,
  © 2023 Azukimochi). The converter's other Modular Avatar support reimplements MA's behaviour in
  Python, written from reading MA's source. The license texts are in [THIRD_PARTY.md](THIRD_PARTY.md).
- The converter's humanoid muscle maths follow lox9973's
  [ShaderMotion](https://gitlab.com/lox9973/ShaderMotion) (MIT, © 2020-2021 lox9973) and
  [uvw.js](https://gitlab.com/lox9973/uvw.js) (Apache 2.0, © 2022-2023 lox9973): the muscle table,
  signs, masses and twist sharing. See [THIRD_PARTY.md](THIRD_PARTY.md).
- The converter knows UnlitWF's stencil and outline shaders by their GUIDs (from the `.meta` files of
  whiteflare's [Unlit_WF_ShaderSuite](https://github.com/whiteflare/Unlit_WF_ShaderSuite), zlib), and
  lilToon's outline shaders by theirs (from [lilToon](https://github.com/lilxyzw/lilToon)'s, MIT,
  © 2020-2024 lilxyzw). What their materials' settings do was read in those shaders' sources, and in
  [Poiyomi Toon](https://github.com/poiyomi/PoiyomiToonShader)'s (MIT); none of their code is in this
  repo. See [THIRD_PARTY.md](THIRD_PARTY.md).
- The converter's VRCFury support reimplements VRCFury's build behaviour
  ([VRCFury](https://github.com/VRCFury/VRCFury), © 2022 Senky, under its own license). It was written
  after reading VRCFury's source for its save format and behaviour, and contains none of its code.
- The vowel recordings in `extras/speech` are from Wikimedia Commons, Lingua Libre and Tofugu/WaniKani's
  pronunciation audio (public domain, CC0, CC BY and CC BY-SA 4.0), and the formant data in
  `extras/speech/ref` is Kakeru Yazawa's (Zenodo 15227304, CC BY 4.0); `extras/speech/LICENSES.md`
  credits each. The patches in `extras/` change Hyprland's and aquamarine's code (BSD 3-Clause).
- VRChat, Modular Avatar, Counter-Strike 2 and Blender belong to their owners. This project has no
  connection with any of them, and it ships none of their assets.
