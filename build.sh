#!/usr/bin/env bash
# Builds hyprwalk.so against the exact Hyprland you are running: a plugin needs that build's own headers and compiler. On
# NixOS it takes the `dev` output of the derivation that built the running binary and runs make in that derivation's
# build shell; elsewhere, the headers pkg-config finds (the distribution's Hyprland development package, or hyprpm's
# from hyprpm update) and the system's compiler.
set -euo pipefail
cd "$(dirname "$0")"

die() { echo "build.sh: $*" >&2; exit 1; }

HYPR_BIN="${HYPR_BIN:-}"
if [[ -z "$HYPR_BIN" ]]; then
    pid="$(pgrep -x Hyprland | head -n1 || true)"
    [[ -z "$pid" ]] && pid="$(pgrep -f 'bin/Hyprland' | head -n1 || true)"
    if [[ -n "$pid" ]]; then
        HYPR_BIN="$(readlink -f "/proc/$pid/exe" 2>/dev/null || true)"
        # /proc/<pid>/exe can be unreadable (sandboxes); argv[0] is the store path on NixOS
        [[ -z "$HYPR_BIN" ]] && HYPR_BIN="$(tr '\0' '\n' < "/proc/$pid/cmdline" | head -n1)"
    elif command -v nix-store > /dev/null; then
        die "Hyprland isn't running; set HYPR_BIN=/nix/store/...-hyprland-.../bin/Hyprland"
    else
        HYPR_BIN="$(command -v Hyprland || true)" # else the installed one, to check the headers against
    fi
fi

# NixOS: the headers and the build shell of the derivation that built that Hyprland
build_nix() {
    local HYPR_OUT="${HYPR_BIN%/bin/*}"
    local DRV DEV
    DRV="$(nix-store --query --deriver "$HYPR_OUT")"
    if [[ "$DRV" == "unknown-deriver" || ! -e "$DRV" ]]; then
        echo "Can't find the derivation of $HYPR_OUT (is it garbage collected?)" >&2
        exit 1
    fi

    echo ":: Hyprland: $HYPR_OUT"
    echo ":: deriver:  $DRV"

    DEV="$(nix build --no-link --print-out-paths "$DRV^dev")"
    echo ":: headers:  $DEV"

    # PipeWire headers for lip sync's microphone (src/mic.cpp), from the running one; optional
    local PW_PC="" pw PW_BIN PW_DRV PW_DEV
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
}

# other distributions: pkg-config's headers, else hyprpm's; with both, whichever matches the running version
build_system() {
    command -v pkg-config > /dev/null || die "no pkg-config: install pkgconf (or pkg-config)"
    command -v make > /dev/null || die "no make: install your distribution's build tools (make and g++)"
    local running="" hyprpm_pc="/var/cache/hyprpm/${USER:-$(id -un)}/headersRoot/share/pkgconfig"
    local version='1s/^Hyprland ([0-9][0-9.]*).*/\1/p'
    # the running Hyprland's version (its binary may have been updated since), else the installed one's
    [[ -n "${pid:-}" ]] && command -v hyprctl > /dev/null && running="$(hyprctl version 2>/dev/null | sed -nE "$version" || true)"
    [[ -z "$running" && -n "$HYPR_BIN" ]] &&
        running="$(XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/tmp}" "$HYPR_BIN" --version 2>/dev/null | sed -nE "$version" || true)"
    if [[ -f "$hyprpm_pc/hyprland.pc" ]] &&
        { ! pkg-config --exists hyprland ||
            { [[ -n "$running" && "$(pkg-config --modversion hyprland)" != "$running" ]] &&
                [[ "$(PKG_CONFIG_PATH="$hyprpm_pc" pkg-config --modversion hyprland)" == "$running" ]]; }; }; then
        export PKG_CONFIG_PATH="$hyprpm_pc${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
    fi
    pkg-config --exists hyprland ||
        die "Hyprland's headers aren't installed (pkg-config finds no hyprland.pc). Install your distribution's Hyprland
development package (Arch: hyprland; Debian, Ubuntu: hyprland-dev; openSUSE, Fedora: hyprland-devel), or run
'hyprpm update' for the headers of the Hyprland you run. See Install in README.md."
    local headers
    headers="$(pkg-config --modversion hyprland)"
    echo ":: Hyprland: ${HYPR_BIN:-not found}${running:+ ($running)}"
    echo ":: headers:  $headers ($(pkg-config --variable=prefix hyprland))"
    if [[ -n "$running" && "$running" != "$headers" ]]; then
        echo ":: warning: these headers are $headers's but Hyprland $running runs, and a plugin only loads into the Hyprland" >&2
        echo "   it was built for. After updating Hyprland, log out and back in, then build again." >&2
    fi
    if pkg-config --exists libpipewire-0.3; then
        echo ":: pipewire: $(pkg-config --modversion libpipewire-0.3)"
    else
        echo ":: pipewire: not found, so lip sync has no microphone (install PipeWire's development files for it)"
    fi
    make -j"$(nproc)" "$@"
}

if [[ "$HYPR_BIN" == /nix/store/* ]]; then
    build_nix "$@"
else
    build_system "$@"
fi
if [[ $# -eq 0 ]]; then
    echo ":: built $(pwd)/hyprwalk.so"
fi
