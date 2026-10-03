#!/usr/bin/env bash
# Tiling mode's ring on its own (src/tiling.cpp, no Hyprland needed to run it): builds build/test/tile_unit through the
# repo's build.sh and runs it; it prints a line a check and exits 1 when one fails
set -euo pipefail
cd "$(dirname "$0")/../../.."
./build.sh -f tools/test/tiling/tiling.mk > build/tile_unit-build.log 2>&1 || { cat build/tile_unit-build.log; exit 1; }
exec build/test/tile_unit
