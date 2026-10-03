#!/usr/bin/env bash
# spring_check.sh: an avatar's springs (its hair, skirt, necktie...: the plugin's physics, through the harness) as it
# stands, walks and stops, runs and stops, turns right round, turns on the spot, jumps, dances (the built-in Dance) and
# runs in first person (what the harness has of those), at the plugin's own speeds and accelerations. Checks that no
# bone with a limit (a PhysBone's, VRMC_springBone_limit's) ever gets out of it, as the harness works out on its own
# where each one is (--limits, every 5 frames), and that the kinds of spring named (a spring's name up to its first
# '.', as Necktie for Necktie.A.001) are never put more than 2 cm deeper inside the body than the animation has them
# (--springclip, every frame), and that every kind swings as smoothly at 143.9 frames a second (a 144 Hz monitor's) as
# at 60 (--springtrace), in first person turning too. Prints how deep every kind went, in each move.
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
    if [[ "$2" == 1 ]]; then echo "ok   $1${3:+: $3}"; else echo "FAIL $1${3:+: $3}"; FAILS=$((FAILS + 1)); fi
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

# smooth at any frame rate: the springs step 60 times a second, and a monitor's frames (143.9 a second) fall between
# the steps. How each kind of spring swings on what it hangs from (each bone's tail in its parent's frame), its wobble
# about its own smooth path (the angle from a moving 1/20 s average, rms over the frames, its bones' mean), at 143.9
# frames a second no more than half as much again as at 60, and 0.05 deg: in first person turning (the sleeves on the
# arms in view), walking round a corner, running, dancing, turning on the spot
if has --springtrace; then
    secs() { awk -v s="$1" -v dt="$2" 'BEGIN { printf "%d", s / dt + 0.5 }'; }
    smooth=(fpturn walkturn run dance spot)
    has --fpturn || { smooth=("${smooth[@]:1}"); echo "(the harness can't turn its camera: no first person turning)"; }
    for hz in 60 143.9; do
        dt="$(awk -v hz="$hz" 'BEGIN { printf "%.7f", 1 / hz }')"
        for name in "${smooth[@]}"; do
            case "$name" in
                fpturn) opts=(--fpbody 0 0 --fpfollow 1 --frames "$(secs 1 "$dt")" --fpturn 90 0) ;;
                walkturn) opts=("${go[@]}" --frames "$(secs 0.5 "$dt")" --move 0 -1.6 --frames "$(secs 1 "$dt")" --move 1.6 0) ;;
                run) opts=("${go[@]}" "${run[@]}" --frames "$(secs 0.5 "$dt")" --move 0 -4.5 --frames "$(secs 1 "$dt")" --move 0 0) ;;
                dance) opts=(--frames "$(secs 0.5 "$dt")" --emote Dance loop) ;;
                spot) opts=(--frames "$(secs 0.5 "$dt")" --turn 360) ;;
            esac
            "$SHOT" --size 64x64 --avatar "$AVATAR" --dt "$dt" "${opts[@]}" --springtrace "$DIR/$name@$hz.trace" "" --frames "$(secs 2 "$dt")" > "$DIR/$name@$hz.log" 2>&1 ||
                { echo "FAIL the harness ($name at $hz frames a second): $(tail -3 "$DIR/$name@$hz.log")"; exit 1; }
        done
    done
    while read -r ok what; do
        check "$what" "$ok" ""
    done < <(python3 - "$DIR" "${smooth[@]}" << 'EOF'
import bisect, math, sys

D, moves = sys.argv[1], sys.argv[2:]


def wobble(path):
    """per kind of spring, its bones' wobble about their own smooth paths (degrees rms), their mean"""
    lines = open(path, encoding='utf-8', errors='replace').read().split('\n')
    kinds = [k.strip('[] ') for k in lines[0].split(' [')[1:]]
    rows = [list(map(float, l.split())) for l in lines[1:] if l.strip()]
    ts, H, per = [r[0] for r in rows], 1 / 40, {}
    for j, kind in enumerate(kinds):
        dirs = []
        for r in rows:
            v = r[1 + 3 * j:4 + 3 * j]
            n = math.sqrt(sum(x * x for x in v)) or 1
            dirs.append([x / n for x in v])
        dev = []
        for i, t in enumerate(ts):
            if t - ts[0] < H or ts[-1] - t < H:
                continue
            lo, hi = bisect.bisect_left(ts, t - H - 1e-9), bisect.bisect_right(ts, t + H + 1e-9)
            m = [sum(dirs[k][q] for k in range(lo, hi)) for q in range(3)]
            n = math.sqrt(sum(x * x for x in m)) or 1
            dev.append(math.degrees(math.acos(max(-1, min(1, sum(m[q] / n * dirs[i][q] for q in range(3)))))))
        if dev:
            per.setdefault(kind, []).append(math.sqrt(sum(x * x for x in dev) / len(dev)))
    return {k: sum(v) / len(v) for k, v in per.items()}


for move in moves:
    slow, fast = wobble(f'{D}/{move}@60.trace'), wobble(f'{D}/{move}@143.9.trace')
    for kind in sorted(slow):
        a, b = slow[kind], fast.get(kind, 0.)
        print(1 if b <= 1.5 * a + 0.05 else 0, f'{kind} as smooth at 143.9 frames a second as at 60 ({move}): {b:.3f} deg, at 60 {a:.3f}')
EOF
)
else
    echo "(the harness has no --springtrace: not checked at 143.9 frames a second)"
fi

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
