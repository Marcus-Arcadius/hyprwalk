#!/usr/bin/env bash
# run.sh: hyprwalk.so in a real Hyprland in headless NixOS VMs (vm.nix), driven by QMP input events (so they go through
# libinput and Hyprland's input stack to the plugin's hooks), with PipeWire and a virtual microphone singing test
# vowels. checks.py is the checklist.
#
#   tools/test/vm/run.sh [--avatars DIR] [--booth DIR] [--only ITEMS] [--gpu virgl] OUTDIR
#
#   --avatars DIR  take BoothAccessories.glb (with its settings) and BoothGimmicks.hands.vrma from DIR, as
#                  regress.sh --keep leaves them in OUT/new; else convert them here (synth/booth.py, unity2hyprwalk.py)
#   --booth DIR    the Booth-style packages to convert (booth.py makes them there if missing)
#   --only ITEMS   only these checks.py sections, comma-separated ("0" starts Hyprland and loads the plugin)
#   --gpu virgl    draw on this machine's GPU through virglrenderer (QEMU egl-headless on HYPRWALK_RENDERNODE, default
#                  /dev/dri/renderD129) instead of llvmpipe in the VM
#
# Hyprland is the running one (as for build.sh) or HYPR_BIN's. OUTDIR gets results.txt, results.json, frames/, logs/,
# live/ and driver.log; OUTDIR/driver is a GC root. Exit status 0 when every check passed ("known" failures,
# Hyprland's own bugs, don't count).
set -euo pipefail
REPO="$(cd "$(dirname "$0")/../../.." && pwd)"
VM="$REPO/tools/test/vm"
AVATARS="" BOOTH="" ONLY="" GPU=llvmpipe OUT=""
while (($#)); do
    case "$1" in
        --avatars) AVATARS="$(realpath "$2")"; shift 2 ;;
        --booth) BOOTH="$(realpath -m "$2")"; shift 2 ;;
        --only) ONLY="$2"; shift 2 ;;
        --gpu) GPU="$2"; shift 2 ;;
        -h|--help) sed -n '2,/^set -euo/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
        -*) echo "unknown option $1" >&2; exit 2 ;;
        *) OUT="$1"; shift ;;
    esac
done
[[ -n "$OUT" ]] || { echo "usage: run.sh [--avatars DIR] [--booth DIR] [--only ITEMS] [--gpu virgl] OUTDIR" >&2; exit 2; }
[[ "$GPU" == llvmpipe || "$GPU" == virgl ]] || { echo "--gpu takes virgl" >&2; exit 2; }
mkdir -p "$OUT"
OUT="$(realpath "$OUT")"
say() { echo ":: $*"; }
die() { echo "run.sh: $*" >&2; exit 1; }
START=$(date +%s)

# the Hyprland hyprwalk.so was built for: the running one, found as build.sh does
HYPR_BIN="${HYPR_BIN:-}"
if [[ -z "$HYPR_BIN" ]]; then
    pid="$(pgrep -x Hyprland | head -n1 || true)"
    [[ -z "$pid" ]] && pid="$(pgrep -f 'bin/Hyprland' | head -n1 || true)"
    [[ -z "$pid" ]] && die "Hyprland isn't running; set HYPR_BIN=/nix/store/...-hyprland-.../bin/Hyprland"
    HYPR_BIN="$(readlink -f "/proc/$pid/exe" 2>/dev/null || true)"
    [[ -z "$HYPR_BIN" ]] && HYPR_BIN="$(tr '\0' '\n' < "/proc/$pid/cmdline" | head -n1)"
fi
HYPR_OUT="${HYPR_BIN%/bin/*}"
[[ -x "$HYPR_OUT/bin/Hyprland" ]] || die "no Hyprland in $HYPR_OUT"
[[ -f "$REPO/hyprwalk.so" ]] || die "no hyprwalk.so: build it with ./build.sh"
say "Hyprland: $HYPR_OUT"

# what goes into the VM
IN="$OUT/in"
rm -rf "$IN"
mkdir -p "$IN/wav" "$IN/emotes"
cp "$REPO/hyprwalk.so" "$VM/wheel.py" "$VM/touchpad.py" "$VM/gamepad.py" "$VM/tkapp.py" "$VM/tkfs.py" "$VM/obsws.py" "$VM/page.html" "$VM/overlay.qml" \
    "$VM/launcher.qml" "$VM/topbar.qml" "$IN/"
cp -r "$VM/electron" "$IN/"
python3 "$VM/assets.py" "$IN" > /dev/null
python3 "$VM/litmap.py" "$IN" > /dev/null
# tools/test/live/check.sh, for the section that runs it in the VM as on a desktop
mkdir -p "$IN/repo/tools/test/live" "$IN/repo/tools/test/vm"
cp "$REPO/tools/test/live/check.sh" "$REPO/tools/test/live/util.py" "$IN/repo/tools/test/live/"
cp "$VM/assets.py" "$IN/repo/tools/test/vm/"
python3 "$REPO/tools/test/synth/vowels.py" "$IN/wav" > /dev/null
if [[ -z "$AVATARS" ]]; then
    AVATARS="$OUT/work/avatars"
    B="${BOOTH:-$OUT/work/booth}"
    mkdir -p "$AVATARS"
    if [[ ! -f "$B/SynthChan_Gimmicks_VRCFury_v1.0.unitypackage" ]]; then
        say "making the Booth-style packages in $B (synth/booth.py)"
        blender -b --factory-startup --python-exit-code 1 -P "$REPO/tools/test/synth/booth.py" -- "$B" > "$OUT/work/booth.log" 2>&1 ||
            { tail -n 20 "$OUT/work/booth.log"; die "booth.py failed"; }
    fi
    say "converting BoothAccessories and BoothGimmicks (unity2hyprwalk.py)"
    python3 "$REPO/tools/unity2hyprwalk.py" "$B/SynthChan_v1.0.unitypackage" --outfit "$B/SynthChan_Accessories_MA_v1.0.unitypackage" \
        -o "$AVATARS/BoothAccessories.glb" > "$OUT/work/BoothAccessories.log" 2>&1 || die "converting BoothAccessories failed, see $OUT/work"
    python3 "$REPO/tools/unity2hyprwalk.py" "$B/SynthChan_v1.0.unitypackage" --outfit "$B/SynthChan_Gimmicks_VRCFury_v1.0.unitypackage" \
        -o "$AVATARS/BoothGimmicks.glb" > "$OUT/work/BoothGimmicks.log" 2>&1 || die "converting BoothGimmicks failed, see $OUT/work"
fi
# copied by name, so nothing else in the folder comes along
for f in BoothAccessories.glb BoothAccessories.hyprwalk.json BoothGimmicks.hands.vrma; do
    [[ -f "$AVATARS/$f" ]] || die "no $AVATARS/$f"
done
cp "$AVATARS/BoothAccessories.glb" "$AVATARS/BoothGimmicks.hands.vrma" "$IN/"
cp "$AVATARS/BoothGimmicks.hands.vrma" "$IN/emotes/Hands.vrma"
# an emote's song (its "sound" setting)
ffmpeg -v error -y -f lavfi -i "aevalsrc=exprs=0.5*sin(2*PI*440*t)|0.5*sin(2*PI*660*t):s=48000:d=2" -c:a libvorbis -q:a 6 "$IN/HandsSong.ogg" ||
    die "ffmpeg couldn't make HandsSong.ogg"
# its settings plus three emotes of BoothGimmicks' 7 s hand poses: double speed, normal, and looped with the song
python3 - "$AVATARS/BoothAccessories.hyprwalk.json" "$IN/BoothAccessories.hyprwalk.json" << 'EOF'
import json, sys
s = json.load(open(sys.argv[1], encoding='utf-8'))
s['emotes'] = s.get('emotes', []) + [{'name': 'Hands Fast', 'file': 'BoothGimmicks.hands.vrma', 'speed': 2},
                                     {'name': 'Hands Slow', 'file': 'BoothGimmicks.hands.vrma'},
                                     {'name': 'Hands Song', 'file': 'BoothGimmicks.hands.vrma', 'loop': True, 'sound': 'HandsSong.ogg'}]
json.dump(s, open(sys.argv[2], 'w', encoding='utf-8'), ensure_ascii=False, indent=1)
EOF
# longer vowels (the WAVs are 0.8 s): each one eight times over, and a man's o then hiss
python3 - "$IN/wav" << 'EOF'
import os, sys, wave
d = sys.argv[1]
for f in sorted(os.listdir(d)):
    if f.endswith('.wav') and not f.endswith('_long.wav'):
        with wave.open(os.path.join(d, f)) as w:
            params, frames = w.getparams(), w.readframes(w.getnframes())
        with wave.open(os.path.join(d, f[:-4] + '_long.wav'), 'wb') as w:
            w.setparams(params)
            w.writeframes(frames * 8)
with wave.open(os.path.join(d, 'man_o.wav')) as o, wave.open(os.path.join(d, 'hiss.wav')) as h:
    params, frames = o.getparams(), o.readframes(o.getnframes()) + h.readframes(h.getnframes()) * 7
with wave.open(os.path.join(d, 'o_then_hiss_long.wav'), 'wb') as w:
    w.setparams(params)
    w.writeframes(frames)
EOF
# a quieter microphone: the vowels 30 dB down (q30_*); and mic hiss: 10 s of white noise at -75 dBFS as raw f32, looped
# into the test microphone
for f in "$IN"/wav/{man,woman}_[aiueo]_long.wav; do
    python3 "$REPO/tools/test/synth/attenuate.py" "$IN/wav/q30_$(basename "$f")" 30 "$f" &
done
python3 - "$IN/wav/micnoise.f32" << 'EOF' &
import random, struct, sys
rnd, sd = random.Random(5), 10 ** ((-75 - 3.01) / 20)
with open(sys.argv[1], 'wb') as f:
    f.write(struct.pack('<%df' % 480000, *(rnd.gauss(0.0, sd) for _ in range(480000))))
EOF
wait

say "building the VM's test driver (vm.nix)"
nix-build "$VM/vm.nix" -A driver --argstr hyprland "$HYPR_OUT" --argstr gpu "$GPU" --argstr rendernode "${HYPRWALK_RENDERNODE:-/dev/dri/renderD129}" \
    -o "$OUT/driver" > "$OUT/nix-build.log" 2>&1 ||
    { tail -n 30 "$OUT/nix-build.log"; die "nix-build failed, see $OUT/nix-build.log"; }
BUILT=$(date +%s)

say "running the checks (checks.py); the VM has no window"
rm -rf "$OUT/frames" "$OUT/logs" "$OUT/live" "$OUT/results.txt" "$OUT/results.json"
# the driver keeps disk images, its shared folder and sockets in XDG_RUNTIME_DIR (before TMPDIR): use a per-run dir on
# disk (the runtime tmpfs is small and a core dump fills it) with a short path (socket paths are limited)
RUNDIR="$(mktemp -d "${HYPRWALK_VM_TMP:-/tmp}/hyprwalk-vm.XXXXXX")"
trap 'rm -rf "$RUNDIR"' EXIT
set +e
# no DISPLAY or WAYLAND_DISPLAY: the driver starts QEMU with -nographic. virgl needs QEMU's egl-headless display
# instead: a WAYLAND_DISPLAY that goes nowhere stops -nographic, and egl-headless opens no window
NODISPLAY=(-u WAYLAND_DISPLAY)
[[ "$GPU" == virgl ]] && NODISPLAY=(WAYLAND_DISPLAY=/nonexistent/no-display)
env -u DISPLAY "${NODISPLAY[@]}" TMPDIR="$RUNDIR" XDG_RUNTIME_DIR="$RUNDIR" HYPRWALK_IN="$IN" HYPRWALK_OUT="$OUT" HYPRWALK_ONLY="$ONLY" \
    "$OUT/driver/bin/nixos-test-driver" --test-script "$VM/checks.py" -o "$OUT" > "$OUT/driver.log" 2>&1
STATUS=$?
set -e
END=$(date +%s)

if [[ -f "$OUT/results.txt" ]]; then
    cat "$OUT/results.txt"
else
    tail -n 40 "$OUT/driver.log"
    echo "no results: see $OUT/driver.log"
fi
say "took $((END - START)) s ($((BUILT - START)) s to get ready, $((END - BUILT)) s in the VM); frames in $OUT/frames, logs in $OUT/logs"
exit $STATUS
