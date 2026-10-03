# gamepad.py [--hold S] STEP...: a uinput Xbox 360 pad in xpad's layout (045e:028e, known to SDL), as root in the VM.
# Steps, 0.4 s apart: a button (a b x y lb rb back start guide ls rs) is pressed and released; lx= ly= rx= ry=
# (-32768..32767), lt= rt= (0..255), dx= dy= (d-pad, -1..1) move an axis there and back. The pad stays --hold s (default
# 1)
import fcntl
import os
import struct
import sys
import time

EV_SYN, EV_KEY, EV_ABS = 0, 1, 3
SYN_REPORT = 0
BUTTONS = {'a': 0x130, 'b': 0x131, 'x': 0x133, 'y': 0x134, 'lb': 0x136, 'rb': 0x137, 'back': 0x13a, 'start': 0x13b,
           'guide': 0x13c, 'ls': 0x13d, 'rs': 0x13e}
# (code, min, max, fuzz, flat) as xpad has them
AXES = {'lx': (0x00, -32768, 32767, 16, 128), 'ly': (0x01, -32768, 32767, 16, 128), 'lt': (0x02, 0, 255, 0, 0),
        'rx': (0x03, -32768, 32767, 16, 128), 'ry': (0x04, -32768, 32767, 16, 128), 'rt': (0x05, 0, 255, 0, 0),
        'dx': (0x10, -1, 1, 0, 0), 'dy': (0x11, -1, 1, 0, 0)}
UI_SET_EVBIT, UI_SET_KEYBIT, UI_SET_ABSBIT = 0x40045564, 0x40045565, 0x40045567
UI_DEV_SETUP, UI_ABS_SETUP, UI_DEV_CREATE, UI_DEV_DESTROY = 0x405C5503, 0x401C5504, 0x5501, 0x5502
BUS_USB = 0x03


def main(args):
    hold = 1.0
    if args[:1] == ['--hold']:
        hold, args = float(args[1]), args[2:]
    fd = os.open('/dev/uinput', os.O_WRONLY | os.O_NONBLOCK)
    for ev in (EV_KEY, EV_ABS):
        fcntl.ioctl(fd, UI_SET_EVBIT, ev)
    for code in BUTTONS.values():
        fcntl.ioctl(fd, UI_SET_KEYBIT, code)
    for code, lo, hi, fuzz, flat in AXES.values():
        fcntl.ioctl(fd, UI_SET_ABSBIT, code)
        fcntl.ioctl(fd, UI_ABS_SETUP, struct.pack('HHiiiiii', code, 0, 0, lo, hi, fuzz, flat, 0))
    fcntl.ioctl(fd, UI_DEV_SETUP, struct.pack('HHHH80sI', BUS_USB, 0x045e, 0x028e, 0x0110, b'Microsoft X-Box 360 pad', 0))
    fcntl.ioctl(fd, UI_DEV_CREATE)

    def emit(kind, code, value):
        os.write(fd, struct.pack('llHHi', 0, 0, kind, code, value))
        os.write(fd, struct.pack('llHHi', 0, 0, EV_SYN, SYN_REPORT, 0))

    time.sleep(2.0)  # let udev and the game find it
    for step in args:
        name, _, value = step.partition('=')
        if name in BUTTONS:
            emit(EV_KEY, BUTTONS[name], 1)
            time.sleep(0.12)
            emit(EV_KEY, BUTTONS[name], 0)
        elif name in AXES:
            code = AXES[name][0]
            emit(EV_ABS, code, int(value))
            time.sleep(0.2)
            emit(EV_ABS, code, 0)
        else:
            raise SystemExit(f'gamepad.py: no such button or axis: {step}')
        time.sleep(0.4)
    time.sleep(hold)
    fcntl.ioctl(fd, UI_DEV_DESTROY)
    os.close(fd)


if __name__ == '__main__':
    main(sys.argv[1:])
