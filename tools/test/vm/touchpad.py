# touchpad.py [h|d|z]D...: a touchpad made through uinput (as root, in tools/test/vm's VM) that scrolls with two
# fingers: for each amount, both fingers go down, move D device units together (30 a millimetre; > 0 down, towards
# you; "h" first: right, "d": down and right at once) in steps of 1 mm, 12 ms apart, stay a moment and lift, 0.5 s
# before the next. "z" first pinches instead: the fingers move D units apart (< 0: together), each half of it
# sideways, which libinput makes a pinch gesture of (zwp_pointer_gestures_v1: a browser zooms). libinput makes finger scrolling of that (wl_pointer axis_source finger, both axes in a frame when
# they move both ways, then axis_stop when they lift). The touchpad goes away afterwards.
# It's on USB, so libinput pairs it with no keyboard and doesn't disable it while typing.
import fcntl
import os
import struct
import sys
import time

EV_SYN, EV_KEY, EV_ABS = 0, 1, 3
SYN_REPORT = 0
ABS_X, ABS_Y, ABS_MT_SLOT, ABS_MT_POSITION_X, ABS_MT_POSITION_Y, ABS_MT_TRACKING_ID = 0x00, 0x01, 0x2f, 0x35, 0x36, 0x39
BTN_LEFT, BTN_TOOL_FINGER, BTN_TOUCH, BTN_TOOL_DOUBLETAP, BTN_TOOL_TRIPLETAP = 0x110, 0x145, 0x14a, 0x14d, 0x14e
INPUT_PROP_POINTER, INPUT_PROP_BUTTONPAD = 0x00, 0x02
UI_SET_EVBIT, UI_SET_KEYBIT, UI_SET_ABSBIT, UI_SET_PROPBIT = 0x40045564, 0x40045565, 0x40045567, 0x4004556E
UI_DEV_SETUP, UI_ABS_SETUP, UI_DEV_CREATE, UI_DEV_DESTROY = 0x405C5503, 0x401C5504, 0x5501, 0x5502  # (92 and 28 bytes)
BUS_USB = 0x03
W, H, RES = 3000, 2000, 30  # 100 x 67 mm


def main(steps):
    fd = os.open('/dev/uinput', os.O_WRONLY | os.O_NONBLOCK)
    for ev in (EV_KEY, EV_ABS):
        fcntl.ioctl(fd, UI_SET_EVBIT, ev)
    for key in (BTN_LEFT, BTN_TOOL_FINGER, BTN_TOUCH, BTN_TOOL_DOUBLETAP, BTN_TOOL_TRIPLETAP):
        fcntl.ioctl(fd, UI_SET_KEYBIT, key)
    for prop in (INPUT_PROP_POINTER, INPUT_PROP_BUTTONPAD):  # a clickpad, as most are
        fcntl.ioctl(fd, UI_SET_PROPBIT, prop)
    # (struct uinput_abs_setup: the code, padding, then input_absinfo: value, min, max, fuzz, flat, resolution)
    for code, hi, res in ((ABS_X, W, RES), (ABS_Y, H, RES), (ABS_MT_SLOT, 4, 0), (ABS_MT_POSITION_X, W, RES),
                          (ABS_MT_POSITION_Y, H, RES), (ABS_MT_TRACKING_ID, 65535, 0)):
        fcntl.ioctl(fd, UI_SET_ABSBIT, code)
        fcntl.ioctl(fd, UI_ABS_SETUP, struct.pack('HHiiiiii', code, 0, 0, 0, hi, 0, 0, res))
    fcntl.ioctl(fd, UI_DEV_SETUP, struct.pack('HHHH80sI', BUS_USB, 0x1d6b, 0x0105, 1, b'hypr3d test touchpad', 0))
    fcntl.ioctl(fd, UI_DEV_CREATE)

    def emit(*events):
        for kind, code, value in events:
            os.write(fd, struct.pack('llHHi', 0, 0, kind, code, value))
        os.write(fd, struct.pack('llHHi', 0, 0, EV_SYN, SYN_REPORT, 0))

    def fingers(pts, first=False):
        evs = []
        for slot, (x, y) in enumerate(pts):
            evs += [(EV_ABS, ABS_MT_SLOT, slot)]
            if first:
                evs += [(EV_ABS, ABS_MT_TRACKING_ID, 100 + slot)]
            evs += [(EV_ABS, ABS_MT_POSITION_X, round(x)), (EV_ABS, ABS_MT_POSITION_Y, round(y))]
        if first:
            evs += [(EV_KEY, BTN_TOUCH, 1), (EV_KEY, BTN_TOOL_DOUBLETAP, 1)]
        emit(*evs, (EV_ABS, ABS_X, round(pts[0][0])), (EV_ABS, ABS_Y, round(pts[0][1])))

    time.sleep(1.5)  # for libinput, and Hyprland, to take it in
    for step in steps:
        d = int(step.lstrip('hdz'))
        if step[0] == 'z':
            # two fingers from 25 mm apart in the middle, each moving d/2 away from the other
            x0, y0 = W / 2 - 375, H / 2
            n = max(1, round(abs(d) / 2 / RES))
            fingers([(x0, y0), (x0 + 750, y0)], first=True)
            time.sleep(0.012)
            for i in range(1, n + 1):
                h = d / 2 * i / n
                fingers([(x0 - h, y0), (x0 + 750 + h, y0)])
                time.sleep(0.012)
            time.sleep(0.1)
            emit((EV_ABS, ABS_MT_SLOT, 0), (EV_ABS, ABS_MT_TRACKING_ID, -1), (EV_ABS, ABS_MT_SLOT, 1), (EV_ABS, ABS_MT_TRACKING_ID, -1),
                 (EV_KEY, BTN_TOUCH, 0), (EV_KEY, BTN_TOOL_DOUBLETAP, 0))
            time.sleep(0.5)
            continue
        dx, dy = (d, 0) if step[0] == 'h' else (d, d) if step[0] == 'd' else (0, d)
        # two fingers 25 mm apart, side by side, starting near the edges they move away from
        x0 = (W * 0.1 if dx > 0 else W * 0.9 - 750) if dx else W / 2 - 375
        y0 = (H * 0.2 if dy > 0 else H * 0.8) if dy else H / 2
        if not (0 <= x0 + dx and x0 + 750 + dx <= W and 0 <= y0 + dy <= H):
            sys.exit(f"touchpad.py: {step} is too far for the pad")
        n = max(1, round(max(abs(dx), abs(dy)) / RES))
        fingers([(x0, y0), (x0 + 750, y0)], first=True)
        time.sleep(0.012)
        for i in range(1, n + 1):
            f = i / n
            fingers([(x0 + dx * f, y0 + dy * f), (x0 + 750 + dx * f, y0 + dy * f)])
            time.sleep(0.012)
        time.sleep(0.1)
        emit((EV_ABS, ABS_MT_SLOT, 0), (EV_ABS, ABS_MT_TRACKING_ID, -1), (EV_ABS, ABS_MT_SLOT, 1), (EV_ABS, ABS_MT_TRACKING_ID, -1),
             (EV_KEY, BTN_TOUCH, 0), (EV_KEY, BTN_TOOL_DOUBLETAP, 0))
        time.sleep(0.5)
    time.sleep(0.5)
    fcntl.ioctl(fd, UI_DEV_DESTROY)
    os.close(fd)


if __name__ == '__main__':
    main(sys.argv[1:])
