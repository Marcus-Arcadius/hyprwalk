#!/usr/bin/env bash
# sound_check.sh: emote sounds (src/sound.cpp's decoding, src/speaker.cpp's PipeWire playback) against a private
# PipeWire with no devices or session manager, so nothing is heard and the desktop's PipeWire isn't touched. Decoding
# is compared with ffmpeg's, playback recorded by pw-record sample by sample.
#
#   tools/test/sound/sound_check.sh [--song FILE.ogg]... [WORKDIR]
#
#   --song FILE.ogg  a real song: compared with ffmpeg's and libvorbis's decodings, and played looping across its end
#   WORKDIR          the sounds, recordings and logs (default build/test/sound_check)
#
# Needs ffmpeg, ffprobe, pipewire, pw-record, pw-link and Blender (numpy); builds build/test/sound_test with build.sh.
# Prints a line per check; exits 1 when one fails.
set -uo pipefail
REPO="$(cd "$(dirname "$0")/../../.." && pwd)"
HERE="$REPO/tools/test/sound"
SONGS=() WORK=""
while (($#)); do
    case "$1" in
        --song) SONGS+=("$(realpath "$2")"); shift 2 ;;
        -h|--help) sed -n '2,/^set -uo/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
        *) WORK="$1"; shift ;;
    esac
done
WORK="${WORK:-$REPO/build/test/sound_check}"
rm -rf "$WORK"
mkdir -p "$WORK"
WORK="$(realpath "$WORK")"
for c in ffmpeg ffprobe pipewire pw-record pw-link blender; do
    command -v "$c" > /dev/null || { echo "sound_check.sh: needs $c" >&2; exit 1; }
done
"$REPO/build.sh" -f tools/test/sound/sound.mk > "$WORK/build.log" 2>&1 || { tail -n 30 "$WORK/build.log"; exit 1; }
T="$REPO/build/test/sound_test"

# test sounds: a chirp per stereo channel (no stretch matches another), mono, 44.1 kHz (resampled), unplayable ones
S="$WORK/sounds"
mkdir -p "$S"
chirps='0.5*sin(2*PI*(200*t+450*t*t))|0.4*sin(2*PI*(300*t+200*t*t))'
mk() { ffmpeg -v error -y -f lavfi -i "aevalsrc=exprs=$1:s=$2:d=$3" "${@:5}" "$S/$4"; }
mk "$chirps" 48000 1.5 chirp.ogg -c:a libvorbis -q:a 6
mk '0.5*sin(2*PI*(500*t+300*t*t))' 48000 1 mono.ogg -c:a libvorbis -q:a 6
mk "$chirps" 44100 2 rate44.ogg -c:a libvorbis -q:a 6
mk "$chirps" 48000 1 opus.ogg -c:a libopus
mk "$chirps|0.3*sin(2*PI*700*t)" 48000 0.5 three.ogg -c:a libvorbis
python3 -c 'import random, sys; r = random.Random(7); open(sys.argv[1], "wb").write(bytes(r.randrange(256) for _ in range(4000)))' "$S/junk.ogg"
: > "$S/empty.ogg"

FAILS=0
ok() { echo "ok   $1"; }
fail() { echo "FAIL $1"; FAILS=$((FAILS + 1)); }

# decoding: must match ffmpeg's samples for the declared length (last granule position; ffmpeg may drop the last block)
CASES="$WORK/cases.jsonl"
: > "$CASES"
decode() { # NAME FILE: decode to $WORK/NAME.s16, print the result
    "$T" decode "$2" "$WORK/$1.s16" 2>&1
}
declared() { # the frames a file says it has
    ffprobe -v error -select_streams a:0 -show_entries stream=duration_ts -of csv=p=0 "$1"
}
compare() { # NAME DECODED FILE SAID REF... (ffmpeg, libvorbis)
    local name="$1" d="$2" file="$3" said="$4" frames ref
    frames="$(declared "$file")"
    shift 4
    for ref in "$@"; do
        if [[ "$ref" == libvorbis ]]; then
            ffmpeg -v error -y -c:a libvorbis -i "$file" -f s16le -acodec pcm_s16le "$WORK/$d.$ref.s16"
        else
            ffmpeg -v error -y -i "$file" -f s16le -acodec pcm_s16le "$WORK/$d.$ref.s16"
        fi
        printf '{"kind": "decode", "name": "%s (%s)", "ours": "%s", "ref": "%s", "frames": %s, "said": "%s"}\n' "$name" "$ref" \
            "$WORK/$d.s16" "$WORK/$d.$ref.s16" "${frames:-0}" "$said" >> "$CASES"
    done
}
for n in chirp mono rate44; do
    r="$(decode "$n" "$S/$n.ogg")"
    [[ "$r" == rate* ]] && ok "$n.ogg decodes: $r" || fail "$n.ogg doesn't decode: $r"
    compare "$n.ogg" "$n" "$S/$n.ogg" "$r" ffmpeg
done
for x in "opus.ogg:Opus" "three.ogg:3 channels" "junk.ogg:isn't an Ogg" "empty.ogg:is empty" "missing.ogg:can't be read"; do
    f="${x%%:*}" want="${x#*:}"
    r="$(decode bad "$S/$f")"
    [[ "$r" == error:*"$want"* ]] && ok "$f: $r" || fail "$f: \"$r\", not an error saying \"$want\""
done
k=0
for song in "${SONGS[@]}"; do
    k=$((k + 1))
    r="$(decode "song$k" "$song")"
    [[ "$r" == rate* ]] && ok "$(basename "$song") decodes: $r" || fail "$(basename "$song") doesn't decode: $r"
    compare "$(basename "$song")" "song$k" "$song" "$r" ffmpeg libvorbis
done

# a private PipeWire; its socket in a short dir (socket paths are limited)
RUN="$(mktemp -d "${XDG_RUNTIME_DIR:-/tmp}/hyprwalk-sound.XXXXXX")"
DAEMON=""
cleanup() {
    local p
    for p in $(jobs -p); do
        kill "$p" 2> /dev/null
    done
    wait 2> /dev/null
    rm -rf "$RUN"
}
trap cleanup EXIT
trap 'exit 1' INT TERM
# clients read PIPEWIRE_CONFIG_DIR's client.conf, so their streams set up their own ports
export PIPEWIRE_RUNTIME_DIR="$RUN" PIPEWIRE_REMOTE=hyprwalk-sound-test PIPEWIRE_CONFIG_DIR="$HERE/pipewire"
pipewire -c pipewire.conf > "$WORK/pipewire.log" 2>&1 &
DAEMON=$!
for _ in $(seq 50); do
    [[ -S "$RUN/hyprwalk-sound-test" ]] && break
    sleep 0.1
done
[[ -S "$RUN/hyprwalk-sound-test" ]] || { cat "$WORK/pipewire.log"; echo "sound_check.sh: its PipeWire didn't start" >&2; exit 1; }

port() { # -o|-i NAME: wait up to 5 s for the port
    for _ in $(seq 100); do
        timeout 2 pw-link "$1" 2> /dev/null | grep -qx "$2" && return 0
        sleep 0.05
    done
    return 1
}

# play NAME SOUND DECODED CHANNELS KIND [sound_test options]: into pw-record (unless KIND is "unlinked"), linked by hand
# once both are there; KIND picks the comparison
play() {
    local name="$1" file="$2" decoded="$3" ch="$4" kind="$5"
    shift 5
    local rec="$WORK/$name.f32" log="$WORK/$name.log" out="$WORK/$name.out" pr="" ports
    [[ "$ch" == 1 ]] && ports=(MONO) || ports=(FL FR)
    if [[ "$kind" != unlinked ]]; then
        pw-record --raw --format f32 --rate 48000 --channels "$ch" -P '{ node.name = hyprwalk-rec }' "$rec" > "$WORK/$name.rec.log" 2>&1 &
        pr=$!
    fi
    "$T" play "$file" --log "$log" "$@" > "$out" 2>&1 &
    local st=$!
    if [[ -n "$pr" ]]; then
        # link all ports at once by node name: a channel linked first would start alone
        local there=1
        for p in "${ports[@]}"; do
            port -o "hyprwalk-emote-sound:output_$p" && port -i "hyprwalk-rec:input_$p" || { there=0; fail "$name: no ports to link ($p)"; }
        done
        ((there)) && { timeout 5 pw-link hyprwalk-emote-sound hyprwalk-rec || fail "$name: couldn't link them"; }
    fi
    wait "$st"
    local status=$?
    if [[ -n "$pr" ]]; then
        sleep 0.3
        kill -INT "$pr" 2> /dev/null
        wait "$pr" 2> /dev/null
    fi
    python3 - "$CASES" "$name" "$kind" "$decoded" "$ch" "$rec" "$log" "$out" "$status" "$*" << 'EOF'
import json, sys
cases, name, kind, decoded, ch, rec, log, out, status, opts = sys.argv[1:]
opts = opts.split()
def opt(o, d):
    return float(opts[opts.index(o) + 1]) if o in opts else d
said = open(out, encoding='utf-8').read().splitlines()
case = {'kind': kind, 'name': name, 'decoded': decoded, 'channels': int(ch), 'rec': rec, 'log': log, 'status': int(status),
        'loop': '--loop' in opts, 'from': opt('--from', 0.0), 'volume': opt('--volume', 1.0), 'for': opt('--for', 2.0),
        'fade': opt('--fade', 0.3), 'said': said}
open(cases, 'a', encoding='utf-8').write(json.dumps(case) + '\n')
EOF
}

play once "$S/chirp.ogg" "$WORK/chirp.s16" 2 exact --for 1
play loop "$S/chirp.ogg" "$WORK/chirp.s16" 2 exact --loop --for 4 --volume 0.5
play ends "$S/chirp.ogg" "$WORK/chirp.s16" 2 exact --from 1.2 --for 3
play quick "$S/chirp.ogg" "$WORK/chirp.s16" 2 exact --loop --from 0.7 --for 0.6 --fade 0.05
play mono "$S/mono.ogg" "$WORK/mono.s16" 1 exact --loop --for 1.5
play rate44 "$S/rate44.ogg" "$WORK/rate44.s16" 2 level --for 1.5
play unlinked "$S/chirp.ogg" "$WORK/chirp.s16" 2 unlinked --for 1 --wait 1.5
k=0
for song in "${SONGS[@]}"; do
    k=$((k + 1))
    # loop across its end
    len="$(python3 -c 'import os, sys; print(os.path.getsize(sys.argv[1]) / 4 / 48000)' "$WORK/song$k.s16")"
    play "song$k" "$song" "$WORK/song$k.s16" 2 exact --loop --from "$(python3 -c "print(max(0, $len - 1.5))")" --for 3
done

blender -b --factory-startup --python-exit-code 1 -P "$HERE/sound_check.py" -- "$CASES" > "$WORK/compare.log" 2>&1
grep -E '^(ok|FAIL|note) ' "$WORK/compare.log"
grep -q '^done$' "$WORK/compare.log" || { tail -n 20 "$WORK/compare.log"; fail "the comparisons didn't finish (see $WORK/compare.log)"; }
FAILS=$((FAILS + $(grep -c '^FAIL ' "$WORK/compare.log")))
echo "$FAILS failed; recordings and logs in $WORK"
((FAILS == 0))
