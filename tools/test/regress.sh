#!/usr/bin/env bash
# regress.sh: converts the test avatars with the working copy's tools/unity2hypr3d.py and with
# another version of it (HEAD's by default), and compares what the two write.
#
#   tools/test/regress.sh [--base REV|FILE] [--robot PATH] [--proj DIR] [--out DIR] [--keep] [CASE...]
#
#   --base REV|FILE  the converter to compare against: a git revision (default HEAD) or a file
#   --robot PATH     also convert the VRChat SDK's robot sample, "Avatar Dynamics Robot Avatar
#                    PC.unity" in the SDK's Samples/Dynamics/Robot Avatar (or set HYPR3D_ROBOT).
#                    It is VRChat's, so it isn't in this repo; it's in com.vrchat.avatars-*.zip
#                    from https://github.com/vrchat/packages/releases
#   --proj DIR       the synthetic Unity project to use; synth/make.py makes it there if it's missing
#   --out DIR        where everything goes (default: a new temporary directory, removed when
#                    nothing differs)
#   --keep           keep the outputs even when nothing differs
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
BASE=HEAD ROBOT="${HYPR3D_ROBOT:-}" PROJ="" OUT="" KEEP=0 ONLY=()
while (($#)); do
    case "$1" in
        --base) BASE="$2"; shift 2 ;;
        --robot) ROBOT="$2"; shift 2 ;;
        --proj) PROJ="$2"; shift 2 ;;
        --out) OUT="$2"; shift 2 ;;
        --keep) KEEP=1; shift ;;
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

# cases: name, kind (strict = no MA or VRCFury: must stay byte-identical), converter arguments
CASES=()
add() { CASES+=("$(printf '%s\x1f' "$@")"); }
add SynthAvatar strict "$A/SynthAvatar.prefab"
add SynthVariant strict "$A/SynthVariant.prefab"
add SynthMA MA "$A/SynthMA.prefab"
add SynthOutfit MA "$A/SynthAvatar.prefab" --outfit Outfit
add SynthPlusMA MA "$A/SynthAvatar.prefab" --outfit "$A/OutfitMA.prefab"
add SynthVRM MA "$A/SynthAvatar.prefab" --outfit OutfitVRM
add VariantTwo MA "$A/SynthVariant.prefab" --outfit OutfitVRM --outfit Outfit
if [[ -n "$ROBOT" ]]; then
    [[ -f "$ROBOT" ]] || die "no robot sample at $ROBOT"
    add robot strict "$ROBOT"
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

if ((DIFF || KEEP)) || [[ -n "$OUT" ]]; then
    echo "outputs: $W (base/ and new/)"
else
    rm -rf "$W"
fi
exit $BAD
