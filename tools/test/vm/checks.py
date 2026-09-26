# checks.py: the checklist tools/test/vm/run.sh runs in the VMs of vm.nix, through nixos-test-driver (`machine`, at
# 1280x800, then `hidpi`, at 1920x1200).
#
# hypr3d.so goes into a real Hyprland 0.55 on a virtio GPU: first with a Lua config (hyprctl plugin load), then with
# hl.plugin.load in the Lua config, then with a classic hyprland.conf (plugin = ..., the hypr3d:toggle and
# hypr3d:menu dispatchers). Keys, the mouse (relative, a PS/2 mouse), the tablet (absolute) and the wheel come from
# the VM's own input devices through QMP's input-send-event, so they pass through the kernel, libinput and
# Hyprland's input stack to the plugin's listeners and function hooks; wheel.py adds a mouse with a high-resolution
# wheel through uinput, and touchpad.py a touchpad that scrolls with two fingers. wev shows what reaches a window. Frames come from grim inside the VM. Lip sync listens to
# PipeWire's default source, a virtual "Test microphone" that pw-cat sings test vowels into. A second monitor is
# Hyprland's own headless output (hyprctl output create), and the hidpi VM runs at scales 1.5 and 2.
#
# Each check passes or fails on its own; a section that throws fails as a whole and the rest go on. A check that
# fails for a known reason outside hypr3d (Hyprland 0.55.2's crash on exit) says "known" and doesn't count. H3D_OUT
# gets results.txt (a line per check), results.json, frames/ (numbered in order) and logs/ (logs/hidpi: the other
# VM's).
import datetime as dt
import json
import math
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
LIT = f"{H}/LitCourt.glb"
LIT_RS = f"{H}/LitCourtRuntimeSun.glb"  # (its sun has no baked shadow channel)
VOWELS = ["a", "i", "u", "e", "o"]
VISEMES = ["aa", "ih", "ou", "ee", "oh"]
for d in (FRAMES, LOGS, RAW):
    d.mkdir(parents=True, exist_ok=True)


def secs(s):
    return dt.timedelta(seconds=s)


# ------------------------------------------------------------------ results

RESULTS = []


def check(item, what, ok, detail="", known=""):
    """known: why it fails when it does, a bug that isn't hypr3d's: reported, not counted"""
    ok = bool(ok)
    RESULTS.append({"item": item, "check": what, "ok": ok, "detail": str(detail), "known": "" if ok else known})
    tag = "ok" if ok else "known" if known else "FAIL"
    line = f"{tag:<5} {item:<6} {what}" + (f"  [{detail}]" if str(detail) else "") + (f"  (known: {known})" if tag == "known" else "")
    print(line, flush=True)
    with open(OUT / "results.txt", "a", encoding="utf-8") as f:
        f.write(line + "\n")
    return ok


def note(item, what, detail=""):
    """something measured, not passed or failed"""
    line = f"      {item:<6} {what}" + (f"  [{detail}]" if str(detail) else "")
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
    """the PS/2 mouse: counts, a hundred at a time (QEMU's PS/2 mouse sends what's over its packets' reach only with
    the next move, much later)"""
    n = max(1, math.ceil(max(abs(dx), abs(dy)) / 100))
    for k in range(n):
        x, y = int(dx * (k + 1) / n) - int(dx * k / n), int(dy * (k + 1) / n) - int(dy * k / n)
        qmp([{"type": "rel", "data": {"axis": "x", "value": x}}, {"type": "rel", "data": {"axis": "y", "value": y}}])
        if k + 1 < n:
            time.sleep(0.02)
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


def frame(name, output=None, timeout=60):
    """what's on the screen (or on that output), through grim; saved as frames/NN-name.png"""
    FRAME_N[0] += 1
    alice("grim -t ppm " + (f"-o {output} " if output else "") + "/tmp/frame.ppm", timeout)
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
-- the test games opaque (as a fullscreen game is anyway, decoration:fullscreen_opacity), so their colours can be read
hl.window_rule({ name = "h3d-games-opaque", match = { class = "h3dgame.*|chocolate-doom|supertux2" }, opacity = "1.0 override 1.0 override" })
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


def lua_config(plugin=None, load=False, scale=1, layout="dwindle"):
    text = BASE_LUA.replace("scale = 1 })", f"scale = {scale} }})").replace('layout = "dwindle"', f'layout = "{layout}"')
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


# a portal's own crash isn't Hyprland's: xdg-desktop-portal-hyprland 1.4.1 can segfault in libwayland-client when
# Hyprland quits under it (its kernel line says ".xdg-desktop-po"); vm_done notes them apart
PORTAL_CRASH = "xdg-desktop-po"


def segfaults():
    return int(machine.execute(f"journalctl -k --no-pager | grep 'segfault at' | grep -vc '{PORTAL_CRASH}' || true")[1].strip() or 0)


def coredumps():
    """the core dumps systemd-coredump has taken so far (their PIDs)"""
    out = machine.execute("coredumpctl list --json=short --no-pager 2>/dev/null || true")[1].strip()
    try:
        return {str(d.get("pid")) for d in json.loads(out)} if out.startswith("[") else set()
    except ValueError:
        return set()


def new_coredump(before):
    """the crashed thread's stack in a core dump taken since `before` (coredumps()); None: none yet"""
    new = sorted(p for p in coredumps() if p not in before)
    if not new:
        return None
    return machine.execute(f"coredumpctl info --no-pager {new[-1]} 2>/dev/null | grep -m1 -A20 'Stack trace of thread' || true")[1]


def restart_after_crash():
    """Hyprland again after a crash a check saw coming, and the plugin; its crash dialog (hyprland-dialog, which
    segfaults in this VM's hyprtoolkit) and its crash counted as seen"""
    n = segfaults()
    start_hyprland(HYPR["config"], lua_config() if HYPR["config"].endswith(".lua") else conf_config(load=False))
    ensure_plugin()
    time.sleep(3)
    KNOWN_CRASHES[0] += segfaults() - n


def stop_hyprland():
    """Hyprland 0.55.2 itself dies in its exit path when windows are still open (CCompositor::cleanup ->
    CWindow::unmapWindow -> CDwindleAlgorithm or CMasterAlgorithm::calculateWorkspace -> ITarget::setPositionGlobal,
    a null pointer; with or without hypr3d), so the terminals go first. A crash now is a crash with the plugin (if
    it's loaded)."""
    if not HYPR["pid"]:
        return
    plugin = "hypr3d" in ctl("plugin", "list")
    machine.execute("pkill -u alice foot || true")
    try:  # (and anything else with a window)
        for c in json.loads(ctl("-j", "clients") or "[]"):
            if c.get("pid", 0) > 1:
                machine.execute(f"kill {c['pid']} 2>/dev/null; true")
    except ValueError:
        pass
    time.sleep(1.5)
    n = segfaults()
    machine.execute("systemctl stop hyprland")
    time.sleep(2)
    if plugin:
        check("exit", f"Hyprland exits without crashing, hypr3d loaded (session {HYPR['session']})", segfaults() == n)
    HYPR["pid"] = ""


def start_hyprland(name, text, terminals=True):
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
    # the graphical session, for the portals (file dialogs, screen capture), as a desktop session has it; portals of
    # an earlier Hyprland (a dead display) go, and come again when asked for
    as_alice("systemctl --user import-environment WAYLAND_DISPLAY; systemctl --user stop xdg-desktop-portal.service xdg-desktop-portal-gtk.service "
             "xdg-desktop-portal-hyprland.service; systemctl --user start hyprland-session.target", 30)
    wait_for("the monitor", lambda: json.loads(ctl("-j", "monitors")), 20)
    if not terminals:
        return
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


def lua_session():
    """the Lua config's session, which the sections from 18 on are written for (8c leaves a hyprland.conf one, whose
    dispatchers and window rules they don't use)"""
    if not HYPR["config"].endswith(".lua"):
        start_hyprland("hyprland.lua", lua_config())
        ensure_plugin()


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


def section(item, title, vm="machine"):
    """a part of the checklist, in the VM of that name"""
    def deco(fn):
        SECTIONS.append((item, title, fn, vm))
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
    rel(125, 60)  # up and to the right (51 degrees round): the second of seven, clockwise from the top
    h = menu()["highlight"]
    check("1", "... up and right points at Expressions", h == 2, f"highlight {h}")
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
    check("1", "a wheel notch goes round to the next item", after == (before % 7) + 1 if before > 0 else after == 1, f"{before} -> {after}")
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
    want = {1: "main/emotes", 2: "main/expressions", 3: "main/gestures", 4: "main/outfit", 5: "main/apps", 6: "main/windows", 7: "main/options"}.get(h)
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
    # high: 150 counts right from the top is atan2(150 / 224, 0.66) round (tools/test/harness/ctl_check.sh has the
    # same numbers)
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
    # a 7 s clip: it fades out over its last 0.3 s of clip time plus 0.3 s, so (7 - 0.3) / speed + 0.3. (Below 20 frames a
    # second the plugin steps its animations by 50 ms a frame at most, so then they keep time by frames: a VM that's
    # busier during one than the other gets the time wrong, not the frames)
    ratio, fratio = t1 / max(t2, 1e-3), f1 / max(f2, 1)
    check("5", "twice as fast: over in about half the time (or half the frames)", 1.75 < ratio < 2.1 or 1.75 < fratio < 2.1,
          f"{t1:.2f} s / {t2:.2f} s = {ratio:.2f}, {f1} / {f2} frames = {fratio:.2f} (7.0 / 3.65 = 1.92)")


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
    press("7")
    items = {i["label"]: i for i in menu()["items"]}
    slot = items["Lip sync"]["slot"]
    press(str(slot))
    ls = lipsync()
    check("mic", "the Action Menu's Options > Lip sync turns it on", ls["on"] and ls["listening"], ls)
    press(str(slot))
    ls = lipsync()
    check("mic", "... and off", not ls["on"] and not ls["listening"], ls)
    press("esc")


def orange(r, g, b):
    """a notification of rgb(ff8800): its bar on the left, and its progress line"""
    return r > 200 and 100 < g < 180 and b < 80


def red(r, g, b):
    """the lip sync badge's dot"""
    return r > 170 and g < 90 and b < 90


def notification_boxes(img):
    """Hyprland's notifications in the top right corner, top down: (x, y, w, h), from their orange bars"""
    x0, x1, boxes, run = img.w // 2, img.w, [], None
    for y in range(0, img.h // 4):
        if img.count(orange, (x0, y, x1, y + 1)):
            run = [y, y] if run is None else [run[0], y]
        elif run is not None:
            boxes.append(run)
            run = None
    out = []
    for y0, y1 in boxes:
        left = min(x for x in range(x0, x1) if any(orange(*img.px[(y * img.w + x) * 3:(y * img.w + x) * 3 + 3]) for y in range(y0, y1 + 1, 2)))
        out.append((left, y0, img.w - left, y1 - y0 + 1))
    return out


def overlap(a, b):
    return a[0] < b[0] + b[2] and b[0] < a[0] + a[2] and a[1] < b[1] + b[3] and b[1] < a[1] + a[3]


def box_of(b):
    """(x, y, w, h) as (x0, y0, x1, y1)"""
    return (int(b[0]), int(b[1]), int(b[0] + b[2]), int(b[1] + b[3]))


@section("13", "the lip sync badge moves out from under Hyprland's notifications")
def s_badge():
    ensure_avatar(AV)
    ensure_3d()
    menu_closed()
    face_avatar(2.0, -4)
    ctl("hypr3d", "avatar", "lipsync", "on")
    wait_for("listening", lambda: lipsync()["listening"], 10)
    ctl("dismissnotify")  # (lip sync's own "on")
    time.sleep(1.2)
    b = wait_for("the badge", lambda: lipsync()["badge"], 5)
    img = frame("badge-alone")
    check("13", "alone, the badge sits in the top right corner, 12 px in", b[1] == 12 and b[0] + b[2] == img.w - 12, b)
    n = img.count(red, box_of(b))
    check("13", "... where the frame has it (its red dot)", n > 20, f"{n} red pixels in {b}")
    ctl("notify", "1", "15000", "rgb(ff8800)", "hypr3d: a notification in the badge's corner")
    time.sleep(1.2)  # (it slides in for 0.6 s)
    b1 = lipsync()["badge"]
    img = frame("badge-under-a-notification")
    notes = notification_boxes(img)
    check("13", "a notification: Hyprland draws it in the top right corner", len(notes) == 1, notes)
    check("13", "... and the badge moves below it, their boxes apart", len(notes) == 1 and b1[1] >= notes[0][1] + notes[0][3] and not overlap(b1, notes[0]),
          f"badge {b1}, notification {notes}")
    n = img.count(red, box_of(b1))
    check("13", "... where the frame has it", n > 20, f"{n} red pixels in {b1}")
    ctl("notify", "1", "15000", "rgb(ff8800)", "hypr3d: and a second one")
    time.sleep(1.2)
    b2 = lipsync()["badge"]
    img = frame("badge-under-two-notifications")
    notes = notification_boxes(img)
    check("13", "two notifications: the badge below both", len(notes) == 2 and b2[1] > b1[1] and not any(overlap(b2, nb) for nb in notes),
          f"badge {b2}, notifications {notes}")
    n = img.count(red, box_of(b2))
    check("13", "... where the frame has it", n > 20, f"{n} red pixels in {b2}")
    ctl("dismissnotify")
    time.sleep(1.2)
    b3 = lipsync()["badge"]
    check("13", "dismissed: the badge goes back up to the corner", b3 and b3[1] == 12, b3)
    ctl("hypr3d", "avatar", "lipsync", "off")
    check("13", "lip sync off: no badge", lipsync()["badge"] is None)


def monitors():
    return {m["name"]: m for m in json.loads(ctl("-j", "monitors"))}


def focused_monitor():
    return next((n for n, m in monitors().items() if m["focused"]), None)



# ------------------------------------------------------------------ wev: what reaches a window

WEV_LINE = re.compile(r"\[\s*\d+:\s*(\S+)\] (\w+)(?:: (.*))?$")


def wev_start():
    """wev alone on the workspace (it fills the desktop), printing a line per event it gets"""
    machine.execute("pkill -u alice foot; pkill -x wev; true")
    time.sleep(1)
    # (appending, as the marks between its lines do: else it writes over them; a line at a time)
    alice("rm -f /tmp/wev.log; setsid -f stdbuf -oL wev >> /tmp/wev.log 2>&1")
    wait_for("wev", lambda: any(c["class"] == "wev" for c in json.loads(ctl("-j", "clients"))), 20)
    time.sleep(1.5)


def wev_mark(name):
    alice(f"echo {shlex.quote('### ' + name)} >> /tmp/wev.log")  # (its file: root can't write to alice's in /tmp)


def wev_events(name):
    """what wev printed from mark `name` to the next: (interface, event, the rest)"""
    out, on = [], False
    for line in machine.succeed("cat /tmp/wev.log").splitlines():
        if line.startswith("### "):
            if on:
                break
            on = line[4:].strip() == name
            continue
        m = WEV_LINE.match(line) if on else None
        if m:
            out.append((m.group(1), m.group(2), m.group(3) or ""))
    return out


def wev_states(name):
    """the xdg_toplevel states each configure since mark `name` (to the next mark) gave: wev prints them on the
    line after the configure"""
    out, on, lines = [], False, machine.succeed("cat /tmp/wev.log").splitlines()
    for i, line in enumerate(lines):
        if line.startswith("### "):
            if on:
                break
            on = line[4:].strip() == name
            continue
        if on and "xdg_toplevel] configure:" in line:
            nxt = lines[i + 1] if i + 1 < len(lines) and not lines[i + 1].startswith(("[", "###")) else ""
            out.append(nxt.split())
    return out


def wev_xy(evs):
    """where the pointer last entered or moved to, surface-local"""
    for iface, ev, rest in reversed(evs):
        if iface == "wl_pointer" and ev in ("enter", "motion"):
            m = re.search(r"x, y: (-?[\d.]+), (-?[\d.]+)", rest)
            return float(m.group(1)), float(m.group(2))
    return None


def wev_buttons(evs):
    return [tuple(int(v) for v in re.search(r"button: (\d+) .*state: (\d)", rest).groups()) for i, ev, rest in evs if i == "wl_pointer" and ev == "button"]


def wev_keys(evs):
    return [tuple(int(v) for v in re.search(r"key: (\d+); state: (\d)", rest).groups()) for i, ev, rest in evs if i == "wl_keyboard" and ev == "key"]


def wev_scrolls(evs):
    """the wheel's frames: (axis, value, value120, discrete)"""
    out, cur = [], {}
    for iface, ev, rest in evs:
        if iface != "wl_pointer":
            continue
        if ev == "axis":
            m = re.search(r"axis: (\d) \(\w+\), value: (-?[\d.]+)", rest)
            cur["axis"], cur["value"] = int(m.group(1)), round(float(m.group(2)), 2)
        elif ev in ("axis_value120", "axis_discrete"):
            cur[ev[5:]] = int(re.search(r"(-?\d+)$", rest).group(1))
        elif ev == "frame" and "axis" in cur:
            out.append((cur["axis"], cur["value"], cur.get("value120"), cur.get("discrete")))
            cur = {}
    return out


def wev_axis_frames(evs):
    """the scrolling's frames: all that wev got in each (event, what it said but the time)"""
    out, cur = [], []
    for iface, ev, rest in evs:
        if iface != "wl_pointer":
            continue
        if ev.startswith("axis"):
            cur.append((ev, re.sub(r"time: \d+; ", "", rest)))
        elif ev == "frame" and cur:
            out.append(tuple(cur))
            cur = []
    return out


def wheel_hires(*steps):
    """wheel.py: a high-resolution wheel turned by each of these, in 1/120ths of a notch (h: the horizontal one)"""
    machine.succeed("python3 " + H + "/wheel.py " + " ".join(str(v) for v in steps), timeout=30)
    time.sleep(0.5)


def touchpad(*steps):
    """touchpad.py: two fingers moved this far on a touchpad, one scroll each, 30 units a millimetre (h: sideways)"""
    machine.succeed("python3 " + H + "/touchpad.py " + " ".join(str(v) for v in steps), timeout=30)
    time.sleep(0.5)


WHEEL_SEQ = [("a notch down", lambda: wheel(1)), ("half notches: down, down, down, up", lambda: wheel_hires(60, 60, 60, -60)),
             ("half notches of the horizontal wheel, right", lambda: wheel_hires("h60", "h60"))]
# wheel.py's device (Hyprland's name for "hypr3d test wheel"), and a window rule for wev: Hyprland takes the rule's
# scroll factor first, then the device's, then input's
WHEEL_DEV = 'hl.device({{ name = "hypr3d-test-wheel", scroll_factor = {} }})'
WEV_RULE = 'hl.window_rule({ name = "h3d-wev-scroll", match = { class = "wev" }, scroll_mouse = 3, scroll_touchpad = 2 })'
WEV_RULE_OFF = 'hl.window_rule({ name = "h3d-wev-scroll", enabled = false })'
EMULATE = 'hl.config({{ input = {{ emulate_discrete_scroll = {} }} }})'
# (what's set, [(what's done, how, what Hyprland sends on the 2D desktop: (axis, value, value120, discrete) a frame,
# or None)]); each after the ones before. The QEMU mouse's wheel is "the other mouse"
WHEEL_CASES = [
    ("", [], [(what, do, None) for what, do in WHEEL_SEQ]),
    ("the wheel's own scroll_factor 2.5", [WHEEL_DEV.format(2.5)], [
        ("a notch down", lambda: wheel_hires(120), [(0, 37.5, 300, None)]),
        ("half notches: down, down, down, up", lambda: wheel_hires(60, 60, 60, -60), [(0, 37.5, 150, None)] * 3 + [(0, -37.5, -150, None)]),
        ("the other mouse's notch", lambda: wheel(1), [(0, 15.0, 120, None)])]),
    ("a window rule's scroll_mouse 3, over the wheel's 2.5", [WEV_RULE], [
        ("a notch down", lambda: wheel_hires(120), [(0, 45.0, 360, None)]),
        ("the other mouse's notch", lambda: wheel(1), [(0, 45.0, 360, None)])]),
    ("input:emulate_discrete_scroll 0", [WEV_RULE_OFF, WHEEL_DEV.format(1), EMULATE.format(0)], [
        ("half notches: down, down, down, up", lambda: wheel_hires(60, 60, 60, -60), [(0, 7.5, 60, None)] * 3 + [(0, -7.5, -60, None)])]),
    ("input:emulate_discrete_scroll 2, the wheel's 2.5", [WHEEL_DEV.format(2.5), EMULATE.format(2)], [
        ("notches down, three", lambda: wheel_hires(120, 120, 120), [(0, 37.5, 300, None), (0, 75.0, 300, None), (0, 112.5, 300, None)])]),
    ("a touchpad", [WHEEL_DEV.format(1), EMULATE.format(1)], [
        ("two fingers down", lambda: touchpad(600), None), ("two fingers right", lambda: touchpad("h450"), None),
        ("two fingers down and right", lambda: touchpad("d450"), None)]),
    ("a touchpad, a window rule's scroll_touchpad 2", [WEV_RULE], [("two fingers down", lambda: touchpad(600), None)]),
]


def wheel_cases(mode):
    """every case of WHEEL_CASES in turn: {(case, what): (wev_scrolls, wev_axis_frames)}"""
    got = {}
    for case, lua, seqs in WHEEL_CASES:
        for stmt in lua:
            r = ctl("eval", stmt)
            if r.strip() not in ("", "ok"):
                note("14", f"hyprctl eval {stmt}", r)
        time.sleep(0.5)
        for what, do, _ in seqs:
            mark = f"{mode} {case}: {what}"
            wev_mark(mark)
            do()
            evs = wev_events(mark)
            got[case, what] = (wev_scrolls(evs), wev_axis_frames(evs))
    ctl("eval", WEV_RULE_OFF)
    return got


@section("14", "what reaches a window in 3D (wev): enter, motion, buttons, the wheel as on the 2D desktop, keys, leave")
def s_wev():
    ensure_avatar(AV)
    ensure_3d(False)
    wev_start()
    # on the 2D desktop first, from Hyprland itself: the pointer in the middle of wev (moved there: it only enters wev
    # when it moves)
    tablet(12000, 12000)
    tablet(16384, 16384)
    time.sleep(0.5)
    flat = wheel_cases("2d")
    note("14", "the wheel on the 2D desktop", {f"{c}: {w}" if c else w: v[0] for (c, w), v in flat.items()})
    for case, lua, seqs in WHEEL_CASES:
        for what, do, want in seqs:
            if want is not None:
                check("14", f"on the 2D desktop, {case}: {what}: Hyprland sends what its onMouseWheel says", flat[case, what][0] == want,
                      f"{flat[case, what][0]}, expected {want}")
    frames = {k: v[1] for k, v in flat.items() if k[0].startswith("a touchpad")}
    note("14", "a touchpad on the 2D desktop", frames)
    check("14", "on the 2D desktop, the touchpad scrolls with fingers: axis_source finger, then axis_stop",
          all(f and all(any(e == "axis_source" and "finger" in r for e, r in fr) for fr in f) and any(e == "axis_stop" for fr in f for e, r in fr)
              for f in frames.values()), frames)
    both = [fr for fr in frames["a touchpad", "two fingers down and right"] if sum(e == "axis" for e, r in fr) == 2]
    check("14", "... down and right at once: both axes in a frame (Hyprland holds a touchpad's frame back for the device's)", len(both) > 3,
          f"{len(both)} frames with both")
    ensure_3d()
    menu_closed()
    ctl("hypr3d", "view", "first")
    ctl("hypr3d", "spawn")
    time.sleep(1.5)
    wev_mark("3d aimed")
    a = st()["aimed"]
    check("14", "the crosshair on wev", a and a["class"] == "wev" and a["surface"], a)
    rel(30, 10)
    time.sleep(1)
    a = st()["aimed"]
    at = wev_xy(wev_events("3d aimed"))
    check("14", "the mouse moves the crosshair: wev gets motion, surface-local, where the crosshair is", at and a and abs(at[0] - a["local"][0]) < 1.5 and abs(at[1] - a["local"][1]) < 1.5,
          f"wev {at}, crosshair {a and a['local']}")
    wev_mark("3d buttons")
    for b in ("left", "right", "middle"):
        click(b)
    got = wev_buttons(wev_events("3d buttons"))
    check("14", "clicks: left, right and middle, pressed and released", got == [(272, 1), (272, 0), (273, 1), (273, 0), (274, 1), (274, 0)], got)
    check("14", "the plugin hooked CInputManager::onMouseWheel and onPointerFrame (the device's scroll factor, a touchpad's frames)",
          st()["hooks"].get("wheel") and st()["hooks"].get("frame"), st()["hooks"])
    got = wheel_cases("3d")
    for case, lua, seqs in WHEEL_CASES:
        for what, do, want in seqs:
            g, f = got[case, what], flat[case, what]
            # (the frames too: a touchpad's axes and axis_stop)
            same = g[0] and g[0] == f[0] and g[1] == f[1]
            check("14", f"the wheel, {case + ': ' if case else ''}{what}: what wev gets in 2D", same,
                  f"3D {g[0]}, 2D {f[0]}" + ("" if g[1] == f[1] else f"; frames 3D {g[1]}, 2D {f[1]}"))
    wev_mark("3d walking")
    press("a")
    got = wev_keys(wev_events("3d walking"))
    check("14", "a key while walking isn't the window's", got == [], got)
    wev_mark("3d typing")
    press("e")
    press("a")
    press("b")
    press("meta_l", "esc")
    got = [k for k in wev_keys(wev_events("3d typing")) if k[0] in (38, 56)]
    check("14", "E, then keys: typed into it (a and b, pressed and released)", got == [(38, 1), (38, 0), (56, 1), (56, 0)], got)
    wev_mark("3d away")
    rel(-900, 0)
    time.sleep(1)
    evs = wev_events("3d away")
    check("14", "the crosshair off it: wev gets a leave", any(i == "wl_pointer" and e == "leave" for i, e, r in evs), [e for i, e, r in evs if i == "wl_pointer"][-4:])
    frame("wev-3d")
    ensure_3d(False)
    machine.execute("pkill -x wev; true")
    alice("setsid -f foot --app-id h3d-left > /dev/null 2>&1; sleep 0.7; setsid -f foot --app-id h3d-right > /dev/null 2>&1")
    wait_for("two terminals", lambda: len(json.loads(ctl("-j", "clients"))) >= 2, 20)
    time.sleep(1.5)


# ------------------------------------------------------------------ carrying windows

def windows3d():
    return ctlj("hypr3d", "windows")


AIM = {}  # how the last aim_at went, for when it didn't


def aim_at(cls, step=-15, tries=40):
    """turns until the crosshair is on that window"""
    seen = []
    for _ in range(tries):
        s = st()
        a = s["aimed"]
        if a and a.get("kind") == "window" and a.get("class") == cls:
            return a
        seen.append(f"{s['yaw']:.0f}:{a and a.get('class')}:{s['panels']}")
        rel(step, 0, after=0.2)
    AIM["last"] = " ".join(seen[:12])
    return None


def settled(timeout=6):
    return wait_for("the windows to settle", lambda: (lambda w: w if all(p["settled"] for p in w["placed"]) else None)(windows3d()), timeout, 0.3)


@section("15", "carrying windows: G, clicks, Esc, X, the wheel, Ctrl+wheel, hyprctl, and a placed window that keeps drawing")
def s_grab():
    ensure_avatar(AV)
    ensure_3d()
    menu_closed()
    ctl("hypr3d", "reset-windows")
    ctl("hypr3d", "view", "first")
    ctl("hypr3d", "spawn")
    time.sleep(1)
    a = aim_at("h3d-left")
    check("15", "the crosshair on the left terminal", a, a or f"yaw:aimed:panels {AIM.get('last')}; {ctl('-j', 'clients')[:300]}")
    press("g")
    w = windows3d()
    check("15", "G picks it up: holding it", st()["holding"] is True and len(w["placed"]) == 1 and w["placed"][0]["held"] and w["placed"][0]["class"] == "h3d-left", w)
    frame("holding")
    d0, s0 = w["hold"]["dist"], w["hold"]["size"]
    wheel(2)
    d1 = windows3d()["hold"]["dist"]
    check("15", "the wheel, two notches down: it comes closer (0.9 of the way a notch)", abs(d1 - d0 * 0.81) < 0.01, f"{d0:.3f} m -> {d1:.3f} m")
    qmp([key_event("ctrl", True)])
    time.sleep(0.1)
    wheel(2)
    qmp([key_event("ctrl", False)])
    time.sleep(0.3)
    s1 = windows3d()["hold"]["size"]
    check("15", "Ctrl+wheel, two notches down: smaller (0.92 a notch)", abs(s1 - s0 * 0.8464) < 0.01, f"{s0:.3f} -> {s1:.3f}")
    w = settled()
    p = w["placed"][0]
    check("15", "... and it's drawn so: that far, that big", abs(p["distance"] - d1) < 0.05 and abs(p["size"] - s1) < 0.02, p)
    c0 = p["center"]
    rel(250, 0)
    w = settled()
    check("15", "turning carries it along", math.dist(w["placed"][0]["center"], c0) > 0.3, f"{c0} -> {w['placed'][0]['center']}")
    frame("carried")
    press("g")
    w = settled()
    here = w["placed"][0]["center"]
    check("15", "G again puts it down where it is", st()["holding"] is False and len(w["placed"]) == 1 and not w["placed"][0]["held"], w)
    rel(-250, 0)
    time.sleep(1)
    w = windows3d()
    check("15", "... and it stays there when you look away", math.dist(w["placed"][0]["center"], here) < 0.01, f"{here} -> {w['placed'][0]['center']}")
    rel(250, 0)
    time.sleep(0.8)
    a = st()["aimed"]
    press("g")
    click("left")
    time.sleep(0.5)
    check("15", "picked up again (it's under the crosshair), a left click puts it down", a and a["class"] == "h3d-left" and st()["holding"] is False, a)
    here = settled()["placed"][0]["center"]
    press("g")
    rel(300, 0)
    time.sleep(0.8)
    click("right")
    w = settled()
    check("15", "picked up and moved, a right click puts it back where it was", st()["holding"] is False and math.dist(w["placed"][0]["center"], here) < 0.02,
          f"{here} -> {w['placed'][0]['center']}")
    rel(-300, 0)
    time.sleep(0.8)
    press("g")
    rel(300, 0)
    time.sleep(0.8)
    press("esc")
    w = settled()
    check("15", "... so does Esc, and 3D stays on", st()["mode"] == "active" and st()["holding"] is False and math.dist(w["placed"][0]["center"], here) < 0.02,
          f"{st()['mode']}; {here} -> {w['placed'][0]['center']}")
    rel(-300, 0)
    time.sleep(0.8)
    a = st()["aimed"]
    press("x")
    w = wait_for("it back on the wall", lambda: (lambda w: w if not w["placed"] else None)(windows3d()), 8, 0.3)
    check("15", "X sends the window under the crosshair back to the wall", a and a["class"] == "h3d-left" and w is not None, f"aimed {a and a['class']}; {windows3d()}")
    # from the wall, picked up and let go with a right click: back to the wall
    ctl("hypr3d", "spawn")
    time.sleep(0.8)
    aim_at("h3d-right", step=15)
    press("g")
    rel(-200, 0)
    time.sleep(0.8)
    click("right")
    w = wait_for("back on the wall", lambda: (lambda w: w if not w["placed"] else None)(windows3d()), 8, 0.3)
    check("15", "a window from the wall, picked up and let go with a right click: back on the wall", w is not None, windows3d())
    # hyprctl
    ctl("hypr3d", "spawn")
    time.sleep(0.8)
    aim_at("h3d-left")
    r1 = ctl("hypr3d", "grab")
    r2 = ctl("hypr3d", "hold", "1.5", "0.5")
    w = windows3d()
    check("15", "hyprctl hypr3d grab, hold 1.5 0.5: holding it 1.5 m out at half size", r1 == "holding" and r2 == "ok" and w["hold"] == {"dist": 1.5, "size": 0.5}, f"{r1}, {r2}; {w['hold']}")
    r3 = ctl("hypr3d", "place")
    s = st()
    check("15", "hyprctl hypr3d place: the status says placed 1, holding false", r3 == "placed" and s["placed"] == 1 and s["holding"] is False, f"{r3}; {s['placed']} {s['holding']}")
    r4 = ctl("hypr3d", "grab")
    r5 = ctl("hypr3d", "grab")
    check("15", "hyprctl hypr3d grab twice: picked up, then put down (like G)", r4 == "holding" and r5 == "placed", f"{r4}, {r5}")
    # a placed window keeps drawing, even with its workspace hidden: a counter in it
    machine.succeed("printf 'i=0\\nwhile sleep 0.2; do i=$((i+1)); echo tick $i; done\\n' > /tmp/tick.sh && chmod 644 /tmp/tick.sh")
    press("e")
    type_text("sh /tmp/tick.sh")
    press("ret")
    press("meta_l", "esc")
    ctl("dispatch", 'hl.dsp.focus({ workspace = "2" })')
    time.sleep(1.5)
    ws = json.loads(ctl("-j", "activeworkspace"))["id"]
    box = (440, 200, 840, 600)
    f1 = frame("placed-counter-1")
    time.sleep(1.2)
    f2 = frame("placed-counter-2")
    d = f1.differs(f2, box, thresh=40, step=1)
    check("15", "on another workspace, the placed window is still drawn and keeps updating (its counter)", ws == 2 and d > 0.001 and len(windows3d()["placed"]) == 1,
          f"workspace {ws}, {d:.2%} of the middle changed in 1.2 s")
    ctl("dispatch", 'hl.dsp.focus({ workspace = "1" })')
    time.sleep(0.5)
    r = ctl("hypr3d", "reset-windows", "forget")  # (forget: where they were put isn't kept for their classes either)
    w = wait_for("everything back on the wall", lambda: (lambda w: w if not w["placed"] else None)(windows3d()), 8, 0.3)
    check("15", "hyprctl hypr3d reset-windows: all back on the wall", r == "ok" and w is not None and windows3d()["spots"] == 0, windows3d())
    machine.execute("pkill -f tick.sh; true")


@section("16", "a second monitor: 3D on one while the other stays 2D, then on the other; the pointer, focus, notifications")
def s_monitors():
    ensure_avatar(AV)
    ensure_3d(False)
    # Hyprland's own headless output, right of the first (QEMU shows the second virtio-gpu output only in a window);
    # the first where it is (at "auto" Hyprland lays them out again, the new one first)
    ctl("eval", 'hl.monitor({ output = "Virtual-1", mode = "preferred", position = "0x0", scale = 1 })')
    r = ctl("output", "create", "headless", "H3D-2")
    wait_for("H3D-2", lambda: "H3D-2" in monitors(), 10)
    ctl("eval", 'hl.monitor({ output = "H3D-2", mode = "1280x800@60", position = "1280x0", scale = 1 })')
    m = wait_for("H3D-2 at 1280x0", lambda: (lambda m: m if m.get("H3D-2", {}).get("x") == 1280 and m["H3D-2"]["width"] == 1280 and m["Virtual-1"]["x"] == 0 else None)(monitors()), 10)
    check("16", "hyprctl output create headless: a second monitor, right of the first", r == "ok" and m, {n: (v["x"], v["width"], v["height"]) for n, v in m.items()})
    ctl("dispatch", 'hl.dsp.focus({ monitor = "H3D-2" })')
    alice("setsid -f foot --app-id h3d-second > /dev/null 2>&1")
    c = wait_for("its terminal", lambda: next((c for c in json.loads(ctl("-j", "clients")) if c["class"] == "h3d-second"), None), 20)
    time.sleep(1)
    check("16", "a terminal on it", c and c["monitor"] == monitors()["H3D-2"]["id"], c and c["monitor"])
    ctl("dispatch", 'hl.dsp.focus({ monitor = "Virtual-1" })')
    ctl("dismissnotify")
    time.sleep(1.2)
    two2d = frame("second-2d", "H3D-2")
    one2d = frame("first-2d", "Virtual-1")
    ensure_3d()
    time.sleep(1)
    s = st()
    check("16", "hypr3d on: 3D on the focused monitor", s["monitor"] == "Virtual-1", s["monitor"])
    one = frame("first-3d", "Virtual-1")
    two = frame("second-while-first-3d", "H3D-2")
    d1, d2 = one2d.differs(one), two2d.differs(two)
    check("16", "grim -o: the first shows 3D, the second its desktop as it was", d1 > 0.3 and d2 < 0.003, f"{d1:.1%} and {d2:.2%} of pixels changed")
    pos = ctl("cursorpos")
    y0 = st()["yaw"]
    rel(900, 0)
    time.sleep(0.5)
    check("16", "in 3D the mouse turns the camera and stays off the other monitor", ctl("cursorpos") == pos and abs(st()["yaw"] - y0) > 20 and focused_monitor() == "Virtual-1",
          f"cursor {pos} -> {ctl('cursorpos')}, yaw {y0} -> {st()['yaw']}, focused {focused_monitor()}")
    ctl("notify", "1", "8000", "rgb(ff8800)", "hypr3d: on the focused monitor, over its 3D view")
    time.sleep(1.2)
    one, two = frame("notification-first-3d", "Virtual-1"), frame("notification-second", "H3D-2")
    n1, n2 = len(notification_boxes(one)), len(notification_boxes(two))
    check("16", "a notification shows on the focused monitor, over 3D, and not on the other", n1 == 1 and n2 == 0, f"{n1} and {n2}")
    ctl("dismissnotify")
    ensure_3d(False)
    rel(700, 0)
    rel(700, 0)
    x = float(ctl("cursorpos").split(",")[0])
    check("16", "out of 3D, the mouse crosses to the second monitor, which takes the focus", x > 1280 and focused_monitor() == "H3D-2", f"x {x}, focused {focused_monitor()}")
    ensure_3d()
    time.sleep(1)
    s = st()
    check("16", "hypr3d on again: 3D on the second monitor now", s["monitor"] == "H3D-2", s["monitor"])
    one, two = frame("first-while-second-3d", "Virtual-1"), frame("second-3d", "H3D-2")
    d1, d2 = one2d.differs(one), two2d.differs(two)
    # (the first's window has lost the focus: its border has the inactive colour)
    check("16", "... its frame is 3D, the first's its desktop", d2 > 0.3 and d1 < 0.02, f"{d2:.1%} and {d1:.2%} of pixels changed")
    ctl("notify", "1", "8000", "rgb(ff8800)", "hypr3d: over the second monitor's 3D view")
    time.sleep(1.2)
    two = frame("notification-second-3d", "H3D-2")
    check("16", "... notifications show over it", len(notification_boxes(two)) == 1, notification_boxes(two))
    ctl("dismissnotify")
    # the monitor goes away while in 3D on it. aquamarine before 0.12.1 (Hyprland 0.55.2 has 0.11.0) queues a headless
    # output's late frame as an idle event that points at the output, and runs it after the output is freed if that
    # comes first (fixed upstream by 1699271 and 6ecde03). In 3D, where the plugin asks for each frame as soon as the
    # last is out, one is nearly always queued so: Hyprland crashed here (CBackend::dispatchIdle -> a freed signal), or
    # its heap was corrupted and malloc aborted later (in section 17, linking the map's shaders). The plugin holds the
    # output until that event has run (holdOutput() in main.cpp)
    dumps = coredumps()
    r = ctl("output", "remove", "H3D-2")
    time.sleep(1)
    if not alive():
        try:
            stack = wait_for("the core dump", lambda: (lambda t: t if t and "Stack trace" in t else None)(new_coredump(dumps)), 30, 1)
        except TimeoutError:
            stack = ""
        aq = "_ZN10Aquamarine8CBackend12dispatchIdleEv" in stack and "emitInternal" in stack
        check("16", "the second monitor removed while in 3D on it: back to 2D, Hyprland fine", False,
              f"{r}; Hyprland crashed{' in aquamarine (CBackend::dispatchIdle): the output went before its queued frame' if aq else ''}; the rest of the section skipped")
        restart_after_crash()
        return
    ok = wait_for("2D", lambda: st()["mode"] == "off", 10)
    check("16", "the second monitor removed while in 3D on it: back to 2D, Hyprland fine", r == "ok" and ok and alive() and "H3D-2" not in monitors(), f"{r}; {st()['mode']}")
    held = machine.execute("journalctl -t start-hyprland --no-pager -n 3000 | grep 'hypr3d.*H3D-2.*held' || true")[1].strip()
    check("16", "... its output held until aquamarine's idle events queued before it had run", held, held.split("[hypr3d] ")[-1])
    time.sleep(1)
    img = calm_frame("first-after-second-gone")
    d = one2d.differs(img)
    check("16", "... the first monitor's desktop as it was", d < 0.01, f"{d:.2%} of pixels differ")
    machine.execute("pkill -f 'foot --app-id h3d-second'; true")
    ensure_3d()
    check("16", "3D again on the first", st()["monitor"] == "Virtual-1", st()["monitor"])
    ensure_3d(False)



def mean(img, box):
    """the mean colour in a box given as fractions of the frame (x0 y0 x1 y1)"""
    x0, y0, x1, y1 = (int(box[0] * img.w), int(box[1] * img.h), int(box[2] * img.w), int(box[3] * img.h))
    s, n = [0, 0, 0], 0
    for y in range(y0, y1):
        row = y * img.w * 3
        for x in range(x0, x1):
            i = row + x * 3
            s[0] += img.px[i]
            s[1] += img.px[i + 1]
            s[2] += img.px[i + 2]
            n += 1
    return tuple(v / max(n, 1) for v in s)


def frac(img, box):
    return (int(box[0] * img.w), int(box[1] * img.h), int(box[2] * img.w), int(box[3] * img.h))


@section("17", "a map with a game's own lighting, as tools/cs2map.py writes it (litmap.py's LitCourt.glb)")
def s_litmap():
    # (what litmap.py's header says a correct render shows from the spawn, with the margin llvmpipe needs)
    ensure_avatar(AV)
    ensure_3d(False)
    r = ctl("eval", f'hl.config({{ plugin = {{ hypr3d = {{ map = "{LIT}" }} }} }})')
    m = wait_for("LitCourt", lambda: (lambda m: m if not m["loading"] and m["world"] == "LitCourt" else None)(ctlj("hypr3d", "map")), 30)
    check("17", "plugin:hypr3d:map (through hyprctl eval): LitCourt loads", m and m["world"] == "LitCourt", f"{r}; {m}")
    journal = machine.execute("journalctl -t start-hyprland --no-pager -n 3000 | grep 'hypr3d.*LitCourt\\|hypr3d.*lighting\\|hypr3d.*backdrop'")[1]
    check("17", "its lighting: 2 lightmap sets, 3 probe volumes, fog, an exposure range; and a backdrop",
          "2 lightmap set(s), 3 light probe volumes, fog, exposure 0.35-0.7" in journal and "backdrop (hypr3d_backdrop)" in journal,
          "; ".join(l.split("[hypr3d] ")[-1] for l in journal.splitlines())[-400:])
    # (llvmpipe compiles the map's shaders at its first frames, and draws each slowly: hyprctl answers between frames)
    slow = lambda *a: json.loads(ctl(*a, timeout=240))
    ctl("hypr3d", "on", timeout=240)
    wait_for("3D in the map", lambda: slow("hypr3d", "status")["mode"] == "active", 400, 2)
    ctl("hypr3d", "view", "first", timeout=240)
    ctl("hypr3d", "spawn", timeout=240)
    time.sleep(3)
    s = slow("hypr3d", "status")
    check("17", "in 3D at its spawn, the exposure held to the map's range (0.35, the metering wants less)", s["world"] == "LitCourt" and abs(s["exposure"] - 0.35) < 0.02,
          f"{s['world']}, exposure {s['exposure']}")
    img = frame("litcourt-spawn", timeout=240)
    orange, teal = mean(img, (.15, .80, .40, .88)), mean(img, (.60, .80, .85, .88))
    check("17", "the floor's lightmap: its orange and teal checker", orange[0] - orange[2] > 45 and teal[2] - teal[0] > 10 and orange[1] > 100,
          f"orange {orange}, teal {teal}")
    band = mean(img, (.10, .735, .42, .775))
    check("17", "the sun's baked shadow: a darker band across the floor", sum(band) < 0.9 * sum(orange), f"{band} over {orange}")
    sky, hills = mean(img, (.02, .02, .30, .08)), mean(img, (.20, .20, .33, .29))
    check("17", "its own sky, violet", sky[0] > sky[1] + 40 and sky[2] > sky[1] + 30, sky)
    check("17", "the backdrop, past the far plane, lit by its own lightmap: green hills", hills[1] > hills[0] and hills[1] > hills[2] + 30, hills)
    n = img.count(lambda r, g, b: r > g + 50 and b > g + 40, frac(img, (.60, .38, .66, .62)))
    check("17", "the pillar, lit by the indoor probe volume (its priority): magenta", n > 3000, f"{n} magenta pixels")
    n = img.count(lambda r, g, b: g > b + 50 and g >= r - 5, frac(img, (.20, .60, .50, .70)))
    check("17", "the floor's blend layer (moss)", n > 3000, f"{n} moss pixels")
    n = img.count(lambda r, g, b: g > 150 and g > r + 40 and b > r + 30, frac(img, (.40, .34, .60, .40)))
    check("17", "the sign's self-illumination", n > 500, f"{n} glowing cyan pixels")
    box = frac(img, (.567, .649, .74, .721))
    dark, bright = img.count(lambda r, g, b: max(r, g, b) < 75, box), img.count(lambda r, g, b: min(r, g, b) > 200, box)
    check("17", "the mod2x decal darkens and brightens the floor", dark > 100 and bright > 100, f"{dark} dark, {bright} bright pixels")
    glass = mean(img, (.29, .45, .35, .58))
    check("17", "the glass pane isn't washed out (a light tint over the wall behind it)", max(glass) < 185, glass)
    # the sun (50 degrees up) glints off the pane where you look up at it from under it: white, not held to the pane's
    # cover (an opacity of 0.2 kept a glint under a quarter of white)
    ctl("hypr3d", "tp", "-3.961", "0", "-5.4845", timeout=240)
    ctl("hypr3d", "turn", "35", "50", timeout=240)
    time.sleep(3)
    img = frame("litcourt-glint", timeout=240)
    n = img.count(lambda r, g, b: min(r, g, b) > 245, frac(img, (.42, .40, .58, .60)))
    check("17", "the sun glints off the glass: white where it's reflected", n > 100, f"{n} white pixels")
    # the same court with a sun that has no baked shadow channel: CS2 then shadows it on every surface by the realtime
    # shadow alone, so the lightmapped floor is as sunlit as LitCourt's (it got no sun at all), and the band that only
    # the baked shadow has is gone
    r = ctl("eval", f'hl.config({{ plugin = {{ hypr3d = {{ map = "{LIT_RS}" }} }} }})')
    ok = wait_for("LitCourtRuntimeSun", lambda: slow("hypr3d", "status")["world"] == "LitCourtRuntimeSun", 240, 2)
    ctl("hypr3d", "spawn", timeout=240)
    time.sleep(3)
    img2 = frame("litcourt-runtime-sun", timeout=240)
    orange2, band2 = mean(img2, (.15, .80, .40, .88)), mean(img2, (.10, .735, .42, .775))
    check("17", "a sun with no baked shadow channel (LitCourtRuntimeSun): the lightmapped floor sunlit as LitCourt's",
          ok and all(abs(a - b) <= 12 for a, b in zip(orange, orange2)), f"{r}; {orange2}, LitCourt's {orange}")
    check("17", "... and only the realtime shadow: the baked shadow's band gone", sum(band2) > 0.95 * sum(orange2) and sum(band) < 0.9 * sum(orange),
          f"band {band2} over {orange2} (LitCourt's {band} over {orange})")
    note("17", "frames a second in the map", slow("hypr3d", "status")["fps"])
    ctl("hypr3d", "off", timeout=240)
    wait_for("2D", lambda: slow("hypr3d", "status")["mode"] == "off", 120, 1)
    ctl("eval", 'hl.config({ plugin = { hypr3d = { map = "" } } })')
    ok = wait_for("the courtyard", lambda: ctlj("hypr3d", "map")["world"] == "courtyard", 20)
    check("17", "map = \"\": back to the courtyard", ok)
    time.sleep(0.5)
    ctl("dismissnotify")  # ("back to the courtyard", so that the next section's frames don't have it)


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



# ------------------------------------------------------------------ play mode: games in the 3D world

GAME_LOG = "/tmp/game.log"


def game_start(args="", env="", cls="h3dgame", alone=True):
    """h3dgame (h3dgame.c) as alice, printing what SDL gives it to GAME_LOG; alone on its workspace (the terminals go)"""
    machine.execute("pkill -x h3dgame; true")
    if alone:
        machine.execute("pkill -u alice foot; true")
    time.sleep(0.8)
    # (by class: SDL takes it from the app id hint on Wayland, WM_CLASS on X11)
    alice(f"rm -f {GAME_LOG}; SDL_APP_ID={cls} SDL_VIDEO_WAYLAND_WMCLASS={cls} SDL_VIDEO_X11_WMCLASS={cls} {env} "
          f"setsid -f stdbuf -oL h3dgame --title {cls} {args} >> {GAME_LOG} 2>&1")
    c = wait_for(f"{cls}'s window", lambda: next((c for c in json.loads(ctl("-j", "clients")) if c["class"] == cls), None), 30)
    time.sleep(1.5)
    return c


def game_mark(name):
    alice(f"echo {shlex.quote('### ' + name)} >> {GAME_LOG}")


def game_lines(name=None):
    """what the game printed since mark `name` (to the next mark), or all of it"""
    out, on = [], name is None
    for line in machine.succeed(f"cat {GAME_LOG}").splitlines():
        if line.startswith("### "):
            if on and name is not None:
                break
            on = on or line[4:].strip() == name
            continue
        if on:
            out.append(line.strip())
    return out


def game_motion(lines):
    """relative motion, summed; the last absolute position"""
    dx = dy = 0
    at = None
    for l in lines:
        if l.startswith("motion "):
            x, y, rx, ry = (int(v) for v in l.split()[1:5])
            dx, dy, at = dx + rx, dy + ry, (x, y)
    return dx, dy, at


def game_frames(lines):
    return [int(l.split()[1]) for l in lines if l.startswith("frames ")]


def game_blue(r, g, b):
    """h3dgame's background: blue 90, red and green 40..240"""
    return abs(b - 90) <= 3 and 35 <= r <= 245 and 35 <= g <= 245 and max(r, g) > 30


def aim_find(cls, step=15, reach=40):
    """turns (hyprctl hypr3d turn) until the crosshair is on that window: across the view, at a few heights"""
    s = st()
    y0, seen = s["yaw"], []
    for pitch in (s["pitch"], 0, -6, 6, -12, 12, -18):
        for dy in range(0, 64, 3):
            for sign in ((1,) if dy == 0 else (1, -1)):
                ctl("hypr3d", "turn", f"{y0 + sign * dy:.1f}", f"{pitch:.1f}")
                time.sleep(0.2)
                a = st()["aimed"]
                if a and a.get("kind") == "window" and a.get("class") == cls:
                    return a
                seen.append(a and a.get("class"))
    AIM["last"] = f"never on {cls}: {sorted(set(map(str, seen)))}"
    return None


def aim_local(cls, x, y, tries=12):
    """walking: the crosshair on x, y of cls's window (panel-local px): found, then looked at by mouse counts, each
    step by what the first one moved"""
    a = aim_find(cls)
    gain = None
    for _ in range(tries):
        a = st()["aimed"]
        if not a or a.get("class") != cls:
            return None
        (lx, ly), (ex, ey) = a["local"], (x - a["local"][0], y - a["local"][1])
        if abs(ex) <= 3 and abs(ey) <= 3:
            return a
        if gain is None:
            ctl("hypr3d", "look", "15", "15")
            time.sleep(0.4)
            b = st()["aimed"] or a
            gx, gy = (b["local"][0] - lx) / 15, (b["local"][1] - ly) / 15
            gain = (gx if abs(gx) > 0.05 else 1, gy if abs(gy) > 0.05 else 1)
            continue
        ctl("hypr3d", "look", f"{ex / gain[0]:.0f}", f"{ey / gain[1]:.0f}")
        time.sleep(0.4)
    return st()["aimed"]


def play_on():
    r = ctl("hypr3d", "play", "on")
    if r != "playing":
        s = st()
        frame("play-on-failed")
        raise RuntimeError(f"hyprctl hypr3d play on: {r}; aimed {s['aimed']}, yaw {s['yaw']}, pitch {s['pitch']}, eye {s['eye']}, view {s['view']}; "
                           f"placed {[(p['class'], p['center'], p['settled']) for p in windows3d()['placed']]}; "
                           f"panels {[(p['kind'], p['class'], p['box'], p['placed']) for p in panels()]}")
    s = wait_for("play mode, the camera facing the window", lambda: (lambda s: s if s["playing"] and s["playing"]["view"] >= 1 else None)(st()), 10, 0.2)
    time.sleep(0.4)
    return r, s


APPS_KILLED = "h3dgame wev xterm swayidle mako obs chromium firefox electron supertux2 chocolate-doom hyprland-share-picker"


def clean_windows():
    """only the terminals: what the sections before may have left (every other window's process, then some by name)"""
    try:
        for c in json.loads(ctl("-j", "clients") or "[]"):
            if not c["class"].startswith("h3d-") and c.get("pid", 0) > 1:
                machine.execute(f"kill {c['pid']} 2>/dev/null; true")
    except ValueError:
        pass
    machine.execute(" ; ".join(f"pkill -x {a}" for a in APPS_KILLED.split()) + " ; pkill -f tkapp[.]py; pkill -f weston-; pkill -f chromium; pkill -f firefox; "
                    "pkill -f electron; pkill -f supertux; pkill -f obs-studio; pkill -f '[.]obs-wrapped'; pkill -f 'bin/[.]?mako'; pkill -x fcitx5; true")
    try:
        wait_for("the other windows gone", lambda: all(c["class"].startswith("h3d-") for c in json.loads(ctl("-j", "clients"))), 20, 0.5)
    except TimeoutError:
        pass


def gone(cls_part, timeout=15):
    """till no window's class has this in it"""
    try:
        wait_for(f"{cls_part} gone", lambda: not any(cls_part in c["class"].lower() for c in json.loads(ctl("-j", "clients"))), timeout, 0.5)
    except TimeoutError:
        pass


def app_logs():
    """the apps' own logs, to logs/apps"""
    (LOGS / "apps").mkdir(exist_ok=True)
    for f in machine.execute("ls /tmp/*.log 2>/dev/null || true")[1].split():
        machine.execute(f"chmod 644 {f}; true")
        copy_out(f, "logs/apps")


@section("18", "play mode (P): a game gets the keyboard, the buttons, the wheel and the mouse as it wants it; the camera faces it")
def s_play():
    lua_session()
    ensure_avatar(AV)
    ensure_3d(False)
    clean_windows()
    c = game_start("--relative")
    check("18", "h3dgame (SDL, Wayland) runs, its pointer locked (relative mode)", c and "relative on" in game_lines(), game_lines()[:6])
    ensure_3d()
    menu_closed()
    ctl("hypr3d", "view", "first")
    ctl("hypr3d", "spawn")
    time.sleep(1)
    game_mark("walking")
    y0 = st()["yaw"]
    rel(60, 0)
    s = st()
    dx, dy, _ = game_motion(game_lines("walking"))
    check("18", "walking: the mouse turns the camera, and the game gets none of it", abs(s["yaw"] - y0) > 3 and dx == 0, f"yaw {y0:.1f} -> {s['yaw']:.1f}, game dx {dx}")
    rel(-60, 0)
    a = aim_find("h3dgame")
    check("18", "the crosshair on the game", a, a or AIM.get("last"))
    feet, yaw = st()["feet"], st()["yaw"]
    game_mark("P")
    press("p")
    s = wait_for("play mode", lambda: (lambda s: s if s["playing"] and s["playing"]["view"] >= 1 else None)(st()), 10, 0.2)
    time.sleep(0.5)
    p = s["playing"]
    check("18", "P: playing it, its pointer lock active (the keyboard focus on it)", p and p["class"] == "h3dgame" and p["locked"] and json.loads(ctl("-j", "activewindow")).get("class") == "h3dgame", p)
    img = frame("play-relative")
    fill = img.count(game_blue, step=4) / ((img.w // 4) * (img.h // 4))
    check("18", "the camera faces it: the game fills most of the view", fill > 0.6, f"{fill:.0%} of the frame is the game's background")
    cx, cy = img.w // 2, img.h // 2
    cross = img.count(lambda r, g, b: (g > 200 and r < 140 and b < 150) or (r > 230 and 180 < g < 230 and b < 140), (cx - 12, cy - 12, cx + 13, cy + 13))
    check("18", "no crosshair over it", cross < 3, f"{cross} crosshair pixels in the middle")
    game_mark("mouse")
    before = frame("play-before-mouse")
    rel(100, 0)
    rel(0, 50)
    s = st()
    dx, dy, _ = game_motion(game_lines("mouse"))
    check("18", "the mouse: relative motion reaches the game (100 counts right, 50 down)", 90 <= dx <= 110 and 45 <= dy <= 55, f"dx {dx}, dy {dy}")
    check("18", "... and neither turns the camera nor walks", abs(s["yaw"] - yaw) < 0.01 and math.dist(s["feet"], feet) < 0.01, f"yaw {yaw} -> {s['yaw']}, feet {feet} -> {s['feet']}")
    d = before.differs(frame("play-after-mouse"), (cx - 200, cy - 100, cx + 200, cy + 100))
    check("18", "... and the game draws what it got: the frame changes", d > 0.5, f"{d:.0%} of the middle changed")
    game_mark("keys")
    press("w")
    press("a")
    press("esc")
    press("tab")
    got = [l for l in game_lines("keys") if l.startswith("key down")]
    s = st()
    check("18", "keys, Esc and Tab too, reach the game", got == ["key down W", "key down A", "key down Escape", "key down Tab"], got)
    check("18", "... and not the player: no walking, no Action Menu, still in 3D", math.dist(s["feet"], feet) < 0.01 and not menu().get("open") and s["mode"] == "active",
          f"feet {s['feet']}, menu {menu().get('open')}, {s['mode']}")
    game_mark("buttons")
    click("left")
    click("right")
    wheel(1)
    got = [" ".join(l.split()[:3]) for l in game_lines("buttons") if l.startswith(("button", "wheel"))]
    check("18", "buttons and the wheel reach the game", got[:4] == ["button down 1", "button up 1", "button down 3", "button up 3"] and any(l.startswith("wheel 0 -") for l in got), got)
    # SDL's relative mode off (R, in the game): no lock, a pointer that moves over the window as over a monitor
    game_mark("pointer")
    press("r")
    s = wait_for("the lock gone", lambda: (lambda s: s if s["playing"] and not s["playing"]["locked"] else None)(st()), 5, 0.2)
    rel(40, 0)
    x0 = game_motion(game_lines("pointer"))[2]
    rel(60, 30)
    x1 = game_motion(game_lines("pointer"))[2]
    check("18", "without a lock the pointer moves over it: the game gets positions that follow the mouse", s and x0 and x1 and x1[0] > x0[0] and x1[1] > x0[1],
          f"{x0} -> {x1}")
    rel(3000, 0)
    rel(0, 3000)
    x2 = game_motion(game_lines("pointer"))[2]
    w, h = c["size"]
    check("18", "... and stays on it, as on a monitor: at its bottom right corner", x2 and w - 3 <= x2[0] <= w and h - 3 <= x2[1] <= h, f"{x2}, the window {w}x{h}")
    img = frame("play-pointer")
    s = st()
    check("18", "the game's own cursor is drawn where the pointer is (there's no other)", s["cursor"] is not None, s["cursor"])
    press("r")
    wait_for("the lock again", lambda: st()["playing"]["locked"], 5, 0.2)
    # fullscreen, as games ask for it: the monitor's size, and the camera fits it again
    game_mark("fullscreen")
    press("f")
    got = wait_for("fullscreen", lambda: [l for l in game_lines("fullscreen") if l.startswith("size ")], 10, 0.3)
    time.sleep(1.2)
    img = frame("play-fullscreen")
    fill = img.count(game_blue, step=4) / ((img.w // 4) * (img.h // 4))
    s = st()
    check("18", "F in the game: fullscreen at the monitor's size, still playing it, and it still fills the view",
          got and got[-1] == "size 1280 800" and s["playing"] and fill > 0.6, f"{got}, {fill:.0%}")
    press("f")
    time.sleep(1)
    # Super+Esc gives everything back
    game_mark("super-esc")
    press("meta_l", "esc")
    time.sleep(0.8)
    s = st()
    got = game_lines("super-esc")
    check("18", "Super+Esc: play mode ends (walking again), and the game gets no Esc from it",
          s["playing"] is None and s["typing"] is False and not any("Escape" in l for l in got), f"{s['playing']}, typing {s['typing']}; {got}")
    eye = s["eye"]
    check("18", "... and the camera is back with the player", abs(eye[1] - (s["feet"][1] + 1.65)) < 0.05 and math.dist([eye[0], eye[2]], [s["feet"][0], s["feet"][2]]) < 0.05,
          f"eye {eye}, feet {s['feet']}")
    rel(60, 0)
    check("18", "... and the mouse turns the camera again", abs(st()["yaw"] - yaw) > 3)
    rel(-60, 0)
    # a game controller: SDL reads it from /dev/input itself, while the game has the keyboard focus
    aim_find("h3dgame")
    play_on()
    game_mark("pad-playing")
    machine.succeed(f"python3 {H}/gamepad.py a b start dx=1 lx=30000", timeout=60)
    time.sleep(0.5)
    got = [l for l in game_lines("pad-playing") if l.startswith(("controller", "cbutton down", "caxis"))]
    check("18", "a game controller in play mode (uinput, an Xbox 360 pad): the game reads it", "cbutton down a" in got and "cbutton down b" in got and "cbutton down start" in got and
          "cbutton down dpright" in got and any(l.startswith("caxis leftx 3") for l in got), got)
    ctl("hypr3d", "play", "off")
    time.sleep(0.5)
    game_mark("pad-walking")
    machine.succeed(f"python3 {H}/gamepad.py a", timeout=60)
    got = [l for l in game_lines("pad-walking") if l.startswith("cbutton down")]
    check("18", "... and walking, while the game still has the keyboard focus", got == ["cbutton down a"], got)
    alice("setsid -f foot --app-id h3d-left > /dev/null 2>&1")
    # (Hyprland doesn't focus a new window while the focused one has its pointer locked: as on the 2D desktop)
    wait_for("a terminal", lambda: any(c["class"] == "h3d-left" for c in json.loads(ctl("-j", "clients"))), 20)
    check("18", "a new window doesn't take the keyboard from a game with its pointer locked (Hyprland's rule, in 2D too)",
          json.loads(ctl("-j", "activewindow")).get("class") == "h3dgame", json.loads(ctl("-j", "activewindow")).get("class"))
    ctl("dispatch", 'hl.dsp.focus({ window = "class:h3d-left" })')
    wait_for("the terminal focused", lambda: json.loads(ctl("-j", "activewindow")).get("class") == "h3d-left", 10)
    time.sleep(0.5)
    game_mark("pad-unfocused")
    machine.succeed(f"python3 {H}/gamepad.py a", timeout=60)
    got = [l for l in game_lines("pad-unfocused") if l.startswith("cbutton down")]
    check("18", "... but not once another window has the keyboard (SDL drops a controller's events then)", got == [], got)
    # leaving 3D ends it
    ctl("hypr3d", "spawn")
    time.sleep(0.8)
    aim_find("h3dgame")
    play_on()
    ensure_3d(False)
    ensure_3d()
    check("18", "leaving 3D ends play mode", st()["playing"] is None)
    # so does a screen lock (swaylock: ext-session-lock-v1), and 3D with it: the lock gets the keyboard
    ctl("hypr3d", "spawn")
    time.sleep(0.8)
    aim_find("h3dgame")
    play_on()
    alice("setsid -f swaylock -c 203040 > /tmp/swaylock.log 2>&1")
    try:
        wait_for("3D to go", lambda: st()["mode"] == "off", 15, 0.3)
    except TimeoutError:
        pass
    time.sleep(1)
    s = st()
    frame("locked")
    check("18", "a screen lock (swaylock) ends play mode and 3D", s["mode"] == "off" and s["playing"] is None, f"mode {s['mode']}, playing {s['playing']}")
    type_text("h3d")
    press("ret")
    try:
        unlocked = wait_for("swaylock to go", lambda: machine.execute("pgrep -f 'bin/[.]?swaylock'")[0] != 0, 15, 0.5)
    except TimeoutError:
        unlocked = False
    check("18", "... and the keys reach the lock: the password typed unlocks it", unlocked, machine.execute("tail -n 3 /tmp/swaylock.log")[1][-200:])
    if not unlocked:  # (a lock whose client is gone stays locked: a new session for the sections after this)
        machine.execute("pkill -f 'bin/[.]?swaylock'; true")
        restart_after_crash()
        game_start("--relative", alone=False)
    ensure_3d()
    # how a game paces itself: its frames a second, on the 2D desktop, walking in 3D, played
    ensure_3d(False)
    machine.execute("pkill -u alice foot; true")
    time.sleep(3)
    f2d = game_frames(game_lines())[-2:]
    ensure_3d()
    ctl("hypr3d", "spawn")
    time.sleep(3)
    f3d = game_frames(game_lines())[-2:]
    fps3d = st()["fps"]
    aim_find("h3dgame")
    play_on()
    time.sleep(3)
    fplay = game_frames(game_lines())[-2:]
    fpsplay = st()["fps"]
    note("18", "the game's frames a second (vsync, FIFO): 2D, walking in 3D, played", f"{f2d}, {f3d} (3D at {fps3d:.1f} fps), {fplay} (3D at {fpsplay:.1f} fps)")
    check("18", "in 3D the game keeps drawing, paced by the 3D view's frames", f3d and fplay and min(f3d + fplay) > 0 and abs(fplay[-1] - fpsplay) < max(3, 0.3 * fpsplay),
          f"{f3d}, {fplay}; 3D {fps3d:.1f}, {fpsplay:.1f} fps")
    ctl("hypr3d", "play", "off")
    machine.execute("pkill -x h3dgame; true")


@section("18b", "what a game needs from the compositor in 3D: activated, wl_output enter, presentation feedback, idle inhibit off its workspace")
def s_play_compositor():
    lua_session()
    ensure_avatar(AV)
    ensure_3d(False)
    clean_windows()
    # the activated state (xdg_toplevel), wl_output enter and the scale: wev prints them
    wev_start()
    ensure_3d()
    menu_closed()
    ctl("hypr3d", "view", "first")
    ctl("hypr3d", "spawn")
    time.sleep(1)
    alice("setsid -f foot --app-id h3d-left > /dev/null 2>&1")
    wait_for("a terminal", lambda: json.loads(ctl("-j", "activewindow")).get("class") == "h3d-left", 20)
    time.sleep(1)
    wev_mark("play wev")
    aim_find("wev")
    r, s = play_on()
    time.sleep(0.5)
    confs = wev_states("play wev")
    check("18b", "P on wev: its xdg_toplevel is configured activated", r == "playing" and confs and "activated" in confs[-1], confs[-3:])
    ctl("hypr3d", "play", "off")
    wev_mark("unplay wev")
    ctl("dispatch", 'hl.dsp.focus({ window = "class:h3d-left" })')
    time.sleep(1)
    confs = wev_states("unplay wev")
    check("18b", "... and not activated once another window has the keyboard", confs and "activated" not in confs[-1], confs[-3:])
    machine.execute("pkill -x wev; pkill -u alice foot; true")
    # presentation feedback: what's drawn in 3D is presented (Hyprland says "discarded" for what the 3D view covers)
    alice("rm -f /tmp/pres.log /tmp/pres-debug.log; WAYLAND_DEBUG=client setsid -f stdbuf -oL weston-presentation-shm -f > /tmp/pres.log 2> /tmp/pres-debug.log")
    wait_for("weston-presentation-shm", lambda: json.loads(ctl("-j", "clients")), 20)
    time.sleep(1)
    ctl("hypr3d", "spawn")
    time.sleep(3)
    machine.execute("pkill -INT -f weston-presentation-shm; true")
    time.sleep(1)
    out = machine.execute("cat /tmp/pres.log")[1]
    (LOGS / "presentation-shm.txt").write_text(out)
    # (a line per frame presented: "N: f2c .. ms, c2p .. ms, ..."; "discarded" per one that wasn't)
    presented = len(re.findall(r"^\s*\d+: f2c", out, re.M))
    discarded = len(re.findall(r"discarded", out, re.I))
    note("18b", "weston-presentation-shm in 3D", " | ".join(out.strip().splitlines()[-6:])[:600])
    check("18b", "weston-presentation-shm in 3D: its frames are presented, none discarded", presented > 0 and discarded == 0, f"presented {presented}, discarded {discarded}")
    # what it got, from its own protocol log (WAYLAND_DEBUG): the output it entered, and each feedback
    dbg = machine.execute("cat /tmp/pres-debug.log")[1]
    # (libwayland 1.23 on writes interface#id, before it interface@id)
    enters = re.findall(r"wl_surface[@#]\d+\.enter\(wl_output[@#]\d+\)", dbg)
    scale = re.findall(r"wl_surface[@#]\d+\.preferred_buffer_scale\((\d+)\)", dbg)
    fb = (len(re.findall(r"wp_presentation_feedback[@#]\d+\.presented\(", dbg)), len(re.findall(r"wp_presentation_feedback[@#]\d+\.discarded\(", dbg)))
    (LOGS / "presentation-shm-debug.txt").write_text(dbg[-20000:])
    check("18b", "... in its protocol log: wl_surface.enter for the monitor's wl_output, its scale, and the feedback presented", enters and fb[0] > 0 and fb[1] == 0,
          f"enter {enters[:2]}, preferred scale {scale[:2]}, presented/discarded {fb}")
    machine.execute("pkill -x weston-presentation-shm; true")
    # the screen doesn't blank while a game (SDL inhibits idling by default) is out in the world, its workspace hidden
    alice("rm -f /tmp/h3d-idle; setsid -f swayidle -w timeout 3 'touch /tmp/h3d-idle' resume 'rm -f /tmp/h3d-idle' > /tmp/swayidle.log 2>&1")
    time.sleep(5)
    check("18b", "(swayidle works: 5 s with no input, it went idle)", machine.execute("test -e /tmp/h3d-idle")[0] == 0)
    rel(5, 0)
    time.sleep(0.5)
    game_start()
    ctl("hypr3d", "spawn")
    time.sleep(0.8)
    aim_find("h3dgame")
    ctl("hypr3d", "grab")
    ctl("hypr3d", "place")
    ctl("dispatch", 'hl.dsp.focus({ workspace = "2" })')
    time.sleep(1)
    machine.execute("rm -f /tmp/h3d-idle")
    time.sleep(6)
    placed = windows3d()["placed"]
    idle = machine.execute("test -e /tmp/h3d-idle")[0] == 0
    frames = game_frames(game_lines())[-2:]
    check("18b", "a game placed in the world, its workspace hidden: still drawing, and the screen doesn't go idle (its idle inhibitor counts)",
          placed and not idle and frames and min(frames) > 0, f"placed {[p['class'] for p in placed]}, idle {idle}, frames {frames}")
    machine.execute("pkill -x h3dgame; true")
    time.sleep(6)
    check("18b", "... and once it's closed, it does", machine.execute("test -e /tmp/h3d-idle")[0] == 0)
    machine.execute("pkill -x swayidle; true")
    ctl("dispatch", 'hl.dsp.focus({ workspace = "1" })')
    ctl("hypr3d", "reset-windows", "forget")
    ensure_3d(False)
    alice("setsid -f foot --app-id h3d-left > /dev/null 2>&1; sleep 0.7; setsid -f foot --app-id h3d-right > /dev/null 2>&1")
    wait_for("two terminals", lambda: len(json.loads(ctl("-j", "clients"))) >= 2, 20)
    time.sleep(1)


# ------------------------------------------------------------------ X11 apps, through XWayland

TK_LOG = "/tmp/tk.log"


def x_display():
    """XWayland's display (Hyprland starts it when it finds the Xwayland binary)"""
    xs = wait_for("XWayland", lambda: machine.execute("ls /tmp/.X11-unix 2>/dev/null")[1].split(), 30, 0.5)
    return ":" + xs[0].lstrip("X")


def tk_start():
    machine.execute("pkill -f tkapp[.]py; true")
    alice(f"rm -f {TK_LOG}; DISPLAY={x_display()} setsid -f python3 -u {H}/tkapp.py >> {TK_LOG} 2>&1")
    return wait_for("the Tk window", lambda: next((c for c in json.loads(ctl("-j", "clients")) if "h3dtk" in c["class"].lower() and c["xwayland"]), None), 30)


def tk_mark(name):
    alice(f"echo {shlex.quote('### ' + name)} >> {TK_LOG}")


def tk_lines(name):
    out, on = [], False
    for line in machine.succeed(f"cat {TK_LOG}").splitlines():
        if line.startswith("### "):
            if on:
                break
            on = line[4:].strip() == name
            continue
        if on:
            out.append(line.strip())
    return out


def panels():
    return ctlj("hypr3d", "panels")


def pointer_to(x, y):
    """play mode: the pointer to x, y over the window played (window-local logical px), with the flat pointer speed
    set up in x11_checks, as counts"""
    s = st()
    if not s["playing"]:
        raise RuntimeError(f"not playing (moving the pointer to {x:.0f}, {y:.0f}); aimed {s['aimed']}")
    p = s["playing"]["pointer"]
    rel(round(x - p[0]), round(y - p[1]), after=0.4)
    return st()["playing"]["pointer"]


def x11_checks(item, scale=1):
    if not HYPR["pid"]:  # (run on its own, in the hidpi VM)
        start_hyprland("hyprland.lua", lua_config(scale=scale))
        ensure_plugin()
    ensure_avatar(AV)
    ensure_3d(False)
    clean_windows()
    ctl("eval", 'hl.config({ input = { accel_profile = "flat" } })')  # (counts are pixels: the pointer goes where it's sent)
    machine.execute("pkill -u alice foot; true")
    c = tk_start()
    check(item, "XWayland runs, and an X11 app (Tk) maps: a window", c and c["xwayland"], c and {k: c[k] for k in ("class", "size", "xwayland")})
    time.sleep(1.5)
    ensure_3d()
    menu_closed()
    ctl("hypr3d", "view", "first")
    ctl("hypr3d", "spawn")
    time.sleep(1)
    a = aim_find(c["class"])
    check(item, "the crosshair on the X11 window", a, a or AIM.get("last"))
    r, s = play_on()
    w, h = c["size"]
    # its canvas: a click where it's sent (X11 window coordinates: the canvas starts 180 px in, under the entry)
    tk_mark("click")
    at = pointer_to(w * 0.6, h * 0.6)
    click("left")
    at2 = pointer_to(w * 0.6 + 60, h * 0.6 + 30)
    click("left")
    got = [tuple(int(v) for v in l.split()[1:3]) for l in tk_lines("click") if l.startswith("click ")]
    # (X11 coordinates are logical ones: xwayland:force_zero_scaling is off)
    check(item, "clicks on its canvas land where the pointer is: 60 and 30 px apart, as sent", len(got) == 2 and abs((got[1][0] - got[0][0]) - 60) <= 2 and
          abs((got[1][1] - got[0][1]) - 30) <= 2, f"{got}; pointer {at} -> {at2}")
    pointer_to(60, h * 0.6)  # (off the canvas, onto its list: then in again)
    time.sleep(0.6)
    tk_mark("tooltip")
    pointer_to(w * 0.6 + 80, h * 0.6)
    time.sleep(1)
    tips = [p for p in panels() if p["kind"] == "popup" and p["class"] == c["class"]]
    shown = [l for l in tk_lines("click") + tk_lines("tooltip") if l.startswith("tooltip")]
    note(item, "the panels, and the X11 windows", f"{[(p['kind'], p['class'], p['box']) for p in panels()]}; "
         f"{[(c['class'], c['at'], c['size'], c.get('xwayland')) for c in json.loads(ctl('-j', 'clients'))]}")
    check(item, "on its canvas: its tooltip (an override-redirect window) shows, as a popup of the window", tips,
          f"{shown}; popups {[(p['box']) for p in tips]}")
    # the menu bar's File menu: an override-redirect window, a popup over its window; an item picked
    tk_mark("menu")
    pointer_to(18, 10)
    click("left")
    time.sleep(0.8)
    pops = [p for p in panels() if p["kind"] == "popup" and p["class"] == c["class"]]
    frame(f"{item}-x11-menu")
    check(item, "File in the menu bar: the menu opens, a popup of the window in 3D", pops, [p["box"] for p in pops])
    if pops:
        m = max(pops, key=lambda p: p["box"][3])  # (the menu, not a tooltip)
        box = m["box"]
        win = next(p for p in panels() if p["kind"] == "window" and p["class"] == c["class"])["box"]
        # "Open", the second of four: a quarter of the menu down, and half another
        pointer_to(box[0] - win[0] + box[2] / 2, box[1] - win[1] + box[3] * 3 / 8)
        click("left")
        time.sleep(0.5)
        check(item, "... and clicking its second item picks Open", "menu Open" in tk_lines("menu"), tk_lines("menu"))
    # typing into the entry at its top
    tk_mark("typing")
    pointer_to(w / 2, 45)
    click("left")
    type_text("hello")
    press("ret")
    time.sleep(0.5)
    check(item, "a click in its entry and typing (play mode: every key)", "text hello" in tk_lines("typing"), tk_lines("typing"))
    # the wheel over the list
    tk_mark("wheel")
    pointer_to(60, h * 0.6)
    wheel(3)
    got = tk_lines("wheel")
    check(item, "the wheel over its list: X11's buttons 5, the list scrolls", got.count("wheel down") >= 3 and any(l.startswith("scroll ") and float(l.split()[1]) > 0 for l in got), got)
    # a right-click menu, while the window is out in the world
    ctl("hypr3d", "play", "off")
    ctl("hypr3d", "spawn")
    time.sleep(0.8)
    aim_find(c["class"])
    ctl("hypr3d", "grab")
    ctl("hypr3d", "hold", "2.2", "0.8")
    rel(-120, 0)
    time.sleep(0.5)
    ctl("hypr3d", "place")
    settled()
    aim_find(c["class"])
    r, s = play_on()
    tk_mark("context")
    pointer_to(w * 0.6, h * 0.6)
    click("right")
    try:  # (at scale 2 on llvmpipe the menu can take a while)
        pops = wait_for("the context menu", lambda: [p for p in panels() if p["kind"] == "popup" and p["class"] == c["class"]], 4, 0.3)
    except TimeoutError:
        pops = []
    time.sleep(0.3)
    img = frame(f"{item}-x11-context-placed")
    check(item, "placed in the world, a right-click menu opens on it, carried along (a placed popup)", "button 3" in tk_lines("context") and pops and all(p["placed"] for p in pops),
          f"{tk_lines('context')}; {[(p['box'], p['placed']) for p in pops]}")
    if pops:
        m = max(pops, key=lambda p: p["box"][3])
        win = next(p for p in panels() if p["kind"] == "window" and p["class"] == c["class"])["box"]
        box = m["box"]
        pointer_to(box[0] - win[0] + box[2] / 2, box[1] - win[1] + box[3] / 2)  # "Copy", the middle one of three
        click("left")
        time.sleep(0.5)
        check(item, "... and its middle item picks Copy", "menu Copy" in tk_lines("context"), tk_lines("context"))
    ctl("hypr3d", "play", "off")
    ctl("hypr3d", "reset-windows", "forget")
    # SDL through XWayland (SDL_VIDEODRIVER=x11), relative mode, the way most Steam and Proton games run: on the 2D
    # desktop first (Hyprland's own), then played in 3D; the game should get the same
    machine.execute("pkill -f tkapp[.]py; true")
    ensure_3d(False)
    g = game_start("--relative", env=f"DISPLAY={x_display()} SDL_VIDEODRIVER=x11", cls="h3dgame-x11")
    check(item, "h3dgame through XWayland (SDL's x11 driver), relative mode", g and g["xwayland"] and "driver x11" in game_lines(), game_lines()[:4])

    def moves(mark):
        game_mark(mark)
        rel(100, 0)
        rel(0, 40)
        rel(1500, 0)  # (a sweep: fifteen PS/2 moves, as a quick turn in a game is)
        press("w")
        click("left")
        return game_lines(mark)
    d2 = moves("x11 2d")
    ensure_3d()
    ctl("hypr3d", "spawn")
    time.sleep(0.8)
    aim_find("h3dgame-x11")
    r, s = play_on()
    d3 = moves("x11 3d")
    note(item, "what SDL's x11 driver got, 2D then 3D", f"{d2[:8]} | {d3[:8]}")
    m2, m3 = game_motion(d2)[:2], game_motion(d3)[:2]
    check(item, "... played in 3D it gets what it gets on the 2D desktop: the same relative motion (100 right, 40 down, then a sweep of 1500), W and a click",
          abs(m3[0] - m2[0]) <= 3 and abs(m3[1] - m2[1]) <= 3 and "key down W" in d3 and any(l.startswith("button down 1") for l in d3),
          f"2D {m2}, 3D {m3}; {[l for l in d3 if not l.startswith(('motion', 'frames'))][:6]}")
    ctl("hypr3d", "play", "off")
    machine.execute("pkill -x h3dgame; true")
    # xterm: its cursor (the X server's, a surface of XWayland's) drawn where the crosshair is on it
    alice(f"DISPLAY={x_display()} setsid -f xterm -class h3dxterm > /dev/null 2>&1")
    wait_for("xterm", lambda: any(c["class"] == "h3dxterm" for c in json.loads(ctl("-j", "clients"))), 20)
    time.sleep(1)
    ctl("hypr3d", "spawn")
    time.sleep(0.8)
    aim_find("h3dxterm")
    time.sleep(0.5)
    s = st()
    frame(f"{item}-xterm-cursor")
    check(item, "xterm's own cursor (XWayland's cursor surface) is drawn at the crosshair", s["cursor"] and s["cursor"]["size"] != [32, 32], s["cursor"])
    machine.execute("pkill -x xterm; true")
    ctl("eval", 'hl.config({ input = { accel_profile = "" } })')
    ensure_3d(False)
    alice("setsid -f foot --app-id h3d-left > /dev/null 2>&1; sleep 0.7; setsid -f foot --app-id h3d-right > /dev/null 2>&1")
    wait_for("two terminals", lambda: len(json.loads(ctl("-j", "clients"))) >= 2, 20)
    time.sleep(1)


@section("19", "X11 apps through XWayland: a Tk app's clicks, typing, wheel, menus and tooltip as popups (placed too); SDL's x11 driver; xterm's cursor")
def s_x11():
    lua_session()
    x11_checks("19")


# ------------------------------------------------------------------ apps and windows in the world

APPS_DIR = f"{HOME}/.local/share/applications"
SPOTS = f"{HOME}/.local/state/hypr3d/windows/courtyard.conf"


def desktop_entry(name, text):
    machine.succeed(f"mkdir -p {APPS_DIR} && cat > {APPS_DIR}/{name}.desktop << 'H3D_EOF'\n[Desktop Entry]\nType=Application\n{text}\nH3D_EOF\n"
                    f"chown -R alice:users {HOME}/.local")


def placed(cls):
    return next((p for p in windows3d()["placed"] if p["class"] == cls), None)


def relative(p, s):
    """a placement's middle from the eye: (ahead, right, up), metres, by the player's yaw"""
    yaw = math.radians(s["yaw"])
    d = [p["center"][i] - s["eye"][i] for i in range(3)]
    return (d[0] * math.sin(yaw) - d[2] * math.cos(yaw), d[0] * math.cos(yaw) + d[2] * math.sin(yaw), d[1])


@section("20", "apps and windows: the Apps page (desktop entries, icons), launching into the world, app rules, remembered places, the Windows page, pinning, real sizes, fullscreen")
def s_apps():
    lua_session()
    ensure_avatar(AV)
    ensure_3d(False)
    clean_windows()
    ensure_3d()
    ctl("hypr3d", "reset-windows", "forget")
    ensure_3d(False)
    machine.execute("pkill -u alice foot; true")
    # a game and a chat app as desktop entries (foot's own icon, an SVG in hicolor), and favourites
    desktop_entry("h3d-test-game", f"Name=Test Game\nComment=Game\nExec=h3dgame --title h3dgame --log {GAME_LOG} %U\nIcon=foot\nStartupWMClass=h3dgame")
    desktop_entry("h3d-chat", "Name=Chat\nExec=foot --app-id discord\nIcon=foot")
    desktop_entry("h3d-hidden", "Name=Hidden\nExec=true\nNoDisplay=true")
    ctl("eval", 'hl.config({ plugin = { hypr3d = { apps = "h3d-test-game, Chat, foot --app-id h3d-fav" } } })')
    alice("setsid -f foot --app-id h3d-left > /dev/null 2>&1")
    wait_for("a terminal", lambda: json.loads(ctl("-j", "clients")), 20)
    ensure_3d()
    menu_closed()
    ctl("hypr3d", "view", "first")
    ctl("hypr3d", "spawn")
    time.sleep(1)
    apps = ctlj("hypr3d", "apps")
    ids = {a["id"]: a for a in apps}
    check("20", "hyprctl hypr3d apps: the desktop entries, not NoDisplay ones; the game's icon (foot's SVG) found",
          "h3d-test-game" in ids and "h3d-chat" in ids and "h3d-hidden" not in ids and ids["h3d-test-game"]["icon"] and ids["h3d-test-game"]["exec"] == f"h3dgame --title h3dgame --log {GAME_LOG}",
          {k: ids.get(k) for k in ("h3d-test-game", "h3d-chat")})
    press("q")
    time.sleep(1.5)  # (the icons load a few a frame)
    m = menu()
    labels = [i["label"] for i in m.get("items", [])]
    check("20", "Q opens the Action Menu's Apps page: the favourites (an entry by id, by name, a command) and All apps",
          m.get("path", "").endswith("apps") and labels[:4] == ["Test Game", "Chat", "foot", "All apps"], f"{m.get('path')}: {labels}")
    check("20", "... with the apps' icons as pictures", [i["picture"] for i in m["items"][:2]] == [True, True], [i["picture"] for i in m.get("items", [])])
    frame("apps-page")
    # the game, launched from it: in front of you, as a game is (the built-in rule: 2 m out, 1.3 m tall)
    s = st()
    press("1")
    g = wait_for("the game in the world", lambda: placed("h3dgame"), 30, 0.5)
    ahead, right, up = relative(g, s)
    check("20", "picked: it starts, and its window opens in front of you, not on the wall: 2 m ahead, 1.3 m tall (a game)",
          g and abs(ahead - 2.0) < 0.1 and abs(right) < 0.1 and abs(g["height"] - 1.3) < 0.05 and not menu().get("open"),
          f"ahead {ahead:.2f}, right {right:.2f}, up {up:.2f}, {g['height']:.2f} m tall")
    frame("launched-game")
    # a chat app by name, from hyprctl: at the left (the built-in rule for chat apps)
    r = ctl("hypr3d", "launch", "Chat")
    c = wait_for("the chat app in the world", lambda: placed("discord"), 30, 0.5)
    ahead, right, up = relative(c, st())
    check("20", "hyprctl hypr3d launch Chat: a chat app (class discord) opens at your left, 1.3 m out, 0.75 m tall",
          r.startswith("launched") and c and right < -0.8 and 0.5 < ahead < 1.4 and abs(c["height"] - 0.75) < 0.05, f"{r}; ahead {ahead:.2f}, right {right:.2f}, {c['height']:.2f} m")
    # a game Steam starts from its own process (steam://rungameid/ID): not the launched process's own, known by its
    # class (steam_app_ID); here the user's systemd starts it, so it has nothing of the launch's
    r = ctl("hypr3d", "launch", "systemd-run --user --setenv=SDL_APP_ID=steam_app_42 h3dgame --title steamfake # steam://rungameid/42")
    g = wait_for("the 'Steam' game in the world", lambda: placed("steam_app_42"), 30, 0.5)
    check("20", "launched as a Steam game (steam://rungameid/42) but started by another process: known by its class steam_app_42, in front of you as a game is",
          g and abs(g["height"] - 1.3) < 0.05, f"{r}; {g}")
    ctl("hypr3d", "window", "steam_app_42", "close")
    # a command, with a rule of the config's
    ctl("eval", 'hl.config({ plugin = { hypr3d = { app_rules = "h3d-raw: 1.0 0.5 right" } } })')
    time.sleep(1.5)
    r = ctl("hypr3d", "launch", "foot --app-id h3d-raw")
    f = wait_for("the command's window", lambda: placed("h3d-raw"), 30, 0.5)
    ahead, right, up = relative(f, st())
    check("20", "launch a command, with app_rules \"h3d-raw: 1.0 0.5 right\": 1 m out, 0.5 m tall, to the right",
          f and right > 0.8 and abs(f["height"] - 0.5) < 0.05, f"{r}; ahead {ahead:.2f}, right {right:.2f}, {f['height']:.2f} m")
    # put somewhere by hand, a class is remembered: its window opens there again, in 3D and after 2D
    a = aim_find("discord")
    press("g")
    rel(-60, 0)
    time.sleep(0.8)
    press("g")
    spot = settled()
    spot = placed("discord")["center"]
    saved = machine.execute(f"cat {SPOTS}")[1]
    check("20", "G, moved, G: the chat app's place is kept, by class, in the courtyard's file", a and "discord" in saved, saved.strip()[-160:])
    ctl("hypr3d", "window", "discord", "close")
    wait_for("it closed", lambda: not placed("discord"), 10, 0.3)
    ctl("hypr3d", "launch", "Chat")
    c = wait_for("the chat app again", lambda: placed("discord"), 30, 0.5)
    check("20", "launched again, it opens where it was put", math.dist(c["center"], spot) < 0.05, f"{spot} -> {c['center']}")
    ensure_3d(False)
    ctl("hypr3d", "window", "discord", "close")
    time.sleep(1)
    alice("setsid -f foot --app-id discord > /dev/null 2>&1")
    wait_for("it on the desktop", lambda: any(c["class"] == "discord" for c in json.loads(ctl("-j", "clients"))), 20)
    time.sleep(1)
    ensure_3d()
    c = wait_for("it back in its place", lambda: placed("discord"), 10, 0.3)
    check("20", "opened on the 2D desktop, entering 3D takes it to its place", c and math.dist(c["center"], spot) < 0.05, c)
    ctl("hypr3d", "spawn")
    time.sleep(0.8)
    aim_find("discord")
    press("x")
    time.sleep(1)
    check("20", "X sends it back to the wall and forgets its place", "discord" not in machine.execute(f"cat {SPOTS}")[1] and not placed("discord"),
          machine.execute(f"cat {SPOTS}")[1].strip()[-120:])
    # the Windows page: each window, and what to do with one
    ctl("hypr3d", "spawn")
    time.sleep(0.8)
    press("b")
    time.sleep(0.8)
    m = menu()
    labels = [i["label"] for i in m.get("items", [])]
    check("20", "B opens the Windows page: the windows, where they are", m.get("path", "").endswith("windows") and "h3dgame" in labels and len(labels) >= 3,
          f"{m.get('path')}: {[(i['label'], i['hint']) for i in m.get('items', [])]}")
    slot = next((i["slot"] for i in m.get("items", []) if i["label"] == "h3dgame"), None)
    press(str(slot or 1))
    m = menu()
    check("20", "... a window's page: focus, bring here, to the wall, pin, bigger, smaller, play, close",
          [i["label"] for i in m.get("items", [])] == ["Focus", "Bring here", "To the wall", "Pin to view", "Bigger", "Smaller", "Play", "Close"], [i["label"] for i in m.get("items", [])])
    press("4")
    time.sleep(0.8)
    s = st()
    g = placed("h3dgame")
    ahead, right, up = relative(g, s)
    pan = next((p for p in panels() if p["kind"] == "window" and p["class"] == "h3dgame"), {})
    check("20", "Pin to view: it's in the view's top right corner, over the world", g["pinned"] and right > 0.2 and up > 0.05 and ahead > 0.5 and pan.get("front"),
          f"ahead {ahead:.2f}, right {right:.2f}, up {up:.2f}, front {pan.get('front')}")
    menu_closed()
    frame("pinned")
    rel(300, -80)
    time.sleep(0.6)
    s2 = st()
    g2 = placed("h3dgame")
    a2 = relative(g2, s2)
    check("20", "... and stays there as you look around", abs(a2[1] - right) < 0.05 and a2[2] > 0.05, f"right {right:.2f} -> {a2[1]:.2f}, up {a2[2]:.2f} (pitch {s2['pitch']:.0f})")
    frame("pinned-turned")
    r = ctl("hypr3d", "window", "h3dgame", "unpin")
    time.sleep(0.5)
    rel(-300, 80)
    time.sleep(0.6)
    g3 = placed("h3dgame")
    check("20", "unpinned: it stays where it was in the world", r == "unpinned" and not g3["pinned"] and math.dist(g3["center"], g2["center"]) < 0.02, f"{g2['center']} -> {g3['center']}")
    # its real size: the app draws itself at it
    game_mark("bigger")
    size0 = next(c["size"] for c in json.loads(ctl("-j", "clients")) if c["class"] == "h3dgame")
    r = ctl("hypr3d", "window", "h3dgame", "bigger")
    time.sleep(1.2)
    cl = next(c for c in json.loads(ctl("-j", "clients")) if c["class"] == "h3dgame")
    got = [l for l in game_lines("bigger") if l.startswith("size ")]
    check("20", "bigger: its real size, a quarter more (floating now), and the game draws itself at it",
          cl["floating"] and abs(cl["size"][0] - size0[0] * 1.25) <= 2 and got and got[-1] == f"size {cl['size'][0]} {cl['size'][1]}", f"{r}; {size0} -> {cl['size']}; {got}")
    ctl("hypr3d", "window", "h3dgame", "bring")
    time.sleep(0.8)
    aim_find("h3dgame")
    press("g")
    size1 = cl["size"]
    qmp([key_event("shift", True)])
    time.sleep(0.1)
    wheel(2)
    qmp([key_event("shift", False)])
    time.sleep(1)
    press("g")
    cl = next(c for c in json.loads(ctl("-j", "clients")) if c["class"] == "h3dgame")
    check("20", "carrying it, Shift+wheel two notches down: its real size, 0.81 of it", abs(cl["size"][0] - size1[0] * 0.81) <= 3, f"{size1} -> {cl['size']}")
    # fullscreen, asked for by the app while it has the keyboard: played, filling the view; leaving fullscreen ends it
    aim_find("h3dgame")
    press("e")
    press("f")
    s = wait_for("played, fullscreen", lambda: (lambda s: s if s["playing"] else None)(st()), 10, 0.3)
    time.sleep(0.8)
    img = frame("fullscreen-played")
    fill = img.count(game_blue, step=4) / ((img.w // 4) * (img.h // 4))
    check("20", "the app goes fullscreen (F in it): it's played, filling the view", s["playing"]["class"] == "h3dgame" and fill > 0.6, f"{s['playing']}, {fill:.0%}")
    press("f")
    s = wait_for("not played", lambda: (lambda s: s if s["playing"] is None else None)(st()), 10, 0.3)
    check("20", "... and out of fullscreen (F again), walking again", s["playing"] is None and s["typing"] is False, s["typing"])
    # maximized (M in it): one of Hyprland's fullscreen modes, but it only makes the window bigger: not played
    time.sleep(0.5)
    size0 = next(c["size"] for c in json.loads(ctl("-j", "clients")) if c["class"] == "h3dgame")
    aim_find("h3dgame")
    press("e")
    press("m")
    time.sleep(1.5)
    cl = next(c for c in json.loads(ctl("-j", "clients")) if c["class"] == "h3dgame")
    s = st()
    check("20", "a maximize request (M in it): bigger, and not played (maximized is one of Hyprland's fullscreen modes too)",
          cl["fullscreen"] == 1 and s["playing"] is None and cl["size"][0] > size0[0], f"fullscreen {cl['fullscreen']}, {size0} -> {cl['size']}; playing {s['playing']}")
    press("m")
    time.sleep(0.8)
    ctl("hypr3d", "type", "off")
    ctl("hypr3d", "reset-windows", "forget")
    clean_windows()
    machine.execute("pkill -u alice foot; true")
    ensure_3d(False)
    alice("setsid -f foot --app-id h3d-left > /dev/null 2>&1; sleep 0.7; setsid -f foot --app-id h3d-right > /dev/null 2>&1")
    wait_for("two terminals", lambda: len(json.loads(ctl("-j", "clients"))) >= 2, 20)
    time.sleep(1)


# ------------------------------------------------------------------ everyday apps: browsers and Electron

PAGE = f"{H}/page.html"


PAGE_NOT = set()  # windows that were there before the page's app started


def page_window():
    """the window showing page.html (its title starts with "h3d[H]: ", H the page's height; a browser adds its name)"""
    return next((c for c in json.loads(ctl("-j", "clients")) if c["title"].startswith("h3d[") and c["address"] not in PAGE_NOT), None)


def page_title():
    c = page_window()
    return c["title"] if c else ""


def page_said(what, timeout=10):
    try:
        return wait_for(f"the page to say {what}", lambda: (lambda t: t if what in t else None)(page_title()), timeout, 0.3)
    except TimeoutError:
        return page_title()


def drag_by(dx, dy):
    """play mode: the left button held while the pointer moves by dx, dy, then let go, after a moment and a nudge (a
    browser starts a drag and drop in its own time, a slow one after the moves: the drop goes where the pointer
    moved last since)"""
    qmp([{"type": "btn", "data": {"down": True, "button": "left"}}])
    time.sleep(0.2)
    n = max(1, round(max(abs(dx), abs(dy)) / 20))
    for k in range(n):
        rel(round(dx * (k + 1) / n) - round(dx * k / n), round(dy * (k + 1) / n) - round(dy * k / n), after=0.05)
    time.sleep(0.6)
    rel(2, 0, after=0.2)
    rel(-2, 0, after=0.4)
    qmp([{"type": "btn", "data": {"down": False, "button": "left"}}])
    time.sleep(0.6)


def popups_of(cls):
    return [p for p in panels() if p["kind"] == "popup" and p["class"] == cls]


def drawn(cls):
    """every surface drawn in 3D for cls's windows, their boxes on the desktop: a popup's, a subsurface's (Firefox's
    <select> list and tooltips are subsurfaces of its window, not popups)"""
    return {tuple(b) for p in panels() if p["class"] == cls for b in p.get("surfaces", [])}


def local_mid(cls, box):
    """the middle of a box on the desktop, as play mode's pointer over cls's window has it (window-local px)"""
    w = next(p for p in panels() if p["kind"] == "window" and p["class"] == cls)
    return box[0] - w["box"][0] + box[2] / 2, box[1] - w["box"][1] + box[3] / 2


def page_dnd_2d(cls):
    """page_checks' drag and drop on the 2D desktop, with the tablet: what Hyprland and XWayland make of it there"""
    ensure_3d(False)
    time.sleep(1.5)
    c = next(x for x in json.loads(ctl("-j", "clients")) if x["class"] == cls)
    ui = max(0, c["size"][1] - int((re.search(r"^h3d\[(\d+)", c["title"]) or re.search("(0)", "0")).group(1)))
    m = next(x for x in json.loads(ctl("-j", "monitors")) if x["focused"])
    w, h = m["width"] / m["scale"], m["height"] / m["scale"]
    to = lambda x, y: tablet((c["at"][0] - m["x"] + x) * 32767 / w, (c["at"][1] - m["y"] + y) * 32767 / h, after=0.05)
    btn = lambda down: qmp([{"type": "btn", "data": {"down": down, "button": "left"}}])
    to(22, ui + 36)
    btn(True)
    for k in range(1, 7):
        to(22 + 20 * k, ui + 36)
    btn(False)
    time.sleep(0.5)
    to(60, ui + 36)
    btn(True)
    time.sleep(0.2)
    for k in range(1, 7):
        to(60 + 10 * k, ui + 36 + 20 * k)
    time.sleep(0.3)
    btn(False)
    time.sleep(0.6)
    t = page_said("dropped", 6)
    ensure_3d()
    return t


def gradient(r, g, b):
    """the page's fullscreen box: red to blue"""
    return g < 90 and (r > 120 or b > 120)


def page_checks(item, launch, name, pinch=True, dialog=True, log=None, dnd2d=False, cross=False):
    """page.html in `launch` (a browser, or Electron), launched from 3D and played: its popups (a <select>'s list, a
    tooltip, the context menu), text selected by dragging onto the clipboard, typing, drag and drop, a touchpad's
    scrolling and pinch, fullscreen, and a file dialog"""
    ensure_3d()
    menu_closed()
    ctl("hypr3d", "view", "first")
    ctl("hypr3d", "spawn")
    ctl("eval", 'hl.config({ input = { accel_profile = "flat" } })')
    time.sleep(0.8)
    PAGE_NOT.clear()
    PAGE_NOT.update(x["address"] for x in json.loads(ctl("-j", "clients")))
    r = ctl("hypr3d", "launch", launch)
    try:
        c = wait_for(f"{name} with the page", lambda: (lambda c: c if c and "loading" not in c["title"] else None)(page_window()), 120, 1)
    except TimeoutError:
        frame(f"{item}-no-page")
        raise RuntimeError(f"no window with the page: {[(x['class'], x['title']) for x in json.loads(ctl('-j', 'clients'))]}")
    time.sleep(2)
    c = page_window()
    cls = c["class"]
    inner = int((re.search(r"^h3d\[(\d+)", c["title"]) or re.search("(0)", "0")).group(1))
    ui = max(0, c["size"][1] - inner)  # (the page is at the bottom of the window, under the browser's bars)

    def top():
        """how far down the window the page starts, under the browser's bars as they are now (an info bar can come
        and go)"""
        c = page_window()
        m = re.search(r"^h3d\[(\d+)", c["title"]) if c else None
        return max(0, c["size"][1] - int(m.group(1))) if m else ui

    def page_to(x, y):
        """play mode's pointer to x, y on the page (CSS px)"""
        return pointer_to(x, top() + y)

    p = placed(cls)
    check(item, f"{name} launched from 3D opens in front of you", r.startswith("launched") and p and not p["pinned"], f"{r}; {cls}, {c['title']}; {p and p['distance']}")
    note(item, f"{name}'s window", f"{cls} {c['size']} xwayland {c['xwayland']}, the page {ui} px down")
    aim_find(cls)
    play_on()
    # a <select>: its list opens over the window (a popup; Firefox's, a subsurface); the keys pick in it
    shown = lambda before: [list(b) for b in sorted(drawn(cls) - before)]
    before = drawn(cls)
    page_to(120, 96)
    click("left")
    time.sleep(1.2)
    new = shown(before)
    frame(f"{item}-select")
    press("down")
    press("down")
    press("ret")
    t = page_said("selected")
    check(item, "a <select>: its list opens, drawn over the window in 3D, and picks", new and "selected three" in t, f"{new}; popups {[p['box'] for p in popups_of(cls)]}; {t}")
    # a title's tooltip
    before = drawn(cls)
    page_to(340, 96)
    time.sleep(2.5)
    new = shown(before)
    frame(f"{item}-tooltip")
    check(item, "a title's tooltip shows, drawn over the window", new, new)
    # the context menu: the page gets the right click, and the menu (an app's own in Electron, a native one here)
    page_to(200, 36)
    click("right")
    time.sleep(1.2)
    pops = popups_of(cls)
    frame(f"{item}-context-menu")
    t = page_title()
    check(item, "a right click: the page gets it, and the context menu opens, a popup", "context menu" in t and pops, f"{t}; {[p['box'] for p in pops]}")
    if pops and log and "Electron" in name:
        pointer_to(*local_mid(cls, pops[-1]["box"]))  # (its middle item)
        click("left")
        time.sleep(0.8)
        out = machine.execute(f"cat {log}")[1]
        check(item, "... clicking its middle item picks it (Paste)", "menu Paste" in out, [l for l in out.splitlines() if l.startswith("menu")])
    else:
        press("esc")
    time.sleep(0.6)
    # text selected by dragging, copied: the clipboard, as the other apps see it
    page_to(22, 36)
    drag_by(240, 0)
    press("ctrl", "c")
    t = page_said("copied")
    paste = as_alice("wl-paste -n", 15)[1]
    check(item, "text selected by dragging over it, Ctrl+C: on the clipboard (wl-paste has it)", "copied The quick" in t and paste.startswith("The quick"), f"{t}; wl-paste {paste[:40]!r}")
    # typing
    page_to(170, 155)
    click("left")
    type_text("hello")
    t = page_said("typed hello")
    check(item, "a click in its input, typing", "typed hello" in t, t)
    # drag and drop: the selection dragged into the input
    page_to(22, 36)
    drag_by(120, 0)
    page_to(60, 36)
    drag_by(60, 120)
    t = page_said("dropped", 6)
    dnd = "dropped" in t and "The quick" in t
    if not dnd and dnd2d:
        t2 = page_dnd_2d(cls)
        check(item, "drag and drop in 3D: as on the 2D desktop", not ("dropped" in t2 and "The quick" in t2), f"3D: {t}; 2D: {t2}")
        aim_find(cls)
        play_on()
    else:
        check(item, "drag and drop in 3D: selected text dragged into the input lands there", dnd, t)
    if cross:
        # between windows, walking: text selected in the page dragged with the crosshair (the button held, turning)
        # out of the window and onto another, wev on the wall, which the drag enters, offering the text
        page_to(22, 36)
        drag_by(120, 0)
        ctl("hypr3d", "play", "off")
        time.sleep(1)
        a = aim_local(cls, 60, top() + 36)
        wev_mark("dnd")
        qmp([{"type": "btn", "data": {"down": True, "button": "left"}}])
        time.sleep(0.3)
        ctl("hypr3d", "look", "40", "0")  # (a move with the button down: the drag starts)
        time.sleep(0.6)
        w = aim_find("wev")
        time.sleep(1.2)
        evs = wev_events("dnd")
        frame(f"{item}-dnd-across")
        qmp([{"type": "btn", "data": {"down": False, "button": "left"}}])
        time.sleep(0.8)
        entered = any(i == "wl_data_device" and e == "enter" for i, e, _ in evs)
        offers = [r for i, e, r in evs if i == "wl_data_offer" and e == "offer"]
        check(item, "drag and drop between windows, walking: selected text dragged out of the page, the crosshair onto another window (wev), which the drag enters offering it",
              entered and any("text/plain" in o for o in offers), f"from {a and a.get('local')} to {w and w.get('class')}; {[(i, e) for i, e, _ in evs if 'data' in i][:6]}; offers {offers[:3]}")
        aim_find(cls)
        play_on()
    # the touchpad: scrolling, and a pinch
    page_to(420, 290)
    touchpad(600)
    t = page_said("scrolled")
    check(item, "two fingers on a touchpad scroll it", "scrolled" in t and not t.endswith("scrolled 0"), t)
    # fullscreen, from the page: still played, filling the view
    page_to(140, 250)
    click("left")
    t = page_said("fullscreen on")
    time.sleep(1.5)
    img = frame(f"{item}-fullscreen")
    fill = img.count(gradient, step=4) / ((img.w // 4) * (img.h // 4))
    s = st()
    check(item, "fullscreen from the page: it fills the view, still played", "fullscreen on" in t and s["playing"] and fill > 0.5, f"{t}; {fill:.0%}")
    press("esc")
    t = page_said("fullscreen off")
    check(item, "... Esc: out of fullscreen", "fullscreen off" in t, t)
    # a file dialog (a window of its own, a dialog of the browser's): by the browser, out in the world
    if dialog:
        before = {x["address"] for x in json.loads(ctl("-j", "clients"))}
        page_to(420, 150)
        click("left")
        d = wait_for("a file dialog", lambda: next((x for x in json.loads(ctl("-j", "clients")) if x["address"] not in before), None), 30, 0.5)
        time.sleep(1.5)
        pd, pb = placed(d["class"]), placed(cls)
        frame(f"{item}-file-dialog")
        near = pd and pb and math.dist(pd["center"], pb["center"]) < 1.5
        check(item, "the file input: a file dialog opens, in the world by the window it's for", near, f"{d['class']} {d['title']!r}; {pd and pd['center']} by {pb and pb['center']}")
        # still played, the dialog with it (the keyboard is the dialog's): the pointer goes over it, Esc closes it
        dp = next((p for p in panels() if p["kind"] == "window" and p["class"] == d["class"]), None)
        at = pointer_to(*local_mid(cls, dp["box"])) if dp else None
        s = st()
        aimed = s["aimed"] or {}
        check(item, "... played along with the window: the pointer goes over the dialog, its cursor on it", s["playing"] and aimed.get("class") == d["class"] and s["cursor"],
              f"pointer {at}; aimed {aimed.get('kind')} {aimed.get('class')}; cursor {s['cursor']}")
        press("esc")
        try:
            shut = wait_for("the dialog to close", lambda: all(x["address"] != d["address"] for x in json.loads(ctl("-j", "clients"))), 10, 0.3)
        except TimeoutError:
            shut = False
        check(item, "... Esc: it closes, and the window is still played", shut and st()["playing"], f"{st()['playing']}")
        if not st()["playing"]:
            ctl("hypr3d", "window", d["address"], "close")
            time.sleep(1)
            aim_find(cls)
            play_on()
    # (last: a pinch zooms the page, and everything moves)
    if pinch:
        page_to(420, 290)
        touchpad("z1200")
        t = page_said("zoom")
        check(item, "a pinch on the touchpad zooms it (zwp_pointer_gestures_v1)", "zoom" in t and "zoom 1.00" not in t, t)
    ctl("hypr3d", "play", "off")
    if log:
        note(item, f"{name}'s log", machine.execute(f"tail -n 5 {log}")[1].strip()[-400:])
    ctl("hypr3d", "window", cls, "close")
    time.sleep(2)
    ctl("eval", 'hl.config({ input = { accel_profile = "" } })')
    ctl("hypr3d", "reset-windows", "forget")


@section("21", "a browser (Chromium, then Firefox) in 3D: popups, tooltips, the context menu, the clipboard, typing, drag and drop, touchpad scroll and zoom, fullscreen, a file dialog")
def s_browsers():
    lua_session()
    ensure_avatar(AV)
    ensure_3d(False)
    clean_windows()
    machine.execute("pkill -u alice foot; pkill -f chromium; pkill -f firefox; true")
    wev_start()  # (on the wall: where a drag goes out of the browser to)
    try:
        page_checks("21", f"chromium --no-first-run --no-default-browser-check --password-store=basic --user-data-dir=/tmp/h3d-chromium file://{PAGE} "
                    "> /tmp/chromium.log 2>&1", "Chromium", log="/tmp/chromium.log", cross=True)
    except Exception as e:  # noqa: BLE001 (Firefox still gets its turn)
        check("21", f"(Chromium: stopped)", False, f"{type(e).__name__}: {e}"[:500])
    machine.execute("pkill -f chromium; true")
    gone("chrom")
    # Firefox: the portal's file chooser, and no first-run pages
    alice("mkdir -p /tmp/h3d-firefox && printf '%s\\n' 'user_pref(\"widget.use-xdg-desktop-portal.file-picker\", 1);' "
          "'user_pref(\"browser.shell.checkDefaultBrowser\", false);' 'user_pref(\"browser.aboutwelcome.enabled\", false);' "
          "'user_pref(\"datareporting.policy.dataSubmissionPolicyBypassNotification\", true);' "
          "'user_pref(\"browser.startup.homepage_override.mstone\", \"ignore\");' 'user_pref(\"toolkit.telemetry.reportingpolicy.firstRun\", false);' "
          "> /tmp/h3d-firefox/user.js")
    page_checks("21", f"firefox --new-instance --profile /tmp/h3d-firefox file://{PAGE} > /tmp/firefox.log 2>&1", "Firefox", log="/tmp/firefox.log")
    machine.execute("pkill -f firefox; true")
    gone("firefox")
    ensure_3d(False)
    alice("setsid -f foot --app-id h3d-left > /dev/null 2>&1; sleep 0.7; setsid -f foot --app-id h3d-right > /dev/null 2>&1")
    wait_for("two terminals", lambda: len(json.loads(ctl("-j", "clients"))) >= 2, 20)
    time.sleep(1)


@section("21e", "Electron (Discord's stack), native Wayland and through XWayland: the page, a notification over the 3D view, a dialog")
def s_electron():
    lua_session()
    ensure_avatar(AV)
    ensure_3d(False)
    clean_windows()
    machine.execute("pkill -u alice foot; pkill -f electron; pkill -f chromium; pkill -f 'bin/[.]?mako'; true")
    gone("chrom")
    alice("setsid -f mako > /tmp/mako.log 2>&1")
    for ozone in ("wayland", "x11"):
        item = "21e"
        machine.execute("rm -f /tmp/electron.log")
        try:
            page_checks(item, f"H3D_PAGE={PAGE} DISPLAY={x_display()} electron {H}/electron --ozone-platform={ozone} > /tmp/electron.log 2>&1",
                        f"Electron ({ozone})", pinch=False, dialog=False, log="/tmp/electron.log", dnd2d=ozone == "x11")
        except Exception as e:  # noqa: BLE001
            check(item, f"(Electron ({ozone}): stopped)", False, f"{type(e).__name__}: {e}"[:500])
        out = machine.execute("cat /tmp/electron.log")[1]
        check(item, f"Electron ({ozone}): its notification was shown (mako)", "notification shown" in out, out.strip()[-300:])
        machine.execute("pkill -f electron; true")
        gone("electron")
    machine.execute("pkill -f 'bin/[.]?mako'; true")  # (.mako-wrapped: -x mako misses it)
    ensure_3d(False)
    alice("setsid -f foot --app-id h3d-left > /dev/null 2>&1; sleep 0.7; setsid -f foot --app-id h3d-right > /dev/null 2>&1")
    wait_for("two terminals", lambda: len(json.loads(ctl("-j", "clients"))) >= 2, 20)
    time.sleep(1)


@section("21i", "an input method (fcitx5): its candidate popup over the window typed into, in 3D")
def s_ime():
    lua_session()
    ensure_avatar(AV)
    ensure_3d(False)
    clean_windows()
    machine.execute("pkill -u alice foot; pkill -x fcitx5; true")
    alice("setsid -f foot --app-id h3d-ime > /dev/null 2>&1")
    wait_for("a terminal", lambda: any(c["class"] == "h3d-ime" for c in json.loads(ctl("-j", "clients"))), 20)
    # fcitx5 as the input method (zwp_input_method_v2, which Hyprland has), its clipboard list for candidates: Ctrl+;
    # shows what was copied since it started, in its popup (zwp_input_popup_surface_v2) by the text cursor
    alice("setsid -f fcitx5 -d -r > /tmp/fcitx5.log 2>&1")
    try:
        wait_for("fcitx5", lambda: as_alice("fcitx5-remote")[1].strip() in ("1", "2"), 30, 0.5)
    except TimeoutError:
        check("21i", "fcitx5 runs", False, machine.execute("tail -n 20 /tmp/fcitx5.log")[1][-600:])
        return
    time.sleep(1)
    alice("wl-copy 'h3d clipboard entry'")
    time.sleep(1)
    ensure_3d()
    menu_closed()
    ctl("hypr3d", "view", "first")
    ctl("hypr3d", "spawn")
    time.sleep(0.8)
    aim_find("h3d-ime")
    ctl("hypr3d", "type", "on")
    time.sleep(0.8)
    press("ctrl", "semicolon")
    try:
        pops = wait_for("the input method's popup", lambda: popups_of("h3d-ime"), 10, 0.3)
    except TimeoutError:
        pops = []
    frame("ime-popup")
    win = next((q for q in panels() if q["kind"] == "window" and q["class"] == "h3d-ime"), None)
    inside = pops and win and all(win["box"][0] <= q["box"][0] < win["box"][0] + win["box"][2] and win["box"][1] <= q["box"][1] < win["box"][1] + win["box"][3] for q in pops)
    check("21i", "Ctrl+; typing into a terminal: fcitx5's list, its input popup, is drawn over the window in 3D, by the text", inside,
          f"{[q['box'] for q in pops]} over {win and win['box']}; fcitx5-remote {as_alice('fcitx5-remote')[1].strip()}")
    press("esc")
    try:
        wait_for("the popup to go", lambda: not popups_of("h3d-ime"), 5, 0.3)
        gone = True
    except TimeoutError:
        gone = False
    check("21i", "... Esc: it goes", gone)
    ctl("hypr3d", "type", "off")
    machine.execute("pkill -x fcitx5; true")
    time.sleep(1)
    ensure_3d(False)
    machine.execute("pkill -u alice foot; true")
    alice("setsid -f foot --app-id h3d-left > /dev/null 2>&1; sleep 0.7; setsid -f foot --app-id h3d-right > /dev/null 2>&1")
    wait_for("two terminals", lambda: len(json.loads(ctl("-j", "clients"))) >= 2, 20)
    time.sleep(1)


# ------------------------------------------------------------------ OBS: its menus and dialogs, capturing the 3D view

OBS_CFG = f"{HOME}/.config/obs-studio"


def obs(request, data=None, save=None):
    """an obs-websocket request (obsws.py, in the VM): the response's data, or an error"""
    cmd = f"python3 {H}/obsws.py {request} {shlex.quote(json.dumps(data or {}))}" + (f" --save {save}" if save else "")
    status, out = as_alice(cmd, 60)
    if status != 0:
        raise RuntimeError(f"obsws {request}: {out.strip()[-300:]}")
    d = json.loads(out.strip().splitlines()[-1])
    if not d.get("requestStatus", {}).get("result"):
        raise RuntimeError(f"obsws {request}: {d.get('requestStatus')}")
    return d.get("responseData", {})


def bmp(path):
    """a BMP (32 or 24 bits a pixel, OBS's screenshot): an Img, as frame() gives"""
    raw = machine.succeed(f"base64 -w0 {path}")
    data = __import__("base64").b64decode(raw)
    off, w, h, bpp = struct_unpack("<I", data, 10), struct_unpack("<i", data, 18), struct_unpack("<i", data, 22), struct_unpack("<H", data, 28)
    step = bpp // 8
    stride = (w * step + 3) & ~3
    rows = []
    for y in range(abs(h)):
        sy = abs(h) - 1 - y if h > 0 else y
        row = data[off + sy * stride: off + sy * stride + w * step]
        rows.append(b"".join(bytes((row[i + 2], row[i + 1], row[i])) for i in range(0, w * step, step)))
    return Img(f"P6 {w} {abs(h)} 255\n".encode() + b"".join(rows))


def struct_unpack(fmt, data, at):
    import struct
    return struct.unpack_from(fmt, data, at)[0]


def mean_rgb(img, step=8):
    n, tot = 0, [0, 0, 0]
    for y in range(0, img.h, step):
        for x in range(0, img.w, step):
            i = (y * img.w + x) * 3
            for k in range(3):
                tot[k] += img.px[i + k]
            n += 1
    return [t / max(n, 1) for t in tot]


@section("22", "OBS in 3D: its window, menu and a dialog; capturing the screen through the portal (its picker used in 3D) shows the 3D view; a placed window's capture")
def s_obs():
    lua_session()
    ensure_avatar(AV)
    ensure_3d(False)
    clean_windows()
    machine.execute("pkill -u alice foot; pkill -x obs; true")
    # no first-run wizard, obs-websocket on without a password
    machine.succeed(f"mkdir -p {OBS_CFG}/plugin_config/obs-websocket && printf '[General]\\nFirstRun=true\\n' > {OBS_CFG}/user.ini && "
                    f"cp {OBS_CFG}/user.ini {OBS_CFG}/global.ini && echo '{{\"server_enabled\": true, \"server_port\": 4455, \"auth_required\": false, "
                    f"\"alerts_enabled\": false, \"first_load\": false}}' > {OBS_CFG}/plugin_config/obs-websocket/config.json && chown -R alice:users {HOME}/.config")
    alice("setsid -f foot --app-id h3d-left > /dev/null 2>&1")
    wait_for("a terminal", lambda: json.loads(ctl("-j", "clients")), 20)
    ensure_3d()
    menu_closed()
    ctl("hypr3d", "view", "first")
    ctl("hypr3d", "spawn")
    ctl("eval", 'hl.config({ input = { accel_profile = "flat" } })')
    time.sleep(0.8)
    r = ctl("hypr3d", "launch", "obs --verbose --disable-shutdown-check --disable-missing-files-check --multi > /tmp/obs.log 2>&1")
    v = wait_for("obs-websocket", lambda: obs("GetVersion"), 120, 2)
    c = wait_for("OBS's window", lambda: next((c for c in json.loads(ctl("-j", "clients")) if "obs" in c["class"].lower()), None), 60, 1)
    time.sleep(3)
    p = placed(c["class"])
    frame("obs")
    check("22", "OBS launched from 3D opens in front of you (obs-websocket answers)", p and v.get("obsVersion"), f"{r}; {c['class']} {c['size']}; OBS {v.get('obsVersion')}")
    # its menu bar's File menu: a popup
    aim_find(c["class"])
    play_on()
    pointer_to(20, 12)
    click("left")
    time.sleep(1.2)
    pops = popups_of(c["class"])
    frame("obs-menu")
    check("22", "its File menu opens, a popup of its window in 3D", pops, [p["box"] for p in pops])
    press("esc")
    ctl("hypr3d", "play", "off")
    # a dialog (a source's properties): by OBS, out in the world
    scene = obs("GetCurrentProgramScene")["currentProgramSceneName"]
    obs("CreateInput", {"sceneName": scene, "inputName": "h3d colour", "inputKind": "color_source_v3", "inputSettings": {"color": 4278190335}})
    before = {x["address"] for x in json.loads(ctl("-j", "clients"))}
    obs("OpenInputPropertiesDialog", {"inputName": "h3d colour"})
    d = wait_for("its properties dialog", lambda: next((x for x in json.loads(ctl("-j", "clients")) if x["address"] not in before), None), 30, 0.5)
    time.sleep(1.5)
    pd, po = placed(d["class"]), placed(c["class"])
    frame("obs-dialog")
    check("22", "a dialog of OBS's (a source's properties) opens by it, in the world", pd and po and math.dist(pd["center"], po["center"]) < 1.5,
          f"{d['title']!r}; {pd and pd['center']} by {po and po['center']}")
    ctl("hypr3d", "window", d["address"], "close")
    time.sleep(1)
    obs("RemoveInput", {"inputName": "h3d colour"})
    # the screen, through the portal (xdg-desktop-portal-hyprland). A PipeWire capture made through obs-websocket
    # never gets past the portal: the portal answers its CreateSession (dbus-monitor shows the Response) and OBS
    # never asks for the sources (obs-websocket makes inputs off OBS's UI thread, whose GLib loop would take the
    # answer). One in the scene collection is made on that thread when OBS starts, as when you start OBS with a screen
    # capture in your scene: so it's added here, OBS closed (it saves the collection) and launched again from 3D,
    # and the portal's picker opens in front of you and is used here
    obs("CreateInput", {"sceneName": scene, "inputName": "h3d screen", "inputKind": "pipewire-screen-capture-source", "inputSettings": {}})
    ctl("hypr3d", "window", c["address"], "close")
    try:
        wait_for("OBS to quit", lambda: machine.execute("pgrep -f '[.]obs-wrapped'")[0] != 0, 30, 0.5)
    except TimeoutError:
        machine.execute("pkill -f '[.]obs-wrapped'; true")
        time.sleep(2)
    before = {x["address"] for x in json.loads(ctl("-j", "clients"))}
    # (the portal's D-Bus traffic, for when it doesn't)
    as_alice("DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus setsid -f dbus-monitor --session > /tmp/dbus.log 2>&1", 10)
    r = ctl("hypr3d", "launch", "obs --verbose --disable-shutdown-check --disable-missing-files-check --multi > /tmp/obs2.log 2>&1")
    picker = lambda x: x["address"] not in before and ("share" in x["title"].lower() or "share-picker" in x["class"])
    try:
        pk = wait_for("the portal's picker", lambda: next((x for x in json.loads(ctl("-j", "clients")) if picker(x)), None), 90, 0.5)
    except TimeoutError:
        machine.execute("pkill -f dbus-monitor; true")
        copy_out("/tmp/dbus.log", "logs")
        calls = machine.execute("grep -A3 'ScreenCast\\|member=Response' /tmp/dbus.log | grep -v '^--' | cut -c1-160 | tail -n 40")[1]
        xdph = as_alice("journalctl --user -u xdg-desktop-portal-hyprland --since '-120 s' --no-pager -o cat | grep screencopy | tail -n 8", 30)[1]
        raise RuntimeError(f"no picker; xdph: {xdph.strip()[-500:]}; D-Bus (logs/dbus.log): {calls.strip()[-1500:]}")
    machine.execute("pkill -f dbus-monitor; true")
    c = wait_for("OBS's window", lambda: next((c for c in json.loads(ctl("-j", "clients")) if "obs" in c["class"].lower()), None), 60, 1)
    time.sleep(2)
    pp = next((q for q in windows3d()["placed"] if q["address"] == pk["address"]), None)
    frame("portal-picker")
    check("22", "the portal's screen picker opens in front of you", pp, f"{pk['class']} {pk['title']!r} {pk['size']}; {pp}")
    note("22", "the picker's panels", [(q["kind"], q["class"], q["box"]) for q in panels() if q["class"] == pk["class"]])
    # (played from the Windows page's Play, as the crosshair would: its class is empty)
    r = ctl("hypr3d", "window", pk["address"], "play")
    wait_for("play mode, the camera facing the picker", lambda: (lambda s: s if s["playing"] and s["playing"]["view"] >= 1 else None)(st()), 10, 0.2)
    # its first screen: the first button of its Screen tab
    pointer_to(pk["size"][0] / 2, 70)
    click("left")
    time.sleep(1)
    gone = wait_for("the picker to go", lambda: not any(x["address"] == pk["address"] for x in json.loads(ctl("-j", "clients"))), 15, 0.5)
    check("22", "... a click on its screen in 3D picks it", gone, machine.execute("grep -a '\\[pipewire\\]' /tmp/obs2.log | tail -n 4")[1][-300:])
    ctl("hypr3d", "play", "off")
    ctl("hypr3d", "spawn")
    time.sleep(3)
    obs("GetSourceScreenshot", {"sourceName": "h3d screen", "imageFormat": "bmp", "imageWidth": 320}, save="/tmp/obs-shot.bmp")
    shot = bmp("/tmp/obs-shot.bmp")
    view = frame("obs-capturing")
    a, b = mean_rgb(shot), mean_rgb(view)
    (FRAMES / "obs-capture.ppm").write_bytes(f"P6 {shot.w} {shot.h} 255\n".encode() + shot.px)
    check("22", "OBS's screen capture shows the 3D view (its colours are the frame's)", shot.w == 320 and max(abs(a[k] - b[k]) for k in range(3)) < 25, f"OBS {a}, the frame {b}")
    # a window placed in the world, captured as a window (ext-image-copy-capture of a toplevel, as the portal does)
    t = next(x for x in json.loads(ctl("-j", "clients")) if x["class"] == "h3d-left")
    ctl("hypr3d", "window", "h3d-left", "bring")
    time.sleep(1)
    status, _ = as_alice(f"grim -T {t['stableId']} -t ppm /tmp/window.ppm", 30)
    ok = status == 0
    img = None
    if ok:
        copy_out("/tmp/window.ppm", "raw")
        img = Img((RAW / "window.ppm").read_bytes())
        (FRAMES / "placed-window-capture.ppm").write_bytes((RAW / "window.ppm").read_bytes())
    check("22", "a placed window captured as a window (grim -T, ext-image-copy-capture): the window, at its size", ok and img and abs(img.w - t["size"][0]) <= 2,
          f"grim {status}; {img and (img.w, img.h)} vs {t['size']}")
    obs("RemoveInput", {"inputName": "h3d screen"})
    ctl("hypr3d", "window", c["class"], "close")
    time.sleep(3)
    machine.execute("pkill -x obs; true")
    ctl("eval", 'hl.config({ input = { accel_profile = "" } })')
    ctl("hypr3d", "reset-windows", "forget")
    ensure_3d(False)
    machine.execute("pkill -u alice foot; true")
    alice("setsid -f foot --app-id h3d-left > /dev/null 2>&1; sleep 0.7; setsid -f foot --app-id h3d-right > /dev/null 2>&1")
    wait_for("two terminals", lambda: len(json.loads(ctl("-j", "clients"))) >= 2, 20)
    time.sleep(1)


# ------------------------------------------------------------------ real games

@section("23", "real open-source games in 3D: Chocolate Doom (mouse look, the pointer locked; Wayland and X11) and SuperTux (keys, a controller)")
def s_games():
    lua_session()
    ensure_avatar(AV)
    ensure_3d(False)
    clean_windows()
    machine.execute("pkill -u alice foot; pkill -f chocolate-doom; pkill -f supertux; true")
    ensure_3d()
    menu_closed()
    ctl("hypr3d", "view", "first")
    ctl("hypr3d", "spawn")
    time.sleep(0.8)
    # Chocolate Doom on the 2D desktop first, Hyprland's own: how much the mouse turns it there
    ensure_3d(False)
    d2 = {}
    for driver in ("wayland", "x11"):
        alice(f"SDL_VIDEODRIVER={driver} DISPLAY={x_display()} SDL_APP_ID=chocolate-doom setsid -f chocolate-doom -iwad /etc/h3d/freedoom2.wad -window "
              f"-geometry 960x600 -nosound -nomusic -warp 1 -skill 3 > /tmp/doom-2d-{driver}.log 2>&1")
        c = wait_for("Chocolate Doom's window", lambda: next((c for c in json.loads(ctl("-j", "clients")) if "doom" in c["class"].lower()), None), 60, 1)
        time.sleep(4)
        box = (c["at"][0] + 20, c["at"][1] + 20, c["at"][0] + c["size"][0] - 20, c["at"][1] + c["size"][1] - 20)
        a = frame(f"doom-2d-{driver}")
        rel(1500, 0)
        time.sleep(0.8)
        b = frame(f"doom-2d-{driver}-turned")
        d2[driver] = a.differs(b, box)
        note("23", f"Chocolate Doom ({driver}) on the 2D desktop: the frame after 1500 counts right", f"{d2[driver]:.0%} changed")
        machine.execute("pkill -f chocolate-doom; true")
        gone("doom")
    ensure_3d()
    ctl("hypr3d", "spawn")
    time.sleep(0.8)
    for driver in ("wayland", "x11"):
        item = "23"
        env = f"SDL_VIDEODRIVER={driver} DISPLAY={x_display()} SDL_APP_ID=chocolate-doom"
        r = ctl("hypr3d", "launch", f"{env} chocolate-doom -iwad /etc/h3d/freedoom2.wad -window -geometry 960x600 -nosound -nomusic -warp 1 -skill 3 "
                f"> /tmp/doom-{driver}.log 2>&1")
        c = wait_for("Chocolate Doom's window", lambda: next((c for c in json.loads(ctl("-j", "clients")) if "doom" in c["class"].lower()), None), 60, 1)
        time.sleep(4)
        aim_find(c["class"])
        r2, s = play_on()
        time.sleep(1.5)
        p = st()["playing"]
        a = frame(f"doom-{driver}")
        rel(1500, 0)  # (a quarter turn or so)
        time.sleep(0.8)
        b = frame(f"doom-{driver}-turned")
        d_turn = a.differs(b, (160, 100, 1120, 700))
        press("up", hold=1.2)
        time.sleep(0.5)
        e = frame(f"doom-{driver}-walked")
        d_walk = b.differs(e, (160, 100, 1120, 700))
        check(item, f"Chocolate Doom ({driver}): played, its pointer locked, it turns with the mouse and walks with the keys",
              p and p["locked"] and d_turn > 0.05 and d_walk > 0.1,
              f"{c['class']} xwayland {c['xwayland']}; locked {p and p['locked']}; turned {d_turn:.0%} (on the 2D desktop {d2[driver]:.0%}), walked {d_walk:.0%}")
        note(item, f"Chocolate Doom ({driver}): 3D frames a second while played", st()["fps"])
        press("meta_l", "esc")
        ctl("hypr3d", "window", c["class"], "close")
        time.sleep(1.5)
        machine.execute("pkill -f chocolate-doom; true")
        time.sleep(1)
    # SuperTux: its menu by the keyboard, and by a controller
    r = ctl("hypr3d", "launch", "supertux2 > /tmp/supertux.log 2>&1")
    c = wait_for("SuperTux's window", lambda: next((c for c in json.loads(ctl("-j", "clients")) if "supertux" in c["class"].lower()), None), 90, 1)
    time.sleep(8)  # (its title screen)
    aim_find(c["class"])
    play_on()
    time.sleep(1)
    # (its first start asks whether it may go online: Yes or No, left and right)
    a = frame("supertux")
    press("right")
    time.sleep(0.8)
    b = frame("supertux-keys")
    machine.succeed(f"python3 {H}/gamepad.py dx=-1", timeout=60)
    time.sleep(0.8)
    e = frame("supertux-pad")
    d_keys, d_pad = a.differs(b), b.differs(e)
    check("23", "SuperTux: played, its menu moves with the keys and with a controller", d_keys > 0.002 and d_pad > 0.002, f"keys {d_keys:.2%}, controller {d_pad:.2%} of the frame changed")
    press("meta_l", "esc")
    ctl("hypr3d", "window", c["class"], "close")
    time.sleep(1.5)
    machine.execute("pkill -f supertux; true")
    ctl("hypr3d", "reset-windows", "forget")
    ensure_3d(False)
    alice("setsid -f foot --app-id h3d-left > /dev/null 2>&1; sleep 0.7; setsid -f foot --app-id h3d-right > /dev/null 2>&1")
    wait_for("two terminals", lambda: len(json.loads(ctl("-j", "clients"))) >= 2, 20)
    time.sleep(1)


# ------------------------------------------------------------------ performance

def frame_rates(label, secs=4):
    """the 3D view's frames a second, the plugin's own time a frame, and the game's frames a second, after `secs`"""
    game_mark(label)
    time.sleep(secs)
    s = st()
    g = game_frames(game_lines(label))
    return {"fps": round(s["fps"], 1), "updateMs": s["updateMs"], "renderMs": s["renderMs"], "game": g[-2:] if g else []}


@section("24", "performance: the 3D view's frame rate with apps and a game in the world, the plugin's own time a frame, and how a game paces itself behind it")
def s_perf():
    lua_session()
    ensure_avatar(AV)
    ensure_3d(False)
    clean_windows()
    # two terminals, the game, and wev: four windows
    alice("setsid -f stdbuf -oL wev > /dev/null 2>&1")
    game_start(alone=False)
    time.sleep(2)
    d2 = frame_rates("2d")
    note("24", "on the 2D desktop, the game draws", f"{d2['game']} frames a second (vsync: FIFO or frame callbacks)")
    ensure_3d()
    menu_closed()
    ctl("hypr3d", "view", "first")
    ctl("hypr3d", "spawn")
    time.sleep(1)
    walk = frame_rates("walking")
    note("24", "3D, first person, 4 windows on the wall", walk)
    ctl("hypr3d", "view", "third")
    time.sleep(1)
    third = frame_rates("third person")
    note("24", "... in third person (the avatar drawn)", third)
    ctl("hypr3d", "view", "first")
    aim_find("h3dgame")
    ctl("hypr3d", "grab")
    ctl("hypr3d", "place")
    play_on()
    play = frame_rates("playing")
    note("24", "... the game played (placed in the world, the camera facing it)", play)
    ctl("hypr3d", "play", "off")
    ctl("hypr3d", "window", "wev", "pin")
    time.sleep(1)
    pin = frame_rates("pinned")
    note("24", "... and wev pinned to the view", pin)
    check("24", "the game keeps drawing behind the 3D view, at its frame rate (frame callbacks, presentation, FIFO at the 3D view's pace)",
          all(r["game"] and min(r["game"]) > 0 and abs(r["game"][-1] - r["fps"]) < max(4, 0.35 * r["fps"]) for r in (walk, play)),
          {"walking": walk, "playing": play})
    # direct scanout: a fullscreen game's frames put on the screen as they are (render:direct_scanout = 1), on the 2D
    # desktop, against the same game fullscreen and played in 3D, drawn into the view (hypr3d blocks direct scanout)
    ctl("hypr3d", "window", "wev", "unpin")
    ensure_3d(False)
    ctl("eval", 'hl.config({ render = { direct_scanout = 1 } })')
    ctl("dispatch", 'hl.dsp.focus({ window = "class:h3dgame" })')
    time.sleep(0.5)
    press("f")
    time.sleep(2.5)
    m = json.loads(ctl("-j", "monitors"))[0]
    fs2d = frame_rates("fullscreen 2d")
    note("24", "the game fullscreen on the 2D desktop, direct scanout allowed", f"game {fs2d['game']} frames a second; directScanoutTo {m.get('directScanoutTo')}, "
         f"blocked by {m.get('directScanoutBlockedBy')}")
    ensure_3d()
    ctl("hypr3d", "spawn")
    time.sleep(1)
    aim_find("h3dgame")
    play_on()
    fs3d = frame_rates("fullscreen played")
    m = json.loads(ctl("-j", "monitors"))[0]
    note("24", "... and fullscreen, played in 3D", f"{fs3d}; directScanoutTo {m.get('directScanoutTo')}, blocked by {m.get('directScanoutBlockedBy')}")
    press("f")
    time.sleep(0.8)
    ctl("hypr3d", "play", "off")
    ctl("eval", 'hl.config({ render = { direct_scanout = 0 } })')
    ctl("hypr3d", "reset-windows", "forget")
    clean_windows()
    ensure_3d(False)


@section("live", "tools/test/live/check.sh, the script for checking your own desktop, run in the VM (and stopped with Ctrl+C)")
def s_live():
    # a session without the plugin (the script loads it, and unloads it), one terminal: under the crosshair in 3D
    start_hyprland("hyprland.lua", lua_config(), terminals=False)
    alice("setsid -f foot --app-id h3d-left > /dev/null 2>&1")
    wait_for("the terminal", lambda: json.loads(ctl("-j", "clients")), 20)
    time.sleep(1.5)
    # --mic's prompts, sung into the test microphone as they come (CHECK_SAY), each cutting the one before short
    machine.succeed(f"""cat > {H}/sing.sh << 'EOF'
#!/bin/sh
pkill -x pw-cat
case "$1" in s) f=hiss ;; quiet) f=silence ;; *) f=man_$1 ;; esac
exec pw-cat -p --target test_mic_in {H}/wav/${{f}}_long.wav
EOF
chmod 755 {H}/sing.sh && chown alice {H}/sing.sh""")
    # --app's prompts, done as they come (CHECK_DO): what you'd do with the keys, through hyprctl
    machine.succeed(f"""cat > {H}/do.sh << 'EOF'
#!/bin/sh
sleep 1
case "$1" in
    play) hyprctl hypr3d play on ;;
    stop) hyprctl hypr3d play off ;;
    type) hyprctl hypr3d type on; sleep 2; hyprctl hypr3d type off ;;
    pin) hyprctl hypr3d window "$2" pin ;;
    free) hyprctl hypr3d window "$2" play; sleep 8; hyprctl hypr3d play off ;;
esac
EOF
chmod 755 {H}/do.sh && chown alice {H}/do.sh""")
    sig = json.loads(alice("hyprctl -j instances"))[0]["instance"]
    lit = f"--map {LIT}" if machine.execute(f"test -f {LIT}")[0] == 0 else "--no-map"
    cmd = (f"cd {H} && HYPRLAND_INSTANCE_SIGNATURE={sig} CHECK_SAY={H}/sing.sh CHECK_DO={H}/do.sh bash {H}/repo/tools/test/live/check.sh /tmp/live --mic "
           f"--avatar {AV} {lit} --app 'h3dgame --title liveapp' --so {SO}")
    t0 = time.time()
    status, out = as_alice(cmd, timeout=900)
    took = time.time() - t0
    (LOGS / "live-check.txt").write_text(out)
    for f in ("results.txt", "lipsync.jsonl", "hypr3d.log"):
        if machine.execute(f"test -f /tmp/live/{f}")[0] == 0:
            copy_out(f"/tmp/live/{f}", "live")
    machine.execute("cd /tmp/live && tar cf /tmp/live-frames.tar frames")
    copy_out("/tmp/live-frames.tar", "live")
    res = machine.execute("cat /tmp/live/results.txt")[1]
    note("live", "its results", "; ".join(l.strip() for l in res.splitlines() if l.startswith(("ok", "FAIL")))[:1500])
    fails = [l for l in res.splitlines() if l.startswith("FAIL")]
    # (llvmpipe draws 6-13 frames a second: its "keeps up with the monitor" fails here, as it should)
    unexpected = [l for l in fails if "keeps up with the monitor" not in l]
    last = res.strip().splitlines()[-1] if res.strip() else ""
    check("live", "check.sh --mic --avatar --map runs to the end", "passed," in last, f"{last}; exit {status}; {took:.0f} s")
    check("live", "... and all its checks pass but the frame rate (software rendering)", not unexpected, unexpected[:6])
    check("live", "... its lip sync heard the five vowels sung into the microphone", len([l for l in res.splitlines() if l.startswith("ok") and "you said" in l]) == 5,
          [l for l in res.splitlines() if "you said" in l])
    apps = [l for l in res.splitlines() if "--app" in l or "playing" in l or "pinned" in l or "typing into it" in l or "own cursor" in l or "your own try" in l]
    check("live", "... --app: the app launched from 3D, played, stopped, typed into, its cursor, pinned (its prompts done as they came)",
          len([l for l in apps if l.startswith("ok")]) >= 6 and not [l for l in apps if l.startswith("FAIL")], apps)
    check("live", "... and the free step: played, then walking again (the prompt done as it came)", "h3dgame, your own try" in res and "... done" in res,
          [l.strip() for l in res.splitlines() if "your own try" in l or "... done" in l])
    check("live", "... and its window closed after, as its close button does", "h3dgame: its window closed" in res,
          [l.strip() for l in res.splitlines() if l.strip().startswith("h3dgame:")])
    check("live", "... and it left 3D, unloaded the plugin and closed the microphone", "hypr3d" not in ctl("plugin", "list") and lipsync_node() is None and not ctl("hypr3d", "status").startswith("{"))
    # Ctrl+C in 3D: its trap leaves 3D, turns lip sync off and unloads the plugin
    before = calm_frame("before-live-check-interrupted")
    machine.execute("rm -rf /tmp/live2")
    alice(f"cd {H} && HYPRLAND_INSTANCE_SIGNATURE={sig} setsid -f bash {H}/repo/tools/test/live/check.sh /tmp/live2 --mic --avatar {AV} --no-map --so {SO} "
          "> /tmp/live2.out 2>&1")
    in3d = wait_for("the script in 3D with its avatar", lambda: (lambda s: s if s.startswith("{") and '"mode": "active"' in s and "BoothAccessories" in s else None)(ctl("hypr3d", "status")), 120, 0.5)
    machine.succeed("pkill -INT -f 'live/check[.]sh /tmp/live2'")  # ([.]: not the shell that runs this)
    # (done when it has said how it went, its last line)
    try:
        gone = wait_for("the script to say how it went", lambda: "failed, in" in machine.execute("cat /tmp/live2.out")[1], 90, 0.5)
    except TimeoutError:
        gone = False
    out2 = machine.execute("cat /tmp/live2.out")[1]
    (LOGS / "live-check-interrupted.txt").write_text(out2 + "\n--- its results.txt:\n" + machine.execute("cat /tmp/live2/results.txt")[1] +
                                                     "\n--- still running:\n" + machine.execute("ps -ef | grep '[c]heck[.]sh'")[1])
    check("live", "Ctrl+C (SIGINT) while it's in 3D: it stops, leaves 3D and unloads the plugin", in3d and gone and "hypr3d" not in ctl("plugin", "list") and "interrupted" in out2,
          out2.strip().splitlines()[-3:])
    d = before.differs(calm_frame("after-live-check-interrupted"))
    check("live", "... and the desktop is as it was", d < 0.005 and lipsync_node() is None, f"{d:.2%} of pixels differ")


@section("exit", "Hyprland exits cleanly with windows open, dwindle and master (0.55.2 crashes, plugin or not: its own bug)")
def s_exit_windows():
    # Hyprland 0.55.2's CCompositor::cleanup drops its windows before their clients, and dwindle then calls an expired
    # target of a window that's gone (fixed in 0.56.0, upstream commit 338bdbb3). stop_hyprland() closes the
    # terminals first because of it; here they stay open, in 3D with the plugin loaded
    start_hyprland("hyprland.lua", lua_config({"avatar": AV}, load=True))
    ensure_avatar(AV)
    ensure_3d()
    n = segfaults()
    r = ctl("dispatch", "hl.dsp.exit()")
    gone = wait_for("Hyprland to go", lambda: not hypr_pid(), 15, 0.5)
    crashed = segfaults() - n
    KNOWN_CRASHES[0] += crashed
    HYPR["pid"] = ""
    check("exit", "hl.dsp.exit() in 3D with two terminals open: Hyprland exits without crashing", gone and not crashed, f"{r}; {crashed} segfaults",
          known="Hyprland 0.55.2 crashes in CDwindleAlgorithm on exit with windows open, fixed in 0.56.0 (upstream 338bdbb3)")
    # the master layout crashes the same way (CMasterAlgorithm::calculateWorkspace -> ITarget::setPositionGlobal on an
    # expired target), and 338bdbb3 doesn't cover it: upstream main (e368c13c) still has no guard in master
    start_hyprland("hyprland.lua", lua_config({"avatar": AV}, load=True, layout="master"))
    ensure_avatar(AV)
    ensure_3d()
    n = segfaults()
    r = ctl("dispatch", "hl.dsp.exit()")
    gone = wait_for("Hyprland to go", lambda: not hypr_pid(), 15, 0.5)
    crashed = segfaults() - n
    KNOWN_CRASHES[0] += crashed
    HYPR["pid"] = ""
    check("exit", "... the same with the master layout", gone and not crashed, f"{r}; {crashed} segfaults",
          known="Hyprland 0.55.2 crashes in CMasterAlgorithm on exit with windows open, not fixed upstream (main e368c13c)")
    note("exit", "the Hyprland", machine.succeed("readlink -f $(command -v Hyprland)").strip())



def hidpi_checks(scale):
    """the plugin at this scale, on the hidpi VM's 1920x1200 screen"""
    item = f"x{scale:g}"
    start_hyprland("hyprland.lua", lua_config(scale=scale))
    mon = json.loads(ctl("-j", "monitors"))[0]
    lw, lh = round(mon["width"] / scale), round(mon["height"] / scale)
    check(item, f"Hyprland at scale {scale:g}: 1920x1200, {lw}x{lh} logical", mon["width"] == 1920 and mon["height"] == 1200 and abs(mon["scale"] - scale) < 1e-3,
          f"{mon['width']}x{mon['height']} at {mon['scale']}")
    ensure_plugin()
    ensure_avatar(AV)
    # Hyprland's cursor over the left terminal (the tablet: 0..32767 across the screen)
    tablet(200 * 32767 // lw, (lh - 100) * 32767 // lh)
    cx, cy = round(200 * scale), round((lh - 100) * scale)
    box = (cx - 20, cy - 20, cx + 20 + round(30 * scale), cy + 20 + round(30 * scale))
    n2d = calm_frame(f"{item}-desktop").count(cursor_cyan, box)
    ensure_3d()
    menu_closed()
    ctl("hypr3d", "view", "first")
    ctl("hypr3d", "spawn")
    time.sleep(1.2)
    img = frame(f"{item}-3d")
    check(item, "grim's frame is 1920x1200, and 3D", img.w == 1920 and img.h == 1200 and img.count(cursor_cyan, box) < 5, f"{img.w}x{img.h}")
    check(item, "Hyprland's cursor is hidden in 3D", n2d > 20 and img.count(cursor_cyan, box) < 5, f"cursor pixels: 2D {n2d}, 3D {img.count(cursor_cyan, box)}")
    # the crosshair, green against the sky, at the monitor's scale: its arms reach 11 logical px out each way
    ctl("hypr3d", "turn", "0", "60")
    time.sleep(0.8)
    sky = frame(f"{item}-crosshair")
    green = lambda r, g, b: g > 200 and r < 140 and b < 150
    row = [x for x in range(sky.w // 2 - 80, sky.w // 2 + 80) if green(*sky.px[(sky.h // 2 * sky.w + x) * 3:(sky.h // 2 * sky.w + x) * 3 + 3])]
    span = row[-1] - row[0] + 1 if row else 0
    check(item, f"the crosshair is drawn at scale {scale:g}: {round(22 * scale)} px across", abs(span - 22 * scale) <= 2, f"{span} px")
    ctl("hypr3d", "turn", "0", "0")
    # aiming at the left terminal, a click, and typing into it
    aimed = None
    for _ in range(40):
        a = st()["aimed"]
        if a and a.get("kind") == "window" and a.get("class") == "h3d-left":
            aimed = a
            break
        rel(-15, 0, after=0.2)
    check(item, "the mouse turns the crosshair onto the left terminal", aimed, aimed)
    click("left")
    check(item, "a left click focuses it", json.loads(ctl("-j", "activewindow")).get("class") == "h3d-left", json.loads(ctl("-j", "activewindow")).get("class"))
    machine.execute("rm -f /tmp/h3d-hidpi")
    press("e")
    type_text("touch /tmp/h3d-hidpi")
    press("ret")
    time.sleep(0.8)
    check(item, "E: what's typed reaches it (it ran touch)", machine.execute("test -e /tmp/h3d-hidpi")[0] == 0)
    frame(f"{item}-typed")
    press("meta_l", "esc")
    # the Action Menu: its radius 28% of the screen's height, the mouse moving its cursor by logical pixels
    ctl("hypr3d", "spawn")
    ctl("hypr3d", "view", "third")
    ctl("hypr3d", "turn", "0", "60")  # (the sky behind it: its dark ring stands out)
    ctl("hypr3d", "avatar", "parts", "reset")
    press("tab")
    for k in ("4", "8", "8", "1"):
        press(k)
    rel(150, 0)
    v = menu()["dial"]["value"]
    r = 0.28 * min(1920, 1200) / scale  # the menu's radius, logical px
    want = math.atan2(150 / r, 0.66) / (2 * math.pi)
    check(item, f"the dial: 150 counts right of the top turn it {want:.3f} (a {r:.0f} px radius)", abs(v - want) < 0.004, f"{v:.3f}")
    img = frame(f"{item}-dial")
    # (a row below its middle, clear of its ticks at 3 and 9 o'clock: a chord of the circle)
    y, dy = img.h // 2 + 60, 60
    dark = [x for x in range(img.w // 2, img.w) if max(img.px[(y * img.w + x) * 3:(y * img.w + x) * 3 + 3]) < 70]
    across, chord = (dark[-1] - dark[0] + 1 if dark else 0), 2 * math.sqrt(336 ** 2 - dy ** 2)
    check(item, f"the dial's ring is drawn 336 px out from its middle ({chord:.0f} px across, 60 px below it)", abs(across - chord) <= 8, f"{across} px of dark ring")
    press("backspace")
    press("esc")
    menu_closed()
    ctl("hypr3d", "turn", "0", "0")
    # the lip sync badge: its size follows the scale
    ctl("hypr3d", "avatar", "lipsync", "on")
    wait_for("listening", lambda: lipsync()["listening"], 10)
    ctl("dismissnotify")
    time.sleep(1.2)
    b = lipsync()["badge"]
    img = frame(f"{item}-badge")
    m = round(12 * scale)
    check(item, f"the badge: {round(32 * scale)} px tall, {m} px in from the top right corner", b and abs(b[3] - 32 * scale) <= 2 and b[1] == m and b[0] + b[2] == img.w - m, b)
    check(item, "... where the frame has it", b and img.count(red, box_of(b)) > 20 * scale * scale, b and img.count(red, box_of(b)))
    ctl("hypr3d", "avatar", "lipsync", "off")
    ensure_3d(False)
    time.sleep(0.5)
    n = calm_frame(f"{item}-back").count(cursor_cyan, box)
    check(item, "out of 3D, the cursor shows again", n > 20, f"{n} cursor pixels")


@section("x1.5", "HiDPI: 1920x1200 at scale 1.5", vm="hidpi")
def s_hidpi_15():
    hidpi_checks(1.5)


@section("19@1.5", "X11 apps at scale 1.5 (as section 19)", vm="hidpi")
def s_x11_15():
    x11_checks("19@1.5", 1.5)


@section("x2", "HiDPI: 1920x1200 at scale 2", vm="hidpi")
def s_hidpi_2():
    hidpi_checks(2)


@section("19@2", "X11 apps at scale 2 (as section 19)", vm="hidpi")
def s_x11_2():
    x11_checks("19@2", 2)


# ------------------------------------------------------------------ run

def collect_logs(sub="logs"):
    try:
        app_logs()
    except Exception:  # noqa: BLE001
        pass
    for i, f in enumerate(machine.execute("ls /run/user/1000/hypr/*/hyprland.log 2>/dev/null || true")[1].split()):
        machine.execute(f"cp {f} /tmp/hyprland-{i}.log; chmod 644 /tmp/hyprland-{i}.log")
        copy_out(f"/tmp/hyprland-{i}.log", sub)
    machine.execute("journalctl -b --no-pager > /tmp/journal.txt 2>&1; true")
    copy_out("/tmp/journal.txt", sub)
    machine.execute(f"runuser -u alice -- env XDG_RUNTIME_DIR=/run/user/1000 pw-dump > /tmp/pw-dump.json 2>&1; true")
    copy_out("/tmp/pw-dump.json", sub)
    for c in crash_reports():
        machine.execute(f"cp {HOME}/.cache/hyprland/{c} /tmp/{c}; true")
        copy_out(f"/tmp/{c}", sub)


KNOWN_CRASHES = [0]  # Hyprland's own crashes that checks saw coming, in this VM


def vm_done(sub):
    """this VM's last checks, and its logs to OUT/sub"""
    try:
        stop_hyprland()
    except Exception as e:  # noqa: BLE001
        check("exit", "stopping Hyprland", False, str(e)[:300])
    n = segfaults() - KNOWN_CRASHES[0]
    check("exit", f"no other Hyprland crashes in the {machine.name} VM", n == 0,
          machine.execute(f"journalctl -k --no-pager | grep 'segfault at' | grep -v '{PORTAL_CRASH}' || true")[1].strip()[:300])
    portal = machine.execute(f"journalctl -k --no-pager | grep 'segfault at' | grep '{PORTAL_CRASH}' || true")[1].strip()
    if portal:
        pid = re.search(r"\[(\d+)\]: segfault", portal.splitlines()[-1])
        stack = machine.execute(f"coredumpctl info --no-pager {pid.group(1)} 2>/dev/null | grep -m1 -A8 'Stack trace of thread' || true")[1] if pid else ""
        note("exit", "a portal crashed (not Hyprland: when it quit)", f"{portal[:300]}; {stack.strip()[:600]}")
    collect_logs(sub)
    machine.shutdown()
    KNOWN_CRASHES[0] = 0


def boot(vm):
    global machine
    machine = vm
    t0 = time.time()
    machine.start()
    machine.wait_for_unit("multi-user.target")
    machine.copy_from_host(str(IN), H)
    machine.succeed(f"chown -R alice:users {H}")
    note("vm", f"{machine.name} booted", f"{time.time() - t0:.1f} s")


(OUT / "results.txt").write_text("")
t_start = time.time()
current = None
ONLY = [i for i in os.environ.get("H3D_ONLY", "").split(",") if i]  # run.sh --only: these sections, after "0"
for item, title, fn, vm in SECTIONS:
    if ONLY and item not in ONLY and not (item == "0" and vm == "machine"):
        continue
    if vm != current:
        if current:
            vm_done("logs" if current == "machine" else f"logs/{current}")
        HYPR["pid"] = ""
        boot(globals()[vm])
        current = vm
    print(f"\n=== {item}: {title}", flush=True)
    t0 = time.time()
    crashes = crash_reports()
    try:
        fn()
    except Exception as e:  # noqa: BLE001
        traceback.print_exc()
        check(item, f"({title}: stopped)", False, f"{type(e).__name__}: {e}"[:700])
        # (what it started, gone: Hyprland 0.55.2 crashes when it quits with windows open)
        try:
            app_logs()
            clean_windows()
        except Exception:  # noqa: BLE001
            traceback.print_exc()
    if HYPR["pid"] and not alive():
        check(item, "Hyprland still running afterwards", False, f"crash reports: {[c for c in crash_reports() if c not in crashes]}")
        collect_logs("logs" if current == "machine" else f"logs/{current}")
        try:
            start_hyprland(HYPR["config"], lua_config() if HYPR["config"].endswith(".lua") else conf_config(load=False))
            ensure_plugin()
        except Exception as e:  # noqa: BLE001
            check(item, "Hyprland started again", False, str(e)[:300])
    print(f"=== {item} took {time.time() - t0:.1f} s", flush=True)

vm_done("logs" if current == "machine" else f"logs/{current}")
passed = sum(1 for r in RESULTS if r["ok"] is True)
failed = [r for r in RESULTS if r["ok"] is False and not r["known"]]
known = [r for r in RESULTS if r["ok"] is False and r["known"]]
(OUT / "results.json").write_text(json.dumps(RESULTS, indent=1, ensure_ascii=False))
summary = f"{passed} passed, {len(failed)} failed" + (f", {len(known)} known" if known else "") + f", in {time.time() - t_start:.0f} s"
with open(OUT / "results.txt", "a") as f:
    f.write(summary + "\n")
print(summary, flush=True)
if failed:
    raise Exception(f"{len(failed)} checks failed")
