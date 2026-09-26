#!/usr/bin/env bash
# check.sh OUTDIR [--mic] [--avatar FILE] [--map FILE|--no-map] [--app CMD]... [--so FILE]: hypr3d.so in the Hyprland
# you're running now, on your own GPU and monitor: what tools/test/vm can't reach. You run it; it goes through what
# hyprctl can do, and asks you to do what only you can.
#
#   tools/test/live/check.sh ~/hypr3d-live                  # 3D, an avatar, the menu, a notification, a map
#   tools/test/live/check.sh ~/hypr3d-live --mic            # and lip sync on your voice
#   tools/test/live/check.sh ~/hypr3d-live --avatar ~/avatars/me.glb
#   tools/test/live/check.sh ~/hypr3d-live --no-map --app discord --app "steam steam://rungameid/APPID"
#
#   --mic          lip sync on your microphone: say what the notifications ask for (a, i, u, e, o, then "sss", then
#                  nothing); OUTDIR/lipsync.jsonl gets what it heard. (CHECK_SAY=CMD runs CMD a, CMD i ... CMD s,
#                  CMD quiet as each prompt shows: tools/test/vm sings them into its test microphone)
#   --avatar FILE  the avatar to load (default: assets.py's ToonTest.glb, made in OUTDIR; your own tells more)
#   --map FILE     a map to walk into (default: ~/.local/share/hypr3d/maps/de_mirage.glb, if it's there)
#   --no-map       no map
#   --so FILE      the plugin to load (default: the repo's hypr3d.so, as ./build.sh left it)
#   --app CMD      an app of yours (repeatable): a desktop id, an app's name or a command, launched from 3D
#                  (hyprctl hypr3d launch). Notifications then ask you to play it (P) and stop (Super+Esc), type into
#                  it (E), point at it (its own cursor), pin it to your view (H) and unpin it, then try what you
#                  want in it (P, then Super+Esc when done: a call, a screen share, OBS capturing the 3D view); then
#                  its window is closed, as its close button does (a chat app goes to its tray, a game quits). Frames of each step
#                  go to OUTDIR/frames. A Steam game (steam steam://rungameid/ID) is the window Steam starts for it,
#                  not Steam's own. (CHECK_DO=CMD runs CMD STEP WINDOW as each prompt shows: tools/test/vm does the
#                  steps with hyprctl)
#
# On the focused monitor it loads the plugin and compares the desktop before and after, enters 3D, loads the avatar
# and looks at it, opens the Action Menu and plays an emote, shows a notification over the 3D view, picks up the
# window the crosshair starts on (if any) and puts it back, walks into the map, leaves 3D and unloads the plugin.
# Frames go to OUTDIR/frames (grim; diff-A-B.png shows in red what changed between two), hyprctl's answers to
# OUTDIR/status, the plugin's log lines to OUTDIR/hypr3d.log, and a line per check to OUTDIR/results.txt.
#
# Don't touch the mouse or keyboard while it runs: in 3D they're the plugin's. Esc leaves 3D at any time, and then
# Ctrl+C here stops it. Whatever happens, it leaves 3D, turns lip sync off and unloads the plugin. It won't start
# while hypr3d is loaded already.
set -uo pipefail
REPO="$(cd "$(dirname "$0")/../../.." && pwd)"
UTIL=(python3 "$REPO/tools/test/live/util.py")
SO="$REPO/hypr3d.so" AVATAR="" MAP="" NOMAP=0 MIC=0 OUT="" APPS=()
while (($#)); do
    case "$1" in
        --mic) MIC=1; shift ;;
        --avatar) AVATAR="$(realpath "$2")"; shift 2 ;;
        --map) MAP="$(realpath "$2")"; shift 2 ;;
        --no-map) NOMAP=1; shift ;;
        --so) SO="$(realpath "$2")"; shift 2 ;;
        --app) APPS+=("$2"); shift 2 ;;
        -h|--help) sed -n '2,/^set -uo/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
        -*) echo "unknown option $1" >&2; exit 2 ;;
        *) OUT="$1"; shift ;;
    esac
done
[[ -n "$OUT" ]] || { echo "usage: check.sh OUTDIR [--mic] [--avatar FILE] [--map FILE|--no-map] [--app CMD]... [--so FILE]" >&2; exit 2; }
die() { echo "check.sh: $*" >&2; exit 1; }
[[ -n "${HYPRLAND_INSTANCE_SIGNATURE:-}" ]] || die "run it from inside your Hyprland session"
for c in hyprctl grim python3; do command -v "$c" > /dev/null || die "no $c"; done
[[ -f "$SO" ]] || die "no $SO: build it with ./build.sh"
hyprctl version > /dev/null 2>&1 || die "hyprctl doesn't answer"
hyprctl plugin list 2>/dev/null | grep -q hypr3d && die "hypr3d is loaded already: unload it first (hyprctl plugin unload PATH)"
[[ -z "$MAP" && $NOMAP -eq 0 && -f "$HOME/.local/share/hypr3d/maps/de_mirage.glb" ]] && MAP="$HOME/.local/share/hypr3d/maps/de_mirage.glb"
((NOMAP)) && MAP=""
mkdir -p "$OUT/frames" "$OUT/status" "$OUT/raw"
OUT="$(realpath "$OUT")"
: > "$OUT/results.txt"
if [[ -z "$AVATAR" ]]; then
    python3 "$REPO/tools/test/vm/assets.py" "$OUT" > /dev/null || die "assets.py failed"
    AVATAR="$OUT/ToonTest.glb"
fi
HYPRLOG="${XDG_RUNTIME_DIR:-/run/user/$UID}/hypr/$HYPRLAND_INSTANCE_SIGNATURE/hyprland.log"

PASS=0 FAIL=0 N=0 LOADED=0 START=$(date +%s)
log() { echo "$*" | tee -a "$OUT/results.txt"; }
check() { # what, an exit status (0 = it passed), what was seen
    if (($2 == 0)); then PASS=$((PASS + 1)); log "ok    $1${3:+  [$3]}"; else FAIL=$((FAIL + 1)); log "FAIL  $1${3:+  [$3]}"; fi
}
note() { log "      $1${2:+  [$2]}"; }
ctl() { hyprctl hypr3d "$@" 2>&1; }
js() { "${UTIL[@]}" json "$1"; }                               # a value out of the JSON on stdin
is() { python3 -c "import sys; sys.exit(0 if ($1) else 1)"; } # a comparison of numbers
wait_for() { # seconds, a command: till it succeeds
    local end=$((SECONDS + $1))
    shift
    while ((SECONDS < end)); do "$@" && return 0; sleep 0.25; done
    return 1
}
mode_is() { [[ "$(ctl status | js mode)" == "$1" ]]; }
avatar_loaded() { [[ "$(ctl avatar | js loading)" == false && "$(ctl avatar | js name)" != null ]]; }
map_is() { [[ "$(ctl map | js loading)" == false && "$(ctl map | js map)" == "$1" ]]; }
placed_is() { [[ "$(ctl status | js placed)" == "$1" ]]; }
listening() { [[ "$(ctl avatar lipsync | js listening)" == true ]]; }
face_avatar() { # distance, pitch: third person, looking at its face
    ctl view third "$1" > /dev/null
    ctl turn "$(python3 -c "print($(ctl avatar | js bodyYaw) + 180)")" "$2" > /dev/null
}

# the focused monitor, where 3D goes, and the frames come from
read -r MON MON_W MON_H MON_HZ MON_SCALE <<< "$(hyprctl -j monitors | python3 -c 'import json, sys
m = next(m for m in json.load(sys.stdin) if m["focused"])
print(m["name"], m["width"], m["height"], m["refreshRate"], m["scale"])')"
shot() { # name: a frame, as frames/NN-name.png (and raw/name.ppm to compare)
    N=$((N + 1))
    grim -o "$MON" -t ppm "$OUT/raw/$1.ppm" && "${UTIL[@]}" png "$OUT/raw/$1.ppm" "$OUT/frames/$(printf %02d $N)-$1.png"
}
differs() { "${UTIL[@]}" diff "$OUT/raw/$1.ppm" "$OUT/raw/$2.ppm" "$OUT/frames/diff-$1-$2.png"; }
calm() { hyprctl dismissnotify > /dev/null 2>&1; sleep 1.2; } # no notifications in the next frame
say() {                                                       # what to do, also over the 3D view
    echo ">>> $1"
    hyprctl notify 1 "${2:-2500}" "rgb(33ccff)" "hypr3d live check: $1" > /dev/null 2>&1
}

cleanup() {
    trap - EXIT INT TERM
    if ((LOADED)); then
        ctl avatar lipsync off > /dev/null
        ctl off now > /dev/null
        sleep 0.5
        local r
        r="$(hyprctl plugin unload "$SO" 2>&1)"
        if [[ "$r" == ok ]]; then note "unloaded the plugin"; else FAIL=$((FAIL + 1)); log "FAIL  unloading the plugin  [$r]"; fi
    fi
    [[ -f "$HYPRLOG" ]] && grep -a "\[hypr3d\]" "$HYPRLOG" > "$OUT/hypr3d.log"
    rm -rf "$OUT/raw"
    log "$PASS passed, $FAIL failed, in $(($(date +%s) - START)) s; frames in $OUT/frames"
}
trap cleanup EXIT
trap 'echo; echo "interrupted: cleaning up"; exit 130' INT TERM

note "monitor" "$MON ${MON_W}x${MON_H} at $MON_HZ Hz, scale $MON_SCALE"
note "Hyprland" "$(hyprctl version | head -n1)"
calm
shot desktop-before

# --- loading
r="$(hyprctl plugin load "$SO" 2>&1)"
[[ "$r" == ok ]] && LOADED=1
check "hyprctl plugin load" $((1 - LOADED)) "$r"
((LOADED)) || exit 1
sleep 1.5
s="$(ctl status | tee "$OUT/status/loaded.json")"
[[ "$(js mode <<< "$s")" == off && "$(js hooks.motion <<< "$s")" == true && "$(js hooks.cursor <<< "$s")" == true ]]
check "status: off, its hooks in" $? "$(js hooks <<< "$s")"
calm
shot desktop-loaded
d="$(differs desktop-before desktop-loaded)"
is "$d < 0.01"
check "loading it changes nothing on the screen (a live desktop can: see frames/diff-*)" $? "$d of the pixels differ"

# --- 3D
r="$(ctl on)"
wait_for 5 mode_is active
check "hyprctl hypr3d on: in 3D" $? "$r"
sleep 2
s="$(ctl status | tee "$OUT/status/3d.json")"
fps="$(js fps <<< "$s")"
is "$fps >= 0.75 * $MON_HZ"
check "it keeps up with the monitor's $MON_HZ Hz" $? "$fps frames a second"
shot 3d
d="$(differs desktop-loaded 3d)"
is "$d > 0.3"
check "the frame is the 3D view" $? "$d of the pixels differ from the desktop"

# --- the avatar
r="$(ctl avatar "$AVATAR")"
wait_for 120 avatar_loaded
check "avatar $(basename "$AVATAR") loads" $? "$r"
a="$(ctl avatar | tee "$OUT/status/avatar.json")"
note "avatar" "$(js name <<< "$a"): $(js triangles <<< "$a") triangles, $(js height <<< "$a") m tall"
face_avatar 2.4 -6
sleep 1.5
shot avatar
note "frames a second with the avatar" "$(ctl status | js fps)"

# --- the Action Menu
r="$(ctl menu open)"
sleep 0.5
[[ "$(ctl menu | js open)" == true && "$(ctl menu | js path)" == main ]]
check "the Action Menu opens" $? "$r"
ctl menu move 0 -160 > /dev/null
[[ "$(ctl menu | js highlight)" == 1 ]]
check "... its cursor points up at Emotes" $? "highlight $(ctl menu | js highlight)"
sleep 0.3
shot menu
ctl menu pick > /dev/null
sleep 0.3
[[ "$(ctl menu | js path)" == main/emotes ]]
check "... Emotes opens its page" $? "$(ctl menu | js path)"
ctl menu pick 1 > /dev/null
sleep 1
e="$(ctl avatar | js emote)"
[[ -n "$e" ]]
check "... its first emote plays" $? "$e"
shot emote
ctl menu close > /dev/null
ctl avatar emote stop > /dev/null

# --- a notification over the 3D view
calm
hyprctl notify 1 6000 "rgb(ff8800)" "hypr3d live check: a notification over the 3D view" > /dev/null
sleep 1
shot notification
o="$("${UTIL[@]}" count "$OUT/raw/notification.ppm" orange 0.5 0 1 0.25)"
((o > 20))
check "Hyprland draws its notification over the 3D view" $? "$o orange pixels in the top right"
hyprctl dismissnotify > /dev/null 2>&1

# --- carrying the window the crosshair starts on, if there's one
ctl view first > /dev/null
ctl spawn > /dev/null
sleep 1
kind="$(ctl status | js aimed.kind)"
if [[ "$kind" == window ]]; then
    r="$(ctl grab)"
    [[ "$r" == holding && "$(ctl status | js holding)" == true ]]
    check "hyprctl hypr3d grab (G): the window under the crosshair, picked up" $? "$r"
    ctl hold 1.6 0.8 > /dev/null
    ctl turn 30 5 > /dev/null
    sleep 1
    shot holding
    r="$(ctl place)"
    sleep 0.8
    [[ "$r" == placed ]] && placed_is 1
    check "... put down where it is" $? "$r; $(ctl windows)"
    shot placed
    ctl reset-windows > /dev/null
    wait_for 5 placed_is 0
    check "... and back on the wall" $?
else
    note "no window under the crosshair at the start: carrying one skipped" "$kind"
fi

# --- your apps: launched from 3D, played, typed into, pointed at, pinned to the view
playing() { [[ "$(ctl status | js playing)" != null ]]; }
walking() { [[ "$(ctl status | js playing)" == null && "$(ctl status | js typing)" == false ]]; }
typing() { [[ "$(ctl status | js typing)" == true && "$(ctl status | js playing)" == null ]]; }
cursor_on() { [[ "$(ctl status | js cursor)" != null ]]; }
pinned() { ctl windows | python3 -c 'import json, sys; sys.exit(0 if any(p["address"] == sys.argv[1] and p["pinned"] for p in json.load(sys.stdin)["placed"]) else 1)' "$1"; }
ask() { # what to do, the step, seconds for it: shown over the 3D view as long as it's waited for
    say "$1" "$(($3 * 1000))"
    [[ -n "${CHECK_DO:-}" ]] && $CHECK_DO "$2" "$ADDR" > /dev/null 2>&1 & # (tools/test/vm does it with hyprctl)
}
for app in "${APPS[@]}"; do
    ctl view first > /dev/null
    before="$(hyprctl -j clients | python3 -c 'import json, sys; print(" ".join(c["address"] for c in json.load(sys.stdin)))')"
    r="$(ctl launch "$app")"
    newwin() { # the window that opened for it: its address and class. One hypr3d placed as launched from 3D, else
        # the first new one (a Steam game's: not Steam's own windows, which open first when Steam wasn't running)
        hyprctl -j clients | python3 -c 'import json, subprocess, sys
old, steam = set(sys.argv[1].split()), "steam://rungameid/" in sys.argv[2]
placed = {p["address"] for p in json.loads(subprocess.run(["hyprctl", "hypr3d", "windows"], capture_output=True, text=True).stdout or "{}").get("placed", [])}
new = [c for c in json.load(sys.stdin) if c["address"] not in old and c["mapped"] and not (steam and c["class"].lower() == "steam")]
new.sort(key=lambda c: c["address"] not in placed)
print(*(new[0]["address"], new[0]["class"]) if new else "")' "$before" "$app"
    }
    got() { [[ -n "$(newwin)" ]]; }
    say "starting $app: its window should open in front of you" 5000
    wait_for 180 got
    check "--app $app: launched from 3D, a window opens" $? "$r"
    read -r ADDR CLS <<< "$(newwin)"
    [[ -z "$ADDR" ]] && continue
    sleep 3
    # (one it doesn't know was launched from 3D, a game Steam starts, opens on the wall: it's brought here)
    if ! ctl windows | grep -q "\"address\": \"$ADDR\""; then
        note "$CLS opened on the wall (not known as launched from 3D): brought in front of you"
        ctl window "$ADDR" bring > /dev/null
        sleep 1
    fi
    note "$CLS" "$(ctl windows | python3 -c 'import json, sys; print([p for p in json.load(sys.stdin)["placed"] if p["address"] == sys.argv[1]])' "$ADDR")"
    shot "app-$CLS-opened"
    ask "look at $CLS and press P to play it (a game: look around, move, use your controller)" play 60
    wait_for 60 playing
    check "P: playing $CLS" $? "$(ctl play)"
    note "while playing" "$(ctl play); $(ctl status | js fps) frames a second"
    sleep 8
    shot "app-$CLS-playing"
    ask "Super+Esc stops playing" stop 30
    wait_for 30 walking
    check "Super+Esc: walking again, the mouse and keys yours" $?
    ask "press E, type something into $CLS, then Super+Esc" type 60
    wait_for 60 typing
    t=$?
    shot "app-$CLS-typing"
    wait_for 60 walking
    check "E: typing into it, and Super+Esc back to walking" $((t || $?))
    ask "point the crosshair at $CLS: its own cursor shows where the crosshair is" cursor 20
    wait_for 20 cursor_on
    check "its own cursor, drawn on it" $? "$(ctl status | js cursor)"
    shot "app-$CLS-cursor"
    ask "press H pointing at $CLS: it pins to your view; look around a bit" pin 45
    wait_for 45 pinned "$ADDR"
    check "H: pinned to the view" $?
    sleep 5
    shot "app-$CLS-pinned"
    ctl window "$ADDR" unpin > /dev/null
    # what only you can judge: a call, a screen share, OBS capturing the 3D view, a controller
    ask "now try what matters to you in $CLS (a call, a screen share, OBS capturing this view, a controller): P to play it, Super+Esc when you're done (5 minutes at most)" free 30
    if wait_for 60 playing; then
        sleep 5
        shot "app-$CLS-free"
        note "$CLS, your own try" "$(ctl play); $(ctl status | js fps) frames a second"
        wait_for 300 walking
        note "... done" "$(ctl status | js fps) frames a second walking"
    else
        note "$CLS: your own try skipped (not played within a minute)"
    fi
    say "closing $CLS's window, as its close button does" 4000
    ctl window "$ADDR" close > /dev/null
    shut() { ! hyprctl -j clients | grep -q "\"address\": \"$ADDR\""; }
    if wait_for 20 shut; then
        note "$CLS: its window closed"
    else
        ctl window "$ADDR" wall > /dev/null
        note "$CLS: its window didn't close (it may ask first): sent back to the wall, open; the desktop checks at the end will see it"
    fi
    sleep 1
done

# --- a map
if [[ -n "$MAP" ]]; then
    r="$(ctl map "$MAP")"
    t0=$SECONDS
    wait_for 180 map_is "$MAP"
    check "map $(basename "$MAP") loads" $? "$r; in about $((SECONDS - t0)) s"
    ctl spawn > /dev/null
    sleep 2
    shot map-spawn
    s="$(ctl status | tee "$OUT/status/map.json")"
    note "in the map" "$(js fps <<< "$s") frames a second, exposure $(js exposure <<< "$s")"
    ctl view third > /dev/null
    ctl walk 1.5 forward > /dev/null
    sleep 2.5
    shot map-walked
    ctl map none > /dev/null
    wait_for 10 map_is ""
    check "... and back to the courtyard" $?
fi

# --- lip sync on your voice
if ((MIC)); then
    r="$(ctl avatar lipsync on)"
    wait_for 5 listening
    check "lip sync on: listening" $? "$r"
    face_avatar 1.4 -3
    sleep 1
    shot lipsync-badge
    # (where it's drawn: under Hyprland's notifications, however many there are)
    box="$(ctl avatar lipsync | python3 -c 'import json, subprocess, sys
b = json.load(sys.stdin)["badge"] or [0, 0, 0, 0]
m = next(m for m in json.loads(subprocess.run(["hyprctl", "-j", "monitors"], capture_output=True, text=True).stdout) if m["focused"])
print((b[0] - 4) / m["width"], (b[1] - 4) / m["height"], (b[0] + b[2] + 4) / m["width"], (b[1] + b[3] + 4) / m["height"])')"
    red="$("${UTIL[@]}" count "$OUT/raw/lipsync-badge.ppm" red $box)"
    ((red > 20))
    check "its badge in the top right corner" $? "$red red pixels; at $(ctl avatar lipsync | js badge)"
    : > "$OUT/lipsync.jsonl"
    listen() { # what to say, a name: what lip sync hears for 3 s
        say "$1" 3500
        [[ -n "${CHECK_SAY:-}" ]] && $CHECK_SAY "$2" & # (tools/test/vm's VM sings it into its test microphone)
        sleep 0.8
        local end=$((SECONDS + 3))
        while ((SECONDS < end)); do
            printf '{"say": "%s", "at": %s, "heard": %s}\n' "$2" "$(date +%s.%N)" "$(ctl avatar lipsync)" >> "$OUT/lipsync.jsonl"
            sleep 0.12
        done
    }
    for v in a i u e o; do listen "say \"$v$v$v$v\" till this goes away" "$v"; done
    listen "say \"sssss\" till this goes away" s
    listen "stay quiet till this goes away" quiet
    heard="$(python3 - "$OUT/lipsync.jsonl" << 'EOF'
import json, sys
rows = [json.loads(l) for l in open(sys.argv[1])]
names = ['aa', 'ih', 'ou', 'ee', 'oh']
want = {'a': 'aa', 'i': 'ih', 'u': 'ou', 'e': 'ee', 'o': 'oh'}
for say in ['a', 'i', 'u', 'e', 'o', 's', 'quiet']:
    heard = [r['heard'] for r in rows if r['say'] == say]
    if not heard:
        continue
    med = {n: sorted(h['visemes'][n] for h in heard)[len(heard) // 2] for n in names}
    loud = sorted(h['level'] for h in heard)[len(heard) // 2]
    each = ' '.join(f'{n} {med[n]:.2f}' for n in names)
    if say in want:
        best = max(names, key=lambda n: med[n])
        ok = best == want[say] and med[best] > 0.3
        print(f"{'ok  ' if ok else 'FAIL'}  you said {say}: {best} the most  [{each}; {loud:.0f} dB]")
    else:
        most = sorted(max(h['visemes'].values()) for h in heard)[len(heard) * 3 // 4]
        ok = most < 0.15
        print(f"{'ok  ' if ok else 'FAIL'}  {'sss' if say == 's' else 'silence'}: the mouth about shut  [{most:.2f} at most, most of the time; {each}; {loud:.0f} dB]")
EOF
)"
    log "$heard"
    PASS=$((PASS + $(grep -c '^ok' <<< "$heard"))) FAIL=$((FAIL + $(grep -c '^FAIL' <<< "$heard")))
    r="$(ctl avatar lipsync off)"
    [[ "$(ctl avatar lipsync | js listening)" == false ]]
    check "lip sync off: not listening" $? "$r"
fi

# --- out of 3D, and the plugin out
ctl view first > /dev/null
r="$(ctl off)"
wait_for 5 mode_is off
check "hyprctl hypr3d off: out of 3D" $? "$r"
calm
shot desktop-after-3d
d="$(differs desktop-loaded desktop-after-3d)"
is "$d < 0.01"
check "the desktop looks as it did before 3D (a live desktop can change: see frames/diff-*)" $? "$d of the pixels differ"
r="$(hyprctl plugin unload "$SO" 2>&1)"
[[ "$r" == ok ]] && LOADED=0
[[ "$r" == ok ]] && ! hyprctl plugin list | grep -q hypr3d
check "hyprctl plugin unload" $? "$r"
calm
shot desktop-unloaded
d="$(differs desktop-before desktop-unloaded)"
is "$d < 0.01"
check "the desktop looks as it did before the plugin (see frames/diff-*)" $? "$d of the pixels differ"
((FAIL == 0))
