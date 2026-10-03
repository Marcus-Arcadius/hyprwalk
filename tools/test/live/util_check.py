# util_check.py: util.py diff on made-up frames: a ticking clock, an animated wallpaper and the check's own terminal
# must be left out (--same, --mask); a window that moved or changed colour must still show.
#   python3 tools/test/live/util_check.py [WORKDIR]
import os, subprocess, sys, tempfile

W, H = 640, 400
UTIL = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'util.py')


def frame(path, clock=0, term=0, move=0, blob=0, colour=(200, 60, 60)):
    px = bytearray(W * H * 3)
    for y in range(H):
        for x in range(W):
            px[(y * W + x) * 3:(y * W + x) * 3 + 3] = bytes((x * 255 // W, y * 255 // H, 120))

    def box(x0, y0, w, h, c):
        for y in range(max(0, y0), min(H, y0 + h)):
            for x in range(max(0, x0), min(W, x0 + w)):
                px[(y * W + x) * 3:(y * W + x) * 3 + 3] = bytes(c)
    box(0, 0, W, 20, (30, 30, 30))                       # a bar
    box(560 + clock * 8, 4, 12, 12, (230, 230, 230))     # its clock
    box(40 + move, 60, 200, 150, colour)                 # a window
    box(360, 60, 240, 300, (20, 20, 20))                 # the terminal the check runs in
    box(370, 70 + term * 14, 200, 10, (200, 200, 200))   # its output
    if blob:
        box(20 + blob * 30, 330, 40, 40, (250, 250, 0))  # an animated wallpaper
    with open(path, 'wb') as f:
        f.write(b'P6\n%d %d\n255\n' % (W, H) + px)


def diff(d, a, b, *extra):
    out = subprocess.run(['python3', UTIL, 'diff', f'{d}/{a}.ppm', f'{d}/{b}.ppm', f'{d}/diff-{a}-{b}.png', *extra], capture_output=True, text=True, check=True)
    return [float(v) for v in out.stdout.split()]


d = sys.argv[1] if len(sys.argv) > 1 else tempfile.mkdtemp()
os.makedirs(d, exist_ok=True)
frame(f'{d}/before.ppm')
frame(f'{d}/before-2.ppm', clock=1, blob=1)
frame(f'{d}/after.ppm', clock=2, term=5, blob=2)
frame(f'{d}/after-2.ppm', clock=3, term=6, blob=3)
frame(f'{d}/moved.ppm', clock=2, term=5, blob=2, move=60)
frame(f'{d}/recoloured.ppm', clock=2, term=5, blob=2, colour=(60, 200, 60))
keep = ['--same', f'{d}/before.ppm', f'{d}/before-2.ppm', '--same', f'{d}/after.ppm', f'{d}/after-2.ppm', '--mask', '356', '56', '248', '308']
fails = 0
for what, (a, b, extra), want in [
        ('a live desktop, all compared: the clock, the wallpaper and the terminal show', ('before', 'after', []), lambda f, l: f > 0.01 and l == 0),
        ('... with what changes on its own and the terminal left out: nothing', ('before', 'after', keep), lambda f, l: f == 0 and l > 0.2),
        ('a window moved: shows', ('before', 'moved', keep), lambda f, l: f > 0.05),
        ('a window in another colour: shows', ('before', 'recoloured', keep), lambda f, l: f > 0.05)]:
    f, left = diff(d, a, b, *extra)
    ok = want(f, left)
    fails += not ok
    print(f"{'ok  ' if ok else 'FAIL'} {what}  [{f:.4f} differ, {left:.4f} left out]")
print('all passed' if not fails else f'{fails} FAILED')
sys.exit(1 if fails else 0)
