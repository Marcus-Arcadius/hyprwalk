#!/usr/bin/env bash
# lipsync_check.sh: lip sync (src/lipsync.cpp) on sung vowels that tools/test/synth/vowels.py makes: a man's and a
# woman's a, i, u, e, o must each open the mouth with their own viseme the most (aa, ih, ou, ee, oh), and silence,
# hiss and hiss that follows a vowel must leave it shut; the consonants between two a's (s, sh, f, m) must each show
# their own (ss, ch, ff, pp) and no other. Through the harness, as the plugin feeds it the microphone.
# Then all of it as quieter microphones give it (tools/test/synth/attenuate.py: --levels dB down, after a second of
# silence, as lip sync hears the room before you speak): the automatic gain must open each vowel as wide as it opens
# at its own level (0.05 less at most), and silence, hiss and the hiss after a vowel stay shut. And a whisper, 40 dB
# down: right after a normal voice it stays shut (the gain goes by the voice of the last 15 s); on its own, or 16 s
# after the normal voice, it opens once the gain has heard it (a syllable's worth).
#   tools/test/harness/lipsync_check.sh AVATAR.glb [WORKDIR] [--real DIR [PERCENT]] [--levels "0 20 30 40"]
# Any avatar will do (it needs no mouth for the numbers). Needs build/test/shot (tools/test/harness/build.sh).
# --real DIR: recordings of real voices too, WAVs named for their vowel (a_*.wav ... o_*.wav; kept out of the repo:
# see the README). For each, how often its viseme is the mouth's biggest (after the first 0.3 s), how open it is on
# average, and how much of it lip sync heard as voiced; at least PERCENT (85) of the files must lead with their own,
# at their own level and at each of --levels, where each must open as wide as at its own level (0.05 less at most).
set -uo pipefail
REAL="" REAL_MIN=85 LEVELS="0 20 30 40" ARGS=()
while (($#)); do
    case "$1" in
        --real) REAL="$2"; shift 2; [[ "${1:-}" =~ ^[0-9]+$ ]] && { REAL_MIN="$1"; shift; } ;;
        --levels) LEVELS="$2"; shift 2 ;;
        *) ARGS+=("$1"); shift ;;
    esac
done
AV="${ARGS[0]:?usage: lipsync_check.sh AVATAR.glb [WORKDIR] [--real DIR [PERCENT]] [--levels \"0 20 30 40\"]}"
REPO="$(cd "$(dirname "$0")/../../.." && pwd)"
SHOT="$REPO/build/test/shot"
ATT=(python3 "$REPO/tools/test/synth/attenuate.py")
[[ -x "$SHOT" ]] || { echo "no $SHOT: tools/test/harness/build.sh builds it" >&2; exit 1; }
W="${ARGS[1]:-$(mktemp -d)}"
python3 "$REPO/tools/test/synth/vowels.py" "$W" > /dev/null || exit 1
mkdir -p "$W/lvl"
FAILS=0
ok() { echo "ok   $*"; }
fail() { echo "FAIL $*"; FAILS=$((FAILS + 1)); }
# the visemes after that many frames (60 a second) of a file
run() { "$SHOT" --size 64x64 --avatar "$AV" --audio "$1" --frames "${2:-30}" --visemes 2>&1 | grep '^visemes'; }
# the viseme with the most: visemes aa X ih X ou X ee X oh X, ...
best() { awk '{m = -1; for (i = 2; i <= 10; i += 2) { x = $(i + 1) + 0; if (x > m) { m = x; n = $i } } print n, m}' <<< "$1"; }
# the most of any of the nine
most() { awk '{m = 0; for (i = 3; i <= 19; i += 2) if ($i + 0 > m) m = $i + 0; print m}' <<< "$1"; }
# the most each consonant viseme shows along a file (the trace's last nine numbers are the mouth's visemes: aa ih ou
# ee oh pp ff ss ch)
consonants() {
    "$SHOT" --size 64x64 --avatar "$AV" --audio "$1" --lipsync-trace 2>&1 | awk '
        /^window/ { for (i = 6; i <= 9; i++) { x = $(NF - 9 + i) + 0; if (x > m[i]) m[i] = x } }
        END { printf "pp %.2f ff %.2f ss %.2f ch %.2f", m[6], m[7], m[8], m[9] }'
}
names=(aa ih ou ee oh)
declare -A OPEN0
for who in man woman; do
    k=0
    for v in a i u e o; do
        line="$(run "$W/${who}_$v.wav")"
        b="$(best "$line")"
        OPEN0[${who}_$v]="${b#* }"
        if [[ "${b%% *}" == "${names[$k]}" && "$(awk '{print ($2 > 0.5)}' <<< "$b")" == 1 ]]; then
            ok "${who}'s $v: ${b}   (${line#*, })"
        else
            fail "${who}'s $v: ${b}, not ${names[$k]}   ($line)"
        fi
        k=$((k + 1))
    done
done
# and hiss after a vowel (as a microphone hears it, one after the other): the vowel's shape mustn't stay
"${ATT[@]}" "$W/o_then_hiss.wav" 0 "$W/man_o.wav" "$W/hiss.wav"
# (quiet_a with nothing before it: no pause heard, so nothing to set the gain by)
for f in silence hiss quiet_a o_then_hiss; do
    line="$(run "$W/$f.wav" $([[ $f == o_then_hiss ]] && echo 90))" # (0.8 s of the o, then 0.7 of hiss)
    m="$(most "$line")"
    if awk -v m="$m" 'BEGIN {exit !(m < 0.05)}'; then ok "$f: shut ($m)"; else fail "$f: open ($line)"; fi
done
# the consonants, a man's and a woman's
check_consonant() { # file, what it is, the viseme it must show
    local line got other
    line="$(consonants "$1")"
    got="$(awk -v w="$3" '{for (i = 1; i <= 8; i += 2) if ($i == w) print $(i + 1)}' <<< "$line")"
    other="$(awk -v w="$3" '{m = 0; for (i = 1; i <= 8; i += 2) if ($i != w && $(i + 1) + 0 > m) m = $(i + 1) + 0; print m}' <<< "$line")"
    if awk -v g="$got" -v o="$other" 'BEGIN {exit !(g > 0.3 && o < 0.15)}'; then ok "$2: $3 ($line)"; else fail "$2: not $3 alone ($line)"; fi
}
for who in man woman; do
    for pair in asa:ss asha:ch afa:ff ama:pp; do
        f="${pair%%:*}"
        check_consonant "$W/${who}_$f.wav" "${who}'s a-${f:1:${#f}-2}-a" "${pair##*:}"
    done
done
# and on the avatar: its own s viseme (VRChat's vrc.v_ss) in place of the vowel's shape, halfway through the s
out="$("$SHOT" --size 64x64 --avatar "$AV" --audio "$W/man_asa.wav" --frames 38 --morphs 2>&1)"
if grep -q "lip sync's consonants:.* ss" <<< "$out"; then
    # (VRChat's own names are vrc.v_SS, vrc.v_aa; some avatars have them in lower case)
    ss="$(grep -io 'vrc.v_ss=[0-9.]*' <<< "$out" | cut -d= -f2)" aa="$(grep -io 'vrc.v_aa=[0-9.]*' <<< "$out" | cut -d= -f2)"
    if awk -v s="${ss:-0}" -v a="${aa:-0}" 'BEGIN {exit !(s > 0.3 && s > a)}'; then
        ok "the avatar's s shape through the s: vrc.v_ss $ss over vrc.v_aa ${aa:-0}"
    else
        fail "the avatar's s shape through the s: vrc.v_ss ${ss:-0}, vrc.v_aa ${aa:-0}"
    fi
else
    echo "skipped: $(basename "$AV") has no s viseme (vrc.v_ss)"
fi

# --- quieter microphones: each file after a second of silence, that many dB down
for L in $LEVELS; do
    for f in man_a man_i man_u man_e man_o woman_a woman_i woman_u woman_e woman_o silence hiss man_asa man_asha man_afa man_ama woman_asa woman_asha woman_afa woman_ama; do
        "${ATT[@]}" "$W/lvl/$f@$L.wav" "$L" silence:1 "$W/$f.wav" &
    done
    "${ATT[@]}" "$W/lvl/o_then_hiss@$L.wav" "$L" silence:1 "$W/man_o.wav" "$W/hiss.wav" &
    wait
    k=0 worst=9 fails0=$FAILS
    for who in man woman; do
        k=0
        for v in a i u e o; do
            line="$(run "$W/lvl/${who}_$v@$L.wav" 90)" # (the second of silence, then 0.5 s of the vowel)
            b="$(best "$line")"
            if [[ "${b%% *}" == "${names[$k]}" ]] && awk -v x="${b#* }" -v o="${OPEN0[${who}_$v]}" 'BEGIN {exit !(x > 0.5 && x >= o - 0.05)}'; then
                worst="$(awk -v x="${b#* }" -v o="${OPEN0[${who}_$v]}" -v w="$worst" 'BEGIN {print (x - o < w ? x - o : w)}')"
            else
                fail "$L dB down, ${who}'s $v: ${b}, not ${names[$k]} as wide as at its own level (${OPEN0[${who}_$v]})   ($line)"
            fi
            k=$((k + 1))
        done
    done
    ((FAILS == fails0)) && ok "$L dB down: the ten vowels lead with their own, as wide as at their own level (the least by $worst)"
    for f in silence hiss o_then_hiss; do
        line="$(run "$W/lvl/$f@$L.wav" $([[ $f == o_then_hiss ]] && echo 150 || echo 108))"
        m="$(most "$line")"
        if awk -v m="$m" 'BEGIN {exit !(m < 0.05)}'; then ok "$L dB down, $f: shut ($m)"; else fail "$L dB down, $f: open ($line)"; fi
    done
    for who in man woman; do
        for pair in asa:ss asha:ch afa:ff ama:pp; do
            f="${pair%%:*}"
            check_consonant "$W/lvl/${who}_$f@$L.wav" "$L dB down, ${who}'s a-${f:1:${#f}-2}-a" "${pair##*:}"
        done
    done
done

# --- a whisper (a man's a 40 dB down), after a second of silence
"${ATT[@]}" "$W/lvl/whisper-after-voice.wav" 0 silence:1 "$W/man_a.wav" "$W/quiet_a.wav" "$W/quiet_a.wav" &
"${ATT[@]}" "$W/lvl/whisper-alone.wav" 0 silence:1 "$W/quiet_a.wav" "$W/quiet_a.wav" &
"${ATT[@]}" "$W/lvl/whisper-16s-later.wav" 0 silence:1 "$W/man_a.wav" silence:16 "$W/quiet_a.wav" "$W/quiet_a.wav" &
wait
line="$(run "$W/lvl/whisper-after-voice.wav" 168)" # (1 s, 0.8 of the a, 1 s into the whisper)
m="$(most "$line")"
if awk -v m="$m" 'BEGIN {exit !(m < 0.05)}'; then ok "a whisper right after a normal voice: shut ($m)"; else fail "a whisper right after a normal voice: open ($line)"; fi
for f in alone:120 16s-later:1128; do # (1 s into the whisper)
    line="$(run "$W/lvl/whisper-${f%%:*}.wav" "${f##*:}")"
    b="$(best "$line")"
    if [[ "${b%% *}" == aa ]] && awk -v x="${b#* }" 'BEGIN {exit !(x > 0.5)}'; then
        ok "a whisper ${f%%:*}: opens, the gain made up for it ($b; ${line##*, })"
    else
        fail "a whisper ${f%%:*}: $b, not aa wide open ($line)"
    fi
done

# --- real voices
if [[ -n "$REAL" ]]; then
    measure() { # a file, the second of silence before it (0 or 1), its vowel: how often its own viseme leads after 0.3 s
        # of it, how open it is then on average, how much of it was voiced
        local want
        want="$(echo aiueo | awk -v v="$3" '{print index($0, v)}')"
        # the trace's last nine numbers are the mouth's visemes (aa ih ou ee oh ...), $2 the window's time, $12 voicing
        "$SHOT" --size 64x64 --avatar "$AV" --audio "$1" --lipsync-trace 2>&1 | awk -v want="$want" -v before="$2" '
            /^window/ { t = $2 - before; if (t < 0) next; voiced += ($12 == "voiced"); n++
                if (t > 0.3) { m = 0; k = 0; for (i = 1; i <= 5; i++) { x = $(NF - 9 + i) + 0; if (x > m) { m = x; k = i } }
                               late++; open += $(NF - 9 + want); if (k == want && m > 0.05) led++ } }
            END { printf "%.0f %.2f %.0f", late ? 100 * led / late : 0, late ? open / late : 0, n ? 100 * voiced / n : 0 }'
    }
    files=()
    for f in "$REAL"/[aiueo]_*.wav; do [[ -f "$f" ]] && files+=("$f"); done
    total=${#files[@]} led=0
    declare -A PCT0 RO0
    for f in "${files[@]}"; do
        n="$(basename "$f")"; v="${n:0:1}"
        read -r pct opn vcd <<< "$(measure "$f" 0 "$v")"
        want="$(echo aiueo | awk -v v="$v" '{print index($0, v) - 1}')"
        PCT0[$n]=$pct RO0[$n]=$opn
        if ((pct >= 50)); then led=$((led + 1)); echo "ok   $n: ${names[$want]} leads ${pct}% of the time, opens $opn (voiced $vcd%)"
        else echo "MISS $n: ${names[$want]} leads only ${pct}% of the time, opens $opn (voiced $vcd%)"; fi
    done
    if ((total == 0)); then
        fail "no a_*.wav ... o_*.wav in $REAL"
    elif ((led * 100 < total * REAL_MIN)); then
        fail "real voices: $led of $total lead with their vowel, under $REAL_MIN%"
    else
        ok "real voices: $led of $total lead with their vowel"
    fi
    for L in $LEVELS; do
        for f in "${files[@]}"; do "${ATT[@]}" "$W/lvl/real-$(basename "$f" .wav)@$L.wav" "$L" silence:1 "$f" & done
        wait
        for f in "${files[@]}"; do
            n="$(basename "$f")"
            (measure "$W/lvl/real-${n%.wav}@$L.wav" 1 "${n:0:1}" > "$W/lvl/real-${n%.wav}@$L.txt") &
        done
        wait
        led=0 narrower="" sum=0 sum0=0
        for f in "${files[@]}"; do
            n="$(basename "$f")"
            read -r pct opn vcd < "$W/lvl/real-${n%.wav}@$L.txt"
            ((pct >= 50)) && led=$((led + 1))
            ((PCT0[$n] >= 50 && pct < 50)) && narrower+=" $n (no longer leads: $pct%)"
            awk -v x="$opn" -v o="${RO0[$n]}" 'BEGIN {exit !(x < o - 0.05)}' && narrower+=" $n (opens $opn, ${RO0[$n]} at its own level)"
            sum="$(awk -v a="$sum" -v b="$opn" 'BEGIN {print a + b}')" sum0="$(awk -v a="$sum0" -v b="${RO0[$n]}" 'BEGIN {print a + b}')"
        done
        mean="$(awk -v s="$sum" -v n="$total" 'BEGIN {printf "%.2f", s / n}')" mean0="$(awk -v s="$sum0" -v n="$total" 'BEGIN {printf "%.2f", s / n}')"
        if ((led * 100 >= total * REAL_MIN)) && [[ -z "$narrower" ]]; then
            ok "real voices $L dB down: $led of $total lead with their vowel, each as wide as at its own level (on average $mean, $mean0 there)"
        else
            fail "real voices $L dB down: $led of $total lead;$narrower"
        fi
    done
fi
((FAILS)) && { echo "$FAILS FAILED"; exit 1; }
echo "all passed"
