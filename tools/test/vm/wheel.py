# wheel.py [h]V120...: a uinput mouse with a hi-res wheel (as root in the VM) turning by each amount 0.3 s apart, in
# 1/120 notch, > 0 down; "h" = horizontal, > 0 right. The QEMU mice only send whole notches
import fcntl
import os
import struct
import sys
import time

EV_SYN, EV_KEY, EV_REL = 0, 1, 2
SYN_REPORT = 0
REL_X, REL_Y, REL_HWHEEL, REL_WHEEL, REL_WHEEL_HI_RES, REL_HWHEEL_HI_RES = 0, 1, 6, 8, 11, 12
BTN_LEFT, BTN_RIGHT, BTN_MIDDLE = 0x110, 0x111, 0x112
UI_SET_EVBIT, UI_SET_KEYBIT, UI_SET_RELBIT = 0x40045564, 0x40045565, 0x40045566
UI_DEV_SETUP, UI_DEV_CREATE, UI_DEV_DESTROY = 0x405C5503, 0x5501, 0x5502
BUS_USB = 0x03


def main(steps):
    fd = os.open('/dev/uinput', os.O_WRONLY | os.O_NONBLOCK)
    for ev in (EV_KEY, EV_REL):
        fcntl.ioctl(fd, UI_SET_EVBIT, ev)
    for key in (BTN_LEFT, BTN_RIGHT, BTN_MIDDLE):  # buttons make libinput see a pointer
        fcntl.ioctl(fd, UI_SET_KEYBIT, key)
    for rel in (REL_X, REL_Y, REL_WHEEL, REL_WHEEL_HI_RES, REL_HWHEEL, REL_HWHEEL_HI_RES):
        fcntl.ioctl(fd, UI_SET_RELBIT, rel)
    fcntl.ioctl(fd, UI_DEV_SETUP, struct.pack('HHHH80sI', BUS_USB, 0x1d6b, 0x0104, 1, b'hyprwalk test wheel', 0))
    fcntl.ioctl(fd, UI_DEV_CREATE)

    def emit(kind, code, value):
        os.write(fd, struct.pack('llHHi', 0, 0, kind, code, value))

    time.sleep(1.5)  # let libinput and Hyprland add it
    acc = {False: 0, True: 0}
    for step in steps:
        horiz = step.startswith('h')
        v = int(step.lstrip('h'))
        # kernel wheel is > 0 up, horizontal > 0 right
        emit(EV_REL, REL_HWHEEL_HI_RES if horiz else REL_WHEEL_HI_RES, v if horiz else -v)
        acc[horiz] += v
        whole = int(acc[horiz] / 120)
        if whole:
            emit(EV_REL, REL_HWHEEL if horiz else REL_WHEEL, whole if horiz else -whole)
            acc[horiz] -= whole * 120
        emit(EV_SYN, SYN_REPORT, 0)
        time.sleep(0.3)
    time.sleep(0.5)
    fcntl.ioctl(fd, UI_DEV_DESTROY)
    os.close(fd)


if __name__ == '__main__':
    main(sys.argv[1:])
