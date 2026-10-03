#!/usr/bin/env bash
# Tiling mode's ring (src/tiling.cpp) on its own, no Hyprland needed: builds build/test/tile_unit with the repo's
# build.sh and runs it (a line per check, exit 1 on a failure).
set -euo pipefail
cd "$(dirname "$0")/../../.."
./build.sh -f tools/test/tiling/tiling.mk > build/tile_unit-build.log 2>&1 || { cat build/tile_unit-build.log; exit 1; }
exec build/test/tile_unit
