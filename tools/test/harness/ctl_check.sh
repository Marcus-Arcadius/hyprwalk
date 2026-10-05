#!/usr/bin/env bash
# ctl_check.sh: the avatar commands and Action Menu glue (src/control.cpp, shared by main.cpp and the harness),
# driven through the harness as hyprctl, keys and mouse would.
#
#   tools/test/harness/ctl_check.sh DIR
#
# DIR has BoothAccessories.glb and SynthDances.glb with their settings and emote files, as regress.sh --keep (with
# --items) leaves them in OUT/new. Needs build/test/shot (tools/test/harness/build.sh), and ffmpeg for the sound.
set -uo pipefail
DIR="${1:?usage: ctl_check.sh DIR, the OUT/new that regress.sh --keep leaves}"
REPO="$(cd "$(dirname "$0")/../../.." && pwd)"
SHOT="$REPO/build/test/shot"
[[ -x "$SHOT" ]] || { echo "no $SHOT: tools/test/harness/build.sh builds it" >&2; exit 1; }
FAILS=0
check() { # WHAT OUTPUT LINE: OUTPUT must contain LINE
    if grep -qF -- "$3" <<< "$2"; then echo "ok   $1"; else echo "FAIL $1: no \"$3\""; FAILS=$((FAILS + 1)); fi
}

A="$DIR/BoothAccessories.glb"
if [[ -f "$A" ]]; then
    # 1280x800 as in the VM: menu radius 224 px, the same numbers as tools/test/vm/checks.py
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
    # the root page: nine slots (no "More"), Avatars last; a wheel notch back from the middle or the mouse at 320
    # degrees reaches the ninth; pick takes 1-9
    out="$("$SHOT" --size 1280x800 --avatar "$A" --key tab --ctl menu --wheel -1 --ctl menu --key esc --key tab --ctl menu \
        --mouse -96 -115 --ctl menu --ctl "menu pick 10" 2>&1)"
    check 'the root page: nine, Avatars last, its hint the avatar'"'"'s name' "$out" '{"slot": 9, "label": "Avatars", "hint": "BoothAccessories", "on": false, "disabled": false, "submenu": true'
    hl="$(grep '^ctl menu -> ' <<< "$out" | grep -o '"highlight": -\?[0-9]*' | awk '{printf "%s ", $2}')"
    if [[ "$hl" == "0 9 0 9 " ]]; then
        echo "ok   ... the wheel back from the middle and the mouse at 320 degrees: the ninth"
    else
        echo "FAIL the ninth by the wheel and the mouse: highlights $hl(want 0 9 0 9)"; FAILS=$((FAILS + 1))
    fi
    check '... pick takes 1-9' "$out" 'ctl menu pick 10 -> error: pick 1-9, or 0 for the middle'
    # the tail held in the world ("fixed", the converter's form of MA's World Fixed Object), without its spring
    W="$(mktemp -d)"
    cp "$A" "$W/"
    python3 -c 'import json, sys; s = json.load(open(sys.argv[1], encoding="utf-8")); s["fixed"] = ["Tail"]; s.pop("springs", None)
json.dump(s, open(sys.argv[2], "w", encoding="utf-8"), ensure_ascii=False)' "${A%.glb}.hyprwalk.json" "$W/BoothAccessories.hyprwalk.json"
    out="$("$SHOT" --size 64x64 --avatar "$W/BoothAccessories.glb" --frames 5 --where Tail --where Head --accel 40 --move 0 3 --frames 60 \
        --where Tail --where Head 2>&1)"
    rm -rf "$W"
    tails=($(grep '^where Tail:' <<< "$out" | awk '{print $3 "," $4 "," $5}')) heads=($(grep '^where Head:' <<< "$out" | awk '{print $5}'))
    if [[ ${#tails[@]} == 2 && "${tails[0]}" == "${tails[1]}" && ${#heads[@]} == 2 ]] && awk -v a="${heads[0]}" -v b="${heads[1]}" 'BEGIN {exit !(b - a > 2)}'; then
        echo "ok   a node held in the world stays where it was while the avatar walks 3 m (at ${tails[0]})"
    else
        echo "FAIL a node held in the world: tail ${tails[*]}, head z ${heads[*]}"; FAILS=$((FAILS + 1))
    fi
else
    echo "skipped: no $A"
fi

D="$DIR/SynthDances.glb"
if [[ -f "$D" ]]; then
    T="$(mktemp -d)"
    trap 'rm -rf "$T"' EXIT
    cp "$D" "$DIR"/SynthDances.*.vrma "$T/"
    # the same with an emote at twice its speed
    sed 's/"name": "Loli Kami Requiem",/"name": "Loli Kami Requiem", "speed": 2,/' "$DIR/SynthDances.hyprwalk.json" > "$T/SynthDances.hyprwalk.json"
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
    # an emote's "sound" (a file next to the settings): decoded at load and listed; a missing one is reported and its
    # emote plays without it
    if command -v ffmpeg > /dev/null; then
        ffmpeg -v error -y -f lavfi -i "aevalsrc=exprs=0.5*sin(2*PI*440*t):s=44100:d=1.5" -c:a libvorbis "$T/song.ogg"
        sed -e 's/"name": "Loli Kami Requiem",/"name": "Loli Kami Requiem", "sound": "song.ogg",/' \
            -e 's/"name": "Doodle Dance",/"name": "Doodle Dance", "sound": "gone.ogg",/' "$DIR/SynthDances.hyprwalk.json" > "$T/SynthDances.hyprwalk.json"
        out="$("$SHOT" --size 64x64 --avatar "$T/SynthDances.glb" --ctl "avatar emote" --ctl "avatar emote Loli Kami Requiem" --frames 10 \
            --ctl "avatar emote" 2>&1)"
        check 'an emote'"'"'s sound, listed with it' "$out" '"sound": {"file": "song.ogg", "duration": 1.50, "rate": 44100, "channels": 1}'
        check '... in the log' "$out" 'sound song.ogg, 1.5 s)'
        check 'one that isn'"'"'t there: said' "$out" 'gone.ogg can'"'"'t be read'
        check '... its emote without one' "$out" '"name": "Doodle Dance", "from": "SynthDances.Doodle Dance.vrma", "loop": true, "hold": false, "duration"'
        check 'playing one: how far into it (no speaker here)' "$out" '{"playing": "Loli Kami Requiem", "time": 0.'
    else
        echo "skipped: an emote's sound (no ffmpeg to make one)"
    fi
else
    echo "skipped: no $D"
fi
((FAILS)) && { echo "$FAILS FAILED"; exit 1; }
echo "all passed"
