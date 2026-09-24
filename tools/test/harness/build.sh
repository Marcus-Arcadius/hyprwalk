#!/usr/bin/env bash
# Builds build/test/shot, the offscreen render harness. It links the plugin's own
# objects, so it goes through the repo's build.sh: that compiles in the build shell
# of the Hyprland you are running, and rebuilds any plugin object that is stale.
set -euo pipefail
exec "$(dirname "$0")/../../../build.sh" -f tools/test/harness/harness.mk "$@"
