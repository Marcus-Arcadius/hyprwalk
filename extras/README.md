# extras

What came with hyprwalk but isn't the plugin: fixes for Hyprland and aquamarine, the briefs the work followed,
recordings for testing lip sync, and scripts for checking converted maps.

| Folder | What |
|---|---|
| `hyprland-exit-crash/` | Hyprland 0.55.x crashes when it quits with windows open: its dwindle and master layouts call a window that's gone (see Known limits in docs/usage.md). `hyprland-exit-crash.patch` guards both; `hyprland-exit-crash-root.patch` also changes the order Hyprland cleans up in, and is optional. `patched.nix` builds the Hyprland you run with them, and `nixos-config.md` says how to use that on NixOS. `pr.md` is the text for an upstream PR, and `upstream/` has the patches for Hyprland's main branch. Applied nowhere. |
| `aquamarine-headless-fix/` | aquamarine 0.11.0 runs a removed headless output's queued frame on freed memory. Upstream fixed it in 1699271 and 6ecde03, released in aquamarine 0.12.1. This folder has those commits as patches for 0.11.0, and `aq.nix` to build it. hyprwalk works around the bug itself (see Known limits). |
| `briefs/` | The brief each working session started from (`DATE_TIME_SESSION_brief.md`). Also three reports: phase 13's, the phase 14 notes, and what CS2 does with a sun that has no baked shadow. |
| `speech/` | 28 recordings of the five Japanese vowels by 11 speakers, public domain, CC0, CC BY or CC BY-SA 4.0; `LICENSES.md` credits each. `raw/` is as downloaded, `wav/` whole, and `in/` the vowel looped, for `tools/test/harness/lipsync_check.sh AVATAR.glb --real extras/speech/in`. `ref/` has the Tokyo formant measurements that lip sync's targets moved towards (Yazawa & Kondo 2019, CC BY 4.0). `tools/` has the scripts that found and cut the recordings. |
| `map-tools/` | Scripts used while converting de_mirage and de_dust2. They render reference views (`views.sh`), tile renders into sheets, and measure brightness. They also find materials and look at lightmaps, skies and colour tables. |

Not here, because it's other people's content or too big for git: avatars and their emotes (Booth items),
converted CS2 maps and VRChat's samples. hyprwalk loads them from wherever its config says, such as
`~/.local/share/hyprwalk/avatars` and `~/.local/share/hyprwalk/maps`.
