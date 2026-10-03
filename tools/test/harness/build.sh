#!/usr/bin/env bash
# Builds build/test/shot, the offscreen render harness, through the repo's build.sh: it links the plugin's own objects,
# which that rebuilds when stale.
set -euo pipefail
exec "$(dirname "$0")/../../../build.sh" -f tools/test/harness/harness.mk "$@"
