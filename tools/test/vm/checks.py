# checks.py: the checklist tools/test/vm/run.sh runs in the VM of vm.nix, through nixos-test-driver (its `machine`).
#
# hypr3d.so goes into a real Hyprland 0.55 on a virtio GPU: first with a Lua config (hyprctl plugin load), then with
# hl.plugin.load in the Lua config, then with a classic hyprland.conf (plugin = ..., the hypr3d:toggle and
# hypr3d:menu dispatchers). Keys, the mouse (relative, a PS/2 mouse), the tablet (absolute) and the wheel come from
# the VM's own input devices through QMP's input-send-event, so they pass through the kernel, libinput and
# Hyprland's input stack to the plugin's listeners and function hooks. Frames come from grim inside the VM. Lip sync
# listens to PipeWire's default source, a virtual "Test microphone" that pw-cat sings test vowels into.
#
# Each check passes or fails on its own; a section that throws fails as a whole and the rest go on. H3D_OUT gets
# results.txt (a line per check), results.json, frames/ (numbered in order) and logs/.
import datetime as dt
import json
import os
import re
import shlex
import subprocess
import time
import traceback
from pathlib import Path

IN = Path(os.environ["H3D_IN"])  # what run.sh put together, copied to H in the VM
OUT = Path(os.environ["H3D_OUT"])
FRAMES, LOGS, RAW = OUT / "frames", OUT / "logs", OUT / "raw"
HOME = "/home/alice"
H = f"{HOME}/h3d"
CFG = f"{HOME}/.config/hypr"
SO = f"{H}/hypr3d.so"
AV = f"{H}/BoothAccessories.glb"
TOON = f"{H}/ToonTest.glb"
ROOM = f"{H}/TestRoom.glb"
VOWELS = ["a", "i", "u", "e", "o"]
VISEMES = ["aa", "ih", "ou", "ee", "oh"]
for d in (FRAMES, LOGS, RAW):
    d.mkdir(parents=True, exist_ok=True)


def secs(s):
    return dt.timedelta(seconds=s)


# ------------------------------------------------------------------ results

RESULTS = []


def check(item, what, ok, detail=""):
    ok = bool(ok)
    RESULTS.append({"item": item, "check": what, "ok": ok, "detail": str(detail)})
    line = f"{'ok  ' if ok else 'FAIL'} {item:<5} {what}" + (f"  [{detail}]" if str(detail) else "")
    print(line, flush=True)
    with open(OUT / "results.txt", "a", encoding="utf-8") as f:
        f.write(line + "\n")
    return ok


def note(item, what, detail=""):
    """something measured, not passed or failed"""
    line = f"     {item:<5} {what}" + (f"  [{detail}]" if str(detail) else "")
    RESULTS.append({"item": item, "check": what, "ok": None, "detail": str(detail)})
    print(line, flush=True)
    with open(OUT / "results.txt", "a", encoding="utf-8") as f:
        f.write(line + "\n")


# ------------------------------------------------------------------ the VM's shell

WL = ["wayland-1"]


def as_alice(cmd, timeout=60):
    env = f"XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY={WL[0]} HOME={HOME}"
    return machine.execute(f"runuser -u alice -- env {env} bash -c {shlex.quote(cmd)}", timeout=secs(timeout))


def alice(cmd, timeout=60):
    status, out = as_alice(cmd, timeout)
    if status != 0:
        raise RuntimeError(f"{cmd!r} exited {status}: {out.strip()[-400:]}")
    return out


def ctl(*args, timeout=30):
    """hyprctl, as alice: what it says"""
    return as_alice("hyprctl -i 0 " + " ".join(shlex.quote(str(a)) for a in args), timeout)[1].strip()


def ctlj(*args):
    out = ctl(*args)
    try:
        return json.loads(out)
    except json.JSONDecodeError:
        raise RuntimeError(f"hyprctl {' '.join(map(str, args))}: not JSON: {out[:300]!r}")


def st():
    return ctlj("hypr3d", "status")


def av():
    return ctlj("hypr3d", "avatar")


def menu():
    return ctlj("hypr3d", "menu")


def lipsync():
    return ctlj("hypr3d", "avatar", "lipsync")


def wait_for(what, fn, timeout=20, every=0.1):
    end, last = time.time() + timeout, None
    while time.time() < end:
        try:
            last = fn()
            if last:
                return last
        except Exception as e:  # noqa: BLE001 (not up yet)
            last = e
        time.sleep(every)
    raise TimeoutError(f"waiting for {what}: last {last!r}"[:600])


def copy_out(src, sub):
    """a file in the VM to OUT/sub"""
    if hasattr(machine, "copy_from_machine"):
        machine.copy_from_machine(src, sub)
    else:
        machine.copy_from_vm(src, sub)


# ------------------------------------------------------------------ input, through QMP

def qmp(events):
    machine.qmp_client.send("input-send-event", {"events": events})


def key_event(name, down):
    return {"type": "key", "data": {"down": down, "key": {"type": "qcode", "data": name}}}


def press(*names, hold=0.08, after=0.3):
    """keys down in order, up the other way round (QEMU's qcodes: tab, esc, ret, meta_l, shift_r, f1, grave_accent)"""
    for n in names:
        qmp([key_event(n, True)])
        time.sleep(0.03)
    time.sleep(hold)
    for n in reversed(names):
        qmp([key_event(n, False)])
        time.sleep(0.03)
    time.sleep(after)


def rel(dx, dy, after=0.3):
    """the PS/2 mouse: counts"""
    qmp([{"type": "rel", "data": {"axis": "x", "value": int(dx)}}, {"type": "rel", "data": {"axis": "y", "value": int(dy)}}])
    time.sleep(after)


def tablet(x, y, after=0.3):
    """the USB tablet: 0..32767 across the screen"""
    qmp([{"type": "abs", "data": {"axis": "x", "value": int(x)}}, {"type": "abs", "data": {"axis": "y", "value": int(y)}}])
    time.sleep(after)


def click(button, hold=0.08, after=0.3):
    qmp([{"type": "btn", "data": {"down": True, "button": button}}])
    time.sleep(hold)
    qmp([{"type": "btn", "data": {"down": False, "button": button}}])
    time.sleep(after)


def wheel(notches):
    """> 0 down (away from you), a notch at a time"""
    for _ in range(abs(notches)):
        click("wheel-down" if notches > 0 else "wheel-up", hold=0.03, after=0.15)
    time.sleep(0.2)


# ------------------------------------------------------------------ frames

FRAME_N = [0]


class Img:
    """a PPM (grim -t ppm): width, height, RGB bytes"""

    def __init__(self, ppm):
        m = re.match(rb"P6\s+(\d+)\s+(\d+)\s+(\d+)\s", ppm)
        self.w, self.h = int(m.group(1)), int(m.group(2))
        self.px = ppm[m.end():]

    def count(self, pred, box=None, step=1):
        x0, y0, x1, y1 = box or (0, 0, self.w, self.h)
        p, w, n = self.px, self.w, 0
        for y in range(max(0, y0), min(self.h, y1), step):
            row = y * w * 3
            for x in range(max(0, x0), min(w, x1), step):
                i = row + x * 3
                if pred(p[i], p[i + 1], p[i + 2]):
                    n += 1
        return n

    def differs(self, other, box=None, thresh=32, step=2):
        """the fraction of pixels (every step-th) whose largest channel difference is over thresh"""
        x0, y0, x1, y1 = box or (0, 0, self.w, self.h)
        a, b, w, n, tot = self.px, other.px, self.w, 0, 0
        for y in range(y0, y1, step):
            row = y * w * 3
            for x in range(x0, x1, step):
                i = row + x * 3
                tot += 1
                if abs(a[i] - b[i]) > thresh or abs(a[i + 1] - b[i + 1]) > thresh or abs(a[i + 2] - b[i + 2]) > thresh:
                    n += 1
        return n / max(tot, 1)


def frame(name):
    """what's on the screen, through grim; saved as frames/NN-name.png"""
    FRAME_N[0] += 1
    alice("grim -t ppm /tmp/frame.ppm")
    copy_out("/tmp/frame.ppm", "raw")
    raw = (RAW / "frame.ppm").read_bytes()
    img = Img(raw)
    png = FRAMES / f"{FRAME_N[0]:02d}-{name}.png"
    try:
        r = subprocess.run(["pnmtopng"], input=raw, capture_output=True, timeout=60)
        if r.returncode != 0:
            raise RuntimeError(r.stderr.decode(errors="replace"))
        png.write_bytes(r.stdout)
    except Exception:  # noqa: BLE001
        png.with_suffix(".ppm").write_bytes(raw)
    return img


def calm_frame(name):
    """a frame without Hyprland's notifications over it"""
    ctl("dismissnotify")
    time.sleep(1.2)
    return frame(name)


# ------------------------------------------------------------------ Hyprland

BASE_LUA = r'''
hl.monitor({ output = "", mode = "preferred", position = "auto", scale = 1 })
hl.config({
    general = { gaps_in = 5, gaps_out = 20, border_size = 3, layout = "dwindle",
        col = { active_border = { colors = { "rgba(33ccffee)", "rgba(00ff99ee)" }, angle = 45 },
                inactive_border = "rgba(595959aa)" } },
    decoration = { rounding = 14, active_opacity = 0.85, inactive_opacity = 0.85,
        shadow = { enabled = true, range = 8, render_power = 3, color = 0xee1a1a1a },
        blur = { enabled = true, size = 6, passes = 2 } },
    animations = { enabled = false },
    misc = { force_default_wallpaper = 0, disable_splash_rendering = true, disable_autoreload = true },
    debug = { disable_logs = false, enable_stdout_logs = true }, -- (standard output: the journal, at once)
    ecosystem = { no_update_news = true, no_donation_nag = true },
})
-- the plugin's Lua functions, bound to keys (they're looked up when the key is pressed)
hl.bind("SUPER + grave", function() hl.plugin.hypr3d.toggle() end)
hl.bind("SUPER + M", function() hl.plugin.hypr3d.menu() end)
-- shortcuts that must reach Hyprland in 3D (Super, Ctrl+Alt), and one that mustn't (a plain key)
hl.bind("SUPER + Y", hl.dsp.exec_cmd("touch /tmp/h3d-super-y"))
hl.bind("CTRL + ALT + Y", hl.dsp.exec_cmd("touch /tmp/h3d-ctrl-alt-y"))
hl.bind("Y", hl.dsp.exec_cmd("touch /tmp/h3d-plain-y"))
'''

BASE_CONF = r'''
monitor = , preferred, auto, 1
general {
    gaps_in = 5
    gaps_out = 20
    border_size = 3
    col.active_border = rgba(33ccffee) rgba(00ff99ee) 45deg
    col.inactive_border = rgba(595959aa)
    layout = dwindle
}
decoration {
    rounding = 14
    active_opacity = 0.85
    inactive_opacity = 0.85
    shadow {
        enabled = true
        range = 8
        render_power = 3
        color = rgba(1a1a1aee)
    }
    blur {
        enabled = true
        size = 6
        passes = 2
    }
}
animations {
    enabled = false
}
misc {
    force_default_wallpaper = 0
    disable_splash_rendering = true
    disable_autoreload = true
}
debug {
    disable_logs = false
    enable_stdout_logs = true
}
ecosystem {
    no_update_news = true
    no_donation_nag = true
}
bind = SUPER, grave, hypr3d:toggle
bind = SUPER, M, hypr3d:menu
bind = SUPER, O, hypr3d:menu, options
'''


def lua_value(v):
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, (int, float)):
        return repr(v)
    return json.dumps(v)  # a string: Lua reads JSON's escapes


def lua_config(plugin=None, load=False):
    text = BASE_LUA
    if load:
        text += f'hl.plugin.load("{SO}")\n'
    if plugin:
        text += "hl.config({ plugin = { hypr3d = {\n" + "".join(f"    {k} = {lua_value(v)},\n" for k, v in plugin.items()) + "} } })\n"
    return text


def conf_config(plugin=None, load=True):
    text = BASE_CONF
    if load:
        text += f"plugin = {SO}\n"
    if plugin:
        text += "plugin {\n    hypr3d {\n" + "".join(
            f"        {k} = {('true' if v else 'false') if isinstance(v, bool) else v}\n" for k, v in plugin.items()) + "    }\n}\n"
    return text


def write_config(name, text):
    machine.succeed(f"mkdir -p {CFG} && cat > {CFG}/{name}.tmp << 'H3D_EOF'\n{text}\nH3D_EOF\nmv {CFG}/{name}.tmp {CFG}/{name} && chown -R alice:users {HOME}/.config")


def reload_config(name, text):
    write_config(name, text)
    r = ctl("reload")
    time.sleep(0.8)
    return r


HYPR = {"pid": "", "config": "", "session": 0}


def hypr_pid():
    """Hyprland's process (Nix wraps it: its name is .Hyprland-wrapped, cut to .Hyprland-wrapp)"""
    return machine.execute("ps -u alice -o pid=,comm= | awk '$2 ~ /Hyprland/ {print $1; exit}'")[1].strip()


def alive():
    return bool(HYPR["pid"]) and hypr_pid() == HYPR["pid"] and as_alice("hyprctl -i 0 version", 15)[0] == 0


def config_ok(errs):
    """hyprctl configerrors found nothing"""
    return errs.strip().lower() in ("", "no errors", "ok")


def segfaults():
    return int(machine.execute("journalctl -k --no-pager | grep -c 'segfault at' || true")[1].strip() or 0)


def stop_hyprland():
    """Hyprland 0.55.2 itself dies in its exit path when windows are still open (CCompositor::cleanup ->
    CWindow::unmapWindow -> CDwindleAlgorithm::calculateWorkspace -> ITarget::setPositionGlobal, a null pointer; with
    or without hypr3d), so the terminals go first. A crash now is a crash with the plugin (if it's loaded)."""
    if not HYPR["pid"]:
        return
    plugin = "hypr3d" in ctl("plugin", "list")
    machine.execute("pkill -u alice foot || true")
    time.sleep(1.5)
    n = segfaults()
    machine.execute("systemctl stop hyprland")
    time.sleep(2)
    if plugin:
        check("exit", f"Hyprland exits without crashing, hypr3d loaded (session {HYPR['session']})", segfaults() == n)
    HYPR["pid"] = ""


def start_hyprland(name, text):
    """Hyprland with this config, as alice's session on tty1, and two terminals"""
    stop_hyprland()
    machine.execute("systemctl stop hyprland; pkill -u alice foot || true")
    time.sleep(1)
    write_config(name, text)
    machine.succeed(f"echo H3D_CONFIG={CFG}/{name} > /run/hyprland-test.env")
    machine.succeed("systemctl start hyprland")
    HYPR["session"] += 1
    HYPR["config"] = name
    wait_for("Hyprland's socket", lambda: as_alice("hyprctl -i 0 version", 10)[0] == 0, 60, 0.5)
    inst = json.loads(alice("hyprctl -j instances"))
    WL[0] = inst[0]["wl_socket"]
    HYPR["pid"] = hypr_pid()
    wait_for("the monitor", lambda: json.loads(ctl("-j", "monitors")), 20)
    # (by app id: the shell sets their titles)
    alice("setsid -f foot --app-id h3d-left > /dev/null 2>&1; sleep 0.7; setsid -f foot --app-id h3d-right > /dev/null 2>&1")
    wait_for("two terminals", lambda: len(json.loads(ctl("-j", "clients"))) >= 2, 20)
    time.sleep(1.5)


def crash_reports():
    return machine.execute(f"ls {HOME}/.cache/hyprland/ 2>/dev/null | grep -i crash || true")[1].split()


# ------------------------------------------------------------------ helpers for the plugin

def ensure_plugin():
    if "hypr3d" not in ctl("plugin", "list"):
        r = ctl("plugin", "load", SO)
        if r != "ok":
            raise RuntimeError(f"plugin load: {r}")
        time.sleep(1.0)


def ensure_3d(on=True):
    mode = st()["mode"]
    if on and mode != "active":
        if mode == "exiting":
            wait_for("2D", lambda: st()["mode"] == "off", 10)
        ctl("hypr3d", "on")
        wait_for("3D", lambda: st()["mode"] == "active", 15)
    elif not on and mode != "off":
        ctl("hypr3d", "off")
        wait_for("2D", lambda: st()["mode"] == "off", 15)


def ensure_avatar(path, timeout=90):
    a = av()
    if a.get("path") != path:
        r = ctl("hypr3d", "avatar", path)
        if r.startswith("error"):
            raise RuntimeError(f"avatar {path}: {r}")
    return wait_for(f"avatar {path}", lambda: (lambda a: a if a.get("path") == path and not a.get("loading") and "name" in a else None)(av()), timeout, 0.25)


def menu_closed():
    if menu().get("open"):
        ctl("hypr3d", "menu", "close")


def face_avatar(dist=2.2, pitch=-6.0, settle=1.2):
    """third person, the camera in front of the avatar looking at it"""
    ctl("hypr3d", "view", "third", dist)
    a = av()
    ctl("hypr3d", "turn", f"{a['bodyYaw'] + 180:.1f}", pitch)
    time.sleep(settle)


def sing(wav, shot=None, skip=0.6, n=8):
    """lip sync's readings while pw-cat plays wav/NAME_long.wav into the test microphone, from `skip` seconds on"""
    alice(f"setsid -f pw-cat -p --target test_mic_in {H}/wav/{wav}_long.wav > /dev/null 2>&1")
    t0 = time.time()
    time.sleep(skip)
    reads = []
    while len(reads) < n and time.time() - t0 < 5.5:  # it plays for 6.4 s
        reads.append(lipsync())
        time.sleep(0.12)
    if shot:
        frame(shot)
    machine.execute("pkill -x pw-cat; true")
    time.sleep(0.6)
    return reads


def lipsync_node():
    """the plugin's PipeWire stream, and whether a link feeds it from the test microphone"""
    dump = json.loads(alice("pw-dump"))
    nodes = {o["id"]: (o.get("info") or {}).get("props") or {} for o in dump if o.get("type") == "PipeWire:Interface:Node"}
    ours = [i for i, p in nodes.items() if p.get("node.name") == "hypr3d-lipsync"]
    if not ours:
        return None
    linked = [o for o in dump if o.get("type") == "PipeWire:Interface:Link" and (o.get("info") or {}).get("input-node-id") == ours[0]]
    fed_by = [nodes.get((l.get("info") or {}).get("output-node-id"), {}).get("node.name") for l in linked]
    return {"id": ours[0], "props": nodes[ours[0]], "fed_by": fed_by}


# ------------------------------------------------------------------ sections

SECTIONS = []


def section(item, title):
    def deco(fn):
        SECTIONS.append((item, title, fn))
        return fn
    return deco


BEFORE = {}  # frames to compare with
LUA_CFG = {}  # the plugin's values in the Lua config, once section 9 is done


@section("0", "a Lua-config Hyprland, and the plugin loaded with hyprctl")
def s_start():
    start_hyprland("hyprland.lua", lua_config())
    errs = ctl("configerrors")
    check("0", "the Lua config has no errors", config_ok(errs), errs[:200])
    mons = json.loads(ctl("-j", "monitors"))
    note("0", "monitor", f"{mons[0]['name']} {mons[0]['width']}x{mons[0]['height']}@{mons[0]['refreshRate']:.1f}")
    BEFORE["desktop"] = calm_frame("desktop-before-plugin")
    r = ctl("plugin", "load", SO)
    check("0", "hyprctl plugin load", r == "ok", r)
    time.sleep(1.5)
    check("0", "hyprctl plugin list names it", "hypr3d" in ctl("plugin", "list"))
    s = st()
    check("0", "hyprctl hypr3d status: off", s["mode"] == "off", s["mode"])
    check("0", "its function hooks are in (mouse motion, warps, the cursor)", all(s["hooks"].values()), s["hooks"])
    # (Hyprland writes its log file in its own time, and its standard output, the journal's, at once)
    logged = wait_for("the log line", lambda: "[hypr3d] loaded" in machine.execute("journalctl -t start-hyprland --no-pager -n 2000")[1], 10, 0.5)
    check("0", "the log says it loaded", logged)
    BEFORE["loaded"] = calm_frame("desktop-plugin-loaded")
    d = BEFORE["desktop"].differs(BEFORE["loaded"])
    check("0", "loading it changes nothing on the screen", d < 0.002, f"{d:.2%} of pixels differ")


@section("9", "config values (Lua): avatar, avatar_physics, avatar_emotes, lipsync, map")
def s_config():
    cfg = {"avatar": AV, "avatar_physics": False, "avatar_emotes": f"{H}/emotes", "lipsync": False, "map": ROOM, "map_scale": 1.0}
    r = reload_config("hyprland.lua", lua_config(cfg))
    check("9", "hyprctl reload", r == "ok", r)
    errs = ctl("configerrors")
    check("9", "the plugin's values are known (no config errors)", config_ok(errs), errs[:300])
    a = ensure_avatar(AV)
    check("9", "avatar: loaded", a.get("name") == "BoothAccessories", a.get("name"))
    check("9", "avatar_physics = false: physics off", a.get("physics") is False, a.get("physics"))
    m = wait_for("the map", lambda: (lambda m: m if not m["loading"] and m["world"] == "TestRoom" else None)(ctlj("hypr3d", "map")), 30)
    check("9", "map: TestRoom loaded", m["world"] == "TestRoom", m)
    em = wait_for("emote files", lambda: (lambda e: e if not e["loading"] and any(x["from"] == "Hands.vrma" for x in e["emotes"]) else None)(ctlj("hypr3d", "avatar", "emote")), 30)
    check("9", "avatar_emotes: the folder's Hands.vrma is an emote", any(x["from"] == "Hands.vrma" for x in em["emotes"]),
          [x["name"] for x in em["emotes"] if x["from"] == "Hands.vrma"])
    check("9", "lipsync = false: off", lipsync()["on"] is False)
    ensure_3d()
    time.sleep(1.0)
    frame("3d-testroom")
    s = st()
    check("9", "in 3D in the test room, at its hypr3d_spawn", s["world"] == "TestRoom" and abs(s["feet"][2] - 4.0) < 0.3, f"{s['world']} feet {s['feet']}")
    ensure_3d(False)

    cfg.update(avatar_physics=True, lipsync=True, map="", avatar_height=2.0)
    reload_config("hyprland.lua", lua_config(cfg))
    wait_for("the courtyard", lambda: ctlj("hypr3d", "map")["map"] == "", 20)
    a = wait_for("height 2 m", lambda: (lambda a: a if abs(a.get("height", 0) - 2.0) < 0.02 and not a["loading"] else None)(av()), 60, 0.25)
    check("9", "avatar_physics = true: physics on", a["physics"] is True, a["physics"])
    check("9", "avatar_height = 2: 2 m tall", abs(a["height"] - 2.0) < 0.02, a["height"])
    check("9", "map = \"\": back to the courtyard", ctlj("hypr3d", "map")["world"] != "TestRoom", ctlj("hypr3d", "map")["world"])
    ls = lipsync()
    check("9", "lipsync = true: on, but not listening outside 3D", ls["on"] is True and ls["listening"] is False, ls)
    check("9", "... and no PipeWire stream outside 3D", lipsync_node() is None)
    ensure_3d()
    ls = wait_for("listening", lambda: (lambda l: l if l["listening"] else None)(lipsync()), 10)
    check("9", "... listening in 3D", ls["listening"] is True, ls)
    ensure_3d(False)
    cfg.update(lipsync=False, avatar_height=0.0, avatar="")
    reload_config("hyprland.lua", lua_config(cfg))
    wait_for("no avatar", lambda: "name" not in av(), 20)
    check("9", "avatar = \"\": no avatar", "name" not in av())
    check("9", "lipsync = false: off again", lipsync()["on"] is False)

    # a value changed at run time, with no reload (hyprctl eval hl.config / hyprctl keyword)
    cfg.update(avatar=AV)
    reload_config("hyprland.lua", lua_config(cfg))
    ensure_avatar(AV)
    r = ctl("eval", 'hl.config({ plugin = { hypr3d = { avatar_physics = false } } })')
    ok = wait_for("physics off", lambda: av()["physics"] is False, 3, 0.2)
    check("9", "hyprctl eval hl.config({plugin = {hypr3d = {avatar_physics = false}}}) takes effect", ok, f"eval: {r}; physics {av()['physics']}")
    r = ctl("eval", f'hl.config({{ plugin = {{ hypr3d = {{ avatar = "{TOON}" }} }} }})')
    ok = wait_for("the other avatar", lambda: av().get("name") == "ToonTest", 10, 0.2)
    check("9", "... and so does a new avatar", ok, f"eval: {r}; {av().get('name')}")
    reload_config("hyprland.lua", lua_config(cfg))
    LUA_CFG.update(cfg)
    time.sleep(0.5)


@section("1", "the Action Menu from the keyboard, the mouse, the tablet and the wheel")
def s_menu():
    ensure_avatar(AV)
    ensure_3d()
    menu_closed()
    tablet(16384, 16384)  # where the tablet is, before the menu (it turns the camera, from here on its moves count)
    frame("3d-courtyard")
    press("tab")
    m = menu()
    check("1", "Tab opens it", m.get("open") and m.get("path") == "main", m.get("path"))
    frame("menu-main")
    labels = [i["label"] for i in m["items"]]
    note("1", "main page", labels)
    rel(0, -160)
    h = menu()["highlight"]
    check("1", "the mouse moves its cursor: up points at Emotes", h == 1, f"highlight {h}")
    rel(160, 160)  # to the right: the second of five, clockwise from the top
    h = menu()["highlight"]
    check("1", "... right points at Expressions", h == 2, f"highlight {h}")
    frame("menu-cursor-right")
    click("left")
    p = menu().get("path")
    check("1", "a left click picks it", p == "main/expressions", p)
    click("right")
    p = menu().get("path")
    check("1", "a right click goes back", p == "main", p)
    before = menu()["highlight"]
    wheel(1)
    after = menu()["highlight"]
    check("1", "a wheel notch goes round to the next item", after == (before % 5) + 1 if before > 0 else after == 1, f"{before} -> {after}")
    wheel(-1)
    back = menu()["highlight"]
    check("1", "... and back", back == before or (before <= 0 and back >= 1), f"{after} -> {back}")
    press("3")
    p = menu().get("path")
    check("1", "3 picks the third (Gestures)", p == "main/gestures", p)
    press("backspace")
    p = menu().get("path")
    check("1", "Backspace goes back", p == "main", p)
    wheel(1)
    h = menu()["highlight"]
    press("ret")
    p = menu().get("path")
    want = {1: "main/emotes", 2: "main/expressions", 3: "main/gestures", 4: "main/outfit", 5: "main/options"}.get(h)
    check("1", "Enter picks what's highlighted", p == want, f"highlight {h}: {p}")
    press("backspace")
    # the tablet: absolute motion, turned into cursor movement from where it was (Backspace put the cursor in the middle)
    tablet(16384, 16384 - 6000)
    h = menu()["highlight"]
    check("1", "the tablet (absolute motion) moves the cursor too: up to Emotes", h == 1, f"highlight {h}")
    tablet(16384 + 7000, 16384 - 6000)
    h = menu()["highlight"]
    check("1", "... and right to Expressions", h == 2, f"highlight {h}")
    click("middle")
    m = menu()
    check("1", "a middle click closes it", m.get("open") is False, m)
    press("tab")
    press("esc")
    m, s = menu(), st()
    check("1", "Esc closes it (and stays in 3D)", m.get("open") is False and s["mode"] == "active", f"{m} {s['mode']}")
    press("tab")
    press("tab")
    check("1", "Tab again closes it", menu().get("open") is False)
    press("w", hold=0.6)
    s2 = st()
    check("1", "W walks (the menu closed)", abs(s2["feet"][0] - s["feet"][0]) + abs(s2["feet"][2] - s["feet"][2]) > 0.5, f"{s['feet']} -> {s2['feet']}")
    y0 = st()["yaw"]
    rel(200, 0)
    time.sleep(0.3)
    y1 = st()["yaw"]
    check("1", "the mouse turns the camera (menu closed)", abs(y1 - y0) > 5, f"yaw {y0} -> {y1}")
    ctl("hypr3d", "spawn")


QCODES = {" ": "spc", "/": "slash", "-": "minus", ".": "dot"}


def type_text(text):
    for ch in text:
        press(QCODES.get(ch, ch), hold=0.03, after=0.05)


def cursor_cyan(r, g, b):
    """Hyprland's cursor (a cyan drop)"""
    return r < 80 and g > 130 and b > 160


@section("1b", "in 3D: the cursor hidden, the crosshair on a window, a click focusing it, E typing into it")
def s_windows():
    ensure_avatar(AV)
    ensure_3d(False)
    # Hyprland's cursor over the left terminal, away from its border
    tablet(200 * 32767 // 1280, 700 * 32767 // 800)
    box = (180, 680, 250, 750)
    n2d = calm_frame("cursor-2d").count(cursor_cyan, box)
    ensure_3d()
    menu_closed()
    ctl("hypr3d", "view", "first")
    ctl("hypr3d", "spawn")
    time.sleep(1.0)
    n3d = frame("3d-first-person").count(cursor_cyan, box)
    check("1b", "Hyprland's cursor is hidden in 3D", n2d > 20 and n3d < 5, f"cursor pixels: 2D {n2d}, 3D {n3d}")
    pos = ctl("cursorpos")
    aimed = None
    for _ in range(40):  # turn left with the mouse until the crosshair is on the left terminal
        a = st()["aimed"]
        if a and a.get("kind") == "window" and a.get("class") == "h3d-left":
            aimed = a
            break
        rel(-15, 0, after=0.15)
    check("1b", "the mouse turns the crosshair onto the left terminal", aimed, aimed)
    check("1b", "... and doesn't move Hyprland's cursor", ctl("cursorpos") == pos, f"{pos} -> {ctl('cursorpos')}")
    frame("aim-left-window")
    click("left")
    active = json.loads(ctl("-j", "activewindow"))
    check("1b", "a left click on it focuses it", active.get("class") == "h3d-left", active.get("class"))
    machine.execute("rm -f /tmp/h3d-text")
    press("e")
    check("1b", "E: typing into it", st()["typing"] is True)
    type_text("touch /tmp/h3d-text")
    press("ret")
    time.sleep(0.8)
    check("1b", "what's typed reaches the terminal (it ran touch)", machine.execute("test -e /tmp/h3d-text")[0] == 0)
    frame("typed")
    press("meta_l", "esc")
    check("1b", "Super+Esc: walking again", st()["typing"] is False)
    ensure_3d(False)
    time.sleep(0.5)
    n2 = calm_frame("cursor-back").count(cursor_cyan, box)
    check("1b", "out of 3D the cursor shows again", n2 > 20, f"{n2} cursor pixels")
    rel(40, 0)
    check("1b", "... and the mouse moves it again", ctl("cursorpos") != pos, f"{pos} -> {ctl('cursorpos')}")
    ensure_3d()
    ctl("hypr3d", "view", "third")


@section("2", "a slider's dial and a two-axis puppet's stick, from the menu")
def s_dial():
    ensure_avatar(AV)
    ensure_3d()
    menu_closed()
    ctl("hypr3d", "avatar", "parts", "reset")
    tablet(16384, 16384)  # (menu closed: the camera turns)
    press("tab")
    for k in ("4", "8", "8", "1"):
        press(k)
    m = menu()
    check("2", "Tab, 4, More, More, 1: the dial of the outfit's first slider on its third page", m.get("path") == "main/outfit:3/~ニーハイの緩さ" and m.get("dial"), m.get("path"))
    frame("dial-open")
    rel(150, 0)
    v_mouse = menu()["dial"]["value"]
    # a mouse count is a logical pixel (libinput's unaccelerated motion), the menu 224 px across the middle at 800 px
    # high: 150 counts right from the top is atan2(150 / 224, 0.66) round. (The harness says 0.103: it moves its menu
    # before laying it out, at 300 px.)
    check("2", "the mouse turns it (clockwise from the top), 150 counts: 12.6%", abs(v_mouse - 0.126) < 0.003, f"{v_mouse:.3f}")
    wheel(2)
    v = menu()["dial"]["value"]
    want = round((v_mouse + 0.10) * 20) / 20
    check("2", "two wheel notches: 5% each", abs(v - want) < 0.001, f"{v_mouse:.3f} -> {v:.3f}, want {want:.3f}")
    frame("dial-wheel")
    press("8")
    v8 = menu()["dial"]["value"]
    press("1")
    v1 = menu()["dial"]["value"]
    press("5")
    v5 = menu()["dial"]["value"]
    check("2", "8, 1 and 5 set 100%, 0% and 57%", abs(v8 - 1) < 1e-3 and abs(v1) < 1e-3 and abs(v5 - 4 / 7) < 2e-3, f"{v8:.3f} {v1:.3f} {v5:.3f}")
    status = ctl("hypr3d", "menu")
    check("2", "hyprctl hypr3d menu shows the dial's value", '"dial": {"label": "ニーハイの緩さ", "value": 0.571}' in status, status[-90:])
    click("left")
    m = menu()
    check("2", "a click closes the dial, back to the page", m.get("dial") is None and m.get("path") == "main/outfit:3", m.get("path"))
    press("3")
    m = menu()
    check("2", "3: the two-axis puppet's stick", m.get("dial") and m["dial"]["label"] == "しっぽの向き" and isinstance(m["dial"]["value"], list), m.get("dial"))
    rel(90, -45)
    x, y = menu()["dial"]["value"]
    check("2", "the mouse moves the stick right and up: 90 and -45 counts to [0.414, 0.207]", abs(x - 0.414) < 0.004 and abs(y - 0.207) < 0.004,
          f"[{x:.3f}, {y:.3f}]")
    frame("stick")
    tablet(16384 - 2000, 16384 + 2000)
    x2, y2 = menu()["dial"]["value"]
    check("2", "the tablet moves it left and down", x2 < x - 0.05 and y2 < y - 0.05, f"[{x:.3f}, {y:.3f}] -> [{x2:.3f}, {y2:.3f}]")
    press("1")
    x3, y3 = menu()["dial"]["value"]
    check("2", "1 pushes it all the way up", abs(x3) < 1e-3 and abs(y3 - 1) < 1e-3, f"[{x3:.3f}, {y3:.3f}]")
    status = ctl("hypr3d", "menu")
    check("2", "hyprctl hypr3d menu shows both of the stick's values", '"dial": {"label": "しっぽの向き", "value": [0.000, 1.000]}' in status, status[-90:])
    press("backspace")
    press("esc")
    parts = ctlj("hypr3d", "avatar", "parts")
    sl = {s["name"]: s["value"] for s in parts["sliders"]}
    check("2", "what the dial and the stick set is kept", abs(sl["ニーハイの緩さ"] - 4 / 7) < 2e-3 and sl["しっぽの向き"] == [0.0, 1.0], sl)


@section("3", "sliders from hyprctl")
def s_sliders():
    ensure_avatar(AV)
    r = ctl("hypr3d", "avatar", "slider", "ニーハイの緩さ", "50%")
    check("3", "avatar slider ニーハイの緩さ 50%", r == "ニーハイの緩さ: 50%", r)
    r = ctl("hypr3d", "avatar", "slider", "しっぽの向き", "0.5", "-0.25")
    check("3", "avatar slider しっぽの向き 0.5 -0.25", r == "しっぽの向き: +50% -25%", r)
    sl = {s["name"]: s["value"] for s in ctlj("hypr3d", "avatar", "parts")["sliders"]}
    check("3", "avatar parts has them", sl["ニーハイの緩さ"] == 0.5 and sl["しっぽの向き"] == [0.5, -0.25], sl)
    r = ctl("hypr3d", "avatar", "slider", "しっぽの向き", "reset")
    check("3", "avatar slider NAME reset", r.startswith("しっぽの向き"), r)


def blue(r, g, b):
    return b > 140 and b > r + 45 and b > g + 30


@section("4", "a toggle that switches a material variant")
def s_variant():
    ensure_avatar(AV)
    ensure_3d()
    menu_closed()
    ctl("hypr3d", "avatar", "parts", "reset")
    ctl("hypr3d", "avatar", "physics", "off")
    face_avatar(2.0, -4)
    off = calm_frame("variant-off")
    r = ctl("hypr3d", "avatar", "toggle", "紺の制服", "on")
    check("4", "avatar toggle 紺の制服 on", r == "紺の制服: on", r)
    time.sleep(0.8)
    on = calm_frame("variant-on")
    box = (on.w * 3 // 10, on.h * 62 // 100, on.w * 55 // 100, on.h * 80 // 100)  # her top, and the skirt's (navy) waist
    n0, n1 = off.count(blue, box, 2), on.count(blue, box, 2)
    check("4", "the uniform's white top turns blue in the frame", n1 > n0 + 300, f"blue pixels (every 2nd) {n0} -> {n1}")
    v = {x["name"]: x["on"] for x in ctlj("hypr3d", "avatar", "parts")["variants"]}
    check("4", "avatar parts: the variant is on", v.get("紺の制服") is True, v)
    ctl("hypr3d", "avatar", "toggle", "紺の制服", "off")
    time.sleep(0.8)
    back = calm_frame("variant-off-again")
    n2 = back.count(blue, box, 2)
    check("4", "... and white again", n2 < n0 + 150, f"blue pixels {n2}")
    ctl("hypr3d", "avatar", "physics", "on")


@section("5", "an emote with \"speed\": 2 in the settings file")
def s_emote_speed():
    ensure_avatar(AV)
    ensure_3d()
    em = {e["name"]: e for e in ctlj("hypr3d", "avatar", "emote")["emotes"]}
    fast, slow = em.get("Hands Fast"), em.get("Hands Slow")
    check("5", "avatar emote lists it, speed 2", fast and abs(fast["speed"] - 2) < 1e-3 and slow and abs(slow["speed"] - 1) < 1e-3,
          f"{fast} / {slow}")

    def play(name):
        r = ctl("hypr3d", "avatar", "emote", name, "once")
        t0 = time.time()
        f0 = st()["frames"]
        wait_for(f"{name} to start", lambda: av()["emote"] == name, 5, 0.05)
        wait_for(f"{name} to end", lambda: av()["emote"] == "", 40, 0.05)
        t, frames = time.time() - t0, st()["frames"] - f0
        return r, t, frames

    r1, t1, f1 = play("Hands Slow")
    r2, t2, f2 = play("Hands Fast")
    fps = st()["fps"]
    note("5", "played once", f"speed 1: {t1:.2f} s ({f1} frames), speed 2: {t2:.2f} s ({f2} frames); {fps:.0f} fps")
    # a 7 s clip: it fades out over its last 0.3 s of clip time plus 0.3 s, so (7 - 0.3) / speed + 0.3
    ratio = t1 / max(t2, 1e-3)
    check("5", "twice as fast: over in about half the time", 1.75 < ratio < 2.1, f"{t1:.2f} s / {t2:.2f} s = {ratio:.2f} (7.0 / 3.65 = 1.92)")


@section("6", "F1-F8 gestures, with and without Left or Right Shift")
def s_gestures():
    ensure_avatar(AV)
    ensure_3d()
    menu_closed()

    def g():
        return av()["gestures"]

    press("f2")
    check("6", "F2: both hands a fist", g() == ["fist", "fist"], g())
    press("shift", "f3")
    check("6", "Left Shift+F3: the left hand open, the right still a fist", g() == ["open", "fist"], g())
    press("shift_r", "f4")
    check("6", "Right Shift+F4: the right hand points, the left still open", g() == ["open", "point"], g())
    press("shift", "shift_r", "f5")
    check("6", "both Shifts+F5: both victory", g() == ["victory", "victory"], g())
    face_avatar(1.6, -4)
    frame("gesture-victory")
    press("f1")
    check("6", "F1: both neutral", g() == ["neutral", "neutral"], g())


@section("7", "Super and Ctrl+Alt shortcuts reach Hyprland in 3D; plain keys don't")
def s_shortcuts():
    ensure_3d()
    menu_closed()
    machine.execute("rm -f /tmp/h3d-*")
    press("meta_l", "y")
    press("ctrl", "alt", "y")
    press("y")
    time.sleep(0.8)
    have = machine.execute("ls /tmp/h3d-* 2>/dev/null || true")[1].split()
    check("7", "Super+Y ran its bind", "/tmp/h3d-super-y" in have, have)
    check("7", "Ctrl+Alt+Y ran its bind", "/tmp/h3d-ctrl-alt-y" in have, have)
    check("7", "plain Y went to the plugin, not to its bind", "/tmp/h3d-plain-y" not in have, have)
    check("7", "... and 3D is still on", st()["mode"] == "active")
    ensure_3d(False)
    press("y")
    time.sleep(0.8)
    check("7", "out of 3D, Y runs its bind (so the bind works)", machine.execute("test -e /tmp/h3d-plain-y")[0] == 0)


@section("8", "the Lua functions and a Lua keybind")
def s_lua():
    ensure_avatar(AV)
    ensure_3d(False)
    r = ctl("eval", "hl.plugin.hypr3d.enter()")
    ok = wait_for("3D", lambda: st()["mode"] == "active", 10)
    check("8", "hl.plugin.hypr3d.enter()", r == "ok" and ok, r)
    r = ctl("eval", 'local p = hl.plugin.hypr3d.menu("emotes"); if p ~= "main/emotes" then error("gave " .. tostring(p)) end')
    check("8", 'hl.plugin.hypr3d.menu("emotes") opens that page and says so', r == "ok" and menu().get("path") == "main/emotes", r)
    r = ctl("eval", 'hl.plugin.hypr3d.menu("pick 2")')
    time.sleep(0.5)
    check("8", 'hl.plugin.hypr3d.menu("pick 2") plays the second emote', r == "ok" and av()["emote"] != "", f"{r}; emote {av()['emote']!r}")
    ctl("hypr3d", "avatar", "emote", "stop")
    r = ctl("eval", "hl.plugin.hypr3d.menu()")
    check("8", "hl.plugin.hypr3d.menu() toggles it closed", r == "ok" and menu().get("open") is False, r)
    r = ctl("eval", "hl.plugin.hypr3d.type()")
    check("8", "hl.plugin.hypr3d.type(): typing", r == "ok" and st()["typing"] is True, r)
    press("meta_l", "esc")
    check("8", "Super+Esc: walking again", st()["typing"] is False)
    r = ctl("eval", "hl.plugin.hypr3d.exit()")
    ok = wait_for("2D", lambda: st()["mode"] == "off", 10)
    check("8", "hl.plugin.hypr3d.exit()", r == "ok" and ok, r)
    r = ctl("eval", "hl.plugin.hypr3d.toggle()")
    ok = wait_for("3D", lambda: st()["mode"] == "active", 10)
    check("8", "hl.plugin.hypr3d.toggle() in", r == "ok" and ok, r)
    press("meta_l", "m")
    check("8", "Super+M (a Lua bind calling menu()) opens the menu", menu().get("open") is True)
    press("meta_l", "m")
    check("8", "... and closes it", menu().get("open") is False)
    press("meta_l", "grave_accent")
    ok = wait_for("2D", lambda: st()["mode"] == "off", 10)
    check("8", "Super+` (a Lua bind calling toggle()) leaves 3D", ok)
    press("meta_l", "grave_accent")
    ok = wait_for("3D", lambda: st()["mode"] == "active", 10)
    check("8", "... and enters it", ok)


def eyes(r, g, b):
    return g > 150 and b > 150 and r < 110


def fringe(r, g, b):
    return r > 80 and b > 60 and g < 50


def outline(r, g, b):
    return g > 140 and r < 90 and b < 90


@section("10", "stencil eyes and outlines in a live frame (ToonTest.glb)")
def s_toon():
    ensure_3d()
    menu_closed()
    ensure_avatar(TOON)
    face_avatar(2.0, -2, 1.5)
    img = calm_frame("toon")
    box = (img.w // 5, 0, img.w * 4 // 5, img.h)
    ne, nf, no = img.count(eyes, box), img.count(fringe, box), img.count(outline, box)
    check("10", "the eyes show through the fringe in front of them (stencil)", ne > 150, f"{ne} cyan pixels")
    check("10", "the fringe is drawn around them", nf > 1500, f"{nf} magenta pixels")
    check("10", "the body has its green outline", no > 150, f"{no} green pixels")
    ctl("hypr3d", "view", "first")
    time.sleep(0.8)
    fp = calm_frame("toon-first-person")
    check("10", "first person: not drawn (no eyes, no outline)", fp.count(eyes, box) < 20 and fp.count(outline, box) < 20)
    ctl("hypr3d", "view", "third")
    ensure_avatar(AV)


@section("mic", "lip sync from PipeWire's default source (the test microphone)")
def s_mic():
    ensure_avatar(AV)
    ensure_3d()
    menu_closed()
    src = alice("wpctl inspect @DEFAULT_AUDIO_SOURCE@")
    check("mic", "the default source is the test microphone", 'node.name = "test_mic"' in src, re.findall(r'node.name = "[^"]*"', src))
    check("mic", "no hypr3d stream before lip sync is on", lipsync_node() is None)
    ls = json.loads(ctl("hypr3d", "avatar", "lipsync", "on"))
    check("mic", "avatar lipsync on: on, listening, a microphone", ls["on"] and ls["listening"] and ls["microphone"], ls)
    node = wait_for("the stream's link", lambda: (lambda n: n if n and n["fed_by"] else None)(lipsync_node()), 10)
    check("mic", "pw-dump: the \"hypr3d lip sync\" stream, fed by the test microphone",
          node["props"].get("node.description") == "hypr3d lip sync" and "test_mic" in node["fed_by"], f"{node['props'].get('media.class')} from {node['fed_by']}")
    face_avatar(1.3, -2)
    img = calm_frame("lipsync-badge")
    corner = (img.w - 320, 0, img.w, 80)
    red = img.count(lambda r, g, b: r > 170 and g < 90 and b < 90, corner)
    check("mic", "the badge in the top right corner (its red dot)", red > 20, f"{red} red pixels")
    shut = lipsync()
    check("mic", "silence: the mouth shut", max(shut["visemes"].values()) < 0.05, shut)
    results = {}
    for who in ("man", "woman"):
        for k, v in enumerate(VOWELS):
            reads = sing(f"{who}_{v}", "lipsync-woman-a" if (who, v) == ("woman", "a") else None)
            med = {n: sorted(r["visemes"][n] for r in reads)[len(reads) // 2] for n in VISEMES}
            best = max(VISEMES, key=lambda n: med[n])
            lvl = sorted(r["level"] for r in reads)[len(reads) // 2]
            f1 = sorted(r["formants"][0] for r in reads)[len(reads) // 2]
            f2 = sorted(r["formants"][1] for r in reads)[len(reads) // 2]
            results[f"{who}_{v}"] = {"level": lvl, "formants": [f1, f2], "visemes": med, "readings": reads}
            check("mic", f"{who}'s {v}: {VISEMES[k]} the most", best == VISEMES[k] and med[best] > 0.5,
                  f"{best} {med[best]:.2f}; level {lvl:.0f} dB, F1 {f1:.0f} F2 {f2:.0f} Hz; " + " ".join(f"{n} {med[n]:.2f}" for n in VISEMES))
    for f in ("silence", "hiss", "quiet_a", "o_then_hiss"):
        # (o_then_hiss: a man's o, then hiss: once a consonant's moment is over, the mouth must shut; its readings
        # start 0.8 s into the hiss)
        reads = sing(f, skip=1.6 if f == "o_then_hiss" else 0.6)
        most = max(max(r["visemes"].values()) for r in reads)
        results[f] = {"readings": reads}
        check("mic", f"{f}: the mouth stays shut", most < 0.05, f"the most {most:.2f}, level {reads[len(reads) // 2]['level']:.0f} dB")
    (LOGS / "lipsync.json").write_text(json.dumps(results, indent=1))
    ensure_3d(False)
    time.sleep(0.5)
    ls = lipsync()
    check("mic", "leaving 3D closes the microphone (still on for next time)", ls["on"] and not ls["listening"] and lipsync_node() is None, ls)
    ensure_3d()
    ls = wait_for("listening", lambda: (lambda l: l if l["listening"] else None)(lipsync()), 10)
    check("mic", "entering 3D opens it again", ls["listening"] and lipsync_node() is not None, ls)
    ls = json.loads(ctl("hypr3d", "avatar", "lipsync", "off"))
    time.sleep(0.5)
    check("mic", "avatar lipsync off: closed, no stream", not ls["on"] and not ls["listening"] and lipsync_node() is None, ls)
    img = calm_frame("lipsync-off")
    red = img.count(lambda r, g, b: r > 170 and g < 90 and b < 90, corner)
    check("mic", "... and no badge", red < 10, f"{red} red pixels")
    # the menu's Options: Lip sync
    press("tab")
    press("5")
    items = {i["label"]: i for i in menu()["items"]}
    slot = items["Lip sync"]["slot"]
    press(str(slot))
    ls = lipsync()
    check("mic", "the Action Menu's Options > Lip sync turns it on", ls["on"] and ls["listening"], ls)
    press(str(slot))
    ls = lipsync()
    check("mic", "... and off", not ls["on"] and not ls["listening"], ls)
    press("esc")


@section("11", "after 3D, Hyprland draws its windows as before (rounding, blur, borders)")
def s_after():
    ensure_3d(False)
    before = calm_frame("desktop-before-3d")
    ensure_3d()
    menu_closed()
    ensure_avatar(TOON)  # stencils and outlines: the most GL state
    face_avatar(2.0, -2, 1.0)
    press("tab")
    time.sleep(0.5)
    ctl("dismissnotify")
    time.sleep(1.0)
    plain = frame("menu-over-toon")
    # Hyprland draws its notifications after the plugin's pass, in the same frame: they must come out right
    ctl("notify", "1", "8000", "rgb(ff8800)", "hypr3d: a notification over the 3D view")
    time.sleep(0.8)
    noted = frame("notification-over-3d")
    corner = (noted.w // 2, 0, noted.w, 160)
    dn = plain.differs(noted, corner, step=1)
    orange = noted.count(lambda r, g, b: r > 200 and 100 < g < 180 and b < 80, corner)
    check("11", "in 3D, Hyprland draws its notification over the view (its box and orange icon)", dn > 0.01 and orange > 20,
          f"{dn:.2%} of the top right changed, {orange} orange pixels")
    press("esc")
    ensure_3d(False)
    time.sleep(0.5)
    after = calm_frame("desktop-after-3d")
    d = before.differs(after)
    check("11", "the desktop looks as it did before 3D", d < 0.002, f"{d:.2%} of pixels differ")
    for name, box in {"a window's rounded corner and border": (0, 0, 120, 120), "the blurred wallpaper through a window": (200, 200, 500, 500),
                      "the other window's border": (after.w - 140, after.h - 140, after.w, after.h)}.items():
        dd = before.differs(after, box, step=1)
        check("11", f"... {name}", dd < 0.005, f"{dd:.2%}")
    ensure_avatar(AV)


@section("12", "unloading and loading the plugin again")
def s_unload():
    ensure_avatar(AV)
    ensure_3d(False)
    before = calm_frame("desktop-before-unload")
    ensure_3d()
    ctl("hypr3d", "avatar", "lipsync", "on")
    press("tab")
    wait_for("the stream", lipsync_node, 10)
    frame("before-unload-in-3d")
    # as when a plugin is taken out of the config, its values go too (Hyprland reloads the config after unloading a
    # plugin, and values no one has would be errors)
    write_config("hyprland.lua", lua_config())
    r = ctl("plugin", "unload", SO)
    check("12", "hyprctl plugin unload, in 3D with the menu open and the microphone on", r == "ok", r)
    time.sleep(1.5)
    check("12", "Hyprland lives on", alive())
    check("12", "the plugin's gone from the list", "hypr3d" not in ctl("plugin", "list"))
    r = ctl("hypr3d", "status")
    check("12", "hyprctl hypr3d is gone", not r.startswith("{"), r[:80])
    check("12", "its PipeWire stream is gone", lipsync_node() is None)
    img = calm_frame("desktop-after-unload")
    errs = ctl("configerrors")
    check("12", "Hyprland reloaded its config: no errors", config_ok(errs), errs[:200])
    d = before.differs(img)
    check("12", "the desktop is back as it was", d < 0.002, f"{d:.2%} of pixels differ")
    write_config("hyprland.lua", lua_config(LUA_CFG))
    r = ctl("plugin", "load", SO)  # (and Hyprland reloads the config: the values are back)
    check("12", "loading it again", r == "ok", r)
    time.sleep(1.5)
    a = ensure_avatar(AV)
    check("12", "the config's avatar is back", a.get("name") == "BoothAccessories", a.get("name"))
    ensure_3d()
    time.sleep(1.0)
    img = frame("3d-after-reload")
    check("12", "3D again: the frame is the 3D view", before.differs(img) > 0.3)
    ensure_3d(False)
    write_config("hyprland.lua", lua_config())
    r = ctl("plugin", "unload", SO)
    time.sleep(1.0)
    check("12", "unloading it out of 3D", r == "ok" and alive(), r)
    write_config("hyprland.lua", lua_config(LUA_CFG))
    r = ctl("plugin", "load", SO)
    time.sleep(1.0)
    check("12", "... and loading it once more", r == "ok" and ensure_avatar(AV).get("name") == "BoothAccessories", r)


@section("8b", "hl.plugin.load in the Lua config, with the plugin's values after it")
def s_lua_load():
    cfg = {"avatar": AV, "avatar_physics": False}
    start_hyprland("hyprland.lua", lua_config(cfg, load=True))
    errs = ctl("configerrors")
    check("8b", "no config errors", config_ok(errs), errs[:300])
    check("8b", "loaded with the config", "hypr3d" in ctl("plugin", "list"))
    a = ensure_avatar(AV)
    check("8b", "its values apply: the avatar, physics off", a.get("name") == "BoothAccessories" and a.get("physics") is False, f"{a.get('name')} physics {a.get('physics')}")
    press("meta_l", "grave_accent")
    ok = wait_for("3D", lambda: st()["mode"] == "active", 10)
    check("8b", "Super+` enters 3D", ok)
    time.sleep(0.8)
    frame("3d-lua-load")
    ensure_3d(False)


@section("8c", "a classic hyprland.conf: plugin =, plugin { hypr3d { } }, the hypr3d:toggle and hypr3d:menu dispatchers")
def s_conf():
    cfg = {"avatar": AV, "avatar_physics": False, "lipsync": False}
    start_hyprland("hyprland.conf", conf_config(cfg))
    errs = ctl("configerrors")
    check("8c", "no config errors", config_ok(errs), errs[:300])
    check("8c", "plugin = loads it", "hypr3d" in ctl("plugin", "list"))
    a = ensure_avatar(AV)
    check("8c", "plugin { hypr3d { avatar, avatar_physics } } apply", a.get("name") == "BoothAccessories" and a.get("physics") is False, f"{a.get('name')} physics {a.get('physics')}")
    before = calm_frame("desktop-conf")
    r = ctl("dispatch", "hypr3d:toggle")
    ok = wait_for("3D", lambda: st()["mode"] == "active", 10)
    check("8c", "hyprctl dispatch hypr3d:toggle", r == "ok" and ok, r)
    r = ctl("dispatch", "hypr3d:menu", "emotes")
    check("8c", "hyprctl dispatch hypr3d:menu emotes", r == "ok" and menu().get("path") == "main/emotes", f"{r}; {menu().get('path')}")
    r = ctl("dispatch", "hypr3d:menu", "pick", "1")
    time.sleep(0.5)
    check("8c", "hyprctl dispatch hypr3d:menu pick 1: an emote", r == "ok" and av()["emote"] != "", f"{r}; {av()['emote']!r}")
    ctl("hypr3d", "avatar", "emote", "stop")
    r = ctl("dispatch", "hypr3d:menu")
    check("8c", "hyprctl dispatch hypr3d:menu toggles it closed", r == "ok" and menu().get("open") is False, r)
    r = ctl("dispatch", "hypr3d:menu", "nosuchpage")
    check("8c", "a page that isn't there: the dispatcher fails, saying why", r != "ok" and "no such page" in r, r)
    press("meta_l", "o")
    check("8c", "bind = SUPER, O, hypr3d:menu, options (pressed)", menu().get("path") == "main/options", menu().get("path"))
    press("meta_l", "m")
    check("8c", "bind = SUPER, M, hypr3d:menu (pressed): closed", menu().get("open") is False)
    press("meta_l", "grave_accent")
    ok = wait_for("2D", lambda: st()["mode"] == "off", 10)
    check("8c", "bind = SUPER, grave, hypr3d:toggle (pressed): out of 3D", ok)
    r = ctl("keyword", "plugin:hypr3d:avatar_physics", "true")
    ok = wait_for("physics on", lambda: av()["physics"] is True, 3, 0.2)
    check("8c", "hyprctl keyword plugin:hypr3d:avatar_physics true takes effect", ok, f"keyword: {r}; physics {av()['physics']}")
    cfg["avatar_physics"] = True
    reload_config("hyprland.conf", conf_config(cfg))
    check("8c", "... and after hyprctl reload", av()["physics"] is True, av()["physics"])
    ensure_3d()
    time.sleep(0.8)
    frame("3d-conf")
    ensure_3d(False)
    d = before.differs(calm_frame("desktop-conf-after-3d"))
    check("8c", "after 3D, this desktop too looks as it did before", d < 0.002, f"{d:.2%} of pixels differ")


# ------------------------------------------------------------------ run

def collect_logs():
    for i, f in enumerate(machine.execute("ls /run/user/1000/hypr/*/hyprland.log 2>/dev/null || true")[1].split()):
        machine.execute(f"cp {f} /tmp/hyprland-{i}.log; chmod 644 /tmp/hyprland-{i}.log")
        copy_out(f"/tmp/hyprland-{i}.log", "logs")
    machine.execute("journalctl -b --no-pager > /tmp/journal.txt 2>&1; true")
    copy_out("/tmp/journal.txt", "logs")
    machine.execute(f"runuser -u alice -- env XDG_RUNTIME_DIR=/run/user/1000 pw-dump > /tmp/pw-dump.json 2>&1; true")
    copy_out("/tmp/pw-dump.json", "logs")
    for c in crash_reports():
        machine.execute(f"cp {HOME}/.cache/hyprland/{c} /tmp/{c}; true")
        copy_out(f"/tmp/{c}", "logs")


(OUT / "results.txt").write_text("")
t_start = time.time()
machine.start()
machine.wait_for_unit("multi-user.target")
machine.copy_from_host(str(IN), H)
machine.succeed(f"chown -R alice:users {H}")
note("vm", "booted", f"{time.time() - t_start:.1f} s")

for item, title, fn in SECTIONS:
    print(f"\n=== {item}: {title}", flush=True)
    t0 = time.time()
    crashes = crash_reports()
    try:
        fn()
    except Exception as e:  # noqa: BLE001
        traceback.print_exc()
        check(item, f"({title}: stopped)", False, f"{type(e).__name__}: {e}"[:700])
    if HYPR["pid"] and not alive():
        check(item, "Hyprland still running afterwards", False, f"crash reports: {[c for c in crash_reports() if c not in crashes]}")
        collect_logs()
        try:
            start_hyprland(HYPR["config"], lua_config() if HYPR["config"].endswith(".lua") else conf_config(load=False))
            ensure_plugin()
        except Exception as e:  # noqa: BLE001
            check(item, "Hyprland started again", False, str(e)[:300])
    print(f"=== {item} took {time.time() - t0:.1f} s", flush=True)

try:
    stop_hyprland()
except Exception as e:  # noqa: BLE001
    check("exit", "stopping Hyprland", False, str(e)[:300])
check("exit", "no Hyprland crashes in the whole run", segfaults() == 0, machine.execute("journalctl -k --no-pager | grep 'segfault at' || true")[1].strip()[:300])
collect_logs()
passed = sum(1 for r in RESULTS if r["ok"] is True)
failed = [r for r in RESULTS if r["ok"] is False]
(OUT / "results.json").write_text(json.dumps(RESULTS, indent=1, ensure_ascii=False))
summary = f"{passed} passed, {len(failed)} failed, in {time.time() - t_start:.0f} s"
with open(OUT / "results.txt", "a") as f:
    f.write(summary + "\n")
print(summary, flush=True)
machine.shutdown()
if failed:
    raise Exception(f"{len(failed)} checks failed")
