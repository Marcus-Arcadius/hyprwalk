#!/usr/bin/env bash
# regress.sh: converts the test avatars with the working copy's tools/unity2hypr3d.py and with
# another version of it (HEAD's by default), and compares what the two write.
#
#   tools/test/regress.sh [--base REV|FILE] [--robot PATH] [--items DIR] [--proj DIR] [--booth DIR]
#                         [--out DIR] [--keep] [--shots] [CASE...]
#
#   --base REV|FILE  the converter to compare against: a git revision (default HEAD) or a file
#   --robot PATH     also convert the VRChat SDK's robot sample, "Avatar Dynamics Robot Avatar
#                    PC.unity" in the SDK's Samples/Dynamics/Robot Avatar (or set HYPR3D_ROBOT).
#                    It is VRChat's, so it isn't in this repo; it's in com.vrchat.avatars-*.zip
#                    from https://github.com/vrchat/packages/releases
#   --items DIR      also convert free Booth items, as downloaded into DIR (or set
#                    HYPR3D_ITEMS): 止丸式初音ミクNT_ver1.1.2.zip (booth.pm/items/3226395) alone
#                    (MikuNT) and with VRSuya's dances set up with MA,
#                    VRSuya_Doodle_Dance_Released_260709.zip (booth.pm/items/6249275),
#                    VRSuya_Loli_Kami_Requiem_Released_260709.zip (booth.pm/items/5157852),
#                    VRSuya_INTERNET_YAMERO_Released_260709.zip and
#                    VRSuya_Reino_Dance_Released_260709.zip, as emotes (MikuDances), and the
#                    dances on SynthChan (SynthDances); pHMToothlessDance.zip, bare clips, with
#                    --emote (MikuClips, SynthClips). They are their makers' under their terms, so
#                    they aren't in this repo; a missing one leaves out its cases
#   --proj DIR       the synthetic Unity project to use; synth/make.py makes it there if it's missing
#   --booth DIR      the Booth-style test packages to use; synth/booth.py makes them there if missing
#   --out DIR        where everything goes (default: a new temporary directory, removed when
#                    nothing differs)
#   --keep           keep the outputs even when nothing differs
#   --shots          render each new GLB (front, side, and walking with physics) into OUT/shots with
#                    build/test/shot (tools/test/harness/build.sh builds it); implies --keep
#   CASE...          only these cases (see "cases" below)
#
# Each case is converted twice, and check.py's report, the settings file (less its date) and the
# GLB's bytes are compared. Avatars with no Modular Avatar or VRCFury setup (the "strict" cases)
# must come out byte for byte the same; the others are reported, with skincmp.py's comparison of
# the skinned vertices when their GLBs differ.
# Exit status: 0 when every strict case is the same and nothing failed, 1 otherwise.
set -uo pipefail

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
SYN="$REPO/tools/test/synth"
BLENDER=(blender -b --factory-startup --python-exit-code 1)
BASE=HEAD ROBOT="${HYPR3D_ROBOT:-}" ITEMS="${HYPR3D_ITEMS:-}" PROJ="" BOOTH="" OUT="" KEEP=0 SHOTS=0 ONLY=()
while (($#)); do
    case "$1" in
        --base) BASE="$2"; shift 2 ;;
        --robot) ROBOT="$2"; shift 2 ;;
        --items) ITEMS="$2"; shift 2 ;;
        --proj) PROJ="$2"; shift 2 ;;
        --booth) BOOTH="$2"; shift 2 ;;
        --out) OUT="$2"; shift 2 ;;
        --keep) KEEP=1; shift ;;
        --shots) SHOTS=1; KEEP=1; shift ;;
        -h|--help) sed -n '2,/^set /p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
        -*) echo "regress.sh: unknown option $1" >&2; exit 2 ;;
        *) ONLY+=("$1"); shift ;;
    esac
done
die() { echo "regress.sh: $*" >&2; exit 1; }

if [[ -n "$OUT" ]]; then mkdir -p "$OUT" && W="$(cd "$OUT" && pwd)" || die "can't make $OUT"
else W="$(mktemp -d "${TMPDIR:-/tmp}/hypr3d-regress.XXXXXX")"; fi
mkdir -p "$W/base" "$W/new"

# the two converters
if [[ -f "$BASE" ]]; then
    cp "$BASE" "$W/base/unity2hypr3d.py"
else
    git -C "$REPO" show "$BASE:tools/unity2hypr3d.py" > "$W/base/unity2hypr3d.py" 2> "$W/base/git.log" ||
        die "no tools/unity2hypr3d.py at $BASE: $(cat "$W/base/git.log")"
fi
cp "$REPO/tools/unity2hypr3d.py" "$W/new/unity2hypr3d.py"
if cmp -s "$W/base/unity2hypr3d.py" "$W/new/unity2hypr3d.py"; then
    echo "note: the converter is the same as $BASE's"
fi

# the synthetic project
PROJ="${PROJ:-$W/proj}"
if [[ ! -d "$PROJ/Assets" ]]; then
    echo "making the synthetic project in $PROJ"
    "${BLENDER[@]}" -P "$SYN/make.py" -- "$PROJ" > "$W/make.log" 2>&1 || { tail -n 30 "$W/make.log"; die "make.py failed"; }
fi
PROJ="$(cd "$PROJ" && pwd)"
A="$PROJ/Assets/Synth"

# the Booth-style packages
BOOTH="${BOOTH:-$W/booth}"
if [[ ! -f "$BOOTH/SynthChan_Gimmicks_VRCFury_v1.0.unitypackage" ]]; then
    echo "making the Booth-style packages in $BOOTH"
    mkdir -p "$BOOTH"
    "${BLENDER[@]}" -P "$SYN/booth.py" -- "$BOOTH" > "$W/booth.log" 2>&1 || { tail -n 30 "$W/booth.log"; die "booth.py failed"; }
fi
B="$(cd "$BOOTH" && pwd)"

# cases: name, kind (strict = no MA or VRCFury: must stay byte-identical; MA; VRCF; emote = --emote's clips), converter
# arguments
CASES=()
add() { CASES+=("$(printf '%s\x1f' "$@")"); }
add SynthAvatar strict "$A/SynthAvatar.prefab"
add SynthVariant strict "$A/SynthVariant.prefab"
add SynthMA MA "$A/SynthMA.prefab"
add SynthOutfit MA "$A/SynthAvatar.prefab" --outfit Outfit
add SynthPlusMA MA "$A/SynthAvatar.prefab" --outfit "$A/OutfitMA.prefab"
add SynthVRM MA "$A/SynthAvatar.prefab" --outfit OutfitVRM
add VariantTwo MA "$A/SynthVariant.prefab" --outfit OutfitVRM --outfit Outfit
add SynthVF VRCF "$A/SynthVF.prefab"
add SynthPlusVF VRCF "$A/SynthAvatar.prefab" --outfit "$A/OutfitVF.prefab"
add BoothChan strict "$B/SynthChan_v1.0.unitypackage"
add BoothZip strict "$B/シンセちゃん_v1.0.zip"
add BoothDress MA "$B/SynthChan_v1.0.unitypackage" --outfit "$B/SynthChan_OnePiece_v1.0.unitypackage"
add BoothParka MA "$B/SynthChan_v1.0.unitypackage" --outfit "$B/Parka_v1.0.unitypackage"
add BoothBoth MA "$B/SynthChan_v1.0.unitypackage" --outfit "$B/SynthChan_OnePiece_v1.0.unitypackage" \
    --outfit "$B/Parka_v1.0.unitypackage"
add BoothCardigan VRCF "$B/SynthChan_v1.0.unitypackage" --outfit "$B/SynthChan_Cardigan_VRCFury_v1.0.unitypackage"
add BoothHairpin VRCF "$B/SynthChan_v1.0.unitypackage" --outfit "$B/Hairpin_v1.0.unitypackage"
add BoothMix VRCF "$B/SynthChan_v1.0.unitypackage" --outfit "$B/SynthChan_OnePiece_v1.0.unitypackage" \
    --outfit "$B/SynthChan_Cardigan_VRCFury_v1.0.unitypackage" --outfit "$B/Hairpin_v1.0.unitypackage"
add BoothAccessories MA "$B/SynthChan_v1.0.unitypackage" --outfit "$B/SynthChan_Accessories_MA_v1.0.unitypackage"
add BoothGimmicks VRCF "$B/SynthChan_v1.0.unitypackage" --outfit "$B/SynthChan_Gimmicks_VRCFury_v1.0.unitypackage"
# pairs of cases whose GLBs must be the same (the zip holds the package)
SAME=("BoothZip BoothChan")
if [[ -n "$ROBOT" ]]; then
    [[ -f "$ROBOT" ]] || die "no robot sample at $ROBOT"
    add robot strict "$ROBOT"
fi
if [[ -n "$ITEMS" ]]; then
    [[ -d "$ITEMS" ]] || die "no folder $ITEMS"
    I="$(cd "$ITEMS" && pwd)"
    MIKU="$I/止丸式初音ミクNT_ver1.1.2.zip"
    DANCES=() CLIPS=()
    for d in VRSuya_Doodle_Dance_Released_260709.zip VRSuya_Loli_Kami_Requiem_Released_260709.zip \
        VRSuya_INTERNET_YAMERO_Released_260709.zip VRSuya_Reino_Dance_Released_260709.zip; do
        if [[ -f "$I/$d" ]]; then DANCES+=(--outfit "$I/$d"); else echo "note: no $d in $I"; fi
    done
    for d in pHMToothlessDance.zip; do
        if [[ -f "$I/$d" ]]; then CLIPS+=(--emote "$I/$d"); else echo "note: no $d in $I"; fi
    done
    if [[ -f "$MIKU" ]]; then
        add MikuNT strict "$MIKU"
        ((${#DANCES[@]})) && add MikuDances MA "$MIKU" "${DANCES[@]}"
        ((${#CLIPS[@]})) && add MikuClips emote "$MIKU" "${CLIPS[@]}"
        SAME+=("MikuClips MikuNT")  # (clips add emote files, never change the GLB)
    else
        echo "note: no ${MIKU##*/} in $I"
    fi
    ((${#DANCES[@]})) && add SynthDances MA "$B/SynthChan_v1.0.unitypackage" "${DANCES[@]}"
    ((${#CLIPS[@]})) && add SynthClips emote "$B/SynthChan_v1.0.unitypackage" "${CLIPS[@]}"
fi

picked() {
    ((${#ONLY[@]} == 0)) && return 0
    local o
    for o in "${ONLY[@]}"; do [[ "$o" == "$1" ]] && return 0; done
    return 1
}

# convert, both sides at once
MAXJ=$(( $(nproc) > 16 ? 16 : $(nproc) ))
throttle() { while (($(jobs -rp | wc -l) >= MAXJ)); do wait -n; done; }
NAMES=()
for c in "${CASES[@]}"; do
    IFS=$'\x1f' read -r -a f <<< "$c"
    picked "${f[0]}" || continue
    NAMES+=("${f[0]}")
    for side in base new; do
        throttle
        (
            cd "$W/$side" &&
                python3 "$W/$side/unity2hypr3d.py" "${f[@]:2}" -o "$W/$side/${f[0]}.glb" > "$W/$side/${f[0]}.log" 2>&1
            echo $? > "$W/$side/${f[0]}.status"
        ) &
    done
done
wait
((${#NAMES[@]})) || die "no such case: ${ONLY[*]}"

# check.py's report on each GLB
for n in "${NAMES[@]}"; do
    for side in base new; do
        [[ -f "$W/$side/$n.glb" ]] || continue
        throttle
        ("${BLENDER[@]}" -P "$SYN/check.py" -- "$W/$side/$n.glb" 2>&1 |
            grep -v -e '^Blender' -e '^Read' -e '^$' -e '^Warning: Unable' -e '^Color management' > "$W/$side/$n.check.txt") &
    done
done
wait

kind_of() {
    local c f
    for c in "${CASES[@]}"; do
        IFS=$'\x1f' read -r -a f <<< "$c"
        [[ "${f[0]}" == "$1" ]] && { echo "${f[1]}"; return; }
    done
}

BAD=0 DIFF=0
for n in "${NAMES[@]}"; do
    kind="$(kind_of "$n")"
    sb="$(cat "$W/base/$n.status" 2>/dev/null || echo ?)" sn="$(cat "$W/new/$n.status" 2>/dev/null || echo ?)"
    if [[ "$sn" != 0 ]]; then
        printf '%-14s %-6s FAILED (exit %s):\n' "$n" "$kind" "$sn"
        tail -n 15 "$W/new/$n.log" | sed 's/^/    /'
        BAD=1
        continue
    fi
    if [[ "$sb" != 0 ]]; then
        printf '%-14s %-6s the base converter failed (exit %s): %s\n' "$n" "$kind" "$sb" "$(grep -m1 error "$W/base/$n.log")"
        [[ "$kind" == strict ]] && BAD=1
        DIFF=1
        continue
    fi
    what=()
    diff -q "$W/base/$n.check.txt" "$W/new/$n.check.txt" > /dev/null || what+=("check.py report")
    python3 - "$W/base/$n.hypr3d.json" "$W/new/$n.hypr3d.json" << 'PY' || what+=("settings")
import json, sys
a, b = (json.load(open(f)) for f in sys.argv[1:3])
for d in (a, b):
    c = d.get('converter', {})
    c.pop('date', None)
    c['warnings'] = sorted(c.get('warnings', []))
sys.exit(a != b)
PY
    cmp -s "$W/base/$n.glb" "$W/new/$n.glb" || what+=("GLB bytes")
    # the emote files the converter writes next to the GLB
    for e in "$W"/base/"$n".*.vrma "$W"/new/"$n".*.vrma; do
        [[ -f "$e" ]] || continue
        cmp -s "$W/base/${e##*/}" "$W/new/${e##*/}" || { what+=("emote files"); break; }
    done
    if ((${#what[@]} == 0)); then
        printf '%-14s %-6s same\n' "$n" "$kind"
        continue
    fi
    DIFF=1
    [[ "$kind" == strict ]] && BAD=1
    printf '%-14s %-6s DIFFERS: %s\n' "$n" "$kind" "$(IFS=,; echo "${what[*]}" | sed 's/,/, /g')"
    diff "$W/base/$n.check.txt" "$W/new/$n.check.txt" | head -n 12 | sed 's/^/    /'
    python3 - "$W/base/$n.hypr3d.json" "$W/new/$n.hypr3d.json" << 'PY'
import json, sys
a, b = (json.load(open(f)) for f in sys.argv[1:3])
for d in (a, b):
    d.get('converter', {}).pop('date', None)
for k in sorted(set(a) | set(b)):
    if a.get(k) != b.get(k):
        print('    settings %s: %s -> %s' % (k, json.dumps(a.get(k), ensure_ascii=False)[:200],
                                            json.dumps(b.get(k), ensure_ascii=False)[:200]))
PY
    if [[ " ${what[*]} " == *"GLB bytes"* ]]; then
        "${BLENDER[@]}" -P "$SYN/skincmp.py" -- "$W/base/$n.glb" "$W/new/$n.glb" 2>&1 | grep -E 'moved|only in|vertices$' |
            grep -v ' 0 of ' | head -n 12 | sed 's/^/    skincmp: /'
    fi
done

for pair in "${SAME[@]}"; do
    read -r a b <<< "$pair"
    [[ -f "$W/new/$a.glb" && -f "$W/new/$b.glb" ]] || continue
    if cmp -s "$W/new/$a.glb" "$W/new/$b.glb"; then
        printf '%-14s %-6s same GLB as %s\n' "$a" "" "$b"
    else
        printf '%-14s %-6s DIFFERS from %s\n' "$a" "" "$b"
        BAD=1
    fi
done

if ((SHOTS)); then
    SHOT="$REPO/build/test/shot"
    [[ -x "$SHOT" ]] || die "no $SHOT: build it with tools/test/harness/build.sh"
    mkdir -p "$W/shots"
    for n in "${NAMES[@]}"; do
        [[ -f "$W/new/$n.glb" ]] || continue
        (cd "$W/shots" && "$SHOT" --size 640x800 --avatar "$W/new/$n.glb" --frames 30 --view 20 --out "$n-front.png" \
            --view 110 --out "$n-side.png" --physics 1 --accel 40 --move 0 3 --frames 45 --view 70 \
            --out "$n-walk.png" --swing > "$n.log" 2>&1) || echo "$n: shot failed, see $W/shots/$n.log"
    done
    echo "renders: $W/shots"
fi

if ((DIFF || KEEP)) || [[ -n "$OUT" ]]; then
    echo "outputs: $W (base/ and new/)"
else
    rm -rf "$W"
fi
exit $BAD
