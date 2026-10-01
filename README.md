# hypr3d

A [Hyprland](https://hyprland.org) plugin that turns the desktop into a place you can walk around in.
While 3D mode is on, your windows hang on a wall in a small courtyard, or in any glTF map, and you
walk up to them in first person. The crosshair clicks, scrolls and types into whatever it points at.
You can take a window off the wall and put it anywhere, pin one to your view, and launch apps
straight into the world. Games are played there too: play mode gives a window the keyboard, the
mouse (a locked pointer's relative motion, as games want it) and the buttons, and turns you to face
it.

With an avatar loaded you can switch to third person. The avatar works much like one in VRChat: it
has faces, hand gestures, emotes and dances, a radial Action Menu, outfit toggles and sliders, hair
and clothes that swing, toon outlines, and lip sync from your microphone if you turn it on. It can be
a VRM, a plain GLB, or a VRChat avatar converted with `tools/unity2hypr3d.py`.

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
| `plugin:hypr3d:avatar_emotes` | `""` | more emotes: VRM animations (`.vrma`) or glTF clips; files or folders, separated by commas |
| `plugin:hypr3d:lipsync` | `false` | lip sync: the microphone moves the avatar's mouth while you are in 3D (below) |
| `plugin:hypr3d:lipsync_gain` | `auto` | lip sync: how much louder the microphone counts, in dB (−20 to 60); `auto` goes by your voice (below) |
| `plugin:hypr3d:lipsync_source` | `""` | lip sync: the microphone, by its name or its description as `wpctl status` lists it; `""` is the default one |
| `plugin:hypr3d:apps` | `""` | the Action Menu's Apps page: desktop ids, app names or commands, separated by commas (below) |
| `plugin:hypr3d:app_rules` | `""` | where apps launched from 3D open: `CLASS: DISTANCE HEIGHT [left\|right\|SIDE]`, separated by commas (below) |
| `plugin:hypr3d:pin_size` | `0.3` | how much of the view's height a window pinned to it takes, 0.05–1 |
| `plugin:hypr3d:monitor` | `""` | the monitor 3D goes on: its name as `hyprctl monitors` lists it (`DP-1`), or `desc:` and the start of its description; `""` is the focused one. The others stay your desktop (below) |

Paths may start with `~/`. Maps and avatars load in the background, and a failure shows up as a
notification. A config reload applies changed values at once; a value changed at run time without one
(`hyprctl keyword plugin:hypr3d:…`, or `hl.config()` through `hyprctl eval`) takes effect within a
second.

## Controls

Enter and leave 3D with the `hypr3d:toggle` dispatcher, `hyprctl hypr3d toggle` or
`hl.plugin.hypr3d.toggle()`. 3D goes on the focused monitor, or on the one `plugin:hypr3d:monitor`
names (see Two monitors, below). Keys held with Super, or with Ctrl+Alt, still go to Hyprland, so
your compositor shortcuts keep working in 3D.

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
| P | play the window under the crosshair: it gets every key, the buttons, the wheel and the mouse, and you face it (below); Super+Esc stops |
| G | pick up the window under the crosshair; G or a left click puts it down where it is, a right click or Esc puts it back where it was; the wheel moves it nearer or further, Ctrl+wheel scales it, Shift+wheel changes its real size (the app draws itself anew) |
| X | send a window you've placed back to the wall (and forget the place its app had) |
| H | pin the window under the crosshair to your view (top right, over the world), or the one you carry; H again puts it down where it is |
| Q | the Action Menu's Apps page: launch an app into the world |
| B | the Action Menu's Windows page: every window, and what to do with it |
| V | first / third person (needs an avatar) |
| Tab | the Action Menu |
| F1–F8 | hand gestures (Neutral, Fist, Open, Point, Victory, Rock'n'roll, Handgun, Thumbs up), as in VRChat's desktop mode: with Left Shift held only the left hand, with Right Shift only the right, otherwise both |
| Esc | leave 3D |
| Super+Esc | with another monitor: the mouse and keyboard to it, the 3D view staying up; Super+Esc there comes back (below) |

The Action Menu has pages for emotes, expressions, gestures, the outfit, apps, windows and options
(view, physics, fly, lip sync, mic gain, respawn, reset face, stop emote). With it open, the mouse moves its cursor, a left click
picks, a right click goes back and a middle click closes it. The wheel goes round it, 1–8 pick an
item, Enter picks, Backspace goes back and Esc closes it. WASD still walks.

A slider on the outfit page (🎚️, VRChat's radial puppet) opens a dial. The cursor sets it by going
round from the top, clockwise from 0% to 100%. It stops at the ends rather than jump across the top.
The wheel moves it in 5% steps, and 1–8 set 0%, 14%, … 100%. A click, Backspace or a right click
closes the dial.

A two-axis puppet (🕹️, VRChat's and VRCFury's two- and four-axis puppets) opens a stick instead:
where the cursor is in the disc sets x and y, −1 to 1 each, right and up positive, and 1–8 push it
all the way in the eight directions, from up clockwise.

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

Point at a window and press P (or `hyprctl hypr3d play on`, the `hypr3d:play` dispatcher,
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

The camera leaves you and turns to face the window, filling most of the view (94% of the way it
fits) and following it if it moves; the crosshair, the aimed window's outline and the Action Menu go
(the lip sync badge stays while the microphone listens), and the window is drawn over the world, so
nothing gets in front of it. Windows pinned to the view stay over it. You don't walk while playing;
your avatar stays where you were.

Play mode ends with **Super+Esc** (the app doesn't get that Esc), when you leave 3D, when the screen
locks, when the window closes or leaves the 3D view (its workspace hidden), when another window takes
the keyboard, and when a layer surface does (a launcher, a lock screen). A notification says how to
stop when it starts. The window's dialogs are played with it: a file chooser (the portal's, or the
app's own) opens over it, as on the 2D desktop, and has the keyboard; the pointer goes over it, and
when it closes you're back in the window, still playing. An X11 menu of the app's own can take the
keyboard too. Pointing at a dialog and pressing P plays the window it belongs to. A window that goes
fullscreen while it has the keyboard (a browser's video, a game) is played at once, and leaving
fullscreen ends that; one that maximizes only gets bigger.

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

### The app's cursor

In 3D the pointer is where the crosshair is on a window, and that app's own cursor is drawn there, on
the window: the shape it asked for (`wp_cursor_shape_v1`, from Hyprland's cursor theme) or its own
cursor surface (an X11 app's comes from XWayland that way), animated ones too. The crosshair shrinks
to a dot at the cursor's hotspot. An app that hides its cursor (a game) shows none, and the whole
crosshair comes back while walking; in play mode the app's cursor is the only one. Hyprland's own
cursor stays off the monitors in 3D (its hardware cursor off, its software cursor not drawn), and
`cursor:invisible` hides the app's too.

### X11 apps

X11 apps run through XWayland, which Hyprland starts when it finds the `Xwayland` binary. Their
windows are panels like the others, and get clicks, typing, the wheel and relative motion as on the 2D
desktop, at any scale. Their menus and tooltips, which X11 makes windows of their own at absolute
positions (override-redirect), are drawn as popups of the window they belong to, so they go along when
it's out in the world: found through `WM_TRANSIENT_FOR`, else the app's window that has the keyboard,
else the one they're over. (Hyprland 0.55.2's own lookup of an X11 window's parent returns a Wayland
window; the plugin walks `WM_TRANSIENT_FOR` itself.)

### Launching apps into the world

Q, or the Action Menu's Apps page, shows the favourites from `plugin:hypr3d:apps` (desktop ids such as
`org.mozilla.firefox`, app names such as `Discord`, or commands), then All apps: the XDG desktop
entries (`applications/*.desktop` in `$XDG_DATA_HOME` and `$XDG_DATA_DIRS`, which on NixOS the plugin
also looks for without the variable), minus hidden ones, with their icons (PNG or SVG from the icon
themes, read with hyprgraphics). `hyprctl hypr3d launch WHAT` does the same from a script or a keybind.

An app launched from 3D starts the way Hyprland's `exec` starts things, with `HYPR3D_LAUNCH` set in its
environment. Its window (one whose process is the one started or a child of it, or has that variable)
opens in front of you, not on the wall: where its class was put the last time in this world, else as
`plugin:hypr3d:app_rules` says, else as the built-in rules say:

| Apps | Distance | Height | Side |
|---|---|---|---|
| games: `steam_app_*`, `gamescope`, `*.exe`, `steam_proton`, `retroarch`, Minecraft's | 2 m | 1.3 m | |
| chat and calls: Discord, Vesktop, WebCord, Equibop, Signal, Telegram, Element, Slack, Zoom, Teams | 1.3 m | 0.75 m | 1 m to the left |
| video players: mpv, VLC, Celluloid, Showtime | 2 m | 1.2 m | |
| anything else | 1.5 m | 0.9 m | |

```lua
hl.config({ plugin = { hypr3d = {
    apps = "firefox, Discord, obs, steam",
    app_rules = "steam_app_.*: 2.4 1.6, discord: 1.2 0.7 left, org.telegram.desktop: 1.2 0.6 right",
} } })
```

A rule is `CLASS: DISTANCE HEIGHT [left|right|SIDE]`: the class a regular expression (the whole of
it, any case), the distance from your eye and the window's height in metres, and to the side in metres
(`left` and `right` are 1 m). A window whose app was already running (a second Discord, a Firefox
that hands the address to the one open) isn't the launched process's: for a minute it's known by the
class its desktop entry names (`StartupWMClass`, else its id, else the program's name). A Steam game,
`steam steam://rungameid/ID`, is started by Steam, which may open windows of its own first: its
window is the one with `SteamAppId=ID` in its environment, as Steam starts every game (native or
Proton), or of class `steam_app_ID`, as Proton's are; Steam's windows stay where they'd be.

Where you put a window (G, then put it down) is kept for its class, for each map, in
`$XDG_STATE_HOME/hypr3d/windows/` (`courtyard.conf` for the courtyard): its windows launched from 3D
open there again, and so does one that opens in 3D, or that's there when you enter 3D, when it's the
only window of its class. X forgets its place; `hyprctl hypr3d reset-windows forget` forgets them all.

A dialog of a window out in the world (a file chooser from the portal, OBS's properties) opens in front
of it. Any other window that opens while you're in 3D, on the monitor you're in 3D on (a terminal from
a keybind, the screen-share portal's picker, a splash screen), opens in front of you too, not out of
sight on the wall: a floating one as big as it would be on the wall, a tiled one as the app rules
say. X sends it to the wall.

### The Windows page, pinning, real sizes

B, or the Action Menu's Windows page, lists every window and where it is (on the wall and on which
workspace, out in the world, or pinned). Picking one gives: Focus (its workspace shown, and the
keyboard), Bring here (out in front of you), To the wall, Pin to view or Unpin, Bigger and Smaller (its
real size, a quarter more or a fifth less), Play, and Close. `hyprctl hypr3d window SEL ACTION` does
the same for a window by address, class or title.

**Pinning** (H on the window under the crosshair or the one you carry, or the Windows page) keeps a
window in the top right corner of your view, `pin_size` of its height, drawn over the world: a video, or
a call, while you walk or play. H again puts it down where it is in the world, whatever the crosshair
points at (it can't point at a pinned window); standing close to a wall, in front of the wall rather than
in it, nearer and smaller so it looks the same. With more than one pinned (the Windows page stacks them
down the side), H puts down the last one pinned; Unpin on the Windows page puts down any of them. To
click in a pinned window, put it down, or play it from the Windows page (Play takes it out of the
corner).

**The real size**: Shift+wheel while you carry a window, Bigger and Smaller, or
`hyprctl hypr3d window SEL size W H` change the window's size in pixels, and the app draws itself
anew at it, text as big as before (Ctrl+wheel scales it instead). A tiled window becomes floating on the
2D desktop for it, as that's the only way a window can be any size.

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
| ... the game played | 21 | 41 ms | 19–21 |
| ... the game fullscreen and played | 23–29 | 29–38 ms | 18–30 |
| Intel iGPU (`--gpu virgl`): 4 windows and the game on the wall | 79–83 | 0.2 ms | 76–83 |
| ... in third person | 79–82 | 0.3 ms | 79–82 |
| ... the game played | 96–102 | 0.2 ms | 95–103 |
| ... the game fullscreen and played | 94–99 | 0.5 ms | 90–98 |
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
| `type [on\|off]` | type into the window under the crosshair, or go back to walking |
| `play [on\|off\|toggle]` | play the window under the crosshair (P), or stop; without an argument, what's played: its class, the pointer, whether it's locked or confined |
| `launch WHAT` | start an app into the world: a desktop id, an app's name or a command |
| `apps` | the desktop entries: id, name, what it runs, its class, whether its icon was found |
| `window SEL focus\|bring\|wall\|pin\|unpin\|bigger\|smaller\|size W H\|play\|close` | do that to a window, by its address (`0x…`, as `hyprctl clients` has it), class or title |
| `panels` | everything drawn in 3D, in drawing order: its kind (window, popup, layer), window, box on the desktop, placed, drawn over the world, and its surfaces' boxes (subsurfaces too) |
| `look dx dy`, `turn yaw pitch`, `tp x y z` | turn by mouse counts, turn to angles in degrees, teleport |
| `walk secs [forward\|back\|left\|right]`, `jump`, `fly` | move from a script |
| `click [left\|right\|middle]` | click where the crosshair is |
| `sens [value]` | mouse sensitivity |
| `grab`, `place`, `hold dist [scale]`, `pin`, `reset-windows [forget]` | carry windows (`pin` is H), and put them all back (`forget`: nor where their classes were put) |
| `aim [window]` | turn to face a window's middle: that one (address, class or title), else the one nearest to where you look |
| `log [lines]` | what the plugin logged lately, its notifications too (the last 400 lines; Hyprland's own log has them only with `debug:disable_logs = false`) |
| `windows` | the windows off the wall: their address, where, how far from your eye, how big (1 = as on the wall) and how tall, held or pinned, how far and big the one you carry is held, and how many places are remembered |
| `map [path\|none\|reload\|forget\|scale s]` | load a map; `forget` drops the start and desktop place saved for it |
| `spawn [here]` | go back to the start, or make where you stand the start |
| `desktop [here [height]]` | where the desktop hangs, or hang it where the crosshair points |
| `view [first\|third\|toggle] [distance] [side]` | the camera |
| `avatar [path\|none\|reload\|height m]` | load an avatar, or print its state |
| `avatar expression [name [weight]\|none]` | set a face |
| `avatar gesture [left\|right\|both gesture]` | set a hand gesture |
| `avatar parts [reset]`, `avatar toggle name [on\|off\|reset]`, `avatar slider name [0..1\|NN%\|reset]`, `avatar slider name x y`, `avatar shape key [weight\|reset]` | the outfit; `parts` lists the toggles, sliders and material variants. A two-axis slider takes x and y, −1..1 or NN% each |
| `avatar physics [on\|off\|toggle]` | spring bones |
| `avatar lipsync [on\|off\|toggle\|gain dB\|auto\|source name\|default]` | lip sync, its gain and its microphone; without an argument, what it hears and what the microphone gives: the level, formants and visemes, the badge's text and `"problem"` (none, starting, unlinked, muted, silent, nothing, missing, error), the source (name, description, state, muted, volume), `"linked"`, `"samples"`, `"buffers"`, `"silentFor"` (seconds of exact zeros), `"peak"` and `"rms"` (the last second, dBFS), `"gain"`, `"reference"` (your voice), `"room"`, `"marks"` (shut below, wide open from) and `"sources"` |
| `avatar emote [name\|number\|file\|folder [once\|loop]\|stop]` | play an emote, or load emotes from files; without a name, the list, with each one's speed |
| `menu [open [page]\|close\|toggle\|back\|pick [n]\|move dx dy\|scroll n]` | drive the Action Menu; without an argument, what it shows (a dial's value, a stick's x and y) |

The `hypr3d:menu` dispatcher toggles the Action Menu. `hypr3d:menu emotes` opens a page (`apps` and
`windows` too), and any other argument does what `hyprctl hypr3d menu` does. The `hypr3d:play`
dispatcher plays the window under the crosshair, or stops, and `hypr3d:away` sends the mouse and
keyboard to another monitor, or brings them back. The Lua functions are
`hl.plugin.hypr3d.toggle()`, `enter()`, `exit()`, `type()`, `play()`, `away()` and
`menu([page or command])`.

The status's `"cursor"` is the app's cursor as it's drawn (where on the window, its size and
hotspot, or null), `"playing"` what's played, and `"updateMs"` and `"renderMs"` the plugin's own
time a frame on the CPU (its update, and its drawing's GL calls), averaged.

## Avatars

`plugin:hypr3d:avatar` (or `hyprctl hypr3d avatar FILE`) takes one of these:

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
and flies. Gestures curl its fingers and can set a face. The built-in emotes are Wave, Clap, Point,
Cheer, Dance, Backflip, Sad Kick and Die. `avatar_emotes` adds your own `.vrma` or glTF clips, and
so does the settings file's `emotes`, where the converter lists the dances and poses it finds. Toggles
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
| `emotes` | `[{"file" or "clip", "name", "loop", "hold", "grounded", "speed"}]`: more emotes. `file` is a `.vrma` or glTF file (next to the settings file unless absolute), `clip` a clip of the avatar's own. `hold` keeps the last frame until you move, `grounded` keeps the feet on the floor, and `speed` (default 1) plays it faster or slower |
| `springs` | `[{"name", "bones": [roots], "ignore": [bones], "stiffness", "drag", "gravity", "gravityDir": [x,y,z], "radius", "center", "immobile", "parentImmobile", "colliders": [names, or "body"], "limit"}]`: each root and everything under it swings. `limit` is a `VRMC_springBone_limit` limit for each of its bones: `{"cone": {"angle", "rotation": [x,y,z,w]}}`, `{"hinge": {"angle", "rotation"}}` or `{"spherical": {"pitch", "yaw", "rotation"}}`, in radians, turned by `rotation` from a frame whose y runs along the bone: how far a bone may turn from where the animation points it (a PhysBone's Angle, Hinge or Polar limit). A bone with a limit leaves out the colliders made for the body (`"body"`) that it starts inside of. `parentImmobile` (0..1) is how much of what the bone the spring hangs from does beyond where the avatar goes (a walk's bob and sway, a turn, a dance) carries the spring along instead of swinging it: a PhysBone's Immobile (All Motion) |
| `colliders` | `[{"name", "node", "offset": [x,y,z], "tail": [x,y,z], "radius", "inside"}]`: spheres, or capsules with a tail, in the node's units; `"inside": true` keeps the bones inside it (PhysBones' inside bounds). A plane is `{"name", "node", "offset", "normal": [x,y,z]}`: the bones keep to the side it faces |
| `immobile` | 0..1, default 0.9: how much of the air the avatar carries along as it moves. With 0, walking at 4.5 m/s blows long hair out level behind it |

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
clip and looping if the clip loops. A package's still poses (a "proxy" standing pose, say) are left
out when it also has clips that move. Name one with `--emote` to have it anyway. Both options can be
given more than once:

```sh
python3 tools/unity2hypr3d.py Avatar.zip --outfit VRSuya_Doodle_Dance_Released_260709.zip \
    --emote pHMToothlessDance.zip -o me.glb
```

hypr3d plays no sound, so play the song yourself. VRSuya's Booth pages name the songs: "Doodle" by
Zachz Winner for Doodle Dance, and しぐれうい's 「粛聖!! ロリ神レクイエム☆」 for Loli Kami Requiem. A
dance's `speed` in the settings file matches it to the song's tempo. The Doodle Dance page suggests
about 97.7%. At `"speed": 0.977`, the dance's 22-frame bounce lasts 0.375 s, one beat at 160 BPM.

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
  GPU has none), timing (`--bench`) and debugging (`--hide`, `--show`, `--glinfo`, `--where NODE`). For
  springs: each one's bones, colliders and limit (`--springs`), how far its bones are out of their limits
  (`--limits`, worked out on its own) and how much deeper than the animation has it physics puts each kind
  of spring into the body (`--springclip FILE`); `--view-chest DEG` puts the camera round where the chest
  faces (a dance turns it). `--ctl`
  runs a `hyprctl hypr3d avatar …` or `menu …` request, and `--key`, `--click`, `--wheel` and
  `--mouse` give the Action Menu the plugin's input: the same code as the plugin's (`src/control.cpp`,
  which `main.cpp` hands its commands and menu input to).
- `tools/test/harness/ctl_check.sh DIR`: those, checked on the MA accessories and dances regress.sh
  converted (its `--keep`'s `OUT/new`): sliders by number and percent, a two-axis one, a toggle and
  its material variant, the menu's pages, a dial turned by the mouse and the wheel, a stick, and an
  emote at twice its speed. It runs at the VM's 1280×800, and the harness lays its menu out for its
  `--size` before each option, as the plugin does every frame, so the dial and the stick come out
  where the VM test has them.
- `tools/test/harness/toon_check.sh [DIR]`: toon shading and matcaps on `toonballs.py`'s six balls
  (a plain one, MToon 1.0's, VRM 0.x MToon's, the converter's extras with and without a matcap, and a
  matcap alone), side on to the sun: the plain ball's light falls off with N·L, a toon ball's is flat
  on each side of a sharp step, each shade has its own colour, a matcap brightens where it's white,
  and a toon ball in a wall's shadow is all shade.
- `tools/test/harness/spring_check.sh AVATAR [DIR] [KIND...]`: the avatar's springs standing, walking and
  stopping, running and stopping, turning on the spot, jumping and dancing (and turning right round and
  running in first person, with a harness that has those), at the plugin's speeds and accelerations: no
  bone with a limit gets out of it, and the kinds of spring named (a spring's name up to its first `.`)
  never go more than 2 cm deeper into the body than the animation has them. It prints how deep every kind
  went in each. Hatsune Miku NT's necktie: `spring_check.sh ~/.local/share/hypr3d/avatars/Miku/Miku.glb ""
  Necktie` (converted before PhysBone limits were carried over, it went 6.7 cm into her running).
- `tools/test/harness/cs2mat_check.sh [DIR]`: CS2's material details as `tools/cs2map.py` writes them,
  on `cs2mats.py`'s panels, each split by its textures: the tint only where the tint mask is, a decal
  multiplied on the second uv set and one mixed in by its alpha, an unlit colour times its second
  texture, an unlit one added to the wall, past the fog a quad with its fog off that stays red and
  added light that fades out; and vertex paint only where the tint mask is (all 0 is none), and unlit
  mod2x in linear light (sRGB 188 leaves the wall as it is, 128 darkens it).
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
  - the config values, set in the config, at run time, and through `hyprctl keyword`
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
    that keeps drawing when its workspace is hidden
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
  - sliders, a material variant and an emote's speed through hyprctl; the Lua functions and the
    `hypr3d:toggle` and `hypr3d:menu` dispatchers, from hyprctl and from keybinds
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
  - play mode (`h3dgame.c`, a small SDL2 game that prints what it gets and draws it): its pointer
    lock, relative motion to the count, keys (Esc and Tab too), buttons and the wheel reaching it and
    not the player, the camera facing it and back, the pointer over it without a lock and kept on it,
    its own cursor, fullscreen, Super+Esc, leaving 3D, a screen lock (swaylock, unlocked by typing into
    it), and a game controller (`gamepad.py`, an Xbox 360 pad through uinput) read by SDL while it has
    the keyboard focus, and not once it hasn't
  - what a game needs from the compositor: the activated state (`wev`), `wl_output` enter and
    presentation feedback presented, not discarded (`weston-presentation-shm`'s own protocol log),
    its idle inhibitor on a hidden workspace (`swayidle`), and its frame rate following the 3D view's
  - X11 apps through XWayland: a Tk app (`tkapp.py`) clicked, typed into and scrolled, its menu bar's
    menu, right-click menu and tooltip (override-redirect windows) as popups, placed in the world too;
    SDL's x11 driver in relative mode getting in 3D what it gets on the 2D desktop; xterm's own cursor;
    at scales 1, 1.5 and 2
  - apps: the Apps page (desktop entries written for the test, their icons), launching into the world
    (the built-in rules for a game and a chat app, a config rule for a command), places remembered by
    class and restored, X forgetting one, the Windows page, pinning, real sizes (Bigger, Shift+wheel),
    and fullscreen from an app starting play mode
  - everyday apps, open-source stand-ins: Chromium and Firefox with `page.html` (a `<select>`'s list,
    a tooltip and the context menu drawn over the window, as popups or, Firefox's list and tooltip,
    subsurfaces; text selected by dragging copied to the clipboard and read back with `wl-paste`,
    typing, drag and drop in the page and, walking, out of it with the crosshair onto another window, a
    touchpad's scrolling and pinch, fullscreen, a file dialog opening by the browser, played along with
    it and closed with Esc); Electron (`electron/`, Discord's stack) as a
    Wayland and an X11 client, with its own context menu (a native one, clicked) and a notification
    (mako), and drag and drop through XWayland compared with the 2D desktop's; OBS (driven through
    obs-websocket by `obsws.py`): its menu, a dialog by it in the world, screen capture through
    xdg-desktop-portal-hyprland with its picker used in 3D, the capture showing the 3D view, and a
    placed window captured as a window
  - an input method: fcitx5's clipboard list (Ctrl+;) shown while typing into a terminal in 3D, its
    popup drawn over the window by the text, and gone with Esc
  - real open-source games: Chocolate Doom with Freedoom's levels (the pointer locked, turned by the
    mouse, walked by the keys; as a Wayland and an X11 client) and SuperTux (its menu by the keys and
    by the controller)
  - frame rates: the 3D view's with four windows and a game, in first and third person, playing,
    with a window pinned, the plugin's own time a frame, and the game's own frame rate behind it; and
    a fullscreen game with direct scanout allowed on the 2D desktop against the same game played in 3D
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
  `page.html`, `electron/`, `obsws.py`), the live check script and `hypr3d.so`; the apps it runs are
  open-source ones from nixpkgs (Chromium, Firefox, Electron, OBS Studio, Chocolate Doom with Freedoom's
  levels, SuperTux, fcitx5, xterm, Tk, weston's demo clients, swayidle, swaylock, mako and the portals).
  A run has about 450 checks and takes about half an hour (23 minutes with `--gpu virgl`), nearly all
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
  cursor) and pin it (H), then to try what matters to you in it (P again, Super+Esc when you're done: a
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
  of a package but kept when named, and a clip with no muscle or body curves isn't an emote.
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

- VRChat's own emote animations are proprietary, so the built-in emotes are procedural look-alikes.
  Load real ones as `.vrma` or glTF clips with `avatar_emotes`.
- A PhysBone's curves (a value that changes along its chain) aren't carried over: the chain gets the
  value set for all of it. Nor is Gravity Falloff, so gravity also pulls on bones hanging as modelled. A
  limit keeps a bone where its avatar's maker meant, as in VRChat: a fast spin can swing a necktie out
  sideways over a shoulder, and Hatsune Miku NT's skirt chain, which may only swing forward and back,
  hangs into her thigh when she sits.
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
  you all the same (a floating one at its size on the wall); the Windows page's "Bring here" brings any
  window.
- Drag and drop works in 3D, but the dragged thing's icon isn't drawn there (Hyprland keeps it
  private); the "grabbing" cursor shows instead.
- Changing a window's real size (Shift+wheel, Bigger, Smaller) makes a tiled window floating on the 2D
  desktop.
- Where windows were put is remembered by class, so an app with several windows of one class is put
  back only when it has one.
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
