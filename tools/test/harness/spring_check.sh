#!/usr/bin/env bash
# spring_check.sh: an avatar's springs (its hair, skirt, necktie...: the plugin's physics, through the harness) as it
# stands, walks and stops, runs and stops, turns right round, turns on the spot, jumps, dances (the built-in Dance) and
# runs in first person (what the harness has of those), at the plugin's own speeds and accelerations. Checks that no
# bone with a limit (a PhysBone's, VRMC_springBone_limit's) ever gets out of it, as the harness works out on its own
# where each one is (--limits, every 5 frames), and that the kinds of spring named (a spring's name up to its first
# '.', as Necktie for Necktie.A.001) are never put more than 2 cm deeper inside the body than the animation has them
# (--springclip, every frame). Prints how deep every kind went, in each move.
#   tools/test/harness/spring_check.sh AVATAR [DIR] [KIND...]   (DIR: where the logs go, a temporary one by default; "" too)
# Hatsune Miku NT's necktie (a Booth avatar, local only): spring_check.sh ~/.local/share/hypr3d/avatars/Miku/Miku.glb ""
# Necktie. Needs build/test/shot (tools/test/harness/build.sh).
set -uo pipefail
REPO="$(cd "$(dirname "$0")/../../.." && pwd)"
SHOT="$REPO/build/test/shot"
[[ -x "$SHOT" ]] || { echo "no $SHOT: tools/test/harness/build.sh builds it" >&2; exit 1; }
AVATAR="${1:-}"
[[ -f "$AVATAR" ]] || { echo "usage: spring_check.sh AVATAR [DIR] [KIND...]" >&2; exit 2; }
DIR="${2:-}"
KINDS=("${@:3}")
[[ -n "$DIR" ]] || { DIR="$(mktemp -d)"; trap 'rm -rf "$DIR"' EXIT; }
mkdir -p "$DIR"
FAILS=0
check() { # what, ok (1/0), the value
    if [[ "$2" == 1 ]]; then echo "ok   $1: $3"; else echo "FAIL $1: $3"; FAILS=$((FAILS + 1)); fi
}

# (as the plugin moves: 10 m/s² to a walk or a run, 14 to a stop, 7 turning right back, the body turned toward where it
# goes; a harness from before it walked so has neither, and no first person body: 40 m/s², and those moves left out;
# --limits every 5 frames)
has() { grep -qaF -- "$1" "$SHOT"; }
lim=()
for i in $(seq 60); do lim+=(--frames 5 --limits); done
if has --turnback && has --face && has --run; then
    go=(--accel 10 --decel 14 --turnback 7 --face 1) run=(--run 1)
else
    go=(--accel 40) run=()
    echo "(the harness walks as the plugin did before --face: no turning right round)"
fi
moves=(
    "idle|--frames 60"
    "walk|${go[*]} --move 0 -1.6 --frames 120 --move 0 0"
    "run|${go[*]} ${run[*]} --move 0 -4.5 --frames 120 --move 0 0"
    "spot|--turn 360 --frames 120 --turn 0"
    "jump|--frames 30 --jump --frames 60 --jump"
    "dance|--frames 30 --emote Dance loop"
)
[[ ${#run[@]} -gt 0 ]] && moves+=("turnround|${go[*]} --move 0 -1.6 --frames 120 --move 0 1.6 --frames 120 --move 0 -1.6")
if has --fpbody; then
    moves+=("fprun|--fpbody 0 0 --frames 60 ${go[*]} ${run[*]} --move 0 -4.5 --frames 120 --move 0 0")
else
    echo "(the harness has no first person body: no run in first person)"
fi
for m in "${moves[@]}"; do
    name="${m%%|*}"
    read -r -a opts <<< "${m#*|}"
    "$SHOT" --size 64x64 --avatar "$AVATAR" --springclip "$DIR/$name.clip" "${opts[@]}" "${lim[@]}" > "$DIR/$name.log" 2>&1 ||
        { echo "FAIL the harness ($name): $(tail -3 "$DIR/$name.log")"; exit 1; }
done

# the limits: the worst any bone was out of its own, over every move
worst="$(cat "$DIR"/*.log | awk '/^limits:/ { if ($6 + 0 > w) { w = $6 + 0; at = $0 } n = $2 } END { printf "%.2f deg (%d bones with limits)%s", w, n, (w > 0 ? ", " at : "") }')"
check "no bone with a limit gets out of it" "$(awk -v w="${worst%% *}" 'BEGIN { print (w <= 0.5) ? 1 : 0 }')" "the worst $worst"

# how deep each kind went (cm, beyond the animation), and how many of its vertices at the worst, per move
report() {
    awk 'NR == 1 { n = split($0, h, "\\["); for (i = 2; i <= n; ++i) { k = h[i]; sub(/:.*/, "", k); kind[i - 1] = k } next }
         { for (i = 1; i in kind; ++i) { c = $(2 * i); d = $(2 * i + 1); if (d > deep[i]) deep[i] = d; if (c > most[i]) most[i] = c } }
         END { for (i = 1; i in kind; ++i) printf "%s\t%.1f\t%d\n", kind[i], 100 * deep[i], most[i] }' "$1"
}
echo "how far each kind of spring went into the body beyond the animation (cm, the most vertices), per move:"
for m in "${moves[@]}"; do
    name="${m%%|*}"
    printf '  %-10s %s\n' "$name" "$(report "$DIR/$name.clip" | awk -F'\t' '$2 > 0 || $3 > 0 { printf "%s %s (%s)  ", $1, $2, $3 }')"
done
for k in "${KINDS[@]}"; do
    deepest="$(for m in "${moves[@]}"; do report "$DIR/${m%%|*}.clip" | awk -F'\t' -v k="$k" -v m="${m%%|*}" '$1 == k { print $2, m }'; done | sort -n -r | head -1)"
    [[ -n "$deepest" ]] || { check "$k kept out of the body" 0 "no such kind of spring"; continue; }
    check "$k kept out of the body (no more than 2 cm deeper than the animation has it)" \
        "$(awk -v d="${deepest%% *}" 'BEGIN { print (d <= 2) ? 1 : 0 }')" "the deepest ${deepest%% *} cm (${deepest#* })"
done
((FAILS)) && echo "$FAILS failed" || echo "all ok"
exit $((FAILS > 0))
