#!/usr/bin/env bash
# lipsync_check.sh: lip sync (src/lipsync.cpp) on sung vowels that tools/test/synth/vowels.py makes: a man's and a
# woman's a, i, u, e, o must each open the mouth with their own viseme the most (aa, ih, ou, ee, oh), and silence,
# hiss and a whisper-quiet voice must leave it shut. Through the harness, as the plugin feeds it the microphone.
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
run() { "$SHOT" --size 64x64 --avatar "$AV" --audio "$W/$1.wav" --frames 30 --visemes 2>&1 | grep '^visemes'; }
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
for f in silence hiss quiet_a; do
    line="$(run "$f")"
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
