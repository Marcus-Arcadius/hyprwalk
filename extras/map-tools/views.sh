#!/usr/bin/env bash
# views.sh BIN GLB OUTDIR [extra args before the views]: renders the reference views
set -euo pipefail
BIN=$1; GLB=$2; OUT=$3; shift 3
mkdir -p "$OUT"
"$BIN" --map "$GLB" --size 1280x720 "$@" \
  --spawn --out "$OUT/start.png" \
  --spawn --fp 0 3 --out "$OUT/behind.png" \
  --stand -22.56 -3.5 -4.11 90 5 --out "$OUT/mid.png" \
  --eye -48.514 -3.454 -8.458 -69 16 --out "$OUT/asite.png" \
  --eye -48.158 -4.446 -45.110 144 0 --out "$OUT/ctspawn.png" \
  --eye 15.646 -1.524 -40.030 -61 -9 --out "$OUT/bsite.png" \
  --stand -20.01 -3.5 -38.40 0 5 --out "$OUT/ctcorner.png" \
  --stand -34.60 -3.5 33.65 0 5 --out "$OUT/palace.png" \
  --stand 4.11 -3.0 -64.77 0 5 --out "$OUT/bencl.png"
