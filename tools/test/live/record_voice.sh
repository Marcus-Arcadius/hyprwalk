#!/usr/bin/env bash
# record_voice.sh [DIR]: records your voice for tuning lip sync: the vowels it knows held (ah, ee, oo, eh, oh: Japanese
# a, i, u, e, o), then "sss", then silence, 4 s each, from the default microphone (pw-record, 32-bit float mono WAVs at
# 48 kHz) into DIR (default ~/h3d-live/voice). Nothing is sent anywhere. tools/test/harness/lipsync_check.sh --real DIR
# or the harness's --audio FILE --lipsync-trace then goes through them offscreen.
set -uo pipefail
DIR="${1:-$HOME/h3d-live/voice}"
command -v pw-record > /dev/null || { echo "no pw-record (PipeWire's tools)" >&2; exit 1; }
mkdir -p "$DIR" || exit 1
{
    echo "recorded $(date '+%F %T') from:"
    wpctl inspect @DEFAULT_AUDIO_SOURCE@ 2>&1 | grep -E 'node.name|node.description|node.nick|audio.channels|audio.format' | sed 's/^/  /'
    wpctl get-volume @DEFAULT_AUDIO_SOURCE@ 2>&1 | sed 's/^/  /'
} > "$DIR/source.txt"
cat "$DIR/source.txt"
echo
echo "Each recording is 4 seconds. Start at \"now\" and hold the sound the whole time, as loud as you talk."
record() { # file, what to do
    printf '\n>>> %s\n' "$2"
    for n in 3 2 1; do printf '%s... ' "$n"; sleep 1; done
    printf 'now\n'
    timeout -s INT 4 pw-record --rate 48000 --channels 1 --format f32 "$DIR/$1" 2> /dev/null
    sleep 0.3
}
record a_me.wav 'hold "ahhh", as in "father"'
record i_me.wav 'hold "eeee", as in "see"'
record u_me.wav 'hold "oooo", as in "food"'
record e_me.wav 'hold "ehhh", as in "bed"'
record o_me.wav 'hold "ohhh", as in "go"'
record s_me.wav 'hold "sssss"'
record "silence_me.wav" "stay quiet (the room as it is)"
echo
echo "levels (a full scale sine is 0 dBFS; exact zeros mean the microphone sent nothing, muted?):"
python3 - "$DIR" << 'EOF'
import math, os, struct, sys
d = sys.argv[1]
for f in ['a_me.wav', 'i_me.wav', 'u_me.wav', 'e_me.wav', 'o_me.wav', 's_me.wav', 'silence_me.wav']:
    p = os.path.join(d, f)
    try:
        data = open(p, 'rb').read()
    except OSError:
        print(f'  {f}: missing')
        continue
    at = data.find(b'data')
    xs = struct.unpack('<%df' % ((len(data) - at - 8) // 4), data[at + 8:at + 8 + (len(data) - at - 8) // 4 * 4]) if at >= 0 else ()
    if not xs:
        print(f'  {f}: empty')
        continue
    peak = max(abs(x) for x in xs)
    rms = math.sqrt(sum(x * x for x in xs) / len(xs))
    zeros = sum(1 for x in xs if x == 0.0) / len(xs)
    db = lambda v: f'{20 * math.log10(v):.1f}' if v > 0 else '-inf'
    print(f'  {f:16s} {len(xs) / 48000:4.1f} s  peak {db(peak):>6} dBFS  RMS {db(rms * math.sqrt(2)) if rms else "-inf":>6} dBFS  exact zeros {zeros:.0%}')
EOF
echo
echo "in $DIR"
