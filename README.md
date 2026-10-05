# hyprwalk

A [Hyprland](https://hyprland.org) plugin that turns your desktop into a place you walk around in. Your windows hang
on a wall in a 3D courtyard (or any glTF map, CS2 maps included). Walk up to one, and the crosshair clicks, scrolls
and types into it. Carry windows around, tile them in a ring around you, launch apps and play games where they hang.

Load an avatar (VRM, glTF or a converted VRChat avatar) and you see the world through its eyes, with faces, hand
gestures, emotes, outfit toggles, swinging hair and lip sync from your microphone.

![Your windows on the courtyard wall](screenshots/desktop-wall.png)

![Third person, with the Action Menu's outfit page open](screenshots/avatar-action-menu.png)

![CS2's de_mirage, converted from a local install with tools/cs2map.py](screenshots/mirage-mid-cs2.png)

## Install

A plugin must be built for the exact Hyprland it runs in, so you build hyprwalk yourself. It needs Hyprland 0.55 or
0.56 and GCC 15 or newer.

### 1. Install the build dependencies

**Arch Linux**

```sh
sudo pacman -S --needed base-devel git hyprland pango libpipewire
```

**Fedora** (Hyprland from the [sdegler/hyprland](https://copr.fedorainfracloud.org/coprs/sdegler/hyprland/) COPR)

```sh
sudo dnf copr enable sdegler/hyprland
sudo dnf install gcc-c++ make pkgconf git hyprland-devel pango-devel pixman-devel pipewire-devel
```

**openSUSE Tumbleweed**

```sh
sudo zypper install gcc-c++ make pkgconf git hyprland-devel glslang-devel pango-devel libpixman-1-0-devel pipewire-devel
```

**Debian sid, Ubuntu 26.10** (older releases ship a Hyprland that is too old)

```sh
sudo apt install build-essential pkgconf git hyprland-dev libpango1.0-dev libpixman-1-dev libpipewire-0.3-dev
```

**NixOS**: nothing to install. `build.sh` builds against the running Hyprland's own Nix derivation.

**Other distributions**: your distribution's Hyprland development package, plus pkg-config, make, GCC 15+, and the
pango, pixman and PipeWire development packages. No Hyprland development package? Use [hyprpm](#with-hyprpm) instead.

PipeWire is optional. Without it there's no lip sync and emotes have no sound.

### 2. Build

```sh
git clone https://github.com/Marcus-Arcadius/hyprwalk
cd hyprwalk
./build.sh
```

This makes `hyprwalk.so`. The repository is private, so git needs your GitHub login (e.g. `gh auth setup-git`). On
NixOS with Hyprland not running, name its binary: `HYPR_BIN=/nix/store/…/bin/Hyprland ./build.sh`.

After updating Hyprland, log out and back in, then run `./build.sh` again.

### 3. Load it

Add to `hyprland.conf`:

```ini
plugin = /path/to/hyprwalk/hyprwalk.so
bind = SUPER, grave, hyprwalk:toggle
```

or, with a Lua config:

```lua
hl.plugin.load("/path/to/hyprwalk/hyprwalk.so")
hl.bind("SUPER + grave", function() hl.plugin.hyprwalk.toggle() end)
```

To try it without editing your config: `hyprctl plugin load "$PWD/hyprwalk.so"` (and `hyprctl plugin unload` with the
same path). The plugin runs inside Hyprland, so if it crashes, your session goes down with it.

### With hyprpm

Hyprland's plugin manager works on any distribution, needs no development package, and works with a Hyprland you
built yourself. It needs `git`, `cmake`, `cpio`, `pkg-config`, `gcc`/`g++` and Hyprland's own build dependencies.

```sh
hyprpm update
hyprpm add https://github.com/Marcus-Arcadius/hyprwalk
hyprpm enable hyprwalk
hyprpm reload
```

To load it at login, add `exec-once = hyprpm reload -n` to `hyprland.conf` (Lua:
`hl.on("hyprland.start", function() hl.exec_cmd("hyprpm reload -n") end)`). Run `hyprpm update` after every Hyprland
update.

## Use

Press Super+` (the bind above) to enter 3D, and Esc to leave.

| Key | |
|---|---|
| mouse, W A S D, Shift | look, walk, run |
| Space, Ctrl | jump, crouch |
| F | fly |
| click, wheel | click and scroll the window you look at |
| E | type into the window you look at (Super+Esc stops) |
| P | play it: a game gets the mouse and every key (Super+Esc stops) |
| G | pick the window up; G again puts it down where you look |
| T | tiling: every window in a ring around you |
| Q, B | launch an app into the world, list the windows |
| Tab | the Action Menu: emotes, outfit, maps, avatars, options |
| V | first or third person (with an avatar) |
| Esc | leave 3D |

Keybinds with Super or Ctrl+Alt still go to Hyprland, and act on the window you look at.

To load an avatar or a map, put it in `~/.local/share/hyprwalk/avatars/` or `~/.local/share/hyprwalk/maps/` and pick
it in the Action Menu, or set it in `hyprland.conf`:

```ini
plugin {
    hyprwalk {
        avatar = ~/avatars/me.glb
        map = ~/.local/share/hyprwalk/maps/de_mirage.glb
    }
}
```

VRChat avatars and CS2 maps need converting first (with [Blender](https://www.blender.org) installed):

```sh
python3 tools/unity2hyprwalk.py Avatar.unitypackage -o ~/avatars/me.glb
python3 tools/cs2map.py de_mirage        # from your own CS2 install
```

## More

- [Using hyprwalk](docs/usage.md): every key, play mode, two monitors, lip sync, all config values, `hyprctl`
  commands and known limits
- [Avatars and maps](docs/avatars-and-maps.md): supported formats, the avatar settings file, map markers and the
  converters
- [Tests](docs/tests.md)
- [Credits](docs/credits.md) and [license texts](THIRD_PARTY.md). VRChat, Modular Avatar, Counter-Strike 2 and Blender
  belong to their owners. This project has no connection with any of them and ships none of their assets.
