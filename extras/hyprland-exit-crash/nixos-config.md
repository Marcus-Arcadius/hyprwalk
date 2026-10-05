# Applying the exit-crash fix to your Hyprland (zaneyos)

Hyprland 0.55.2 crashes when it quits with two or more tiled windows open, in the dwindle and the master layouts.
`hyprland-exit-crash.patch` fixes both. Your Hyprland session is currently the serpantinum one, which uses dwindle. The
astroland session uses dwindle too by the time Hyprland exits. Either way, the patch covers it.

## 1. Put the patch next to packages.nix

    cp hyprland-exit-crash.patch ~/zaneyos/modules/astroland/
    git -C ~/zaneyos add modules/astroland/hyprland-exit-crash.patch

The `git add` is needed. zaneyos is a flake, and a flake only sees files that git tracks, even in a dirty tree. If the
file isn't added, evaluation fails with "path ... does not exist". You don't have to commit it.

## 2. Add it to the patches in ~/zaneyos/modules/astroland/packages.nix

This goes in the `hyprland = baseHyprland.overrideAttrs (old: { ... })` block. Change this line:

    patches = (old.patches or []) ++ [clampToBorderPatch];

to:

    patches = (old.patches or []) ++ [clampToBorderPatch ./hyprland-exit-crash.patch];

Both of your Hyprland sessions use this package (`astrolandPackages.hyprland`): `serpantinum-hyprland.nix` sets
`hyprlandPkg = astrolandPackages.hyprland`, and `astroland.nix` uses it too. One change covers both.

There's also an optional second patch, `hyprland-exit-crash-root.patch`, which fixes the root cause. It changes the
order Hyprland shuts down in, so that every window is closed normally. It works on its own, and it works with the first
patch; both were tested in the VM. The layout guards alone are enough, though, and they're the smaller change. If you
want both:

    patches = (old.patches or []) ++ [clampToBorderPatch ./hyprland-exit-crash.patch ./hyprland-exit-crash-root.patch];

(`git add` that file as well.)

Don't use upstream's patch through `fetchpatch` any more: 338bdbb3 only fixes dwindle. `hyprland-exit-crash.patch`
already contains its dwindle change, plus the same fix for master.

## 3. Rebuild

Rebuild as usual. Here's what that costs:

- Hyprland builds from source. That took 3.3 to 3.9 minutes here with `--cores 16`: one derivation, nothing to
  download. The output is 68 MB, and the closure is 567 MB, the same as now.
- Everything that depends on the Hyprland package gets rebuilt too: the astroland plugin (`plugin`), and the portal
  (`portal`, which is xdph with this Hyprland). These are small.
- Your plugins keep loading. The plugin API hash is the git commit (39d7e20), and the patch doesn't change it. The
  patches only touch .cpp files, with no header changes, so the ABI stays the same too. hyprwalk.so built for the
  current Hyprland loads into the patched one. You can still run `./build.sh` in the hyprwalk repo after switching, so
  that it builds against the new store path.
- The Hyprland you're running now is the old binary until you log out and back in. So the first logout after
  switching can still crash once. After that, exits are clean.

The build that was tested (this patch, your `packages.nix`, through `builtins.getFlake` of `~/zaneyos`) is
`/nix/store/anyv25wc4mgll7vhv2rsdsrcw879zd03-hyprland-astroland-0.55.2+date=2026-05-16_39d7e20`.

With both patches, the build is
`/nix/store/j75wvpvxkffpkw6bn80irwlaxa5sxsdv-hyprland-astroland-0.55.2+date=2026-05-16_39d7e20`.

If you copy the patch byte for byte, keeping its file name, your rebuild should reuse that build instead of compiling.
Nix stores a patch file under a path made from its name and its content, and gives it the same path whether it comes
from a flake or not. For example, `astroland-stability.patch` is `/nix/store/a667bnx7...` both ways. So the patch
becomes `/nix/store/kv27shgm67shxxs2fhp20lhh68hb7vxr-hyprland-exit-crash.patch`, and your Hyprland derivation is
the same as the tested one. The same goes for the root-cause patch,
`/nix/store/hmpw67d3fxj2ygwmsmdizkaa1a88nizi-hyprland-exit-crash-root.patch`. This only works while those builds are
still in the store. Their GC roots are symlinks in session 085051d7's scratchpad (`/tmp/claude-1000/-home-monero-Documents-3D/085051d7-540b-4886-ae17-dbd3dfc7c646/scratchpad/hypr/patched-exit` and `patched-both`). A
reboot wipes the scratchpad, and the next garbage collection after that deletes the builds.
