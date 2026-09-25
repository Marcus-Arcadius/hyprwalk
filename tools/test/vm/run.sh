#!/usr/bin/env bash
# run.sh OUTDIR: hypr3d.so in a real Hyprland, in NixOS VMs (vm.nix) that QEMU runs with no window, driven through
# the VM's own keyboard, mouse and tablet (QMP input events, so they go through libinput and Hyprland's input stack
# to the plugin's hooks), with PipeWire and a virtual microphone singing test vowels. checks.py is the checklist.
#
#   tools/test/vm/run.sh [--avatars DIR] [--booth DIR] [--only ITEMS] [--gpu virgl] OUTDIR
#
#   --avatars DIR  take BoothAccessories.glb (with its settings file) and BoothGimmicks.hands.vrma from DIR, as
#                  regress.sh --keep leaves them in OUT/new; else they're converted here from the Booth-style
#                  packages (synth/booth.py, then unity2hypr3d.py)
#   --booth DIR    the Booth-style packages to convert (booth.py makes them there if missing)
#   --only ITEMS   only these sections of checks.py, by item, separated by commas (and "0", which starts Hyprland
#                  and loads the plugin): for working on one
#   --gpu virgl    a GPU of this machine draws, through virglrenderer (QEMU's egl-headless display on the render
#                  node H3D_RENDERNODE, /dev/dri/renderD129 by default: the Intel iGPU here), instead of Mesa's
#                  llvmpipe in the VM
#
# Only synthetic things go into the VM: those two avatars, ToonTest.glb and TestRoom.glb (assets.py), LitCourt.glb
# (litmap.py), the vowels (synth/vowels.py), wheel.py, the live check script and hypr3d.so. OUTDIR gets results.txt
# (a line per check), results.json, frames/ (grim's PNGs from inside the VMs), logs/ (Hyprland's logs, the journal,
# pw-dump; logs/hidpi: the second VM's), live/ (the live check's results and frames) and driver.log. OUTDIR/driver is
# the driver (a GC root: delete OUTDIR to let the VMs' closure go). The Hyprland is the running one's, as for
# build.sh, or HYPR_BIN's. Exit status: 0 when every check passed (a "known" failure is Hyprland's own bug).
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

# the Hyprland hypr3d.so was built for: the running one's, as build.sh finds it
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
[[ -f "$REPO/hypr3d.so" ]] || die "no hypr3d.so: build it with ./build.sh"
say "Hyprland: $HYPR_OUT"

# what goes into the VM
IN="$OUT/in"
rm -rf "$IN"
mkdir -p "$IN/wav" "$IN/emotes"
cp "$REPO/hypr3d.so" "$VM/wheel.py" "$IN/"
python3 "$VM/assets.py" "$IN" > /dev/null
python3 "$VM/litmap.py" "$IN" > /dev/null
# tools/test/live/check.sh, as you'd run it on your desktop (its section runs it in the VM)
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
    say "converting BoothAccessories and BoothGimmicks (unity2hypr3d.py)"
    python3 "$REPO/tools/unity2hypr3d.py" "$B/SynthChan_v1.0.unitypackage" --outfit "$B/SynthChan_Accessories_MA_v1.0.unitypackage" \
        -o "$AVATARS/BoothAccessories.glb" > "$OUT/work/BoothAccessories.log" 2>&1 || die "converting BoothAccessories failed, see $OUT/work"
    python3 "$REPO/tools/unity2hypr3d.py" "$B/SynthChan_v1.0.unitypackage" --outfit "$B/SynthChan_Gimmicks_VRCFury_v1.0.unitypackage" \
        -o "$AVATARS/BoothGimmicks.glb" > "$OUT/work/BoothGimmicks.log" 2>&1 || die "converting BoothGimmicks failed, see $OUT/work"
fi
# by name, so nothing else in the folder (a Booth item's conversion) comes along
for f in BoothAccessories.glb BoothAccessories.hypr3d.json BoothGimmicks.hands.vrma; do
    [[ -f "$AVATARS/$f" ]] || die "no $AVATARS/$f"
done
cp "$AVATARS/BoothAccessories.glb" "$AVATARS/BoothGimmicks.hands.vrma" "$IN/"
cp "$AVATARS/BoothGimmicks.hands.vrma" "$IN/emotes/Hands.vrma"
# its settings with two emotes more: BoothGimmicks' hand poses (a 7 s VRM animation) at twice its speed and as it is
python3 - "$AVATARS/BoothAccessories.hypr3d.json" "$IN/BoothAccessories.hypr3d.json" << 'EOF'
import json, sys
s = json.load(open(sys.argv[1], encoding='utf-8'))
s['emotes'] = s.get('emotes', []) + [{'name': 'Hands Fast', 'file': 'BoothGimmicks.hands.vrma', 'speed': 2},
                                     {'name': 'Hands Slow', 'file': 'BoothGimmicks.hands.vrma'}]
json.dump(s, open(sys.argv[2], 'w', encoding='utf-8'), ensure_ascii=False, indent=1)
EOF
# the vowels sung for longer (the WAVs are 0.8 s): the same file eight times over, and a man's o then hiss
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

say "building the VM's test driver (vm.nix)"
nix-build "$VM/vm.nix" -A driver --argstr hyprland "$HYPR_OUT" --argstr gpu "$GPU" --argstr rendernode "${H3D_RENDERNODE:-/dev/dri/renderD129}" \
    -o "$OUT/driver" > "$OUT/nix-build.log" 2>&1 ||
    { tail -n 30 "$OUT/nix-build.log"; die "nix-build failed, see $OUT/nix-build.log"; }
BUILT=$(date +%s)

say "running the checks (checks.py); the VM has no window"
rm -rf "$OUT/frames" "$OUT/logs" "$OUT/live" "$OUT/results.txt" "$OUT/results.json"
# The driver keeps the VMs' disk images, its shared folder and its sockets in XDG_RUNTIME_DIR (before TMPDIR): a
# folder of this run's, on disk (the runtime dir is a small tmpfs, which a core dump fills) and with a short path (a
# socket's path can't be long), gone afterwards
RUNDIR="$(mktemp -d "${H3D_VM_TMP:-/tmp}/h3d-vm.XXXXXX")"
trap 'rm -rf "$RUNDIR"' EXIT
set +e
# no DISPLAY or WAYLAND_DISPLAY: the driver then starts QEMU with -nographic (the VM's config asks for it too). With
# virgl, a WAYLAND_DISPLAY that goes nowhere keeps it off, so QEMU's display is egl-headless, which opens no window
NODISPLAY=(-u WAYLAND_DISPLAY)
[[ "$GPU" == virgl ]] && NODISPLAY=(WAYLAND_DISPLAY=/nonexistent/no-display)
env -u DISPLAY "${NODISPLAY[@]}" TMPDIR="$RUNDIR" XDG_RUNTIME_DIR="$RUNDIR" H3D_IN="$IN" H3D_OUT="$OUT" H3D_ONLY="$ONLY" \
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
