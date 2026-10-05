#!/usr/bin/env bash
# check.sh: hyprwalk.so in the Hyprland you're running, on your own GPU and monitor (what tools/test/vm can't reach).
# It does what hyprctl can and asks you to do the rest. Each run gets its own OUTDIR/run-N (OUTDIR/latest).
#
#   tools/test/live/check.sh OUTDIR [--mic] [--avatar FILE] [--map FILE|--no-map] [--app CMD]... [--so FILE]
#                            [--monitor NAME]
#
#   tools/test/live/check.sh ~/hyprwalk-live                  # 3D, an avatar, the menu, a notification, a map
#   tools/test/live/check.sh ~/hyprwalk-live --mic            # and lip sync on your voice
#   tools/test/live/check.sh ~/hyprwalk-live --no-map --app discord --app "steam steam://rungameid/APPID"
#
#   --mic          lip sync on your microphone: hold each sound a notification asks for (ah, ee, oo, eh, oh, sss,
#                  silence) until it goes; results in lipsync.jsonl, PipeWire's view of the microphone in audio/
#   --avatar FILE  the avatar to load (default: assets.py's ToonTest.glb, made in OUTDIR)
#   --map FILE     a map to walk into (default: ~/.local/share/hyprwalk/maps/de_mirage.glb, if there)
#   --no-map       no map
#   --so FILE      the plugin to load (default: the repo's hyprwalk.so from ./build.sh)
#   --monitor NAME 3D on that monitor, not the focused one: run it from a terminal on another monitor to keep the
#                  terminal out of the desktop comparisons
#   --app CMD      an app to launch from 3D (repeatable): a desktop id, name or command. Notifications take you
#                  through playing, typing, pointing, carrying, pinning and a free try; then its window is closed.
#                  A Steam game (steam steam://rungameid/ID) is the game's window, not Steam's
#   CHECK_SAY=CMD  runs CMD a|i|u|e|o|s|quiet at each lip sync prompt (tools/test/vm sings into its test microphone)
#   CHECK_DO=CMD   runs CMD STEP WINDOW at each --app prompt (tools/test/vm does the steps with hyprctl)
#
# The desktop must look the same after 3D and after unloading, leaving out the terminal and what changes within a
# second. Output: frames/ (diff-A-B.png: red changed, blue left out), status/, hyprwalk.log, results.txt.
#
# Don't touch the mouse or keyboard while it runs; Esc leaves 3D, then Ctrl+C stops it. It always cleans up (3D and
# lip sync off, plugin unloaded).
set -uo pipefail
REPO="$(cd "$(dirname "$0")/../../.." && pwd)"
UTIL=(python3 "$REPO/tools/test/live/util.py")
SO="$REPO/hyprwalk.so" AVATAR="" MAP="" NOMAP=0 MIC=0 OUT="" APPS=() MONITOR=""
while (($#)); do
    case "$1" in
        --mic) MIC=1; shift ;;
        --avatar) AVATAR="$(realpath "$2")"; shift 2 ;;
        --map) MAP="$(realpath "$2")"; shift 2 ;;
        --no-map) NOMAP=1; shift ;;
        --so) SO="$(realpath "$2")"; shift 2 ;;
        --app) APPS+=("$2"); shift 2 ;;
        --monitor) MONITOR="$2"; shift 2 ;;
        -h|--help) sed -n '2,/^set -uo/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
        -*) echo "unknown option $1" >&2; exit 2 ;;
        *) OUT="$1"; shift ;;
    esac
done
[[ -n "$OUT" ]] || { echo "usage: check.sh OUTDIR [--mic] [--avatar FILE] [--map FILE|--no-map] [--app CMD]... [--so FILE] [--monitor NAME]" >&2; exit 2; }
BASE="$OUT"
die() { echo "check.sh: $*" >&2; exit 1; }
[[ -n "${HYPRLAND_INSTANCE_SIGNATURE:-}" ]] || die "run it from inside your Hyprland session"
for c in hyprctl grim python3; do command -v "$c" > /dev/null || die "no $c"; done
[[ -f "$SO" ]] || die "no $SO: build it with ./build.sh"
hyprctl version > /dev/null 2>&1 || die "hyprctl doesn't answer"
hyprctl plugin list 2>/dev/null | grep -q hyprwalk && die "hyprwalk is loaded already: unload it first (hyprctl plugin unload PATH)"
[[ -z "$MAP" && $NOMAP -eq 0 && -f "$HOME/.local/share/hyprwalk/maps/de_mirage.glb" ]] && MAP="$HOME/.local/share/hyprwalk/maps/de_mirage.glb"
((NOMAP)) && MAP=""
# each run gets a new run-N folder
mkdir -p "$BASE" || die "can't make $BASE"
BASE="$(realpath "$BASE")"
RUN=1
while [[ -e "$BASE/run-$RUN" ]]; do RUN=$((RUN + 1)); done
OUT="$BASE/run-$RUN"
mkdir -p "$OUT/frames" "$OUT/status" "$OUT/raw" || die "can't make $OUT"
ln -sfn "run-$RUN" "$BASE/latest"
: > "$OUT/results.txt"
echo "this run's results go to $OUT"
if [[ -z "$AVATAR" ]]; then
    python3 "$REPO/tools/test/vm/assets.py" "$OUT" > /dev/null || die "assets.py failed"
    AVATAR="$OUT/ToonTest.glb"
fi
HYPRLOG="${XDG_RUNTIME_DIR:-/run/user/$UID}/hypr/$HYPRLAND_INSTANCE_SIGNATURE/hyprland.log"

PASS=0 FAIL=0 N=0 LOADED=0 START=$(date +%s)
log() { echo "$*" | tee -a "$OUT/results.txt"; }
check() { # WHAT STATUS [SEEN]; status 0 = passed
    if (($2 == 0)); then PASS=$((PASS + 1)); log "ok    $1${3:+  [$3]}"; else FAIL=$((FAIL + 1)); log "FAIL  $1${3:+  [$3]}"; fi
}
note() { log "      $1${2:+  [$2]}"; }
ctl() { hyprctl hyprwalk "$@" 2>&1; }
js() { "${UTIL[@]}" json "$1"; }                               # value at a dotted path in the JSON on stdin
is() { python3 -c "import sys; sys.exit(0 if ($1) else 1)"; } # numeric test, as a Python expression
wait_for() { # SECONDS CMD...: retry CMD until it succeeds
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
face_avatar() { # DISTANCE PITCH: third person, facing the avatar
    ctl view third "$1" > /dev/null
    ctl turn "$(python3 -c "print($(ctl avatar | js bodyYaw) + 180)")" "$2" > /dev/null
}

# 3D and the frames use the focused monitor (--monitor focuses that one first)
if [[ -n "$MONITOR" ]]; then
    hyprctl -j monitors | python3 -c 'import json, sys; sys.exit(0 if any(m["name"] == sys.argv[1] for m in json.load(sys.stdin)) else 1)' "$MONITOR" ||
        die "no monitor $MONITOR (hyprctl monitors lists them)"
    hyprctl dispatch focusmonitor "$MONITOR" > /dev/null
    sleep 0.3
fi
read -r MON MON_W MON_H MON_HZ MON_SCALE MON_X MON_Y MON_ID <<< "$(hyprctl -j monitors | python3 -c 'import json, sys
m = next(m for m in json.load(sys.stdin) if m["focused"])
print(m["name"], m["width"], m["height"], m["refreshRate"], m["scale"], m["x"], m["y"], m["id"])')"
shot() { # NAME: frames/NN-NAME.png, raw/NAME.ppm to compare
    N=$((N + 1))
    grim -o "$MON" -t ppm "$OUT/raw/$1.ppm" && "${UTIL[@]}" png "$OUT/raw/$1.ppm" "$OUT/frames/$(printf %02d $N)-$1.png"
}
# two shots a second apart: what differs between them changes on its own (a clock) and comparisons skip it
desktop_shot() {
    shot "$1"
    sleep 1
    grim -o "$MON" -t ppm "$OUT/raw/$1-2.ppm"
    SAME+=(--same "$OUT/raw/$1.ppm" "$OUT/raw/$1-2.ppm")
}
SAME=()
# box of the terminal this runs in (an ancestor process's window), in frame pixels: its scrolling output is left out
TERM_BOX="$(hyprctl -j clients | python3 -c 'import json, sys
pid, mon, mx, my, scale = int(sys.argv[1]), int(sys.argv[2]), float(sys.argv[3]), float(sys.argv[4]), float(sys.argv[5])
up = set()
while pid > 1 and pid not in up:
    up.add(pid)
    try:
        pid = int(open(f"/proc/{pid}/stat").read().rsplit(")", 1)[1].split()[1])
    except OSError:
        break
for c in json.load(sys.stdin):
    if c.get("pid") in up and c.get("monitor") == mon and c.get("mapped"):
        x, y = (c["at"][0] - mx) * scale, (c["at"][1] - my) * scale
        print(int(x) - 4, int(y) - 4, int(c["size"][0] * scale) + 8, int(c["size"][1] * scale) + 8)
        break' "$$" "$MON_ID" "$MON_X" "$MON_Y" "$MON_SCALE")"
MASK=()
[[ -n "$TERM_BOX" ]] && MASK=(--mask $TERM_BOX)
differs() { "${UTIL[@]}" diff "$OUT/raw/$1.ppm" "$OUT/raw/$2.ppm" "$OUT/frames/diff-$1-$2.png" "${SAME[@]}" "${MASK[@]}"; }
calm() { hyprctl dismissnotify > /dev/null 2>&1; sleep 1.2; } # no notifications in the next frame
say() {                                                       # what to do, also over the 3D view
    echo ">>> $1"
    hyprctl notify 1 "${2:-2500}" "rgb(33ccff)" "hyprwalk live check: $1" > /dev/null 2>&1
}

cleanup() {
    trap - EXIT INT TERM
    if ((LOADED)); then
        ctl avatar lipsync off > /dev/null
        ctl off now > /dev/null
        sleep 0.5
        ctl log > "$OUT/hyprwalk.log" # the plugin's own log; Hyprland's needs debug logs
        local r
        r="$(hyprctl plugin unload "$SO" 2>&1)"
        if [[ "$r" == ok ]]; then note "unloaded the plugin"; else FAIL=$((FAIL + 1)); log "FAIL  unloading the plugin  [$r]"; fi
    fi
    [[ -f "$HYPRLOG" ]] && grep -a "\[hyprwalk\]" "$HYPRLOG" > "$OUT/hyprland-log.txt" && [[ ! -s "$OUT/hyprland-log.txt" ]] && rm -f "$OUT/hyprland-log.txt"
    rm -rf "$OUT/raw"
    log "$PASS passed, $FAIL failed, in $(($(date +%s) - START)) s; frames in $OUT/frames"
}
trap cleanup EXIT
trap 'echo; log "      interrupted: cleaning up"; exit 130' INT TERM

note "monitor" "$MON ${MON_W}x${MON_H} at $MON_HZ Hz, scale $MON_SCALE"
note "Hyprland" "$(hyprctl version | head -n1)"
note "left out of the desktop comparisons" "${TERM_BOX:+the terminal this runs in, at $TERM_BOX; }what changes on its own within a second"
calm
desktop_shot desktop-before

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
desktop_shot desktop-loaded
read -r d left <<< "$(differs desktop-before desktop-loaded)"
is "$d < 0.005"
check "loading it changes nothing on the screen" $? "$d of the pixels differ, $left left out: see frames/diff-*"
is "$left > 0.5" && note "most of the monitor was left out (the terminal this runs in covers it): --monitor OTHER, from a terminal on another monitor, compares all of it"

# --- 3D
r="$(ctl on)"
wait_for 5 mode_is active
check "hyprctl hyprwalk on: in 3D" $? "$r"
sleep 2
s="$(ctl status | tee "$OUT/status/3d.json")"
fps="$(js fps <<< "$s")"
is "$fps >= 0.75 * $MON_HZ"
check "it keeps up with the monitor's $MON_HZ Hz" $? "$fps frames a second"
shot 3d
read -r d left <<< "$("${UTIL[@]}" diff "$OUT/raw/desktop-loaded.ppm" "$OUT/raw/3d.ppm" "$OUT/frames/diff-desktop-loaded-3d.png")"
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
hyprctl notify 1 6000 "rgb(ff8800)" "hyprwalk live check: a notification over the 3D view" > /dev/null
sleep 1
shot notification
o="$("${UTIL[@]}" count "$OUT/raw/notification.ppm" orange 0.5 0 1 0.25)"
((o > 20))
check "Hyprland draws its notification over the 3D view" $? "$o orange pixels in the top right"
hyprctl dismissnotify > /dev/null 2>&1

# --- first person (first_person_body): the hands up low in the view
ctl view first > /dev/null
ctl spawn > /dev/null
sleep 1.5
a="$(ctl avatar | tee "$OUT/status/first-person.json")"
if [[ "$(js body <<< "$a")" == true ]]; then
    ly="$(js hands.at.0.1 <<< "$a")" ry="$(js hands.at.1.1 <<< "$a")"
    [[ "$(js hands.mode <<< "$a")" == ready && "$ly" != null && "$ry" != null ]] && is "0.5 < $ly <= 1 and 0.5 < $ry <= 1"
    check "first person from the avatar's eyes ($(js eyeHeight <<< "$a") m up): its hands up low in the view" $? "$(js hands <<< "$a")"
    shot first-person
else
    note "first person from the avatar's eyes: not with this avatar (it isn't a humanoid with a head and arms), or first_person_body is off" "$(js view <<< "$a")"
fi

# --- carrying the window under the crosshair, if any
ctl view first > /dev/null
ctl spawn > /dev/null
sleep 1
kind="$(ctl status | js aimed.kind)"
if [[ "$kind" != window ]]; then # else aim at the nearest window
    r="$(ctl aim)"
    sleep 0.8
    note "the crosshair started on ${kind/null/nothing}: turned to the window nearest to it" "$r; now on $(ctl status | js aimed.kind)"
    kind="$(ctl status | js aimed.kind)"
fi
if [[ "$kind" == window ]]; then
    r="$(ctl grab)"
    [[ "$r" == holding && "$(ctl status | js holding)" == true ]]
    check "hyprctl hyprwalk grab (G): the window under the crosshair, picked up" $? "$r"
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
    note "no window to aim at: carrying one skipped" "$kind"
fi

# --- your apps (--app)
playing() { [[ "$(ctl status | js playing)" != null ]]; }
walking() { [[ "$(ctl status | js playing)" == null && "$(ctl status | js typing)" == false ]]; }
typing() { [[ "$(ctl status | js typing)" == true && "$(ctl status | js playing)" == null ]]; }
cursor_on() { [[ "$(ctl status | js cursor)" != null ]]; }
pinned() { ctl windows | python3 -c 'import json, sys; sys.exit(0 if any(p["address"] == sys.argv[1] and p["pinned"] for p in json.load(sys.stdin)["placed"]) else 1)' "$1"; }
held() { ctl windows | python3 -c 'import json, sys; sys.exit(0 if any(p["address"] == sys.argv[1] and p["held"] for p in json.load(sys.stdin)["placed"]) else 1)' "$1"; }
put_down() { # placed, not held, and nothing pinned
    ctl windows | python3 -c 'import json, sys
placed = json.load(sys.stdin)["placed"]
sys.exit(0 if any(p["address"] == sys.argv[1] and not p["held"] for p in placed) and not any(p["pinned"] for p in placed) else 1)' "$1"
}
where() { # distance, apparent size (1 = as in 2D), held, pinned
    ctl windows | python3 -c 'import json, sys
print("; ".join("{class}: {distance:.2f} m, looks {apparent:.2f}, held {held}, pinned {pinned}".format(**p) for p in json.load(sys.stdin)["placed"] if p["address"] == sys.argv[1]))' "$1"
}
ask() { # MSG STEP SECONDS: notify that long, run CHECK_DO
    say "$1" "$(($3 * 1000))"
    [[ -n "${CHECK_DO:-}" ]] && $CHECK_DO "$2" "$ADDR" > /dev/null 2>&1 & # tools/test/vm does the step with hyprctl
}
for app in "${APPS[@]}"; do
    ctl view first > /dev/null
    before="$(hyprctl -j clients | python3 -c 'import json, sys; print(" ".join(c["address"] for c in json.load(sys.stdin)))')"
    r="$(ctl launch "$app")"
    newwin() {
        # address and class of the app's new window: the one hyprwalk placed as launched from 3D, else the first new one;
        # not Steam's own windows, which open first when Steam wasn't running
        hyprctl -j clients | python3 -c 'import json, subprocess, sys
old, steam = set(sys.argv[1].split()), "steam://rungameid/" in sys.argv[2]
placed = {p["address"] for p in json.loads(subprocess.run(["hyprctl", "hyprwalk", "windows"], capture_output=True, text=True).stdout or "{}").get("placed", [])}
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
    # a window not known as launched from 3D (a game Steam starts) opens on the wall: bring it here
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
    ask "press H pointing at $CLS: you pick it up; walk a few steps and look where it should go" carry 45
    wait_for 45 held "$ADDR"
    check "H: $CLS picked up, carried where you look" $? "$(where "$ADDR")"
    sleep 5
    shot "app-$CLS-carried"
    ask "press H again: $CLS goes where you look (a wall, or the air in front of you)" place 45
    wait_for 45 put_down "$ADDR"
    check "H again: $CLS put down where you look, carried no more" $? "$(where "$ADDR")"
    shot "app-$CLS-put-down"
    ask "press Shift+H pointing at $CLS: it pins to your view; look around a bit" pin 45
    wait_for 45 pinned "$ADDR"
    check "Shift+H: pinned to the view" $?
    sleep 5
    shot "app-$CLS-pinned"
    ask "walk a few steps, press Shift+H again (it's back in your hands, as big as it was), look where it should go and press H" unpin 60
    wait_for 60 put_down "$ADDR"
    check "Shift+H again, then H: $CLS out of the view's corner, put down where you look, and nothing else pinned" $? "$(where "$ADDR")"
    shot "app-$CLS-put-down-again"
    pinned "$ADDR" && ctl window "$ADDR" unpin > /dev/null
    [[ "$(ctl status | js holding)" == true ]] && ctl place > /dev/null
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
    check "lip sync on: listening" $? "$(ctl avatar lipsync | js text)"
    face_avatar 1.0 -3 # close up on the mouth for the frames below
    sleep 2.5 # microphone problems show after 2 s
    ctl avatar lipsync > "$OUT/status/lipsync.json"
    # PipeWire's view of the microphone: which one, what lip sync's stream is linked to, muted (records no sound)
    mkdir -p "$OUT/audio"
    wpctl status > "$OUT/audio/wpctl-status.txt" 2>&1
    wpctl inspect @DEFAULT_AUDIO_SOURCE@ > "$OUT/audio/wpctl-inspect-default-source.txt" 2>&1
    pw-dump > "$OUT/audio/pw-dump.json" 2> /dev/null
    while IFS= read -r line; do note "${line%%|*}" "${line#*|}"; done < <(python3 - "$OUT/status/lipsync.json" "$OUT/audio/pw-dump.json" << 'EOF'
import json, sys
ls = json.load(open(sys.argv[1]))
try:
    dump = json.load(open(sys.argv[2]))
except (OSError, ValueError):
    dump = []
props = {o['id']: (o.get('info') or {}).get('props') or {} for o in dump if o.get('type', '').endswith(':Node')}
ours = [i for i, p in props.items() if p.get('node.name') == 'hyprwalk-lipsync']
links = [o['info'] for o in dump if o.get('type', '').endswith(':Link') and ours and (o.get('info') or {}).get('input-node-id') == ours[0]]
src = ls.get('source') or {}
print(f"lip sync's badge|{ls.get('text')} ({ls.get('problem')})")
print(f"its stream|{ls.get('stream')}{', ' + ls['error'] if ls.get('error') else ''}; linked: {ls.get('linked')}; asked for: {ls.get('target') or 'the default source'}")
if src:
    print(f"linked to|{src.get('description')} ({src.get('name')}, node {src.get('id')}), PipeWire has it {src.get('state')}, "
          f"muted: {src.get('muted')}, volume {src.get('volume')}")
print(f"pw-dump's links into it|{', '.join(props.get(l.get('output-node-id'), {}).get('node.name', '?') + ' (' + str(l.get('state')) + ')' for l in links) or 'none'}")
print(f"what came|{ls.get('samples')} samples in {ls.get('buffers')} buffers ({ls.get('emptyBuffers')} flagged empty); exact zeros for the last "
      f"{ls.get('silentFor')} s; the last second's peak {ls.get('peak')} dBFS, RMS {ls.get('rms')} dBFS")
print(f"the sources there are|{'; '.join(s['description'] + ' (' + s['name'] + ')' for s in ls.get('sources', []))}")
EOF
)
    calm # no notifications, so the badge stays put
    shot lipsync-badge
    # the badge sits below Hyprland's notifications, however many there are
    box="$(ctl avatar lipsync | python3 -c 'import json, subprocess, sys
b = json.load(sys.stdin)["badge"] or [0, 0, 0, 0]
m = next(m for m in json.loads(subprocess.run(["hyprctl", "-j", "monitors"], capture_output=True, text=True).stdout) if m["focused"])
print((b[0] - 4) / m["width"], (b[1] - 4) / m["height"], (b[0] + b[2] + 4) / m["width"], (b[1] + b[3] + 4) / m["height"])')"
    red="$("${UTIL[@]}" count "$OUT/raw/lipsync-badge.ppm" red $box)"
    ((red > 20))
    check "its badge in the top right corner" $? "$red red pixels; at $(ctl avatar lipsync | js badge)"
    : > "$OUT/lipsync.jsonl"
    listen() { # MSG NAME: 4 s of lip sync, frame at the loudest
        say "$1" 4800
        [[ -n "${CHECK_SAY:-}" ]] && $CHECK_SAY "$2" & # tools/test/vm sings into its test microphone
        sleep 0.8
        local end=$((SECONDS + 4)) loudest=-60 heard level grabbed=""
        while ((SECONDS < end)); do
            heard="$(ctl avatar lipsync)"
            printf '{"say": "%s", "at": %s, "heard": %s}\n' "$2" "$(date +%s.%N)" "$heard" >> "$OUT/lipsync.jsonl"
            [[ "$heard" =~ \"level\":\ (-?[0-9.]+) ]] && level="${BASH_REMATCH[1]}" || level=-120
            if awk -v a="$level" -v b="$loudest" 'BEGIN {exit !(a > b + 1)}'; then # loudest yet: grab this frame
                loudest="$level"
                [[ -n "$grabbed" ]] && wait "$grabbed"
                grim -o "$MON" -t ppm "$OUT/raw/lipsync-$2.ppm" &
                grabbed=$!
            fi
            sleep 0.12
        done
    }
    # lip sync's Japanese vowels, spelled as English sounds (the letters' names would be other sounds)
    listen 'hold "ahhh", as in "father", from now until this goes away' a
    listen 'hold "eeee", as in "see", from now until this goes away' i
    listen 'hold "oooo", as in "food", from now until this goes away' u
    listen 'hold "ehhh", as in "bed", from now until this goes away' e
    listen 'hold "ohhh", as in "go", from now until this goes away' o
    listen 'hold "sssss" from now until this goes away' s
    listen "stay quiet until this goes away" quiet
    wait
    for v in a i u e o s quiet; do # PNG conversion is slow: done after listening
        N=$((N + 1))
        [[ -f "$OUT/raw/lipsync-$v.ppm" ]] && "${UTIL[@]}" png "$OUT/raw/lipsync-$v.ppm" "$OUT/frames/$(printf %02d $N)-lipsync-$v.png"
    done
    # judge each by its upper quartile, so a sound held for most of the prompt still counts
    heard="$(python3 - "$OUT/lipsync.jsonl" << 'EOF'
import json, sys
rows = [json.loads(l) for l in open(sys.argv[1])]
names = ['aa', 'ih', 'ou', 'ee', 'oh']
want = {'a': 'aa', 'i': 'ih', 'u': 'ou', 'e': 'ee', 'o': 'oh'}


def top(xs):
    xs = sorted(xs)
    return xs[len(xs) * 3 // 4] if xs else 0


zeros = []
for say in ['a', 'i', 'u', 'e', 'o', 's', 'quiet']:
    heard = [r['heard'] for r in rows if r['say'] == say]
    if not heard:
        continue
    loud = {n: top([h['visemes'][n] for h in heard]) for n in names}
    level = top([h['level'] for h in heard])
    peak = max((h.get('peak') or -200) for h in heard)
    if all(h['level'] <= -150 or (h.get('silentFor') or 0) >= 0.5 for h in heard):
        zeros.append(say)
    last = heard[-1]
    info = (' '.join(f'{n} {loud[n]:.2f}' for n in names) + f'; level {level:.0f} dBFS at its loudest, peak {peak:.0f}; gain {last.get("gain", 0):+.0f} dB'
            f' ({last.get("gainSetting")}), the room {last.get("room")} dBFS; {last.get("text")}')
    if say in want:
        best = max(names, key=lambda n: loud[n])
        ok = best == want[say] and loud[best] > 0.3
        print(f"{'ok  ' if ok else 'FAIL'}  you held {say}: {best} the most, the mouth {loud[best]:.2f} open at its loudest  [{info}]")
    else:
        most = top([max(h['visemes'].values()) for h in heard])
        ok = most < 0.15
        print(f"{'ok  ' if ok else 'FAIL'}  {'sss' if say == 's' else 'silence'}: the mouth about shut  [{most:.2f} at most, most of the time; {info}]")
if any(z in want for z in zeros):
    print(f"FAIL  your microphone sent only silence (exact zeros) for {', '.join(zeros)}: is it muted? (its own mute button, "
          "a light on it that turns red, silences it where PipeWire can't see)")
EOF
)"
    log "$heard"
    PASS=$((PASS + $(grep -c '^ok' <<< "$heard"))) FAIL=$((FAIL + $(grep -c '^FAIL' <<< "$heard")))
    r="$(ctl avatar lipsync off)"
    [[ "$(ctl avatar lipsync | js listening)" == false ]]
    check "lip sync off: not listening" $? "$(js on <<< "$r")"
fi

# --- out of 3D, and the plugin out
ctl view first > /dev/null
r="$(ctl off)"
wait_for 5 mode_is off
check "hyprctl hyprwalk off: out of 3D" $? "$r"
calm
desktop_shot desktop-after-3d
read -r d left <<< "$(differs desktop-loaded desktop-after-3d)"
is "$d < 0.005"
check "the desktop looks as it did before 3D" $? "$d of the pixels differ, $left left out: see frames/diff-*"
ctl log > "$OUT/hyprwalk.log"
r="$(hyprctl plugin unload "$SO" 2>&1)"
[[ "$r" == ok ]] && LOADED=0
[[ "$r" == ok ]] && ! hyprctl plugin list | grep -q hyprwalk
check "hyprctl plugin unload" $? "$r"
calm
desktop_shot desktop-unloaded
read -r d left <<< "$(differs desktop-before desktop-unloaded)"
is "$d < 0.005"
check "the desktop looks as it did before the plugin" $? "$d of the pixels differ, $left left out: see frames/diff-*"
((FAIL == 0))
