#!/usr/bin/env bash
# fp_check.sh: first person with the avatar's body (the harness's --fpbody, the plugin's code): the camera in its eyes,
# nothing of its head in the view, its hands up low in it (ready); still, standing; walking they stay in the view,
# running they pump; looking far down they let go; touching (a window pressed) the right hand's wrist goes in toward the
# crosshair; typing they come nearer together, holding up and out; let down, gone; a gesture is held up where it shows;
# a wall just ahead keeps them back; out of first person (V) they go down to the sides from where they were, smoothly,
# its camera gone (as the plugin's motion has none then). From the harness's --fpstatus and --wrists lines (where each
# wrist is in the view, 0..1 across and down; from the feet) and two frames.
#   tools/test/harness/fp_check.sh AVATAR [DIR]   (a humanoid: a VRM, or regress.sh --keep's OUT/new/BoothAccessories.glb;
#                                                  DIR: where the frames and logs go, a temporary one by default)
# Needs build/test/shot (tools/test/harness/build.sh).
set -uo pipefail
REPO="$(cd "$(dirname "$0")/../../.." && pwd)"
SHOT="$REPO/build/test/shot"
[[ -x "$SHOT" ]] || { echo "no $SHOT: tools/test/harness/build.sh builds it" >&2; exit 1; }
AVATAR="${1:-}"
[[ -f "$AVATAR" ]] || { echo "usage: fp_check.sh AVATAR [DIR]" >&2; exit 2; }
DIR="${2:-}"
[[ -n "$DIR" ]] || { DIR="$(mktemp -d)"; trap 'rm -rf "$DIR"' EXIT; }
mkdir -p "$DIR"

run() { # name, then the harness's options (after the avatar, 640x360 as a 16:10-ish monitor's view)
    local name="$1"
    shift
    "$SHOT" --size 640x360 --avatar "$AVATAR" "$@" > "$DIR/$name.log" 2>&1 || { echo "FAIL the harness ($name): $(tail -3 "$DIR/$name.log")"; exit 1; }
}
st=(--fpstatus)
run ready --fpbody 0 0 --frames 60 "${st[@]}" --frames 120 "${st[@]}" --out "$DIR/ready.png" --hide-avatar "" --out "$DIR/no-avatar.png"
walk=() run=()
for i in $(seq 12); do walk+=(--frames 5 "${st[@]}"); run+=(--frames 3 "${st[@]}"); done
run walk --fpbody 0 0 --frames 30 --accel 10 --decel 14 --move 0 -1.6 --frames 60 "${walk[@]}"
run run --fpbody 0 0 --frames 30 --accel 10 --decel 14 --run 1 --move 0 -4.5 --frames 60 "${run[@]}"
run down --fpbody 0 -80 --frames 60 "${st[@]}" --fpbody 0 0 --frames 60 "${st[@]}"
fade=()
for i in $(seq 20); do fade+=(--frames 1 --wrists); done
# (the world's middle behind it: going by a camera that's gone, the arms would reach back there)
run fade --pos 0 0 -6 --fpbody 0 0 --frames 60 --fphands touch --fppress --frames 30 --wrists --fpoff "${fade[@]}" --frames 60 --wrists
run modes --fpbody 0 0 --frames 60 --fphands touch --fppress --frames 40 "${st[@]}" --fphands type --frames 40 "${st[@]}" --fptap left --frames 2 \
    --fphands hold --frames 40 "${st[@]}" --fphands down --frames 40 "${st[@]}" --fphands ready --frames 40 --gesture right thumbsup --frames 40 "${st[@]}" \
    --gesture right neutral --fproom 0.25 --frames 40 "${st[@]}"

python3 - "$DIR" << 'EOF'
import math, re, struct, sys, zlib

D = sys.argv[1]
FP = re.compile(r'fp: on (\d) weight ([\d.]+) arms ([\d.]+) eye ([-\d.]+) ([-\d.]+) ([-\d.]+) \(([-\d.]+) up, eyes ([-\d.]+) ([-\d.]+) ([-\d.]+)\)')
HAND = re.compile(r'(left|right)(wrist|elbow) ([-\d.na]+) ([-\d.na]+) \(([\d.]+) m\)')


def status(name):
    out = []
    for line in open(f'{D}/{name}.log', encoding='utf-8', errors='replace'):
        m = FP.search(line)
        if not m:
            continue
        g = m.groups()
        s = {'on': g[0] == '1', 'weight': float(g[1]), 'arms': float(g[2]), 'eye': tuple(map(float, g[3:6])), 'up': float(g[6]), 'eyes': tuple(map(float, g[7:10]))}
        for side, part, x, y, d in HAND.findall(line):
            s[side + part] = (float(x), float(y), float(d))
        out.append(s)
    return out


def png(path):
    """(width, height, rows of RGB bytes) of the harness's PNGs (8 bit RGB, stored)"""
    data = open(path, 'rb').read()
    pos, idat = 8, b''
    while pos < len(data):
        n, kind = struct.unpack('>I4s', data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if kind == b'IHDR':
            w, h = struct.unpack('>II', body[:8])
        elif kind == b'IDAT':
            idat += body
        pos += 12 + n
    raw = zlib.decompress(idat)
    return w, h, [raw[y * (w * 3 + 1) + 1:(y + 1) * (w * 3 + 1)] for y in range(h)]


fails = 0


def check(what, ok, detail=''):
    global fails
    print(f"{'ok  ' if ok else 'FAIL'} {what}" + (f'  [{detail}]' if detail else ''))
    fails += 0 if ok else 1


def view(s, side):
    return s.get(side + 'wrist', (math.nan, math.nan, math.nan))


def in_view(p, below=0.0):
    return 0 <= p[0] <= 1 and below <= p[1] <= 1


log = open(f'{D}/ready.log', encoding='utf-8', errors='replace').read()
m = re.search(r'first person: the eyes ([\d.]+) m up, ([-\d.]+) cm ahead of the head.s joint \(([^)]*)\); (\d+) of (\d+) triangles', log)
check('the loader found its eyes and its head', m and 0.8 < float(m.group(1)) < 2.2 and int(m.group(4)) > 0, m and m.group(0))
r = status('ready')
s, s2 = r[0], r[-1]
L, R = view(s, 'left'), view(s, 'right')
check('the camera in its eyes (the eyes as drawn, within 2 cm)', s['on'] and math.dist(s['eye'], s['eyes']) < 0.02, f"eye {s['eye']}, eyes {s['eyes']}")
check('ready: both hands up in the bottom half of the view, the left left of the middle and the right right of it',
      s['arms'] > 0.99 and in_view(L, 0.55) and in_view(R, 0.55) and L[0] < 0.5 < R[0], f'left {L[:2]}, right {R[:2]}')
check('standing: the camera stays still (2 s more: within 1 mm)', math.dist(s['eye'], s2['eye']) < 0.001, f"{math.dist(s['eye'], s2['eye']) * 1000:.2f} mm")
w, h, a = png(f'{D}/ready.png')
_, _, b = png(f'{D}/no-avatar.png')
top = sum(1 for y in range(h // 2) for x in range(w) if max(abs(a[y][3 * x + c] - b[y][3 * x + c]) for c in range(3)) > 24)
bottom = sum(1 for y in range(h // 2, h) for x in range(w) if max(abs(a[y][3 * x + c] - b[y][3 * x + c]) for c in range(3)) > 24)
check('nothing of the head (or the hair) in the view: its top half as with no avatar; the hands in the bottom half', top < 0.002 * w * h and bottom > 0.02 * w * h,
      f'{top} px in the top half differ, {bottom} in the bottom')

walk = status('walk')
check('walking: the hands stay up in the view all the way', walk and all(in_view(view(x, 'left'), 0.45) and in_view(view(x, 'right'), 0.45) for x in walk),
      [tuple(round(v, 2) for v in view(x, 'right')[:2]) for x in walk[:6]])
ys = [view(x, side)[1] for x in status('run') for side in ('left', 'right')]
ys = [y for y in ys if not math.isnan(y)]
check('running: the hands pump (up into the view and down: their height goes over a sixth of it)', ys and max(ys) - min(ys) > 0.16 and min(ys) < 0.9, f'{min(ys):.2f}..{max(ys):.2f}')

d = status('down')
check('looking far down (80°): they let go; level again: back up', d[0]['arms'] < 0.02 and d[1]['arms'] > 0.98, f"arms {d[0]['arms']}, {d[1]['arms']}")

touch, typing, hold, down, gesture, wall = status('modes')
T = view(touch, 'right')
check('touching: the right hand up toward the crosshair (its wrist right of the middle, a little under it, well above where it was ready)',
      0.5 < T[0] < 0.8 and 0.5 < T[1] < 0.88 and T[1] < R[1] - 0.15, f'{T[:2]} (ready {R[:2]})')
gap = view(typing, 'right')[0] - view(typing, 'left')[0]
check('typing: the hands nearer together', in_view(view(typing, 'left')) and in_view(view(typing, 'right')) and gap < R[0] - L[0] - 0.08, f'{gap:.2f} apart (ready {R[0] - L[0]:.2f})')
check('holding: both up and out, higher than ready', view(hold, 'left')[1] < L[1] - 0.08 and view(hold, 'right')[1] < R[1] - 0.08, f"{view(hold, 'left')[:2]} {view(hold, 'right')[:2]}")
check('let down: the arms the animation\'s', down['arms'] < 0.02, down['arms'])
check('a gesture (thumbs up): that hand held up where it shows', view(gesture, 'right')[1] < R[1] - 0.05, f"{view(gesture, 'right')[:2]} (ready {R[:2]})")
check('a wall 25 cm ahead: the hands kept back from it, lower (out of the view, or nearly)',
      all(view(wall, side)[2] < ready[2] - 0.02 and view(wall, side)[1] > ready[1] + 0.1 for side, ready in (('left', L), ('right', R))),
      f"{view(wall, 'left')} {view(wall, 'right')} (ready {L} {R})")
ws = [tuple(map(float, l.split()[1:7])) for l in open(f'{D}/fade.log', encoding='utf-8', errors='replace') if l.startswith('wrists:')]


def off_line(p, a, b):
    ab = [b[i] - a[i] for i in range(3)]
    t = max(0.0, min(1.0, sum((p[i] - a[i]) * ab[i] for i in range(3)) / max(sum(c * c for c in ab), 1e-9)))
    return math.dist(p, [a[i] + ab[i] * t for i in range(3)])


worst = max((off_line(w[3 * s:3 * s + 3], ws[0][3 * s:3 * s + 3], ws[-1][3 * s:3 * s + 3]) for w in ws for s in (0, 1)), default=9)
step = max((math.dist(a[3 * s:3 * s + 3], b[3 * s:3 * s + 3]) for a, b in zip(ws, ws[1:]) for s in (0, 1)), default=9)
ahead = max((w[3 * s + 2] for w in ws for s in (0, 1)), default=9)
check('out of first person: the hands down to the sides from where they were, smoothly (each wrist swinging down near the line from there to there, no frame a jump, never behind the body)',
      len(ws) > 20 and worst < 0.2 and step < 0.1 and ahead < 0.08, f'{len(ws)} frames, {worst * 100:.1f} cm off the line at most, steps up to {step * 100:.1f} cm, z up to {ahead:.2f}')
print(f'{fails} failed' if fails else 'all passed')
sys.exit(1 if fails else 0)
EOF
