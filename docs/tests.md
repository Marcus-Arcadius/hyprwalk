# Tests

Back to the [README](../README.md).

Everything is in `tools/test`. Each script's header (or `--help`) explains it.

- `harness/`: `shot`, an offscreen renderer that drives the plugin's own renderer, animator and Action Menu
  (`tools/test/harness/build.sh` builds `build/test/shot`; `grep 'a == "--' tools/test/harness/shot.cpp` lists
  its options). The checks built on it: `ctl_check.sh`, `toon_check.sh`, `fp_check.sh`, `attack_check.sh`,
  `spring_check.sh`, `disc_check.sh`, `cs2mat_check.sh`, `lipsync_check.sh` and `legacy_check.sh` (files from when
  hyprwalk was hypr3d).
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
