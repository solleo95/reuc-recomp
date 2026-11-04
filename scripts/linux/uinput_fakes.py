"""Virtual input devices through /dev/uinput, to look at what the evdev layer (game/evdev_probe.cpp, uc_evdev_probe) makes of a mouse, a light gun and a Wii Remote without the hardware.

  python3 uinput_fakes.py <seconds>     create the devices, play a short script on them, wait until <seconds> have passed, remove them
  python3 uinput_fakes.py --help

Run it on the Linux machine (the Steam Deck: `ssh deck python3 - 12 < scripts/linux/uinput_fakes.py`) while uc_evdev_probe runs in another terminal (`uc_evdev_probe pointers 10 grab`,
`uc_evdev_probe remote 10`). Needs write access to /dev/uinput (root, or a udev rule). Pure standard library, no python-evdev. Nothing is left behind: the devices go away with the process.

What the script does, a second after the devices appear:
  mouse   moves (12, 5), clicks the left button, turns the wheel one notch towards the user, clicks the right button
  gun     points at the middle (0.5, 0.5), fires, points at the top left corner (0, 0), fires, points at the right edge (1, 0.5)
  remote  presses A and 1, tilts the accelerometer, shows two infrared dots (500,300) and (700,310), releases the buttons, hides the dots
"""
import fcntl
import os
import struct
import sys
import time

EV_SYN, EV_KEY, EV_REL, EV_ABS = 0, 1, 2, 3
REL_X, REL_Y, REL_WHEEL = 0, 1, 8
ABS_X, ABS_Y, ABS_RX, ABS_RY, ABS_RZ, ABS_HAT0X = 0, 1, 3, 4, 5, 0x10
BTN_LEFT, BTN_RIGHT = 0x110, 0x111
BTN_1, BTN_A, BTN_MODE = 0x101, 0x130, 0x13c
KEY_LEFT, KEY_RIGHT, KEY_UP, KEY_DOWN, KEY_NEXT, KEY_PREVIOUS, BTN_2, BTN_B = 105, 106, 103, 108, 407, 412, 0x102, 0x131

UI_SET_EVBIT, UI_SET_KEYBIT, UI_SET_RELBIT, UI_SET_ABSBIT = 0x40045564, 0x40045565, 0x40045566, 0x40045567
UI_SET_PHYS, UI_DEV_SETUP, UI_ABS_SETUP, UI_DEV_CREATE, UI_DEV_DESTROY = 0x4008556C, 0x405C5503, 0x401C5504, 0x5501, 0x5502


class Device:
    def __init__(self, name, phys, keys=(), rels=(), abss=()):
        """abss: (code, minimum, maximum) triples."""
        self.fd = os.open("/dev/uinput", os.O_WRONLY | os.O_NONBLOCK)
        for kind, bit in ((EV_KEY, keys), (EV_REL, rels), (EV_ABS, [a[0] for a in abss])):
            if bit:
                fcntl.ioctl(self.fd, UI_SET_EVBIT, kind)
        for code in keys:
            fcntl.ioctl(self.fd, UI_SET_KEYBIT, code)
        for code in rels:
            fcntl.ioctl(self.fd, UI_SET_RELBIT, code)
        for code, low, high in abss:
            fcntl.ioctl(self.fd, UI_SET_ABSBIT, code)
            fcntl.ioctl(self.fd, UI_ABS_SETUP, struct.pack("<Hxx6i", code, 0, low, high, 0, 0, 0))   # struct uinput_abs_setup: the code, then input_absinfo (value, minimum, maximum, fuzz, flat, resolution)
        fcntl.ioctl(self.fd, UI_SET_PHYS, phys.encode() + b"\0")
        fcntl.ioctl(self.fd, UI_DEV_SETUP, struct.pack("<HHHH80sI", 0x03, 0x1234, 0x5678, 1, name.encode(), 0))
        fcntl.ioctl(self.fd, UI_DEV_CREATE)

    def event(self, kind, code, value):
        os.write(self.fd, struct.pack("<qqHHi", 0, 0, kind, code, value))

    def sync(self):
        self.event(EV_SYN, 0, 0)

    def key(self, code, down):
        self.event(EV_KEY, code, 1 if down else 0)
        self.sync()

    def close(self):
        fcntl.ioctl(self.fd, UI_DEV_DESTROY)
        os.close(self.fd)


def main():
    if len(sys.argv) != 2 or sys.argv[1] in ("-h", "--help"):
        print(__doc__)
        return 0 if len(sys.argv) == 2 else 2
    total = float(sys.argv[1])
    mouse = Device("UC Fake Mouse", "ucfake-mouse", keys=(BTN_LEFT, BTN_RIGHT), rels=(REL_X, REL_Y, REL_WHEEL))
    gun = Device("UC Fake Gun", "ucfake-gun", keys=(BTN_LEFT,), abss=((ABS_X, 0, 32767), (ABS_Y, 0, 32767)))
    remote_keys = (KEY_LEFT, KEY_RIGHT, KEY_UP, KEY_DOWN, KEY_NEXT, KEY_PREVIOUS, BTN_1, BTN_2, BTN_A, BTN_B, BTN_MODE)
    wii = Device("Nintendo Wii Remote", "ucfake-wii0", keys=remote_keys)
    accel = Device("Nintendo Wii Remote Accelerometer", "ucfake-wii0", abss=tuple((code, -500, 500) for code in (ABS_RX, ABS_RY, ABS_RZ)))
    ir = Device("Nintendo Wii Remote IR", "ucfake-wii0", abss=tuple((ABS_HAT0X + i, 0, 1023 if i % 2 == 0 else 767) for i in range(8)))
    devices = (mouse, gun, wii, accel, ir)
    started = time.time()
    try:
        time.sleep(3.5)   # udev and sysfs need a moment, and the game looks for new devices every two seconds
        mouse.event(EV_REL, REL_X, 12); mouse.event(EV_REL, REL_Y, 5); mouse.sync()
        mouse.key(BTN_LEFT, True); mouse.key(BTN_LEFT, False)
        mouse.event(EV_REL, REL_WHEEL, -1); mouse.sync()
        mouse.key(BTN_RIGHT, True); mouse.key(BTN_RIGHT, False)
        time.sleep(0.3)
        for x, y, fire in ((16384, 16384, True), (0, 0, True), (32767, 16384, False)):
            gun.event(EV_ABS, ABS_X, x); gun.event(EV_ABS, ABS_Y, y); gun.sync()
            if fire:
                gun.key(BTN_LEFT, True); gun.key(BTN_LEFT, False)
            time.sleep(0.2)
        wii.key(BTN_A, True); wii.key(BTN_1, True)
        accel.event(EV_ABS, ABS_RX, 40); accel.event(EV_ABS, ABS_RY, -40); accel.event(EV_ABS, ABS_RZ, 104); accel.sync()
        for dot, (x, y) in enumerate(((500, 300), (700, 310))):
            ir.event(EV_ABS, ABS_HAT0X + 2 * dot, x); ir.event(EV_ABS, ABS_HAT0X + 2 * dot + 1, y)
        ir.sync()
        time.sleep(0.5)
        wii.key(BTN_A, False); wii.key(BTN_1, False)
        for dot in range(2):
            ir.event(EV_ABS, ABS_HAT0X + 2 * dot, 1023); ir.event(EV_ABS, ABS_HAT0X + 2 * dot + 1, 1023)   # the kernel's "dot not seen"
        ir.sync()
        time.sleep(max(0.0, total - (time.time() - started)))
    finally:
        for device in devices:
            device.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
