#!/usr/bin/env bash
# legacy_check.sh: files made when hyprwalk was called hypr3d still load as they did: an avatar with hypr3d_* extras
# (toon, matcap, a part) and an AVATAR.hypr3d.json, and a map with hypr3d_* markers and HYPR3D_* extensions (litmap.py's
# LitCourt.glb: everything tools/cs2map.py writes). legacy.py makes each with today's names and the old ones; their frames
# must be the same, and the settings file must hide its ball.
#
#   tools/test/harness/legacy_check.sh [DIR]   (DIR: where the files and frames go; a temporary one by default)
#
# Needs build/test/shot (tools/test/harness/build.sh).
set -uo pipefail
REPO="$(cd "$(dirname "$0")/../../.." && pwd)"
SHOT="$REPO/build/test/shot"
[[ -x "$SHOT" ]] || { echo "no $SHOT: tools/test/harness/build.sh builds it" >&2; exit 1; }
DIR="${1:-}"
[[ -n "$DIR" ]] || { DIR="$(mktemp -d)"; trap 'rm -rf "$DIR"' EXIT; }
mkdir -p "$DIR"
python3 "$REPO/tools/test/harness/toonballs.py" "$DIR" > /dev/null &&
    python3 "$REPO/tools/test/vm/litmap.py" "$DIR" > /dev/null &&
    python3 "$REPO/tools/test/harness/legacy.py" "$DIR" || exit 1

FAILS=0
check() { # NAME OK DETAIL
    if [[ "$2" == 1 ]]; then echo "ok   $1"; else echo "FAIL $1 ($3)"; FAILS=$((FAILS + 1)); fi
}
shot() { # NAME OPTIONS...
    local name="$1"
    shift
    "$SHOT" --size 480x240 "$@" --frames 2 --out "$DIR/$name.png" > "$DIR/$name.log" 2>&1 ||
        { echo "FAIL the harness ($name): $(tail -3 "$DIR/$name.log")"; exit 1; }
}
same() { cmp -s "$DIR/$1.png" "$DIR/$2.png" && echo 1 || echo 0; }

BALLS=(--view 0 --pitch 0 --dist 3 --fov 45 --target 1.2)
shot balls-all --avatar "$DIR/ToonBalls.glb" "${BALLS[@]}"
for v in new old; do
    shot "balls-$v" --avatar "$DIR/$v/Balls.glb" "${BALLS[@]}"
    for view in spawn desk; do
        shot "court-$v-$view" --map "$DIR/$v/LitCourt.glb" --"$view" --autoexp 1
    done
done
check "the avatar with hypr3d_* extras and Balls.hypr3d.json looks as with today's names" "$(same balls-new balls-old)" \
    "$DIR/balls-new.png vs balls-old.png"
check "its settings file hid the ball tagged as a part" "$((1 - $(same balls-new balls-all)))" "balls-new.png is balls-all.png"
check "Balls.hypr3d.json was read" "$(grep -q 'Balls.hypr3d.json' "$DIR/balls-old.log" && echo 1 || echo 0)" "$DIR/balls-old.log"
for view in spawn desk; do
    check "the map with hypr3d_* markers and HYPR3D_* extensions, from the $view, looks as with today's names" \
        "$(same "court-new-$view" "court-old-$view")" "$DIR/court-new-$view.png vs court-old-$view.png"
done
where() { grep -o 'spawn .* sun' "$DIR/$1.log"; }
check "the same spawn and desktop" "$([[ -n "$(where court-new-spawn)" && "$(where court-new-spawn)" == "$(where court-old-spawn)" ]] && echo 1 || echo 0)" \
    "$(where court-new-spawn) vs $(where court-old-spawn)"

[[ $FAILS == 0 ]] && echo "all legacy checks passed" || echo "$FAILS legacy checks failed"
exit $((FAILS > 0))
