#!/usr/bin/env bash
# Builds hypr3d.so against the exact Hyprland you are running.
#
# Hyprland plugins must be compiled against the headers of the very same
# Hyprland build, with the same compiler. On NixOS we get both by asking Nix
# for the derivation that produced the running binary, realising its `dev`
# output (the headers) and compiling inside that derivation's build shell.
set -euo pipefail
cd "$(dirname "$0")"

HYPR_BIN="${HYPR_BIN:-}"
if [[ -z "$HYPR_BIN" ]]; then
    pid="$(pgrep -x Hyprland | head -n1 || true)"
    [[ -z "$pid" ]] && pid="$(pgrep -f 'bin/Hyprland' | head -n1 || true)"
    [[ -z "$pid" ]] && { echo "Hyprland isn't running; set HYPR_BIN=/nix/store/...-hyprland-.../bin/Hyprland" >&2; exit 1; }
    HYPR_BIN="$(readlink -f "/proc/$pid/exe" 2>/dev/null || true)"
    # /proc/<pid>/exe can be unreadable (sandboxes); argv[0] is the store path on NixOS
    [[ -z "$HYPR_BIN" ]] && HYPR_BIN="$(tr '\0' '\n' < "/proc/$pid/cmdline" | head -n1)"
fi
HYPR_OUT="${HYPR_BIN%/bin/*}"

DRV="$(nix-store --query --deriver "$HYPR_OUT")"
if [[ "$DRV" == "unknown-deriver" || ! -e "$DRV" ]]; then
    echo "Can't find the derivation of $HYPR_OUT (is it garbage collected?)" >&2
    exit 1
fi

echo ":: Hyprland: $HYPR_OUT"
echo ":: deriver:  $DRV"

DEV="$(nix build --no-link --print-out-paths "$DRV^dev")"
echo ":: headers:  $DEV"

# PipeWire, for lip sync's microphone (src/mic.cpp): the headers of the one that runs, as for Hyprland. Without
# them hypr3d builds without a microphone
PW_PC=""
if pw="$(pgrep -x pipewire | head -n1)" && [[ -n "$pw" ]]; then
    PW_BIN="$(readlink -f "/proc/$pw/exe" 2>/dev/null || true)"
    [[ -z "$PW_BIN" ]] && PW_BIN="$(tr '\0' '\n' < "/proc/$pw/cmdline" | head -n1)"
    PW_DRV="$(nix-store --query --deriver "${PW_BIN%/bin/*}" 2>/dev/null || true)"
    if [[ -n "$PW_DRV" && -e "$PW_DRV" ]] && PW_DEV="$(nix build --no-link --print-out-paths "$PW_DRV^dev" 2>/dev/null)"; then
        PW_PC="$PW_DEV/lib/pkgconfig"
        echo ":: pipewire: $PW_DEV"
    fi
fi
[[ -z "$PW_PC" ]] && echo ":: pipewire: not found, so lip sync has no microphone"

nix develop "$DRV^*" --command bash -c "
    export PKG_CONFIG_PATH='$DEV/share/pkgconfig':'$PW_PC':\"\$PKG_CONFIG_PATH\"
    make -j\$(nproc) $*
"
if [[ $# -eq 0 ]]; then
    echo ":: built $(pwd)/hypr3d.so"
fi
