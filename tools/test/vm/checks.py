# checks.py: the checklist tools/test/vm/run.sh runs through nixos-test-driver in vm.nix's VMs (`machine` at 1280x800,
# `hidpi` at 1920x1200, scales 1.5 and 2). Input goes through QMP input-send-event, the kernel, libinput and Hyprland. A
# section that throws fails as a whole; "known" failures (bugs outside hypr3d) don't count.
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

IN = Path(os.environ["H3D_IN"])  # run.sh's inputs, copied to H in the VM
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
LIT_RS = f"{H}/LitCourtRuntimeSun.glb"  # its sun has no baked shadow channel
VOWELS = ["a", "i", "u", "e", "o"]
VISEMES = ["aa", "ih", "ou", "ee", "oh"]
for d in (FRAMES, LOGS, RAW):
    d.mkdir(parents=True, exist_ok=True)


def secs(s):
    return dt.timedelta(seconds=s)


# ------------------------------------------------------------------ results

RESULTS = []


def check(item, what, ok, detail="", known=""):
    """known: why it fails, for a bug outside hypr3d (reported, not counted)"""
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
    """keys down in order, up in reverse; names are QEMU qcodes (tab, esc, ret, meta_l, shift_r, f1, grave_accent)"""
    for n in names:
        qmp([key_event(n, True)])
        time.sleep(0.03)
    time.sleep(hold)
    for n in reversed(names):
        qmp([key_event(n, False)])
        time.sleep(0.03)
    time.sleep(after)


def rel(dx, dy, after=0.3):
    """PS/2 mouse counts, 100 per event: QEMU sends motion beyond a packet's range only with the next move"""
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
        """fraction of sampled pixels with a channel differing by more than thresh"""
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
    """grim's frame of the screen (or one output), saved as frames/NN-name.png"""
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
    """Hyprland's PID (under Nix its comm is .Hyprland-wrapp)"""
    return machine.execute("ps -u alice -o pid=,comm= | awk '$2 ~ /Hyprland/ {print $1; exit}'")[1].strip()


def alive():
    return bool(HYPR["pid"]) and hypr_pid() == HYPR["pid"] and as_alice("hyprctl -i 0 version", 15)[0] == 0


def config_ok(errs):
    return errs.strip().lower() in ("", "no errors", "ok")


# xdg-desktop-portal-hyprland 1.4.1 can segfault when Hyprland quits; the kernel logs it as .xdg-desktop-po and vm_done
# reports those apart
PORTAL_CRASH = "xdg-desktop-po"


def segfaults():
    return int(machine.execute(f"journalctl -k --no-pager | grep 'segfault at' | grep -vc '{PORTAL_CRASH}' || true")[1].strip() or 0)


def coredumps():
    """PIDs of systemd-coredump's core dumps so far"""
    out = machine.execute("coredumpctl list --json=short --no-pager 2>/dev/null || true")[1].strip()
    try:
        return {str(d.get("pid")) for d in json.loads(out)} if out.startswith("[") else set()
    except ValueError:
        return set()


def new_coredump(before):
    """the crashed thread's stack from a core dump not in `before`, or None"""
    new = sorted(p for p in coredumps() if p not in before)
    if not new:
        return None
    return machine.execute(f"coredumpctl info --no-pager {new[-1]} 2>/dev/null | grep -m1 -A20 'Stack trace of thread' || true")[1]


def restart_after_crash():
    """restarts Hyprland and the plugin after an expected crash; segfaults meanwhile (hyprland-dialog's, in this VM)
    count as known"""
    n = segfaults()
    start_hyprland(HYPR["config"], lua_config() if HYPR["config"].endswith(".lua") else conf_config(load=False))
    ensure_plugin()
    time.sleep(3)
    KNOWN_CRASHES[0] += segfaults() - n


def stop_hyprland():
    """Hyprland 0.55.2 crashes on exit with windows open (a null pointer in ITarget::setPositionGlobal), plugin or not,
    so windows are closed first"""
    if not HYPR["pid"]:
        return
    plugin = "hypr3d" in ctl("plugin", "list")
    machine.execute("pkill -u alice foot || true")
    try:
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
    # start the graphical session target for the portals, stopping ones left on an earlier Hyprland's display (they come
    # back on demand)
    as_alice("systemctl --user import-environment WAYLAND_DISPLAY; systemctl --user stop xdg-desktop-portal.service xdg-desktop-portal-gtk.service "
             "xdg-desktop-portal-hyprland.service; systemctl --user start hyprland-session.target", 30)
    wait_for("the monitor", lambda: json.loads(ctl("-j", "monitors")), 20)
    if not terminals:
        return
    # by app id: the shell sets the titles
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
    first_person_body(False)


def first_person_body(on):
    """first person with the avatar's body (first_person_body, on by default) or the plain 1.65 m view, which all but
    section 32 expect"""
    r = ctl("hypr3d", "view", "body", "on" if on else "off")
    if r != ("on" if on else "off"):
        raise RuntimeError(f"hyprctl hypr3d view body: {r}")


def lua_session():
    """a Lua-config Hyprland, which sections 18 on expect (8c leaves a hyprland.conf one, "exit" none)"""
    if not HYPR["config"].endswith(".lua") or not HYPR["pid"]:
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
    a = wait_for(f"avatar {path}", lambda: (lambda a: a if a.get("path") == path and not a.get("loading") and "name" in a else None)(av()), timeout, 0.25)
    first_person_body(False)
    return a


def menu_closed():
    if menu().get("open"):
        ctl("hypr3d", "menu", "close")


def redrawn(timeout=90):
    """the status after a new frame: a new map's first frame compiles every shader, which on llvmpipe outlasts hyprctl's
    5 s timeout"""
    f0 = wait_for("Hyprland to answer", st, timeout, 0.5)["frames"]
    return wait_for("a frame drawn", lambda: (lambda s: s if s["frames"] > f0 else None)(st()), timeout, 0.2)


def face_avatar(dist=2.2, pitch=-6.0, settle=1.2):
    """third person, the camera in front of the avatar looking at it"""
    ctl("hypr3d", "view", "third", dist)
    a = av()
    ctl("hypr3d", "turn", f"{a['bodyYaw'] + 180:.1f}", pitch)
    time.sleep(settle)


def sing(wav, shot=None, skip=0.6, n=8):
    """lip sync readings while pw-cat plays wav/NAME_long.wav into the test microphone"""
    alice(f"setsid -f pw-cat -p --target test_mic_in -P node.name=h3d-sing {H}/wav/{wav}_long.wav > /dev/null 2>&1")
    t0 = time.time()
    time.sleep(skip)
    reads = []
    while len(reads) < n and time.time() - t0 < 5.5:  # it plays for 6.4 s
        reads.append(lipsync())
        time.sleep(0.12)
    if shot:
        frame(shot)
    machine.execute("pkill -f 'node.name=h3d-[s]ing'; true")
    time.sleep(0.6)
    return reads


def mic_noise(on=True):
    """the test microphone's hiss (white noise at -75 dBFS, like a real one) on or off; off it gives exact zeros"""
    running = as_alice("pgrep -f 'micnoise[.]f32'")[0] == 0
    if on and not running:
        alice(f"setsid -f sh -c 'while :; do cat {H}/wav/micnoise.f32; done | pw-cat -p --raw --format f32 --rate 48000 --channels 1 "
              f"--target test_mic_in -P node.name=h3d-mic-noise -' > /dev/null 2>&1")
        time.sleep(0.5)
    elif not on and running:
        machine.execute("pkill -f 'micnoise[.]f32'; pkill -f 'node.name=h3d-mic-[n]oise'; true")
        time.sleep(0.3)


def node_id(name):
    for o in json.loads(alice("pw-dump")):
        if o.get("type") == "PipeWire:Interface:Node" and ((o.get("info") or {}).get("props") or {}).get("node.name") == name:
            return o["id"]
    return None


def lipsync_node():
    """the plugin's PipeWire stream and the nodes linked into it, or None"""
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
LUA_CFG = {}  # the plugin's values in the Lua config, set by section 9


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
    # Hyprland's stdout reaches the journal at once; its log file lags
    logged = wait_for("the log line", lambda: "[hypr3d] loaded" in machine.execute("journalctl -t start-hyprland --no-pager -n 2000")[1], 10, 0.5)
    check("0", "the log says it loaded", logged)
    BEFORE["loaded"] = calm_frame("desktop-plugin-loaded")
    d = BEFORE["desktop"].differs(BEFORE["loaded"])
    check("0", "loading it changes nothing on the screen", d < 0.002, f"{d:.2%} of pixels differ")


def maps_page():
    """the Action Menu's Maps page in TestRoom (the configured map); map_scale 2 tells the configured map apart from the
    others' guessed scale (1)"""
    maps = f"{HOME}/.local/share/hypr3d/maps"
    copy, sub = f"{maps}/Copy.glb", f"{maps}/sub/LitCourt.glb"
    alice(f"rm -rf {maps} && mkdir -p {maps}/sub {maps}/empty && cp {LIT} {copy} && cp {LIT} {sub} && echo x > {maps}/notes.txt")
    menu_closed()
    ctl("hypr3d", "menu", "open")
    root = {i["label"]: i for i in menu()["items"]}
    check("9m", "the Action Menu's main page has Maps, its hint the map you're on", root.get("Maps", {}).get("hint") == "TestRoom", root.get("Maps"))
    ctl("hypr3d", "menu", "open", "maps")
    time.sleep(0.3)
    frame("menu-maps")
    m = menu()
    items = [(i["label"], i["hint"], i["on"]) for i in m.get("items", [])]
    check("9m", "Maps: the folder's maps (a folder's by its name, not notes.txt), the configured one, the courtyard",
          m.get("path") == "main/maps" and [i[0] for i in items] == ["Copy", "sub", "TestRoom", "Courtyard"], f"{m.get('path')}: {items}")
    size = re.compile(r"\d+(\.\d)? (KB|MB|GB)")
    check("9m", "... TestRoom lit, \"here · default\"; the others their size, the courtyard \"built in\"",
          len(items) == 4 and items[2][1:] == ("here · default", True) and all(size.fullmatch(i[1]) and not i[2] for i in items[:2])
          and items[3][1:] == ("built in", False), items)
    ctl("eval", "hl.config({ plugin = { hypr3d = { map_scale = 2 } } })")
    wait_for("TestRoom at map_scale 2", lambda: (lambda m: m if not m["loading"] and m["world"] == "TestRoom" and m["scale"] == 2 else None)(ctlj("hypr3d", "map")), 30)
    redrawn()
    ctl("hypr3d", "menu", "open", "maps")
    r = ctl("hypr3d", "menu", "pick", "1")
    s, m = wait_for("Hyprland to answer", st, 90, 0.5), wait_for("Hyprland to answer", menu, 90, 0.5)  # Copy may load in between
    check("9m", "picking Copy: it's loading, the menu closed", r == "loading" and s["map"] == copy and not m.get("open"), f"{r}; {s['map']}; {s['menu']}")
    s = wait_for("Copy", lambda: (lambda s: s if not s["mapLoading"] and s["world"] == "Copy" else None)(st()), 30)
    redrawn()
    mp = ctlj("hypr3d", "map")
    check("9m", "... loaded, at the scale guessed (1), not map_scale (2: the configured map's)", s["world"] == "Copy" and mp["scale"] == 1, mp)
    ctl("hypr3d", "menu", "open", "maps")
    items = [(i["label"], i["hint"], i["on"]) for i in menu().get("items", [])]
    check("9m", "... Copy lit, \"here\"; TestRoom its size and \"default\"",
          len(items) == 4 and items[0][1:] == ("here", True) and items[2][1].endswith(" · default") and not items[2][2], items)
    # sub, then Copy in one batch (a loaded map is only taken between requests): Copy stays
    ctl("hypr3d", "menu", "close")
    out = ctl("--batch", "hypr3d menu open maps; hypr3d menu pick 2; hypr3d menu open maps; hypr3d menu pick 1")
    time.sleep(2.0)
    s = st()
    if out.count("loading") == 1 and out.rstrip().endswith("ok"):  # sub's pick says loading, Copy's ok
        log = ctl("hypr3d", "log", "40")
        check("9m", "sub on its way, Copy picked: it stays (\"staying on Copy\"), sub isn't loaded",
              s["world"] == "Copy" and s["map"] == copy and not s["mapLoading"] and "staying on Copy" in log, f"{s['world']} {s['map']}")
    else:
        check("9m", "sub on its way, Copy picked (one hyprctl --batch): sub's pick loading, Copy's ok", False, repr(out))
    ctl("hypr3d", "menu", "open", "maps")
    r = ctl("hypr3d", "menu", "pick", "4")
    redrawn()
    mp = ctlj("hypr3d", "map")
    check("9m", "the Courtyard: back at once", r == "ok" and mp["world"] == "courtyard" and mp["map"] == "", f"{r}; {mp}")
    ctl("hypr3d", "menu", "open", "maps")
    r = ctl("hypr3d", "menu", "pick", "3")
    mp = wait_for("TestRoom", lambda: (lambda m: m if not m["loading"] and m["world"] == "TestRoom" else None)(ctlj("hypr3d", "map")), 30)
    redrawn()
    check("9m", "TestRoom, the configured map, again: at map_scale (2)", r == "loading" and mp["scale"] == 2, f"{r}; {mp}")
    ctl("eval", "hl.config({ plugin = { hypr3d = { map_scale = 1 } } })")
    mp = wait_for("TestRoom at map_scale 1", lambda: (lambda m: m if not m["loading"] and m["world"] == "TestRoom" and m["scale"] == 1 else None)(ctlj("hypr3d", "map")), 30)
    time.sleep(0.5)
    s = redrawn()
    check("9m", "... map_scale 1 again: TestRoom reloaded at it, at its hypr3d_spawn", abs(s["feet"][2] - 4.0) < 0.3, f"scale {mp['scale']}; feet {s['feet']}")
    alice(f"rm -rf {maps}")


def avatars_page():
    """the Action Menu's Avatars page, with BoothAccessories configured and avatar_height 2"""
    avs = f"{HOME}/.local/share/hypr3d/avatars"
    solo = f"{avs}/Solo.vrm"
    alice(f"rm -rf {avs} && mkdir -p {avs}/Booth {avs}/pair {avs}/empty && cp {TOON} {solo} && cp {AV} {avs}/Booth/ && "
          f"cp {H}/BoothGimmicks.hands.vrma {avs}/Booth/BoothAccessories.hands.vrma && cp {TOON} {avs}/pair/ToonTest.glb && "
          f"cp {TOON} {avs}/pair/Twin.glb && echo x > {avs}/notes.txt")
    ctl("eval", "hl.config({ plugin = { hypr3d = { avatar_height = 2 } } })")
    wait_for("BoothAccessories at avatar_height 2",
             lambda: (lambda a: a if a.get("name") == "BoothAccessories" and not a["loading"] and abs(a["height"] - 2) < 0.02 else None)(av()), 60, 0.25)
    menu_closed()
    ctl("hypr3d", "menu", "open")
    items = menu().get("items", [])
    check("9a", "the main page has nine: Avatars the ninth (after the eight as they were), its hint the avatar's name",
          [i["label"] for i in items] == ["Emotes", "Expressions", "Gestures", "Outfit", "Apps", "Windows", "Options", "Maps", "Avatars"]
          and items[8]["slot"] == 9 and items[8]["hint"] == "BoothAccessories" and items[8]["submenu"], [(i["slot"], i["label"], i["hint"]) for i in items])
    r = ctl("hypr3d", "menu", "pick", "9")
    ctl("hypr3d", "menu", "back")
    check("9a", "hyprctl hypr3d menu pick 9 opens it", r == "main/avatars" and menu().get("path") == "main", r)
    press("9")
    m = menu()
    check("9a", "9 opens it", m.get("path") == "main/avatars", m.get("path"))
    frame("menu-avatars")
    items = [(i["label"], i["hint"], i["on"]) for i in m.get("items", [])]
    check("9a", "Avatars: the folder's (Booth's one by the folder's name, not its .vrma; pair's two by name; Solo.vrm), then the configured one",
          [i[0] for i in items] == ["Booth", "pair/ToonTest", "pair/Twin", "Solo", "BoothAccessories"], items)
    size = re.compile(r"\d+(\.\d)? (KB|MB|GB)")
    check("9a", "... BoothAccessories lit, \"here · default\"; the others their size",
          len(items) == 5 and items[4][1:] == ("here · default", True) and all(size.fullmatch(i[1]) and not i[2] for i in items[:4]), items)
    r = ctl("hypr3d", "menu", "pick", "4")
    s = st()
    check("9a", "picking Solo: it's loading, the menu closed", r == "loading" and s["avatar"] == solo and not menu().get("open"), f"{r}; {s['avatar']}; menu {s['menu']!r}")
    a = wait_for("Solo", lambda: (lambda a: a if a.get("name") == "Solo" and not a["loading"] else None)(av()), 60, 0.25)
    check("9a", "... loaded, at avatar_height (2 m)", a["path"] == solo and abs(a["height"] - 2) < 0.02, f"{a['path']}; {a['height']}")
    ctl("hypr3d", "menu", "open", "avatars")
    items = [(i["label"], i["hint"], i["on"]) for i in menu().get("items", [])]
    check("9a", "... Solo lit, \"here\"; BoothAccessories its size and \"default\"",
          len(items) == 5 and items[3][1:] == ("here", True) and items[4][1].endswith(" · default") and size.fullmatch(items[4][1][:-len(" · default")])
          and not items[4][2], items)
    r = ctl("hypr3d", "menu", "pick", "4")
    s = st()
    check("9a", "Solo picked again: ok, nothing loads, the menu closed", r == "ok" and s["avatar"] == solo and not s["avatarLoading"] and not menu().get("open"),
          f"{r}; {s['avatar']}, loading {s['avatarLoading']}; menu {s['menu']!r}")
    # pair/ToonTest, then Solo in one batch (a loaded avatar is only taken between requests): Solo stays
    out = ctl("--batch", "hypr3d menu open avatars; hypr3d menu pick 2; hypr3d menu open; hypr3d menu; hypr3d menu open avatars; hypr3d menu pick 4")
    time.sleep(2.0)
    s, a, log = st(), av(), ctl("hypr3d", "log", "40")
    lines = [l.strip() for l in out.splitlines() if l.strip()]
    at = out.find('{"open"')
    root = json.JSONDecoder().raw_decode(out[at:])[0] if at >= 0 else {}
    hint = next((i["hint"] for i in root.get("items", []) if i["label"] == "Avatars"), None)
    check("9a", "pair/ToonTest on its way: the main page's Avatars says \"loading…\"", hint == "loading…", f"{hint!r}; {out[:300]!r}")
    check("9a", "... Solo picked: it stays (\"keeping avatar Solo\"), pair/ToonTest isn't loaded",
          "loading" in lines and lines[-1:] == ["ok"] and a.get("name") == "Solo" and s["avatar"] == solo and not s["avatarLoading"] and "keeping avatar Solo" in log,
          f"{[l for l in lines if not l.startswith('{')]}; {a.get('name')} {s['avatar']}")
    ctl("eval", "hl.config({ plugin = { hypr3d = { avatar_height = 0 } } })")
    a = wait_for("BoothAccessories again", lambda: (lambda a: a if a.get("name") == "BoothAccessories" and not a["loading"] else None)(av()), 60, 0.25)
    check("9a", "avatar_height back to 0: the configured avatar back, at its own height", a["path"] == AV and abs(a["height"] - 2) > 0.2, f"{a['path']}; {a['height']}")
    alice(f"rm -rf {avs}")


@section("9", "config values (Lua): avatar, avatar_physics, avatar_emotes, lipsync, lipsync_gain, lipsync_source, map")
def s_config():
    mic_noise()
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
    avatars_page()
    maps_page()
    ensure_3d(False)

    cfg.update(avatar_physics=True, lipsync=True, map="", avatar_height=2.0, lipsync_gain="24", lipsync_source="Test microphone")
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
    ls = wait_for("listening", lambda: (lambda l: l if l["listening"] and l["linked"] else None)(lipsync()), 10)
    check("9", "... listening in 3D", ls["listening"] is True, ls["text"])
    check("9", "lipsync_gain = \"24\": 24 dB, set", ls["gainSetting"] == 24 and abs(ls["gain"] - 24) < 0.1, f"{ls['gainSetting']}, {ls['gain']}")
    check("9", "lipsync_source = \"Test microphone\" (a description): that one, by its node.name", ls["target"] == "test_mic" and ls["source"]["name"] == "test_mic",
          f"{ls['target']}: {ls['text']}")
    ensure_3d(False)
    cfg.update(lipsync=False, avatar_height=0.0, avatar="", lipsync_gain="auto", lipsync_source="")
    reload_config("hyprland.lua", lua_config(cfg))
    wait_for("no avatar", lambda: "name" not in av(), 20)
    check("9", "avatar = \"\": no avatar", "name" not in av())
    check("9", "lipsync = false: off again", lipsync()["on"] is False)
    check("9", "lipsync_gain = \"auto\": automatic again", lipsync()["gainSetting"] == "auto", lipsync()["gainSetting"])

    # run-time changes without a reload (hyprctl eval hl.config)
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
    tablet(16384, 16384)  # later tablet moves count from here
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
    rel(100, 40)  # 40° clockwise: the 2nd of 9 items
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
    check("1", "a wheel notch goes round to the next item", after == (before % 9) + 1 if before > 0 else after == 1, f"{before} -> {after}")
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
    want = {1: "main/emotes", 2: "main/expressions", 3: "main/gestures", 4: "main/outfit", 5: "main/apps", 6: "main/windows", 7: "main/options", 8: "main/maps",
            9: "main/avatars"}.get(h)
    check("1", "Enter picks what's highlighted", p == want, f"highlight {h}: {p}")
    press("backspace")
    # the tablet's absolute positions move the cursor by their change (Backspace re-centred it)
    tablet(16384, 16384 - 6000)
    h = menu()["highlight"]
    check("1", "the tablet (absolute motion) moves the cursor too: up to Emotes", h == 1, f"highlight {h}")
    tablet(16384 + 3150, 16384 - 6000)  # 147 px up, 123 right: 40° clockwise
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
    press("w", hold=1.2)  # walks at 1.6 m/s
    s2 = st()
    check("1", "W walks (the menu closed)", abs(s2["feet"][0] - s["feet"][0]) + abs(s2["feet"][2] - s["feet"][2]) > 0.4, f"{s['feet']} -> {s2['feet']}")
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
    for _ in range(40):  # until the crosshair is on h3d-left
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
    tablet(16384, 16384)  # menu closed, so the camera turns
    press("tab")
    for k in ("4", "8", "8", "1"):
        press(k)
    m = menu()
    check("2", "Tab, 4, More, More, 1: the dial of the outfit's first slider on its third page", m.get("path") == "main/outfit:3/~ニーハイの緩さ" and m.get("dial"), m.get("path"))
    frame("dial-open")
    rel(150, 0)
    v_mouse = menu()["dial"]["value"]
    # a count is a logical pixel (libinput's unaccelerated motion) and the menu's middle is 224 px across at 800 px
    # high, so 150 counts turn the dial atan2(150 / 224, 0.66)
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
    # the 7 s clip lasts (7 - 0.3) / speed + 0.3 s with its fade; under 20 fps animations step 50 ms a frame at most, so
    # then the frames count
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
    ctl("hypr3d", "tile", "follow", "on")  # plain Y made tiling's ring stay; undo it
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


@section("mic", "lip sync from PipeWire's default source (the test microphone): vowels, a quieter microphone and the gain; muted, silent, suspended, gone, missing, unlinked")
def s_mic():
    ensure_avatar(AV)
    ensure_3d()
    menu_closed()
    mic_noise()
    src = alice("wpctl inspect @DEFAULT_AUDIO_SOURCE@")
    check("mic", "the default source is the test microphone", 'node.name = "test_mic"' in src, re.findall(r'node.name = "[^"]*"', src))
    check("mic", "no hypr3d stream before lip sync is on", lipsync_node() is None)
    ls = json.loads(ctl("hypr3d", "avatar", "lipsync", "on"))
    check("mic", "avatar lipsync on: on, listening, a microphone", ls["on"] and ls["listening"] and ls["microphone"], ls)
    node = wait_for("the stream's link", lambda: (lambda n: n if n and n["fed_by"] else None)(lipsync_node()), 10)
    check("mic", "pw-dump: the \"hypr3d lip sync\" stream, fed by the test microphone",
          node["props"].get("node.description") == "hypr3d lip sync" and "test_mic" in node["fed_by"], f"{node['props'].get('media.class')} from {node['fed_by']}")
    keys = ("text", "problem", "stream", "linked", "source", "peak", "rms", "silentFor", "sinceData", "buffers", "emptyBuffers", "gain", "room")
    # peak and RMS need a full second
    ls = wait_for("its report", lambda: (lambda l: l if l["problem"] == "none" and l["rms"] is not None else None)(lipsync()), 10)
    check("mic", "its report: linked to the test microphone, not muted, its hiss heard (-75 dBFS); the badge names it",
          ls["linked"] and (ls["source"] or {}).get("name") == "test_mic" and ls["source"]["muted"] is False and ls["text"] == "lip sync: listening (Test microphone)"
          and ls["rms"] is not None and -85 < ls["rms"] < -65 and ls["silentFor"] < 0.1, {k: ls[k] for k in keys})
    face_avatar(1.3, -2)
    img = calm_frame("lipsync-badge")
    corner = (img.w - 420, 0, img.w, 80)
    red = img.count(lambda r, g, b: r > 170 and g < 90 and b < 90, corner)
    check("mic", "the badge in the top right corner (its red dot)", red > 20, f"{red} red pixels")
    shut = lipsync()
    check("mic", "the microphone's hiss alone: the mouth shut", max(shut["visemes"].values()) < 0.05, shut["visemes"])
    results = {}

    def vowels(prefix, what, least=0.5):
        opened = []
        for who in ("man", "woman"):
            for k, v in enumerate(VOWELS):
                reads = sing(f"{prefix}{who}_{v}", f"lipsync-{prefix}woman-a" if (who, v) == ("woman", "a") else None)
                med = {n: sorted(r["visemes"][n] for r in reads)[len(reads) // 2] for n in VISEMES}
                best = max(VISEMES, key=lambda n: med[n])
                lvl = sorted(r["level"] for r in reads)[len(reads) // 2]
                f1 = sorted(r["formants"][0] for r in reads)[len(reads) // 2]
                f2 = sorted(r["formants"][1] for r in reads)[len(reads) // 2]
                results[f"{prefix}{who}_{v}"] = {"level": lvl, "formants": [f1, f2], "visemes": med, "readings": reads}
                opened.append(med[best])
                check("mic", f"{what}{who}'s {v}: {VISEMES[k]} the most", best == VISEMES[k] and med[best] > least,
                      f"{best} {med[best]:.2f}; level {lvl:.0f} dB, F1 {f1:.0f} F2 {f2:.0f} Hz, gain {reads[-1]['gain']:+.0f} dB; " + " ".join(f"{n} {med[n]:.2f}" for n in VISEMES))
        return opened

    own = vowels("", "")
    for f in ("silence", "hiss", "quiet_a", "o_then_hiss"):
        # o_then_hiss: after a consonant's moment the hiss must leave the mouth shut (read from 0.8 s in); quiet_a: a
        # whisper 40 dB down, kept shut by the gain the normal voices set
        reads = sing(f, skip=1.6 if f == "o_then_hiss" else 0.6)
        most = max(max(r["visemes"].values()) for r in reads)
        results[f] = {"readings": reads}
        check("mic", f"{f}: the mouth stays shut", most < 0.05, f"the most {most:.2f}, level {reads[len(reads) // 2]['level']:.0f} dB, gain {reads[-1]['gain']:+.0f} dB")

    # 30 dB quieter: the automatic gain makes up for it once the normal voices leave its 15 s window
    time.sleep(15.5)
    quiet = vowels("q30_", "30 dB quieter: ")
    ls = lipsync()
    check("mic", "... the gain made up for it: about +30 dB, and the mouth as wide as at their own level",
          20 <= ls["gain"] <= 40 and min(q - o for q, o in zip(quiet, own)) > -0.1, f"gain {ls['gain']:+.1f} dB, the voice at {ls['reference']:.0f} dBFS, the room at {ls['room']} dBFS; "
          f"opened {[round(q, 2) for q in quiet]} vs {[round(o, 2) for o in own]}")
    # a fixed gain: at 0 dB the quiet vowels barely open, 30 dB makes up for them
    r = ctlj("hypr3d", "avatar", "lipsync", "gain", "0")
    reads = sing("q30_man_a")
    at0 = sorted(x["visemes"]["aa"] for x in reads)[len(reads) // 2]
    ctl("hypr3d", "avatar", "lipsync", "gain", "30")
    reads = sing("q30_man_a")
    at30 = sorted(x["visemes"]["aa"] for x in reads)[len(reads) // 2]
    back = ctlj("hypr3d", "avatar", "lipsync", "gain", "auto")
    check("mic", "avatar lipsync gain 0 / 30 / auto: the quiet a barely opens at 0 dB, wide at 30, and back to automatic",
          r["gainSetting"] == 0 and at0 < 0.4 and at30 > 0.5 and back["gainSetting"] == "auto", f"aa {at0:.2f} at 0 dB, {at30:.2f} at 30 dB; {back['gainSetting']}")
    # Options > Mic gain: a dial, its first step automatic, up to 60 dB
    press("tab")
    press("7")
    items = {i["label"]: i for i in menu()["items"]}
    press(str(items["Mic gain"]["slot"]))
    dial = menu().get("dial") or {}
    press("5")  # 4/7 of the way: 34 dB
    set34 = lipsync()["gainSetting"]
    hint = next((i["hint"] for i in menu()["items"] if i["label"] == "Mic gain"), "")
    press("1")  # the first: automatic
    auto = lipsync()["gainSetting"]
    press("esc")
    check("mic", "Options > Mic gain: a dial; round to 4/7 sets 34 dB, back to its start automatic", dial.get("label") == "Mic gain" and set34 == 34 and auto == "auto",
          f"dial {dial}, {set34} ({hint}), then {auto}")

    def fresh():
        """restarts lip sync: it notifies once per start, so its next notification is this one"""
        ctl("hypr3d", "avatar", "lipsync", "off")
        ctl("hypr3d", "avatar", "lipsync", "on")
        ctl("dismissnotify")
        return wait_for("listening again", lambda: (lambda l: l if l["linked"] and l["problem"] == "none" else None)(lipsync()), 10)

    def told(text):
        """the plugin's log has it (notifications included)"""
        return text in ctl("hypr3d", "log")

    mic = node_id("test_mic")
    fresh()
    alice(f"wpctl set-mute {mic} 1")
    ls = wait_for("muted", lambda: (lambda l: l if l["problem"] == "muted" and l["silentFor"] > 0.5 else None)(lipsync()), 6)
    check("mic", "muted in PipeWire (wpctl set-mute): \"Test microphone is muted\", exact zeros, the source's mute", ls["text"] == "lip sync: Test microphone is muted"
          and ls["source"]["muted"] is True and ls["silentFor"] > 0.5, {k: ls[k] for k in keys})
    check("mic", "... and a notification says how to unmute it", told("is muted in PipeWire: wpctl set-mute"))
    alice(f"wpctl set-mute {mic} 0")
    ls = wait_for("unmuted", lambda: (lambda l: l if l["problem"] == "none" else None)(lipsync()), 6)
    check("mic", "... unmuted: listening again", ls["text"] == "lip sync: listening (Test microphone)", ls["text"])
    fresh()
    mic_noise(False)
    ls = wait_for("silent", lambda: (lambda l: l if l["problem"] == "silent" else None)(lipsync()), 8)
    check("mic", "exact zeros, not muted (as a microphone's own mute button gives): \"no sound from Test microphone (muted?)\"",
          ls["text"] == "lip sync: no sound from Test microphone (muted?)" and ls["source"]["muted"] is False and ls["silentFor"] >= 2 and ls["peak"] is None,
          {k: ls[k] for k in keys})
    frame("lipsync-badge-silent")
    check("mic", "... and a notification: is it muted?", told("sends only silence: is it muted, maybe by its own button?"))
    mic_noise()
    ls = wait_for("its hiss again", lambda: (lambda l: l if l["problem"] == "none" else None)(lipsync()), 6)
    check("mic", "... its hiss back: listening", ls["text"] == "lip sync: listening (Test microphone)", ls["text"])
    # WirePlumber suspends an unused source after 5 s; lip sync on must resume it
    ctl("hypr3d", "avatar", "lipsync", "off")
    mic_noise(False)  # its stream keeps the source running

    def state_of(name):
        return next((o["info"].get("state") for o in json.loads(alice("pw-dump")) if o.get("type") == "PipeWire:Interface:Node"
                     and ((o.get("info") or {}).get("props") or {}).get("node.name") == name), None)
    try:
        suspended = wait_for("suspended", lambda: state_of("test_mic") == "suspended", 15, 0.5)
    except TimeoutError:
        suspended = False
    ctl("hypr3d", "avatar", "lipsync", "on")
    seen = []
    for _ in range(10):
        l = lipsync()
        seen.append((l["problem"], (l["source"] or {}).get("state"), l["stream"], l["samples"], l["silentFor"], l["sinceData"]))
        time.sleep(0.25)
    note("mic", "the test microphone suspended, then lip sync on: what it saw, every 0.25 s", f"suspended first: {suspended}; {seen}")
    mic_noise()
    ls = wait_for("its hiss", lambda: (lambda l: l if l["problem"] == "none" else None)(lipsync()), 8)
    reads = sing("man_a")
    aa = sorted(x["visemes"]["aa"] for x in reads)[len(reads) // 2]
    check("mic", "... it runs again for lip sync: samples at once, its hiss, and a vowel opens the mouth",
          suspended and seen[2][1] == "running" and seen[2][3] > 0 and aa > 0.5, f"aa {aa:.2f}; {reads[-1]['text']}")
    alice("setsid -f pw-loopback -n h3d-mic2 --capture-props='media.class=Audio/Sink node.name=test_mic2_in node.description=\"Test microphone 2 in\" audio.position=[MONO]' "
          "--playback-props='media.class=Audio/Source node.name=test_mic2 node.description=\"Test microphone 2\" audio.position=[MONO]' > /dev/null 2>&1")
    mic2 = wait_for("the second microphone", lambda: node_id("test_mic2"), 10)
    alice(f"wpctl set-default {mic2}")
    fresh_ok = True
    try:
        ctl("hypr3d", "avatar", "lipsync", "off")
        ctl("hypr3d", "avatar", "lipsync", "on")
        ls = wait_for("linked to the second", lambda: (lambda l: l if l["linked"] and l["source"]["name"] == "test_mic2" else None)(lipsync()), 10)
    except TimeoutError as e:
        fresh_ok, ls = False, {"error": str(e)}
    check("mic", "a second microphone made the default: lip sync listens to it", fresh_ok, ls.get("text", ls))
    machine.execute("pkill -f 'pw-loopback -n h3d-[m]ic2'; true")
    time.sleep(3)
    ls, node = lipsync(), lipsync_node()
    note("mic", "... unplugged (pw-loopback gone): what lip sync says, and pw-dump", f"{ {k: ls[k] for k in keys} }; fed by {node and node['fed_by']}")
    check("mic", "... its report agrees with pw-dump (linked when a link feeds it), and the badge with the report",
          ls["linked"] == bool(node and node["fed_by"]) and (ls["linked"] or ls["problem"] in ("unlinked", "error")), f"{ls['text']}; fed by {node and node['fed_by']}")
    alice(f"wpctl set-default {mic}")
    ls = fresh()
    check("mic", "... the test microphone the default again: listening to it", ls["source"]["name"] == "test_mic", ls["text"])
    # WirePlumber links a stream whose target is missing to the default source
    ctl("hypr3d", "avatar", "lipsync", "source", "no_such_mic")
    ls = wait_for("the one missing", lambda: (lambda l: l if l["problem"] == "missing" else None)(lipsync()), 8)
    check("mic", "avatar lipsync source no_such_mic: \"no no_such_mic (listening to Test microphone)\", the default in its place",
          ls["text"] == "lip sync: no no_such_mic (listening to Test microphone)" and ls["linked"] and ls["target"] == "no_such_mic",
          {k: ls[k] for k in keys + ("error", "target", "coreError")})
    check("mic", "... and a notification: which there are", told("no microphone no_such_mic (lipsync_source), so Test microphone instead"))
    ctl("hypr3d", "avatar", "lipsync", "source", "Test microphone")  # a description, as wpctl status lists it
    ls = wait_for("linked by its description", lambda: (lambda l: l if l["linked"] else None)(lipsync()), 10)
    check("mic", "avatar lipsync source \"Test microphone\" (its description): found, and listened to",
          ls["target"] == "test_mic" and ls["source"]["name"] == "test_mic" and ls["problem"] in ("none", "starting"), f"{ls['target']}: {ls['text']}")
    ctl("hypr3d", "avatar", "lipsync", "source", "default")
    ls = wait_for("the default again", lambda: (lambda l: l if l["linked"] and l["target"] == "" else None)(lipsync()), 10)
    check("mic", "avatar lipsync source default: the default microphone again", ls["source"]["name"] == "test_mic", ls["text"])
    # no WirePlumber, no link: whatever PipeWire gives the stream, it must say "no microphone linked"
    alice("systemctl --user stop wireplumber")
    ctl("hypr3d", "avatar", "lipsync", "off")
    ctl("hypr3d", "avatar", "lipsync", "on")
    ls = wait_for("unlinked", lambda: (lambda l: l if l["problem"] == "unlinked" else None)(lipsync()), 8)
    b0, t0 = ls["buffers"], time.time()
    time.sleep(1.0)
    ls = lipsync()
    note("mic", "a stream nothing links: what PipeWire gives it", f"{(ls['buffers'] - b0) / (time.time() - t0):.0f} buffers a second, {ls['samples']} samples in all, "
         f"exact zeros for {ls['silentFor']} s, {ls['emptyBuffers']} flagged empty; stream {ls['stream']}")
    check("mic", "unlinked (no WirePlumber to link it): \"no microphone linked\"", ls["text"] == "lip sync: no microphone linked" and not ls["linked"], {k: ls[k] for k in keys})
    alice("systemctl --user start wireplumber")
    ls = wait_for("linked again", lambda: (lambda l: l if l["linked"] else None)(lipsync()), 15)
    check("mic", "... WirePlumber back: linked to the test microphone again", ls["source"]["name"] == "test_mic", ls["text"])
    (LOGS / "lipsync.json").write_text(json.dumps(results, indent=1))
    ctl("dismissnotify")

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
    """an rgb(ff8800) notification's left bar and progress line"""
    return r > 200 and 100 < g < 180 and b < 80


def red(r, g, b):
    """the lip sync badge's dot"""
    return r > 170 and g < 90 and b < 90


def notification_boxes(img):
    """boxes (x, y, w, h) of Hyprland's top-right notifications, top down, by their orange bars"""
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
    mic_noise()  # else a silent-mic notification covers the corner
    face_avatar(2.0, -4)
    ctl("hypr3d", "avatar", "lipsync", "on")
    wait_for("listening", lambda: lipsync()["listening"], 10)
    ctl("dismissnotify")  # lip sync's "on"
    time.sleep(1.2)
    b = wait_for("the badge", lambda: lipsync()["badge"], 5)
    img = frame("badge-alone")
    check("13", "alone, the badge sits in the top right corner, 12 px in", b[1] == 12 and b[0] + b[2] == img.w - 12, b)
    n = img.count(red, box_of(b))
    check("13", "... where the frame has it (its red dot)", n > 20, f"{n} red pixels in {b}")
    ctl("notify", "1", "15000", "rgb(ff8800)", "hypr3d: a notification in the badge's corner")
    time.sleep(1.2)  # slides in for 0.6 s
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
    """wev alone on the workspace, logging a line per event"""
    machine.execute("pkill -u alice foot; pkill -x wev; true")
    time.sleep(1)
    # wev appends too, else it writes over the marks; stdbuf -oL: a line at a time
    alice("rm -f /tmp/wev.log; setsid -f stdbuf -oL wev >> /tmp/wev.log 2>&1")
    wait_for("wev", lambda: any(c["class"] == "wev" for c in json.loads(ctl("-j", "clients"))), 20)
    time.sleep(1.5)


def wev_mark(name):
    alice(f"echo {shlex.quote('### ' + name)} >> /tmp/wev.log")  # root can't write alice's file in /tmp


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
    """xdg_toplevel states per configure from mark `name` to the next (wev prints them on the next line)"""
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
    """axis events per frame: (event, text without the time)"""
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
    """wheel.py's high-resolution wheel turned by each step, in 120ths of a notch ("h": horizontal)"""
    machine.succeed("python3 " + H + "/wheel.py " + " ".join(str(v) for v in steps), timeout=30)
    time.sleep(0.5)


def touchpad(*steps):
    """two-finger moves on touchpad.py's touchpad, one per step, 30 units a millimetre ("h": right, "d": down and right,
    "z": a pinch)"""
    machine.succeed("python3 " + H + "/touchpad.py " + " ".join(str(v) for v in steps), timeout=30)
    time.sleep(0.5)


WHEEL_SEQ = [("a notch down", lambda: wheel(1)), ("half notches: down, down, down, up", lambda: wheel_hires(60, 60, 60, -60)),
             ("half notches of the horizontal wheel, right", lambda: wheel_hires("h60", "h60"))]
# wheel.py's device and a window rule for wev; Hyprland's scroll factor is the rule's, else the device's, else input's
WHEEL_DEV = 'hl.device({{ name = "hypr3d-test-wheel", scroll_factor = {} }})'
WEV_RULE = 'hl.window_rule({ name = "h3d-wev-scroll", match = { class = "wev" }, scroll_mouse = 3, scroll_touchpad = 2 })'
WEV_RULE_OFF = 'hl.window_rule({ name = "h3d-wev-scroll", enabled = false })'
EMULATE = 'hl.config({{ input = {{ emulate_discrete_scroll = {} }} }})'
# (case, Lua, [(action, how, Hyprland's 2D frames as (axis, value, value120, discrete), or None)]), each case on top of
# the ones before; "the other mouse" is QEMU's
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
    """{(case, what): (wev_scrolls, wev_axis_frames)} for every case of WHEEL_CASES"""
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
    # the 2D desktop first, from Hyprland itself; the pointer only enters wev when it moves
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
            # frames too: a touchpad's axes and axis_stop
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


AIM = {}  # the last aim_at's tries


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


def walked_off(timeout=20):
    """after hyprctl hypr3d walk: till the feet stop (slow frames stretch the walk)"""
    time.sleep(0.5)
    last = [None]

    def still():
        f = st()["feet"]
        done = last[0] is not None and math.dist(f, last[0]) < 0.002
        last[0] = f
        return done
    wait_for("the walk done", still, timeout, 0.3)


def settled(timeout=6):
    return wait_for("the windows to settle", lambda: (lambda w: w if all(p["settled"] for p in w["placed"]) else None)(windows3d()), timeout, 0.3)


@section("15", "carrying windows: G, clicks, Esc, X, the wheel (bigger, smaller), Ctrl+wheel (nearer, further), hyprctl, and a placed window that keeps drawing")
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
    h = windows3d()["hold"]
    s1 = h["size"]
    check("15", "the wheel, two notches down: smaller where it is (0.95 a notch)", abs(s1 - s0 * WHEEL_SIZE**2) < 0.01 and abs(h["dist"] - d0) < 0.001, f"{s0:.3f} -> {s1:.3f}; {d0:.3f} m -> {h['dist']:.3f} m")
    qmp([key_event("ctrl", True)])
    time.sleep(0.1)
    wheel(2)
    qmp([key_event("ctrl", False)])
    time.sleep(0.3)
    h = windows3d()["hold"]
    d1 = h["dist"]
    check("15", "Ctrl+wheel, two notches down: it comes closer (0.9 of the way a notch), as big", abs(d1 - d0 * 0.81) < 0.01 and abs(h["size"] - s1) < 0.001,
          f"{d0:.3f} m -> {d1:.3f} m; {s1:.3f} -> {h['size']:.3f}")
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
    check("15", "hyprctl hypr3d grab, hold 1.5 0.5: holding it 1.5 m out at half size", r1 == "holding" and r2 == "ok" and w["hold"]["dist"] == 1.5 and w["hold"]["size"] == 0.5,
          f"{r1}, {r2}; {w['hold']}")
    r3 = ctl("hypr3d", "place")
    s = st()
    check("15", "hyprctl hypr3d place: the status says placed 1, holding false", r3 == "placed" and s["placed"] == 1 and s["holding"] is False, f"{r3}; {s['placed']} {s['holding']}")
    r4 = ctl("hypr3d", "grab")
    r5 = ctl("hypr3d", "grab")
    check("15", "hyprctl hypr3d grab twice: picked up, then put down (like G)", r4 == "holding" and r5 == "placed", f"{r4}, {r5}")
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
    r = ctl("hypr3d", "reset-windows", "forget")  # forget: also their classes' spots
    w = wait_for("everything back on the wall", lambda: (lambda w: w if not w["placed"] else None)(windows3d()), 8, 0.3)
    check("15", "hyprctl hypr3d reset-windows: all back on the wall", r == "ok" and w is not None and windows3d()["spots"] == 0, windows3d())
    machine.execute("pkill -f tick.sh; true")


@section("15t", "third person: the avatar carries a window, out past the avatar (made bigger, pulled in, one that was nearer), as big on a wall as it was carried; one opening in 3D is as big as the camera needs")
def s_grab_third():
    ensure_avatar(AV)
    ensure_3d()
    menu_closed()
    ctl("hypr3d", "reset-windows", "forget")
    ctl("hypr3d", "view", "third", "2.6", "0.4")
    ctl("hypr3d", "spawn")
    time.sleep(1.2)

    def past(g, s):
        """how far a placement's middle is past the avatar's feet, along the view"""
        return relative(g, s)[0] - relative({"center": s["feet"]}, s)[0]

    def drawn(cls):
        settled()
        return st(), placed(cls)

    a = aim_at("h3d-left")
    press("g")
    s, g = drawn("h3d-left")
    h = windows3d()["hold"]
    check("15t", "G in third person picks it up where it is: its distance counts from the avatar",
          a and g and g["held"] and h and abs(past(g, s) - h["dist"]) < 0.15 and past(g, s) > 0.7, f"aimed {a}; {g}; {past(g, s) if g else 0:.2f} m past the avatar; hold {h}")
    s0 = h["size"]
    wheel(-3)
    s1, g1 = drawn("h3d-left")
    h1 = windows3d()["hold"]
    # with something in the way it's drawn nearer and smaller, looking as big
    check("15t", "the wheel, three notches up: bigger (1/0.95 a notch), looks it, as far out",
          abs(h1["size"] - s0 / WHEEL_SIZE**3) < 0.02 and abs(h1["dist"] - h["dist"]) < 0.001 and abs(g1["apparent"] / g["apparent"] - 1 / WHEEL_SIZE**3) < 0.03 and past(g1, s1) > past(g, s) - 0.5,
          f"{s0:.3f} -> {h1['size']:.3f} (drawn {g1['size']:.3f}); looks {g['apparent']:.3f} -> {g1['apparent']:.3f}; {past(g, s):.2f} -> {past(g1, s1):.2f} m past the avatar")
    frame("third-bigger")
    qmp([key_event("ctrl", True)])
    time.sleep(0.1)
    wheel(25)
    qmp([key_event("ctrl", False)])
    time.sleep(0.5)
    s2, g2 = drawn("h3d-left")
    h2 = windows3d()["hold"]
    check("15t", "Ctrl+wheel, 25 notches down: as near as it comes, still out past the avatar (not between the camera and it)",
          abs(h2["dist"] - 0.6) < 0.01 and past(g2, s2) > 0.35 and relative(g2, s2)[0] > av()["distance"] + 0.3, f"hold {h2}; {past(g2, s2):.2f} m past the avatar, boom {av()['distance']:.2f} m")
    frame("third-nearest")
    press("g")

    # placed 0.8 m ahead in first person, the avatar 1.6 m on: it's between the camera and the avatar
    ctl("hypr3d", "reset-windows", "forget")
    ctl("hypr3d", "view", "first")
    ctl("hypr3d", "spawn")
    time.sleep(1.0)
    aim_at("h3d-left")
    ctl("hypr3d", "turn", f"{st()['yaw']:.2f}", "0")
    ctl("hypr3d", "grab")
    ctl("hypr3d", "hold", "0.8")
    settled()
    ctl("hypr3d", "place")
    s = st()
    yaw = math.radians(s["yaw"])
    f = s["feet"]
    ctl("hypr3d", "tp", f"{f[0] + 1.6 * math.sin(yaw):.3f}", f"{f[1]:.3f}", f"{f[2] - 1.6 * math.cos(yaw):.3f}")
    ctl("hypr3d", "view", "third", "2.6", "0.4")
    time.sleep(1.5)
    s3, g3 = st(), placed("h3d-left")
    a = st()["aimed"]
    press("g")
    s4, g4 = drawn("h3d-left")
    check("15t", "a window between the camera and the avatar, picked up: out past the avatar",
          g3 and past(g3, s3) < -0.3 and a and a["class"] == "h3d-left" and g4 and g4["held"] and past(g4, s4) > 0.6,
          f"{past(g3, s3) if g3 else 0:.2f} m past the avatar, aimed {a}; picked up: {past(g4, s4) if g4 else 0:.2f} m past the avatar, hold {windows3d()['hold']}")
    press("g")

    # the courtyard's south gate: the doors' face is at z 21.88
    ctl("hypr3d", "reset-windows", "forget")
    ctl("hypr3d", "spawn")
    time.sleep(1.0)
    aim_at("h3d-left")
    press("g")
    wheel(-4)
    size = windows3d()["hold"]["size"]
    ctl("hypr3d", "tp", "0", "0", "20.88")
    ctl("hypr3d", "turn", "180", "0")
    time.sleep(1.2)
    s5, g5 = drawn("h3d-left")
    check("15t", "made bigger and carried to the gate, the doors 1 m past the avatar: flat on them, as big",
          g5 and g5["held"] and 21.8 < g5["center"][2] < 21.88 and abs(g5["size"] - size) < 0.02 and 0.8 < past(g5, s5) < 1.1,
          f"{g5 and g5['center']}, size {g5 and g5['size']} (made {size:.3f}), {past(g5, s5) if g5 else 0:.2f} m past the avatar")
    frame("third-on-the-doors")
    press("g")
    time.sleep(0.5)
    g6 = placed("h3d-left")
    check("15t", "... G puts it down there, as big", g6 and not g6["held"] and abs(g6["size"] - size) < 0.02 and g5 and math.dist(g6["center"], g5["center"]) < 0.02,
          f"{g6}")

    # opening in third person, it's sized for the camera (THIRD_FIT of the view), not for the avatar's eye
    ctl("hypr3d", "reset-windows", "forget")
    ctl("hypr3d", "spawn")
    ctl("hypr3d", "turn", f"{st()['yaw']:.2f}", "0")
    time.sleep(1.5)
    alice("setsid -f foot --app-id h3d-third > /dev/null 2>&1")
    g7 = wait_for("it in front of you", lambda: placed("h3d-third"), 20, 0.3)
    time.sleep(0.5)
    s7, g7 = drawn("h3d-third")
    (w, h), (mw, mh) = client("h3d-third")["size"], logical_size()
    ahead = relative({"center": s7["feet"]}, s7)[0] + 1.5
    view_h = 2 * ahead * math.tan(HALF_FOV)
    reach = THIRD_FIT * view_h / 2
    band = s7["eye"][1] + reach - max(s7["eye"][1] - reach, s7["feet"][1] + 0.05)
    want = min(view_h / mh, THIRD_FIT * view_h / h, THIRD_FIT * view_h * mw / mh / w, band / h) / (view_h / mh)
    before = on_screen("h3d-third") * 1.5 / ahead  # its first-person size, seen from the camera
    bottom = g7["center"][1] - g7["height"] / 2 if g7 else 0
    check("15t", "a terminal opening in 3D in third person: 1.5 m past the avatar, as big as fits the view from the camera above the ground, standing on it",
          g7 and abs(past(g7, s7) - 1.5) < 0.1 and abs(g7["apparent"] - want) < 0.03 and bottom > s7["feet"][1] + 0.03 and want > 2 * before,
          f"{g7 and g7['center']}, {past(g7, s7):.2f} m past the avatar; looks {g7 and g7['apparent']:.3f} (want {want:.3f}; from the eye it looked {before:.3f}); bottom {bottom:.2f} m, feet {s7['feet'][1]:.2f}")
    frame("third-opened")
    machine.execute("pkill -f 'app-id h3d-third'; true")
    # as tall as the screen: THIRD_FIT round the middle would reach 0.8 m into the ground, so it's fit above it
    ctl("hypr3d", "reset-windows", "forget")
    ctl("dispatch", 'hl.dsp.focus({ workspace = "3" })')
    time.sleep(1.0)
    alice("setsid -f foot --app-id h3d-tall > /dev/null 2>&1")
    wait_for("it in front of you", lambda: placed("h3d-tall"), 20, 0.3)
    time.sleep(0.5)
    s8, g8 = drawn("h3d-tall")
    (w, h), (mw, mh) = client("h3d-tall")["size"], logical_size()
    band = s8["eye"][1] + reach - max(s8["eye"][1] - reach, s8["feet"][1] + 0.05)
    want = min(view_h / mh, THIRD_FIT * view_h / h, THIRD_FIT * view_h * mw / mh / w, band / h) / (view_h / mh)
    bottom = g8["center"][1] - g8["height"] / 2 if g8 else 0
    check("15t", "... alone on a workspace, as tall as the screen: made smaller to fit the view above the ground, and standing on it",
          g8 and abs(g8["apparent"] - want) < 0.03 and band / h < THIRD_FIT * view_h / h and abs(bottom - s8["feet"][1] - 0.05) < 0.05 and abs(past(g8, s8) - 1.5) < 0.1,
          f"{w}x{h} px; looks {g8 and g8['apparent']:.3f} (want {want:.3f}); bottom {bottom:.2f} m, feet {s8['feet'][1]:.2f}; {past(g8, s8):.2f} m past the avatar")
    frame("third-opened-tall")
    machine.execute("pkill -f 'app-id h3d-tall'; true")
    ctl("dispatch", 'hl.dsp.focus({ workspace = "1" })')
    time.sleep(0.5)
    ctl("hypr3d", "reset-windows", "forget")
    ctl("hypr3d", "view", "first")


@section("16", "a second monitor: 3D on one while the other stays 2D, then on the other; the pointer, focus, notifications")
def s_monitors():
    ensure_avatar(AV)
    ensure_3d(False)
    # Hyprland's headless output (QEMU shows a second virtio-gpu output only in a window); Virtual-1 pinned at 0x0, as
    # "auto" would put the new one first
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
    # the first's window lost focus: its border changed
    check("16", "... its frame is 3D, the first's its desktop", d2 > 0.3 and d1 < 0.02, f"{d2:.1%} and {d1:.2%} of pixels changed")
    ctl("notify", "1", "8000", "rgb(ff8800)", "hypr3d: over the second monitor's 3D view")
    time.sleep(1.2)
    two = frame("notification-second-3d", "H3D-2")
    check("16", "... notifications show over it", len(notification_boxes(two)) == 1, notification_boxes(two))
    ctl("dismissnotify")
    # aquamarine < 0.12.1 (0.55.2 has 0.11.0) can run a headless output's queued idle event after the output is freed
    # (CBackend::dispatchIdle; fixed upstream in 1699271, 6ecde03); the plugin holds the output till it ran
    # (holdOutput() in main.cpp)
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


def second_monitor():
    """headless H3D-2 right of Virtual-1, as section 16 makes it: hyprctl's answer, the monitors"""
    ctl("eval", 'hl.monitor({ output = "Virtual-1", mode = "preferred", position = "0x0", scale = 1 })')
    r = ctl("output", "create", "headless", "H3D-2")
    wait_for("H3D-2", lambda: "H3D-2" in monitors(), 10)
    ctl("eval", 'hl.monitor({ output = "H3D-2", mode = "1280x800@60", position = "1280x0", scale = 1 })')
    m = wait_for("H3D-2 at 1280x0", lambda: (lambda m: m if m.get("H3D-2", {}).get("x") == 1280 and m["H3D-2"]["width"] == 1280 and m["Virtual-1"]["x"] == 0 else None)(monitors()), 10)
    return r, m


def cursor():
    x, y = ctl("cursorpos").split(",")
    return float(x), float(y)


def cursor_pixels(img, at):
    """Hyprland's cursor pixels near `at` in a frame of the first monitor"""
    x, y = int(at[0]), int(at[1])
    return img.count(cursor_cyan, (x - 20, y - 20, x + 50, y + 50))


@section("16b", "3D on the monitor plugin:hypr3d:monitor names, the other one the desktop meanwhile: the mouse and keyboard there and back (Super+Esc, a keybind's focus, the mouse)")
def s_away():
    ensure_avatar(AV)
    ensure_3d(False)
    r, m = second_monitor()
    check("16b", "a second monitor, right of the first", r == "ok" and m, {n: (v["x"], v["width"]) for n, v in m.items()})
    # wev fills the first monitor; keybinds that move the focus, as a desktop config has
    ctl("dispatch", 'hl.dsp.focus({ monitor = "Virtual-1" })')
    wev_start()
    ctl("dispatch", 'hl.dsp.cursor.move({ x = 600, y = 400 })')
    rel(20, 10)
    ctl("eval", 'hl.bind("SUPER + Left", hl.dsp.focus({ direction = "left" }))')
    ctl("eval", 'hl.bind("SUPER + Right", hl.dsp.focus({ direction = "right" }))')
    ctl("eval", 'hl.config({ plugin = { hypr3d = { monitor = "H3D-2" } } })')
    home = cursor()
    ctl("dismissnotify")
    time.sleep(1.2)
    one2d, two2d = frame("away-first-2d", "Virtual-1"), frame("away-second-2d", "H3D-2")
    n2d = cursor_pixels(one2d, home)

    ensure_3d()  # the first monitor focused
    s = st()
    check("16b", "plugin:hypr3d:monitor = H3D-2: 3D goes there, though the first has the focus", s["monitor"] == "H3D-2" and s["away"] is False,
          f"{s['monitor']}, away {s['away']}")
    c = cursor()
    check("16b", "... the cursor goes to it (and the focus)", c[0] >= 1280 and focused_monitor() == "H3D-2", f"cursor {home} -> {c}, focused {focused_monitor()}")
    time.sleep(1)
    one, two = frame("away-first-while-3d", "Virtual-1"), frame("away-second-3d", "H3D-2")
    d1, d2, n3d = one2d.differs(one), two2d.differs(two), cursor_pixels(one, home)
    # wev lost focus: its border changed
    check("16b", "grim -o: the second shows 3D, the first its desktop as it was, the cursor gone from it", d2 > 0.3 and d1 < 0.02 and n2d > 20 and n3d < 5,
          f"{d2:.1%} and {d1:.2%} of pixels changed; cursor pixels {n2d} -> {n3d}")
    y0 = st()["yaw"]
    rel(300, 0)
    check("16b", "... the mouse turns the camera, the cursor staying", abs(st()["yaw"] - y0) > 5 and cursor() == c, f"yaw {y0} -> {st()['yaw']}, cursor {c} -> {cursor()}")

    press("meta_l", "esc")
    time.sleep(0.5)
    s, c2 = st(), cursor()
    check("16b", "Super+Esc: away to the first monitor, the cursor where it was and the focus there; 3D stays up",
          s["away"] is True and s["mode"] == "active" and abs(c2[0] - home[0]) < 2 and abs(c2[1] - home[1]) < 2 and focused_monitor() == "Virtual-1",
          f"away {s['away']}, {s['mode']}, cursor {c2} (was {home}), focused {focused_monitor()}")
    told = ctl("hypr3d", "log", "30")
    check("16b", "... a notification says how to come back", "move it back onto H3D-2" in told, told.strip().splitlines()[-3:])
    wev_mark("away")
    f0, y1 = s["frames"], s["yaw"]
    rel(30, 20)
    click("left")
    press("a")
    press("esc")
    wheel(1)
    time.sleep(1.0)
    evs, s, c3 = wev_events("away"), st(), cursor()
    pointer = [e for i, e, r in evs if i == "wl_pointer" and e in ("enter", "motion")]
    keys = wev_keys(evs)
    check("16b", "... the mouse moves Hyprland's cursor there, not the camera", c3 != c2 and abs(s["yaw"] - y1) < 0.01, f"cursor {c2} -> {c3}, yaw {y1} -> {s['yaw']}")
    check("16b", "... wev on the first gets the pointer, a click, keys (Esc too) and the wheel",
          pointer and wev_buttons(evs) == [(272, 1), (272, 0)] and (38, 1) in keys and (38, 0) in keys and (9, 1) in keys and (9, 0) in keys and wev_scrolls(evs),
          f"pointer {pointer[-3:]}, buttons {wev_buttons(evs)}, keys {keys}, wheel {wev_scrolls(evs)}")
    check("16b", "... 3D stays up on the second and goes on drawing (Esc was wev's)", s["mode"] == "active" and s["monitor"] == "H3D-2" and s["frames"] > f0 and s["aimed"] is None,
          f"{s['mode']} on {s['monitor']}, frames {f0} -> {s['frames']}, aimed {s['aimed']}")
    n = cursor_pixels(frame("away-first", "Virtual-1"), c3)
    check("16b", "... Hyprland's cursor shows on the first", n > 20, f"{n} cursor pixels")
    ctl("dismissnotify")
    ctl("notify", "1", "8000", "rgb(ff8800)", "hypr3d: on the first monitor, which has the focus")
    time.sleep(1.2)
    n1, n2 = len(notification_boxes(frame("away-notification-first", "Virtual-1"))), len(notification_boxes(frame("away-notification-second", "H3D-2")))
    check("16b", "... a notification shows on the first, not over the 3D view", n1 == 1 and n2 == 0, f"{n1} and {n2}")
    ctl("dismissnotify")

    rel(1500, 0)
    time.sleep(0.5)
    s, c4 = st(), cursor()
    check("16b", "the mouse moved onto the second monitor: back in 3D, the focus there", s["away"] is False and c4[0] >= 1280 and focused_monitor() == "H3D-2",
          f"away {s['away']}, cursor {c4}, focused {focused_monitor()}")
    active = json.loads(ctl("-j", "activewindow") or "{}")
    check("16b", "... wev on the first has lost the keyboard focus (a Super shortcut in 3D can't close it unseen)", not active.get("class"), active.get("class"))
    y2 = s["yaw"]
    rel(200, 0)
    check("16b", "... the mouse turns the camera again, the cursor staying", abs(st()["yaw"] - y2) > 3 and cursor() == c4, f"yaw {y2} -> {st()['yaw']}, cursor {c4} -> {cursor()}")
    ctl("hypr3d", "spawn")
    time.sleep(0.5)
    wev_mark("back")
    feet = st()["feet"]
    press("w", hold=1.2)
    time.sleep(0.3)
    keys, moved = wev_keys(wev_events("back")), math.dist(feet, st()["feet"])
    check("16b", "... W walks again, and isn't wev's", moved > 0.3 and not keys, f"walked {moved:.2f} m, wev keys {keys}")

    press("meta_l", "left")
    time.sleep(0.6)
    s, active = st(), json.loads(ctl("-j", "activewindow"))
    check("16b", "Super+Left (the focus to the left): away to the first monitor, wev focused",
          s["away"] is True and cursor()[0] < 1280 and focused_monitor() == "Virtual-1" and active.get("class") == "wev",
          f"away {s['away']}, cursor {cursor()}, focused {focused_monitor()}, active {active.get('class')}")
    # the second has no window: Hyprland only warps the cursor there, with no motion event; the plugin sees it after the
    # frame
    press("meta_l", "right")
    time.sleep(0.6)
    s = st()
    check("16b", "Super+Right (the focus to the second, which has no window): back in 3D", s["away"] is False and cursor()[0] >= 1280 and focused_monitor() == "H3D-2",
          f"away {s['away']}, cursor {cursor()}, focused {focused_monitor()}")

    r1, s1, c5 = ctl("hypr3d", "away", "on"), st(), cursor()
    r2, s2 = ctl("hypr3d", "away", "off"), st()
    check("16b", "hyprctl hypr3d away on, then off", r1 == "away" and s1["away"] and c5[0] < 1280 and r2 == "in 3D" and not s2["away"] and cursor()[0] >= 1280,
          f"{r1}, away {s1['away']}, cursor {c5}; {r2}, away {s2['away']}, cursor {cursor()}")
    ctl("hypr3d", "away", "on")
    press("meta_l", "esc")
    s = st()
    check("16b", "away, Super+Esc comes back into 3D", s["away"] is False and focused_monitor() == "H3D-2", f"away {s['away']}, focused {focused_monitor()}")
    ctl("hypr3d", "away", "on")
    press("meta_l", "m")
    s, mn = st(), menu()
    check("16b", "away, the Action Menu's keybind (Super+M) comes back into 3D and opens it", s["away"] is False and mn.get("open"), f"away {s['away']}, menu open {mn.get('open')}")
    menu_closed()

    ctl("hypr3d", "away", "on")
    c6 = cursor()
    ctl("hypr3d", "off")
    rel(-40, 0, after=0.1)
    moving, mode = cursor() != c6, st()["mode"]
    wait_for("2D", lambda: st()["mode"] == "off", 15)
    check("16b", "leaving 3D while away: the mouse moves the cursor on the first meanwhile", moving and mode in ("exiting", "off"), f"{c6} -> {cursor()} ({mode})")

    ctl("eval", 'hl.config({ plugin = { hypr3d = { monitor = "" } } })')
    r = ctl("hypr3d", "on", "NOPE-9")
    check("16b", "hyprctl hypr3d on NOPE-9: an error, no 3D", r.startswith("error") and st()["mode"] == "off", r)
    ctl("dispatch", 'hl.dsp.focus({ monitor = "Virtual-1" })')
    r = ctl("hypr3d", "on", "H3D-2")
    ok = wait_for("3D", lambda: st()["mode"] == "active", 15)
    check("16b", "hyprctl hypr3d on H3D-2, from the first: 3D there", r == "ok" and ok and st()["monitor"] == "H3D-2", f"{r}; {st()['monitor']}")
    ensure_3d(False)
    desc = monitors()["H3D-2"].get("description", "")
    if desc:
        ctl("eval", f'hl.config({{ plugin = {{ hypr3d = {{ monitor = {lua_value("desc:" + desc)} }} }} }})')
        ctl("dispatch", 'hl.dsp.focus({ monitor = "Virtual-1" })')
        ensure_3d()
        check("16b", "plugin:hypr3d:monitor = desc: and its description", st()["monitor"] == "H3D-2", f"desc:{desc} -> {st()['monitor']}")
        ensure_3d(False)
    else:
        note("16b", "H3D-2 has no description to go by")
    ctl("eval", 'hl.config({ plugin = { hypr3d = { monitor = "NOPE-9" } } })')
    ctl("dispatch", 'hl.dsp.focus({ monitor = "Virtual-1" })')
    ensure_3d()
    told = ctl("hypr3d", "log", "30")
    check("16b", "plugin:hypr3d:monitor = NOPE-9 (none such): 3D on the focused one, and a notification says so", st()["monitor"] == "Virtual-1" and "no monitor NOPE-9" in told,
          f"{st()['monitor']}; {told.strip().splitlines()[-2:]}")
    ensure_3d(False)

    ctl("eval", 'hl.config({ plugin = { hypr3d = { monitor = "H3D-2" } } })')
    ensure_3d()
    ctl("hypr3d", "away", "on")
    r = ctl("output", "remove", "H3D-2")
    time.sleep(1)
    if not alive():
        check("16b", "the second monitor removed while away from 3D on it: back to 2D, Hyprland fine", False, f"{r}; Hyprland crashed")
        restart_after_crash()
        return
    ok = wait_for("2D", lambda: st()["mode"] == "off", 10)
    c7 = cursor()
    rel(-30, 0)
    check("16b", "the second monitor removed while away from 3D on it: 2D, Hyprland fine, the mouse moving on the first",
          r == "ok" and ok and "H3D-2" not in monitors() and cursor() != c7, f"{r}; {st()['mode']}; cursor {c7} -> {cursor()}")

    ctl("eval", 'hl.config({ plugin = { hypr3d = { monitor = "" } } })')
    ctl("eval", 'hl.unbind("SUPER + Left")')
    ctl("eval", 'hl.unbind("SUPER + Right")')
    machine.execute("pkill -x wev; true")
    alice("setsid -f foot --app-id h3d-left > /dev/null 2>&1; sleep 0.7; setsid -f foot --app-id h3d-right > /dev/null 2>&1")
    wait_for("two terminals", lambda: len(json.loads(ctl("-j", "clients"))) >= 2, 20)
    time.sleep(1.5)



def mean(img, box):
    """mean colour of a box given as frame fractions (x0, y0, x1, y1)"""
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
    # the checks follow litmap.py's header (a correct render from the spawn), with margins for llvmpipe
    ensure_avatar(AV)
    ensure_3d(False)
    r = ctl("eval", f'hl.config({{ plugin = {{ hypr3d = {{ map = "{LIT}" }} }} }})')
    m = wait_for("LitCourt", lambda: (lambda m: m if not m["loading"] and m["world"] == "LitCourt" else None)(ctlj("hypr3d", "map")), 30)
    check("17", "plugin:hypr3d:map (through hyprctl eval): LitCourt loads", m and m["world"] == "LitCourt", f"{r}; {m}")
    journal = machine.execute("journalctl -t start-hyprland --no-pager -n 3000 | grep 'hypr3d.*LitCourt\\|hypr3d.*lighting\\|hypr3d.*backdrop'")[1]
    check("17", "its lighting: 2 lightmap sets, 3 probe volumes, fog, an exposure range; and a backdrop",
          "2 lightmap set(s), 3 light probe volumes, fog, exposure 0.35-0.7" in journal and "backdrop (hypr3d_backdrop)" in journal,
          "; ".join(l.split("[hypr3d] ")[-1] for l in journal.splitlines())[-400:])
    # llvmpipe compiles the map's shaders on the first frames and draws slowly; hyprctl answers between frames
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
    # the sun glints off the pane seen from below: white, not scaled by the pane's 0.2 opacity
    ctl("hypr3d", "tp", "-3.961", "0", "-5.4845", timeout=240)
    ctl("hypr3d", "turn", "35", "50", timeout=240)
    time.sleep(3)
    img = frame("litcourt-glint", timeout=240)
    n = img.count(lambda r, g, b: min(r, g, b) > 245, frac(img, (.42, .40, .58, .60)))
    check("17", "the sun glints off the glass: white where it's reflected", n > 100, f"{n} white pixels")
    # a sun without a baked shadow channel: CS2 shadows it with the realtime shadow alone, so the floor is sunlit and
    # the baked band is gone
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
    ctl("dismissnotify")  # keeps "back to the courtyard" out of later frames


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
    # Hyprland draws its notifications after the plugin's pass, in the same frame
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
    # Hyprland reloads the config after an unload: the plugin's values would be errors
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
    r = ctl("plugin", "load", SO)  # the reload brings the values back
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


@section("8b", "hl.plugin.load in the Lua config, with the plugin's values after it (tiling mode's row staying from the start)")
def s_lua_load():
    cfg = {"avatar": AV, "avatar_physics": False, "tiling_follow": False}
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
    f = ctlj("hypr3d", "tile")["follow"]
    ctl("hypr3d", "tile", "on")
    told = ctl("hypr3d", "log", "6")
    ctl("hypr3d", "tile", "off")
    ctl("hypr3d", "tile", "follow", "on")
    check("8b", "tiling_follow = false: tiling mode's row stays where you turn it on, T says so", f is False and "round you, staying here" in told, f"{f}; {told.strip()[-200:]}")
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
    r1 = ctl("dispatch", "hypr3d:tile")
    on = st()["tiling"]
    r2 = ctl("dispatch", "hypr3d:tile")
    check("8c", "hyprctl dispatch hypr3d:tile: tiling mode on, and again off", r1 == "ok" and r2 == "ok" and on is True and st()["tiling"] is False, f"{r1} {on}, {r2} {st()['tiling']}")
    r1 = ctl("dispatch", "hypr3d:tile", "follow")
    f1 = ctlj("hypr3d", "tile")["follow"]
    r2 = ctl("dispatch", "hypr3d:tile", "follow")
    f2 = ctlj("hypr3d", "tile")["follow"]
    check("8c", "hyprctl dispatch hypr3d:tile follow: tiling mode's row staying where it is (Y), and again going with you", r1 == "ok" and r2 == "ok" and f1 is False and f2 is True,
          f"{r1} {f1}, {r2} {f2}")
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
    """h3dgame as alice, logging what SDL gives it to GAME_LOG; alone: the terminals go first"""
    machine.execute("pkill -x h3dgame; true")
    if alone:
        machine.execute("pkill -u alice foot; true")
    time.sleep(0.8)
    # SDL takes the class from the app id hint on Wayland, WM_CLASS on X11
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
    """turns (hyprctl hypr3d turn) at a few pitches till the crosshair is on that window"""
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
    """while walking, the crosshair onto x, y (panel-local px) of cls's window, steered by hyprctl look scaled on a
    first step"""
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


def play_settled(s):
    """the status once play mode settled (filling the view, or played here)"""
    p = s["playing"]
    return s if p and (p["view"] >= 1 or p.get("fill", True) is False) else None


def play_on(view=None):
    """hyprctl hypr3d play on in that view (here, fill; None: play_view's), till it settled"""
    r = ctl("hypr3d", "play", "on", *([view] if view else []))
    if r != "playing":
        s = st()
        frame("play-on-failed")
        raise RuntimeError(f"hyprctl hypr3d play on: {r}; aimed {s['aimed']}, yaw {s['yaw']}, pitch {s['pitch']}, eye {s['eye']}, view {s['view']}; "
                           f"placed {[(p['class'], p['center'], p['settled']) for p in windows3d()['placed']]}; "
                           f"panels {[(p['kind'], p['class'], p['box'], p['placed']) for p in panels()]}")
    s = wait_for("play mode" + (", the camera facing the window" if view == "fill" else ""), lambda: play_settled(st()), 10, 0.2)
    time.sleep(0.4)
    return r, s


APPS_KILLED = "h3dgame wev xterm swayidle mako obs chromium firefox electron supertux2 chocolate-doom hyprland-share-picker"


def clean_windows():
    """kills every window but the terminals (by PID, then known apps by name)"""
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


@section("18", "play mode: a game gets the keyboard, the buttons, the wheel and the mouse as it wants it; filling the view (Shift+P), the camera faces it")
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
    press("shift", "p")  # Shift+P fills the view; P plays in place
    s = wait_for("play mode", lambda: (lambda s: s if s["playing"] and s["playing"]["view"] >= 1 else None)(st()), 10, 0.2)
    time.sleep(0.5)
    p = s["playing"]
    check("18", "Shift+P: playing it, filling the view, its pointer lock active (the keyboard focus on it)", p and p["class"] == "h3dgame" and p["locked"] and
          p.get("fill") is True and json.loads(ctl("-j", "activewindow")).get("class") == "h3dgame", p)
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
    # R toggles SDL's relative mode in the game
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
    # SDL reads the controller from /dev/input itself, while the game has the keyboard
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
    # the new terminal opened in front of the game: to the wall
    ctl("hypr3d", "window", "h3d-left", "wall")
    wait_for("the terminal on the wall", lambda: not placed("h3d-left"), 10, 0.3)
    ctl("hypr3d", "spawn")
    time.sleep(0.8)
    aim_find("h3dgame")
    play_on()
    ensure_3d(False)
    ensure_3d()
    check("18", "leaving 3D ends play mode", st()["playing"] is None)
    # a screen lock (swaylock, ext-session-lock-v1)
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
    if not unlocked:  # a killed lock client leaves it locked: restart
        machine.execute("pkill -f 'bin/[.]?swaylock'; true")
        restart_after_crash()
        game_start("--relative", alone=False)
    ensure_3d()
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
    # wev prints the xdg_toplevel states
    wev_start()
    ensure_3d()
    menu_closed()
    ctl("hypr3d", "view", "first")
    ctl("hypr3d", "spawn")
    time.sleep(1)
    alice("setsid -f foot --app-id h3d-left > /dev/null 2>&1")
    wait_for("a terminal", lambda: json.loads(ctl("-j", "activewindow")).get("class") == "h3d-left", 20)
    # it opened in front, hiding wev: to the wall
    ctl("hypr3d", "window", "h3d-left", "wall")
    wait_for("the terminal on the wall", lambda: not any(p["class"] == "h3d-left" for p in windows3d()["placed"]), 10, 0.3)
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
    # Hyprland reports "discarded" for what the 3D view covers; drawn in 3D, a window must get "presented"
    alice("rm -f /tmp/pres.log /tmp/pres-debug.log; WAYLAND_DEBUG=client setsid -f stdbuf -oL weston-presentation-shm -f > /tmp/pres.log 2> /tmp/pres-debug.log")
    wait_for("weston-presentation-shm", lambda: json.loads(ctl("-j", "clients")), 20)
    time.sleep(1)
    ctl("hypr3d", "spawn")
    time.sleep(3)
    machine.execute("pkill -INT -f weston-presentation-shm; true")
    time.sleep(1)
    out = machine.execute("cat /tmp/pres.log")[1]
    (LOGS / "presentation-shm.txt").write_text(out)
    # a line per presented frame ("N: f2c .. ms, ..."), "discarded" per other
    presented = len(re.findall(r"^\s*\d+: f2c", out, re.M))
    discarded = len(re.findall(r"discarded", out, re.I))
    note("18b", "weston-presentation-shm in 3D", " | ".join(out.strip().splitlines()[-6:])[:600])
    check("18b", "weston-presentation-shm in 3D: its frames are presented, none discarded", presented > 0 and discarded == 0, f"presented {presented}, discarded {discarded}")
    # its WAYLAND_DEBUG log: the output entered and each feedback
    dbg = machine.execute("cat /tmp/pres-debug.log")[1]
    # libwayland 1.23+ logs interface#id, older ones interface@id
    enters = re.findall(r"wl_surface[@#]\d+\.enter\(wl_output[@#]\d+\)", dbg)
    scale = re.findall(r"wl_surface[@#]\d+\.preferred_buffer_scale\((\d+)\)", dbg)
    fb = (len(re.findall(r"wp_presentation_feedback[@#]\d+\.presented\(", dbg)), len(re.findall(r"wp_presentation_feedback[@#]\d+\.discarded\(", dbg)))
    (LOGS / "presentation-shm-debug.txt").write_text(dbg[-20000:])
    check("18b", "... in its protocol log: wl_surface.enter for the monitor's wl_output, its scale, and the feedback presented", enters and fb[0] > 0 and fb[1] == 0,
          f"enter {enters[:2]}, preferred scale {scale[:2]}, presented/discarded {fb}")
    machine.execute("pkill -x weston-presentation-shm; true")
    # SDL inhibits idle by default
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
    """XWayland's display (Hyprland starts it when it finds Xwayland)"""
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
    """play mode: the pointer to x, y over the played window (window-local px) in counts, pixels with x11_checks' flat
    accel"""
    s = st()
    if not s["playing"]:
        raise RuntimeError(f"not playing (moving the pointer to {x:.0f}, {y:.0f}); aimed {s['aimed']}")
    p = s["playing"]["pointer"]
    rel(round(x - p[0]), round(y - p[1]), after=0.4)
    return st()["playing"]["pointer"]


def x11_checks(item, scale=1):
    if not HYPR["pid"]:  # run alone in the hidpi VM
        start_hyprland("hyprland.lua", lua_config(scale=scale))
        ensure_plugin()
    ensure_avatar(AV)
    ensure_3d(False)
    clean_windows()
    ctl("eval", 'hl.config({ input = { accel_profile = "flat" } })')  # a count is a pixel
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
    # the canvas starts 180 px down in X11 window coordinates, under the entry
    tk_mark("click")
    at = pointer_to(w * 0.6, h * 0.6)
    click("left")
    at2 = pointer_to(w * 0.6 + 60, h * 0.6 + 30)
    click("left")
    got = [tuple(int(v) for v in l.split()[1:3]) for l in tk_lines("click") if l.startswith("click ")]
    # X11 coordinates are logical: xwayland:force_zero_scaling is off
    check(item, "clicks on its canvas land where the pointer is: 60 and 30 px apart, as sent", len(got) == 2 and abs((got[1][0] - got[0][0]) - 60) <= 2 and
          abs((got[1][1] - got[0][1]) - 30) <= 2, f"{got}; pointer {at} -> {at2}")
    pointer_to(60, h * 0.6)  # off the canvas, then back in
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
    # the File menu: an override-redirect window, shown as a popup
    tk_mark("menu")
    pointer_to(18, 10)
    click("left")
    time.sleep(0.8)
    pops = [p for p in panels() if p["kind"] == "popup" and p["class"] == c["class"]]
    frame(f"{item}-x11-menu")
    check(item, "File in the menu bar: the menu opens, a popup of the window in 3D", pops, [p["box"] for p in pops])
    if pops:
        m = max(pops, key=lambda p: p["box"][3])  # the menu, not a tooltip
        box = m["box"]
        win = next(p for p in panels() if p["kind"] == "window" and p["class"] == c["class"])["box"]
        # "Open", 2nd of 4 items: 3/8 down
        pointer_to(box[0] - win[0] + box[2] / 2, box[1] - win[1] + box[3] * 3 / 8)
        click("left")
        time.sleep(0.5)
        check(item, "... and clicking its second item picks Open", "menu Open" in tk_lines("menu"), tk_lines("menu"))
    # the entry at the top
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
    try:  # slow at scale 2 on llvmpipe
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
    # SDL's x11 driver in relative mode, as most Steam and Proton games run: played in 3D it must get what it gets in 2D
    machine.execute("pkill -f tkapp[.]py; true")
    ensure_3d(False)
    g = game_start("--relative", env=f"DISPLAY={x_display()} SDL_VIDEODRIVER=x11", cls="h3dgame-x11")
    check(item, "h3dgame through XWayland (SDL's x11 driver), relative mode", g and g["xwayland"] and "driver x11" in game_lines(), game_lines()[:4])

    def moves(mark):
        game_mark(mark)
        rel(100, 0)
        rel(0, 40)
        rel(1500, 0)  # a sweep, as a quick turn
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
FAKE_STEAM = "/tmp/h3d-steam"
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


def client(cls):
    return next((c for c in json.loads(ctl("-j", "clients")) if c["class"] == cls), None)


def logical_size():
    """the focused monitor's size, logical px"""
    m = next(m for m in json.loads(ctl("-j", "monitors")) if m["focused"])
    return m["width"] / m["scale"], m["height"] / m["scale"]


FRONT_FIT = 0.85  # as in main.cpp: most of the view a new window fills
THIRD_FIT = 0.6  # the same in third person
WHEEL_SIZE = 0.95  # as in main.cpp: size factor per wheel notch, carrying
HALF_FOV = math.radians(35)


def on_screen(cls):
    """how big a new cls window with no rule height should look (1 = as on screen), fit into FRONT_FIT of the view"""
    (w, h), (mw, mh) = client(cls)["size"], logical_size()
    return min(1.0, FRONT_FIT * mh / h, FRONT_FIT * mw / w)


def looks(p, s, cls):
    """how big a placed window looks from the eye, looking level (worked out apart from the plugin's "apparent" field)"""
    h, (mw, mh) = client(cls)["size"][1], logical_size()
    return (p["height"] / h) / (2 * relative(p, s)[0] * math.tan(HALF_FOV) / mh)


def side_shown(p, cls, dist, side):
    """where to the side a window with no rule height opens: the rule's side, nearer the middle if it wouldn't show
    whole"""
    (w, h), (mw, mh) = client(cls)["size"], logical_size()
    room = FRONT_FIT * dist * math.tan(HALF_FOV) * mw / mh - p["height"] * w / h / 2
    return math.copysign(min(abs(side), max(0.0, room)), side)


def off_middle(p, s):
    """degrees between where you look and a placed window's middle"""
    yaw, pitch = math.radians(s["yaw"]), math.radians(s["pitch"])
    f = (math.sin(yaw) * math.cos(pitch), math.sin(pitch), -math.cos(yaw) * math.cos(pitch))
    d = [p["center"][i] - s["eye"][i] for i in range(3)]
    return math.degrees(math.acos(max(-1.0, min(1.0, sum(a * b for a, b in zip(f, d)) / math.hypot(*d)))))


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
    # desktop entries (foot's icon, an SVG in hicolor) and favourites
    desktop_entry("h3d-test-game", f"Name=Test Game\nComment=Game\nExec=h3dgame --title h3dgame --log {GAME_LOG} %U\nIcon=foot\nStartupWMClass=h3dgame")
    desktop_entry("h3d-chat", "Name=Chat\nExec=foot --app-id discord\nIcon=foot")
    desktop_entry("h3d-hidden", "Name=Hidden\nExec=true\nNoDisplay=true")
    # a Steam shortcut ("steam steam://rungameid/ID"); the stand-in steam has systemd start the game with SteamAppId, as
    # Steam does
    alice(f"printf '%s\\n' '#!/bin/sh' 'id=${{1##*/}}' 'exec systemd-run --user --quiet --setenv=SteamAppId=$id --setenv=SteamGameId=$id "
          f"--setenv=SDL_APP_ID=steam_app_$id h3dgame --title steamentry' > {FAKE_STEAM} && chmod +x {FAKE_STEAM}")
    desktop_entry("h3d-steam-game", f"Name=Steam Test Game\nComment=Play this game on Steam\nExec={FAKE_STEAM} steam://rungameid/43\nIcon=foot\nTerminal=false\nCategories=Game;")
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
    time.sleep(1.5)  # icons load a few a frame
    m = menu()
    labels = [i["label"] for i in m.get("items", [])]
    check("20", "Q opens the Action Menu's Apps page: the favourites (an entry by id, by name, a command) and All apps",
          m.get("path", "").endswith("apps") and labels[:4] == ["Test Game", "Chat", "foot", "All apps"], f"{m.get('path')}: {labels}")
    check("20", "... with the apps' icons as pictures", [i["picture"] for i in m["items"][:2]] == [True, True], [i["picture"] for i in m.get("items", [])])
    frame("apps-page")
    s = st()
    press("1")
    g = wait_for("the game in the world", lambda: placed("h3dgame"), 30, 0.5)
    ahead, right, up = relative(g, s)
    want, seen = on_screen("h3dgame"), looks(g, s, "h3dgame")
    check("20", "picked: it starts, and its window opens in front of you, not on the wall: 2 m ahead (a game), as big as on the screen (as fits the view)",
          g and abs(ahead - 2.0) < 0.1 and abs(right) < 0.1 and abs(g["apparent"] - want) < 0.02 and abs(seen - want) < 0.02 and not menu().get("open"),
          f"ahead {ahead:.2f}, right {right:.2f}, up {up:.2f}, {g['height']:.2f} m tall; looks {g['apparent']:.3f} ({seen:.3f} worked out here), {want:.3f} wanted; {client('h3dgame')['size']}")
    frame("launched-game")
    # chat apps go left, nearer the middle if too wide to show whole there
    r = ctl("hypr3d", "launch", "Chat")
    c = wait_for("the chat app in the world", lambda: placed("discord"), 30, 0.5)
    s = st()
    ahead, right, up = relative(c, s)
    want, side = on_screen("discord"), side_shown(c, "discord", 1.3, -1.0)
    check("20", "hyprctl hypr3d launch Chat: a chat app (class discord) opens at your left, 1.3 m out, as big as on the screen",
          r.startswith("launched") and c and abs(ahead - 1.3) < 0.1 and right < -0.1 and abs(right - side) < 0.05 and abs(c["apparent"] - want) < 0.02,
          f"{r}; ahead {ahead:.2f}, right {right:.2f} ({side:.2f} wanted), {c['height']:.2f} m tall, looks {c['apparent']:.3f}, {want:.3f} wanted; {client('discord')['size']}")
    # a game Steam starts isn't the launched process's: it's known by its class steam_app_ID
    r = ctl("hypr3d", "launch", "systemd-run --user --setenv=SDL_APP_ID=steam_app_42 h3dgame --title steamfake # steam://rungameid/42")
    g = wait_for("the 'Steam' game in the world", lambda: placed("steam_app_42"), 30, 0.5)
    ahead, first = relative(g, st())[0], relative(placed("h3dgame"), st())[0]
    check("20", "launched as a Steam game (steam://rungameid/42) but started by another process: known by its class steam_app_42, in front of you as a game is (10 cm in front of the first game, where it would go)",
          g and abs(first - 2.0) < 0.05 and abs(ahead - 1.9) < 0.02 and abs(g["apparent"] - on_screen("steam_app_42")) < 0.02,
          f"{r}; ahead {ahead:.2f} (the first game {first:.2f}); {g}; {client('steam_app_42')['size']}")
    ctl("hypr3d", "window", "steam_app_42", "close")
    # a Steam shortcut by name: the URL is in its command, and the window is tied to the launch by SteamAppId, not by
    # just opening on the 3D monitor
    wait_for("the 'Steam' game gone", lambda: not placed("steam_app_42"), 10, 0.2)
    r = ctl("hypr3d", "launch", "Steam Test Game")
    g = wait_for("the Steam shortcut's game in the world", lambda: placed("steam_app_43"), 30, 0.5)
    ahead = relative(g, st())[0] if g else 0
    said = [l for l in ctl("hypr3d", "log", "40").splitlines() if "opened a window (steam_app_43)" in l]
    check("20", "a Steam shortcut (a desktop entry running \"steam steam://rungameid/43\") launched by name: its game, started by Steam's own process, is the launch's window, in front of you as a game is",
          r.startswith("launched") and g and any("Steam Test Game opened a window" in l for l in said) and abs(ahead - 1.9) < 0.02,
          f"{r}; said {said}; ahead {ahead:.2f}; {g}")
    ctl("hypr3d", "window", "steam_app_43", "close")
    ctl("eval", 'hl.config({ plugin = { hypr3d = { app_rules = "h3d-raw: 1.0 0.5 right, h3d-auto: 1.2 auto left" } } })')
    time.sleep(1.5)
    r = ctl("hypr3d", "launch", "foot --app-id h3d-raw")
    f = wait_for("the command's window", lambda: placed("h3d-raw"), 30, 0.5)
    ahead, right, up = relative(f, st())
    check("20", "launch a command, with app_rules \"h3d-raw: 1.0 0.5 right\": 1 m out, 0.5 m tall, to the right",
          f and right > 0.8 and abs(f["height"] - 0.5) < 0.05, f"{r}; ahead {ahead:.2f}, right {right:.2f}, {f['height']:.2f} m")
    r = ctl("hypr3d", "launch", "foot --app-id h3d-auto")
    f = wait_for("the other command's window", lambda: placed("h3d-auto"), 30, 0.5)
    ahead, right, up = relative(f, st())
    want, side = on_screen("h3d-auto"), side_shown(f, "h3d-auto", 1.2, -1.0)
    check("20", "... and with \"h3d-auto: 1.2 auto left\": 1.2 m out, to the left, as big as on the screen",
          f and abs(ahead - 1.2) < 0.1 and right < -0.1 and abs(right - side) < 0.05 and abs(f["apparent"] - want) < 0.02,
          f"{r}; ahead {ahead:.2f}, right {right:.2f} ({side:.2f} wanted), looks {f['apparent']:.3f}, {want:.3f} wanted; {client('h3d-auto')['size']}")
    ctl("hypr3d", "window", "h3d-auto", "close")
    wait_for("it closed", lambda: not placed("h3d-auto"), 10, 0.3)
    ctl("hypr3d", "turn", "-40", "0")
    time.sleep(0.5)
    s = st()
    alice("setsid -f foot --app-id h3d-other > /dev/null 2>&1")
    o = wait_for("the terminal in the world", lambda: placed("h3d-other"), 20, 0.5)
    ahead, right, up = relative(o, s)
    want = on_screen("h3d-other")
    check("20", "a terminal opening in 3D, not launched from it (as from a keybind): in front of you, 1.5 m out, as big as on the screen",
          o and abs(ahead - 1.5) < 0.1 and abs(right) < 0.1 and abs(o["apparent"] - want) < 0.02, f"ahead {ahead:.2f}, right {right:.2f}, looks {o['apparent']:.3f}, {want:.3f} wanted; {client('h3d-other')['size']}")
    frame("opened-in-3d")
    alice("setsid -f foot --app-id h3d-other2 > /dev/null 2>&1")
    o2 = wait_for("the second terminal in the world", lambda: placed("h3d-other2"), 20, 0.5)
    o = placed("h3d-other")
    ahead2, right2, up2 = relative(o2, s)
    want = on_screen("h3d-other2")
    check("20", "... and another right after it: in front of it, 10 cm nearer, as big as on the screen",
          o2 and abs(ahead2 - (relative(o, s)[0] - 0.1)) < 0.02 and abs(right2) < 0.1 and abs(o2["apparent"] - want) < 0.02,
          f"ahead {ahead2:.2f} (the other {relative(o, s)[0]:.2f}), right {right2:.2f}, looks {o2['apparent']:.3f}, {want:.3f} wanted")
    for cls in ("h3d-other", "h3d-other2"):
        ctl("hypr3d", "window", cls, "close")
        wait_for("it closed", lambda: not placed(cls), 10, 0.3)
    ctl("hypr3d", "turn", "0", "0")
    time.sleep(0.5)
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
    # a remembered place behind you is skipped: out there it would look like it never opened
    ctl("hypr3d", "window", "discord", "close")
    wait_for("it closed", lambda: not placed("discord"), 10, 0.3)
    yaw0 = st()["yaw"]
    ctl("hypr3d", "turn", f"{yaw0 + 180:.1f}", "0")
    time.sleep(0.5)
    s = st()
    ctl("hypr3d", "launch", "Chat")
    c = wait_for("the chat app again", lambda: placed("discord"), 30, 0.5)
    ahead, right, up = relative(c, s)
    told = ctl("hypr3d", "log", "20")
    check("20", "launched with its place behind you: in front of you (at the left), not there; its place kept",
          0.3 < ahead < 1.4 and right < 0 and math.dist(c["center"], spot) > 1 and "discord" in machine.execute(f"cat {SPOTS}")[1]
          and "discord: not to its place (out of sight)" in told, f"ahead {ahead:.2f}, right {right:.2f}; {spot} -> {c['center']}; {told.strip()[-200:]}")
    ctl("hypr3d", "turn", f"{yaw0:.1f}", "0")
    time.sleep(0.5)
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
    check("20", "... a window's page: focus, bring here, to the wall, pin, bigger, smaller, play, close (asking first)",
          [i["label"] for i in m.get("items", [])] == ["Focus", "Bring here", "To the wall", "Pin to view", "Bigger", "Smaller", "Play", "Close…"], [i["label"] for i in m.get("items", [])])
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
    check("20", "carrying it, Shift+wheel two notches down: its real size, 0.95 of it a notch", abs(cl["size"][0] - size1[0] * WHEEL_SIZE**2) <= 3, f"{size1} -> {cl['size']}")
    ctl("eval", 'hl.config({ plugin = { hypr3d = { play_view = "fill" } } })')
    try:
        aim_find("h3dgame")
        press("e")
        press("f")
        s = wait_for("played, fullscreen", lambda: (lambda s: s if s["playing"] else None)(st()), 10, 0.3)
        time.sleep(0.8)
        img = frame("fullscreen-played")
        fill = img.count(game_blue, step=4) / ((img.w // 4) * (img.h // 4))
        check("20", "the app goes fullscreen (F in it): it's played, filling the view (play_view = fill)", s["playing"]["class"] == "h3dgame" and fill > 0.6, f"{s['playing']}, {fill:.0%}")
        press("f")
        try:
            out = wait_for("out of fullscreen", lambda: (lambda c: c if c and c["fullscreen"] == 0 else None)(client("h3dgame")), 10, 0.3)
        except TimeoutError:
            out = None
        time.sleep(0.8)
        s = st()
        check("20", "... and out of fullscreen (F again): still played, the keys the app's (as on the 2D desktop)", out and (s["playing"] or {}).get("class") == "h3dgame" and s["typing"],
              f"fullscreen {(client('h3dgame') or {}).get('fullscreen')}; playing {s['playing']}, typing {s['typing']}")
        press("meta_l", "esc")
        s = wait_for("walking", lambda: (lambda s: s if s["playing"] is None else None)(st()), 10, 0.3)
        check("20", "... Super+Esc: walking again", s["playing"] is None and s["typing"] is False, s["typing"])
    finally:  # later sections expect the default play_view
        ctl("eval", 'hl.config({ plugin = { hypr3d = { play_view = "here" } } })')
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


PAGE_NOT = set()  # windows open before the page's app started


def page_window():
    """the page.html window: its title starts "h3d[H]: ", H the page's height"""
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
    """play mode drag by dx, dy; it waits and nudges before letting go, as a browser starts a drag late and drops where
    the pointer last moved"""
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
    """desktop boxes of every surface drawn in 3D for cls (Firefox's <select> lists and tooltips are subsurfaces, not
    popups)"""
    return {tuple(b) for p in panels() if p["class"] == cls for b in p.get("surfaces", [])}


def local_mid(cls, box):
    """a desktop box's middle in cls's window-local px, as play mode's pointer takes it"""
    w = next(p for p in panels() if p["kind"] == "window" and p["class"] == cls)
    return box[0] - w["box"][0] + box[2] / 2, box[1] - w["box"][1] + box[3] / 2


def page_dnd_2d(cls):
    """the same drag and drop on the 2D desktop, with the tablet"""
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
    """page.html in `launch` (a browser or Electron), launched from 3D and played"""
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
    ui = max(0, c["size"][1] - inner)  # below the browser's bars

    def top():
        """where the page starts now, under the bars (an info bar comes and goes)"""
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
    before = drawn(cls)
    page_to(340, 96)
    time.sleep(2.5)
    new = shown(before)
    frame(f"{item}-tooltip")
    check(item, "a title's tooltip shows, drawn over the window", new, new)
    # Electron draws its own context menu, browsers a native one
    page_to(200, 36)
    click("right")
    time.sleep(1.2)
    pops = popups_of(cls)
    frame(f"{item}-context-menu")
    t = page_title()
    check(item, "a right click: the page gets it, and the context menu opens, a popup", "context menu" in t and pops, f"{t}; {[p['box'] for p in pops]}")
    if pops and log and "Electron" in name:
        pointer_to(*local_mid(cls, pops[-1]["box"]))  # its middle item
        click("left")
        time.sleep(0.8)
        out = machine.execute(f"cat {log}")[1]
        check(item, "... clicking its middle item picks it (Paste)", "menu Paste" in out, [l for l in out.splitlines() if l.startswith("menu")])
    else:
        press("esc")
    time.sleep(0.6)
    page_to(22, 36)
    drag_by(240, 0)
    press("ctrl", "c")
    t = page_said("copied")
    paste = as_alice("wl-paste -n", 15)[1]
    check(item, "text selected by dragging over it, Ctrl+C: on the clipboard (wl-paste has it)", "copied The quick" in t and paste.startswith("The quick"), f"{t}; wl-paste {paste[:40]!r}")
    page_to(170, 155)
    click("left")
    type_text("hello")
    t = page_said("typed hello")
    check(item, "a click in its input, typing", "typed hello" in t, t)
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
        # page text dragged with the crosshair, the button held, stepping aside (the browser hides the wall) onto wev,
        # which the drag must enter
        page_to(22, 36)
        drag_by(120, 0)
        ctl("hypr3d", "play", "off")
        time.sleep(1)
        a = aim_local(cls, 60, top() + 36)
        wev_mark("dnd")
        qmp([{"type": "btn", "data": {"down": True, "button": "left"}}])
        time.sleep(0.3)
        ctl("hypr3d", "look", "40", "0")  # moving with the button down starts the drag
        time.sleep(0.6)
        wev_x = next((q["box"][0] for q in panels() if q["kind"] == "window" and q["class"] == "wev"), 0)
        ctl("hypr3d", "walk", "0.85", "left" if wev_x < logical_size()[0] / 2 else "right")  # towards wev, about 1.3 m
        walked_off()
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
        ctl("hypr3d", "spawn")
        time.sleep(0.8)
        aim_find(cls)
        play_on()
    page_to(420, 290)
    touchpad(600)
    t = page_said("scrolled")
    check(item, "two fingers on a touchpad scroll it", "scrolled" in t and not t.endswith("scrolled 0"), t)
    # switched to fill while playing (played here it would stay where it is)
    ctl("hypr3d", "play", "fill")
    wait_for("the camera facing it", lambda: (lambda s: s if s["playing"] and s["playing"]["view"] >= 1 else None)(st()), 10, 0.2)
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
    ctl("hypr3d", "play", "here")
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
    # last: a pinch zooms the page, moving everything
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
    wev_start()  # on the wall: the cross-window drag's target
    try:
        page_checks("21", f"chromium --no-first-run --no-default-browser-check --password-store=basic --user-data-dir=/tmp/h3d-chromium file://{PAGE} "
                    "> /tmp/chromium.log 2>&1", "Chromium", log="/tmp/chromium.log", cross=True)
    except Exception as e:  # noqa: BLE001 (Firefox still gets its turn)
        check("21", f"(Chromium: stopped)", False, f"{type(e).__name__}: {e}"[:500])
    machine.execute("pkill -f chromium; true")
    gone("chrom")
    # Firefox: the portal's file chooser, no first-run pages
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
    # a fresh Hyprland: 0.55.2's XWM can keep a destroyed X window's record (section 19 leaves one), and a new window
    # reusing its id (Electron's tooltip) is mapped as the old one, a managed window, not a tooltip
    start_hyprland("hyprland.lua", lua_config())
    ensure_plugin()
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
    machine.execute("pkill -f 'bin/[.]?mako'; true")  # .mako-wrapped: -x mako misses it
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
    # fcitx5 (zwp_input_method_v2): Ctrl+; lists clipboard entries as candidates in its popup
    # (zwp_input_popup_surface_v2)
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
    """an obs-websocket request through obsws.py: the response data, or raises"""
    cmd = f"python3 {H}/obsws.py {request} {shlex.quote(json.dumps(data or {}))}" + (f" --save {save}" if save else "")
    status, out = as_alice(cmd, 60)
    if status != 0:
        raise RuntimeError(f"obsws {request}: {out.strip()[-300:]}")
    d = json.loads(out.strip().splitlines()[-1])
    if not d.get("requestStatus", {}).get("result"):
        raise RuntimeError(f"obsws {request}: {d.get('requestStatus')}")
    return d.get("responseData", {})


def bmp(path):
    """a 24- or 32-bit BMP (OBS's screenshot) as an Img"""
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
    # a PipeWire capture created through obs-websocket stalls at the portal: it's made off OBS's UI thread, whose GLib
    # loop would take the answer. Saved in the scene collection and OBS launched again, it's made on that thread and the
    # picker opens
    obs("CreateInput", {"sceneName": scene, "inputName": "h3d screen", "inputKind": "pipewire-screen-capture-source", "inputSettings": {}})
    ctl("hypr3d", "window", c["address"], "close")
    try:
        wait_for("OBS to quit", lambda: machine.execute("pgrep -f '[.]obs-wrapped'")[0] != 0, 30, 0.5)
    except TimeoutError:
        machine.execute("pkill -f '[.]obs-wrapped'; true")
        time.sleep(2)
    before = {x["address"] for x in json.loads(ctl("-j", "clients"))}
    # the portal's D-Bus traffic, for a missing picker
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
    # played by address: the picker's class is empty
    r = ctl("hypr3d", "window", pk["address"], "play")
    wait_for("play mode", lambda: play_settled(st()), 10, 0.2)
    # the first button of its Screen tab
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
    # 2D first: how far the mouse turns Doom there
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
        rel(1500, 0)  # about a quarter turn
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
    r = ctl("hypr3d", "launch", "supertux2 > /tmp/supertux.log 2>&1")
    c = wait_for("SuperTux's window", lambda: next((c for c in json.loads(ctl("-j", "clients")) if "supertux" in c["class"].lower()), None), 90, 1)
    time.sleep(8)  # its title screen
    aim_find(c["class"])
    play_on()
    time.sleep(1)
    # its first start asks Yes or No to going online; left and right switch
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
    """the 3D view's fps, the plugin's ms a frame and the game's fps, after `secs`"""
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
    play_on("fill")
    play = frame_rates("playing")
    note("24", "... the game played (placed in the world, the camera facing it: Shift+P)", play)
    ctl("hypr3d", "play", "off")
    ctl("hypr3d", "window", "wev", "pin")
    time.sleep(1)
    pin = frame_rates("pinned")
    note("24", "... and wev pinned to the view", pin)
    check("24", "the game keeps drawing behind the 3D view, at its frame rate (frame callbacks, presentation, FIFO at the 3D view's pace)",
          all(r["game"] and min(r["game"]) > 0 and abs(r["game"][-1] - r["fps"]) < max(4, 0.35 * r["fps"]) for r in (walk, play)),
          {"walking": walk, "playing": play})
    # direct scanout (render:direct_scanout = 1) on the 2D desktop vs played in 3D, where hypr3d blocks it
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
    play_on("fill")
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


def shell_overlay(on=True):
    """quickshell's full-screen see-through overlay (overlay.qml: input only in a band down the middle)"""
    machine.execute("pkill -f 'quickshell -p'; true")
    if on:
        alice(f"setsid -f quickshell -p {H}/overlay.qml > /tmp/quickshell.log 2>&1")
        wait_for("the overlay", lambda: "h3d-overlay" in ctl("-j", "layers"), 30)
        time.sleep(0.5)


@section("25", "a shell's see-through overlay over the whole screen (quickshell): the crosshair and clicks go through it where it takes no input, and hit it where it does")
def s_overlay():
    lua_session()
    ensure_plugin()
    ensure_3d(False)
    shell_overlay()
    ensure_3d()
    menu_closed()
    ctl("hypr3d", "view", "first")
    ctl("hypr3d", "spawn")
    time.sleep(1.0)
    s = st()
    check("25", "the crosshair on the overlay's band (it takes input there): the overlay", (s.get("aimed") or {}).get("kind") == "layer", s.get("aimed"))
    r = ctl("hypr3d", "aim", "h3d-left")
    time.sleep(0.8)
    s = st()
    aimed = s.get("aimed") or {}
    check("25", "on the terminal behind its see-through part: the terminal, not the overlay", aimed.get("kind") == "window" and aimed.get("class") == "h3d-left", f"{r}; {aimed}")
    click("left")
    time.sleep(0.5)
    active = json.loads(ctl("-j", "activewindow") or "{}").get("class")
    check("25", "... a click there focuses the terminal", active == "h3d-left", active)
    frame("overlay-3d")
    ensure_3d(False)
    shell_overlay(False)


@section("25h", "H and Shift+H, the keys (with a shell's overlay over the desktop, as on a user's): H picks a window up and puts it down where you point, as G does, by a wall too; Shift+H pins one to the view and takes it back into your hands; the one carried, two pinned, the Action Menu open, hyprctl hypr3d grab and pin")
def s_pin_key():
    lua_session()
    ensure_plugin()
    ensure_3d(False)
    clean_windows()
    for cls in ("h3d-left", "h3d-right"):  # section 20 leaves only the left one
        if not any(c["class"] == cls for c in json.loads(ctl("-j", "clients"))):
            alice(f"setsid -f foot --app-id {cls} > /dev/null 2>&1")
            wait_for(cls, lambda: any(c["class"] == cls for c in json.loads(ctl("-j", "clients"))), 20)
    shell_overlay()
    ensure_3d()
    menu_closed()
    ctl("hypr3d", "reset-windows", "forget")
    ctl("hypr3d", "view", "first")
    ctl("hypr3d", "spawn")
    time.sleep(1.0)

    def pins():
        return {p["class"]: p["pinned"] for p in windows3d()["placed"]}

    def held():
        return next((p["class"] for p in windows3d()["placed"] if p["held"]), None)

    def errors(text):
        return ctl("hypr3d", "log").count(text)

    def fresh():
        ctl("hypr3d", "reset-windows", "forget")
        ctl("hypr3d", "spawn")
        time.sleep(1.0)

    a = aim_find("h3d-left")
    press("h")
    s, g = st(), placed("h3d-left")
    check("25h", "H on a terminal on the wall, through the overlay's see-through part: picked up, as G does",
          a and g and g["held"] and not g["pinned"] and s["holding"] is True, f"aimed {a}; {g}; holding {s['holding']}")
    ctl("hypr3d", "walk", "1.5", "back")  # about 2 m
    rel(250, 0)
    walked_off()
    w1 = settled()
    s1, g1 = st(), placed("h3d-left")
    moved = math.dist(s1["eye"], s["eye"])
    check("25h", "walked back and turned: it goes along, its middle where you look",
          g1 and g1["held"] and moved > 1 and off_middle(g1, s1) < 1.5 and math.dist(g1["center"], g["center"]) > 0.5,
          f"walked {moved:.2f} m; {off_middle(g1, s1):.2f} degrees off the middle; {g['center']} -> {g1['center']}; {w1['hold']}")
    frame("h-carried")
    press("h")
    time.sleep(0.3)
    s2, g2 = st(), placed("h3d-left")
    check("25h", "H again: put down there, where you point, and carried no more",
          g2 and not g2["held"] and not g2["pinned"] and s2["holding"] is False and off_middle(g2, s2) < 1.5 and math.dist(g2["center"], g1["center"]) < 0.02,
          f"{g1['center']} -> {g2 and g2['center']}; {off_middle(g2, s2):.2f} degrees off the middle")
    ctl("hypr3d", "walk", "0.5", "left")  # stays on its front side
    rel(-400, 0)
    walked_off()
    g3 = placed("h3d-left")
    check("25h", "... and it stays there as you walk away and look around", g3 and not g3["held"] and math.dist(g3["center"], g2["center"]) < 0.02,
          f"{g2['center']} -> {g3 and g3['center']}")
    ctl("hypr3d", "aim", "h3d-left")
    time.sleep(0.8)
    frame("h-put-down")
    a = st()["aimed"]
    press("g")
    s4 = st()
    press("h")
    check("25h", "G picks it up and H puts it down (one carrying, whichever key)", a and a["class"] == "h3d-left" and s4["holding"] is True and st()["holding"] is False,
          f"aimed {a}; holding {s4['holding']} -> {st()['holding']}")

    # the south gate's doors (face at z 21.88) 0.6 m ahead, nearer than it's held
    fresh()
    a = aim_find("h3d-left")
    press("h")
    ctl("hypr3d", "tp", "0", "0", "21.3")
    ctl("hypr3d", "turn", "180", "0")
    time.sleep(1.0)
    settled()
    press("h")
    time.sleep(0.5)
    s, g = st(), placed("h3d-left")
    check("25h", "carried to the gate, the doors 0.6 m ahead: H puts it flat on them, where you point",
          a and g and not g["held"] and 21.8 < g["center"][2] < 21.88 and off_middle(g, s) < 3 and abs(g["distance"] - 0.56) < 0.1,
          f"{g and g['center']}, {g and g['distance']} m from the eye, {off_middle(g, s):.2f} degrees off the middle")
    frame("h-on-the-doors")

    fresh()
    ctl("hypr3d", "turn", "0", "70")
    time.sleep(0.8)
    n = errors("point the crosshair at a window to pick it up")
    press("h")
    check("25h", "H at the sky: nothing picked up, and a notification says to point at a window",
          st()["holding"] is False and not windows3d()["placed"] and errors("point the crosshair at a window to pick it up") == n + 1, f"{windows3d()}; aimed {st()['aimed']}")

    ctl("hypr3d", "turn", "0", "0")
    a = aim_find("h3d-right")
    press("shift", "h")
    s, g = st(), placed("h3d-right")
    ahead, right, up = relative(g, s) if g else (0, 0, 0)
    pan = next((p for p in panels() if p["kind"] == "window" and p["class"] == "h3d-right"), {})
    check("25h", "Shift+H on a terminal on the wall: pinned to the view's top right corner, over the world",
          a and g and g["pinned"] and not g["held"] and right > 0.2 and up > 0.05 and pan.get("front"), f"aimed {a}; {g}; ahead {ahead:.2f} right {right:.2f} up {up:.2f}")
    frame("shift-h-pinned")
    ctl("hypr3d", "walk", "0.6", "back")
    time.sleep(1.2)
    press("shift", "h")
    w, s = windows3d(), st()
    g = placed("h3d-right")
    check("25h", "walked back, Shift+H again: out of the corner into your hands, as big as it was on the wall, as far off as it was",
          g and g["held"] and not g["pinned"] and s["holding"] is True and w["hold"] and abs(w["hold"]["size"] - 1) < 0.02 and 3.5 < w["hold"]["dist"] < 5,
          f"{g}; hold {w['hold']}")
    settled()
    s1, g1 = st(), placed("h3d-right")
    press("h")
    time.sleep(0.3)
    g2 = placed("h3d-right")
    check("25h", "... and H puts it down where you point", g2 and not g2["held"] and not g2["pinned"] and off_middle(g1, s1) < 1.5 and math.dist(g2["center"], g1["center"]) < 0.02,
          f"{off_middle(g1, s1):.2f} degrees off the middle; {g1['center']} -> {g2 and g2['center']}")
    press("shift", "h")  # under the crosshair: pinned again
    p1 = pins()
    press("shift", "h")
    h1 = held()
    press("esc")
    s = st()
    check("25h", "pinned again, taken back, then Esc: back in the view's corner, and 3D stays on",
          p1.get("h3d-right") is True and h1 == "h3d-right" and pins().get("h3d-right") is True and s["holding"] is False and s["mode"] == "active", f"{p1}, held {h1} -> {pins()}; {s['mode']}")

    fresh()
    a = aim_find("h3d-right")
    press("h")
    h0 = held()
    press("shift", "h")
    p = pins()
    check("25h", "carrying a terminal (H), Shift+H pins it, not what's behind it: carried no more",
          a and h0 == "h3d-right" and p.get("h3d-right") and not p.get("h3d-left") and st()["holding"] is False, f"aimed {a and a.get('class')}, held {h0}; {p}")

    ctl("hypr3d", "reset-windows", "forget")
    time.sleep(1.0)
    r = [ctl("hypr3d", "window", "h3d-left", "pin"), ctl("hypr3d", "window", "h3d-right", "pin")]
    p0 = pins()
    press("shift", "h")
    p1, h1 = pins(), held()
    press("h")
    press("shift", "h")
    p2, h2 = pins(), held()
    press("h")
    check("25h", "two pinned: Shift+H takes the last one pinned into your hands (H puts it down), Shift+H again the other",
          r == ["pinned", "pinned"] and p0 == {"h3d-left": True, "h3d-right": True} and p1 == {"h3d-left": True, "h3d-right": False} and h1 == "h3d-right"
          and p2 == {"h3d-left": False, "h3d-right": False} and h2 == "h3d-left" and st()["holding"] is False, f"{r}; {p0} -> {p1} (held {h1}) -> {p2} (held {h2})")

    fresh()
    a = aim_find("h3d-left")
    ctl("hypr3d", "window", "h3d-right", "pin")
    press("tab")
    n = errors("close the Action Menu first")
    press("h")
    press("shift", "h")
    s0, p0 = st(), pins()
    check("25h", "the Action Menu open (the crosshair hidden): H picks nothing up, Shift+H doesn't take the pinned one back, and they say to close the menu",
          a and menu().get("open") and s0["holding"] is False and p0 == {"h3d-right": True} and errors("close the Action Menu first") == n + 2, f"{p0}; holding {s0['holding']}; menu {menu().get('open')}")
    press("esc", after=0.05)
    press("h")  # at once, while the menu fades
    h1 = held()
    press("h")
    check("25h", "... closed (H at once, as it fades), H picks up; H again puts it down",
          h1 == "h3d-left" and st()["holding"] is False and not menu().get("open"), f"held {h1}; holding {st()['holding']}")

    fresh()
    a = aim_find("h3d-right")
    r1 = ctl("hypr3d", "pin")
    p1 = pins()
    r2 = ctl("hypr3d", "pin")
    h2 = held()
    r3 = ctl("hypr3d", "grab")
    p3 = pins()
    check("25h", "hyprctl hypr3d pin: pins the window under the crosshair, then takes it back into your hands, as Shift+H does; hyprctl hypr3d grab puts it down, as H does",
          a and r1 == "pinned" and p1.get("h3d-right") is True and r2 == "holding" and h2 == "h3d-right" and r3 == "placed" and p3.get("h3d-right") is False and st()["holding"] is False,
          f"{r1} {p1}; {r2} held {h2}; {r3} {p3}")

    ctl("hypr3d", "reset-windows", "forget")
    ensure_3d(False)
    shell_overlay(False)


LAUNCHER = f"{H}/launcher.qml"
LAUNCHER_LOG = "/tmp/h3d-launcher.log"


def launcher_log():
    """launcher.qml's log: opened, typed, clicked, closed"""
    return machine.execute(f"cat {LAUNCHER_LOG} 2>/dev/null || true")[1].splitlines()


def shell_layer(input_=True):
    """the shell layer over the 3D view if it has the keyboard (any with input_=False), else None"""
    s = st().get("shell")
    return s if s and (s.get("input") or not input_) else None


def tablet_to(x, y, w=1280, h=800):
    """the tablet to this spot of the screen, logical px"""
    tablet(x * 32767 / w, y * 32767 / h)


def launcher_toggle(which="toggle"):
    alice(f"quickshell ipc -p {LAUNCHER} call launcher {which}")


@section("26", "a launcher on a keybind (Super+D, a shell's layer surface that takes the keyboard): over the 3D view where you see it, the keys and a pointer its own till it closes, walking again after; one that takes the keyboard exclusively (as rofi and fuzzel do) too")
def s_launcher():
    lua_session()
    ensure_plugin()
    ensure_3d(False)
    clean_windows()
    machine.execute(f"pkill -f 'quickshell -p'; rm -f {LAUNCHER_LOG}; true")
    alice(f"setsid -f quickshell -p {LAUNCHER} > /tmp/quickshell-launcher.log 2>&1")
    wait_for("the launcher's IPC", lambda: as_alice(f"quickshell ipc -p {LAUNCHER} show", 10)[0] == 0, 30, 0.5)
    r = ctl("eval", f'hl.bind("SUPER + D", hl.dsp.exec_cmd("quickshell ipc -p {LAUNCHER} call launcher toggle"))')
    # 2D first
    press("meta_l", "d")
    wait_for("the launcher", lambda: "h3d-launcher" in ctl("-j", "layers"), 10)
    time.sleep(0.8)
    frame("launcher-2d")
    press("esc")
    wait_for("the launcher gone", lambda: "h3d-launcher" not in ctl("-j", "layers"), 10)
    check("26", "on the 2D desktop Super+D opens the launcher and Esc closes it", "open" in launcher_log() and "escape" in launcher_log(), f"{r}; {launcher_log()}")

    ensure_3d()
    menu_closed()
    ctl("hypr3d", "view", "first")
    ctl("hypr3d", "spawn")
    time.sleep(1.0)
    feet = st()["feet"]
    n = len(launcher_log())
    press("meta_l", "d")
    sh = wait_for("the launcher over the 3D view", shell_layer, 10)
    time.sleep(0.8)
    check("26", "Super+D in 3D (the keybind still Hyprland's): its launcher comes over the 3D view, with the keys and the mouse",
          sh.get("namespace") == "h3d-launcher" and st()["mode"] == "active", sh)
    img = frame("launcher-3d")
    cx, cy = img.w // 2, img.h // 2

    def orange(r, g, b):
        return r > 200 and 90 < g < 170 and b < 70
    got = img.count(orange, (cx - 200, cy - 100, cx + 200, cy + 100), step=4) / (100 * 50)
    check("26", "... where you see it: its box in the middle of the screen, as on the 2D desktop", got > 0.8, f"{got:.0%} of the middle its orange")
    full = [p for p in panels() if p["kind"] == "layer" and p["box"] == [0, 0, img.w, img.h]]
    check("26", "... and not on the desktop wall meanwhile", not full, full)
    type_text("wasd")
    time.sleep(0.5)
    s, log = st(), launcher_log()[n:]
    check("26", "typing goes to it (WASD too), not to walking", "text wasd" in log and math.dist(feet, s["feet"]) < 0.01, f"{log[-3:]}; feet {feet} -> {s['feet']}")

    # the box is 480x260 in the middle, its button at x 20-220, y 180-240
    tablet_to(cx - 100, cy + 60)
    time.sleep(0.5)
    ptr = (shell_layer() or {}).get("pointer")
    click("left")
    time.sleep(0.6)
    log = launcher_log()[n:]
    go = [l for l in log if l.startswith("clicked go")]
    xy = [int(v) for v in go[-1].split()[2:4]] if go else None
    check("26", "the pointer over it (a tablet here), a click on its button: the button's, where the pointer is",
          xy is not None and abs(xy[0] - 120) <= 2 and abs(xy[1] - 10) <= 2, f"pointer {ptr}; {log[-3:]}")
    frame("launcher-pointer")  # its cursor at the pointer
    rel(-60, 40)
    time.sleep(0.5)
    ptr2 = (shell_layer() or {}).get("pointer")
    check("26", "the mouse moves its pointer (not the camera)", ptr and ptr2 and ptr2[0] < ptr[0] and ptr2[1] > ptr[1] and abs(st()["yaw"] - s["yaw"]) < 0.01,
          f"{ptr} -> {ptr2}, yaw {s['yaw']} -> {st()['yaw']}")
    tablet_to(cx, cy)  # back over its box
    time.sleep(0.4)
    wheel(1)
    time.sleep(0.4)
    check("26", "the wheel goes to it", any(l.startswith("wheel") for l in launcher_log()[n:]), launcher_log()[-2:])

    press("esc")
    wait_for("the launcher closed", lambda: not shell_layer(), 10)
    time.sleep(1.0)
    s = st()
    check("26", "Esc is the launcher's: it closes, and it's 3D's again (still in 3D)", "escape" in launcher_log()[n:] and s["mode"] == "active", f"{s['mode']} {s.get('shell')}")
    feet = s["feet"]
    press("w", hold=1.2)
    time.sleep(0.3)
    moved = math.dist(feet, st()["feet"])
    check("26", "... W walks again", moved > 0.4, f"{moved:.2f} m")

    press("meta_l", "d")
    wait_for("the launcher again", shell_layer, 10)
    press("meta_l", "d")
    closed = wait_for("closed by the keybind", lambda: not shell_layer(), 10)
    check("26", "Super+D again closes it", closed and launcher_log()[-1] == "closed", launcher_log()[-2:])
    press("meta_l", "d")
    wait_for("the launcher again", shell_layer, 10)
    tablet_to(80, 80)
    click("left")
    wait_for("closed by the click", lambda: not shell_layer(), 10)
    check("26", "a click outside its box closes it, as on the 2D desktop", "clicked outside" in launcher_log()[n:], launcher_log()[-2:])

    time.sleep(0.8)
    launcher_toggle("exclusive")
    sh = wait_for("the exclusive one over the view", shell_layer, 10)
    time.sleep(0.6)
    type_text("hi")
    time.sleep(0.4)
    img = frame("launcher-exclusive")

    def green(r, g, b):
        return r < 60 and g > 170 and b < 140
    got = img.count(green, (cx - 150, cy - 50, cx + 150, cy + 50), step=4) / (75 * 25)
    check("26", "one that takes the keyboard exclusively (as rofi and fuzzel do): over the view too, in the middle, typed into",
          sh.get("namespace") == "h3d-exclusive" and got > 0.6 and "exclusive text hi" in launcher_log(), f"{sh}; {got:.0%} green; {launcher_log()[-2:]}")
    press("esc")
    wait_for("the exclusive one closed", lambda: not shell_layer(), 10)
    feet = st()["feet"]
    press("w", hold=1.2)
    time.sleep(0.3)
    check("26", "... Esc closes it, and W walks again", "exclusive escape" in launcher_log() and math.dist(feet, st()["feet"]) > 0.4, launcher_log()[-2:])
    ensure_3d(False)
    machine.execute("pkill -f 'quickshell -p'; true")
    ctl("eval", 'hl.unbind("SUPER + D")')  # else a later bind toggles twice


def gait():
    return av().get("gait") or {}


def walking(keys, hold, every=0.25, after=0.4):
    """keys held, the gait sampled after the first second; then released"""
    for k in keys:
        qmp([key_event(k, True)])
        time.sleep(0.03)
    time.sleep(1.0)
    seen = []
    end = time.time() + hold
    while time.time() < end:
        seen.append(gait())
        time.sleep(every)
    for k in reversed(keys):
        qmp([key_event(k, False)])
        time.sleep(0.03)
    time.sleep(after)
    return seen


@section("27", "the avatar walking: W walks at walk_speed (feet planted where they land, a stride and cadence as people's), Shift runs at run_speed, letting go stops it on its feet; walk_speed set at run time; up a step and a platform, a foot on each, the view going up smoothly, and back down on its feet")
def s_gait():
    lua_session()
    ensure_plugin()
    ensure_3d(False)
    clean_windows()
    ctl("hypr3d", "map", "none")
    ensure_avatar(AV)
    ensure_3d()
    menu_closed()
    ctl("hypr3d", "view", "third")
    ctl("hypr3d", "tp", "0", "0", "6")
    ctl("hypr3d", "turn", "180", "0")  # away from the desktop wall
    time.sleep(1.5)
    g = gait()
    check("27", "standing: walked procedurally (the avatar has no clips), both feet down",
          g and not g.get("moving") and all(f["planted"] for f in g.get("feet", [])), g)

    seen = walking(["w"], 1.5)
    frame("gait-walk")
    moving = [x for x in seen if x.get("moving")]
    speeds = [x["speed"] for x in moving]
    down = {(x["feet"][0]["planted"], x["feet"][1]["planted"]) for x in moving}
    check("27", "W walks: at walk_speed (1.6 m/s), a walk (not a run)",
          len(moving) >= 3 and all(abs(v - 1.6) < 0.15 for v in speeds) and all(x["run"] < 0.1 for x in moving), f"{len(moving)} of {len(seen)}; speeds {speeds}")
    check("27", "... a stride and a cadence as people's (0.9-1.8 m, 0.8-1.6 strides a second), one foot down at a time or both, never neither",
          moving and all(0.9 <= x["stride"] <= 1.8 and 0.8 <= x["cadence"] <= 1.6 for x in moving) and (False, False) not in down,
          [(round(x["stride"], 2), round(x["cadence"], 2)) for x in moving[:4]], )
    time.sleep(1.5)
    g = gait()
    check("27", "letting go: it stops, standing on both feet", g and not g["moving"] and all(f["planted"] for f in g["feet"]), g)

    seen = walking(["shift", "w"], 1.5)
    frame("gait-run")
    moving = [x for x in seen if x.get("moving")]
    check("27", "Shift+W runs: at run_speed (4.5 m/s), a run (each foot down less than half the time: time in the air)",
          len(moving) >= 3 and all(abs(x["speed"] - 4.5) < 0.3 and x["run"] > 0.9 and x["duty"] < 0.45 for x in moving),
          [(round(x["speed"], 2), x["run"], round(x["duty"], 2)) for x in moving[:4]])
    time.sleep(2.0)

    r = ctl("eval", "hl.config({ plugin = { hypr3d = { walk_speed = 1.2 } } })")
    time.sleep(0.5)
    ctl("hypr3d", "tp", "0", "0", "6")
    time.sleep(0.5)
    seen = walking(["w"], 1.0)
    speeds = [x["speed"] for x in seen if x.get("moving")]
    check("27", "plugin:hypr3d:walk_speed set at run time (hl.config() through hyprctl eval, 1.2): walks at that", speeds and all(abs(v - 1.2) < 0.15 for v in speeds), f"eval: {r}; {speeds}")
    ctl("eval", "hl.config({ plugin = { hypr3d = { walk_speed = 1.6 } } })")
    time.sleep(0.5)

    # the courtyard's east corner: a 0.45 m step (x 6.2-6.8) onto a 0.9 m platform (from x 6.8), z 18-20
    ctl("hypr3d", "tp", "3.5", "0", "19")
    ctl("hypr3d", "turn", "90", "0")
    time.sleep(1.5)

    def over_steps(done):
        """walks forward till done(status): planted feet heights, (body, seen) heights, samples off the ground or in an
        air pose"""
        ctl("hypr3d", "walk", "3", "forward")
        heights, ys, off, x = set(), [], [], {}
        end, n = time.time() + 30, 0
        while time.time() < end:
            s, n = st(), n + 1  # polled as fast as it answers; the gait every 3rd
            if n % 3 == 0:
                x = gait()
                for f in x.get("feet", []):
                    if f["planted"]:
                        heights.add(round(f["at"][1], 2))
            ys.append((s["feet"][1], s["seenY"]))
            if not s["onGround"] or s["anim"].startswith("air"):
                off.append((s["feet"], s["anim"]))
            if done(s) and x and not x.get("moving"):
                break
        return heights, ys, off

    heights, ys, _ = over_steps(lambda s: s["feet"][0] > 8.0)
    frame("gait-platform")
    on = lambda y: any(abs(h - y) < 0.03 for h in heights)  # noqa: E731
    check("27", "up the step and onto the platform: feet stood on the floor, on the step and on the platform (not in the air, not sunk)",
          on(0.0) and on(0.45) and on(0.9) and st()["feet"][1] > 0.85, f"planted at {sorted(heights)}; feet {st()['feet']}")
    # the body steps 0.45 m up in a frame; the view must follow smoothly
    jumps = [(round(b[0] - a[0], 2), round(b[1] - a[1], 2)) for a, b in zip(ys, ys[1:]) if b[0] - a[0] > 0.3]
    check("27", "... the body stepped up each ledge at once, the view went up smoothly (the seen height not half as far in that moment)",
          jumps and all(sy < 0.5 * fy for fy, sy in jumps) and abs(ys[-1][1] - ys[-1][0]) < 0.02, f"steps (body, seen): {jumps}; last {ys[-1]}")
    ctl("hypr3d", "turn", "270", "0")
    time.sleep(1.0)
    heights, ys, off = over_steps(lambda s: s["feet"][0] < 4.0)
    frame("gait-platform-down")
    on = lambda y: any(abs(h - y) < 0.03 for h in heights)  # noqa: E731
    check("27", "down off the platform and the step: on the ground all the way (never falling, nor the air pose), feet on the platform, the step and the floor",
          not off and on(0.0) and on(0.45) and on(0.9) and abs(st()["feet"][1]) < 0.05, f"off the ground {off[:4]}; planted at {sorted(heights)}; feet {st()['feet']}")
    ctl("hypr3d", "spawn")
    ensure_3d(False)


@section("30", "flying and jumping: F takes to the air and out of it going on as it went (not stopping dead); hovering upright, flying ahead lying along the way it goes, leaning back as it stops; out of the air it falls and lands on its feet; a jump from a run: a stride in the air (the leg that was stepping ahead), landing into the run")
def s_fly():
    lua_session()
    ensure_plugin()
    ensure_3d(False)
    clean_windows()
    ctl("hypr3d", "map", "none")
    ensure_avatar(AV)
    ensure_3d()
    menu_closed()
    ctl("hypr3d", "view", "third")
    ctl("hypr3d", "tp", "0", "0", "6")
    ctl("hypr3d", "turn", "180", "0")  # away from the desktop wall
    time.sleep(1.5)

    def air():
        return gait().get("air") or {}

    def sample(secs, every=0.1):
        seen, end = [], time.time() + secs
        while time.time() < end:
            s, a = st(), air()
            seen.append((time.time(), s, a))
            time.sleep(every)
        return seen

    # Space for a second: above the yard's walls
    press("f")
    qmp([key_event("spc", True)])
    time.sleep(1.0)
    qmp([key_event("spc", False)])
    time.sleep(2.0)
    s, a = st(), air()
    check("30", "F then Space: flying, up off the ground, hovering upright (the body lying less than 12° from upright)",
          s["fly"] and s["feet"][1] > 2.0 and a and a["fly"] > 0.9 and abs(a["pitch"]) < 12, f"fly {s['fly']}, feet {s['feet']}, air {a}")
    qmp([key_event("w", True)])
    time.sleep(1.2)
    seen = sample(1.0)
    frame("fly-ahead")
    pitches = [a.get("pitch", 0) for _, _, a in seen if a]
    check("30", "W flying: the body lies along the way it goes (pitched 35-80° ahead at 8 m/s)",
          pitches and all(35 <= p <= 80 for p in pitches), pitches)
    cruise = sum(pitches) / max(len(pitches), 1)
    qmp([key_event("w", False)])
    seen = sample(0.8, 0.05)
    least = min((a.get("pitch", 99) for _, _, a in seen if a), default=99)
    time.sleep(2.5)
    a = air()
    check("30", "... letting go it leans back as it slows (to 20° or more under how it lay), then hovers upright again",
          least < cruise - 20 and a and abs(a["pitch"]) < 12, f"cruise {cruise:.0f}, least {least:.0f}, then {a}")
    ctl("hypr3d", "tp", "0", "3", "6")
    ctl("hypr3d", "turn", "180", "0")
    time.sleep(1.0)
    qmp([key_event("w", True)])
    time.sleep(0.6)
    feet0 = st()["feet"]
    press("f", after=0.0)
    time.sleep(0.4)
    feet1 = st()["feet"]
    qmp([key_event("w", False)])
    went = math.hypot(feet1[0] - feet0[0], feet1[2] - feet0[2])
    check("30", "... F flying ahead: out of the air going on ahead as it was (not stopping dead): over 1 m in the next 0.4 s",
          went > 1.0 and not st()["fly"], f"{went:.2f} m; {feet0} -> {feet1}")
    s = wait_for("landed", lambda: (lambda s: s if s["onGround"] else None)(st()), 8, 0.1)
    time.sleep(1.5)
    g, s1 = gait(), st()
    check("30", "... it falls and lands on its feet ahead in the yard: on the ground, standing on both, not in the air pose",
          s["onGround"] and s1["feet"][2] > feet0[2] + 1.0 and g and not g.get("air") and all(f["planted"] for f in g.get("feet", [])) and not s1["anim"].startswith("air"),
          f"at {s1['feet']}; {s1['anim']}; {g}")
    ctl("hypr3d", "tp", "0", "0", "6")
    ctl("hypr3d", "turn", "180", "0")
    time.sleep(1.0)
    qmp([key_event("shift", True)])
    time.sleep(0.03)
    qmp([key_event("w", True)])
    time.sleep(0.8)
    press("spc", hold=0.05, after=0.0)
    seen = sample(0.9, 0.04)
    qmp([key_event("w", False)])
    time.sleep(0.03)
    qmp([key_event("shift", False)])
    strides = [a for _, _, a in seen if a and a.get("lead", -1) >= 0 and not a.get("fly")]
    ahead = [a["legs"][a["lead"]][0] - a["legs"][1 - a["lead"]][0] for a in strides]
    landed = [s for t, s, a in seen if not a and s["onGround"] and s["anim"].split(":")[0] in ("walk", "run")]
    check("30", "a jump from a run: a stride in the air (the leading thigh 20° or more ahead of the other), and it lands running on",
          strides and min(ahead) >= 20 and landed, f"{len(strides)} in the air, ahead {ahead[:6]}; landed running {len(landed)}")
    time.sleep(1.5)
    ctl("hypr3d", "spawn")
    ensure_3d(False)


# ------------------------------------------------------------------ first person with the avatar's body

def hands():
    """first person with the body: the hands' mode, arm weight and wrists in the view"""
    return av().get("hands") or {}


def wrist_in_view(p, below=0.0):
    """a wrist (0..1 across and down) in the view, at least `below` down"""
    return p is not None and 0.0 <= p[0] <= 1.0 and below <= p[1] <= 1.0


def hands_while(keys, secs, every=0.15):
    """keys held, the hands sampled after 0.8 s; then released"""
    for k in keys:
        qmp([key_event(k, True)])
        time.sleep(0.03)
    time.sleep(0.8)
    seen, end = [], time.time() + secs
    while time.time() < end:
        seen.append(hands())
        time.sleep(every)
    for k in reversed(keys):
        qmp([key_event(k, False)])
        time.sleep(0.03)
    time.sleep(0.5)
    return seen


def mode_is(mode, arms=None):
    return lambda: (lambda h: h if h.get("mode") == mode and (arms is None or arms(h.get("arms", -1))) else None)(hands())


@section("32", "first person with the avatar's body (plugin:hypr3d:first_person_body): the camera in its eyes, its own height, still standing; its hands up low in the view as a first person game's, walking (bobbing) and running (pumping); looking far down they let go; a window pressed: the right hand's finger to the crosshair; typing (E): lower and nearer together; carrying (G): out to it; playing here (P): let down, the camera still; an emote: the camera out behind it and back in; hyprctl view body and the config value: as before when off (1.65 m, no body)")
def s_fpbody():
    lua_session()
    ensure_plugin()
    ensure_3d(False)
    clean_windows()
    machine.execute("pkill -u alice foot; true")
    wait_for("no windows", lambda: not json.loads(ctl("-j", "clients")), 10, 0.3)
    ctl("hypr3d", "map", "none")
    ensure_avatar(AV)
    ensure_3d()
    menu_closed()
    ctl("hypr3d", "view", "first")
    first_person_body(True)
    ctl("hypr3d", "tp", "0", "0", "6")
    ctl("hypr3d", "turn", "180", "0")  # away from the desktop wall
    wait_for("the hands up", mode_is("ready", lambda w: w > 0.95), 15, 0.2)
    time.sleep(0.6)
    s, a = st(), av()
    h, up = a.get("hands") or {}, s["eye"][1] - s["seenY"]
    check("32", "first person with the avatar: its body's view (the status and the avatar's say so), the camera in its eyes at its own height (not 1.65 m)",
          s["body"] is True and a["body"] is True and 0.9 < a["eyeHeight"] < 1.6 and abs(up - a["eyeHeight"]) < 0.06,
          f"body {s['body']}/{a['body']}; the eye {up:.3f} m up, its eyes {a.get('eyeHeight')}")
    ready = h.get("at") or [None, None]
    check("32", "... its hands up in the view, low: the left left of the middle and the right right of it, both in the bottom half (ready)",
          h.get("mode") == "ready" and h.get("arms", 0) > 0.95 and wrist_in_view(ready[0], 0.55) and wrist_in_view(ready[1], 0.55) and ready[0][0] < 0.5 < ready[1][0], h)
    frame("fpbody-ready")
    # wait till the body has turned and stands: a step moves the eyes
    wait_for("standing still", lambda: (lambda g: not g.get("moving") and all(f["planted"] for f in g.get("feet", [{}])))(gait()), 15, 0.2)
    time.sleep(1.0)
    e0 = st()["eye"]
    time.sleep(2.0)
    e1 = st()["eye"]
    check("32", "... standing, the camera doesn't move (breathing moves the eyes a little: over 2 s it's within 1 mm)", math.dist(e0, e1) < 0.001, f"{math.dist(e0, e1) * 1000:.2f} mm")
    r = ctl("hypr3d", "view", "body", "sideways")
    check("32", "hyprctl hypr3d view body with a wrong word: says how it goes", r == "error: view body [on|off|toggle]", r)

    seen = hands_while(["w"], 1.5)
    ok = [x for x in seen if x.get("arms", 0) > 0.9 and all(wrist_in_view(p, 0.45) for p in (x.get("at") or [None, None]))]
    check("32", "walking (W): the hands stay up in the view", len(seen) >= 3 and len(ok) == len(seen), [x.get("at") for x in seen[:4]])
    seen = hands_while(["shift", "w"], 1.6, 0.1)
    ys = [p[1] for x in seen for p in (x.get("at") or []) if p]
    check("32", "running (Shift+W): the hands pump, each up into the view and back down (their height in the view goes over a sixth of it)",
          len(ys) >= 6 and max(ys) - min(ys) > 0.16 and min(ys) < 0.9, f"{len(ys)} samples, {min(ys or [0]):.2f}..{max(ys or [0]):.2f}")
    ctl("hypr3d", "tp", "0", "0", "6")
    time.sleep(0.5)
    s, a = st(), av()
    check("32", "hyprctl hypr3d tp: the camera with the body at once, in its eyes", abs(s["eye"][1] - s["seenY"] - a["eyeHeight"]) < 0.06 and math.dist([s["eye"][0], s["eye"][2]], [0, 6]) < 0.3,
          f"eye {s['eye']}, feet {s['feet']}")

    ctl("hypr3d", "turn", "180", "-80")
    h = wait_for("the arms let go", mode_is("ready", lambda w: w < 0.05), 10, 0.2)
    frame("fpbody-down")
    ctl("hypr3d", "turn", "180", "0")
    h2 = wait_for("the arms up again", mode_is("ready", lambda w: w > 0.95), 10, 0.2)
    check("32", "looking far down (80°): the hands let go, the arms hang (the body below in view); looking up again they come back", h and h2, f"{h}; {h2}")
    # the camera follows the eyes down, not the crouched body's top (its neck)
    qmp([key_event("c", True)])
    try:
        time.sleep(1.5)
        s, h = st(), hands()
        frame("fpbody-crouch")
    finally:
        qmp([key_event("c", False)])
    at, up = h.get("at") or [None, None], s["eye"][1] - s["seenY"]
    check("32", "crouching (C held): the camera comes down with its eyes, the hands still up in the view", 0.6 < up < 1.15 and all(wrist_in_view(p, 0.45) for p in at),
          f"the eye {up:.3f} m up; {h}")
    wait_for("standing up", lambda: abs(st()["eye"][1] - st()["seenY"] - av()["eyeHeight"]) < 0.06, 10, 0.2)

    alice("setsid -f foot --app-id h3d-fp > /dev/null 2>&1")
    wait_for("the terminal in the world", lambda: placed("h3d-fp"), 20, 0.5)
    time.sleep(1.0)
    a = aim_find("h3d-fp")
    check("32", "a terminal opened in 3D, in front of you, the crosshair on it", a, a or AIM.get("last"))
    qmp([{"type": "btn", "data": {"down": True, "button": "left"}}])
    try:
        wait_for("touching", mode_is("touch"), 5, 0.1)
        time.sleep(1.0)
        h = hands()
        frame("fpbody-touch")
    finally:
        qmp([{"type": "btn", "data": {"down": False, "button": "left"}}])
    r = (h.get("at") or [None, None])[1]
    check("32", "pressing on it (the left button held): the right hand reaches to the crosshair, its finger pointing (touch): the wrist right of the middle, a little under it",
          h.get("mode") == "touch" and r and 0.5 < r[0] < 0.8 and 0.5 < r[1] < 0.88, h)
    h = wait_for("ready again", mode_is("ready"), 5, 0.1)
    check("32", "... let go: the hand back, ready", h, h)
    press("e")
    h = wait_for("typing hands", mode_is("type"), 5, 0.1)
    time.sleep(1.0)
    h = hands()
    t = h.get("at") or [None, None]
    type_text("echo hi")
    frame("fpbody-type")
    check("32", "typing (E): the hands lower and nearer together, in the view (type)",
          st()["typing"] is True and h.get("mode") == "type" and all(wrist_in_view(p) for p in t) and t[1][0] - t[0][0] < ready[1][0] - ready[0][0] - 0.08, f"{h}; ready {ready}")
    press("meta_l", "esc")
    wait_for("walking again", lambda: st()["typing"] is False, 5, 0.2)
    a = aim_find("h3d-fp")
    press("g")
    h = wait_for("holding", mode_is("hold"), 5, 0.1)
    time.sleep(1.0)
    h = hands()
    g = h.get("at") or [None, None]
    frame("fpbody-hold")
    check("32", "carrying it (G): both hands out to it, higher in the view than ready (hold)",
          st()["holding"] is True and h.get("mode") == "hold" and wrist_in_view(g[0]) and wrist_in_view(g[1]) and g[0][1] < ready[0][1] - 0.08 and g[1][1] < ready[1][1] - 0.08, f"{h}; ready {ready}")
    press("esc")  # put back where it was
    wait_for("not holding", lambda: st()["holding"] is False, 5, 0.2)
    a = aim_find("h3d-fp")
    wait_for("ready again", mode_is("ready", lambda w: w > 0.95), 10, 0.2)
    time.sleep(0.5)
    before = calm_frame("fpbody-before-play")
    c0 = st()["eye"]
    press("p")
    h = wait_for("the arms let down", mode_is("down", lambda w: w < 0.02), 10, 0.2)
    img = calm_frame("fpbody-play")
    s = st()
    check("32", "playing it here (P): the hands let down out of the view (down), the camera where it was",
          s["playing"] and h and math.dist(c0, s["eye"]) < 0.001, f"{s['playing']}; {h}; the eye moved {math.dist(c0, s['eye']) * 1000:.2f} mm")
    W, H = img.w, img.h
    boxes = [(int((p[0] - 0.07) * W), int((p[1] - 0.08) * H), int((p[0] + 0.07) * W), H) for p in ready]
    gone = [before.differs(img, b) for b in boxes]
    still = before.differs(img, (int(0.3 * W), int(0.08 * H), int(0.7 * W), int(0.3 * H)))
    check("32", "... where the hands were it changed (they went), the top of the view didn't", min(gone) > 0.2 and still < 0.03, f"hands' places {[f'{d:.0%}' for d in gone]}, the top {still:.1%}")
    press("meta_l", "esc")
    h = wait_for("ready again", mode_is("ready", lambda w: w > 0.95), 10, 0.2)
    check("32", "... Super+Esc: walking again, the hands back up", st()["playing"] is None and h, h)
    machine.execute("pkill -u alice foot; true")
    wait_for("no windows", lambda: not json.loads(ctl("-j", "clients")), 10, 0.3)

    ctl("hypr3d", "tp", "0", "0", "6")
    ctl("hypr3d", "turn", "180", "0")
    time.sleep(0.8)
    r = ctl("hypr3d", "avatar", "emote", "Wave")
    a = wait_for("out behind it", lambda: (lambda a: a if a.get("emoteView", 0) >= 1 else None)(av()), 15, 0.2)
    s = st()
    eyes = [s["feet"][0], s["seenY"] + a["eyeHeight"], s["feet"][2]]
    out = math.dist(s["eye"], eyes)
    img = frame("fpbody-emote")
    check("32", "an emote (Wave): the camera out behind the avatar while it plays, as in third person (the status still says first)",
          a["emote"] == "Wave" and a["view"] == "first" and out > 0.8, f"{r}; {a['emote']}, out {out:.2f} m")
    # while it comes back in, the hands must stay down, not reach behind
    back, end = [], time.time() + 30
    while time.time() < end:
        a = av()
        if not a.get("emote") and a.get("emoteView", 1) > 0:
            back.append(a)
        if a.get("emoteView", 1) <= 0 and not a.get("emote"):
            break
        time.sleep(0.05)
    time.sleep(0.5)
    s = st()
    check("32", "... done, back into its eyes", a.get("emoteView", 1) <= 0 and abs(s["eye"][1] - s["seenY"] - a["eyeHeight"]) < 0.06, f"eye {s['eye']}, feet {s['feet']}")
    behind = [w[2] for x in back for w in (x.get("wrists") or [])]
    check("32", "... while the camera came back in, the hands let down (not reaching back to it behind the avatar)",
          back and all((x.get("hands") or {}).get("mode") == "down" for x in back) and max(behind) < 0.08,
          f"{len(back)} samples, modes {sorted({(x.get('hands') or {}).get('mode') for x in back})}, the wrists' z up to {max(behind or [0]):.2f}")

    first_person_body(False)
    time.sleep(1.0)
    s, a = st(), av()
    check("32", "hyprctl hypr3d view body off: first person as before (1.65 m up, the body not drawn, no hands)",
          s["body"] is False and a["body"] is False and abs(s["eye"][1] - s["seenY"] - 1.65) < 0.03 and a.get("hands") is None, f"eye {s['eye'][1] - s['seenY']:.3f} m up; {a.get('hands')}")
    frame("fpbody-off")
    ctl("eval", "hl.config({ plugin = { hypr3d = { first_person_body = false } } })")
    time.sleep(1.5)
    r = ctl("eval", "hl.config({ plugin = { hypr3d = { first_person_body = true } } })")
    on = wait_for("the config's on", lambda: st()["body"] is True, 5, 0.2)
    ctl("eval", "hl.config({ plugin = { hypr3d = { first_person_body = false } } })")
    off = wait_for("the config's off", lambda: st()["body"] is False, 5, 0.2)
    check("32", "plugin:hypr3d:first_person_body set at run time (true, then false) takes effect", on and off, f"eval: {r}")
    ctl("eval", "hl.config({ plugin = { hypr3d = { first_person_body = true } } })")
    wait_for("the config's on", lambda: st()["body"] is True, 5, 0.2)
    wait_for("the hands up", mode_is("ready", lambda w: w > 0.95), 15, 0.2)
    ctl("hypr3d", "view", "third")
    going, end = [], time.time() + 1.5
    while time.time() < end:
        going.append(av().get("wrists") or [])
        time.sleep(0.05)
    third = st()["body"]
    ctl("hypr3d", "view", "first")
    time.sleep(0.5)
    check("32", "third person (V): not the body's first person; V again: it is", third is False and st()["body"] is True, f"{third} -> {st()['body']}")
    zs = [w[2] for x in going for w in x]
    check("32", "... going to third person the hands go down to the sides from where they were (never reaching behind the avatar)", zs and max(zs) < 0.08,
          f"{len(going)} samples, the wrists' z up to {max(zs or [0]):.2f}, last {going[-1] if going else None}")
    first_person_body(False)
    ensure_3d(False)


# ------------------------------------------------------------------ attacks

def attacks():
    """the attacks in the avatar's status: each arm's swing, the queued one, the last arm, the count"""
    return av().get("attack") or {}


def swing_seen(n, timeout=8):
    """till the n-th swing has started"""
    return wait_for(f"swing {n}", lambda: (lambda a: a if a.get("swings", 0) >= n else None)(attacks()), timeout, 0.05)


def swings_done(timeout=10):
    return wait_for("the swings done", lambda: (lambda a: a if a.get("left", 1) is None and a.get("right", 1) is None and a.get("next", 1) is None else None)(attacks()),
                    timeout, 0.1)


def aim_nothing():
    """the crosshair on nothing: in the yard, away from the desktop wall, looking up a little"""
    ctl("hypr3d", "tp", "0", "0", "6")
    ctl("hypr3d", "turn", "180", "12")
    return wait_for("the crosshair on nothing", lambda: st()["aimed"] is None or None, 5, 0.2) and st()["aimed"] is None


def click_nothing(n=1, every=0.35):
    """left clicks on nothing; the attacks sampled till they're done"""
    seen = []
    for k in range(n):
        click("left", hold=0.05, after=0.0)
        end = time.time() + every
        while time.time() < end:
            seen.append(attacks())
            time.sleep(0.03)
    end = time.time() + 6
    while time.time() < end:
        a = attacks()
        seen.append(a)
        if a.get("left") is None and a.get("right") is None and a.get("next") is None:
            break
        time.sleep(0.05)
    return seen


def swing_order(seen, before):
    """the arms in the order their swings started (R, L), from samples of the status"""
    out, n = "", before
    for a in seen:
        while a.get("swings", 0) > n:
            n += 1
            out += "L" if a["swings"] == n and a.get("last") == "left" else "R" if a["swings"] == n else "?"
    return out


@section("33", "attacks: a left click with the crosshair on nothing swings the avatar's arm, the fist closed (the right; clicks while it swings, one arm then the other; after a pause the right again), third person and first (the fist drawn back at the right of the view, struck across the middle); on a window the click is the window's; typing (E), the Action Menu, carrying (G) and playing (P): no swing; a window's menu open: the click closes it, the next swings; hyprctl hypr3d avatar attack; an avatar that isn't a humanoid: nothing")
def s_attack():
    lua_session()
    ensure_plugin()
    ensure_3d(False)
    clean_windows()
    machine.execute("pkill -u alice foot; true")
    wait_for("no windows", lambda: not json.loads(ctl("-j", "clients")), 10, 0.3)
    ctl("hypr3d", "map", "none")
    ensure_avatar(AV)
    ensure_3d()
    menu_closed()
    ctl("hypr3d", "view", "third")
    on_nothing = aim_nothing()
    time.sleep(1.0)
    n0, f0 = attacks().get("swings", 0), st()["feet"]
    seen = click_nothing()
    order = swing_order(seen, n0)
    fist = [a for a in seen if (a.get("right") or {}).get("fist")]
    check("33", "third person, the crosshair on nothing: a left click swings the right arm, the fist closed (the avatar's status); it stays where it stands; done after",
          on_nothing and order == "R" and fist and seen[-1].get("right") is None and math.dist(f0, st()["feet"]) < 0.01,
          f"aimed {st()['aimed']}; order {order!r}, {len(fist)} samples with the fist, last {seen[-1]}; feet {f0} -> {st()['feet']}")
    # a frame mid-swing
    click("left", hold=0.05, after=0.12)
    frame("attack-third")
    swings_done()
    time.sleep(1.2)  # after a pause the right swings again
    n0 = attacks().get("swings", 0)
    seen = click_nothing(3, 0.35)
    order = swing_order(seen, n0)
    check("33", "... three clicks a third of a second apart: one arm then the other (R L R), each its own swing", order == "RLR", f"{order!r}; {[a.get('swings') for a in seen[::6]]}")
    time.sleep(1.2)
    n0 = attacks().get("swings", 0)
    order = swing_order(click_nothing(), n0)
    check("33", "... after a pause (over a second): the right arm again", order == "R", repr(order))

    press("tab")
    wait_for("the menu", lambda: menu().get("open"), 5, 0.2)
    n0 = attacks().get("swings", 0)
    click("left", hold=0.05, after=0.6)
    check("33", "the Action Menu open (Tab): a left click is the menu's, no swing", attacks().get("swings", 0) == n0, attacks())
    menu_closed()
    time.sleep(0.5)

    # wev opened outside 3D stays on the desktop wall (opened in 3D it'd come out in front)
    ensure_3d(False)
    wev_start()
    ensure_3d()
    menu_closed()
    ctl("hypr3d", "view", "third")
    ctl("hypr3d", "spawn")
    time.sleep(0.8)
    a = aim_find("wev")
    wev_mark("atk-click")
    n0 = attacks().get("swings", 0)
    click("left", hold=0.05, after=0.8)
    b = wev_buttons(wev_events("atk-click"))
    check("33", "the crosshair on a window (wev): a left click is the window's (wev gets the button), no swing",
          a and (272, 1) in b and (272, 0) in b and attacks().get("swings", 0) == n0, f"aimed {a and a.get('class')}; wev's buttons {b}; {attacks()}")
    press("e")
    wait_for("typing", lambda: st()["typing"] is True, 5, 0.2)
    ctl("hypr3d", "turn", f"{st()['yaw'] + 180:.1f}", "12")
    time.sleep(0.6)
    s = st()
    n0 = attacks().get("swings", 0)
    click("left", hold=0.05, after=0.6)
    check("33", "typing into it (E), the crosshair turned off it onto nothing: a left click doesn't swing (still typing)",
          s["typing"] is True and s["aimed"] is None and attacks().get("swings", 0) == n0 and st()["typing"] is True, f"typing {s['typing']}, aimed {s['aimed']}; {attacks()}")
    press("meta_l", "esc")
    wait_for("walking again", lambda: st()["typing"] is False, 5, 0.2)
    ctl("hypr3d", "spawn")
    time.sleep(0.8)
    aim_find("wev")
    press("g")
    wait_for("holding", lambda: st()["holding"] is True, 5, 0.2)
    n0 = attacks().get("swings", 0)
    click("left", hold=0.05, after=0.8)
    check("33", "carrying it (G): a left click puts it down, no swing", st()["holding"] is False and attacks().get("swings", 0) == n0, f"holding {st()['holding']}; {attacks()}")
    settled()
    aim_find("wev")
    press("p")
    wait_for("playing", lambda: st()["playing"], 5, 0.2)
    time.sleep(0.5)
    wev_mark("atk-play")
    n0 = attacks().get("swings", 0)
    click("left", hold=0.05, after=0.8)
    b = wev_buttons(wev_events("atk-play"))
    check("33", "playing it (P): a left click is the window's (wev gets it), no swing", st()["playing"] and (272, 1) in b and attacks().get("swings", 0) == n0, f"{b}; {attacks()}")
    press("meta_l", "esc")
    wait_for("walking again", lambda: st()["playing"] is None, 5, 0.2)
    ctl("hypr3d", "reset-windows", "forget")  # later sections expect wev on the wall
    machine.execute("pkill -x wev; true")
    wait_for("no windows", lambda: not json.loads(ctl("-j", "clients")), 10, 0.3)

    ctl("hypr3d", "view", "first")
    first_person_body(True)
    aim_nothing()
    wait_for("the hands up", mode_is("ready", lambda w: w > 0.95), 15, 0.2)
    time.sleep(0.6)
    n0 = attacks().get("swings", 0)
    click("left", hold=0.05, after=0.0)
    seen, end = [], time.time() + 2.5
    while time.time() < end:
        a = av()
        seen.append((a.get("attack") or {}, (a.get("hands") or {}).get("at") or [None, None]))
        time.sleep(0.02)
    rights = [r for at, (l, r) in seen if at.get("swings", 0) > n0 and r]
    drawn = [i for i, r in enumerate(rights) if r[0] > 0.68 and 0 <= r[1] <= 1]
    across = [r for r in rights[drawn[0]:] if 0.25 < r[0] < 0.7 and 0.25 < r[1] < 0.9] if drawn else []
    check("33", "first person with the body: a left click on nothing, the right fist drawn back at the right of the view, then struck across its middle",
          drawn and across, f"{len(rights)} samples: {[[round(v, 2) for v in r] for r in rights[:30]]}")
    swings_done()
    time.sleep(0.5)
    click("left", hold=0.05, after=0.15)
    frame("attack-first")
    swings_done()
    first_person_body(False)
    ctl("hypr3d", "view", "third")

    # hyprctl
    swings_done()
    r = ctl("hypr3d", "avatar", "attack", "left")
    ok = r.startswith("{") and json.loads(r).get("last") == "left"
    swings_done()
    r2 = ctl("hypr3d", "avatar", "attack", "sideways")
    check("33", "hyprctl hypr3d avatar attack left: that arm's swing (the attacks, as JSON); a wrong word says how it goes",
          ok and r2 == "error: avatar attack [left|right]", f"{r}; {r2}")

    # a menu that grabs the pointer (weston-terminal's; browser menus take no grab): a click on nothing only closes it,
    # the next swings
    try:
        ensure_3d(False)
        alice("setsid -f weston-terminal > /tmp/weston-terminal.log 2>&1")
        c = wait_for("weston-terminal", lambda: next((c for c in json.loads(ctl("-j", "clients")) if c["title"] == "Wayland Terminal"), None), 20, 0.5)
        ensure_3d()
        menu_closed()
        ctl("hypr3d", "view", "first")
        ctl("hypr3d", "spawn")
        time.sleep(0.8)
        cls = c["class"]
        a = aim_find(cls)
        click("right", hold=0.05, after=1.0)
        pops = popups_of(cls)
        ctl("hypr3d", "turn", f"{st()['yaw'] + 180:.1f}", "12")
        time.sleep(0.6)
        s = st()
        n0 = attacks().get("swings", 0)
        click("left", hold=0.05, after=1.0)
        closed, n1 = not popups_of(cls), attacks().get("swings", 0)
        click("left", hold=0.05, after=0.4)
        n2 = attacks().get("swings", 0)
        check("33", "a window's menu open (weston-terminal's, which grabs the pointer), the crosshair on nothing: a left click closes the menu, no swing; the next click swings",
              a and pops and s["aimed"] is None and closed and n1 == n0 and n2 == n0 + 1,
              f"class {cls!r}, aimed {a and a.get('class')}; popups {[p['box'] for p in pops]}; then aimed {s['aimed']}; closed {closed}; swings {n0}, {n1}, {n2}")
    except Exception as e:  # noqa: BLE001
        check("33", "(a window's menu: stopped)", False, f"{type(e).__name__}: {e}"[:500])
    machine.execute("pkill -x weston-terminal; pkill -f weston-; true")
    ctl("hypr3d", "view", "third")

    ensure_avatar(TOON)
    aim_nothing()
    time.sleep(0.5)
    click("left", hold=0.05, after=0.6)
    r = ctl("hypr3d", "avatar", "attack")
    check("33", "an avatar that isn't a humanoid (ToonTest.glb): a click on nothing swings nothing, Hyprland fine; hyprctl hypr3d avatar attack says it has no arms",
          alive() and attacks().get("swings", 0) == 0 and "no arms to swing" in r, f"{attacks()}; {r}")
    ensure_avatar(AV)
    ensure_3d(False)


@section("28", "typing (E): into the window under the crosshair, and said so; walking again when that window closes or another takes the keyboard; E with nothing to type into stays walking")
def s_typing():
    lua_session()
    ensure_plugin()
    ensure_3d(False)
    clean_windows()
    machine.execute("pkill -u alice foot; true")
    wait_for("no windows", lambda: not json.loads(ctl("-j", "clients")), 10, 0.3)
    ensure_3d()
    menu_closed()
    ctl("hypr3d", "view", "first")
    ctl("hypr3d", "spawn")
    time.sleep(0.8)

    def walks():
        """how far W takes you in 0.6 s"""
        feet = st()["feet"]
        press("w", hold=0.6, after=0.5)
        return math.dist(feet, st()["feet"])

    press("e")
    s, told = st(), ctl("hypr3d", "log", "10")
    moved = walks()
    check("28", "E with no window to type into: says to point at one, and W still walks",
          s["typing"] is False and "point the crosshair at a window to type into it" in told and moved > 0.3, f"typing {s['typing']}, moved {moved:.2f} m; {told.strip()[-160:]}")
    ctl("hypr3d", "spawn")
    time.sleep(0.5)
    yaw0 = st()["yaw"]
    for cls, dy in (("h3d-type1", -35), ("h3d-type2", 35)):
        ctl("hypr3d", "turn", f"{yaw0 + dy:.1f}", "0")
        time.sleep(0.4)
        alice(f"setsid -f foot --app-id {cls} > /dev/null 2>&1")
        wait_for(f"{cls} in the world", lambda: placed(cls), 20, 0.5)
    time.sleep(1)
    a = aim_find("h3d-type1")
    press("e")
    s, told = st(), ctl("hypr3d", "log", "10")
    check("28", "E at a terminal: typing into it, and a notification says how to stop (Super+Esc)",
          a and s["typing"] is True and "Super+Esc to walk again" in told, f"aimed {a}; typing {s['typing']}; {told.strip()[-160:]}")
    machine.execute("rm -f /tmp/h3d-sent")
    type_text("touch /tmp/h3d-sent")
    frame("typed-into-terminal")
    press("ret")
    time.sleep(0.8)
    check("28", "... what's typed reaches it, and it's typing still", machine.execute("test -e /tmp/h3d-sent")[0] == 0 and st()["typing"] is True, st()["typing"])
    # on close Hyprland focuses the other terminal; typing must not follow
    type_text("exit")
    press("ret")
    wait_for("it closed", lambda: not placed("h3d-type1"), 10, 0.3)
    time.sleep(0.3)
    s, told = st(), ctl("hypr3d", "log", "10")
    moved = walks()
    frame("typing-window-closed")
    check("28", "... it closed (exit): walking again, W walks (not typed into the other terminal)",
          s["typing"] is False and "typing ended: the window left the 3D view" in told and moved > 0.3, f"typing {s['typing']}, moved {moved:.2f} m; {told.strip()[-200:]}")
    ctl("hypr3d", "spawn")
    time.sleep(0.5)
    a = aim_find("h3d-type2")
    press("e")
    typing = st()["typing"]
    alice("setsid -f foot --app-id h3d-type3 > /dev/null 2>&1")
    wait_for("the new terminal", lambda: client("h3d-type3"), 20, 0.5)
    time.sleep(0.6)
    s, told = st(), ctl("hypr3d", "log", "10")
    active = json.loads(ctl("-j", "activewindow")).get("class")
    check("28", "typing into the other, a new window takes the keyboard: walking again",
          a and typing is True and active == "h3d-type3" and s["typing"] is False and "typing ended: another window has the keyboard" in told,
          f"aimed {a and a.get('class')}; typing {typing} -> {s['typing']}; active {active}; {told.strip()[-200:]}")
    machine.execute("pkill -u alice foot; true")
    ensure_3d(False)


# ------------------------------------------------------------------ tiling mode: the windows side by side round you

TILE_GAP = 0.04  # radians between windows, as in tiling.hpp


def ring_view(w=None):
    """the row seen from the ring's middle, left to right: class, rel (yaw from the row's middle), half (half its
    angle), dist, faces (1 = straight at the middle), p"""
    w = w or windows3d()
    t = w["tiling"]
    c, yaw0 = t["center"], math.radians(t["yaw"])
    out = []
    for addr in t["row"]:
        p = next((p for p in w["placed"] if p["address"] == addr), None)
        if not p:
            continue
        dx, dz = p["center"][0] - c[0], p["center"][2] - c[2]
        d = math.hypot(dx, dz)
        rel = (math.atan2(dx, -dz) - yaw0 + math.pi) % (2 * math.pi) - math.pi
        out.append({"class": p["class"], "rel": rel, "half": math.atan(p["width"] / 2 / max(d, 1e-3)), "dist": d,
                    "faces": -(p["normal"][0] * dx + p["normal"][2] * dz) / max(d, 1e-3), "p": p})
    return out


def ring_checks(what, w, item="29"):
    """checks the row: facing the ring's middle, within its radius, TILE_GAP apart, centred where you looked"""
    t, v = w["tiling"], ring_view(w)
    gaps = [(v[i + 1]["rel"] - v[i + 1]["half"]) - (v[i]["rel"] + v[i]["half"]) for i in range(len(v) - 1)]
    mid = (v[0]["rel"] - v[0]["half"] + v[-1]["rel"] + v[-1]["half"]) / 2 if v else 1
    check(item, f"{what}: each faces the ring's middle, {t['radius']:.1f} m out", v and all(x["faces"] > 0.999 and 0.3 < x["dist"] < t["radius"] + 0.02 for x in v),
          [(x["class"], round(x["dist"], 3), round(x["faces"], 4)) for x in v])
    check(item, f"{what}: side by side left to right, {TILE_GAP} rad apart, the row's middle where you looked", all(abs(g - TILE_GAP) < 0.006 for g in gaps) and abs(mid) < 0.01,
          f"gaps {[round(g, 4) for g in gaps]}, middle {mid:.4f}")
    return v


def tiled_row():
    return [x["class"] for x in ring_view()]


def aim_row(cls):
    """aims at a window anywhere round you (aim_find searches only the view)"""
    ctl("hypr3d", "aim", cls)
    time.sleep(0.3)
    return aim_find(cls)


@section("29", "tiling mode (T): the windows in the world and the wall's side by side round you, facing you, in the order they were round you; one opening goes in where you look; a window carried goes where it's put down (on a wall it stays there); X, Shift+T, the ring going with you (walking, not up with a jump, flying, a window opening and one put down up in the air, landing, third person, the boom, V), the Windows page, hyprctl, Lua, the config value, entering 3D tiled, third person; T again puts them back")
def s_tiling():
    lua_session()
    ensure_plugin()
    ensure_3d(False)
    clean_windows()
    machine.execute("pkill -u alice foot; true")
    wait_for("no windows", lambda: not json.loads(ctl("-j", "clients")), 10, 0.3)
    ctl("hypr3d", "tile", "off")
    ctl("hypr3d", "tile", "follow", "on")  # an earlier Y may have left it staying
    ctl("hypr3d", "reset-windows", "forget")
    alice("setsid -f foot --app-id h3d-wall > /dev/null 2>&1")
    wait_for("the wall's terminal", lambda: client("h3d-wall"), 20, 0.5)
    ensure_3d()
    menu_closed()
    ctl("hypr3d", "view", "first")
    ctl("hypr3d", "spawn")
    time.sleep(0.8)
    yaw0 = st()["yaw"]
    for cls, dy in (("h3d-t1", -60), ("h3d-t2", 0), ("h3d-t3", 60)):
        ctl("hypr3d", "turn", f"{yaw0 + dy:.1f}", "0")
        time.sleep(0.4)
        alice(f"setsid -f foot --app-id {cls} > /dev/null 2>&1")
        wait_for(f"{cls} in the world", lambda: placed(cls), 20, 0.5)
    ctl("hypr3d", "turn", f"{yaw0:.1f}", "0")
    time.sleep(0.5)
    before = {p["class"]: p["center"] for p in settled(10)["placed"]}
    frame("tiling-before")

    press("t")
    s, told = st(), ctl("hypr3d", "log", "6")
    w = settled(10)
    row = [x["class"] for x in ring_view(w)]
    check("29", "T: tiling, said so; the three in the world and the wall's in the row", s["tiling"] is True and w["tiling"]["on"] and sorted(row) == ["h3d-t1", "h3d-t2", "h3d-t3", "h3d-wall"]
          and "tiling: 4 windows round you" in told and all(p["tiled"] for p in w["placed"]), f"{row}; {told.strip()[-160:]}")
    check("29", "... in the order they were round you: the left one first, the right one last", row.index("h3d-t1") < row.index("h3d-t2") < row.index("h3d-t3"), row)
    t = w["tiling"]
    check("29", "... round your eye, 2 m out, the row's middle where you look", math.dist(t["center"], s["eye"]) < 0.02 and abs(t["radius"] - 2) < 0.001 and t["back"] == 0
          and abs(((t["yaw"] - s["yaw"] + 180) % 360) - 180) < 0.2, t)
    v = ring_checks("tiled", w)
    mh = logical_size()[1]
    sizes = [(x["class"], round((x["p"]["height"] / client(x["class"])["size"][1]) / (2 * x["dist"] * math.tan(HALF_FOV) / mh), 3), round(on_screen(x["class"]), 3)) for x in v]
    check("29", "... each as big as on your screen, facing it, made smaller to fit 85% of the view", all(abs(a - b) < 0.02 for _, a, b in sizes), sizes)
    frame("tiled")
    for dy in (-75, 75):
        ctl("hypr3d", "turn", f"{yaw0 + dy:.1f}", "0")
        time.sleep(0.5)
        frame(f"tiled-{'left' if dy < 0 else 'right'}")

    # a window opening while tiling
    v = ring_view()
    look = t["yaw"] + math.degrees((v[1]["rel"] + v[2]["rel"]) / 2)  # between the second and third
    ctl("hypr3d", "turn", f"{look:.1f}", "0")
    time.sleep(0.4)
    rel_look = (math.radians(look - t["yaw"]) + math.pi) % (2 * math.pi) - math.pi
    slot = sum(1 for x in v if x["rel"] < rel_look)
    alice("setsid -f foot --app-id h3d-t4 > /dev/null 2>&1")
    wait_for("h3d-t4 in the row", lambda: "h3d-t4" in tiled_row(), 20, 0.5)
    w = settled(10)
    row = [x["class"] for x in ring_view(w)]
    check("29", "a window opening while tiling goes into the row where you look (between the second and the third)", row.index("h3d-t4") == slot == 2 and len(row) == 5, f"{row}, slot {slot}")
    ring_checks("five", w)
    frame("tiled-five")

    a = aim_row("h3d-t1")
    press("g")
    row = tiled_row()
    check("29", "G on a window in the row: carrying it, out of the row", a and st()["holding"] is True and "h3d-t1" not in row and len(row) == 4, f"aimed {a and a['class']}; {row}")
    v = ring_view()
    ctl("hypr3d", "turn", f"{t['yaw'] + math.degrees(v[-1]['rel'] + v[-1]['half']) + 30:.1f}", "0")  # past the right end
    time.sleep(1.2)
    w = windows3d()
    check("29", "... carried past the row's right end: room left for it there", w["tiling"]["holdSlot"] == 4 and not w["hold"]["onWall"], f"{w['tiling']['holdSlot']}; {w['hold']}")
    frame("tiling-carried")
    press("g")
    w = settled(10)
    row = [x["class"] for x in ring_view(w)]
    check("29", "G puts it down in the air there: the row's right end now", st()["holding"] is False and row[-1] == "h3d-t1" and len(row) == 5, row)
    ring_checks("reordered", w)
    row0 = row
    aim_row("h3d-t2")
    press("g")
    rel(250, 0)
    time.sleep(0.8)
    press("esc")
    w = settled(10)
    check("29", "carried out of the row, Esc puts it back where it was in it (and 3D stays on)", st()["mode"] == "active" and tiled_row() == row0, f"{row0} -> {tiled_row()}")
    aim_row("h3d-t3")
    press("g")
    ctl("hypr3d", "turn", f"{yaw0:.1f}", "0")
    ctl("hypr3d", "hold", "6")
    time.sleep(1.2)
    on_wall = windows3d()["hold"]["onWall"]
    press("g")
    w = settled(10)
    p3 = next(p for p in w["placed"] if p["class"] == "h3d-t3")
    row = [x["class"] for x in ring_view(w)]
    check("29", "carried to a wall and put down: it stays there, out of the row, the row closing up", on_wall and "h3d-t3" not in row and not p3["tiled"] and len(row) == 4
          and p3["center"][2] < 0.1, f"on the wall {on_wall}; {row}; {p3['center']}")
    ring_checks("closed up", w)
    wall_spot = p3["center"]
    aim_row("h3d-t2")
    press("x")
    time.sleep(1.5)
    row = tiled_row()
    check("29", "X on a window in the row: back to the wall, and it stays there", "h3d-t2" not in row and not placed("h3d-t2") and len(row) == 3, row)

    ctl("hypr3d", "tp", "1.5", "0", "6")
    time.sleep(0.5)
    s = st()
    w = settled(10)
    t2 = w["tiling"]
    check("29", "put somewhere else: the ring's round you there at once, the row as it was", math.dist(t2["center"], s["eye"]) < 0.05
          and [x["class"] for x in ring_view(w)] == row, f"{t2}; eye {s['eye']}")
    ctl("hypr3d", "turn", f"{yaw0 + 150:.1f}", "0")
    time.sleep(0.8)
    press("shift", "t")
    s = st()
    w = settled(10)
    t2 = w["tiling"]
    check("29", "Shift+T: the row's middle where you look now, round you", math.dist(t2["center"], s["eye"]) < 0.05
          and abs(((t2["yaw"] - s["yaw"] + 180) % 360) - 180) < 0.2 and [x["class"] for x in ring_view(w)] == row, f"{t2}; eye {s['eye']} yaw {s['yaw']}")
    ring_checks("brought round", w)
    frame("tiling-here")

    # walking: the ring follows with no lag
    def way_round(w, before):
        """per window: bearing change since `before` (radians) and |eye distance - distance from the ring's middle|"""
        return [(x["class"], round(abs((x["rel"] - before[x["class"]] + math.pi) % (2 * math.pi) - math.pi), 4),
                 round(abs(x["p"]["distance"] - math.dist(x["p"]["center"], w["tiling"]["center"])), 3)) for x in ring_view(w) if x["class"] in before]

    ctl("hypr3d", "turn", "180", "0")  # into the yard: turning doesn't turn the row
    time.sleep(0.5)
    w = settled(10)
    round0 = {x["class"]: x["rel"] for x in ring_view(w)}
    ground0, feet0 = w["tiling"]["ground"], st()["feet"]
    ctl("hypr3d", "walk", "2.5")
    time.sleep(1.0)
    mid = []
    for _ in range(4):
        mid.append(windows3d())
        time.sleep(0.1)
    walked_off()
    centers = [m["tiling"]["center"] for m in mid]
    lag = [way_round(m, round0) for m in mid]
    check("29", "walking: the ring goes along with you, each window at once the same way round you as before (none lagging)",
          math.dist(centers[0], centers[-1]) > 0.1 and all(len(l) == len(round0) and all(a < 0.01 and d < 0.02 for _, a, d in l) for l in lag),
          f"the ring went {math.dist(centers[0], centers[-1]):.2f} m in the frames; {lag}")
    s = st()
    w = settled(10)
    t = w["tiling"]
    check("29", "... walked on 3 m or more: still round you, the row as it was", math.dist(s["feet"], feet0) > 3 and math.dist(t["center"], s["eye"]) < 0.05
          and all(a < 0.01 for _, a, _ in way_round(w, round0)) and [x["class"] for x in ring_view(w)] == row, f"walked {math.dist(s['feet'], feet0):.2f} m; {t}; eye {s['eye']}")
    ring_checks("walked with you", w)
    frame("tiling-walked")

    # a jump doesn't lift the ring; flying does
    ground0 = t["ground"]
    ctl("hypr3d", "jump")
    hop, end = [], time.time() + 1.2
    while time.time() < end:
        hop.append((st()["feet"][1], windows3d()["tiling"]["ground"]))
    top = max(f for f, _ in hop)
    # landing: the ring follows the smoothed eye height down, a few cm at most with 50 ms frames (see tile_unit)
    check("29", "a jump: the ring doesn't go up with you (at most a landing's settle)", top > ground0 + 0.4 and max(g for _, g in hop) < ground0 + 0.1,
          f"feet up to {top - ground0:.2f} m, the ring's ground up to {max(g for _, g in hop) - ground0:.3f} m")
    wait_for("down from the jump", lambda: st()["onGround"], 10, 0.2)
    ctl("hypr3d", "fly")
    qmp([key_event("spc", True)])  # held until above the 5.55 m yard walls
    try:
        wait_for("up over the yard's walls", lambda: st()["feet"][1] > ground0 + 6.5, 20, 0.1)
    finally:
        qmp([key_event("spc", False)])
    time.sleep(1.0)
    s = st()
    w = settled(10)
    t = w["tiling"]
    v = ring_checks("up in the air", w)
    check("29", "flying up: the ring goes up with you, round you where you are in the air, the row as it was",
          s["fly"] and s["feet"][1] > ground0 + 6 and abs(t["ground"] - s["feet"][1]) < 0.03 and math.dist(t["center"], s["eye"]) < 0.05
          and all(a < 0.01 for _, a, _ in way_round(w, round0)) and all(x["p"]["center"][1] > s["feet"][1] for x in v),
          f"feet {s['feet']}; {t}; {[(x['class'], x['p']['center']) for x in v]}")
    frame("tiling-flying")

    alice("setsid -f foot --app-id h3d-t5 > /dev/null 2>&1")
    wait_for("h3d-t5 in the row", lambda: "h3d-t5" in tiled_row(), 20, 0.5)
    s = st()
    w = settled(10)
    p5 = next((x for x in ring_view(w) if x["class"] == "h3d-t5"), None)
    check("29", "... a window opening up there: in the row round you, in front of you", p5 and p5["dist"] < w["tiling"]["radius"] + 0.02
          and p5["p"]["center"][1] > s["feet"][1] and p5["p"]["apparent"] > 0.3, p5 and {k: p5[k] for k in ("dist", "rel")} | {"apparent": p5["p"]["apparent"], "center": p5["p"]["center"]})
    a = aim_row("h3d-t1")
    press("g")
    ctl("hypr3d", "turn", f"{st()['yaw'] + 100:.1f}", "0")
    time.sleep(1.0)
    on_wall = windows3d()["hold"]["onWall"]
    press("g")
    s = st()
    w = settled(10)
    p1 = next((x for x in ring_view(w) if x["class"] == "h3d-t1"), None)
    check("29", "... one carried and put down up there in the air: into the row round you", a and not on_wall and p1 and p1["dist"] < w["tiling"]["radius"] + 0.02
          and p1["p"]["center"][1] > s["feet"][1] and math.dist(w["tiling"]["center"], s["eye"]) < 0.05, f"aimed {a and a.get('class')}, on a wall {on_wall}; {p1 and p1['p']['center']}; {w['tiling']}")
    ring_checks("in the air", w)
    frame("tiling-flying-row")

    ctl("hypr3d", "fly")
    wait_for("landed", lambda: st()["onGround"], 10, 0.2)
    time.sleep(1.0)
    s = st()
    w = settled(10)
    t = w["tiling"]
    check("29", "out of the air: the ring comes down with you, round you on the ground", abs(t["ground"] - s["feet"][1]) < 0.03 and abs(s["feet"][1] - ground0) < 0.1
          and math.dist(t["center"], s["eye"]) < 0.05, f"feet {s['feet']}; {t}")
    ring_checks("landed", w)
    machine.execute("pkill -u alice -f 'app-id h3d-t5'; true")
    wait_for("h3d-t5 gone", lambda: "h3d-t5" not in tiled_row() and not client("h3d-t5"), 20, 0.5)
    row = tiled_row()

    press("b")
    time.sleep(0.8)
    m = menu()
    it = (m.get("items") or [{}])[0]
    check("29", "B, the Windows page: Tiling first, on, how many round you", it.get("label") == "Tiling" and it.get("on") is True and it.get("hint") == "on: 3 round you", it)
    t4 = placed("h3d-t4")["center"]
    press("1")
    time.sleep(0.3)
    s, told = st(), ctl("hypr3d", "log", "4")
    w = settled(10)
    pl = {p["class"]: p for p in w["placed"]}
    check("29", "... picked: tiling off, said so, the menu closed", s["tiling"] is False and not w["tiling"]["on"] and "tiling off" in told and menu().get("open") is False,
          f"{s['tiling']}; {told.strip()[-120:]}")
    check("29", "... the one from the world back where it was, the wall's back on the wall", "h3d-t1" in pl and math.dist(pl["h3d-t1"]["center"], before["h3d-t1"]) < 0.05
          and "h3d-wall" not in pl, f"{pl.get('h3d-t1', {}).get('center')} vs {before['h3d-t1']}; {sorted(pl)}")
    check("29", "... the one that opened meanwhile and the one on the wall stay where they are", math.dist(pl["h3d-t4"]["center"], t4) < 0.05
          and math.dist(pl["h3d-t3"]["center"], wall_spot) < 0.05 and not any(p["tiled"] for p in w["placed"]), f"{pl['h3d-t4']['center']} vs {t4}; {pl['h3d-t3']['center']} vs {wall_spot}")
    frame("untiled")

    # hyprctl, Lua, the config value
    r = [ctl("hypr3d", "tile", "on"), ctl("hypr3d", "tile", "on"), st()["tiling"], ctl("hypr3d", "tile", "toggle"), st()["tiling"], ctl("hypr3d", "tile", "here"),
         st()["tiling"], ctl("hypr3d", "tile", "off"), ctl("hypr3d", "tile", "sideways")]
    check("29", "hyprctl hypr3d tile on, on, toggle, here (not tiling: on), off, and a wrong word",
          r[:8] == ["tiling", "tiling", True, "not tiling", False, "tiling", True, "not tiling"] and r[8].startswith("error"), r)
    j = ctlj("hypr3d", "tile")
    check("29", "hyprctl hypr3d tile: how it is (JSON)", j["on"] is False and j["row"] == [] and j["holdSlot"] == -1, j)
    r1 = ctl("eval", "hl.plugin.hypr3d.tile()")
    on1 = st()["tiling"]
    r2 = ctl("eval", "hl.plugin.hypr3d.tile()")
    check("29", "hl.plugin.hypr3d.tile(): tiling on, and again off", r1 == "ok" and r2 == "ok" and on1 is True and st()["tiling"] is False, f"{r1} {on1}, {r2} {st()['tiling']}")
    ctl("eval", "hl.config({ plugin = { hypr3d = { tiling = true } } })")
    on2 = wait_for("tiling from the config", lambda: st()["tiling"], 4, 0.2)
    ctl("eval", "hl.config({ plugin = { hypr3d = { tiling = false } } })")
    off2 = wait_for("not tiling from the config", lambda: st()["tiling"] is False, 4, 0.2)
    check("29", "plugin:hypr3d:tiling set at run time: tiling on, and off again", on2 and off2, f"{on2} {off2}")

    ctl("hypr3d", "tile", "on")
    ensure_3d(False)
    ensure_3d()
    time.sleep(0.5)
    s = st()
    w = settled(10)
    check("29", "leaving 3D tiling and coming back: tiling still, round where you come in", s["tiling"] is True and math.dist(w["tiling"]["center"], s["eye"]) < 0.05
          and len(ring_view(w)) >= 3, f"{w['tiling']}; eye {s['eye']}")
    ring_checks("coming in", w)
    ctl("hypr3d", "tile", "off")

    # third person: the ring reaches 1 m past the boom, so the camera is inside it and sees every window's front
    ensure_avatar(AV)
    ctl("hypr3d", "view", "third", "2.6", "0.4")
    ctl("hypr3d", "spawn")
    time.sleep(1.2)
    ctl("hypr3d", "tile", "on")
    s = st()
    w = settled(10)
    t3 = w["tiling"]
    feet = s["feet"][1]
    check("29", "third person: round the avatar, 1 m past the camera's boom (2.6 m), for the camera's view (60%)", abs(t3["back"] - 2.6) < 0.001 and abs(t3["radius"] - 3.6) < 0.001
          and abs(t3["fit"] - 0.6) < 0.001 and math.dist(s["eye"], t3["center"]) < t3["radius"], t3)
    v = ring_checks("third person", w)
    check("29", "... every window standing on the ground (5 cm up or more)", all(x["p"]["center"][1] - x["p"]["height"] / 2 >= feet + 0.049 for x in v),
          [(x["class"], round(x["p"]["center"][1] - x["p"]["height"] / 2 - feet, 3)) for x in v])
    frame("tiled-third")
    # third person: the ring follows the avatar and refits to the boom
    round3 = {x["class"]: x["rel"] for x in v}
    ctl("hypr3d", "turn", "180", "0")  # into the yard
    time.sleep(0.5)
    feet3 = st()["feet"]
    ctl("hypr3d", "walk", "2")
    walked_off()
    s = st()
    w = settled(10)
    t3 = w["tiling"]
    check("29", "third person: walking, the ring goes with the avatar, the row as it was round it", math.dist(s["feet"], feet3) > 2
          and math.hypot(t3["center"][0] - s["feet"][0], t3["center"][2] - s["feet"][2]) < 0.05 and all(a < 0.01 for _, a, _ in way_round(w, round3)),
          f"walked {math.dist(s['feet'], feet3):.2f} m; {t3}; feet {s['feet']}; {way_round(w, round3)}")
    ring_checks("third person, walked", w)
    ctl("hypr3d", "view", "third", "4")
    time.sleep(0.5)
    w = settled(10)
    t3 = w["tiling"]
    check("29", "... the camera's boom made longer (4 m): the ring fits it, 1 m past it", abs(t3["back"] - 4) < 0.001 and abs(t3["radius"] - 5) < 0.001, t3)
    ring_checks("a longer boom", w)
    ctl("hypr3d", "view", "first")
    time.sleep(0.5)
    w = settled(10)
    t1 = w["tiling"]
    check("29", "... V to first person: the ring round your eye, 2 m out", t1["back"] == 0 and abs(t1["radius"] - 2) < 0.001 and math.dist(t1["center"], st()["eye"]) < 0.05, t1)
    ring_checks("first person again", w)
    ctl("hypr3d", "view", "third", "2.6", "0.4")
    ctl("hypr3d", "tile", "off")
    ctl("hypr3d", "view", "first")
    ctl("hypr3d", "reset-windows", "forget")
    machine.execute("pkill -u alice foot; true")
    ensure_3d(False)


def ring_look(s, t):
    """yaw from the row's middle (radians) where your level view crosses the ring, as the plugin's ringLookYaw"""
    c, yaw = t["center"], math.radians(s["yaw"])
    dx, dz = math.sin(yaw), -math.cos(yaw)
    x, z = s["eye"][0], s["eye"][2]
    ox, oz = x - c[0], z - c[2]
    b, cc = ox * dx + oz * dz, ox * ox + oz * oz - t["radius"] ** 2
    if b * b - cc >= 0 and -b + math.sqrt(b * b - cc) > 0:
        k = -b + math.sqrt(b * b - cc)
        x, z = x + dx * k, z + dz * k
    return (math.atan2(x - c[0], -(z - c[2])) - math.radians(t["yaw"]) + math.pi) % (2 * math.pi) - math.pi


@section("29s", "tiling mode's ring staying where it is (Y): said so, nothing moving; walking up to the row, the ring and every window staying put; at the ring a window opening goes into the row where you look; away from it one opening comes in front of you, one carried out there has no room in the row and put down in the air stays there (let go of, Esc, it's back in its place in the row), one brought and one played from another workspace stay out of the row, the ring not moving; Shift+T there brings it round you, still staying; Y again takes it with you (round your eye, the row the same way round you, the windows flying over); tiling ending leaves them there; hyprctl, the JSON, Lua, the config value, Y with tiling off, the Windows page's Follow me, entering 3D staying, third person's V and wheel not moving it, reset-windows still staying")
def s_tiling_stay():
    lua_session()
    ensure_plugin()
    ensure_3d(False)
    clean_windows()
    machine.execute("pkill -u alice foot; true")
    wait_for("no windows", lambda: not json.loads(ctl("-j", "clients")), 10, 0.3)
    ctl("hypr3d", "tile", "off")
    ctl("hypr3d", "tile", "follow", "on")
    ctl("hypr3d", "reset-windows", "forget")
    # a terminal on workspace 2, for Play to bring out
    ctl("dispatch", 'hl.dsp.focus({ workspace = "2" })')
    time.sleep(0.5)
    alice("setsid -f foot --app-id h3d-s6 > /dev/null 2>&1")
    wait_for("h3d-s6 on workspace 2", lambda: ((client("h3d-s6") or {}).get("workspace") or {}).get("id") == 2, 20, 0.5)
    ctl("dispatch", 'hl.dsp.focus({ workspace = "1" })')
    time.sleep(0.5)
    ensure_3d()
    menu_closed()
    ctl("hypr3d", "view", "first")
    ctl("hypr3d", "tp", "0", "0", "8")
    for cls, yaw in (("h3d-s1", 130), ("h3d-s2", 180), ("h3d-s3", 230)):
        ctl("hypr3d", "turn", f"{yaw}", "0")
        time.sleep(0.4)
        alice(f"setsid -f foot --app-id {cls} > /dev/null 2>&1")
        wait_for(f"{cls} in the world", lambda: placed(cls), 20, 0.5)
    ctl("hypr3d", "turn", "180", "0")
    time.sleep(0.5)
    before = {p["class"]: p["center"] for p in settled(10)["placed"]}
    press("t")
    told = ctl("hypr3d", "log", "6")
    w = settled(10)
    t0 = w["tiling"]
    check("29s", "T: tiling, going with you, said so (and that Y leaves it here)", t0["on"] and t0["follow"] is True and t0["atRing"] is True
          and "tiling: 3 windows round you, going with you" in told and "Y leaves it here" in told and tiled_row() == ["h3d-s1", "h3d-s2", "h3d-s3"],
          f"{t0}; {tiled_row()}; {told.strip()[-200:]}")

    ctl("hypr3d", "turn", "215", "0")
    time.sleep(0.5)
    w = settled(10)
    pre, turned = {p["class"]: p["center"] for p in w["placed"]}, w["tiling"]
    press("y")
    told = ctl("hypr3d", "log", "6")
    w = settled(10)
    ring0 = w["tiling"]
    went = max([math.dist(p["center"], pre[p["class"]]) for p in w["placed"] if p["class"] in pre] or [9])
    check("29s", "Y, turned away from the row's middle: the row stays here, said so; the ring where it was, its yaw too, and no window moved", ring0["follow"] is False
          and ring0["atRing"] is True and "the row stays here" in told and abs(turned["yaw"] - t0["yaw"]) < 0.01 and math.dist(ring0["center"], t0["center"]) < 0.001
          and abs(ring0["yaw"] - t0["yaw"]) < 0.01 and went < 0.005 and ctlj("hypr3d", "tile")["follow"] is False,
          f"{ring0} (T: {t0['yaw']}, turned: {turned['yaw']}); the windows moved up to {went:.3f} m; {told.strip()[-160:]}")
    frame("staying")

    # walking up to the row
    ctl("hypr3d", "turn", "180", "0")
    time.sleep(0.5)
    at0 = {p["class"]: p["center"] for p in w["placed"]}
    ahead = min(ring_view(w), key=lambda x: abs(x["rel"]))["class"]
    ctl("hypr3d", "walk", "0.8", "back")
    walked_off()
    s0, near0 = st(), placed(ahead)["distance"]
    ctl("hypr3d", "walk", "1.6")
    time.sleep(0.5)
    mid = []
    for _ in range(4):
        mid.append(windows3d())
        time.sleep(0.1)
    walked_off()
    s, w = st(), settled(10)
    moved = [max([math.dist(m["tiling"]["center"], ring0["center"])] + [math.dist(p["center"], at0[p["class"]]) for p in m["placed"]]) for m in mid + [w]]
    near = next(p for p in w["placed"] if p["class"] == ahead)["distance"]
    check("29s", "walking up to the row (2.5 m): the ring and every window stay where they are (none moving while you walk), the window ahead nearer",
          math.dist(s["feet"], s0["feet"]) > 2 and all(d < 0.02 for d in moved) and near < near0 - 1.5 and w["tiling"]["atRing"] is True,
          f"walked {math.dist(s['feet'], s0['feet']):.2f} m; moved at most {[round(d, 3) for d in moved]}; {ahead} {near0:.2f} -> {near:.2f} m")
    frame("staying-walked-up")

    ctl("hypr3d", "turn", "215", "0")
    time.sleep(0.5)
    s, w = st(), windows3d()
    look = ring_look(s, w["tiling"])
    slot = sum(1 for x in ring_view(w) if x["rel"] < look)
    alice("setsid -f foot --app-id h3d-s4 > /dev/null 2>&1")
    wait_for("h3d-s4 in the row", lambda: "h3d-s4" in tiled_row(), 20, 0.5)
    w = settled(10)
    row = [x["class"] for x in ring_view(w)]
    check("29s", "in the ring: a window opening goes into the row where you look, the ring where it was", row.index("h3d-s4") == slot and len(row) == 4
          and math.dist(w["tiling"]["center"], ring0["center"]) < 0.001, f"{row}, slot {slot} (looking {look:.3f} rad from the row's middle); {w['tiling']['center']}")
    ring_checks("staying, four", w, "29s")

    ctl("hypr3d", "tp", "0", "0", "8")
    ctl("hypr3d", "turn", "180", "0")
    time.sleep(0.5)
    a = aim_row("h3d-s1")
    press("g")
    time.sleep(0.5)
    slot_in = windows3d()["tiling"]["holdSlot"]
    ctl("hypr3d", "tp", "-4.5", "0", "11")
    ctl("hypr3d", "turn", "180", "0")
    ctl("hypr3d", "hold", "2")
    time.sleep(1.2)
    s, w = st(), windows3d()
    t = w["tiling"]
    check("29s", "a row window carried out of the ring: room made for it in the row at the ring, none out there", a and s["holding"] is True and slot_in >= 0
          and t["holdSlot"] == -1 and w["hold"] and not w["hold"]["onWall"] and t["atRing"] is False and "h3d-s1" not in tiled_row(),
          f"aimed {a and a.get('class')}; at the ring {slot_in}, out there {t['holdSlot']}; {w['hold']}; {t}")
    frame("staying-carried-away")
    carried = placed("h3d-s1")["center"]
    press("g")
    s, w = st(), settled(10)
    p1 = next(p for p in w["placed"] if p["class"] == "h3d-s1")
    check("29s", "... put down in the air out there: it stays there, out of the row; the ring where it was", s["holding"] is False and not p1["tiled"]
          and "h3d-s1" not in tiled_row() and math.dist(p1["center"], carried) < 0.25 and abs(relative(p1, s)[0] - 2) < 0.3
          and math.dist(w["tiling"]["center"], ring0["center"]) < 0.001, f"{p1['center']} (carried at {carried}); {relative(p1, s)}; {w['tiling']}")
    ctl("hypr3d", "tp", "0", "0", "8")
    ctl("hypr3d", "turn", "180", "0")
    time.sleep(0.5)
    w = settled(10)
    row, at2 = tiled_row(), next(p for p in w["placed"] if p["class"] == "h3d-s2")["center"]
    a = aim_row("h3d-s2")
    press("g")
    time.sleep(0.5)
    ctl("hypr3d", "tp", "-4.5", "0", "11")
    ctl("hypr3d", "turn", "150", "0")
    time.sleep(1.2)
    s, t = st(), windows3d()["tiling"]
    if s["holding"]:  # Esc with nothing carried would leave 3D
        press("esc")
    s2, w = st(), settled(10)
    p2 = next(p for p in w["placed"] if p["class"] == "h3d-s2")
    check("29s", "... one carried out there and let go of (Esc): back in its place in the row", a and s["holding"] is True and t["holdSlot"] == -1 and t["atRing"] is False
          and s2["holding"] is False and s2["mode"] == "active" and p2["tiled"] and tiled_row() == row and math.dist(p2["center"], at2) < 0.02,
          f"aimed {a and a.get('class')}; out there {t['holdSlot']}; {row} -> {tiled_row()}; {p2['center']} (was {at2})")

    ctl("hypr3d", "turn", "140", "0")
    time.sleep(0.5)
    alice("setsid -f foot --app-id h3d-s5 > /dev/null 2>&1")
    wait_for("h3d-s5 in the world", lambda: placed("h3d-s5"), 20, 0.5)
    s, w = st(), settled(10)
    p5 = next(p for p in w["placed"] if p["class"] == "h3d-s5")
    check("29s", "away from the ring: a window opening comes in front of you, not into the row; the ring where it was", not p5["tiled"] and "h3d-s5" not in tiled_row()
          and relative(p5, s)[0] > 0.3 and off_middle(p5, s) < 20 and math.dist(w["tiling"]["center"], ring0["center"]) < 0.001 and abs(w["tiling"]["yaw"] - ring0["yaw"]) < 0.01,
          f"{relative(p5, s)}; {p5['center']}; {w['tiling']}")
    ctl("hypr3d", "turn", "220", "0")
    time.sleep(0.5)
    r = ctl("hypr3d", "window", "h3d-s3", "bring")
    s, w = st(), settled(10)
    p3 = next(p for p in w["placed"] if p["class"] == "h3d-s3")
    check("29s", "... Bring here out there: in front of you, out of the row; the ring where it was", r == "here" and not p3["tiled"] and "h3d-s3" not in tiled_row()
          and relative(p3, s)[0] > 0.3 and off_middle(p3, s) < 20 and math.dist(w["tiling"]["center"], ring0["center"]) < 0.001, f"{r}; {relative(p3, s)}; {w['tiling']}")
    ctl("hypr3d", "turn", "100", "0")
    time.sleep(0.5)
    r = ctl("hypr3d", "window", "h3d-s6", "play")  # brought out, not played yet: "try again"
    wait_for("h3d-s6 in the world", lambda: placed("h3d-s6"), 10, 0.3)
    ctl("hypr3d", "play", "off")
    s, w = st(), settled(10)
    p6 = next(p for p in w["placed"] if p["class"] == "h3d-s6")
    check("29s", "... Play on one from another workspace out there: out here in front of you first, not into the row; the ring where it was", not p6["tiled"]
          and "h3d-s6" not in tiled_row() and relative(p6, s)[0] > 0.3 and math.dist(w["tiling"]["center"], ring0["center"]) < 0.001, f"{r}; {relative(p6, s)}; {w['tiling']}")
    frame("staying-away")

    row = tiled_row()
    ctl("hypr3d", "turn", "150", "0")
    time.sleep(0.5)
    press("shift", "t")
    s, w = st(), settled(10)
    ring1 = w["tiling"]
    check("29s", "Shift+T away from the ring: the ring round you, its middle where you look, the row as it was, still staying", math.dist(ring1["center"], s["eye"]) < 0.05
          and abs(((ring1["yaw"] - s["yaw"] + 180) % 360) - 180) < 0.2 and ring1["follow"] is False and ring1["atRing"] is True and [x["class"] for x in ring_view(w)] == row,
          f"{ring1}; eye {s['eye']} yaw {s['yaw']}; {row}")
    ring_checks("brought round you", w, "29s")
    frame("staying-here")

    # tile follow on (Y) elsewhere: the ring centres on your eye at once, yaw kept, and the windows fly over (one
    # --batch request sees the state before any frame; a poll after a frame sees them on the way)
    ctl("hypr3d", "tp", "2", "0", "7")
    ctl("hypr3d", "turn", "250", "0")
    time.sleep(0.5)
    was = {p["class"]: p["center"] for p in settled(10)["placed"] if p["tiled"]}
    t_y = time.time()
    got = ctl("--batch", "hypr3d tile follow on ; hypr3d windows ; hypr3d status").split("\n\n\n")
    first, took, framed = None, time.time() - t_y, False
    while not framed and time.time() - t_y < 0.25:
        first = windows3d()
        took = time.time() - t_y
        framed = any(math.dist(p["center"], was[p["class"]]) > 0.05 for p in first["placed"] if p["class"] in was)
    told = ctl("hypr3d", "log", "6")
    s, w = st(), settled(10)
    t = w["tiling"]
    try:
        w0, s0 = json.loads(got[1]), json.loads(got[2])
    except (IndexError, ValueError):
        w0, s0 = {"placed": [], "tiling": {}}, {}
    t1 = w0["tiling"]
    check("29s", "Y again (tile follow on): going with you, the ring round your eye at once, no window moved yet (they fly over, not jump)", got[0] == "following"
          and t1.get("follow") is True and "eye" in s0 and math.dist(t1["center"], s0["eye"]) < 0.05 and was and len(was) == len(row)
          and all(math.dist(p["center"], was[p["class"]]) < 0.02 for p in w0["placed"] if p["class"] in was), f"{got[0]!r}; {t1}; eye {s0.get('eye')}; {was}")
    end = {p["class"]: p["center"] for p in w["placed"]}
    flying = [p["class"] for p in (first or {"placed": []})["placed"] if p["class"] in was and not p["settled"]
              and math.dist(p["center"], was[p["class"]]) > 0.05 and math.dist(p["center"], end[p["class"]]) > 0.05]
    if framed and took < 0.25:  # eased over 7.6 m at rate 18/s: still >8 cm away after 0.25 s
        check("29s", f"... {took:.2f} s on, after a frame: every window of the row on its way over, neither where it was nor there yet", was and len(flying) == len(was),
              f"{flying} of {sorted(was)}")
    else:
        note("29s", "... a moment on: too late to tell whether the windows fly over (or no frame yet)", f"{took:.2f} s")
    check("29s", "... said so; settled round your eye, the row the same way round you (its yaw kept)", t["follow"] is True
          and "the row goes with you again" in told and math.dist(t["center"], s["eye"]) < 0.05 and abs(((t["yaw"] - ring1["yaw"] + 180) % 360) - 180) < 0.01
          and [x["class"] for x in ring_view(w)] == row, f"{t}; eye {s['eye']}; the yaw was {ring1['yaw']}; {told.strip()[-120:]}")
    v = ring_checks("with you again", w, "29s")
    frame("staying-with-you-again")
    rel0, feet0 = {x["class"]: x["rel"] for x in v}, s["feet"]
    ctl("hypr3d", "walk", "1.2")
    walked_off()
    s, w = st(), settled(10)
    t = w["tiling"]
    check("29s", "... walking: the ring goes along with you, each window the same way round you", math.dist(s["feet"], feet0) > 1 and math.dist(t["center"], s["eye"]) < 0.05
          and all(abs((x["rel"] - rel0[x["class"]] + math.pi) % (2 * math.pi) - math.pi) < 0.01 for x in ring_view(w)),
          f"walked {math.dist(s['feet'], feet0):.2f} m; {t}; eye {s['eye']}")

    kept = {p["class"]: p["center"] for p in w["placed"]}
    ctl("hypr3d", "tile", "off")
    w = settled(10)
    pl = {p["class"]: p["center"] for p in w["placed"]}
    check("29s", "tiling off: the ones put down, brought, opened and played away from the ring stay where they are (and the one opened in it), the one from before goes back",
          all(c in pl and math.dist(pl[c], kept[c]) < 0.05 for c in ("h3d-s1", "h3d-s3", "h3d-s4", "h3d-s5", "h3d-s6")) and "h3d-s2" in pl and math.dist(pl["h3d-s2"], before["h3d-s2"]) < 0.05,
          {c: (pl[c], before["h3d-s2"] if c == "h3d-s2" else kept.get(c)) for c in pl})

    # hyprctl, the JSON, Lua, the config value
    r = [ctl("hypr3d", "tile", "follow", "off"), ctl("hypr3d", "tile", "follow", "off"), ctlj("hypr3d", "tile")["follow"], ctl("hypr3d", "tile", "follow", "toggle"),
         ctlj("hypr3d", "tile")["follow"], ctl("hypr3d", "tile", "follow", "on"), ctl("hypr3d", "tile", "follow"), ctlj("hypr3d", "tile")["follow"],
         ctl("hypr3d", "tile", "follow"), ctl("hypr3d", "tile", "follow", "sideways"), ctl("hypr3d", "tile", "sideways")]
    check("29s", "hyprctl hypr3d tile follow off, off, toggle, on, none (toggle) twice, and a wrong word; tile and a wrong word",
          r[:9] == ["staying", "staying", False, "following", True, "following", "staying", False, "following"] and r[9] == "error: tile follow [on|off|toggle]"
          and r[10] == "error: tile [on|off|toggle|here|follow]", r)
    j = ctlj("hypr3d", "tile")
    ctl("hypr3d", "tile", "on")
    j2, jw = ctlj("hypr3d", "tile"), windows3d()["tiling"]
    ctl("hypr3d", "tile", "follow", "off")
    j3 = ctlj("hypr3d", "tile")
    check("29s", "hyprctl hypr3d tile (and windows): follow and atRing (off: not at it; going with you: at it; staying, in it: at it)",
          (j["follow"], j["atRing"], j2["follow"], j2["atRing"], jw["follow"], jw["atRing"], j3["follow"], j3["atRing"]) == (True, False, True, True, True, True, False, True),
          f"{j}; {j2}; {j3}")
    r1 = ctl("eval", 'local r = hl.plugin.hypr3d.tile("follow"); if r ~= "following" then error("gave " .. tostring(r)) end')
    f1 = ctlj("hypr3d", "tile")["follow"]
    r2 = ctl("eval", 'local r = hl.plugin.hypr3d.tile("follow"); if r ~= "staying" then error("gave " .. tostring(r)) end')
    f2 = ctlj("hypr3d", "tile")["follow"]
    check("29s", 'hl.plugin.hypr3d.tile("follow"): going with you, and again staying (saying so)', r1 == "ok" and r2 == "ok" and f1 is True and f2 is False, f"{r1} {f1}, {r2} {f2}")
    ctl("hypr3d", "turn", "30", "0")
    time.sleep(0.5)
    r3 = ctl("eval", 'local r = hl.plugin.hypr3d.tile("here"); if r ~= "tiling here" then error("gave " .. tostring(r)) end')
    s, t = st(), ctlj("hypr3d", "tile")
    r4 = ctl("eval", "hl.plugin.hypr3d.tile()")
    check("29s", 'hl.plugin.hypr3d.tile("here"): the ring round you, its middle where you look (Shift+T); tile(): tiling off', r3 == "ok" and math.dist(t["center"], s["eye"]) < 0.05
          and abs(((t["yaw"] - s["yaw"] + 180) % 360) - 180) < 0.2 and t["follow"] is False and r4 == "ok" and st()["tiling"] is False, f"{r3}; {t}; eye {s['eye']} yaw {s['yaw']}; {r4}")
    ctl("hypr3d", "tile", "follow", "on")
    ctl("eval", "hl.config({ plugin = { hypr3d = { tiling_follow = false } } })")
    off = wait_for("staying from the config", lambda: ctlj("hypr3d", "tile")["follow"] is False, 4, 0.2)
    ctl("eval", "hl.config({ plugin = { hypr3d = { tiling_follow = true } } })")
    on = wait_for("going with you from the config", lambda: ctlj("hypr3d", "tile")["follow"] is True, 4, 0.2)
    check("29s", "plugin:hypr3d:tiling_follow set at run time: staying, and going with you again", off and on, f"{off} {on}")

    press("y")
    told, f = ctl("hypr3d", "log", "4"), ctlj("hypr3d", "tile")["follow"]
    check("29s", "Y with tiling off: the row will stay where you turn it on, said so", f is False and st()["tiling"] is False and "will stay where you turn it on (T)" in told,
          f"{f}; {told.strip()[-120:]}")
    press("t")
    told = ctl("hypr3d", "log", "6")
    s, w = st(), settled(10)
    t = w["tiling"]
    ctl("hypr3d", "walk", "0.8", "back")
    walked_off()
    t2 = windows3d()["tiling"]
    check("29s", "... T: tiling round you, staying (said so), and it stays there as you walk", s["tiling"] is True and t["follow"] is False and "round you, staying here" in told
          and "Y takes it with you" in told and math.dist(t["center"], s["eye"]) < 0.05 and math.dist(t2["center"], t["center"]) < 0.001 and math.dist(st()["feet"], s["feet"]) > 0.8,
          f"{t}; eye {s['eye']}; after walking {t2['center']}; {told.strip()[-200:]}")

    press("b")
    time.sleep(0.8)
    items = menu().get("items") or []
    it = items[1] if len(items) > 1 else {}
    check("29s", "B, the Windows page, tiling: Follow me second, off, staying here (Y)", it.get("label") == "Follow me" and it.get("on") is False and it.get("hint") == "staying here (Y)",
          items[:2])
    press("2")
    time.sleep(0.3)
    s, told = st(), ctl("hypr3d", "log", "4")
    t = settled(10)["tiling"]
    check("29s", "... picked: going with you again, said so, the menu closed, the ring round you", t["follow"] is True and "the row goes with you again" in told
          and menu().get("open") is False and math.dist(t["center"], s["eye"]) < 0.05, f"{t}; eye {s['eye']}; {told.strip()[-120:]}")

    ctl("hypr3d", "tile", "follow", "off")
    ensure_3d(False)
    ensure_3d()
    time.sleep(0.5)
    s, w = st(), settled(10)
    t = w["tiling"]
    ctl("hypr3d", "walk", "0.8", "back")
    walked_off()
    t2 = windows3d()["tiling"]
    check("29s", "leaving 3D staying and coming back: tiling, still staying, round where you come in (and it stays there as you walk)", s["tiling"] is True
          and t["follow"] is False and math.dist(t["center"], s["eye"]) < 0.05 and math.dist(t2["center"], t["center"]) < 0.001 and math.dist(st()["feet"], s["feet"]) > 0.8,
          f"{t}; eye {s['eye']}; after walking {t2['center']}")
    ring_checks("coming in staying", w, "29s")

    # staying, third person: V and the wheel's boom move nothing
    ensure_avatar(AV)
    ctl("hypr3d", "view", "first")
    time.sleep(0.5)
    w = settled(10)
    t, at = w["tiling"], {p["class"]: p["center"] for p in w["placed"]}

    def same(w):
        """largest change in the ring (center, radius, back, fit) or its windows' positions"""
        u = w["tiling"]
        return max([math.dist(u["center"], t["center"]), abs(u["radius"] - t["radius"]), abs(u["back"] - t["back"]), abs(u["fit"] - t["fit"])]
                   + [math.dist(p["center"], at[p["class"]]) for p in w["placed"] if p["class"] in at])

    press("v")
    wait_for("third person", lambda: st()["view"] == "third", 5, 0.2)
    time.sleep(0.8)
    d1 = same(settled(10))
    s = st()
    ctl("hypr3d", "turn", f"{s['yaw']:.1f}", "-30")  # aim at the ground so the wheel zooms the camera
    time.sleep(0.5)
    boom = av()["distance"]
    wheel(3)
    time.sleep(1.0)
    boom2 = av()["distance"]
    d2 = same(settled(10))
    ctl("hypr3d", "view", "third", "4")  # the wheel's longest boom, whatever is aimed at
    time.sleep(1.0)
    boom3 = av()["distance"]
    d3 = same(settled(10))
    frame("staying-third")
    ctl("hypr3d", "turn", f"{s['yaw']:.1f}", "0")
    press("v")
    wait_for("first person", lambda: st()["view"] == "first", 5, 0.2)
    time.sleep(0.5)
    d4 = same(settled(10))
    check("29s", "third person while staying: V, the wheel and a longer boom, and V back move neither the ring nor its windows", abs(boom3 - boom) > 0.3
          and max(d1, d2, d3, d4) < 0.02, f"moved {d1:.3f}, {d2:.3f}, {d3:.3f}, {d4:.3f} m; the boom {boom:.2f}, after the wheel {boom2:.2f}, then {boom3:.2f} m")
    ctl("hypr3d", "view", "third", "2.6", "0.4")
    ctl("hypr3d", "view", "first")
    r = ctl("hypr3d", "reset-windows")
    j = ctlj("hypr3d", "tile")
    check("29s", "reset-windows while staying: tiling off, still staying", r == "ok" and j["on"] is False and j["follow"] is False, f"{r}; {j}")
    ctl("hypr3d", "tile", "off")
    ctl("hypr3d", "tile", "follow", "on")
    ctl("hypr3d", "reset-windows", "forget")
    machine.execute("pkill -u alice foot; true")
    ensure_3d(False)


# ------------------------------------------------------------------ a game fullscreen in 3D: the others stay, it's played

FS_BAR = f"{H}/topbar.qml"
FS_BG_LOG = "/tmp/game-bg.log"
FS_TK_LOG = "/tmp/tkfs.log"
FS_S2_LOG = "/tmp/fs-socket2.log"
FS = {"yaw0": 0.0}

# logs Hyprland's socket2 events, with arrival times, to FS_S2_LOG
FS_S2 = r'''
import glob, select, socket, sys, time
out = open(sys.argv[1], "a", buffering=1)
socks = []
for p in glob.glob("/run/user/1000/hypr/*/.socket2.sock"):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        s.connect(p)
        socks.append(s)
        out.write(f"{time.time():.4f} #connected {p}\n")
    except OSError:
        s.close()
buf = {s: b"" for s in socks}
while socks:
    for s in select.select(socks, [], [], 1.0)[0]:
        d = s.recv(65536)
        if not d:
            socks.remove(s)
            continue
        buf[s] += d
        while b"\n" in buf[s]:
            line, buf[s] = buf[s].split(b"\n", 1)
            out.write(f"{time.time():.4f} {line.decode(errors='replace')}\n")
'''


def fs_events_start():
    machine.execute("pkill -f /tmp/fs-socket2[.]py; true")
    machine.succeed(f"cat > /tmp/fs-socket2.py << 'H3D_EOF'\n{FS_S2}\nH3D_EOF\nchmod 644 /tmp/fs-socket2.py; rm -f {FS_S2_LOG}")
    alice(f"touch {FS_S2_LOG}; setsid -f python3 -u /tmp/fs-socket2.py {FS_S2_LOG} > /dev/null 2>&1")
    wait_for("Hyprland's events", lambda: "#connected" in machine.execute(f"cat {FS_S2_LOG}")[1], 10, 0.3)


def fs_events_mark(name):
    alice(f"echo {shlex.quote('0 ### ' + name)} >> {FS_S2_LOG}")


def fs_events(name):
    """Hyprland's events since the mark (titles left out)"""
    lines = machine.execute(f"cat {FS_S2_LOG}")[1].splitlines()
    at = max((i for i, l in enumerate(lines) if l.endswith(f"### {name}")), default=-1)
    return [e for e in (l.partition(" ")[2] for l in lines[at + 1:]) if e and not e.startswith(("###", "#connected", "windowtitle>>", "windowtitlev2>>"))]


def fs_bar(on=True):
    """a status bar on the top layer (topbar.qml: orange, 36 px tall)"""
    machine.execute("pkill -f 'quickshell -p .*topbar'; true")
    if on:
        alice(f"setsid -f quickshell -p {FS_BAR} > /tmp/topbar.log 2>&1")
        wait_for("the top bar", lambda: "h3d-topbar" in ctl("-j", "layers"), 30)
        time.sleep(0.5)


def fs_game(cls, args="", log=GAME_LOG):
    """starts h3dgame as alice logging to `log`, killing no other; returns its window"""
    alice(f"rm -f {log}; SDL_APP_ID={cls} SDL_VIDEO_WAYLAND_WMCLASS={cls} setsid -f stdbuf -oL h3dgame --title {cls} {args} >> {log} 2>&1")
    return wait_for(f"{cls}'s window", lambda: client(cls), 30, 0.2)


def fs_bg_rate():
    """the background game's last three per-second frame counts, and how many it printed"""
    lines = [l for l in machine.succeed(f"cat {FS_BG_LOG} 2>/dev/null || true").splitlines() if l.startswith("frames ")]
    return [int(l.split()[1]) for l in lines[-3:]], len(lines)


def fs_rates(secs=3):
    """the 3D view's fps once a second for `secs` s, then fs_bg_rate()"""
    view = []
    for _ in range(secs):
        time.sleep(1.05)
        view.append(round(st()["fps"]))
    return (view,) + fs_bg_rate()


def fs_kept_rate(view, game):
    """the background game keeps 80% of the 3D view's pace (Hyprland gives a window hidden under a fullscreen one 20
    fps); below 25 fps (llvmpipe) the shared CPU limits both, so only drawing at all counts"""
    if not view or not game:
        return False
    v = sum(view) / len(view)
    return v < 25 or sum(game) / len(game) >= 0.8 * v


def fs_active():
    return json.loads(ctl("-j", "activewindow") or "{}")


def fs_state(tag):
    """snapshot of panels, placed windows, row and clients; noted and saved to raw/fs-TAG.json"""
    s, w, p = st(), windows3d(), panels()
    cl = json.loads(ctl("-j", "clients") or "[]")
    act = fs_active()
    (RAW / f"fs-{tag}.json").write_text(json.dumps({"status": s, "windows": w, "panels": p, "clients": cl, "active": act}, indent=1))
    wins = [x for x in p if x["kind"] == "window"]
    row = [next((x["class"] for x in w["placed"] if x["address"] == a), a) for a in w["tiling"]["row"]]
    note(tag, "drawn: class alpha [out in the world]", ", ".join(f"{x['class']} {x['alpha']:.2f}{' placed' if x['placed'] else ''}" for x in wins) or "none")
    note(tag, "Hyprland: class fullscreen visible", "; ".join(f"{c['class']} fs{c['fullscreen']} vis={c['visible']}" for c in cl))
    note(tag, "status", f"playing {s['playing'] and s['playing'].get('class')}, typing {s['typing']}, active {act.get('class')}, tiling {w['tiling']['on']}, row {row}")
    return {"panels": {x["class"]: x for x in wins}, "placed": {x["class"]: x for x in w["placed"]}, "row": row, "clients": {c["class"]: c for c in cl},
            "status": s, "playing": (s["playing"] or {}).get("class"), "active": act.get("class"), "tiling": w["tiling"]["on"],
            "bar": any(x["kind"] == "layer" and x["box"][1] == 0 and x["box"][3] == 36 for x in p)}


def fs_setup(tiling, bg=True):
    """wall: tiled and floating terminals, with `bg` a background game; world: foot and xterm; a top bar; with `tiling`,
    T and one more terminal into the row"""
    lua_session()
    ensure_plugin()
    ensure_3d(False)
    clean_windows()
    machine.execute("pkill -u alice foot; pkill -f tkfs[.]py; true")
    wait_for("no windows", lambda: not json.loads(ctl("-j", "clients")), 10, 0.3)
    ctl("hypr3d", "tile", "off")
    ctl("hypr3d", "reset-windows", "forget")
    ctl("eval", 'hl.window_rule({ name = "h3d-fs-float", match = { class = "h3d-float|h3d-fshelp" }, float = true })')
    fs_bar(True)
    alice("setsid -f foot --app-id h3d-wall > /dev/null 2>&1")
    wait_for("the wall's terminal", lambda: client("h3d-wall"), 20, 0.5)
    alice("setsid -f foot --app-id h3d-float > /dev/null 2>&1")
    wait_for("the floating terminal", lambda: client("h3d-float"), 20, 0.5)
    if bg:
        fs_game("h3dgame-bg", log=FS_BG_LOG)
    time.sleep(1)
    ensure_3d()
    menu_closed()
    ctl("hypr3d", "view", "first")
    ctl("hypr3d", "spawn")
    time.sleep(0.8)
    FS["yaw0"] = yaw0 = st()["yaw"]
    for cls, dy, cmd in (("h3d-t1", -50, "foot --app-id h3d-t1"), ("h3dxterm", 50, f"env DISPLAY={x_display()} xterm -class h3dxterm")):
        ctl("hypr3d", "turn", f"{yaw0 + dy:.1f}", "0")
        time.sleep(0.4)
        alice(f"setsid -f {cmd} > /dev/null 2>&1")
        wait_for(f"{cls} in the world", lambda: placed(cls), 20, 0.5)
    ctl("hypr3d", "turn", f"{yaw0:.1f}", "0")
    time.sleep(0.5)
    settled(10)
    if tiling:
        press("t")
        time.sleep(0.5)
        settled(10)
        ctl("hypr3d", "turn", f"{yaw0 + 170:.1f}", "0")  # behind you: the row's end
        time.sleep(0.4)
        alice("setsid -f foot --app-id h3d-t2 > /dev/null 2>&1")
        wait_for("h3d-t2 in the row", lambda: "h3d-t2" in tiled_row(), 20, 0.5)
        ctl("hypr3d", "turn", f"{yaw0:.1f}", "0")
        settled(10)


def fs_playing(cls, timeout=4):
    """what's played once `cls` is (or after `timeout` s)"""
    try:
        return wait_for(f"{cls} played", lambda: (lambda p: p if p and p["class"] == cls else None)(st()["playing"]), timeout, 0.2)
    except TimeoutError:
        return st()["playing"]


def fs_keys(mark):
    """W (held) and 8: what the game got, and how far you walked"""
    game_mark(mark)
    feet = st()["feet"]
    press("w", hold=0.4)
    press("8")
    time.sleep(0.4)
    return [l for l in game_lines(mark) if l.startswith("key down")], math.dist(st()["feet"], feet)


def fs_kill(cls):
    """kills the class's windows by PID (by name would miss xterm's .xterm-wrapped)"""
    for c in json.loads(ctl("-j", "clients") or "[]"):
        if c["class"] == cls and c.get("pid", 0) > 1:
            machine.execute(f"kill {c['pid']} 2>/dev/null; true")
    wait_for(f"{cls} gone", lambda: not any(c["class"] == cls for c in json.loads(ctl("-j", "clients"))), 10, 0.3)


def fs_done():
    machine.execute("pkill -f 'title h3dgame'; pkill -x h3dgame; pkill -f tkfs[.]py; pkill -f /tmp/fs-socket2[.]py; true")
    ctl("hypr3d", "play", "off")
    ctl("hypr3d", "tile", "off")
    ctl("hypr3d", "reset-windows", "forget")
    fs_bar(False)
    clean_windows()


def fs_plain(inside=True):
    """no windows, tiling off, h3d-fshelp set to float; with `inside`, in 3D in first person at the spawn"""
    lua_session()
    ensure_plugin()
    ensure_3d(False)
    clean_windows()
    machine.execute("pkill -u alice foot; pkill -f tkfs[.]py; true")
    wait_for("no windows", lambda: not json.loads(ctl("-j", "clients")), 10, 0.3)
    ctl("hypr3d", "tile", "off")
    ctl("hypr3d", "reset-windows", "forget")
    ctl("eval", 'hl.window_rule({ name = "h3d-fs-float", match = { class = "h3d-float|h3d-fshelp" }, float = true })')
    if inside:
        ensure_3d()
        menu_closed()
        ctl("hypr3d", "view", "first")
        ctl("hypr3d", "spawn")
        time.sleep(0.8)


def fs_title(playing):
    """the title of what's played (st()'s "playing"), "" for none"""
    return (playing or {}).get("title") or ""


@section("31", "a game going fullscreen in 3D: the other windows stay where they are, drawn (tiling mode or not, maximized too), and it's played by itself (opening fullscreen, a helper window holding the keyboard, focus gone, a click later, a launcher closing; a dialog of its own keeping the keyboard; a fullscreen dialog played itself; not after the mouse was away); T twice while it's fullscreen; entering 3D with it fullscreen")
def s_fullscreen():
    # ---- tiling: a game that opens fullscreen (set_fullscreen before its first commit: fullscreen as it maps, before
    # it's in the row)
    fs_setup(True)
    time.sleep(2.5)
    A = fs_state("31a")
    bg0, n0 = fs_bg_rate()
    fs_game("h3dgame", "--fullscreen")
    p = fs_playing("h3dgame")
    time.sleep(1.0)
    B = fs_state("31b")
    frame("31b-played")
    g = B["clients"].get("h3dgame") or {}
    check("31", "tiling: a game opening fullscreen (Hyprland's fullscreen 2) goes into the row", g.get("fullscreen") == 2 and "h3dgame" in B["row"],
          f"fullscreen {g.get('fullscreen')}; row {B['row']}")
    check("31", "... and it's played by itself (it has the keyboard)", (p or {}).get("class") == "h3dgame", p)
    lost = [c for c in A["panels"] if c not in B["panels"]]
    kept = [c for c in A["panels"] if c in B["panels"]]
    check("31", "... every other window round you is still drawn (from the wall, from the world, a floating one, another game), the row as it was",
          not lost and [c for c in B["row"] if c != "h3dgame"] == A["row"],
          f"gone {lost}; row {A['row']} -> {B['row']}; Hyprland shows {[c for c in A['panels'] if (B['clients'].get(c) or {}).get('visible')]}")
    check("31", "... as they were (alpha)", kept and all(abs(B["panels"][c]["alpha"] - A["panels"][c]["alpha"]) < 0.02 for c in kept),
          {c: (A["panels"][c]["alpha"], B["panels"][c]["alpha"]) for c in kept})
    check("31", "... and the top bar, on the wall (the fullscreen window isn't: it's in the row)", A["bar"] and B["bar"], f"{A['bar']} -> {B['bar']}")
    view, bg1, n1 = fs_rates()
    check("31", "... and the other game goes on drawing at the 3D view's pace (frame callbacks, presented)", n1 >= n0 + 3 and fs_kept_rate(view, bg1),
          f"frames a second {bg0} -> {bg1}, the 3D view's {view}; lines {n0} -> {n1}")
    got, moved = fs_keys("31-keys")
    check("31", "... its keys are its: W and 8 reach it, and you don't walk", "key down W" in got and "key down 8" in got and moved < 0.05,
          f"the game got {got}; you moved {moved:.2f} m")
    press("meta_l", "esc")
    time.sleep(1.0)
    C = fs_state("31c")
    check("31", "Super+Esc: walking, it stays fullscreen", C["playing"] is None and (C["clients"].get("h3dgame") or {}).get("fullscreen") == 2,
          f"playing {C['playing']}; fullscreen {(C['clients'].get('h3dgame') or {}).get('fullscreen')}")
    ctl("hypr3d", "turn", f"{FS['yaw0']:.1f}", "0")
    time.sleep(0.5)
    frame("31c-ring")
    a = aim_row("h3dgame")
    click("left")
    time.sleep(1.0)
    s, act = st(), fs_active().get("class")
    check("31", "... a click on it gives it the keyboard, and it isn't played (you ended that, till it's fullscreen again)", a and act == "h3dgame" and s["playing"] is None,
          f"aimed {a and a.get('class')}; active {act}; playing {s['playing']}")
    ctl("hypr3d", "turn", f"{FS['yaw0']:.1f}", "0")
    time.sleep(0.3)
    press("t")
    time.sleep(1.2)
    D = fs_state("31d")
    gone = [c for c in A["panels"] if c not in D["panels"]]
    check("31", "T while it's fullscreen: tiling off, every window still drawn (flying back where it was)", not D["tiling"] and not gone, f"tiling {D['tiling']}; gone {gone}")
    press("t")
    time.sleep(1.2)
    E = fs_state("31e")
    check("31", "T again: tiling on, every window in the row again, the fullscreen game too", E["tiling"] and set(A["row"]) | {"h3dgame"} <= set(E["row"]),
          f"{A['row']} + h3dgame -> {E['row']}")
    frame("31e-ring")
    ctl("dispatch", 'hl.dsp.focus({ window = "class:h3dgame" })')
    time.sleep(0.3)
    ctl("dispatch", 'hl.dsp.window.fullscreen({ action = "unset", mode = "fullscreen", window = "class:h3dgame" })')
    time.sleep(1.0)
    ctl("dispatch", 'hl.dsp.window.fullscreen({ action = "set", mode = "fullscreen", window = "class:h3dgame" })')
    p = fs_playing("h3dgame")
    check("31", "out of fullscreen and fullscreen again, with the keyboard: played by itself again", (p or {}).get("class") == "h3dgame", p)
    # a floating window over it takes the keyboard, ending play; when it closes by itself play resumes at once (the
    # shortcut hold applies only when a window closes just after a shortcut key)
    alice("setsid -f foot --app-id h3d-fshelp > /dev/null 2>&1")
    wait_for("the window over it", lambda: client("h3d-fshelp"), 20, 0.3)
    time.sleep(1.2)
    F = fs_state("31f")
    check("31", "a window opening over it takes the keyboard: play ends, it stays fullscreen",
          F["active"] == "h3d-fshelp" and F["playing"] is None and (F["clients"].get("h3dgame") or {}).get("fullscreen") == 2,
          f"active {F['active']}; playing {F['playing']}; fullscreen {(F['clients'].get('h3dgame') or {}).get('fullscreen')}")
    machine.execute("pkill -f 'app-id h3d-fshelp'; true")
    wait_for("the window over it gone", lambda: not client("h3d-fshelp"), 10, 0.3)
    time.sleep(0.8)
    act = fs_active().get("class")
    if act != "h3dgame":  # Hyprland focused another window: refocus the game
        note("31", "the keyboard after the window over it closed", act)
        ctl("dispatch", 'hl.dsp.focus({ window = "class:h3dgame" })')
    s1 = st()
    p = fs_playing("h3dgame", 2)
    check("31", "... it closes by itself, the keyboard back with the game: played by itself again at once (no shortcut closed it: nothing held back, not the 5 s)",
          (p or {}).get("class") == "h3dgame", f"active {act}; 0.8 s after: playing {s1['playing']}, playHeld {s1.get('playHeld')}; then {p}")
    # ... same once the keys' 4 s hold ran out: auto-played at once when it closes
    alice("setsid -f foot --app-id h3d-fshelp > /dev/null 2>&1")
    wait_for("the window over it", lambda: client("h3d-fshelp"), 20, 0.3)
    try:
        s0 = wait_for("the game's keys held back", lambda: (lambda s: s if s.get("playHeld") else None)(st()), 5, 0.2)
        s1 = wait_for("walking again, 4 s without a key", lambda: (lambda s: s if s.get("playHeld") is False else None)(st()), 8, 0.3)
    except TimeoutError:
        s0, s1 = None, st()
    act0 = fs_active().get("class")
    machine.execute("pkill -f 'app-id h3d-fshelp'; true")
    wait_for("the window over it gone", lambda: not client("h3d-fshelp"), 10, 0.3)
    time.sleep(0.8)
    act = fs_active().get("class")
    if act != "h3dgame":
        note("31", "the keyboard after the window over it closed", act)
        ctl("dispatch", 'hl.dsp.focus({ window = "class:h3dgame" })')
        time.sleep(0.3)
    s2 = st()
    p = fs_playing("h3dgame", 2)
    told = cl_log_since(40, "walking again: the keys are hypr3d's")
    check("31", "... over it again, the game's keys' hold run out (4 s without a key), then it closes: the keyboard back with the game, played by itself at once (\"h3dgame: fullscreen, played\")",
          s0 and s1.get("playHeld") is False and act0 == "h3d-fshelp" and (p or {}).get("class") == "h3dgame" and "h3dgame: fullscreen, played" in told
          and "has the keyboard again" not in told, f"held {bool(s0)}, then playHeld {s1.get('playHeld')}, active {act0}; closed: playing {s2['playing']}, then {p}; {told.strip()[-400:]}")
    alice("setsid -f foot --app-id h3d-fshelp > /dev/null 2>&1")
    wait_for("the window over it", lambda: client("h3d-fshelp"), 20, 0.3)
    time.sleep(1.2)
    s0, act0 = st(), fs_active().get("class")
    a = aim_row("h3dgame")
    click("left")
    p = fs_playing("h3dgame")
    check("31", "... over it again, the keyboard its: a click on the game gives the game the keyboard, and it's played by itself",
          act0 == "h3d-fshelp" and s0["playing"] is None and a and (p or {}).get("class") == "h3dgame",
          f"before: active {act0}, playing {s0['playing']}; aimed {a and a.get('class')}; after {p}")
    machine.execute("pkill -f 'app-id h3d-fshelp'; true")
    wait_for("the window over it gone", lambda: not client("h3d-fshelp"), 10, 0.3)
    ctl("hypr3d", "play", "off")
    machine.execute("pkill -f 'title h3dgame --fullscreen'; true")
    wait_for("the game gone", lambda: not client("h3dgame"), 10, 0.3)
    time.sleep(2.0)
    Z = fs_state("31z")
    missing = [c for c in A["panels"] if c not in Z["panels"]]
    check("31", "the game quit: every window drawn, the top bar too, the row as it was", not missing and Z["bar"] and set(A["row"]) <= set(Z["row"]),
          f"missing {missing}; bar {Z['bar']}; row {A['row']} -> {Z['row']}")
    fs_done()

    # ---- no tiling: a game opening fullscreen in front of you
    fs_setup(False)
    time.sleep(2.5)
    A = fs_state("31g")
    bg0, n0 = fs_bg_rate()
    fs_game("h3dgame", "--fullscreen")
    p = fs_playing("h3dgame")
    time.sleep(1.0)
    B = fs_state("31h")
    g = B["clients"].get("h3dgame") or {}
    check("31", "no tiling: a game opening fullscreen opens in front of you, played by itself",
          g.get("fullscreen") == 2 and "h3dgame" in B["placed"] and (p or {}).get("class") == "h3dgame", f"fullscreen {g.get('fullscreen')}; placed {list(B['placed'])}; {p}")
    lost = [c for c in A["panels"] if c not in B["panels"]]
    kept = [c for c in A["panels"] if c in B["panels"]]
    check("31", "... every other window still drawn, as it was: the wall's (tiled, floating, another game), the world's",
          not lost and all(abs(B["panels"][c]["alpha"] - A["panels"][c]["alpha"]) < 0.02 for c in kept), f"gone {lost}; alpha {[(c, B['panels'][c]['alpha']) for c in kept]}")
    check("31", "... and the top bar", B["bar"], B["bar"])
    view, bg1, n1 = fs_rates()
    check("31", "... and the other game on the wall (Hyprland draws it nowhere: under the fullscreen one) goes on drawing at the 3D view's pace",
          n1 >= n0 + 3 and fs_kept_rate(view, bg1), f"frames a second {bg0} -> {bg1}, the 3D view's {view}; lines {n0} -> {n1}")
    press("meta_l", "esc")
    time.sleep(0.8)
    frame("31h-view")
    r = ctl("hypr3d", "window", "h3dgame", "wall")
    wait_for("the game on the wall", lambda: not placed("h3dgame"), 10, 0.3)
    time.sleep(1.0)
    C = fs_state("31i")
    frame("31i-wall")
    wall = [c for c in ("h3d-wall", "h3d-float", "h3dgame-bg") if c in C["panels"]]
    world = [c for c in ("h3d-t1", "h3dxterm") if c not in C["panels"]]
    check("31", "sent to the wall, fullscreen: it hides the wall's other windows and the top bar there, as on the 2D desktop",
          (C["clients"].get("h3dgame") or {}).get("fullscreen") == 2 and "h3dgame" in C["panels"] and not wall and not C["bar"], f"{r}; still drawn {wall}; bar {C['bar']}")
    check("31", "... and nothing else: the windows out in the world still drawn", not world, f"gone {world}")
    fs_bar(True)  # restarted: maps over it, like a notification
    time.sleep(0.8)
    C2 = fs_state("31i2")
    check("31", "... a surface on the top layer mapped after it went fullscreen (the bar started again, as a notification maps) is drawn over it, as on the 2D desktop",
          (C2["clients"].get("h3dgame") or {}).get("fullscreen") == 2 and "h3dgame" in C2["panels"] and C2["bar"], f"bar {C['bar']} -> {C2['bar']}")
    fs_done()

    # ---- maximized (Hyprland's fullscreen 1) hides the rest of its workspace too
    fs_setup(True, bg=False)
    A = fs_state("31j")
    ctl("dispatch", 'hl.dsp.focus({ window = "class:h3d-t2" })')
    time.sleep(0.4)
    ctl("dispatch", 'hl.dsp.window.fullscreen({ action = "set", mode = "maximized" })')
    time.sleep(1.5)
    B = fs_state("31k")
    g = B["clients"].get("h3d-t2") or {}
    lost = [c for c in A["panels"] if c not in B["panels"]]
    check("31", "tiling: a terminal in the row maximized (Hyprland's fullscreen 1): every other window still drawn", g.get("fullscreen") == 1 and not lost,
          f"fullscreen {g.get('fullscreen')}; gone {lost}")
    check("31", "... and it isn't played (maximized isn't fullscreen)", B["playing"] is None, B["playing"])
    frame("31k-maximized")
    ctl("dispatch", 'hl.dsp.window.fullscreen({ action = "unset", mode = "maximized" })')
    time.sleep(1.0)
    fs_done()

    # ---- Helldivers 2 (Proton, X11): maps windowed, a second window takes the keyboard, then fullscreen: Hyprland
    # unfocuses the covered window and focuses none
    fs_setup(True, bg=False)
    fs_events_start()
    fs_events_mark("hd2")
    xd = x_display()
    alice(f"rm -f {GAME_LOG}; touch {GAME_LOG}")
    env = f"DISPLAY={xd} SDL_VIDEODRIVER=x11 SDL_VIDEO_X11_WMCLASS=steam_app_553850 SteamAppId=553850 SteamGameId=553850"
    hd2 = lambda: next((c for c in json.loads(ctl("-j", "clients")) if c["title"] == "hd2main"), None)
    alice(f"env {env} setsid -f h3dgame --title hd2main --log {GAME_LOG} > /dev/null 2>&1")
    try:
        g = wait_for("the game's window", hd2, 20, 0.1)
    except TimeoutError:  # sometimes: no matching xwaylandSurface
        note("31", "the game's X11 window never mapped: started again")
        machine.execute("pkill -f 'title hd2main'; true")
        time.sleep(0.5)
        alice(f"env {env} setsid -f h3dgame --title hd2main --log {GAME_LOG} > /dev/null 2>&1")
        g = wait_for("the game's window", hd2, 30, 0.1)
    time.sleep(0.3)
    alice(f"env DISPLAY={xd} SteamAppId=553850 SteamGameId=553850 setsid -f xterm -class steam_app_553850 -T hd2helper -geometry 60x12 > /dev/null 2>&1")
    h = wait_for("its second window", lambda: next((c for c in json.loads(ctl("-j", "clients")) if c["class"] == "steam_app_553850" and c["address"] != g["address"]), None), 20, 0.1)
    try:
        wait_for("the second window with the keyboard", lambda: fs_active().get("address") == h["address"], 5, 0.1)
    except TimeoutError:
        pass
    time.sleep(0.8)
    act0 = fs_active().get("title")
    ctl("dispatch", 'hl.dsp.window.fullscreen({ mode = "fullscreen", action = "set", window = "title:hd2main" })')
    p = fs_playing("steam_app_553850")
    time.sleep(0.5)
    ev = fs_events("hd2")
    told = ctl("hypr3d", "log", "12")
    i = ev.index("fullscreen>>1") if "fullscreen>>1" in ev else -1
    # the empty activewindow comes after fullscreen>>1 in Hyprland 0.55, before it in 0.56
    nofocus = ev[i + 1:i + 2] == ["activewindow>>,"] or ev[max(i - 2, 0):i] == ["activewindow>>,", "activewindowv2>>"]
    check("31", "Helldivers 2's way: the game maps, its second window maps and takes the keyboard, the game goes fullscreen, and Hyprland gives the keyboard to no window",
          i > 0 and any(e.startswith("openwindow>>") and "hd2main" in e for e in ev[:i]) and nofocus, f"before it: {act0}; {ev[:16]}")
    check("31", "... the game is given the keyboard, and played by itself", p and p["title"] == "hd2main" and "fullscreen with no window focused: focused and played" in told,
          f"{p}; {told.strip()[-240:]}")
    got, moved = fs_keys("31-hd2")
    check("31", "... its keys are its: W and 8 reach it, and you don't walk", "key down W" in got and "key down 8" in got and moved < 0.05,
          f"the game got {got}; you moved {moved:.2f} m")
    ctl("hypr3d", "play", "off")
    fs_kill("steam_app_553850")

    # ---- Wine's way (Tk): fullscreen by client message after its own dialog took the keyboard
    alice(f"rm -f {FS_TK_LOG}; DISPLAY={xd} setsid -f python3 -u {H}/tkfs.py h3dtkfs transient 1500 3000 > {FS_TK_LOG} 2>&1")
    wait_for("its dialog", lambda: "helper mapped" in machine.succeed(f"cat {FS_TK_LOG}"), 30, 0.2)
    wait_for("its fullscreen request", lambda: "asked for fullscreen" in machine.succeed(f"cat {FS_TK_LOG}"), 30, 0.2)
    time.sleep(0.5)
    main = next((c for c in json.loads(ctl("-j", "clients")) if c["title"].startswith("MAIN h3dtkfs")), {})
    p = fs_playing(main.get("class", "?"))
    check("31", "an X11 app (Tk) going fullscreen by a client message after its dialog took the keyboard (Wine's way): played by itself",
          main.get("fullscreen") == 2 and p and p["title"].startswith("MAIN"), f"fullscreen {main.get('fullscreen')}; {p}")
    machine.execute(f"echo '### T' >> {FS_TK_LOG}")
    t0 = windows3d()["tiling"]["on"]
    press("t")
    time.sleep(0.8)
    t1 = windows3d()["tiling"]["on"]
    got = [l for l in machine.succeed(f"cat {FS_TK_LOG}").split("### T", 1)[-1].splitlines() if l.startswith("key ")]
    check("31", "... T is its: the app gets it, and tiling stays on", t0 and t1 and "key t" in got, f"tiling {t0} -> {t1}; the app got {got}")
    frame("31-tk-played")
    fs_done()

    # ---- the game's own dialog (a Wine message box) after play ended: it keeps the keyboard, the game is played
    fs_plain()
    xd = x_display()
    go = "/tmp/tkfs-dialog.go"
    alice(f"rm -f {go} {FS_TK_LOG}; DISPLAY={xd} setsid -f python3 -u {H}/tkfs.py h3dtkdlg none 500 1000 {go} > {FS_TK_LOG} 2>&1")
    wait_for("its fullscreen request", lambda: "asked for fullscreen" in machine.succeed(f"cat {FS_TK_LOG}"), 30, 0.2)
    p0 = fs_playing("H3dtkdlg")
    alice("setsid -f foot --app-id h3d-fshelp > /dev/null 2>&1")
    wait_for("the window over it", lambda: client("h3d-fshelp"), 20, 0.3)
    time.sleep(1.2)
    s1, act1 = st(), fs_active().get("class")
    alice(f"touch {go}")
    wait_for("its dialog", lambda: "dialog mapped" in machine.succeed(f"cat {FS_TK_LOG}"), 20, 0.2)
    time.sleep(1.5)
    s2, act2 = st(), fs_active().get("title") or ""
    machine.execute(f"echo '### keys' >> {FS_TK_LOG}")
    press("k")
    press("j")
    time.sleep(0.5)
    got = [l for l in machine.succeed(f"cat {FS_TK_LOG}").split("### keys", 1)[-1].splitlines() if l.startswith("key ")]
    frame("31-dialog")
    check("31", "a dialog of the game's own opening over it after play mode ended by itself (a window over it took the keyboard): the dialog keeps the keyboard, the game's played with it, and your keys are the dialog's",
          fs_title(p0).startswith("MAIN") and act1 == "h3d-fshelp" and s1["playing"] is None and act2.startswith("DIALOG") and fs_title(s2["playing"]).startswith("MAIN") and
          got == ["key k dialog", "key j dialog"],
          f"played first: {fs_title(p0)}; a window over it: active {act1}, playing {fs_title(s1['playing'])}; its dialog: active {act2}, playing {fs_title(s2['playing'])}; "
          f"keys {got}")
    machine.execute("pkill -f tkfs[.]py; pkill -f 'app-id h3d-fshelp'; true")
    wait_for("the app gone", lambda: not client("H3dtkdlg"), 10, 0.3)

    # ---- a transient dialog going fullscreen: the dialog itself is played (focusing its parent would end the
    # fullscreen); Super+Esc or play off ends play for good; also with misc:on_focus_under_fullscreen 0
    dlg = lambda: client("H3dtkpfsdlg") or {}
    autoplays = lambda: sum(1 for l in ctl("hypr3d", "log", "400").splitlines() if "fullscreen, played" in l or "focused and played" in l)
    for setting in (2, 0):
        ctl("eval", f"hl.config({{ misc = {{ on_focus_under_fullscreen = {setting} }} }})")
        alice(f"rm -f {FS_TK_LOG}; DISPLAY={xd} setsid -f python3 -u {H}/tkfs.py h3dtkpfs fullscreen 1000 1500 > {FS_TK_LOG} 2>&1")
        wait_for("its fullscreen request", lambda: "asked for fullscreen" in machine.succeed(f"cat {FS_TK_LOG}"), 30, 0.2)
        p = fs_playing("H3dtkpfsdlg")
        time.sleep(1.0)
        d0, s0 = dlg(), st()
        frame(f"31-dialog-fullscreen-{setting}")
        check("31", f"a dialog of an app's out in the world going fullscreen (misc:on_focus_under_fullscreen {setting}): it's played itself, and stays fullscreen",
              fs_title(p).startswith("DIALOG") and fs_title(s0["playing"]).startswith("DIALOG") and d0.get("fullscreen") == 2,
              f"played {fs_title(p)}, then {fs_title(s0['playing'])}; the dialog: fullscreen {d0.get('fullscreen')}; the window: {(client('H3dtkpfs') or {}).get('fullscreen')}")
        n0 = autoplays()
        press("meta_l", "esc")
        time.sleep(1.5)
        s1, d1 = st(), dlg()
        check("31", "... Super+Esc: walking, and it stays so while the dialog stays fullscreen", s0["playing"] and s1["playing"] is None and d1.get("fullscreen") == 2,
              f"playing {fs_title(s0['playing'])} -> {fs_title(s1['playing'])}; played by itself since: {autoplays() - n0}; fullscreen {d1.get('fullscreen')}")
        if setting == 2:
            a = aim_row("H3dtkpfsdlg")
            press("p")
            p2 = fs_playing("H3dtkpfsdlg")
            time.sleep(1.0)
            d2 = dlg()
            n1 = autoplays()
            ctl("hypr3d", "play", "off")
            time.sleep(1.5)
            s3, d3 = st(), dlg()
            check("31", "... P on it: it's played itself, still fullscreen; hyprctl hypr3d play off: walking, and it stays so",
                  a and fs_title(p2).startswith("DIALOG") and d2.get("fullscreen") == 2 and s3["playing"] is None and d3.get("fullscreen") == 2,
                  f"aimed {a and a.get('class')}; P: {fs_title(p2)}, fullscreen {d2.get('fullscreen')}; play off: {fs_title(s3['playing'])}, fullscreen {d3.get('fullscreen')}, "
                  f"played by itself since: {autoplays() - n1}")
        machine.execute("pkill -f tkfs[.]py; true")
        wait_for("the app gone", lambda: not client("H3dtkpfs") and not dlg(), 10, 0.3)
        ctl("hypr3d", "play", "off")
    ctl("eval", "hl.config({ misc = { on_focus_under_fullscreen = 2 } })")
    fs_done()

    # ---- two monitors: play ended, the mouse away and back: the fullscreen game isn't auto-played (you came back to
    # walk); a click plays it
    fs_plain(inside=False)
    ctl("eval", 'hl.config({ plugin = { hypr3d = { monitor = "" } } })')
    r, m = second_monitor()
    ctl("dispatch", 'hl.dsp.focus({ monitor = "H3D-2" })')
    alice("setsid -f foot --app-id h3d-other > /dev/null 2>&1")
    wait_for("a terminal on the other monitor", lambda: (client("h3d-other") or {}).get("monitor") == m["H3D-2"]["id"], 20, 0.3)
    ctl("dispatch", 'hl.dsp.focus({ monitor = "Virtual-1" })')
    ensure_3d()
    menu_closed()
    ctl("hypr3d", "view", "first")
    ctl("hypr3d", "spawn")
    time.sleep(0.8)
    fs_game("h3dgame", "--fullscreen")
    p0 = fs_playing("h3dgame")
    alice("setsid -f foot --app-id h3d-fshelp > /dev/null 2>&1")
    wait_for("the window over it", lambda: client("h3d-fshelp"), 20, 0.3)
    time.sleep(1.2)
    s1, act1 = st(), fs_active().get("class")
    ctl("hypr3d", "away", "on")
    time.sleep(0.5)
    ctl("dispatch", 'hl.dsp.focus({ window = "class:h3d-other" })')  # as a click on it there
    time.sleep(0.5)
    s2, act2 = st(), fs_active().get("class")
    ctl("hypr3d", "away", "off")
    time.sleep(1.5)
    s3, act3 = st(), fs_active().get("class")
    check("31", "two monitors: the game fullscreen, play mode ended by itself (a window over it took the keyboard), the mouse away on the other monitor (a terminal there gets the keyboard) and back: the game isn't played by itself",
          fs_title(p0) == "h3dgame" and act1 == "h3d-fshelp" and s1["playing"] is None and s2["away"] and act2 == "h3d-other" and not s3["away"] and s3["playing"] is None and
          act3 != "h3dgame", f"{r}; played first: {fs_title(p0)}; the window over it: active {act1}, playing {fs_title(s1['playing'])}; away {s2['away']}, active {act2}; "
          f"back: away {s3['away']}, active {act3}, playing {fs_title(s3['playing'])}")
    a = aim_row("h3dgame")
    click("left")
    p = fs_playing("h3dgame")
    check("31", "... a click on it: played by itself", a and fs_title(p) == "h3dgame", f"aimed {a and a.get('class')}; {fs_title(p)}")
    fs_done()
    machine.execute("pkill -f 'app-id h3d-other'; pkill -f 'app-id h3d-fshelp'; true")
    ensure_3d(False)
    ctl("output", "remove", "H3D-2")
    wait_for("one monitor", lambda: "H3D-2" not in monitors(), 10, 0.3)

    # ---- a launcher (a keyboard-grabbing layer surface) over the played game: play ends, and resumes after Esc
    fs_plain()
    machine.execute(f"pkill -f 'quickshell -p'; rm -f {LAUNCHER_LOG}; true")
    alice(f"setsid -f quickshell -p {LAUNCHER} > /tmp/quickshell-launcher.log 2>&1")
    wait_for("the launcher's IPC", lambda: as_alice(f"quickshell ipc -p {LAUNCHER} show", 10)[0] == 0, 30, 0.5)
    ctl("eval", 'hl.unbind("SUPER + D")')  # an earlier bind would toggle it twice
    ctl("eval", f'hl.bind("SUPER + D", hl.dsp.exec_cmd("quickshell ipc -p {LAUNCHER} call launcher toggle"))')
    fs_game("h3dgame", "--fullscreen")
    p0 = fs_playing("h3dgame")
    press("meta_l", "d")
    try:
        sh = wait_for("the launcher over the 3D view", shell_layer, 10)
    except TimeoutError:
        sh = None
    time.sleep(0.5)
    s1 = st()
    type_text("wasd")
    time.sleep(0.3)
    press("esc")
    try:
        wait_for("the launcher closed", lambda: not shell_layer(), 10)
    except TimeoutError:
        pass
    p2 = fs_playing("h3dgame")
    log = launcher_log()
    check("31", "a launcher (a layer surface on a keybind) over the game played: play mode ends, the keys its own; it closes (Esc): the game's played by itself again",
          fs_title(p0) == "h3dgame" and sh and s1["playing"] is None and "text wasd" in log and "escape" in log and fs_title(p2) == "h3dgame",
          f"played first: {fs_title(p0)}; the launcher: {sh and sh.get('namespace')}, playing {fs_title(s1['playing'])}, it got {log[-3:]}; closed: playing {fs_title(p2)}")
    machine.execute("pkill -f 'quickshell -p'; true")
    ctl("eval", 'hl.unbind("SUPER + D")')
    fs_done()

    # ---- 3D left with the game fullscreen, and entered again
    fs_setup(True, bg=False)
    A = fs_state("31l")
    fs_game("h3dgame", "--fullscreen")
    fs_playing("h3dgame")
    time.sleep(0.5)
    ensure_3d(False)
    time.sleep(1.0)
    cl = json.loads(ctl("-j", "clients"))
    shown = [c["class"] for c in cl if c["visible"] and c["class"] != "h3dgame"]
    check("31", "3D left with the game fullscreen in the row: on the 2D desktop only it shows (Hyprland's own)",
          any(c["class"] == "h3dgame" and c["fullscreen"] == 2 and c["visible"] for c in cl) and not shown, f"shown too: {shown}")
    frame("31-2d")
    ensure_3d()
    time.sleep(2.0)
    G = fs_state("31m")
    check("31", "3D entered with it fullscreen: it isn't played (it was fullscreen already: P plays it)", G["playing"] is None, G["playing"])
    lost = [c for c in A["panels"] if c not in G["panels"]]
    check("31", "... and every other window is drawn round you, the game in the row", not lost and "h3dgame" in G["row"], f"gone {lost}; row {G['row']}")
    frame("31m-entered")
    fs_done()

    # ---- an X11 app's tooltip (override-redirect) stays drawn while a no_focus wall window goes fullscreen
    machine.execute("pkill -u alice foot; true")
    wait_for("no windows", lambda: not json.loads(ctl("-j", "clients")), 10, 0.3)
    ctl("eval", 'hl.window_rule({ name = "h3d-fs-nofocus", match = { class = "h3d-nofocus" }, no_focus = true })')
    ensure_3d(False)
    alice("setsid -f foot --app-id h3d-nofocus > /dev/null 2>&1")
    wait_for("the wall's terminal", lambda: client("h3d-nofocus"), 20, 0.5)
    ensure_3d()
    menu_closed()
    ctl("hypr3d", "spawn")
    time.sleep(0.8)
    c = tk_start()
    wait_for("the Tk app out in the world", lambda: placed(c["class"]), 20, 0.5)
    settled(10)
    size = client(c["class"])["size"]
    a = aim_local(c["class"], size[0] * 0.7, size[1] * 0.6)  # its canvas
    try:
        tips = wait_for("its tooltip", lambda: [p for p in panels() if p["kind"] == "popup" and p["class"] == c["class"]], 6, 0.3)
    except TimeoutError:
        tips = []
    ctl("dispatch", 'hl.dsp.window.fullscreen({ action = "set", mode = "fullscreen", window = "class:h3d-nofocus" })')
    time.sleep(1.5)
    B = fs_state("31o")
    tips2 = [p for p in panels() if p["kind"] == "popup" and p["class"] == c["class"]]
    frame("31o-tooltip")
    check("31", "an X11 app out in the world with its tooltip up, a window on the wall going fullscreen: the app and its tooltip (an override-redirect window) still drawn, over it",
          a and tips and (B["clients"].get("h3d-nofocus") or {}).get("fullscreen") == 2 and B["playing"] is None and c["class"] in B["panels"] and tips2 and
          all(p["placed"] for p in tips2), f"aimed {a and a.get('local')}; tooltip before {[p['box'] for p in tips]}, after {[(p['box'], p['placed']) for p in tips2]}; "
          f"drawn {list(B['panels'])}; playing {B['playing']}")
    ctl("dispatch", 'hl.dsp.window.fullscreen({ action = "unset", mode = "fullscreen", window = "class:h3d-nofocus" })')
    machine.execute("pkill -f tkapp[.]py; true")
    fs_done()
    machine.execute("pkill -u alice foot; true")
    ensure_3d(False)
    alice("setsid -f foot --app-id h3d-left > /dev/null 2>&1; sleep 0.7; setsid -f foot --app-id h3d-right > /dev/null 2>&1")
    wait_for("two terminals", lambda: len(json.loads(ctl("-j", "clients"))) >= 2, 20)
    time.sleep(1)


@section("live","tools/test/live/check.sh, the script for checking your own desktop, run in the VM from a terminal, with a ticking clock and a wallpaper (and a real change, and Ctrl+C)")
def s_live():
    # a session without the plugin (check.sh loads and unloads it): a wallpaper, a ticking clock, the script's terminal
    start_hyprland("hyprland.lua", lua_config(), terminals=False)
    mic_noise()
    machine.execute("rm -rf /tmp/live /tmp/live.out /tmp/live.status")
    alice("setsid -f swaybg -c '#2b4a6f' > /dev/null 2>&1")
    shell_overlay()  # the crosshair starts on this overlay
    alice("setsid -f foot --app-id h3d-clock sh -c 'while :; do clear; date +%T.%N; sleep 0.25; done' > /dev/null 2>&1")
    wait_for("the clock", lambda: any(c["class"] == "h3d-clock" for c in json.loads(ctl("-j", "clients"))), 20)
    time.sleep(1.5)
    # CHECK_SAY: sings --mic's prompts into the test microphone, cutting off the previous one
    machine.succeed(f"""cat > {H}/sing.sh << 'EOF'
#!/bin/sh
pkill -f 'node.name=h3d-[s]ing'
case "$1" in s) f=hiss ;; quiet) f=silence ;; *) f=man_$1 ;; esac
exec pw-cat -p --target test_mic_in -P node.name=h3d-sing {H}/wav/${{f}}_long.wav
EOF
chmod 755 {H}/sing.sh && chown alice {H}/sing.sh""")
    # CHECK_DO: does --app's prompts through hyprctl instead of keys
    machine.succeed(f"""cat > {H}/do.sh << 'EOF'
#!/bin/sh
sleep 1
case "$1" in
    play) hyprctl hypr3d play on ;;
    stop) hyprctl hypr3d play off ;;
    type) hyprctl hypr3d type on; sleep 2; hyprctl hypr3d type off ;;
    carry) hyprctl hypr3d aim "$2"; sleep 1; hyprctl hypr3d grab; sleep 0.5; hyprctl hypr3d walk 0.4 back ;;
    place) hyprctl hypr3d grab ;;
    pin) hyprctl hypr3d window "$2" pin ;;
    unpin) hyprctl hypr3d walk 0.4 back; sleep 1.5; hyprctl hypr3d pin; sleep 1; hyprctl hypr3d grab ;;
    free) hyprctl hypr3d window "$2" play; sleep 8; hyprctl hypr3d play off ;;
esac
EOF
chmod 755 {H}/do.sh && chown alice {H}/do.sh""")
    sig = json.loads(alice("hyprctl -j instances"))[0]["instance"]
    lit = f"--map {LIT}" if machine.execute(f"test -f {LIT}")[0] == 0 else "--no-map"

    def in_terminal(args, name):
        """runs check.sh ARGS in its own terminal, output teed to /tmp/NAME.out"""
        run = (f"cd {H} && HYPRLAND_INSTANCE_SIGNATURE={sig} CHECK_SAY={H}/sing.sh CHECK_DO={H}/do.sh bash {H}/repo/tools/test/live/check.sh /tmp/live {args} "
               f"2>&1 | tee /tmp/{name}.out; echo ${{PIPESTATUS[0]}} > /tmp/{name}.status")
        alice(f"setsid -f foot --app-id h3d-check bash -c {shlex.quote(run)} > /dev/null 2>&1")

    t0 = time.time()
    in_terminal(f"--mic --avatar {AV} {lit} --app 'h3dgame --title liveapp' --so {SO}", "live")
    status = wait_for("the script to finish", lambda: machine.execute("cat /tmp/live.status")[1].strip(), 900, 1)
    took = time.time() - t0
    (LOGS / "live-check.txt").write_text(machine.execute("cat /tmp/live.out")[1])
    run1 = machine.execute("readlink /tmp/live/latest")[1].strip()
    for f in ("results.txt", "lipsync.jsonl", "hypr3d.log"):
        if machine.execute(f"test -f /tmp/live/{run1}/{f}")[0] == 0:
            copy_out(f"/tmp/live/{run1}/{f}", "live")
    machine.execute(f"cd /tmp/live/{run1} && tar cf /tmp/live-frames.tar frames audio status")
    copy_out("/tmp/live-frames.tar", "live")
    res = machine.execute(f"cat /tmp/live/{run1}/results.txt")[1]
    note("live", "its results", "; ".join(l.strip() for l in res.splitlines() if l.startswith(("ok", "FAIL")))[:1500])
    fails = [l for l in res.splitlines() if l.startswith("FAIL")]
    # llvmpipe draws 6-13 fps, so "keeps up with the monitor" rightly fails
    unexpected = [l for l in fails if "keeps up with the monitor" not in l]
    last = res.strip().splitlines()[-1] if res.strip() else ""
    check("live", "check.sh --mic --avatar --map --app, from a terminal, runs to the end, into a folder of its own (run-1, and latest)",
          "passed," in last and run1 == "run-1", f"{last}; exit {status}; {took:.0f} s; latest -> {run1}")
    check("live", "... and all its checks pass but the frame rate (software rendering)", not unexpected, unexpected[:6])
    desk = [l for l in res.splitlines() if "changes nothing on the screen" in l or "looks as it did" in l]
    check("live", "... the desktop comparisons pass, the clock ticking and the script's own terminal scrolling (left out)",
          len(desk) == 3 and all(l.startswith("ok") for l in desk) and "the terminal this runs in, at" in res, desk)
    carry = [l.strip() for l in res.splitlines() if "crosshair started on" in l or "grab (G)" in l or "put down" in l]
    # without plugin:hypr3d:wallpaper the wallpaper isn't drawn in 3D: nothing to aim at there
    check("live", "... the crosshair started on the shell's overlay: it turned to the nearest window (hyprctl hypr3d aim), through the overlay, and carried it",
          any("started on layer" in l for l in carry) and any(l.startswith("ok") and "grab (G)" in l for l in carry), carry)
    check("live", "... its lip sync heard the five vowels sung into the microphone", len([l for l in res.splitlines() if l.startswith("ok") and "you held" in l]) == 5,
          [l for l in res.splitlines() if "you held" in l])
    mic = [l.strip() for l in res.splitlines() if l.strip().startswith(("lip sync's badge", "its stream", "linked to", "pw-dump's links", "what came"))]
    check("live", "... and wrote what PipeWire said: the test microphone linked, not muted, its links, what came",
          any("linked to  [Test microphone (test_mic" in l and "muted: False" in l for l in mic) and any("links into it  [test_mic" in l for l in mic), mic)
    audio = machine.execute(f"ls /tmp/live/{run1}/audio; grep -c 'Test microphone' /tmp/live/{run1}/audio/wpctl-status.txt")[1]
    check("live", "... and saved wpctl status, wpctl inspect and pw-dump in audio/", all(f in audio for f in ("wpctl-status.txt", "wpctl-inspect-default-source.txt", "pw-dump.json")), audio)
    plog = machine.execute(f"cat /tmp/live/{run1}/hypr3d.log")[1]
    check("live", "... and the plugin's own log (hyprctl hypr3d log) in hypr3d.log", "INFO loaded" in plog and "lip sync: listening to test_mic" in plog,
          f"{len(plog.splitlines())} lines: " + " | ".join(plog.splitlines()[:3]))
    apps = [l for l in res.splitlines() if "--app" in l or "playing" in l or "pinned" in l or "picked up" in l or "put down" in l or "typing into it" in l or "own cursor" in l or "your own try" in l]
    check("live", "... --app: the app launched from 3D, played, stopped, typed into, its cursor, carried and put down (H), pinned and taken back (Shift+H) (its prompts done as they came)",
          len([l for l in apps if l.startswith("ok")]) >= 10 and not [l for l in apps if l.startswith("FAIL")], apps)
    check("live", "... and the free step: played, then walking again (the prompt done as it came)", "h3dgame, your own try" in res and "... done" in res,
          [l.strip() for l in res.splitlines() if "your own try" in l or "... done" in l])
    check("live", "... and its window closed after, as its close button does", "h3dgame: its window closed" in res,
          [l.strip() for l in res.splitlines() if l.strip().startswith("h3dgame:")])
    check("live", "... and it left 3D, unloaded the plugin and closed the microphone", "hypr3d" not in ctl("plugin", "list") and lipsync_node() is None and not ctl("hypr3d", "status").startswith("{"))

    # a real change: the clock closed mid-run
    machine.execute("pkill -f 'app-id h3d-[c]heck'; true")
    time.sleep(1)
    in_terminal(f"--avatar {AV} --no-map --so {SO}", "live2")
    wait_for("its first frames", lambda: machine.execute("test -f /tmp/live/run-2/frames/01-desktop-before.png")[0] == 0, 60, 0.2)
    time.sleep(2.5)  # and the one a second later
    machine.execute("pkill -f 'app-id h3d-[c]lock'; true")
    wait_for("the script to finish", lambda: machine.execute("cat /tmp/live2.status")[1].strip(), 400, 1)
    res2 = machine.execute("cat /tmp/live/run-2/results.txt")[1]
    desk = [l for l in res2.splitlines() if "changes nothing on the screen" in l or "looks as it did" in l]
    check("live", "a window closed while it ran (a real change): its desktop comparisons fail, in run-2, run-1 as it was",
          len(desk) == 3 and all(l.startswith("FAIL") for l in desk) and machine.execute("readlink /tmp/live/latest")[1].strip() == "run-2"
          and machine.execute("cat /tmp/live/run-1/results.txt")[1] == res, desk)
    machine.execute("pkill -f 'app-id h3d-[c]heck'; true")

    # Ctrl+C in 3D: check.sh's trap cleans up
    before = calm_frame("before-live-check-interrupted")
    alice(f"cd {H} && HYPRLAND_INSTANCE_SIGNATURE={sig} setsid -f bash {H}/repo/tools/test/live/check.sh /tmp/live --mic --avatar {AV} --no-map --so {SO} "
          "> /tmp/live3.out 2>&1")
    in3d = wait_for("the script in 3D with its avatar", lambda: (lambda s: s if s.startswith("{") and '"mode": "active"' in s and "BoothAccessories" in s else None)(ctl("hypr3d", "status")), 120, 0.5)
    machine.succeed("pkill -INT -f 'live/check[.]sh /tmp/live --mic'")  # [.] keeps pkill from matching its own shell
    # wait for its final line in results.txt: redirected to a file, its stdout loses what follows the interrupt
    try:
        gone = wait_for("the script to say how it went", lambda: "failed, in" in machine.execute("cat /tmp/live/run-3/results.txt")[1], 90, 0.5)
    except TimeoutError:
        gone = False
    res3 = machine.execute("cat /tmp/live/run-3/results.txt")[1]
    (LOGS / "live-check-interrupted.txt").write_text(machine.execute("cat /tmp/live3.out")[1] + "\n--- its results.txt:\n" + res3 +
                                                     "\n--- still running:\n" + machine.execute("ps -ef | grep '[c]heck[.]sh'")[1])
    check("live", "Ctrl+C (SIGINT) while it's in 3D: it stops, leaves 3D and unloads the plugin (run-3)", in3d and gone and "hypr3d" not in ctl("plugin", "list")
          and "interrupted: cleaning up" in res3 and machine.execute("test -s /tmp/live/run-3/hypr3d.log")[0] == 0, res3.strip().splitlines()[-3:])
    d = before.differs(calm_frame("after-live-check-interrupted"))
    check("live", "... and the desktop is as it was", d < 0.005 and lipsync_node() is None, f"{d:.2%} of pixels differ")
    machine.execute("pkill swaybg; true")
    shell_overlay(False)


# ------------------------------------------------------------------ closing in 3D, and a game's keys after play mode

CL_BIND = 'hl.bind("SUPER + Q", hl.dsp.window.close())'  # a typical close bind: closes the focused window
CL_SLOW = r'''
import sys, tkinter as tk
root = tk.Tk(className=sys.argv[1])
root.title(sys.argv[1])
root.geometry("640x400")
tk.Label(root, text="closes 1.3 s after it's asked to, as OBS does (its outputs stop first)").pack(expand=True)
root.protocol("WM_DELETE_WINDOW", lambda: (print("close asked", flush=True), root.after(1300, root.destroy)))
root.mainloop()
'''


def cl_wait(what, fn, timeout=10, every=0.2):
    """wait_for, but None on timeout so the section goes on"""
    try:
        return wait_for(what, fn, timeout, every)
    except TimeoutError:
        return None


def cl_clients():
    return {c["class"]: c for c in json.loads(ctl("-j", "clients") or "[]")}


def cl_active(what="class"):
    try:
        return (json.loads(ctl("-j", "activewindow") or "{}") or {}).get(what)
    except ValueError:
        return None


def cl_titles():
    """the windows' titles (a game's two windows have one class)"""
    return sorted(c["title"] for c in json.loads(ctl("-j", "clients") or "[]"))


def cl_fullscreen(cls):
    """hyprctl clients' fullscreen of that class's window: 0 none, 1 maximized, 2 fullscreen"""
    return (cl_clients().get(cls) or {}).get("fullscreen")


def cl_snap(tag):
    """keyboard focus, aimed and played class, and each window's degrees off the view's middle (None: not placed)"""
    s = st()
    cl = cl_clients()
    where = {}
    for c in cl:
        p = placed(c)
        where[c] = round(off_middle(p, s)) if p else None
    snap = {"active": cl_active(), "aimed": (s["aimed"] or {}).get("class"), "playing": (s["playing"] or {}).get("class"), "clients": set(cl), "where": where}
    note("31b", f"{tag}: the keyboard on {snap['active']}, the crosshair on {snap['aimed']}, playing {snap['playing']}", f"degrees off the view's middle {where}")
    return snap


def cl_press(tag, keys=("meta_l", "q"), times=1, every=0.6, wait=1.5):
    """presses keys `times` times (as for a slow-closing window); returns before, after and the closed classes"""
    before = cl_snap(f"{tag}, before")
    for _ in range(times):
        press(*keys, after=0.0)
        time.sleep(every)
    time.sleep(wait)
    after = cl_snap(f"{tag}, after")
    closed = sorted(before["clients"] - after["clients"])
    note("31b", f"{tag}: closed {closed}", f"{[(c, before['where'].get(c)) for c in closed]} degrees off the view's middle")
    return before, after, closed


def cl_fresh(n):
    """tiling on with only n terminals (h3d-c1...) 50 degrees apart, facing their middle; the last has the keyboard"""
    ensure_3d(False)
    clean_windows()
    machine.execute("pkill -u alice foot; pkill -f slowclose[.]py; true")
    cl_wait("no windows", lambda: not cl_clients(), 15, 0.3)
    ctl("hypr3d", "tile", "off")
    ctl("hypr3d", "reset-windows", "forget")
    ensure_3d()
    menu_closed()
    ctl("hypr3d", "play", "off")
    ctl("hypr3d", "view", "first")
    ctl("hypr3d", "spawn")
    time.sleep(0.8)
    ctl("hypr3d", "tile", "on")
    time.sleep(0.5)
    yaw0 = st()["yaw"]
    for i in range(n):
        cls = f"h3d-c{i + 1}"
        ctl("hypr3d", "turn", f"{yaw0 + (i - (n - 1) / 2) * 50:.1f}", "0")
        time.sleep(0.4)
        alice(f"setsid -f foot --app-id {cls} > /dev/null 2>&1")
        wait_for(f"{cls}'s window", lambda: client(cls), 20, 0.3)
        time.sleep(1.0)
    ctl("hypr3d", "turn", f"{yaw0:.1f}", "0")
    time.sleep(0.8)
    return yaw0


def cl_game(args="", title="h3dgame", cls="h3dgame", kill=True):
    """opens h3dgame into the row; kill=False keeps a running one"""
    if kill:
        machine.execute("pkill -x h3dgame; true")
        time.sleep(0.8)
    alice(f"SDL_APP_ID={cls} SDL_VIDEO_WAYLAND_WMCLASS={cls} setsid -f stdbuf -oL h3dgame --title {title} {args} >> {GAME_LOG} 2>&1")
    c = cl_wait(f"{title}'s window in the row", lambda: next((c for c in json.loads(ctl("-j", "clients")) if c["title"] == title), None) if cls in tiled_row() else None, 30, 0.3)
    time.sleep(1.0)
    return c


def cl_held(held=True, timeout=5):
    """the status once playHeld is `held`, or as it is after timeout"""
    s = cl_wait(f"playHeld {held}", lambda: (lambda s: s if s.get("playHeld") is held else None)(st()), timeout, 0.1)
    return s or st()


def cl_log_since(n, mark):
    """the plugin's log lines after the last one with `mark` in it, out of the last n"""
    lines = ctl("hypr3d", "log", str(n)).splitlines()
    at = max((i for i, l in enumerate(lines) if mark in l), default=-1)
    return "\n".join(lines[at + 1:])


def cl_menu_to(slot, n, r=170):
    """moves the Action Menu's cursor from its middle to slot `slot` of n; returns the highlight"""
    t = (slot - 1) * 2 * math.pi / n
    rel(round(r * math.sin(t)), round(-r * math.cos(t)))
    return menu().get("highlight")


def cl_state(tag):
    """what a gamer's key did: the windows, the menu, tiling, 3D"""
    s, m = st(), menu()
    info = {"clients": set(cl_clients()), "menu": m.get("path") if m.get("open") else None, "tiling": s["tiling"], "mode": s["mode"], "held": s.get("playHeld"),
            "row": tiled_row() if s["tiling"] else []}
    note("31b", tag, {k: sorted(v) if isinstance(v, set) else v for k, v in info.items()})
    return info


@section("31b", "closing in 3D and a game's keys: Super+Q (a user's hl.dsp.window.close()) closes the window the crosshair is on, nothing when it's on none, and pressed again (a window slow to close, a game) not the next one; the Windows page's Close… asks first; closes, the Action Menu, X and shortcuts in the log; play mode ending by itself (the game quitting, another window taking the keyboard, a splash's real window) holds a gamer's keys back, walking still, till Super+Esc, P, a click on the game, its window with the keyboard again or 4 s without a key; a Steam game's hint")
def s_closing():
    lua_session()
    ensure_plugin()
    ctl("eval", CL_BIND)

    cl_fresh(4)
    aim_row("h3d-c1")
    b, a, closed = cl_press("Super+Q twice, 0.8 s apart, the crosshair on h3d-c1", times=2, every=0.8)
    told = ctl("hypr3d", "log", "8")
    check("31b", "Super+Q closes the window the crosshair is on (h3d-c1), not the one with the keyboard (h3d-c4, opened last); pressed again at once, nothing more",
          closed == ["h3d-c1"] and b["active"] == "h3d-c4", f"closed {closed}; the keyboard was on {b['active']}")
    check("31b", "... the log says where the shortcut's focus went, what closed (where it was, the keyboard, the crosshair), and that the second was for no window",
          "a shortcut: the keyboard to h3d-c1 (the crosshair's), was h3d-c4" in told and "closed: h3d-c1 (" in told and "in the ring, it had the keyboard, the crosshair on it" in told
          and "a shortcut: the keyboard to none (a window with the keyboard closed" in told, told.strip()[-600:])
    aim_row("h3d-c3")
    b, a, closed = cl_press("turned onto h3d-c3, Super+Q")
    check("31b", "turned onto another window, Super+Q closes that one", closed == ["h3d-c3"], f"closed {closed}; the crosshair was on {b['aimed']}")
    s = st()
    ctl("hypr3d", "turn", f"{s['yaw']:.1f}", "80")
    time.sleep(0.6)
    b, a, closed = cl_press("looking at the sky, Super+Q")
    told = ctl("hypr3d", "log", "4")
    check("31b", "the crosshair on no window (the sky): Super+Q closes nothing, the keyboard on no window",
          closed == [] and b["aimed"] is None and a["active"] is None and "a shortcut: the keyboard to none (the crosshair on no window)" in told,
          f"closed {closed}; aimed {b['aimed']}; the keyboard {b['active']} -> {a['active']}; {told.strip()[-200:]}")
    ctl("hypr3d", "turn", f"{s['yaw']:.1f}", "0")

    cl_fresh(3)
    cl_game()
    aim_row("h3d-c1")
    click("left")
    time.sleep(0.5)
    aim_row("h3dgame")
    b, a, closed = cl_press("h3d-c1 clicked, then the crosshair on the game, Super+Q")
    check("31b", "walking, a terminal clicked before, the crosshair on a game: Super+Q closes the game, not the terminal", closed == ["h3dgame"] and b["active"] == "h3d-c1",
          f"closed {closed}; the keyboard was on {b['active']}")

    # a window 1.3 s slow to close (Tk via XWayland, like OBS stopping its outputs): Super+Q four times
    machine.succeed(f"cat > {H}/slowclose.py << 'H3D_EOF'\n{CL_SLOW}\nH3D_EOF\nchown alice:users {H}/slowclose.py")
    alice(f"DISPLAY={x_display()} setsid -f python3 -u {H}/slowclose.py h3dslow > /tmp/slowclose.log 2>&1")
    c = cl_wait("the slow window in the row", lambda: next((c for c in cl_clients() if c.lower() == "h3dslow" and c in tiled_row()), None), 30, 0.5)
    time.sleep(1.0)
    aim_row(c)
    click("left")
    time.sleep(0.6)
    b, a, closed = cl_press("the slow window clicked, Super+Q 4 times 0.6 s apart", times=4, every=0.6, wait=2.0)
    note("31b", "the slow window's own log", machine.execute("cat /tmp/slowclose.log")[1].strip()[-200:])
    check("31b", "Super+Q pressed again and again while a window takes 1.3 s to close: only it closes", c and closed == [c], f"closed {closed}")

    cl_game("--relative")
    aim_row("h3dgame")
    play_on()
    b, a, closed = cl_press("playing the game, Super+Q 3 times 0.4 s apart", times=3, every=0.4)
    told = ctl("hypr3d", "log", "12")
    check("31b", "Super+Q three times while playing a game closes only the game", closed == ["h3dgame"], f"closed {closed}; the keyboard then {a['active']}")
    check("31b", "... the log says it closed while played, and that play mode ended by itself",
          "closed: h3dgame (h3dgame), in the ring, it had the keyboard, the crosshair on it, played" in told and "play mode ended: its window closed" in told, told.strip()[-500:])
    ctl("hypr3d", "play", "off")

    # the played game quits: a gamer's keys keep coming and must be held back
    cl_game()
    aim_row("h3dgame")
    play_on()
    row = [r for r in tiled_row() if r != "h3dgame"]
    machine.execute("pkill -x h3dgame; true")
    cl_wait("the game gone", lambda: "h3dgame" not in cl_clients(), 10, 0.2)
    s = cl_held(True, 2)
    told = ctl("hypr3d", "log", "6")
    check("31b", "the game quits while played: its keys held back (playHeld), said so", s.get("playHeld") is True and s["playing"] is None and
          "play mode ended (its window closed): the keys are held back. P plays h3dgame again, Super+Esc walks" in told, f"playHeld {s.get('playHeld')}; {told.strip()[-300:]}")
    before = set(cl_clients())
    got = []
    for k in ("b", "2", "8", "tab", "6", "2", "8", "x", "t", "esc"):
        press(k)
        got.append(cl_state(f"{k} (a gamer's key, the game gone)"))
    s = st()
    closed = sorted(before - set(cl_clients()))
    check("31b", "... B 2 8, Tab 6 2 8, X, T, Esc: nothing closed, no Action Menu, no window sent to the wall, tiling on, still in 3D, still held back",
          not closed and not any(g["menu"] for g in got) and all(g["tiling"] and g["mode"] == "active" and g["row"] == row for g in got) and s.get("playHeld") is True,
          f"closed {closed}; menus {[g['menu'] for g in got]}; tiling {[g['tiling'] for g in got]}; mode {s['mode']}; row {row} -> {got[-1]['row']}; playHeld {s.get('playHeld')}")
    if st()["mode"] != "active" or not st()["tiling"]:  # undo what the keys did if not held back
        ensure_3d()
        ctl("hypr3d", "tile", "on")
        time.sleep(0.8)
    press("f9")  # a held-back key restarts the 4 s; W walks
    feet = st()["feet"]
    press("w", hold=0.6)
    s = st()
    check("31b", "... W still walks, the keys still held back", math.dist(s["feet"], feet) > 0.2 and s.get("playHeld") is True,
          f"moved {math.dist(s['feet'], feet):.2f} m; playHeld {s.get('playHeld')}")
    frame("keys-held-back")
    press("f9")
    s0 = st()
    press("meta_l", "esc")
    s = st()
    press("b")
    m = menu()
    said = [l for l in ctl("hypr3d", "log", "4").splitlines() if "walking again" in l]  # Super+Esc's line, not the 4 s timeout's
    check("31b", "Super+Esc: walking again, said so; B opens the Windows page again",
          s0.get("playHeld") is True and s.get("playHeld") is False and said and said[-1].endswith("INFO walking again") and m.get("path", "").endswith("windows"),
          f"playHeld {s0.get('playHeld')} -> {s.get('playHeld')}; menu {m.get('path')}; {said}")
    menu_closed()

    cl_game()
    aim_row("h3dgame")
    play_on()
    machine.execute("pkill -x h3dgame; true")
    cl_wait("the game gone", lambda: "h3dgame" not in cl_clients(), 10, 0.2)
    s0 = cl_held(True, 2)
    time.sleep(4.5)
    s = st()
    check("31b", "the game quit, no key for 4 s: walking again, said so", s0.get("playHeld") is True and s.get("playHeld") is False and
          "walking again: the keys are hypr3d's" in ctl("hypr3d", "log", "6"), f"playHeld {s0.get('playHeld')} -> {s.get('playHeld')}")

    # another window takes the keyboard
    cl_game()
    aim_row("h3dgame")
    play_on()
    ctl("dispatch", 'hl.dsp.focus({ window = "class:h3d-c1" })')
    s0 = cl_held(True, 3)
    told = ctl("hypr3d", "log", "4")
    time.sleep(0.5)  # let the camera come back
    ctl("hypr3d", "aim", "h3d-c2")
    time.sleep(0.4)
    aimed = (st()["aimed"] or {}).get("class")
    press("p")
    s = cl_wait("playing", lambda: (lambda s: s if s["playing"] else None)(st()), 3) or st()
    check("31b", "another window takes the keyboard: play mode ends, the keys held back, said so",
          s0.get("playHeld") is True and "play mode ended (another window has the keyboard): the keys are held back" in told, f"playHeld {s0.get('playHeld')}; {told.strip()[-200:]}")
    check("31b", "... P plays the game again, not the terminal under the crosshair", aimed == "h3d-c2" and (s["playing"] or {}).get("class") == "h3dgame",
          f"the crosshair on {aimed}; playing {s['playing']}")

    # ... and hyprctl hypr3d play on (shared with the hypr3d:play dispatcher and hl.plugin.hypr3d.play())
    ctl("hypr3d", "play", "off")
    aim_row("h3dgame")
    play_on()
    ctl("dispatch", 'hl.dsp.focus({ window = "class:h3d-c1" })')
    s0 = cl_held(True, 3)
    time.sleep(0.5)
    ctl("hypr3d", "aim", "h3d-c2")
    time.sleep(0.4)
    aimed = (st()["aimed"] or {}).get("class")
    r = ctl("hypr3d", "play", "on")
    s = cl_wait("playing", lambda: (lambda s: s if s["playing"] else None)(st()), 3) or st()
    check("31b", "... hyprctl hypr3d play on too (P's, as the hypr3d:play dispatcher and hl.plugin.hypr3d.play() are): the game, not the terminal under the crosshair",
          s0.get("playHeld") is True and aimed == "h3d-c2" and r == "playing" and (s["playing"] or {}).get("class") == "h3dgame",
          f"playHeld {s0.get('playHeld')}; the crosshair on {aimed}; {r}; playing {s['playing']}")

    # ... a left click on the game
    ctl("hypr3d", "play", "off")
    aim_row("h3dgame")
    play_on()
    ctl("dispatch", 'hl.dsp.focus({ window = "class:h3d-c1" })')
    s0 = cl_held(True, 3)
    time.sleep(0.5)
    aim_row("h3dgame")
    game_mark("held-click")
    click("left")
    s = cl_wait("playing", lambda: (lambda s: s if s["playing"] else None)(st()), 3) or st()
    time.sleep(0.3)
    got = [l for l in game_lines("held-click") if l.startswith("button")]
    check("31b", "... a left click on the game plays it again, the click going nowhere", s0.get("playHeld") is True and (s["playing"] or {}).get("class") == "h3dgame" and not got,
          f"playHeld {s0.get('playHeld')}; playing {s['playing']}; the game got {got}")

    ctl("dispatch", 'hl.dsp.focus({ window = "class:h3d-c1" })')
    s0 = cl_held(True, 3)
    ctl("dispatch", 'hl.dsp.focus({ window = "class:h3dgame" })')
    s = cl_wait("playing", lambda: (lambda s: s if s["playing"] else None)(st()), 3) or st()
    check("31b", "... the game's window taking the keyboard back is played again, said so", s0.get("playHeld") is True and (s["playing"] or {}).get("class") == "h3dgame"
          and "play mode: h3dgame has the keyboard again" in ctl("hypr3d", "log", "6"), f"playHeld {s0.get('playHeld')}; playing {s['playing']}")
    ctl("hypr3d", "play", "off")

    # a game's splash, then its real window (same class, another process)
    machine.execute("pkill -x h3dgame; true")
    cl_wait("the game gone", lambda: "h3dgame" not in cl_clients(), 10, 0.2)
    cl_game("--size 480x270", title="steamsplash", cls="steam_app_31")
    hint = cl_wait("the hint", lambda: "steamsplash: P plays it (Super+Esc gives the keys back)" in ctl("hypr3d", "log", "8"), 4, 0.3)
    told = ctl("hypr3d", "log", "8")
    ctl("hypr3d", "aim", "steamsplash")
    time.sleep(0.4)
    r = ctl("hypr3d", "play", "on")
    time.sleep(0.8)
    cl_game(title="steamgame", cls="steam_app_31", kill=False)
    s = cl_wait("the real window played", lambda: (lambda s: s if (s["playing"] or {}).get("title") == "steamgame" else None)(st()), 8) or st()
    check("31b", "a game's splash played, its real window (same class, another process) taking the keyboard: the real one played", r == "playing" and (s["playing"] or {}).get("title") == "steamgame",
          f"{r}; playing {s['playing']}; {cl_log_since(20, 'playing steamsplash').strip()[-300:]}")
    check("31b", "a Steam game's window (class steam_app_*) opening in 3D, not played a moment later: said how to play it", hint, told.strip()[-400:])
    ctl("hypr3d", "play", "off")
    machine.execute("pkill -x h3dgame; true")

    # the Windows page: Close… asks first, so a game's keys (B, a digit, 8) close nothing
    cl_fresh(3)
    press("b")
    m = menu()
    slot = next((i["slot"] for i in m.get("items", []) if i["label"] not in ("Tiling", "Follow me")), 3)
    press(str(slot))
    page = menu()
    before = set(cl_clients())
    press("8")
    m = menu()
    frame("close-asks-first")
    closed = sorted(before - set(cl_clients()))
    labels = [i["label"] for i in m.get("items", [])]
    check("31b", "B, a window, 8 (Close…): nothing closes; a page asks first, \"Close it\" at the bottom (5), \"Keep it\" in every other slot",
          not closed and "/close:" in m.get("path", "") and m.get("title", "").startswith("Close ") and labels == ["Keep it"] * 4 + ["Close it"] + ["Keep it"] * 3,
          f"closed {closed}; {page.get('title')} -> {m.get('path')} {m.get('title')!r}: {labels}")
    press("8")
    m = menu()
    closed = sorted(before - set(cl_clients()))
    check("31b", "... 8 again (Keep it): back on the window's page, nothing closed", not closed and m.get("open") and m.get("path") == page.get("path"),
          f"closed {closed}; {m.get('path')} (the window's page {page.get('path')})")
    press("8")
    press("5")
    time.sleep(1.0)
    closed = sorted(before - set(cl_clients()))
    told = ctl("hypr3d", "log", "6")
    check("31b", "... Close…, then 5 (Close it): it closes, the menu closes, and the log says what the Action Menu closed",
          len(closed) == 1 and not menu().get("open") and f"the Action Menu: close {closed[0]} (" in told and f"closed: {closed[0]} (" in told, f"closed {closed}; {told.strip()[-300:]}")
    # the same by mouse: Tab, then clicks onto Windows, a window and Close…
    before = set(cl_clients())
    press("tab")
    m = menu()
    n = len(m.get("items", []))
    cl_menu_to(next((i["slot"] for i in m.get("items", []) if i["label"] == "Windows"), 6), n)
    click("left")
    m = menu()
    cl_menu_to(next((i["slot"] for i in m.get("items", []) if i["label"] not in ("Tiling", "Follow me")), 3), len(m.get("items", [])))
    click("left")
    page = menu()
    h = cl_menu_to(8, 8)
    click("left")
    m1 = menu()
    click("left")  # a new page puts the cursor back in the middle
    m2 = menu()
    closed = sorted(before - set(cl_clients()))
    check("31b", "Tab, then the mouse and clicks onto Windows, a window and Close…, and a click again: nothing closes, back on the window's page",
          not closed and h == 8 and "/close:" in m1.get("path", "") and m2.get("path") == page.get("path"), f"closed {closed}; highlight {h}; {m1.get('path')} -> {m2.get('path')}")
    cl_menu_to(8, 8)
    click("left")
    cl_menu_to(8, 8)
    click("left")  # Close…'s slot, now Keep it
    m3 = menu()
    cl_menu_to(8, 8)
    click("left")
    h = cl_menu_to(5, 8)
    click("left")
    time.sleep(1.0)
    closed = sorted(before - set(cl_clients()))
    check("31b", "... the mouse on the top left again (Keep it): back; Close… and the bottom (Close it): it closes",
          m3.get("path") == page.get("path") and h == 5 and len(closed) == 1 and not menu().get("open"), f"{m3.get('path')}; highlight {h}; closed {closed}")
    left = sorted(c for c in cl_clients() if c.startswith("h3d-c")) or ["h3d-c3"]
    aim_row(left[0])
    press("x")
    told = ctl("hypr3d", "log", "3")
    check("31b", "X sends the window under the crosshair to the wall, and the log says so", f"X: {left[0]} back to the wall" in told and left[0] not in tiled_row(),
          f"{told.strip()[-200:]}; row {tiled_row()}")
    r = ctl("hypr3d", "window", left[0], "close")
    time.sleep(1.0)
    check("31b", "hyprctl hypr3d window SEL close closes at once (no asking)", r == "closing" and not client(left[0]), r)

    ctl("eval", 'h3dNoFocus = hl.window_rule({ name = "h3d-nofocus", match = { class = "h3d-c2" }, no_focus = true })')
    cl_fresh(3)
    aim_row("h3d-c2")
    b, a, closed = cl_press("the crosshair on h3d-c2, which won't take the keyboard (no_focus), Super+Q")
    told = ctl("hypr3d", "log", "4")
    check("31b", "the crosshair on a window that won't take the keyboard (a no_focus rule): Super+Q closes nothing, not the one out of sight that has it; the log says why",
          b["aimed"] == "h3d-c2" and b["active"] == "h3d-c3" and closed == [] and "a shortcut: the keyboard to none (h3d-c2 won't take it), was h3d-c3" in told,
          f"aimed {b['aimed']}; the keyboard was on {b['active']}; closed {closed}; {told.strip()[-300:]}")
    ctl("eval", "h3dNoFocus:set_enabled(false)")

    # a game's two windows (one class, two processes): when the played one closes, Hyprland focuses the other, which
    # isn't played for that, and a second Super+Q closes nothing
    cl_fresh(0)
    cl_game(title="gameB")
    cl_game(title="gameA", kill=False)
    cl_wait("both in the row", lambda: tiled_row().count("h3dgame") == 2, 10, 0.2)
    ctl("hypr3d", "aim", "gameA")
    time.sleep(0.6)
    play_on()
    before = cl_titles()
    press("meta_l", "q", after=0.0)
    time.sleep(0.4)
    s1, act1 = st(), cl_active("title")
    press("meta_l", "q", after=0.0)
    time.sleep(1.5)
    after = cl_titles()
    told = cl_log_since(30, "playing gameA")
    check("31b", "a game's two windows (one class), the one played closed by Super+Q: the other, given the keyboard by Hyprland, isn't played for that; Super+Q again at once closes nothing",
          before == ["gameA", "gameB"] and after == ["gameB"] and act1 == "gameB" and s1["playing"] is None and s1.get("playHeld") is True,
          f"{before} -> {after}; after the first: the keyboard on {act1}, playing {(s1['playing'] or {}).get('title')}, playHeld {s1.get('playHeld')}; {told.strip()[-700:]}")
    ctl("hypr3d", "play", "off")
    cl_game(title="gameA", kill=False)
    cl_wait("both in the row", lambda: tiled_row().count("h3dgame") == 2, 10, 0.2)
    ctl("hypr3d", "aim", "gameA")
    time.sleep(0.6)
    play_on()
    machine.execute("pkill -f 'title gameA'; true")
    cl_wait("gameA gone", lambda: "gameA" not in cl_titles(), 10, 0.1)
    time.sleep(1.0)
    s, act = st(), cl_active("title")
    check("31b", "... the one played quitting by itself: the other, given the keyboard, isn't played either, the game's keys held back",
          act == "gameB" and s["playing"] is None and s.get("playHeld") is True,
          f"the keyboard on {act}; playing {(s['playing'] or {}).get('title')}; playHeld {s.get('playHeld')}; {cl_log_since(12, 'playing gameA').strip()[-500:]}")
    ctl("hypr3d", "play", "off")

    # a popup of another app (Discord's, Steam's) over the played game: Super+Q twice closes only the popup, since
    # Hyprland refocuses the game and the held-back second press must go to no window; turning ends the hold
    ctl("eval", 'hl.window_rule({ name = "h3d-cl-popup", match = { class = "h3d-popup" }, float = true })')
    popup = lambda: alice("setsid -f foot --app-id h3d-popup > /dev/null 2>&1")
    cl_fresh(1)
    cl_game()
    aim_row("h3dgame")
    play_on()
    popup()
    up = cl_wait("the popup with the keyboard", lambda: cl_active() == "h3d-popup", 20, 0.2)
    s0 = cl_held(True, 3)
    ctl("hypr3d", "aim", "h3d-popup")
    time.sleep(0.5)
    b, a, closed = cl_press("a game played, a popup over it took the keyboard, the crosshair on the popup: Super+Q twice, 0.8 s apart", times=2, every=0.8, wait=1.0)
    s1 = st()
    told = cl_log_since(30, "play mode ended (another window has the keyboard)")
    check("31b", "a game played, a popup of another app's taking the keyboard (the game's keys held back), the crosshair on the popup: Super+Q twice closes only the popup, "
          "not the game Hyprland gives the keyboard back to (the second is for no window)",
          up and s0.get("playHeld") is True and b["aimed"] == "h3d-popup" and closed == ["h3d-popup"] and "h3dgame" in a["clients"] and s1["playing"] is None
          and "a shortcut: the keyboard to none (a window with the keyboard closed: not turned or moved since), was h3dgame" in told,
          f"the popup with the keyboard {bool(up)}, playHeld {s0.get('playHeld')}; aimed {b['aimed']}; closed {closed}; then playing {s1['playing']}, playHeld {s1.get('playHeld')}; "
          f"{told.strip()[-500:]}")
    rel(100, 0)  # turning ends the shortcut hold
    s3 = cl_wait("playing", lambda: (lambda s: s if s["playing"] else None)(st()), 3) or st()
    told = cl_log_since(30, "play mode ended (another window has the keyboard)")
    check("31b", "... as you turn (the hold on shortcuts over), the game gets the keyboard back and is played again, as after one press",
          s1["playing"] is None and s1.get("playHeld") is True and (s3["playing"] or {}).get("class") == "h3dgame"
          and "the keyboard back to h3dgame, shortcuts held no more" in told and "play mode: h3dgame has the keyboard again" in told,
          f"just after: playing {s1['playing']}, playHeld {s1.get('playHeld')}; turned: playing {s3['playing']}, the keyboard on {cl_active()}; {told.strip()[-400:]}")
    # ... with no key, turn or move after: held back past the usual 4 s, played when the 5 s hold ends
    popup()
    up = cl_wait("the popup with the keyboard", lambda: cl_active() == "h3d-popup", 20, 0.2)
    cl_held(True, 3)
    ctl("hypr3d", "aim", "h3d-popup")
    time.sleep(0.5)
    t0 = time.time()
    b, a, closed = cl_press("the popup again, Super+Q twice, 0.8 s apart, then nothing", times=2, every=0.8, wait=0.3)
    time.sleep(max(0.0, t0 + 3.8 - time.time()))
    s4 = st()
    s5 = cl_wait("played by the hold's end", lambda: (lambda s: s if s["playing"] else None)(st()), 5, 0.2) or st()
    t5 = time.time() - t0
    check("31b", "... Super+Q twice on it again, then nothing: the game's keys still held back 3.8 s after (not walking), and it's played once the hold's over (5 s)",
          up and closed == ["h3d-popup"] and s4["playing"] is None and s4.get("playHeld") is True and (s5["playing"] or {}).get("class") == "h3dgame" and 4.5 < t5 < 8,
          f"closed {closed}; 3.8 s after: playing {s4['playing']}, playHeld {s4.get('playHeld')}; played {(s5['playing'] or {}).get('class')} {t5:.1f} s after the first press")
    if (st()["playing"] or {}).get("class") != "h3dgame":  # keep the next steps independent of a failure
        ctl("hypr3d", "play", "off")
        if "h3dgame" not in cl_clients():
            cl_game()
        aim_row("h3dgame")
        play_on()
    popup()
    up = cl_wait("the popup with the keyboard", lambda: cl_active() == "h3d-popup", 20, 0.2)
    cl_held(True, 3)
    ctl("hypr3d", "aim", "h3d-popup")
    time.sleep(0.5)
    b, a, closed = cl_press("the popup over the game played again, Super+Q once", wait=1.0)
    s1 = st()
    rel(100, 0)  # turning ends the shortcut hold
    s2 = cl_wait("played again", lambda: (lambda s: s if s["playing"] else None)(st()), 3) or st()
    told = cl_log_since(30, "play mode ended (another window has the keyboard)")
    check("31b", "... a popup again, Super+Q once: it closes, the keyboard back with the game, which isn't played till you turn (shortcuts held back), then played again",
          up and closed == ["h3d-popup"] and a["active"] == "h3dgame" and s1["playing"] is None and (s2["playing"] or {}).get("class") == "h3dgame"
          and "play mode: h3dgame has the keyboard again" in told, f"closed {closed}; the keyboard on {a['active']}; playing {s1['playing']}, turned: {s2['playing']}; {told.strip()[-400:]}")
    ctl("hypr3d", "play", "off")
    # the popup closing by itself with no recent shortcut: replayed at once, and a key 0.5 s later reaches the game
    if "h3dgame" not in cl_clients():
        cl_game()
    aim_row("h3dgame")
    play_on()
    time.sleep(3.2)  # the last Super+Q well before the popup goes
    popup()
    up = cl_wait("the popup with the keyboard", lambda: cl_active() == "h3d-popup", 20, 0.2)
    s0 = cl_held(True, 3)
    game_mark("31b-popup-gone")
    machine.execute("pkill -f 'app-id h3d-popup'; true")
    cl_wait("the popup gone", lambda: "h3d-popup" not in cl_clients(), 10, 0.1)
    time.sleep(0.5)
    press("1")  # a game's key, not a walking one
    s1 = cl_wait("played again", lambda: (lambda s: s if s["playing"] else None)(st()), 3, 0.1) or st()
    got = [l for l in game_lines("31b-popup-gone") if l.startswith("key") or l.startswith("focus")]
    told = cl_log_since(20, "play mode ended (another window has the keyboard)")
    check("31b", "... a popup over the game played closing by itself (no shortcut): the game played again at once, and a key typed half a second after reaches it",
          up and s0.get("playHeld") is True and (s1["playing"] or {}).get("class") == "h3dgame" and "key down 1" in got,
          f"the popup with the keyboard {bool(up)}, playHeld {s0.get('playHeld')}; playing {s1['playing']}; the game got {got[-6:]}; {told.strip()[-400:]}")
    ctl("hypr3d", "play", "off")
    if "h3dgame" not in cl_clients():
        cl_game()
    ctl("dispatch", 'hl.dsp.focus({ window = "class:h3dgame" })')
    time.sleep(0.3)
    ctl("dispatch", 'hl.dsp.window.fullscreen({ action = "set", mode = "fullscreen", window = "class:h3dgame" })')
    p0 = cl_wait("the game played by itself, fullscreen", lambda: (lambda s: s["playing"] if s["playing"] else None)(st()), 5)
    popup()
    up = cl_wait("the popup with the keyboard", lambda: cl_active() == "h3d-popup", 20, 0.2)
    s0 = cl_held(True, 3)
    s1 = cl_held(False, 8)  # 4 s without a key: walking again
    ctl("hypr3d", "aim", "h3d-popup")
    time.sleep(0.5)
    b, a, closed = cl_press("the game fullscreen, played by itself, the popup over it, its keys' hold run out: Super+Q twice, 0.8 s apart", times=2, every=0.8, wait=1.0)
    s2, fs = st(), cl_fullscreen("h3dgame")
    told = cl_log_since(20, "walking again: the keys are hypr3d's")
    check("31b", "... the game fullscreen, played by itself, a popup over it, the game's keys' hold run out (walking): Super+Q twice on the popup closes only it, the game still "
          "fullscreen and not played (the second for no window, not the game Hyprland gave the keyboard back to)",
          (p0 or {}).get("class") == "h3dgame" and up and s0.get("playHeld") is True and s1.get("playHeld") is False and b["aimed"] == "h3d-popup" and closed == ["h3d-popup"]
          and fs == 2 and s2["playing"] is None and "a shortcut: the keyboard to none (a window with the keyboard closed: not turned or moved since), was h3dgame" in told,
          f"played {p0}; playHeld {s0.get('playHeld')} -> {s1.get('playHeld')}; aimed {b['aimed']}; closed {closed}; fullscreen {fs}; playing {s2['playing']}; {told.strip()[-400:]}")
    ctl("hypr3d", "play", "off")
    machine.execute("pkill -x h3dgame; pkill -f 'app-id h3d-popup'; true")

    # Super+D's launcher open over the played game past 4 s, then Esc
    machine.execute(f"pkill -f 'quickshell -p'; rm -f {LAUNCHER_LOG}; true")
    alice(f"setsid -f quickshell -p {LAUNCHER} > /tmp/quickshell-launcher.log 2>&1")
    wait_for("the launcher's IPC", lambda: as_alice(f"quickshell ipc -p {LAUNCHER} show", 10)[0] == 0, 30, 0.5)
    ctl("eval", 'hl.unbind("SUPER + D")')  # an earlier bind would toggle it twice
    ctl("eval", f'hl.bind("SUPER + D", hl.dsp.exec_cmd("quickshell ipc -p {LAUNCHER} call launcher toggle"))')
    cl_fresh(1)
    cl_game()
    aim_row("h3dgame")
    play_on()
    press("meta_l", "d")
    up = cl_wait("the launcher over the view", lambda: shell_layer(), 10, 0.1)
    time.sleep(5.0)
    s1 = st()
    press("esc")
    cl_wait("the launcher gone", lambda: not shell_layer(), 10, 0.1)
    s2 = cl_wait("played again", lambda: (lambda s: s if s["playing"] else None)(st()), 3) or st()
    told = cl_log_since(14, "h3dgame: into the row")  # from before play, to see both lines
    check("31b", "playing, Super+D's launcher over the view for 5 s, then Esc: the game's keys held back meanwhile, and it's played again with the keyboard back",
          up and s1.get("playHeld") is True and s1["playing"] is None and (s2["playing"] or {}).get("class") == "h3dgame" and "play mode ended: a layer surface took the keyboard" in told
          and "play mode: h3dgame has the keyboard again" in told,
          f"the launcher {bool(up)}; in it: playing {s1['playing']}, playHeld {s1.get('playHeld')}; after Esc: playing {s2['playing']}; {told.strip()[-500:]}")
    ctl("hypr3d", "play", "off")
    machine.execute("pkill -f 'quickshell -p'; pkill -x h3dgame; true")
    ctl("eval", 'hl.unbind("SUPER + D")')

    alice(f"SDL_APP_ID=steam_app_79 SDL_VIDEO_WAYLAND_WMCLASS=steam_app_79 setsid -f stdbuf -oL h3dgame --title hintmain >> {GAME_LOG} 2>&1; sleep 0.3; "
          f"SDL_APP_ID=steam_app_79 SDL_VIDEO_WAYLAND_WMCLASS=steam_app_79 setsid -f stdbuf -oL h3dgame --size 200x150 --title hinthelper >> {GAME_LOG} 2>&1")
    both = cl_wait("both windows", lambda: (lambda t: t if "hintmain" in t and "hinthelper" in t else None)(cl_titles()), 15, 0.05)
    machine.execute("pkill -f 'title hinthelper'; true")
    hint = cl_wait("the hint", lambda: "hintmain: P plays it (Super+Esc gives the keys back)" in ctl("hypr3d", "log", "12"), 5, 0.3)
    check("31b", "a Steam game's window, then a helper of its class that goes at once: the hint still names the first", both and hint,
          f"{both}; {cl_log_since(12, 'playing h3dgame').strip()[-400:]}")
    machine.execute("pkill -x h3dgame; true")

    # aimed at a ring window under a fullscreen game: a shortcut focuses it, the game stays fullscreen
    cl_fresh(2)
    cl_game()
    ctl("dispatch", 'hl.dsp.focus({ window = "class:h3dgame" })')
    time.sleep(0.3)
    ctl("dispatch", 'hl.dsp.window.fullscreen({ action = "set", mode = "fullscreen", window = "class:h3dgame" })')
    time.sleep(1.5)
    ctl("hypr3d", "play", "off")
    time.sleep(0.8)
    fs0 = cl_fullscreen("h3dgame")
    r = ctl("hypr3d", "aim", "h3d-c1")
    time.sleep(0.4)
    s = st()
    if fs0 != 2 or (s["aimed"] or {}).get("class") != "h3d-c1":  # collectPanels draws the ring under a fullscreen one
        check("31b", "a game fullscreen in tiling mode's ring, walking: the ring's other windows drawn under it, and aimed at", False,
              f"fullscreen {fs0}; {r}; aimed {(s['aimed'] or {}).get('class')}")
    else:
        press("meta_l", "j")  # a shortcut bound to nothing, like a screenshot's
        time.sleep(1.0)
        fs1, act = cl_fullscreen("h3dgame"), cl_active()
        told = ctl("hypr3d", "log", "4")
        check("31b", "a game fullscreen, walking, the crosshair on a window of the ring under it, a Super shortcut: that window has the keyboard, the game stays fullscreen",
              fs1 == 2 and act == "h3d-c1" and "a shortcut: the keyboard to h3d-c1 (the crosshair's)" in told, f"fullscreen {fs0} -> {fs1}; the keyboard on {act}; {told.strip()[-300:]}")
        b, a, closed = cl_press("the game fullscreen, the crosshair on h3d-c1, Super+Q")
        fs2 = cl_fullscreen("h3dgame")
        check("31b", "... and Super+Q closes that window, not the game, which stays fullscreen (the keyboard to no window, not the next one)",
              closed == ["h3d-c1"] and fs2 == 2 and a["active"] is None, f"closed {closed}; the game's fullscreen then {fs2}; the keyboard on {a['active']}")
    machine.execute("pkill -x h3dgame; true")

    # aimed at a workspace 2 window from workspace 1: shortcuts switch no workspace and close nothing
    ensure_3d(False)
    clean_windows()
    machine.execute("pkill -u alice foot; true")
    cl_wait("no windows", lambda: not cl_clients(), 15, 0.3)
    ctl("hypr3d", "tile", "off")
    ctl("hypr3d", "reset-windows", "forget")
    ensure_3d()
    menu_closed()
    ctl("hypr3d", "view", "first")
    ctl("hypr3d", "spawn")
    time.sleep(0.8)
    ctl("dispatch", 'hl.dsp.focus({ workspace = "2" })')
    time.sleep(0.8)
    alice("setsid -f foot --app-id h3d-ws2 > /dev/null 2>&1")
    cl_wait("h3d-ws2 in front of you", lambda: placed("h3d-ws2"), 20, 0.3)
    time.sleep(0.5)
    ctl("dispatch", 'hl.dsp.focus({ workspace = "1" })')
    time.sleep(0.8)
    ctl("hypr3d", "turn", "90", "0")
    time.sleep(0.5)
    alice("setsid -f foot --app-id h3d-ws1 > /dev/null 2>&1")
    cl_wait("h3d-ws1 in front of you", lambda: placed("h3d-ws1"), 20, 0.3)
    time.sleep(0.8)
    ctl("hypr3d", "aim", "h3d-ws2")
    time.sleep(0.6)
    s = st()
    ws0, act0 = json.loads(ctl("-j", "activeworkspace"))["id"], cl_active()
    press("meta_l", "j")
    time.sleep(1.0)
    ws1 = json.loads(ctl("-j", "activeworkspace"))["id"]
    told = ctl("hypr3d", "log", "4")
    check("31b", "the crosshair on a window from a workspace not shown (2), a Super shortcut: still on workspace 1, the log says why the keyboard went to none",
          (s["aimed"] or {}).get("class") == "h3d-ws2" and act0 == "h3d-ws1" and ws0 == 1 and ws1 == 1 and "a shortcut: the keyboard to none (h3d-ws2 is on a workspace not shown), was h3d-ws1" in told,
          f"aimed {(s['aimed'] or {}).get('class')}; the keyboard was on {act0}; workspace {ws0} -> {ws1}; {told.strip()[-300:]}")
    b, a, closed = cl_press("the crosshair on h3d-ws2 (workspace 2), Super+Q")
    check("31b", "... and Super+Q closes nothing (not h3d-ws1, turned away from)", closed == [] and json.loads(ctl("-j", "activeworkspace"))["id"] == 1, f"closed {closed}")
    ctl("dispatch", 'hl.dsp.focus({ workspace = "1" })')

    # a scratchpad open, aimed at a window under it: Super+Space (toggle_special) must hide it for good; the shortcut
    # goes to no window, as focusing the one under would close the scratchpad and the toggle reopen it
    ensure_3d(False)
    clean_windows()
    machine.execute("pkill -u alice foot; true")
    cl_wait("no windows", lambda: not cl_clients(), 15, 0.3)
    ctl("hypr3d", "reset-windows", "forget")
    ctl("eval", 'hl.bind("SUPER + SPACE", hl.dsp.workspace.toggle_special())')
    special = lambda: next((m["specialWorkspace"]["name"] for m in json.loads(ctl("-j", "monitors")) if m["focused"]), None)  # ("": none open)
    ctl("dispatch", "hl.dsp.workspace.toggle_special()")
    alice("setsid -f foot --app-id h3d-scratch > /dev/null 2>&1")
    sc = cl_wait("the scratchpad's terminal", lambda: (lambda c: c if c and c["workspace"]["name"].startswith("special") else None)(client("h3d-scratch")), 20, 0.3)
    time.sleep(0.5)
    ctl("dispatch", "hl.dsp.workspace.toggle_special()")  # hidden, as a scratchpad usually is
    time.sleep(0.5)
    ensure_3d()
    menu_closed()
    ctl("hypr3d", "view", "first")
    ctl("hypr3d", "spawn")
    time.sleep(0.8)
    alice("setsid -f foot --app-id h3d-under > /dev/null 2>&1")
    cl_wait("h3d-under in front of you", lambda: placed("h3d-under"), 20, 0.3)
    time.sleep(0.5)
    sp0 = special()
    ctl("dispatch", "hl.dsp.workspace.toggle_special()")  # open the scratchpad
    time.sleep(0.8)
    ctl("hypr3d", "aim", "h3d-under")
    time.sleep(0.5)
    s, sp1, act1 = st(), special(), cl_active()
    frame("scratchpad-open")
    press("meta_l", "spc")
    time.sleep(1.5)
    sp2, act2 = special(), cl_active()
    told = ctl("hypr3d", "log", "4")
    check("31b", "a scratchpad (a special workspace) open over the view, its terminal with the keyboard, the crosshair on a window under it: Super+Space (its toggle) hides it, "
          "and it stays hidden; the log says why the shortcut was for no window",
          sc and sp0 == "" and (s["aimed"] or {}).get("class") == "h3d-under" and sp1 and act1 == "h3d-scratch" and sp2 == ""
          and "a shortcut: the keyboard to none (h3d-under is under a special workspace), was h3d-scratch" in told,
          f"the scratchpad {sp0!r} -> open {sp1!r} -> {sp2!r}; aimed {(s['aimed'] or {}).get('class')}; the keyboard {act1} -> {act2}; {told.strip()[-300:]}")
    ctl("eval", 'hl.unbind("SUPER + SPACE")')

    note("31b", "the log", ctl("hypr3d", "log", "30").strip()[-2500:])
    ctl("eval", 'hl.unbind("SUPER + Q")')
    ctl("hypr3d", "play", "off")
    ctl("hypr3d", "tile", "off")
    ctl("hypr3d", "reset-windows", "forget")
    clean_windows()
    machine.execute("pkill -u alice foot; pkill -f slowclose[.]py; true")
    ensure_3d(False)


@section("exit", "Hyprland exits cleanly with windows open, dwindle and master (0.55.2 crashes, plugin or not: its own bug)")
def s_exit_windows():
    # Hyprland 0.55.2's CCompositor::cleanup drops windows before their clients, so dwindle uses an expired target
    # (fixed in 0.56.0, upstream 338bdbb3); stop_hyprland() closes terminals first for that reason, here they stay open
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
    # master crashes the same way (CMasterAlgorithm::calculateWorkspace -> ITarget::setPositionGlobal on an expired
    # target); 338bdbb3 doesn't cover it, upstream main (e368c13c) still lacks a guard
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
    # Hyprland's cursor over the left terminal (tablet coordinates 0..32767)
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
    # the crosshair against the sky: 22 logical px across, scaled
    ctl("hypr3d", "turn", "0", "60")
    time.sleep(0.8)
    sky = frame(f"{item}-crosshair")
    green = lambda r, g, b: g > 200 and r < 140 and b < 150
    row = [x for x in range(sky.w // 2 - 80, sky.w // 2 + 80) if green(*sky.px[(sky.h // 2 * sky.w + x) * 3:(sky.h // 2 * sky.w + x) * 3 + 3])]
    span = row[-1] - row[0] + 1 if row else 0
    check(item, f"the crosshair is drawn at scale {scale:g}: {round(22 * scale)} px across", abs(span - 22 * scale) <= 2, f"{span} px")
    ctl("hypr3d", "turn", "0", "0")
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
    # the Action Menu: radius 28% of the screen's height; the mouse moves its cursor in logical px
    ctl("hypr3d", "spawn")
    ctl("hypr3d", "view", "third")
    ctl("hypr3d", "turn", "0", "60")  # sky behind, so its dark ring stands out
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
    # a chord 60 px below the middle, clear of the 3 and 9 o'clock ticks
    y, dy = img.h // 2 + 60, 60
    dark = [x for x in range(img.w // 2, img.w) if max(img.px[(y * img.w + x) * 3:(y * img.w + x) * 3 + 3]) < 70]
    across, chord = (dark[-1] - dark[0] + 1 if dark else 0), 2 * math.sqrt(336 ** 2 - dy ** 2)
    check(item, f"the dial's ring is drawn 336 px out from its middle ({chord:.0f} px across, 60 px below it)", abs(across - chord) <= 8, f"{across} px of dark ring")
    press("backspace")
    press("esc")
    menu_closed()
    ctl("hypr3d", "turn", "0", "0")
    # the lip sync badge: its size follows the scale
    mic_noise()  # silence would put a notification over the corner
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


# ------------------------------------------------------------------ play mode in place: the game where it is, the others round it

PC_LOCK, PC_FREE = "h3dgame-pc-lock", "h3dgame-pc-free"  # opaque via the Lua config's h3dgame.* rule


def pc_game(cls, args=""):
    """starts h3dgame as alice, logging what it gets to /tmp/CLS.log; returns its window"""
    alice(f"rm -f /tmp/{cls}.log; SDL_APP_ID={cls} SDL_VIDEO_WAYLAND_WMCLASS={cls} setsid -f h3dgame --title {cls} --log /tmp/{cls}.log {args} > /dev/null 2>&1")
    return wait_for(f"{cls}'s window", lambda: client(cls), 30, 0.5)


def pc_mark(cls, name):
    alice(f"echo {shlex.quote('### ' + name)} >> /tmp/{cls}.log")


def pc_lines(cls, name):
    """what the game printed since mark `name` (to the next one)"""
    out, on = [], False
    for line in machine.succeed(f"cat /tmp/{cls}.log").splitlines():
        if line.startswith("### "):
            if on:
                break
            on = line[4:].strip() == name
            continue
        if on:
            out.append(line.strip())
    return out


def pc_wait(what, fn, timeout=10, every=0.2):
    """wait_for, but None on timeout so the section goes on"""
    try:
        return wait_for(what, fn, timeout, every)
    except TimeoutError:
        return None


def pc_playing(cls, facing=False):
    """the status's playing once cls is played (with facing, once the camera faces it); {} if not"""
    s = pc_wait(f"{cls} played", lambda: (lambda s: s if s["playing"] and s["playing"]["class"] == cls and (not facing or s["playing"]["view"] >= 1) else None)(st()))
    return s["playing"] if s else {}


def pc_aim(cls):
    """the crosshair on the middle of that window"""
    ctl("hypr3d", "aim", cls)
    time.sleep(0.5)
    return aim_find(cls)


def pc_steady(timeout=6):
    """the camera, once it's still (third person's boom eases out)"""
    last = [None]

    def still():
        e = st()["eye"]
        done = last[0] is not None and math.dist(e, last[0]) < 0.0005
        last[0] = e
        return e if done else None
    return pc_wait("the camera still", still, timeout, 0.3)


def pc_cam():
    """the drawn camera (hyprctl hypr3d camera: eye, yaw, pitch, up; the status's yaw and pitch are the player's, which
    play mode never turns); without that command, the status's eye, yaw and pitch, no up"""
    try:
        return json.loads(ctl("hypr3d", "camera"))
    except json.JSONDecodeError:
        s = st()
        return {"eye": s["eye"], "yaw": s["yaw"], "pitch": s["pitch"], "up": None}


def pc_turned(c, c0):
    """largest turn from c0 to c in degrees: yaw, pitch or roll (from up)"""
    a = max(abs((c["yaw"] - c0["yaw"] + 180) % 360 - 180), abs(c["pitch"] - c0["pitch"]))
    if c.get("up") and c0.get("up"):  # angle from the chord: exactly 0 when still
        a = max(a, math.degrees(2 * math.asin(min(1.0, math.dist(c["up"], c0["up"]) / 2))))
    return a


def pc_still(c0, secs=2.0):
    """over `secs`: the most the camera moved from c0 (metres), turned (degrees), and the status's play view"""
    moved = turned = view = 0.0
    end = time.time() + secs
    while True:
        c = pc_cam()
        moved = max(moved, math.dist(c["eye"], c0["eye"]))
        turned = max(turned, pc_turned(c, c0))
        view = max(view, (st()["playing"] or {}).get("view", 0))
        if time.time() >= end:
            return moved, turned, view
        time.sleep(0.25)


def pc_others(cls):
    """inView of the other windows, by class"""
    return {p["class"]: p.get("inView") for p in panels() if p["kind"] == "window" and p["class"] != cls}


def pc_in_view(cls):
    """the window's panel inView (hyprctl hypr3d panels); None if unknown"""
    return next((p.get("inView") for p in panels() if p["kind"] == "window" and p["class"] == cls), None)


def pc_off_line(p, a, b):
    """distance from p to the segment a-b (metres)"""
    ab, ap = [b[i] - a[i] for i in range(3)], [p[i] - a[i] for i in range(3)]
    n = sum(x * x for x in ab)
    t = max(0.0, min(1.0, sum(ap[i] * ab[i] for i in range(3)) / n)) if n > 0 else 0.0
    return math.dist(p, [a[i] + ab[i] * t for i in range(3)])


def pc_cross(img):
    """crosshair pixels at the frame's middle: green, aimed orange or typing blue"""
    cx, cy = img.w // 2, img.h // 2
    return img.count(lambda r, g, b: (g > 200 and r < 140 and b < 150) or (r > 230 and 180 < g < 230 and b < 140) or (90 < r < 150 and 190 < g < 240 and b > 230),
                     (cx - 12, cy - 12, cx + 13, cy + 13))


def pc_span(img):
    """the larger share of the middle column or row that is the game's background"""
    cx, cy = img.w // 2, img.h // 2
    col = sum(1 for y in range(img.h) if game_blue(*img.px[(y * img.w + cx) * 3:(y * img.w + cx) * 3 + 3]))
    row = sum(1 for x in range(img.w) if game_blue(*img.px[(cy * img.w + x) * 3:(cy * img.w + x) * 3 + 3]))
    return max(col / img.h, row / img.w)


def pc_mid_game(img):
    """share of game background just up-left of the frame's middle (the app's cursor draws down-right of it)"""
    cx, cy = img.w // 2, img.h // 2
    return img.count(game_blue, (cx - 14, cy - 14, cx - 4, cy - 4)) / 100


def pc_active():
    return json.loads(ctl("-j", "activewindow") or "{}").get("class")


@section("31c", "play mode in place (P): the game played where it is, the view as it was and the other windows round it (first and third person, tiling mode's ring, the game moving in it), its keys, buttons, wheel and mouse (locked, and a pointer over it); Super+Esc walking on from there; Shift+P or play_view = fill filling the view; hyprctl hypr3d play here|fill and switching while playing, Lua's play(), the hypr3d:play dispatcher")
def s_play_here():
    lua_session()
    ensure_avatar(AV)
    ensure_3d(False)
    clean_windows()
    ctl("hypr3d", "tile", "off")
    ctl("hypr3d", "tile", "follow", "on")
    ctl("hypr3d", "reset-windows", "forget")
    ctl("eval", 'hl.config({ plugin = { hypr3d = { play_view = "here" } } })')  # the default; earlier sections may change it
    machine.execute("pkill -u alice foot; true")
    wait_for("no windows", lambda: not json.loads(ctl("-j", "clients")), 10, 0.3)
    # on the wall: two terminals, a game locking the pointer (SDL relative mode) and one that doesn't
    alice("setsid -f foot --app-id h3d-left > /dev/null 2>&1; sleep 0.7; setsid -f foot --app-id h3d-right > /dev/null 2>&1")
    wait_for("two terminals", lambda: len(json.loads(ctl("-j", "clients"))) >= 2, 20)
    pc_game(PC_LOCK, "--relative")
    pc_game(PC_FREE)
    time.sleep(1.5)
    ensure_3d()
    menu_closed()
    ctl("hypr3d", "view", "first")
    ctl("hypr3d", "spawn")
    time.sleep(1)

    a = pc_aim(PC_LOCK)
    check("31c", "four windows on the wall (two terminals, two games), the crosshair on the middle of the game that locks the pointer", a and len(pc_others(PC_LOCK)) == 3,
          a or AIM.get("last"))
    before = calm_frame("31c-before")
    s0, c0 = st(), pc_cam()
    press("p")
    p = pc_playing(PC_LOCK)
    told = ctl("hypr3d", "log", "4")
    check("31c", "P: playing it here (not filling the view), its pointer lock active (the keyboard focus on it)", p.get("fill") is False and p.get("locked") and pc_active() == PC_LOCK, p)
    check("31c", "... the notification says so, and how to fill the view instead", "here: Super+Esc gives the mouse and keyboard back (Shift+P fills the view)" in told,
          told.strip()[-240:])
    moved, turned, view = pc_still(c0)
    check("31c", "... the camera doesn't move: over 2 s its eye, yaw, pitch and up as they were (hyprctl hypr3d camera), and it never goes to face the game", moved < 0.001 and turned < 0.01 and view == 0,
          f"eye {moved * 100:.1f} cm, {turned:.2f}°, view {view}")
    others = pc_others(PC_LOCK)
    check("31c", "... the other windows still in the view round it (each one's middle in it, hyprctl hypr3d panels)", len(others) == 3 and all(v is True for v in others.values()), others)
    img = calm_frame("31c-here")
    d = before.differs(img)
    check("31c", "... and drawn as they were: the frame is the one before P (but for the crosshair and the aimed window's outline)", d < 0.05, f"{d:.1%} of the frame changed")
    check("31c", "... no crosshair over it (there was one before P: its dot, by the game's cursor)", pc_cross(before) >= 2 and pc_cross(img) < 2,
          f"{pc_cross(before)} -> {pc_cross(img)} crosshair pixels in the middle")
    # its keys, buttons, wheel and mouse, as ever
    feet = st()["feet"]
    pc_mark(PC_LOCK, "keys")
    press("w")
    press("a")
    press("esc")
    press("tab")
    got = [l for l in pc_lines(PC_LOCK, "keys") if l.startswith("key down")]
    s = st()
    check("31c", "keys, Esc and Tab too, reach the game played here", got == ["key down W", "key down A", "key down Escape", "key down Tab"], got)
    check("31c", "... and not the player: no walking, no Action Menu, still in 3D", math.dist(s["feet"], feet) < 0.01 and not menu().get("open") and s["mode"] == "active",
          f"feet {s['feet']}, menu {menu().get('open')}, {s['mode']}")
    pc_mark(PC_LOCK, "buttons")
    click("left")
    click("right")
    wheel(1)
    got = [" ".join(l.split()[:3]) for l in pc_lines(PC_LOCK, "buttons") if l.startswith(("button", "wheel"))]
    check("31c", "buttons and the wheel reach it", got[:4] == ["button down 1", "button up 1", "button down 3", "button up 3"] and any(l.startswith("wheel 0 -") for l in got), got)
    pc_mark(PC_LOCK, "mouse")
    rel(100, 0)
    rel(0, 50)
    dx, dy, _ = game_motion(pc_lines(PC_LOCK, "mouse"))
    moved, turned, view = pc_still(c0, 0)
    check("31c", "the mouse, locked: its relative motion reaches it (100 counts right, 50 down), and the camera stays where it was",
          90 <= dx <= 110 and 45 <= dy <= 55 and moved < 0.001 and turned < 0.01 and view == 0, f"dx {dx}, dy {dy}; eye {moved * 100:.1f} cm, {turned:.2f}°")
    # Super+Esc: walking at once; the view never moved, so nothing to return from
    pc_mark(PC_LOCK, "super-esc")
    press("meta_l", "esc", after=0)
    s, c = st(), pc_cam()
    got = pc_lines(PC_LOCK, "super-esc")
    check("31c", "Super+Esc: walking again at once, the camera as it was, the game gets no Esc",
          s["playing"] is None and s["typing"] is False and math.dist(c["eye"], c0["eye"]) < 0.001 and pc_turned(c, c0) < 0.01 and not any("Escape" in l for l in got),
          f"playing {s['playing']}, typing {s['typing']}, eye {math.dist(c['eye'], c0['eye']) * 100:.1f} cm, {pc_turned(c, c0):.2f}°; {got}")
    rel(60, 0)
    check("31c", "... and the mouse turns the camera again", abs(st()["yaw"] - s0["yaw"]) > 3, st()["yaw"])

    # the game that doesn't lock the pointer
    a = pc_aim(PC_FREE)
    c0 = pc_cam()
    press("p")
    p = pc_playing(PC_FREE)
    check("31c", "the other game (no lock), P: played here too, the pointer not locked", a and p.get("fill") is False and not p.get("locked"), p)
    pc_mark(PC_FREE, "pointer")
    rel(40, 0)
    x0 = game_motion(pc_lines(PC_FREE, "pointer"))[2]
    rel(60, 30)
    x1 = game_motion(pc_lines(PC_FREE, "pointer"))[2]
    check("31c", "... the pointer moves over it: the game gets positions that follow the mouse", x0 and x1 and x1[0] > x0[0] and x1[1] > x0[1], f"{x0} -> {x1}")
    rel(3000, 0)
    rel(0, 3000)
    x2 = game_motion(pc_lines(PC_FREE, "pointer"))[2]
    w, h = client(PC_FREE)["size"]
    s = st()
    check("31c", "... and stays on it, as on a monitor: at its bottom right corner, its own cursor drawn there", x2 and w - 3 <= x2[0] <= w and h - 3 <= x2[1] <= h and s["cursor"],
          f"{x2}, the window {w}x{h}; cursor {s['cursor']}")
    pc_mark(PC_FREE, "click")
    click("left")
    got = [l.split() for l in pc_lines(PC_FREE, "click") if l.startswith("button down")]
    check("31c", "... a click lands where the pointer is", got and x2 and abs(int(got[0][3]) - x2[0]) <= 2 and abs(int(got[0][4]) - x2[1]) <= 2, f"{got}, pointer {x2}")
    moved, turned, view = pc_still(c0, 0.5)
    check("31c", "... and the camera stays where it was all along", moved < 0.001 and turned < 0.01 and view == 0, f"eye {moved * 100:.1f} cm, {turned:.2f}°, view {view}")
    press("meta_l", "esc")

    ctl("hypr3d", "view", "third")
    ctl("hypr3d", "spawn")
    time.sleep(1)
    a = pc_aim(PC_LOCK)
    pc_steady()
    c0 = pc_cam()
    press("p")
    p = pc_playing(PC_LOCK)
    moved, turned, view = pc_still(c0)
    img = frame("31c-here-third")
    check("31c", "third person, P: played here, the camera where it was behind the avatar over 2 s (its eye, yaw, pitch and up), never going to face the game",
          a and p.get("fill") is False and st()["view"] == "third" and moved < 0.001 and turned < 0.01 and view == 0, f"{p}; eye {moved * 100:.1f} cm, {turned:.2f}°, view {view}")
    pc_mark(PC_LOCK, "third")
    press("d")
    rel(50, 0)
    got = pc_lines(PC_LOCK, "third")
    dx = game_motion(got)[0]
    check("31c", "... its keys and the mouse reach it, no crosshair over it", "key down D" in got and 45 <= dx <= 55 and pc_cross(img) < 3,
          f"{[l for l in got if l.startswith('key')]}, dx {dx}; {pc_cross(img)} crosshair pixels")
    press("meta_l", "esc", after=0)
    s, c = st(), pc_cam()
    check("31c", "... Super+Esc: walking at once, the camera where it was", s["playing"] is None and math.dist(c["eye"], c0["eye"]) < 0.001 and pc_turned(c, c0) < 0.01,
          f"playing {s['playing']}, eye {math.dist(c['eye'], c0['eye']) * 100:.1f} cm, {pc_turned(c, c0):.2f}°")
    ctl("hypr3d", "view", "first")

    ctl("hypr3d", "spawn")
    time.sleep(0.8)
    pc_aim(PC_LOCK)
    s0 = st()
    press("shift", "p")
    p = pc_playing(PC_LOCK, facing=True)
    time.sleep(0.5)
    span = pc_span(before)
    img = frame("31c-fill")
    moved = math.dist(st()["eye"], s0["eye"])
    check("31c", "Shift+P: played filling the view (the status's view 1), the camera facing it: the game across the frame (its band at the top aside)",
          p.get("fill") is True and p.get("view", 0) >= 1 and pc_span(img) > 0.7 and moved > 0.5,
          f"{p}; the game {pc_span(img):.0%} across the frame (played here {span:.0%}), the eye {moved:.2f} m from where it was")
    press("meta_l", "esc")
    back = pc_wait("the camera back", lambda: math.dist(st()["eye"], s0["eye"]) < 0.001, 5)
    check("31c", "... Super+Esc: the camera comes back to you", back, math.dist(st()["eye"], s0["eye"]))

    r = ctl("eval", 'hl.config({ plugin = { hypr3d = { play_view = "fill" } } })')
    try:
        pc_aim(PC_LOCK)
        press("p")
        p1 = pc_playing(PC_LOCK, facing=True)
        press("meta_l", "esc")
        pc_wait("the camera back", lambda: math.dist(st()["eye"], s0["eye"]) < 0.001, 5)
        pc_aim(PC_LOCK)
        s1, c1 = st(), pc_cam()
        press("shift", "p")
        p2 = pc_playing(PC_LOCK)
        moved, turned, view = pc_still(c1, 1)
        press("meta_l", "esc")
    finally:
        ctl("eval", 'hl.config({ plugin = { hypr3d = { play_view = "here" } } })')
    check("31c", "plugin:hypr3d:play_view = fill (set at run time): P fills the view, Shift+P plays it here, the camera staying", r == "ok" and p1.get("fill") is True
          and p2.get("fill") is False and moved < 0.001 and view == 0, f"{r}; P {p1}; Shift+P {p2}, eye {moved * 100:.1f} cm, view {view}")

    pc_wait("the camera back", lambda: math.dist(st()["eye"], s1["eye"]) < 0.001, 5)
    pc_aim(PC_LOCK)
    s0 = st()
    r1 = ctl("hypr3d", "play", "on", "here")
    p1 = st()["playing"] or {}
    r2 = ctl("hypr3d", "play", "fill")
    p2 = pc_playing(PC_LOCK, facing=True)
    r3 = ctl("hypr3d", "play", "here")
    # wait for the eye, not view 0: the play view runs a frame ahead of the status's eye
    s3 = pc_wait("the camera back", lambda: (lambda s: s if s["playing"] and math.dist(s["eye"], s0["eye"]) < 0.001 else None)(st()), 5) or st()
    p3 = s3["playing"] or {}
    check("31c", "hyprctl hypr3d play on here: played here; play fill while playing switches: the camera goes to face it; play here: back where it was, still played",
          r1 == r2 == r3 == "playing" and p1.get("fill") is False and p1.get("view") == 0 and p2.get("fill") is True and p3.get("fill") is False and p3.get("view") == 0
          and p3.get("class") == PC_LOCK and math.dist(s3["eye"], s0["eye"]) < 0.001, f"{r1} {p1}; {r2} {p2}; {r3} {p3}, eye {math.dist(s3['eye'], s0['eye']) * 100:.1f} cm")
    r4 = ctl("hypr3d", "play", "off")
    r5 = ctl("hypr3d", "play", "on", "fill")
    p5 = pc_playing(PC_LOCK, facing=True)
    r6 = ctl("hypr3d", "play", "toggle")
    r7 = ctl("hypr3d", "play", "sideways")
    check("31c", "... play off; play on fill: playing, filling the view; play toggle: off; a word it doesn't know: an error, saying which it takes",
          r4 == r6 == "walking" and r5 == "playing" and p5.get("fill") is True and "[here|fill]" in r7 and r7.startswith("error") and st()["playing"] is None, f"{r4}; {r5} {p5}; {r6}; {r7}")

    pc_wait("the camera back", lambda: math.dist(st()["eye"], s0["eye"]) < 0.001, 5)
    pc_aim(PC_LOCK)
    r1 = ctl("eval", 'local r = hl.plugin.hypr3d.play("fill"); if r ~= "playing" then error("gave " .. tostring(r)) end')
    p1 = pc_playing(PC_LOCK, facing=True)
    r2 = ctl("eval", 'local r = hl.plugin.hypr3d.play("here"); if r ~= "playing" then error("gave " .. tostring(r)) end')
    p2 = st()["playing"] or {}
    r3 = ctl("eval", 'local r = hl.plugin.hypr3d.play(); if r ~= "walking" then error("gave " .. tostring(r)) end')
    check("31c", 'hl.plugin.hypr3d.play("fill"): played filling the view; play("here"): switched to here; play(): stopped (each saying so)',
          r1 == r2 == r3 == "ok" and p1.get("fill") is True and p2.get("fill") is False and st()["playing"] is None, f"{r1} {p1}; {r2} {p2}; {r3}")

    # play off, then at once another window played here: the camera returns the way it went, never veering to the second
    pc_wait("the camera back", lambda: math.dist(st()["eye"], s0["eye"]) < 0.001, 5)
    pc_aim(PC_LOCK)
    c0 = pc_cam()
    r1 = ctl("hypr3d", "play", "on", "fill")
    pc_playing(PC_LOCK, facing=True)
    time.sleep(0.5)
    e1 = pc_cam()["eye"]
    out = as_alice("hyprctl -i 0 hypr3d play off; hyprctl -i 0 hypr3d window h3d-left play; "
                   "for i in $(seq 50); do hyprctl -i 0 hypr3d status; echo; sleep 0.01; done")[1]
    ss = [json.loads(l) for l in out.splitlines() if l.startswith("{")]
    off = max((pc_off_line(x["eye"], c0["eye"], e1) for x in ss), default=9.0)
    last = ss[-1] if ss else {"eye": [9, 9, 9], "playing": None}
    p = last["playing"] or {}
    check("31c", "a window played here (window SEL play) the moment one played filling the view stops (play off): the camera comes back from the first the way it went, "
          "never off towards the second, and ends where it was", r1 == "playing" and len(ss) > 20 and off < 0.05 and math.dist(last["eye"], c0["eye"]) < 0.001
          and p.get("class") == "h3d-left" and p.get("fill") is False,
          f"{len(ss)} samples, at most {off * 100:.1f} cm off the way back; the last eye {last['eye']}, where it was {c0['eye']}; playing {p}")
    out = as_alice("hyprctl -i 0 hypr3d play fill; for i in $(seq 50); do hyprctl -i 0 hypr3d status; echo; sleep 0.01; done")[1]
    ss = [json.loads(l) for l in out.splitlines() if l.startswith("{")]
    time.sleep(0.5)
    e2 = pc_cam()["eye"]
    off = max((pc_off_line(x["eye"], c0["eye"], e2) for x in ss), default=9.0)
    p = st()["playing"] or {}
    check("31c", "... then play fill, the camera all the way back meanwhile: it goes straight to that window, not by way of the first one",
          len(ss) > 20 and off < 0.02 and p.get("class") == "h3d-left" and p.get("fill") is True and p.get("view", 0) >= 1,
          f"{len(ss)} samples, at most {off * 100:.1f} cm off the way there ({c0['eye']} -> {e2}); playing {p}")
    press("meta_l", "esc")

    # Play on the Windows page on a game behind you: you turn to face it, the eye stays put
    ctl("hypr3d", "spawn")
    time.sleep(0.8)
    pc_aim(PC_LOCK)
    ctl("hypr3d", "turn", f"{st()['yaw'] + 180:.1f}", "0")
    time.sleep(0.8)
    c0 = pc_cam()
    seen0 = pc_in_view(PC_LOCK)
    press("b")
    time.sleep(0.8)
    slot = next((i["slot"] for i in menu().get("items", []) if i["label"] == PC_LOCK), None)
    press(str(slot or 1))
    time.sleep(0.3)
    act = next((i["slot"] for i in menu().get("items", []) if i["label"] == "Play"), None)
    press(str(act or 7))
    p = pc_playing(PC_LOCK)
    moved, _, view = pc_still(c0, 1)
    c1 = pc_cam()
    seen1 = pc_in_view(PC_LOCK)
    told = ctl("hypr3d", "log", "6")
    img = frame("31c-play-behind")
    check("31c", "Play on the Windows page (B, the game, Play) on a game behind you: played here, and you turn to face it: its middle in the view, the eye where it was, "
          "the camera never going to face it", slot and act and p.get("fill") is False and seen0 is False and seen1 is True and pc_turned(c1, c0) > 90 and moved < 0.001
          and view == 0 and pc_mid_game(img) > 0.8, f"slots {slot}, {act}; {p}; its middle in the view {seen0} -> {seen1}; turned {pc_turned(c1, c0):.0f}°, "
          f"eye {moved * 100:.1f} cm, view {view}; the game's background {pc_mid_game(img):.0%} of the middle of the frame; {told.strip()[-160:]}")
    press("meta_l", "esc", after=0)
    c2 = pc_cam()
    check("31c", "... Super+Esc: walking on from there, facing it", st()["playing"] is None and pc_turned(c2, c1) < 0.01 and math.dist(c2["eye"], c1["eye"]) < 0.001,
          f"{pc_turned(c2, c1):.2f}°, eye {math.dist(c2['eye'], c1['eye']) * 100:.1f} cm")

    # a game typed into (E) going fullscreen behind you: auto-played here, turning you to face it
    ctl("hypr3d", "spawn")
    time.sleep(0.8)
    pc_aim(PC_FREE)
    press("e")
    ctl("hypr3d", "turn", f"{st()['yaw'] + 180:.1f}", "0")
    time.sleep(0.8)
    c0 = pc_cam()
    typing = st()["typing"]
    seen0 = pc_in_view(PC_FREE)
    pc_mark(PC_FREE, "fs")
    press("f")
    p = pc_playing(PC_FREE)
    time.sleep(1)
    c1 = pc_cam()
    seen1 = pc_in_view(PC_FREE)
    got = [l for l in pc_lines(PC_FREE, "fs") if l.startswith("size ")]
    img = frame("31c-fullscreen-behind")
    check("31c", "a game going fullscreen while you look away (typed into, then turned from; F in it): played by itself, here, and you turn to face it, its middle in the "
          "view, the eye where it was", typing and got and p.get("fill") is False and seen0 is False and seen1 is True and pc_turned(c1, c0) > 90
          and math.dist(c1["eye"], c0["eye"]) < 0.001 and pc_mid_game(img) > 0.8, f"typing {typing}; {got}; {p}; its middle in the view {seen0} -> {seen1}; "
          f"turned {pc_turned(c1, c0):.0f}°, eye {math.dist(c1['eye'], c0['eye']) * 100:.1f} cm; the game's background {pc_mid_game(img):.0%} of the middle of the frame")
    pc_mark(PC_FREE, "out")
    press("f")
    out = pc_wait("out of fullscreen", lambda: (lambda c: c if c and c["fullscreen"] == 0 else None)(client(PC_FREE)), 10, 0.3)
    time.sleep(0.8)
    s = st()
    check("31c", "... F again, out of fullscreen: still played, the keys the app's (as on the 2D desktop)", out and (s["playing"] or {}).get("class") == PC_FREE and s["typing"],
          f"fullscreen {(client(PC_FREE) or {}).get('fullscreen')}; playing {s['playing']}, typing {s['typing']}")
    # the pointer, at the fullscreen middle, stays put when the window shrinks, just clamped onto it
    ptr, size, lines = (s["playing"] or {}).get("pointer"), (client(PC_FREE) or {}).get("size"), pc_lines(PC_FREE, "out")
    check("31c", "... the play pointer on it, and no mouse leave for the game",
          bool(ptr and size and 0 <= ptr[0] < size[0] and 0 <= ptr[1] < size[1]) and "mouse leave" not in lines,
          f"pointer {ptr}, the window {size}; the game got {lines[-8:]}")
    press("meta_l", "esc")
    s = pc_wait("walking", lambda: (lambda s: s if s["playing"] is None else None)(st()), 10, 0.3)
    check("31c", "... Super+Esc: walking again", s and s["typing"] is False, s and s["typing"])
    time.sleep(1)

    # a pinned window played: here in its corner; play fill faces it there without it running ahead of the camera;
    # Super+Esc: back in its corner
    ctl("hypr3d", "spawn")
    time.sleep(0.8)
    r0 = ctl("hypr3d", "window", "h3d-right", "pin")
    time.sleep(1)
    c0 = pc_cam()
    g0 = placed("h3d-right") or {}
    r1 = ctl("hypr3d", "window", "h3d-right", "play")
    p1 = pc_playing("h3d-right")
    moved, turned, view = pc_still(c0, 1)
    g1 = placed("h3d-right") or {}
    seen1 = pc_in_view("h3d-right")
    went = math.dist(g1["center"], g0["center"]) if g0 and g1 else 9.0
    check("31c", "a window pinned to the view, Play (hyprctl hypr3d window SEL play): played here, in its corner as it was, the camera staying",
          r0 == "pinned" and r1 == "playing" and p1.get("fill") is False and g1.get("pinned") and went < 0.01 and seen1 is True and moved < 0.001 and turned < 0.01 and view == 0,
          f"{r0} {r1}; {p1}; pinned {g1.get('pinned')}, it moved {went:.3f} m, its middle in the view {seen1}; eye {moved * 100:.1f} cm, {turned:.2f}°, view {view}")
    r2 = ctl("hypr3d", "play", "fill")
    eyes = []
    for _ in range(8):
        time.sleep(0.25)
        eyes.append(pc_cam()["eye"])
    p2 = st()["playing"] or {}
    seen2 = pc_in_view("h3d-right")
    frame("31c-pinned-fill")
    far = max(math.dist(e, c0["eye"]) for e in eyes)
    check("31c", "... play fill: the camera goes to face it in the corner and stays there (the window doesn't go on ahead of it)",
          r2 == "playing" and p2.get("fill") is True and p2.get("view", 0) >= 1 and seen2 is True and far < 1.5 and math.dist(eyes[-1], eyes[-3]) < 0.005,
          f"{r2}; {p2}; its middle in the view {seen2}; the eye over 2 s: {[[round(v, 2) for v in e] for e in eyes]}")
    press("meta_l", "esc")
    back = pc_wait("the camera back", lambda: math.dist(pc_cam()["eye"], c0["eye"]) < 0.001, 5)
    g3 = placed("h3d-right") or {}
    went = math.dist(g3["center"], g0["center"]) if g0 and g3 else 9.0
    check("31c", "... Super+Esc: the camera back where it was, the window pinned in its corner again", back and g3.get("pinned") and went < 0.01,
          f"camera back {bool(back)}; pinned {g3.get('pinned')}, {went:.3f} m from where it was")
    ctl("hypr3d", "window", "h3d-right", "unpin")
    ctl("hypr3d", "window", "h3d-right", "wall")
    time.sleep(1)

    r = ctl("eval", 'hl.config({ plugin = { hypr3d = { play_view = "full" } } })')
    try:
        told = pc_wait("the value said", lambda: (lambda l: l if "not full" in l else None)(ctl("hypr3d", "log", "8")), 5)
        pc_aim(PC_LOCK)
        press("p")
        p = pc_playing(PC_LOCK)
        press("meta_l", "esc")
        time.sleep(1.5)
        times = ctl("hypr3d", "log", "40").count("not full")
    finally:
        ctl("eval", 'hl.config({ plugin = { hypr3d = { play_view = "here" } } })')
    check("31c", "plugin:hypr3d:play_view = full (neither here nor fill): a notification says so, once, and P plays here", r == "ok" and told and times == 1 and p.get("fill") is False,
          f"{r}; said {times} time(s): {(told or '').strip()[-160:]}; {p}")

    # third person: Play on a game behind you turns you to face it past the avatar
    ctl("hypr3d", "view", "third")
    ctl("hypr3d", "spawn")
    time.sleep(1)
    pc_aim(PC_FREE)
    ctl("hypr3d", "turn", f"{st()['yaw'] + 180:.1f}", "0")
    pc_steady()
    seen0 = pc_in_view(PC_FREE)
    r = ctl("hypr3d", "window", PC_FREE, "play")
    p = pc_playing(PC_FREE)
    pc_steady()
    _, turned, view = pc_still(pc_cam(), 0.5)
    seen1 = pc_in_view(PC_FREE)
    img = frame("31c-play-behind-third")
    check("31c", "third person: Play on a game behind you: played here, turned to face it past the avatar (its middle in the view), the camera then still, never going to face it",
          r == "playing" and p.get("fill") is False and seen0 is False and seen1 is True and turned < 0.01 and view == 0 and st()["view"] == "third" and pc_mid_game(img) > 0.8,
          f"{r}; {p}; its middle in the view {seen0} -> {seen1}; then {turned:.2f}°, view {view}; the game's background {pc_mid_game(img):.0%} of the middle of the frame")
    press("meta_l", "esc")
    ctl("hypr3d", "view", "first")

    # the ring: the game played with its neighbours in view; a new window joins beside it, the game and view staying put
    pc_wait("the camera back", lambda: math.dist(st()["eye"], s0["eye"]) < 0.001, 5)
    # sizes small enough that the neighbours' middles stay in view beside the game at half the view: under a quarter
    # wide
    for cls, size in (("h3d-left", "280 240"), ("h3d-right", "280 240"), (PC_LOCK, "400 250"), (PC_FREE, "280 240")):
        ctl("hypr3d", "window", cls, "size", *size.split())
    time.sleep(1)
    ctl("hypr3d", "spawn")
    time.sleep(0.8)
    press("t")
    w = settled(10)
    row = [x["class"] for x in ring_view(w)]
    a = aim_row(PC_LOCK)
    time.sleep(0.5)
    i = row.index(PC_LOCK) if PC_LOCK in row else -1
    near = [row[j] for j in (i - 1, i + 1) if 0 <= j < len(row)] if i >= 0 else []
    s0, c0 = st(), pc_cam()
    g0 = (placed(PC_LOCK) or {}).get("center")
    press("p")
    p = pc_playing(PC_LOCK)
    moved, turned, view = pc_still(c0, 1.5)
    inview = pc_others(PC_LOCK)
    frame("31c-ring-here")
    check("31c", "tiling mode: the game in the ring, P: played here, the camera where it was, its neighbours in the row round it in the view",
          a and p.get("fill") is False and moved < 0.001 and turned < 0.01 and view == 0 and near and all(inview.get(n) is True for n in near),
          f"row {row}, its neighbours {near}: {inview}; eye {moved * 100:.1f} cm, {turned:.2f}°, view {view}")
    pc_mark(PC_LOCK, "ring")
    press("s")
    got = pc_lines(PC_LOCK, "ring")
    check("31c", "... its keys reach it", "key down S" in got, got)
    pc_wait("the row settled", lambda: all(q["settled"] for q in windows3d()["placed"]), 10, 0.3)
    g0 = (placed(PC_LOCK) or {}).get("center")  # played: half the view, where you look
    alice("setsid -f foot --app-id h3d-pc-new > /dev/null 2>&1")
    pc_wait("the new terminal in the row", lambda: "h3d-pc-new" in tiled_row(), 20, 0.5)
    pc_wait("the row settled", lambda: all(q["settled"] for q in windows3d()["placed"]), 10, 0.3)
    s, c, row = st(), pc_cam(), tiled_row()
    g1 = (placed(PC_LOCK) or {}).get("center")
    check("31c", "... a window opening meanwhile goes into the row beside it: the game stays where it is (the row turning round it), still played (its lock keeps the "
          "keyboard), and the view stays where it was", s["playing"] and s["playing"]["class"] == PC_LOCK and "h3d-pc-new" in row and g0 and g1 and math.dist(g0, g1) < 0.02
          and math.dist(c["eye"], c0["eye"]) < 0.001 and pc_turned(c, c0) < 0.01,
          f"row {row}; the game {g0} -> {g1}; eye {math.dist(c['eye'], c0['eye']) * 100:.1f} cm, {pc_turned(c, c0):.2f}°; playing {s['playing']}")
    frame("31c-ring-moved")
    press("meta_l", "esc")
    ctl("hypr3d", "window", "h3d-pc-new", "close")
    ctl("hypr3d", "tile", "off")
    ensure_3d(False)
    machine.execute("pkill -f 'h3dgame --title h3dgame-pc'; true")

    # the hypr3d:play dispatcher with an argument, from hyprland.conf (Lua uses play())
    start_hyprland("hyprland.conf", conf_config({"avatar": AV}))
    ensure_avatar(AV)
    ensure_3d()
    menu_closed()
    ctl("hypr3d", "view", "first")
    ctl("hypr3d", "spawn")
    time.sleep(1)
    a = pc_aim("h3d-left")
    r1 = ctl("dispatch", "hypr3d:play", "fill")
    p1 = pc_playing("h3d-left", facing=True)
    r2 = ctl("dispatch", "hypr3d:play", "here")
    p2 = st()["playing"] or {}
    r3 = ctl("dispatch", "hypr3d:play")
    s3 = st()
    r4 = ctl("dispatch", "hypr3d:play", "sideways")
    check("31c", "the hypr3d:play dispatcher (hyprland.conf): fill plays filling the view, here switches to here, none stops; a word it doesn't know fails, saying which it takes",
          a and r1 == r2 == r3 == "ok" and p1.get("fill") is True and p2.get("fill") is False and p2.get("class") == "h3d-left" and s3["playing"] is None and r4 != "ok"
          and "[here|fill]" in r4, f"{r1} {p1}; {r2} {p2}; {r3} {s3['playing']}; {r4}")
    ensure_3d(False)
    lua_session()


# ------------------------------------------------------------------ a game played here in tiling mode's ring: play_size

def ps_view(cls):
    """the window's middle right of the view's (degrees) and its larger share of the view's height or width at its depth"""
    c, p = pc_cam(), placed(cls)
    if not p:
        return 99.0, 0.0
    e, yaw = c["eye"], math.radians(c["yaw"])
    dx, dz = p["center"][0] - e[0], p["center"][2] - e[2]
    off = (math.degrees(math.atan2(dx, -dz)) - c["yaw"] + 180) % 360 - 180
    depth = max(dx * math.sin(yaw) - dz * math.cos(yaw), 1e-3)
    lw, lh = logical_size()
    vh = 2 * depth * math.tan(HALF_FOV)
    return off, max(p["height"] / vh, p["width"] / (vh * lw / lh))


def ps_up(cls):
    """degrees the window's middle is above the camera's pitch, seen from the eye"""
    c, p = pc_cam(), placed(cls)
    if not p:
        return 99.0
    d = [p["center"][i] - c["eye"][i] for i in range(3)]
    return math.degrees(math.atan2(d[1], math.hypot(d[0], d[2]))) - c["pitch"]


def ps_box(img):
    """h3dgame's window at the frame's middle: (width, height, right, up) as frame fractions, (0, 0, 9, 9) if absent;
    its colour is the commonest game-blue near the middle (world greys can match), and the height adds the top band
    (an eighth)"""
    cx, cy = img.w // 2, img.h // 2

    def px(x, y):
        return img.px[(y * img.w + x) * 3:(y * img.w + x) * 3 + 3]
    seen = {}
    for y in range(cy - 20, cy + 21, 2):
        for x in range(cx - 20, cx + 21, 2):
            c = tuple(px(x, y))
            if game_blue(*c):
                seen[c] = seen.get(c, 0) + 1
    if not seen:
        return 0.0, 0.0, 9.0, 9.0
    bg = max(seen, key=seen.get)

    def like(c):
        return all(abs(c[i] - bg[i]) <= 3 for i in range(3))
    col = [y for y in range(img.h) if like(px(cx, y))]
    row = [x for x in range(img.w) if like(px(x, cy))]
    bottom = (col[-1] + 1) / img.h
    tall = (bottom - col[0] / img.h) * 8 / 7
    return (row[-1] - row[0] + 1) / img.w, tall, (row[0] + row[-1] + 1) / 2 / img.w - 0.5, 0.5 - (bottom - tall / 2)


def ps_wheel(n):
    """Super+wheel n notches (> 0 down: smaller), then waits for the row to settle"""
    qmp([key_event("meta_l", True)])
    time.sleep(0.15)
    wheel(n)
    qmp([key_event("meta_l", False)])
    time.sleep(0.3)
    ps_settle()


def ps_settle():
    try:
        settled(8)
    except TimeoutError:
        pass
    time.sleep(0.3)


def ps_size():
    """the play JSON's size: the played window's share of the view in the ring (None: not played or unsupported)"""
    return (st()["playing"] or {}).get("size")


def ps_played():
    """the tile JSON's played window: fit and share ({}: none or unsupported)"""
    return windows3d()["tiling"].get("played") or {}


def ps_near(size, want, tol=1e-3):
    return size is not None and abs(size - want) < tol


def ps_config(v):
    return ctl("eval", "hl.config({ plugin = { hypr3d = { play_size = %s } } })" % v)


@section("31d", "a game played here in tiling mode's ring (P, first and third person): just play_size of the view (25% to 94%, bigger than on your screen too), in the middle of your view left and right and up and down (looking up or down too; the camera still), the windows beside it in the view; Super+wheel bigger and smaller, 5% of the view a notch, up to 94% and down to 25% (a high-resolution wheel's half notches too), never the game's; the wheel alone the game's; Super+Esc its size back, the row where it is; Shift+P as ever; play_size set at run time (used at once, and out of range); a crowded ring (the others smaller round it, not the game); a ring that stays (Y); a window opening while playing goes right of it; played out in the world and on the wall")
def s_play_size():
    lua_session()
    ensure_avatar(AV)
    ensure_3d(False)
    clean_windows()
    ctl("hypr3d", "tile", "off")
    ctl("hypr3d", "tile", "follow", "on")
    ctl("hypr3d", "reset-windows", "forget")
    ctl("eval", 'hl.config({ plugin = { hypr3d = { play_view = "here", play_size = 0.5 } } })')
    machine.execute("pkill -u alice foot; pkill -f 'h3dgame --title'; true")
    wait_for("no windows", lambda: not json.loads(ctl("-j", "clients")), 10, 0.3)
    # four small terminals and a game nearly monitor-sized (1280x800) in the ring
    for i in range(4):
        alice(f"setsid -f foot --app-id h3d-d{i} > /dev/null 2>&1")
        time.sleep(0.6)
    wait_for("four terminals", lambda: len(json.loads(ctl("-j", "clients"))) >= 4, 20)
    pc_game(PC_FREE)
    time.sleep(1.5)
    ensure_3d()
    menu_closed()
    ctl("hypr3d", "view", "first")
    ctl("hypr3d", "spawn")
    time.sleep(1)
    for i in range(4):
        ctl("hypr3d", "window", f"h3d-d{i}", "size", "280", "240")
    ctl("hypr3d", "window", PC_FREE, "size", "1200", "750")
    time.sleep(1.5)
    press("t")
    w = settled(10)
    row = [x["class"] for x in ring_view(w)]
    # P aimed 12 degrees right of the game's middle: its middle reaching the view's shows the row turned
    a = aim_row(PC_FREE)
    ctl("hypr3d", "turn", f"{st()['yaw'] + 12:.2f}", "0")
    time.sleep(0.8)
    aimed = (st().get("aimed") or {}).get("class")
    off0, share0 = ps_view(PC_FREE)
    c0, yaw0 = pc_cam(), windows3d()["tiling"]["yaw"]
    press("p")
    p = pc_playing(PC_FREE)
    moved, turned, view = pc_still(c0, 2.0)
    ps_settle()
    (off1, share1), up1 = ps_view(PC_FREE), ps_up(PC_FREE)
    pl1 = ps_played()
    others = pc_others(PC_FREE)
    i = row.index(PC_FREE) if PC_FREE in row else -1
    near = [row[j] for j in (i - 1, i + 1) if 0 <= j < len(row)] if i >= 0 else []
    img = frame("31d-played")
    told = ctl("hypr3d", "log", "10")
    check("31d", "tiling mode's ring, four terminals and a 1200x750 game (85% of the view, as big as the ring has one), P with the crosshair 12 deg right of its middle: "
          "played here", a and aimed == PC_FREE and p.get("fill") is False and abs(share0 - FRONT_FIT) < 0.03, f"row {row}; aimed {aimed}; share {share0:.3f}; {p}")
    check("31d", "... it takes play_size (0.5) of the view (hyprctl hypr3d windows and camera; the status's size, the tile JSON's played)",
          abs(share1 - 0.5) < 0.03 and ps_near(p.get("size"), 0.5) and abs(pl1.get("share", 0) - 0.5) < 0.01, f"share {share0:.3f} -> {share1:.3f}; size {p.get('size')}; played {pl1}")
    check("31d", "... and so in the frame: the game's background across half of it, through its middle", abs(pc_span(img) - 0.5) < 0.06, f"{pc_span(img):.3f}")
    check("31d", "... the row turned so its middle is the view's (within 2 deg), up and down too (0.5 deg)", abs(off1) < 2 and abs(off0 + 12) < 2 and abs(up1) < 0.5,
          f"{off0:.2f} -> {off1:.2f} deg, up {up1:.2f} deg; the ring's yaw {yaw0} -> {windows3d()['tiling']['yaw']}")
    check("31d", "... the camera still over 2 s (hyprctl hypr3d camera: its eye, yaw, pitch, up), never going to face it", moved < 0.001 and turned < 0.01 and view == 0,
          f"eye {moved * 100:.2f} cm, {turned:.3f} deg, view {view}")
    check("31d", "... the windows beside it in the row in the view (their middles, panels' inView)", near and all(others.get(n) is True for n in near), f"{near}: {others}")
    check("31d", "... the notification says Super+wheel sizes it", "Super+wheel makes it bigger or smaller" in told, told.strip()[-200:])

    pc_mark(PC_FREE, "sw")
    ps_wheel(-2)
    time.sleep(0.8)
    s2, (off2, share2) = ps_size(), ps_view(PC_FREE)
    log2 = ctl("hypr3d", "log", "12")
    ps_wheel(4)
    time.sleep(0.8)
    s3, (off3, share3) = ps_size(), ps_view(PC_FREE)
    got = [l for l in pc_lines(PC_FREE, "sw") if l.startswith("wheel")]
    check("31d", "Super+wheel up x2: 0.6 of the view, still in the middle", ps_near(s2, 0.6) and abs(share2 - 0.6) < 0.03 and abs(off2) < 2,
          f"size {s2}, share {share2:.3f}, off {off2:.2f} deg")
    check("31d", "... down x4: 0.4, still in the middle", ps_near(s3, 0.4) and abs(share3 - 0.4) < 0.03 and abs(off3) < 2, f"size {s3}, share {share3:.3f}, off {off3:.2f} deg")
    check("31d", "... the game got none of it", not got, got)
    lines = log2.splitlines()
    check("31d", "... logged once a notch, and said once a turn of the wheel (a notification a moment after it stops)",
          sum("Super+wheel: the window played takes" in l for l in lines) == 2 and sum("takes 60% of the view" in l and "Super+wheel" not in l for l in lines) == 1,
          "; ".join(l for l in lines if "of the view" in l)[-500:])
    pc_mark(PC_FREE, "plain")
    wheel(1)
    got = [l for l in pc_lines(PC_FREE, "plain") if l.startswith("wheel")]
    check("31d", "the wheel alone reaches the game (its log), the size as it was", got and ps_near(ps_size(), 0.4), f"{got}; size {ps_size()}")

    # limits: 94% at most (above the ring's 85%), 25% at least; the frame waits for the notification to go
    ps_wheel(-12)
    time.sleep(4.5)
    s4, (off4, share4), up4, pl4 = ps_size(), ps_view(PC_FREE), ps_up(PC_FREE), ps_played()
    told4 = ctl("hypr3d", "log", "4")
    bw, bh, bx, by = ps_box(frame("31d-most"))
    ps_wheel(1)
    s5, share5 = ps_size(), ps_view(PC_FREE)[1]
    check("31d", "Super+wheel up x12: the most, 94% of the view, more than the ring gives a window (the size stops there; hyprctl hypr3d windows and camera, the tile "
          "JSON's played), in the middle of the view left and right (2 deg) and up and down (0.5 deg), said so",
          ps_near(s4, 0.94) and abs(share4 - 0.94) < 0.03 and abs(pl4.get("share", 0) - 0.94) < 0.005 and abs(off4) < 2 and abs(up4) < 0.5 and "(the most)" in told4,
          f"size {s4}, share {share4:.3f}, off {off4:.2f} deg, up {up4:.2f} deg; played {pl4}; {told4.strip()[-160:]}")
    check("31d", "... and so in the frame: the game across 94% of it, its middle the frame's", abs(max(bw, bh) - 0.94) < 0.03 and abs(bx) < 0.02 and abs(by) < 0.02,
          f"{bw:.3f} x {bh:.3f} of the frame, its middle {bx:+.3f}, {by:+.3f} from the frame's")
    check("31d", "... a notch down from there: 0.89", ps_near(s5, 0.89) and abs(share5 - 0.89) < 0.03, f"size {s5}, share {share5:.3f}")
    ps_wheel(20)
    time.sleep(0.8)
    s6, (off6, share6) = ps_size(), ps_view(PC_FREE)
    told6 = ctl("hypr3d", "log", "4")
    check("31d", "... down x20: the least, a quarter of the view, still in the middle, said so", ps_near(s6, 0.25) and abs(share6 - 0.25) < 0.03 and abs(off6) < 2 and "(the least)" in told6,
          f"size {s6}, share {share6:.3f}, off {off6:.2f}; {told6.strip()[-120:]}")
    # a high-resolution wheel: half notches, two make one
    ps_wheel(-5)
    sa = ps_size()
    qmp([key_event("meta_l", True)])
    time.sleep(0.15)
    wheel_hires(60, 60, 60)
    qmp([key_event("meta_l", False)])
    time.sleep(0.5)
    sb = ps_size()
    check("31d", "a high-resolution wheel with Super, three half notches down: a notch smaller (0.05)", ps_near(sa, 0.5) and ps_near(sb, 0.45), f"{sa} -> {sb}")
    moved2, turned2 = math.dist(pc_cam()["eye"], c0["eye"]), pc_turned(pc_cam(), c0)
    check("31d", "the camera where it was all along", moved2 < 0.001 and turned2 < 0.01, f"{moved2 * 100:.2f} cm, {turned2:.3f} deg")

    yawA = windows3d()["tiling"]["yaw"]
    press("meta_l", "esc")
    time.sleep(1.2)
    ps_settle()
    off7, share7 = ps_view(PC_FREE)
    yawB = windows3d()["tiling"]["yaw"]
    check("31d", "Super+Esc: walking, the game as big as the ring has it again (85%), the row where it was, not turned back",
          st()["playing"] is None and abs(share7 - FRONT_FIT) < 0.03 and abs(yawB - yawA) < 0.01 and abs(yawB - yaw0) > 5,
          f"share {share7:.3f}; the ring's yaw {yaw0} at first, {yawA} -> {yawB}")

    aim_row(PC_FREE)
    press("shift", "p")
    pf = pc_playing(PC_FREE, facing=True)
    time.sleep(0.5)
    pc_mark(PC_FREE, "fill")
    sz0 = ps_size()
    ps_wheel(-2)
    img = frame("31d-fill")
    got = [l for l in pc_lines(PC_FREE, "fill") if l.startswith("wheel")]
    check("31d", "Shift+P: filling the view (the game across the frame), not held to play_size in the row (played null); Super+wheel nothing, the game none of it",
          pf.get("fill") is True and pc_span(img) > 0.7 and windows3d()["tiling"].get("played") is None and ps_size() == sz0 and not got,
          f"{pf}; the game {pc_span(img):.0%} across the frame; played {windows3d()['tiling'].get('played')}; size {sz0} -> {ps_size()}; {got}")
    press("meta_l", "esc")
    pc_wait("the camera back", lambda: math.dist(pc_cam()["eye"], c0["eye"]) < 0.001, 5)

    seen = []
    for v in (0.35, 0.3):
        aim_row(PC_FREE)
        r = ps_config(v)
        press("p", after=0.05)
        first = ps_size()
        ps_settle()
        seen.append((v, r, first, round(ps_view(PC_FREE)[1], 3)))
        press("meta_l", "esc")
        time.sleep(0.8)
    check("31d", "play_size set at run time (hyprctl eval), P at once: the play takes it from the start (size, share)",
          all(r == "ok" and ps_near(f, v) and abs(sh - v) < 0.03 for v, r, f, sh in seen), f"(set, eval, size at P, share) {seen}")
    r = ps_config(1.5)
    told = pc_wait("the value said", lambda: (lambda l: l if "not 1.5" in l else None)(ctl("hypr3d", "log", "8")), 5) or ""
    aim_row(PC_FREE)
    press("p")
    sA, shareA = ps_size(), ps_view(PC_FREE)[1]
    ps_wheel(1)
    sB, shareB = ps_size(), ps_view(PC_FREE)[1]
    press("meta_l", "esc")
    time.sleep(1.5)
    times = ctl("hypr3d", "log", "40").count("not 1.5")
    check("31d", "play_size = 1.5: said once, taken as 0.94 (the game 94% of the view); Super+wheel down: 0.89",
          r == "ok" and times == 1 and "0.94 then" in told and ps_near(sA, 0.94) and abs(shareA - 0.94) < 0.03 and ps_near(sB, 0.89) and abs(shareB - 0.89) < 0.03,
          f"{r}; said {times} time(s): {told.strip()[-150:]}; size {sA} (share {shareA:.3f}) -> {sB} (share {shareB:.3f})")
    got = []
    for v, want in (("-math.huge", 0.25), ("0/0", 0.5)):
        r = ps_config(v)
        time.sleep(0.3)
        aim_row(PC_FREE)
        press("p")
        got.append((v, r, ps_size()))
        press("meta_l", "esc")
        time.sleep(0.8)
    took = [(s, w) for (v, r, s), w in zip(got, (0.25, 0.5)) if r == "ok"]
    if took:
        check("31d", "... -inf: the least (0.25); not a number: play_size's default (0.5)", all(ps_near(s, w) for s, w in took), f"(value, eval, size) {got}")
    else:
        note("31d", "play_size = -math.huge and 0/0 (NaN): the config didn't take them", got)
    ps_config(0.5)
    time.sleep(1.2)

    # P while looking up, then down: its middle where the view crosses the ring; Super+Esc levels it with the ring's
    # middle
    got, ok = [], True
    for pitch in (8.0, -10.0):
        aim_row(PC_FREE)
        ctl("hypr3d", "turn", f"{st()['yaw'] - 5:.2f}", f"{pitch:.1f}")
        time.sleep(0.8)
        aimed = (st().get("aimed") or {}).get("class")
        press("p")
        p = pc_playing(PC_FREE)
        ps_settle()
        (off, share), up, pl = ps_view(PC_FREE), ps_up(PC_FREE), ps_played()
        bw, bh, bx, by = ps_box(frame(f"31d-look{pitch:+.0f}"))
        press("meta_l", "esc")
        time.sleep(1.2)
        ps_settle()
        t, g = windows3d()["tiling"], placed(PC_FREE) or {}
        back = abs(g.get("center", [0, 99, 0])[1] - t["center"][1])
        got.append(f"{pitch:+.0f} deg: aimed {aimed}, fill {p.get('fill')}, off {off:.2f}, up {up:.2f} deg, played {pl.get('share')}; in the frame "
                   f"{max(bw, bh):.3f}, its middle {bx:+.3f}, {by:+.3f}; walking, {back * 100:.1f} cm from the ring's middle up and down")
        ok = ok and aimed == PC_FREE and p.get("fill") is False and abs(off) < 2 and abs(up) < 0.5 and abs(pl.get("share", 0) - 0.5) < 0.005 and abs(bx) < 0.03 and abs(by) < 0.03 \
            and back < 0.01
    ctl("hypr3d", "turn", f"{st()['yaw']:.2f}", "0")
    time.sleep(0.5)
    check("31d", "looking 8 deg up, then 10 down, P: the game's middle where you look (within 0.5 deg up and down, 2 left and right; in the frame too), half the view; "
          "Super+Esc: level with the ring's middle again", ok, "; ".join(got))

    # a staying ring (Y), you 0.8 m off its middle: the game turns to your view from your eye
    ctl("hypr3d", "tile", "follow", "off")
    time.sleep(0.5)
    s = st()
    yaw = math.radians(s["yaw"])
    ctl("hypr3d", "tp", f"{s['feet'][0] + 0.8 * math.cos(yaw):.3f}", f"{s['feet'][1]:.3f}", f"{s['feet'][2] + 0.8 * math.sin(yaw):.3f}")
    time.sleep(1.0)
    aim_row(PC_FREE)
    ctl("hypr3d", "turn", f"{st()['yaw'] - 8:.2f}", "0")
    time.sleep(0.8)
    offY0 = ps_view(PC_FREE)[0]
    c1 = pc_cam()
    press("p")
    p = pc_playing(PC_FREE)
    movedY, turnedY, _ = pc_still(c1, 1.0)
    ps_settle()
    t = windows3d()["tiling"]
    (offY1, shareY), upY = ps_view(PC_FREE), ps_up(PC_FREE)
    plY = t.get("played") or {}
    # half the view from the ring's middle; slightly less from your eye, further from the row
    check("31d", "a ring that stays (Y), you 0.8 m off its middle, P 8 deg off the game: at the ring, turned to the middle of your view from your eye (up and down too), "
          "half the view as the ring has it, the camera still", p.get("fill") is False and t["atRing"] and not t["follow"] and abs(offY1) < 2 and abs(upY) < 0.5
          and abs(plY.get("share", 0) - 0.5) < 0.005 and movedY < 0.001 and turnedY < 0.01, f"atRing {t['atRing']}, follow {t['follow']}; off {offY0:.2f} -> {offY1:.2f} deg, "
          f"up {upY:.2f} deg; played {plY}, from your eye {shareY:.3f}; eye {movedY * 100:.2f} cm, {turnedY:.3f} deg")
    press("meta_l", "esc")
    ctl("hypr3d", "tile", "follow", "on")
    ctl("hypr3d", "spawn")
    time.sleep(1.0)
    ps_settle()

    # a window opening while a pointer-locking game is played here (the lock keeps the keyboard): it goes right of the
    # game, which stays mid-view
    pc_game(PC_LOCK, "--relative")
    time.sleep(1.0)
    ctl("hypr3d", "window", PC_LOCK, "size", "1200", "750")
    time.sleep(1.5)
    ps_settle()
    aim_row(PC_LOCK)
    ctl("hypr3d", "turn", f"{st()['yaw'] + 5:.2f}", "0")
    time.sleep(0.6)
    press("p")
    pc_playing(PC_LOCK)
    ps_settle()
    offA = ps_view(PC_LOCK)[0]
    alice("setsid -f foot --app-id h3d-dnew > /dev/null 2>&1")
    pc_wait("the new terminal in the row", lambda: "h3d-dnew" in tiled_row(), 20, 0.5)
    time.sleep(1.0)
    ps_settle()
    offB, rowB, sB = ps_view(PC_LOCK)[0], tiled_row(), st()
    ps_wheel(1)
    offC, sC = ps_view(PC_LOCK)[0], st()
    k = rowB.index(PC_LOCK) if PC_LOCK in rowB else -9
    check("31d", "a window opening while playing here goes right of the game (in the middle of your view), the game staying there (the row turning round it), still played",
          k >= 0 and k + 1 < len(rowB) and rowB[k + 1] == "h3d-dnew" and abs(offA) < 2 and abs(offB) < 2 and (sB["playing"] or {}).get("class") == PC_LOCK,
          f"row {rowB}; the game {offA:.2f} -> {offB:.2f} deg; playing {(sB['playing'] or {}).get('class')}")
    check("31d", "... Super+wheel then: still in the middle", abs(offC) < 2 and (sC["playing"] or {}).get("class") == PC_LOCK, f"{offB:.2f} -> {offC:.2f} deg")
    ps_wheel(-1)  # back to play_size's half, for what follows
    press("meta_l", "esc")
    ctl("hypr3d", "window", "h3d-dnew", "close")
    machine.execute("pkill -f 'h3dgame --title h3dgame-pc-lock'; true")
    time.sleep(1.0)

    # a crowded ring (seven terminals as big as the game, shrunk alike to fit): P still gives the game play_size
    # mid-view; the others shrink alike by ring angle (kept by pulled-in windows), the row spanning as far as before
    for i in range(4):
        ctl("hypr3d", "window", f"h3d-d{i}", "size", "1200", "750")
    for i in range(4, 7):
        alice(f"setsid -f foot --app-id h3d-d{i} > /dev/null 2>&1")
        time.sleep(0.6)
    pc_wait("seven terminals in the row", lambda: sum(c.startswith("h3d-d") for c in tiled_row()) >= 7, 20, 0.5)
    for i in range(4, 7):
        ctl("hypr3d", "window", f"h3d-d{i}", "size", "1200", "750")
    time.sleep(1.5)
    ps_settle()
    v0 = ring_view()
    before = {x["class"]: x["half"] for x in v0}
    aim_row(PC_FREE)
    press("p")
    p = pc_playing(PC_FREE)
    ps_settle()
    w = windows3d()
    v1 = ring_view(w)
    after = {x["class"]: x["half"] for x in v1}
    plC, (offC, shareC), upC = w["tiling"].get("played") or {}, ps_view(PC_FREE), ps_up(PC_FREE)
    others = [c for c in after if c != PC_FREE]
    span = lambda v: (v[-1]["rel"] + v[-1]["half"]) - (v[0]["rel"] - v[0]["half"]) if v else 0  # noqa: E731
    deg = lambda h: round(math.degrees(2 * h), 1)  # noqa: E731
    check("31d", "a crowded ring (seven big terminals and the game, each about a third of the view, all smaller alike to fit round you): P, the game takes play_size "
          "(half the view) all the same, in the middle of the view", len(others) == 7 and p.get("fill") is False and abs(plC.get("share", 0) - 0.5) < 0.005
          and abs(shareC - 0.5) < 0.03 and abs(offC) < 2 and abs(upC) < 0.5 and after.get(PC_FREE, 0) > before.get(PC_FREE, 9),
          f"row {[x['class'] for x in v1]}; played {plC}; from the camera {shareC:.3f}, off {offC:.2f}, up {upC:.2f} deg; the game {deg(before.get(PC_FREE, 0))} -> "
          f"{deg(after.get(PC_FREE, 0))} deg round the ring")
    check("31d", "... the others smaller round it, alike (the angle each takes round the ring), the row going as far round as before",
          others and all(after[c] < before.get(c, 0) - math.radians(0.4) for c in others) and max(after[c] for c in others) - min(after[c] for c in others) < math.radians(0.2)
          and abs(span(v1) - span(v0)) < 0.02, f"{math.degrees(span(v0)):.1f} -> {math.degrees(span(v1)):.1f} deg round you; "
          + "; ".join(f"{c} {deg(before.get(c, 0))} -> {deg(after[c])}" for c in others))
    press("meta_l", "esc")
    for i in range(4, 7):
        ctl("hypr3d", "window", f"h3d-d{i}", "close")
    for i in range(4):
        ctl("hypr3d", "window", f"h3d-d{i}", "size", "280", "240")
    time.sleep(1.5)
    ps_settle()

    # third person: P at play_size 0.5 takes half the view mid-view; Super+wheel up x6: 0.8. Its frame middle sits a
    # little right: it faces the avatar's head, not the camera, and its nearer edge looks bigger (about 0.56 x share^2 x
    # sin 7 deg of the frame's width)
    ctl("hypr3d", "view", "third")
    ctl("hypr3d", "spawn")
    time.sleep(1.2)
    pc_steady()
    ps_settle()
    aim_row(PC_FREE)
    ctl("hypr3d", "turn", f"{st()['yaw'] + 6:.2f}", "0")
    pc_steady()
    aimed = (st().get("aimed") or {}).get("class")
    offT0, shareT0 = ps_view(PC_FREE)
    c2 = pc_cam()
    press("p")
    p = pc_playing(PC_FREE)
    movedT, turnedT, viewT = pc_still(c2, 1.5)
    ps_settle()
    (offT1, shareT1), upT1, plT = ps_view(PC_FREE), ps_up(PC_FREE), ps_played()
    bw, bh, bx, by = ps_box(frame("31d-third"))
    check("31d", "third person, P 6 deg off the game: played here, the camera still, the row turned so the game's in the middle of the view (within 2 deg), up and down "
          "too (0.5 deg)", aimed == PC_FREE and p.get("fill") is False and st()["view"] == "third" and movedT < 0.001 and turnedT < 0.01 and viewT == 0 and abs(offT1) < 2
          and abs(upT1) < 0.5, f"aimed {aimed}; {p}; eye {movedT * 100:.2f} cm, {turnedT:.3f} deg; off {offT0:.2f} -> {offT1:.2f} deg, up {upT1:.2f} deg")
    check("31d", "... play_size 0.5: half the view all the same (not held to what the ring gives a window there, standing on the ground: a little under half), from the "
          "camera and in the frame",
          ps_near(p.get("size"), 0.5) and abs(plT.get("share", 0) - 0.5) < 0.005 and abs(shareT1 - 0.5) < 0.02 and abs(max(bw, bh) - 0.5) < 0.03 and abs(bx) < 0.04
          and abs(by) < 0.03, f"size {p.get('size')}; played {plT}; from the camera {shareT0:.3f} -> {shareT1:.3f}; in the frame {bw:.3f} x {bh:.3f}, its middle {bx:+.3f}, {by:+.3f}")
    ps_wheel(-6)
    time.sleep(0.8)
    sT2, (offT2, shareT2), upT2, plT2 = ps_size(), ps_view(PC_FREE), ps_up(PC_FREE), ps_played()
    bw, bh, bx, by = ps_box(frame("31d-third-0.8"))
    check("31d", "... Super+wheel up x6: 0.8 of the view (from the camera and in the frame), still in the middle",
          ps_near(sT2, 0.8) and abs(plT2.get("share", 0) - 0.8) < 0.005 and abs(shareT2 - 0.8) < 0.03 and abs(max(bw, bh) - 0.8) < 0.03 and abs(offT2) < 2 and abs(upT2) < 0.5
          and abs(bx) < 0.07 and abs(by) < 0.03, f"size {sT2}; played {plT2}; from the camera {shareT2:.3f}, off {offT2:.2f}, up {upT2:.2f} deg; in the frame {bw:.3f} x "
          f"{bh:.3f}, its middle {bx:+.3f}, {by:+.3f}")
    press("meta_l", "esc")
    time.sleep(1.0)
    ps_settle()
    aim_row(PC_FREE)
    ctl("hypr3d", "turn", f"{st()['yaw']:.2f}", "-8")
    pc_steady()
    aimed = (st().get("aimed") or {}).get("class")
    press("p")
    p = pc_playing(PC_FREE)
    ps_settle()
    (offT3, shareT3), upT3 = ps_view(PC_FREE), ps_up(PC_FREE)
    check("31d", "... looking 8 deg down, P: its middle where you look (within 0.5 deg), left and right too", aimed == PC_FREE and p.get("fill") is False and abs(upT3) < 0.5
          and abs(offT3) < 2, f"aimed {aimed}; off {offT3:.2f}, up {upT3:.2f} deg; pitch {pc_cam()['pitch']:.1f}")
    press("meta_l", "esc")
    ps_config(0.5)
    ctl("hypr3d", "view", "first")
    ctl("hypr3d", "spawn")
    ctl("hypr3d", "turn", f"{st()['yaw']:.2f}", "0")
    time.sleep(1)
    ctl("hypr3d", "tile", "off")
    time.sleep(1.5)

    # tiling off: brought out and played, Super+wheel scales it in place
    r = ctl("hypr3d", "window", PC_FREE, "bring")
    time.sleep(1.5)
    ps_settle()
    r2 = ctl("hypr3d", "window", PC_FREE, "play")
    pc_playing(PC_FREE)
    g0 = placed(PC_FREE) or {}
    pc_mark(PC_FREE, "world")
    ps_wheel(-2)
    g1 = placed(PC_FREE) or {}
    ps_wheel(3)
    g2 = placed(PC_FREE) or {}
    got = [l for l in pc_lines(PC_FREE, "world") if l.startswith("wheel")]
    up, down = g1.get("size", 0) / max(g0.get("size", 1e-9), 1e-9), g2.get("size", 0) / max(g1.get("size", 1e-9), 1e-9)
    check("31d", "out in the world, played where it hangs: Super+wheel up x2 1/0.95^2 as big, down x3 0.95^3, where it is; the game none of it",
          r == "here" and r2 == "playing" and abs(up - 1 / WHEEL_SIZE**2) < 0.01 and abs(down - WHEEL_SIZE**3) < 0.01 and math.dist(g0.get("center", [9] * 3), g2.get("center", [0] * 3)) < 0.01
          and not got, f"{r} {r2}; size {g0.get('size')} -> {g1.get('size')} ({up:.4f}) -> {g2.get('size')} ({down:.4f}); center {g0.get('center')} -> {g2.get('center')}; {got}")
    press("meta_l", "esc")
    # on the wall it stays screen-sized: Super+wheel does nothing, logged once
    ctl("hypr3d", "window", PC_FREE, "wall")
    time.sleep(1.5)
    pc_aim(PC_FREE)
    press("p")
    p = pc_playing(PC_FREE)
    pc_mark(PC_FREE, "wall")
    ps_wheel(-3)
    got = [l for l in pc_lines(PC_FREE, "wall") if l.startswith("wheel")]
    n = ctl("hypr3d", "log", "40").count("on the desktop wall, as big as it goes")
    check("31d", "on the desktop wall: Super+wheel nothing (the game none of it), logged once", p.get("fill") is False and not got and n == 1 and placed(PC_FREE) is None,
          f"{p}; {got}; logged {n} time(s)")
    press("meta_l", "esc")
    machine.execute("pkill -f 'h3dgame --title h3dgame-pc'; pkill -u alice foot; true")
    ensure_3d(False)


def ps_proj(cls, cam=None):
    """the window as the camera projects it: share (the larger of its height and width shares of the view) and its
    middle's offset x, y (fractions of the half view, + right and up)"""
    def cross(a, b):
        return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])

    def dot(a, b):
        return sum(a[i] * b[i] for i in range(3))
    c = cam or pc_cam()
    p = placed(cls)
    if not p:
        return {"share": 0.0, "h": 0.0, "w": 0.0, "x": 9.0, "y": 9.0}
    yaw, pit = math.radians(c["yaw"]), math.radians(c["pitch"])
    fwd = (math.sin(yaw) * math.cos(pit), math.sin(pit), -math.cos(yaw) * math.cos(pit))
    right = (math.cos(yaw), 0.0, math.sin(yaw))
    up = cross(right, fwd)
    lw, lh = logical_size()
    t = math.tan(HALF_FOV)
    wr = cross((0.0, 1.0, 0.0), p["normal"])
    n = math.sqrt(dot(wr, wr)) or 1.0
    wr = tuple(x / n for x in wr)
    C = p["center"]

    def pr(X):
        d = [X[i] - c["eye"][i] for i in range(3)]
        z = max(dot(d, fwd), 1e-4)
        return dot(d, right) / z, dot(d, up) / z
    top, bot = pr([C[0], C[1] + p["height"] / 2, C[2]]), pr([C[0], C[1] - p["height"] / 2, C[2]])
    lef, rig = pr([C[i] - wr[i] * p["width"] / 2 for i in range(3)]), pr([C[i] + wr[i] * p["width"] / 2 for i in range(3)])
    mid = pr(C)
    h, w = abs(top[1] - bot[1]) / (2 * t), abs(rig[0] - lef[0]) / (2 * t * lw / lh)
    return {"share": round(max(h, w), 3), "h": round(h, 3), "w": round(w, 3), "x": round(mid[0] / (t * lw / lh), 3), "y": round(mid[1] / t, 3)}


def ps_part(name, fn):
    """runs one part of 31e: an exception fails a check and the next parts still run"""
    try:
        fn()
    except Exception as e:  # noqa: BLE001
        traceback.print_exc()
        check("31e", f"{name}: ran to the end", False, f"{type(e).__name__}: {e}"[:500])
        try:
            if st()["playing"]:
                press("meta_l", "esc")
        except Exception:  # noqa: BLE001
            pass


@section("31e", "a game played here in tiling mode's ring, as the camera draws it: play_size of the view looking up or down, in a ring that stays (Y) off its middle, in third person with a wall behind the avatar (the camera's boom pulled in; 94% at the most, no more), after V; away from a ring that stays, played as the ring has it; play ending by itself (a window opening takes the keyboard), the game keeping its size and place, the new window beside it")
def s_play_seen():
    lua_session()
    ensure_avatar(AV)
    ensure_3d(False)
    clean_windows()
    ctl("hypr3d", "tile", "off")
    ctl("hypr3d", "tile", "follow", "on")
    ctl("hypr3d", "reset-windows", "forget")
    # only a play_size change resets the play's size (31d's Super+wheel left 0.8); the plugin reads it once a second
    ctl("eval", 'hl.config({ plugin = { hypr3d = { play_view = "here", play_size = 0.55 } } })')
    time.sleep(1.5)
    ctl("eval", 'hl.config({ plugin = { hypr3d = { play_view = "here", play_size = 0.5 } } })')
    time.sleep(1.5)
    machine.execute("pkill -u alice foot; pkill -f 'h3dgame --title'; true")
    wait_for("no windows", lambda: not json.loads(ctl("-j", "clients")), 10, 0.3)
    for i in range(4):
        alice(f"setsid -f foot --app-id h3d-e{i} > /dev/null 2>&1")
        time.sleep(0.6)
    wait_for("four terminals", lambda: len(json.loads(ctl("-j", "clients"))) >= 4, 20)
    pc_game(PC_FREE)
    time.sleep(1.5)
    ensure_3d()
    menu_closed()
    ctl("hypr3d", "view", "first")
    ctl("hypr3d", "spawn")
    time.sleep(1)
    for i in range(4):
        ctl("hypr3d", "window", f"h3d-e{i}", "size", "280", "240")
    ctl("hypr3d", "window", PC_FREE, "size", "1200", "750")
    time.sleep(1.5)
    press("t")
    settled(10)
    note("31e", "the row", [x["class"] for x in ring_view()])

    def level_then_pitched():
        for pitch in (0.0, 25.0, -25.0):
            aim_row(PC_FREE)
            ctl("hypr3d", "turn", f"{st()['yaw']:.2f}", f"{pitch:.1f}")
            time.sleep(0.6)
            aimed = (st().get("aimed") or {}).get("class")
            press("p")
            p = pc_playing(PC_FREE)
            ps_settle()
            g, pl = ps_proj(PC_FREE), ps_played()
            bw, bh, bx, by = ps_box(frame(f"31e-pitch{pitch:+.0f}"))
            check("31e", f"first person, looking {pitch:+.0f} deg, P: the game takes play_size (0.5) of the view as the camera draws it, its middle the view's",
                  aimed == PC_FREE and p.get("fill") is False and abs(g["share"] - 0.5) < 0.03 and abs(g["x"]) < 0.05 and abs(g["y"]) < 0.05,
                  f"aimed {aimed}; drawn {g}; in the frame {bw:.3f} x {bh:.3f} (middle {bx:+.3f}, {by:+.3f}); the plugin's played {pl}")
            press("meta_l", "esc")
            time.sleep(1.0)
            ps_settle()
        ctl("hypr3d", "turn", f"{st()['yaw']:.2f}", "0")
        time.sleep(0.4)
    ps_part("looking level, up and down", level_then_pitched)

    def staying():
        # a staying ring, you 1 m nearer the game: it's shrunk to play_size of your view
        aim_row(PC_FREE)
        ctl("hypr3d", "turn", f"{st()['yaw']:.2f}", "0")
        time.sleep(0.4)
        ctl("hypr3d", "tile", "follow", "off")
        time.sleep(0.6)
        s = st()
        yaw = math.radians(s["yaw"])
        ctl("hypr3d", "tp", f"{s['feet'][0] + math.sin(yaw):.3f}", f"{s['feet'][1]:.3f}", f"{s['feet'][2] - math.cos(yaw):.3f}")
        time.sleep(1.0)
        aim_row(PC_FREE)
        ctl("hypr3d", "turn", f"{st()['yaw']:.2f}", "0")
        time.sleep(0.5)
        g0 = ps_proj(PC_FREE)
        press("p")
        p = pc_playing(PC_FREE)
        ps_settle()
        t1 = windows3d()["tiling"]
        g1 = ps_proj(PC_FREE)
        bw, bh, bx, by = ps_box(frame("31e-staying"))
        check("31e", "a ring that stays (Y), you 1 m from its middle towards the game, P: the game takes play_size (0.5) of your view (made smaller: it's nearer)",
              p.get("fill") is False and t1["atRing"] and abs(g1["share"] - 0.5) < 0.03,
              f"at the ring {t1['atRing']}; before P {g0}; played {g1}; in the frame {bw:.3f} x {bh:.3f}; the plugin's played {t1.get('played')}")
        press("meta_l", "esc")
        time.sleep(1.0)
        ctl("hypr3d", "tile", "follow", "on")
        ctl("hypr3d", "spawn")
        time.sleep(1.2)
        ps_settle()
    ps_part("a ring that stays, off its middle", staying)

    def away():
        # 3.5 m behind a staying ring's middle: the game is played as the ring has it
        aim_row(PC_FREE)
        ctl("hypr3d", "turn", f"{st()['yaw']:.2f}", "0")
        time.sleep(0.4)
        ctl("hypr3d", "tile", "follow", "off")
        time.sleep(0.6)
        s = st()
        yaw = math.radians(s["yaw"])
        ctl("hypr3d", "tp", f"{s['feet'][0] - 3.5 * math.sin(yaw):.3f}", f"{s['feet'][1]:.3f}", f"{s['feet'][2] + 3.5 * math.cos(yaw):.3f}")
        time.sleep(1.0)
        aim_row(PC_FREE)
        time.sleep(0.5)
        pl0, g0 = placed(PC_FREE) or {}, ps_proj(PC_FREE)
        press("p")
        p = pc_playing(PC_FREE)
        ps_settle()
        t1, pl1, g1 = windows3d()["tiling"], placed(PC_FREE) or {}, ps_proj(PC_FREE)
        ps_wheel(-2)
        time.sleep(0.8)
        g2, told, size = ps_proj(PC_FREE), ctl("hypr3d", "log", "8"), (st()["playing"] or {}).get("size")
        check("31e", "away from a ring that stays (3.5 m behind its middle), P on the game in the row: played as the ring has it, as big as it was, where it was",
              p.get("fill") is False and not t1["atRing"] and t1.get("played") is None and abs(g1["share"] - g0["share"]) < 0.02
              and math.dist(pl0.get("center", [0, 0, 0]), pl1.get("center", [9, 9, 9])) < 0.02,
              f"at the ring {t1['atRing']}; before P {g0}, played {g1}; its middle {pl0.get('center')} -> {pl1.get('center')}; the plugin's played {t1.get('played')}")
        check("31e", "... Super+wheel there: nothing (logged: away from you), play_size as it was", abs(g2["share"] - g1["share"]) < 0.02 and "away from you" in told and size == 0.5,
              f"{g1['share']} -> {g2['share']}; the play's size {size}; {told.strip()[-240:]}")
        press("meta_l", "esc")
        time.sleep(1.0)
        ctl("hypr3d", "tile", "follow", "on")
        ctl("hypr3d", "spawn")
        time.sleep(1.2)
        ps_settle()
    ps_part("away from a ring that stays", away)

    def ended():
        # a terminal opening ends play and joins the row beside the game, which keeps its size and place; once it's gone
        # the game is played again, half the view
        aim_row(PC_FREE)
        ctl("hypr3d", "turn", f"{st()['yaw']:.2f}", "0")
        time.sleep(0.5)
        press("p")
        pc_playing(PC_FREE)
        ps_settle()
        g0, pl0 = ps_proj(PC_FREE), ps_played()
        alice("setsid -f foot --app-id h3d-epop > /dev/null 2>&1")
        s1 = pc_wait("play ended by itself", lambda: (lambda s: s if s["playing"] is None and s.get("playHeld") else None)(st()), 15, 0.2)
        pc_wait("the terminal in the row", lambda: "h3d-epop" in tiled_row(), 10, 0.3)
        time.sleep(0.5)
        ps_settle()
        g1, row = ps_proj(PC_FREE), tiled_row()
        check("31e", "play ending by itself (a terminal opening takes the keyboard, the game's keys held back): the game keeps its size and its place, the terminal beside it",
              s1 and abs(g1["share"] - g0["share"]) < 0.03 and abs(g1["x"] - g0["x"]) < 0.03 and abs(g1["y"] - g0["y"]) < 0.03 and "h3d-epop" in row,
              f"held {bool(s1)}; played {g0} ({pl0}) -> held back {g1}; row {row}")
        machine.execute("pkill -f 'app-id h3d-epop'; true")
        wait_for("the terminal gone", lambda: not client("h3d-epop"), 10, 0.3)
        time.sleep(0.8)
        if fs_active().get("class") != PC_FREE:
            ctl("dispatch", f'hl.dsp.focus({{ window = "class:{PC_FREE}" }})')
        p = pc_playing(PC_FREE)
        ps_settle()
        g2 = ps_proj(PC_FREE)
        check("31e", "... the terminal gone, the keyboard back with the game: played again, half the view, in its middle", bool(p) and abs(g2["share"] - 0.5) < 0.03 and abs(g2["x"]) < 0.05,
              f"played {bool(p)}: {g2}")
        if st()["playing"]:
            press("meta_l", "esc")
        time.sleep(1.0)
        ctl("hypr3d", "spawn")
        time.sleep(1.0)
        ps_settle()
    ps_part("play ending by itself", ended)

    def v_switch():
        aim_row(PC_FREE)
        ctl("hypr3d", "turn", f"{st()['yaw']:.2f}", "0")
        time.sleep(0.5)
        press("p")
        pc_playing(PC_FREE)
        ps_settle()
        g0 = ps_proj(PC_FREE)
        ctl("hypr3d", "view", "third")
        pc_steady()
        ps_settle()
        g1 = ps_proj(PC_FREE)
        bw, bh, bx, by = ps_box(frame("31e-v-third"))
        check("31e", "V (hyprctl hypr3d view third) while playing here: still half the view, in its middle (the row turned to it from the camera behind the avatar)",
              abs(g1["share"] - 0.5) < 0.03 and abs(g1["x"]) < 0.05 and abs(g1["y"]) < 0.05, f"first {g0} -> third {g1}; in the frame {bw:.3f} x {bh:.3f} (middle {bx:+.3f}, {by:+.3f})")
        press("meta_l", "esc")
        time.sleep(1.0)
        ctl("hypr3d", "view", "first")
        ctl("hypr3d", "spawn")
        time.sleep(1.0)
        ps_settle()
    ps_part("V while playing", v_switch)

    def wall():
        # third person, the avatar's back to the courtyard's south wall: the boom is pulled in
        ctl("hypr3d", "view", "third")
        ctl("hypr3d", "tp", "0.3", "0", "20.9")
        time.sleep(1.2)
        aim_row(PC_FREE)
        pc_steady()
        ps_settle()
        a = av()
        aimed = (st().get("aimed") or {}).get("class")
        press("p")
        p = pc_playing(PC_FREE)
        ps_settle()
        g1, pl = ps_proj(PC_FREE), ps_played()
        bw, bh, bx, by = ps_box(frame("31e-wall"))
        check("31e", "third person, the camera's boom pulled in by a wall behind the avatar, P: the game takes play_size (0.5) of the view as the camera draws it",
              aimed == PC_FREE and p.get("fill") is False and a.get("distance", 9) < 2.0 and abs(g1["share"] - 0.5) < 0.03,
              f"aimed {aimed}; the boom {a.get('distance')} m (2.6 wanted); drawn {g1}; in the frame {bw:.3f} x {bh:.3f} (middle {bx:+.3f}, {by:+.3f}); the plugin's played {pl}")
        ps_wheel(-9)
        time.sleep(0.8)
        g2, pl2 = ps_proj(PC_FREE), ps_played()
        bw, bh, bx, by = ps_box(frame("31e-wall-most"))
        check("31e", "... Super+wheel up to the most: 94% of the view as the camera draws it, not more than the view", abs(g2["share"] - 0.94) < 0.03,
              f"drawn {g2}; in the frame {bw:.3f} x {bh:.3f}; the plugin's played {pl2}")
        ps_wheel(9)
        press("meta_l", "esc")
        time.sleep(1.0)
        ctl("hypr3d", "view", "first")
        ctl("hypr3d", "spawn")
        time.sleep(1.0)
    ps_part("third person by a wall", wall)

    ps_config(0.5)
    machine.execute("pkill -f 'h3dgame --title h3dgame-pc'; pkill -u alice foot; true")
    ensure_3d(False)


CL_SLOW36 = CL_SLOW.replace("root.after(1300", "root.after(3600")  # 3.6 s to close, like a game saving


def sh_slow_window():
    """a Tk window that closes 3.6 s after it's asked to, into the row; its class"""
    machine.succeed(f"cat > {H}/slowclose36.py << 'H3D_EOF'\n{CL_SLOW36}\nH3D_EOF\nchown alice:users {H}/slowclose36.py")
    alice(f"DISPLAY={x_display()} setsid -f python3 -u {H}/slowclose36.py h3dslow > /tmp/slowclose36.log 2>&1")
    return cl_wait("the slow window in the row", lambda: next((c for c in cl_clients() if c.lower() == "h3dslow" and c in tiled_row()), None), 30, 0.5)


def sh_ws():
    try:
        return json.loads(ctl("-j", "activeworkspace")).get("id")
    except ValueError:
        return None


def sh_popup():
    alice("setsid -f foot --app-id h3d-popup > /dev/null 2>&1")


def sh_popup_held():
    """plays the game in the row, then a popup takes the keyboard; True if the keys are held back"""
    cl_fresh(1)
    cl_game()
    aim_row("h3dgame")
    play_on()
    sh_popup()
    up = cl_wait("the popup with the keyboard", lambda: cl_active() == "h3d-popup", 20, 0.2)
    s0 = cl_held(True, 3)
    ctl("hypr3d", "aim", "h3d-popup")
    time.sleep(0.5)
    return up and s0.get("playHeld") is True


def sh_slow(played):
    """Super+Q on a 3.6 s slow window (walking or playing), and again 0.3 s after it went"""
    tag = "a game played" if played else "walking"
    cl_fresh(3)
    c = sh_slow_window()
    time.sleep(1.0)
    aim_row(c)
    if played:
        play_on()
    before = cl_snap(f"31f {tag}: before")
    press("meta_l", "q", after=0.0)
    t0 = time.time()
    gone = cl_wait("the slow window gone", lambda: c not in cl_clients(), 12, 0.05)
    t1 = time.time()
    time.sleep(0.3)
    mid = cl_snap(f"31f {tag}: 0.3 s after it went")
    press("meta_l", "q", after=0.0)
    time.sleep(1.5)
    after = cl_snap(f"31f {tag}: after the second Super+Q")
    closed = sorted(before["clients"] - after["clients"])
    told = ctl("hypr3d", "log", "14").strip()
    check("31f", f"{tag}: a window slow to close (3.6 s), Super+Q, then again just after it went: only it closes, the second for no window",
          bool(gone) and closed == [c] and "a shortcut: the keyboard to none (a window with the keyboard closed" in told.split("closed: " + c)[-1],
          f"closed {closed}; it went {t1 - t0:.1f} s after Super+Q; then the crosshair on {mid['aimed']}, the keyboard on {mid['active']}; {told[-300:]}")
    machine.execute("pkill -f slowclose36[.]py; true")
    ctl("hypr3d", "play", "off")


def sh_slow_over_game():
    """a slow window over the played game: Super+Q twice, game keys meanwhile, once more after it went"""
    cl_fresh(2)
    cl_game()
    aim_row("h3dgame")
    play_on()
    machine.succeed(f"cat > {H}/slowclose36.py << 'H3D_EOF'\n{CL_SLOW36}\nH3D_EOF\nchown alice:users {H}/slowclose36.py")
    alice(f"DISPLAY={x_display()} setsid -f python3 -u {H}/slowclose36.py h3dslow > /tmp/slowclose36.log 2>&1")
    up = cl_wait("the slow window with the keyboard", lambda: (cl_active() or "").lower() == "h3dslow", 30, 0.1)
    c = next((k for k in cl_clients() if k.lower() == "h3dslow"), None)
    ctl("hypr3d", "aim", c)
    time.sleep(0.3)
    before = cl_snap("31f over a game: before")
    press("meta_l", "q", after=0.0)
    time.sleep(0.3)
    press("meta_l", "q", after=0.0)
    t0 = time.time()
    while c in cl_clients() and time.time() - t0 < 12:
        press("1", after=0.0)  # a game key, held back
        time.sleep(0.5)
    time.sleep(0.3)
    mid = cl_snap("31f over a game: 0.3 s after it went")
    press("meta_l", "q", after=0.0)
    time.sleep(1.5)
    after = cl_snap("31f over a game: after the third Super+Q")
    closed = sorted(before["clients"] - after["clients"])
    check("31f", "a game played, a window slow to close (3.6 s) over it: Super+Q twice on it, the game's keys pressed meanwhile, and once more just after it went: the game isn't closed",
          up and c not in after["clients"] and "h3dgame" not in closed, f"closed {closed}; then playing {mid['playing']}, the keyboard on {mid['active']}")
    machine.execute("pkill -f slowclose36[.]py; true")
    ctl("hypr3d", "play", "off")


def sh_workspace():
    """Super+Q twice on a popup over the game, then an empty workspace 2, then a turn: the monitor stays there"""
    held = sh_popup_held()
    b, a, closed = cl_press("31f: Super+Q twice on the popup, then workspace 2", times=2, every=0.8, wait=0.5)
    ctl("dispatch", 'hl.dsp.focus({ workspace = "2" })')
    time.sleep(0.6)
    ws1, act1 = sh_ws(), cl_active()
    rel(100, 0)  # turning ends the shortcut hold
    time.sleep(1.5)
    s2, ws2, act2 = st(), sh_ws(), cl_active()
    check("31f", "Super+Q twice on a popup over the game, then an empty workspace 2 (the game still drawn in the ring), then a turn: the monitor stays on workspace 2, the game not given the keyboard",
          held and closed == ["h3d-popup"] and ws1 == 2 and ws2 == 2 and act2 is None and s2["playing"] is None,
          f"closed {closed}; ws {ws1} -> {ws2}; the keyboard {act1} -> {act2}; playing {(s2['playing'] or {}).get('class')}")
    ctl("hypr3d", "play", "off")
    ctl("dispatch", 'hl.dsp.focus({ workspace = "1" })')
    time.sleep(0.5)


def sh_alt_held():
    """a popup over the played game, Alt+1 held back, then the popup closes by itself"""
    if "h3dgame" not in cl_clients():
        cl_fresh(1)
        cl_game()
    aim_row("h3dgame")
    play_on()
    sh_popup()
    up = cl_wait("the popup with the keyboard", lambda: cl_active() == "h3d-popup", 20, 0.2)
    s0 = cl_held(True, 3)
    game_mark("31f-alt")
    press("alt", "1", after=0.0)  # a game's Alt chord, held back: reaches nothing
    machine.execute("pkill -f 'app-id h3d-popup'; true")
    cl_wait("the popup gone", lambda: "h3d-popup" not in cl_clients(), 10, 0.05)
    time.sleep(0.5)
    press("1", after=0.0)
    s1 = cl_wait("played again", lambda: (lambda s: s if s["playing"] else None)(st()), 1.5, 0.1) or st()
    got = [l for l in game_lines("31f-alt") if l.startswith("key") or l.startswith("focus")]
    check("31f", "a popup over the game played, Alt+1 pressed (a game's key, held back), then the popup closes by itself: the game played again at once, a key typed 0.5 s after reaching it",
          up and s0.get("playHeld") is True and (s1["playing"] or {}).get("class") == "h3dgame" and "key down 1" in got,
          f"playing {(s1['playing'] or {}).get('class')}; playHeld {s1.get('playHeld')}; the game got {got[-6:]}")
    ctl("hypr3d", "play", "off")


def sh_resized(args, tag, off_middle=False):
    """a played game toggles fullscreen twice with the mouse still (or moved right first): no leave or enter"""
    cl_fresh(1)
    cl_game(args)
    aim_row("h3dgame")
    play_on()
    time.sleep(0.5)
    game_mark(f"31f-{tag}-in")
    press("f")
    fs = cl_wait("fullscreen", lambda: cl_fullscreen("h3dgame") == 2, 10, 0.2)
    time.sleep(1.5)
    if off_middle:
        rel(300, 0)  # outside the window's smaller size
        time.sleep(0.5)
    game_mark(f"31f-{tag}-out")
    press("f")
    out = cl_wait("out of fullscreen", lambda: cl_fullscreen("h3dgame") == 0, 10, 0.2)
    time.sleep(1.5)
    s2 = st()
    lin, lout = game_lines(f"31f-{tag}-in"), game_lines(f"31f-{tag}-out")
    ptr, size = (s2["playing"] or {}).get("pointer"), (cl_clients().get("h3dgame") or {}).get("size")
    check("31f", f"a game played ({tag}), in and out of fullscreen with the mouse still: no mouse leave or enter for it either way, the pointer on it after",
          fs and out and not any(l in ("mouse leave", "mouse enter") for l in lin + lout) and bool(ptr and size and 0 <= ptr[0] < size[0] and 0 <= ptr[1] < size[1]),
          f"in: {[l for l in lin if not l.startswith('frames')][-6:]}; out: {[l for l in lout if not l.startswith('frames')][-6:]}; pointer {ptr}, the window {size}")
    ctl("hypr3d", "play", "off")
    machine.execute("pkill -x h3dgame; true")


def sh_launcher(kind):
    """Super+Q twice on a popup over the game, then Super+D's launcher (on demand like Quickshell's, or exclusive like
    rofi's) open past the 5 s hold, a letter, Esc"""
    machine.execute(f"pkill -f 'quickshell -p'; rm -f {LAUNCHER_LOG}; true")
    alice(f"setsid -f quickshell -p {LAUNCHER} > /tmp/quickshell-launcher.log 2>&1")
    wait_for("the launcher's IPC", lambda: as_alice(f"quickshell ipc -p {LAUNCHER} show", 10)[0] == 0, 30, 0.5)
    ctl("eval", 'hl.unbind("SUPER + D")')
    ctl("eval", f'hl.bind("SUPER + D", hl.dsp.exec_cmd("quickshell ipc -p {LAUNCHER} call launcher {kind}"))')
    held = sh_popup_held()
    t0 = time.time()
    b, a, closed = cl_press(f"31f {kind}: Super+Q twice on the popup, then the launcher", times=2, every=0.8, wait=0.3)
    press("meta_l", "d")
    time.sleep(1.0)
    press("a")  # typed into the launcher
    time.sleep(max(0.0, t0 + 6.5 - time.time()))  # past the 5 s hold, without turning or moving
    s1, act1 = st(), cl_active()
    game_mark(f"31f-{kind}")
    press("b")
    time.sleep(0.5)
    lg = launcher_log()
    got = [l for l in game_lines(f"31f-{kind}") if l.startswith("key")]
    check("31f", f"Super+Q twice on a popup over the game, then a launcher ({kind}) open past the hold's end: it keeps the keyboard (a letter typed goes to it, not the game)",
          held and closed == ["h3d-popup"] and s1["playing"] is None and "key down B" not in got and any("text" in l and "b" in l for l in lg),
          f"the keyboard on {act1}; playing {(s1['playing'] or {}).get('class')}; the launcher said {lg[-6:]}; the game got {got[-4:]}")
    press("esc")
    s2 = cl_wait("played again", lambda: (lambda s: s if s["playing"] else None)(st()), 4) or st()
    told = ctl("hypr3d", "log", "10").strip()
    check("31f", f"... Esc (the launcher gone): the game gets the keyboard back and is played again", (s2["playing"] or {}).get("class") == "h3dgame"
          and ("the keyboard back to h3dgame" in told or "h3dgame has the keyboard again" in told), f"playing {(s2['playing'] or {}).get('class')}, the keyboard on {cl_active()}; {told[-300:]}")
    ctl("hypr3d", "play", "off")
    machine.execute("pkill -f 'quickshell -p'; true")
    ctl("eval", 'hl.unbind("SUPER + D")')


def sh_fullscreen_twice():
    """fullscreen auto-played game, a popup over it, the key hold run out: Super+Q twice on the popup, then a turn"""
    cl_fresh(1)
    cl_game()
    ctl("dispatch", 'hl.dsp.focus({ window = "class:h3dgame" })')
    time.sleep(0.3)
    ctl("dispatch", 'hl.dsp.window.fullscreen({ action = "set", mode = "fullscreen", window = "class:h3dgame" })')
    p0 = cl_wait("the game played by itself, fullscreen", lambda: (lambda s: s["playing"] if s["playing"] else None)(st()), 5)
    sh_popup()
    up = cl_wait("the popup with the keyboard", lambda: cl_active() == "h3d-popup", 20, 0.2)
    cl_held(True, 3)
    s1 = cl_held(False, 8)  # 4 s without a key: walking again
    ctl("hypr3d", "aim", "h3d-popup")
    time.sleep(0.5)
    b, a, closed = cl_press("31f: the game fullscreen, its keys' hold run out, Super+Q twice on the popup", times=2, every=0.8, wait=1.0)
    s2 = st()
    rel(100, 0)  # turning ends the shortcut hold
    s3 = cl_wait("played again", lambda: (lambda s: s if s["playing"] else None)(st()), 3) or st()
    fs = cl_fullscreen("h3dgame")
    check("31f", "the game fullscreen (played by itself, its keys' hold run out), Super+Q twice on a popup over it, then a turn: only the popup closed, the game played again (as after one press)",
          (p0 or {}).get("class") == "h3dgame" and up and s1.get("playHeld") is False and closed == ["h3d-popup"] and fs == 2 and s2["playing"] is None
          and (s3["playing"] or {}).get("class") == "h3dgame", f"closed {closed}; before the turn: playing {(s2['playing'] or {}).get('class')}; after: playing "
          f"{(s3['playing'] or {}).get('class')}, the keyboard on {cl_active()}; fullscreen {fs}")
    ctl("hypr3d", "play", "off")
    machine.execute("pkill -x h3dgame; pkill -f 'app-id h3d-popup'; true")


@section("31f", "the hold on shortcuts after a close, and the game's keyboard given back: a window slow to close (3.6 s), walking and with a game played, Super+Q pressed again after it went; the game's keys pressed meanwhile; Super+Q twice on a popup over the game, then an empty workspace, or a launcher (on demand and exclusive) past the hold's end; Alt+1 as a game's key; the game fullscreen with its keys' hold run out; a game played in and out of fullscreen with the mouse still (a pointer lock too, off its middle, Hyprland's animations on)")
def s_shortcut_hold():
    lua_session()
    ensure_plugin()
    ctl("eval", CL_BIND)
    ctl("eval", 'hl.window_rule({ name = "h3d-cl-popup", match = { class = "h3d-popup" }, float = true })')
    parts = (("slow, walking", lambda: sh_slow(False)), ("slow, a game played", lambda: sh_slow(True)), ("slow, over a game", sh_slow_over_game),
             ("a workspace", sh_workspace), ("Alt+1", sh_alt_held), ("resized", lambda: sh_resized("", "free")),
             ("resized, locked", lambda: sh_resized("--relative", "locked")), ("a launcher", lambda: sh_launcher("toggle")),
             ("an exclusive launcher", lambda: sh_launcher("exclusive")), ("fullscreen, twice", sh_fullscreen_twice))
    for name, fn in parts:
        try:
            fn()
        except Exception as e:  # noqa: BLE001
            traceback.print_exc()
            check("31f", f"{name}: ran to the end", False, f"{type(e).__name__}: {e}"[:600])
            try:
                ctl("hypr3d", "play", "off")
                ctl("dispatch", 'hl.dsp.focus({ workspace = "1" })')
            except Exception:  # noqa: BLE001
                pass
    # with Hyprland's default animations the window resizes over several frames, the app's new buffer arriving before
    # the box
    ctl("eval", 'hl.config({ animations = { enabled = true } })')
    try:
        for name, fn in (("resized, animated", lambda: sh_resized("", "animated")), ("resized, animated, off the middle", lambda: sh_resized("", "animated, off the middle", True))):
            try:
                fn()
            except Exception as e:  # noqa: BLE001
                traceback.print_exc()
                check("31f", f"{name}: ran to the end", False, f"{type(e).__name__}: {e}"[:600])
                try:
                    ctl("hypr3d", "play", "off")
                except Exception:  # noqa: BLE001
                    pass
    finally:
        ctl("eval", 'hl.config({ animations = { enabled = false } })')
    ctl("eval", 'hl.unbind("SUPER + Q")')
    machine.execute("pkill -x h3dgame; pkill -u alice foot; pkill -f slowclose36[.]py; pkill -f 'quickshell -p'; true")


@section("34", "an X11 game the way CS2 runs (SDL3's x11 driver through XWayland, fullscreen, its menu's pointer, then mouse-look in a match), opened in 3D after a move of Hyprland's that no frame followed was left on an X11 window on the 2D desktop: every move of the mouse reaches its mouse-look, played here and filling the view; and the cursor on a window's edge (resize_on_border's arrow) as 3D comes in: an app's own cursor drawn on its panel, not the arrow")
def s_x11_look():
    lua_session()
    ensure_3d(False)
    clean_windows()
    # flat accel: menu pointer moves match the mouse counts exactly
    ctl("eval", 'hl.config({ input = { accel_profile = "flat" } })')
    # XWayland holds a frame-less Hyprland pointer move on an X11 window (hl.dsp.cursor.move, a closing window) and
    # delivers it, as absolute, on the next frame over its windows (the game's). The xterm stays till the edge check: if
    # closed now, the game reuses its X ids, which Hyprland 0.55.2's XWM still records, and never maps
    ctl("dispatch", "hl.dsp.workspace.toggle_special()")
    alice(f"DISPLAY={x_display()} setsid -f xterm -class h3dstale > /dev/null 2>&1")
    wait_for("xterm on the scratchpad", lambda: (lambda c: c if c and c["workspace"]["name"].startswith("special") else None)(client("h3dstale")), 20)
    time.sleep(1)
    x = client("h3dstale")
    ctl("dispatch", f'hl.dsp.cursor.move({{ x = {x["at"][0] + x["size"][0] // 3}, y = {x["at"][1] + x["size"][1] // 3} }})')
    time.sleep(0.3)
    ctl("dispatch", "hl.dsp.workspace.toggle_special()")  # hides the scratchpad and the xterm
    time.sleep(0.5)
    # opened in 3D in front of you, as CS2 from Super+D, the crosshair moving over it as it maps: if XWayland sends
    # those moves from its absolute device, SDL3 reads later relative moves as positions (mouse-look sees only
    # differences)
    ensure_3d()
    game_start("", env=f"DISPLAY={x_display()} SDL_VIDEODRIVER=x11", cls="h3dgame-x11")
    # fullscreen after map, as CS2 does (Hyprland 0.55.2 ignores an X11 fullscreen state set before map)
    ctl("dispatch", 'hl.dsp.window.fullscreen({ mode = "fullscreen", action = "set", window = "class:h3dgame-x11" })')
    p = wait_for("the game played by itself", lambda: st()["playing"], 15, 0.2)
    g = client("h3dgame-x11")
    check("34", "h3dgame through XWayland (SDL's x11 driver), fullscreen, played by itself", g and g["xwayland"] and g["fullscreen"] == 2 and p["class"] == "h3dgame-x11",
          f"{g and (g['xwayland'], g['fullscreen'])}; {p}")
    for view in ("here", "fill"):
        if view == "fill":
            press("meta_l", "esc")
            time.sleep(0.8)
            aim_find("h3dgame-x11")
            press("shift", "p")
            wait_for("filling the view", lambda: play_settled(st()), 10, 0.2)
        game_mark(f"{view} menu")
        rel(100, 0)
        rel(0, 50)
        lines = game_lines(f"{view} menu")
        moves = [l for l in lines if l.startswith("motion ")]
        right = sum(int(l.split()[3]) for l in moves)
        down = sum(int(l.split()[4]) for l in moves)
        check("34", f"played {view}: the pointer moves over its menu (100 right, 50 down)", 95 <= right <= 105 and 45 <= down <= 55, moves[:6])
        # a match: SDL relative mode (cursor hidden, pointer grabbed; XWayland locks it)
        game_mark(f"{view} relative")
        press("r")
        s = wait_for("the pointer locked", lambda: (lambda s: s if s["playing"] and s["playing"]["locked"] else None)(st()), 5, 0.2)
        check("34", f"played {view}: R, mouse-look on, the pointer locked", "relative on" in game_lines(f"{view} relative") and s, game_lines(f"{view} relative")[:4])
        game_mark(f"{view} look")
        rel(100, 0)
        rel(0, 40)
        rel(1500, 0)  # a quick turn: fifteen PS/2 moves
        click("left")
        lines = game_lines(f"{view} look")
        dx, dy, _ = game_motion(lines)
        check("34", f"played {view}: mouse-look gets every move (100 right, 40 down, then a turn of 1500), and the click",
              abs(dx - 1600) <= 3 and abs(dy - 40) <= 3 and any(l.startswith("button down 1") for l in lines),
              f"dx {dx}, dy {dy}; {[l for l in lines if not l.startswith('frames')][:8]}")
        press("r")
        time.sleep(0.5)
    press("meta_l", "esc")
    machine.execute("pkill -x h3dgame; true")
    ensure_3d(False)
    # 3D entered with the cursor on a window's border gap (Hyprland's resize arrow): apps' own cursors drawn, not the
    # arrow
    machine.execute("pkill -u alice foot; pkill -x xterm; true")
    ctl("eval", 'hl.config({ general = { resize_on_border = true, extend_border_grab_area = 30 } })')
    alice(f"DISPLAY={x_display()} setsid -f xterm -class h3dxterm > /dev/null 2>&1")
    wait_for("xterm", lambda: client("h3dxterm"), 20)
    time.sleep(1.5)
    x = client("h3dxterm")
    m = json.loads(ctl("-j", "monitors"))[0]
    tablet((x["at"][0] - 12) * 32767 / m["width"], (x["at"][1] + x["size"][1] // 2) * 32767 / m["height"])
    rel(0, 2)
    time.sleep(0.5)
    ensure_3d()
    aim_find("h3dxterm")
    time.sleep(0.8)
    s = st()
    frame("34-edge-cursor")
    check("34", "the cursor on a window's edge (its resize arrow) as 3D comes in: xterm's own cursor drawn at the crosshair, not the arrow",
          s["cursor"] is not None and s["cursor"]["size"] != [32, 32], s["cursor"])
    ensure_3d(False)
    machine.execute("pkill -x xterm; true")
    ctl("eval", 'hl.config({ general = { resize_on_border = false }, input = { accel_profile = "" } })')
    clean_windows()


def emote_sound_node():
    """the plugin's emote sound stream, and the nodes it's linked to; None = there's none"""
    dump = json.loads(alice("pw-dump"))
    nodes = {o["id"]: (o.get("info") or {}).get("props") or {} for o in dump if o.get("type") == "PipeWire:Interface:Node"}
    ours = [i for i, p in nodes.items() if p.get("node.name") == "hypr3d-emote-sound"]
    if not ours:
        return None
    links = [o for o in dump if o.get("type") == "PipeWire:Interface:Link" and (o.get("info") or {}).get("output-node-id") == ours[0]]
    return {"id": ours[0], "props": nodes[ours[0]],
            "to": sorted({nodes.get((l.get("info") or {}).get("input-node-id"), {}).get("node.name") for l in links} - {None})}


def wav_floats(path):
    """the samples of a 32-bit float WAV file (pw-record's)"""
    import array
    b, i = Path(path).read_bytes(), 12
    while i + 8 <= len(b):
        tag, n = b[i:i + 4], int.from_bytes(b[i + 4:i + 8], "little")
        if tag == b"data":
            a = array.array("f")
            a.frombytes(b[i + 8:i + 8 + n - n % 4])
            return a
        i += 8 + n + (n & 1)
    return []


def tone_level(x, f, rate=48000):
    """the amplitude of frequency f in x (Goertzel: a sine of amplitude A gives A)"""
    w = 2 * math.pi * f / rate
    c, s1, s2 = 2 * math.cos(w), 0.0, 0.0
    for v in x:
        s1, s2 = v + c * s1 - s2, s1
    return 2 * math.sqrt(max(0.0, s1 * s1 + s2 * s2 - c * s1 * s2)) / max(1, len(x))


def rms(x):
    return math.sqrt(sum(v * v for v in x) / max(1, len(x)))


@section("35", "an emote's sound (the settings file's \"sound\", a dance's song): played through PipeWire as \"hypr3d emote sound\" while the emote plays, to the default output (WirePlumber links it), round as it loops without a gap, at emote_volume (0.5 unless set, 1 twice as loud, 0 none), stopping with the emote (after a fade) and at once out of 3D; the dance where its song is heard; played again it starts again, and 3D back in the middle of it carries on from where the dance is")
def s_emote_sound():
    ensure_avatar(AV)
    ensure_3d()
    ctl("hypr3d", "avatar", "lipsync", "off")
    mic_noise(False)
    ctl("hypr3d", "avatar", "emote", "stop")
    time.sleep(0.5)

    def emotes():
        return ctlj("hypr3d", "avatar", "emote")

    em = emotes()
    song = next((e for e in em["emotes"] if e["name"] == "Hands Song"), {})
    snd = song.get("sound") or {}
    check("35", "the settings file's \"sound\": listed with its emote (HandsSong.ogg, 2 s, 48 kHz, stereo)",
          snd.get("file") == "HandsSong.ogg" and abs(snd.get("duration", 0) - 2) < 0.01 and snd.get("rate") == 48000 and snd.get("channels") == 2, song)
    check("35", "... and the other emotes have none", [e["name"] for e in em["emotes"] if e["sound"]] == ["Hands Song"],
          [e["name"] for e in em["emotes"] if e["sound"]])
    check("35", "emote_volume is 0.5 unless set; built with PipeWire", em["sound"]["volume"] == 0.5 and em["sound"]["available"] is True, em["sound"])
    check("35", "... and no stream while no emote with a sound plays", emote_sound_node() is None)

    # records the default output, the test microphone's (mono) sink
    def record(name):
        alice(f"setsid -f pw-record --target test_mic --rate 48000 --channels 1 --format f32 -P node.name=h3d-rec35 /tmp/{name}.wav > /dev/null 2>&1")
        time.sleep(0.6)

    def recorded(name):
        machine.execute("pkill -INT -f 'node.name=h3d-rec[3]5'; true")
        time.sleep(0.6)
        copy_out(f"/tmp/{name}.wav", "raw")
        return wav_floats(RAW / f"{name}.wav")

    def heard(more=0.3):
        return wait_for("its sound heard", lambda: (lambda e: e if e["sound"]["on"] and e["sound"]["at"] >= more else None)(emotes()), 10, 0.1)

    def played(x):
        """first and last 10 ms window above -60 dBFS, and all windows' RMS"""
        win = [rms(x[i:i + 480]) for i in range(0, len(x) - 479, 480)]
        loud = [i for i, w in enumerate(win) if w > 1e-3]
        return (loud[0], loud[-1], win) if loud else (0, -1, win)

    record("35-half")
    ctl("hypr3d", "avatar", "emote", "Hands Song")
    e = heard()
    n = emote_sound_node()
    props = {k: (n or {}).get("props", {}).get(k) for k in ("node.description", "media.name", "application.name", "media.role")}
    check("35", "played: a PipeWire stream, \"hypr3d emote sound\", named in the mixer for the emote, linked to the default output",
          n is not None and props["media.name"] == "Hands Song" and props["application.name"] == "hypr3d" and "test_mic_in" in n["to"],
          {**props, "to": (n or {}).get("to")})
    check("35", "... streaming, round as the emote loops, from the start (where the dance was when it was first heard)",
          e["sound"]["stream"] == "streaming" and e["sound"]["loop"] is True and e["sound"]["file"] == "HandsSong.ogg" and 0 <= e["sound"]["start"] < 1.0,
          e["sound"])
    # the emote's time (mod 7 s) follows the song's clock, which runs on past the 2 s song; the dance lags a frame at
    # most
    dur, offs = song.get("duration", 7.0), []
    for _ in range(8):
        e = emotes()
        if e["playing"] == "Hands Song" and e["sound"]["at"] >= 0:
            d = (e["sound"]["at"] - e["time"]) % dur
            offs.append(min(d, dur - d))
        time.sleep(0.3)
    check("35", "the dance where its song is heard: the emote's time is the song's clock, round the emote's 7 s", len(offs) >= 6 and max(offs) < 0.15,
          [round(d, 3) for d in offs])
    t1, a1 = time.time(), emotes()["sound"]["at"]
    time.sleep(2.0)
    t2, a2 = time.time(), emotes()["sound"]["at"]
    check("35", "its clock goes at the rate time does", abs((a2 - a1) - (t2 - t1)) < 0.2, f"{a2 - a1:.3f} s heard in {t2 - t1:.3f} s")
    ctl("hypr3d", "avatar", "emote", "stop")
    ts = time.time()
    wait_for("the stream gone", lambda: emote_sound_node() is None, 5, 0.05)
    took = time.time() - ts
    check("35", "the emote stopped: the stream goes after the song's fade (0.3 s)", 0.25 <= took < 2.0, f"{took:.2f} s")
    x = recorded("35-half")
    a, b, win = played(x)
    length = (b - a + 1) / 100
    check("35", "what the default output got: the song, once heard for as long as it played", length > 3.5, f"{length:.2f} s of it")
    inner = win[a + 5: b - 35]  # skip the first 50 ms and the fade
    med = sorted(inner)[len(inner) // 2] if inner else 0
    gaps = [i for i, w in enumerate(inner) if w < 0.5 * med]
    check("35", "... with no gap anywhere, round its end (2 s) and on", inner and not gaps, f"{len(gaps)} quiet windows of {len(inner)} (median {med:.4f})")
    mid = x[(a + 50) * 480: (a + 150) * 480]
    l440, l660, l1000 = tone_level(mid, 440), tone_level(mid, 660), tone_level(mid, 1000)
    check("35", "... its two tones (440 Hz on the left, 660 Hz on the right, to a mono sink), as loud as each other, and nothing else",
          l440 > 0.01 and 0.7 < l440 / max(l660, 1e-9) < 1.4 and l1000 < 0.05 * l440, f"440 Hz {l440:.4f}, 660 Hz {l660:.4f}, 1000 Hz {l1000:.5f}")
    half = rms(mid)

    ctl("eval", "hl.config({ plugin = { hypr3d = { emote_volume = 1 } } })")
    record("35-full")
    ctl("hypr3d", "avatar", "emote", "Hands Song")
    heard(1.6)
    ctl("hypr3d", "avatar", "emote", "stop")
    wait_for("the stream gone", lambda: emote_sound_node() is None, 5, 0.05)
    x = recorded("35-full")
    a, b, _ = played(x)
    full = rms(x[(a + 50) * 480: (a + 150) * 480])
    db = 20 * math.log10(max(full, 1e-9) / max(half, 1e-9))
    check("35", "emote_volume = 1: twice as loud as 0.5 (6 dB)", abs(db - 6.02) < 0.5, f"{db:+.2f} dB ({half:.4f} -> {full:.4f})")

    ctl("eval", "hl.config({ plugin = { hypr3d = { emote_volume = 0 } } })")
    ctl("hypr3d", "avatar", "emote", "Hands Song")
    time.sleep(1.5)
    e = emotes()
    check("35", "emote_volume = 0: the emote plays, with no sound and no stream", e["playing"] == "Hands Song" and e["sound"]["on"] is False and emote_sound_node() is None,
          e["sound"])
    ctl("hypr3d", "avatar", "emote", "stop")
    ctl("eval", "hl.config({ plugin = { hypr3d = { emote_volume = 0.5 } } })")
    time.sleep(0.6)

    ctl("hypr3d", "avatar", "emote", "Hands Song")
    first = heard(1.2)["sound"]
    ctl("hypr3d", "avatar", "emote", "Hands Song")
    time.sleep(0.4)
    again = heard(0.1)["sound"]
    check("35", "played again while it plays: the song starts again with it", again["start"] < 1.0 and again["at"] < first["at"],
          f"at {first['at']:.2f} s, then from {again['start']:.2f} s, at {again['at']:.2f} s")
    time.sleep(1.0)
    ensure_3d(False)
    wait_for("the stream gone", lambda: emote_sound_node() is None, 1.5, 0.05)
    e = emotes()
    check("35", "out of 3D: the stream goes at once (no fade: nothing runs to close it)", e["sound"]["on"] is False, e["sound"])
    ensure_3d()
    e = heard(0)
    check("35", "back in 3D in the middle of it: the song again, from where the dance is", e["playing"] == "Hands Song" and e["sound"]["start"] > 0.5,
          f"the dance at {e['time']:.2f} s, the song came in at {e['sound']['start']:.2f} s")
    ctl("hypr3d", "avatar", "emote", "stop")
    wait_for("the stream gone", lambda: emote_sound_node() is None, 5, 0.05)


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


KNOWN_CRASHES = [0]  # expected Hyprland crashes in this VM


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
        # close what it started: Hyprland 0.55.2 crashes quitting with windows open
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
