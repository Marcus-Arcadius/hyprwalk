#!/usr/bin/env bash
# ctl_check.sh: the plugin's avatar commands and Action Menu glue (src/control.cpp, which main.cpp and the harness
# share) driven through the harness as hyprctl and the keyboard and mouse would: sliders (one and two axes, by
# number and percent), toggles and the material variants they switch, the menu's pages, a slider's dial by the mouse,
# the wheel and a click, a two-axis puppet's stick, and an emote's speed.
#   tools/test/harness/ctl_check.sh DIR
# DIR has BoothAccessories.glb and SynthDances.glb with their settings and emote files, as regress.sh --keep (with
# --items) leaves them in OUT/new. Needs build/test/shot (tools/test/harness/build.sh).
set -uo pipefail
DIR="${1:?usage: ctl_check.sh DIR, the OUT/new that regress.sh --keep leaves}"
REPO="$(cd "$(dirname "$0")/../../.." && pwd)"
SHOT="$REPO/build/test/shot"
[[ -x "$SHOT" ]] || { echo "no $SHOT: tools/test/harness/build.sh builds it" >&2; exit 1; }
FAILS=0
check() { # what, the harness output, a line it must have
    if grep -qF -- "$3" <<< "$2"; then echo "ok   $1"; else echo "FAIL $1: no \"$3\""; FAILS=$((FAILS + 1)); fi
}

A="$DIR/BoothAccessories.glb"
if [[ -f "$A" ]]; then
    # (at the VM's 1280x800, the menu's radius is 224 px: the same numbers as tools/test/vm/checks.py's)
    out="$("$SHOT" --size 1280x800 --avatar "$A" \
        --ctl "avatar slider ニーハイの緩さ 50%" --ctl "avatar slider しっぽの向き 0.5 -0.25" \
        --ctl "avatar slider しっぽの向き 0.5" --ctl "avatar toggle 紺の制服 on" --ctl "avatar parts" --ctl "avatar parts reset" \
        --key tab --key 4 --key 8 --key 8 --key 1 --mouse 150 0 --ctl menu --wheel 2 --ctl menu --click left \
        --key 3 --mouse 90 -45 --ctl menu --key backspace --key esc --ctl menu --ctl "avatar parts" 2>&1)"
    check 'a slider by percent' "$out" 'ニーハイの緩さ: 50%'
    check 'a two-axis one by x and y' "$out" 'しっぽの向き: +50% -25%'
    check '... which takes both' "$out" 'has no slider "しっぽの向き 0.5"'
    check 'a toggle' "$out" '紺の制服: on'
    check '... and the material variant it switches' "$out" '{"name": "紺の制服", "on": true}'
    check 'all as it came again' "$out" 'ctl avatar parts reset -> ok'
    check 'Tab, 4, More, More, 1: the third outfit page'"'"'s first slider'"'"'s dial' \
        "$out" '"path": "main/outfit:3/~ニーハイの緩さ", "title"'
    check '... turned by the mouse, 150 counts right of the top (224 px out): 12.6%' "$out" '"dial": {"label": "ニーハイの緩さ", "value": 0.126}'
    check '... and two wheel notches on: 25%' "$out" '"dial": {"label": "ニーハイの緩さ", "value": 0.250}'
    check 'a stick moved by the mouse, 90 and -45 counts' "$out" '"dial": {"label": "しっぽの向き", "value": [0.414, 0.207]}'
    check 'Backspace and Esc: closed' "$out" 'ctl menu -> {"open": false}'
    check 'what the dial and the stick set, kept' "$out" '{"name": "ニーハイの緩さ", "value": 0.250}'
    check '...' "$out" '{"name": "しっぽの向き", "value": [0.414, 0.207]}'
else
    echo "skipped: no $A"
fi

D="$DIR/SynthDances.glb"
if [[ -f "$D" ]]; then
    T="$(mktemp -d)"
    trap 'rm -rf "$T"' EXIT
    cp "$D" "$DIR"/SynthDances.*.vrma "$T/"
    # the same with an emote at twice its speed
    sed 's/"name": "Loli Kami Requiem",/"name": "Loli Kami Requiem", "speed": 2,/' "$DIR/SynthDances.hypr3d.json" > "$T/SynthDances.hypr3d.json"
    where=(--where Hand_L --where Hand_R --where Head)
    fast="$("$SHOT" --size 64x64 --avatar "$T/SynthDances.glb" --ctl "avatar emote" --ctl "avatar emote Loli Kami Requiem once" \
        --frames 90 "${where[@]}" 2>&1)"
    slow="$("$SHOT" --size 64x64 --avatar "$D" --ctl "avatar emote Loli Kami Requiem once" --frames 180 "${where[@]}" 2>&1)"
    check 'an emote'"'"'s speed, listed' "$fast" '"name": "Loli Kami Requiem", "from"'
    check '...' "$fast" '"speed": 2.000'
    if [[ "$(grep '^where' <<< "$fast")" == "$(grep '^where' <<< "$slow")" && -n "$(grep '^where' <<< "$fast")" ]]; then
        echo "ok   twice as fast: in 90 frames where it is in 180"
    else
        echo "FAIL twice as fast: 90 frames and 180 differ"; FAILS=$((FAILS + 1))
    fi
else
    echo "skipped: no $D"
fi
((FAILS)) && { echo "$FAILS FAILED"; exit 1; }
echo "all passed"
