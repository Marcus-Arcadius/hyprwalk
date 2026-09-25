#!/usr/bin/env bash
# lipsync_check.sh: lip sync (src/lipsync.cpp) on sung vowels that tools/test/synth/vowels.py makes: a man's and a
# woman's a, i, u, e, o must each open the mouth with their own viseme the most (aa, ih, ou, ee, oh), and silence,
# hiss, a whisper-quiet voice and hiss that follows a vowel must leave it shut. Through the harness, as the plugin
# feeds it the microphone.
#   tools/test/harness/lipsync_check.sh AVATAR.glb [WORKDIR]
# Any avatar will do (it needs no mouth for the numbers). Needs build/test/shot (tools/test/harness/build.sh).
set -uo pipefail
AV="${1:?usage: lipsync_check.sh AVATAR.glb [WORKDIR]}"
REPO="$(cd "$(dirname "$0")/../../.." && pwd)"
SHOT="$REPO/build/test/shot"
[[ -x "$SHOT" ]] || { echo "no $SHOT: tools/test/harness/build.sh builds it" >&2; exit 1; }
W="${2:-$(mktemp -d)}"
python3 "$REPO/tools/test/synth/vowels.py" "$W" > /dev/null || exit 1
FAILS=0
run() { "$SHOT" --size 64x64 --avatar "$AV" --audio "$W/$1.wav" --frames "${2:-30}" --visemes 2>&1 | grep '^visemes'; }
names=(aa ih ou ee oh)
for who in man woman; do
    k=0
    for v in a i u e o; do
        line="$(run "${who}_$v")"
        # the viseme with the most: visemes aa X ih X ou X ee X oh X, ...
        best="$(awk '{m = -1; for (i = 2; i <= 10; i += 2) { x = $(i + 1) + 0; if (x > m) { m = x; n = $i } } print n, m}' <<< "$line")"
        if [[ "${best%% *}" == "${names[$k]}" && "$(awk '{print ($2 > 0.5)}' <<< "$best")" == 1 ]]; then
            echo "ok   ${who}'s $v: ${best}   (${line#*, })"
        else
            echo "FAIL ${who}'s $v: ${best}, not ${names[$k]}   ($line)"
            FAILS=$((FAILS + 1))
        fi
        k=$((k + 1))
    done
done
# and hiss after a vowel (as a microphone hears it, one after the other): the vowel's shape mustn't stay
python3 - "$W" << 'EOF'
import sys, wave
d = sys.argv[1]
frames, params = b'', None
for f in ('man_o', 'hiss'):
    with wave.open(f'{d}/{f}.wav') as w:
        params = params or w.getparams()
        frames += w.readframes(w.getnframes())
with wave.open(f'{d}/o_then_hiss.wav', 'wb') as w:
    w.setparams(params)
    w.writeframes(frames)
EOF
for f in silence hiss quiet_a o_then_hiss; do
    line="$(run "$f" $([[ $f == o_then_hiss ]] && echo 90))" # (0.8 s of the o, then 0.7 of hiss)
    most="$(awk '{m = 0; for (i = 3; i <= 11; i += 2) if ($i + 0 > m) m = $i + 0; print m}' <<< "$line")"
    if [[ "$(awk -v m="$most" 'BEGIN {print (m < 0.05)}')" == 1 ]]; then
        echo "ok   $f: shut ($most)"
    else
        echo "FAIL $f: open ($line)"
        FAILS=$((FAILS + 1))
    fi
done
((FAILS)) && { echo "$FAILS FAILED"; exit 1; }
echo "all passed"
