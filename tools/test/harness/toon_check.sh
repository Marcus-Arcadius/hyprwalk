#!/usr/bin/env bash
# toon_check.sh: toon shading and matcaps (MToon's, and unity2hyprwalk's "hyprwalk_toon" and "hyprwalk_matcap" extras) on
# toonballs.py's six balls, side on to the sun so N·L goes from 1 to -1 across each: a plain ball falls off with N·L,
# a toon ball is flat either side of a sharp step, a matcap brightens where white, toon in shadow is all shade.
#
#   tools/test/harness/toon_check.sh [DIR]   (DIR: where the ball file and the frames go; a temporary one by default)
#
# Needs build/test/shot (tools/test/harness/build.sh).
set -uo pipefail
REPO="$(cd "$(dirname "$0")/../../.." && pwd)"
SHOT="$REPO/build/test/shot"
[[ -x "$SHOT" ]] || { echo "no $SHOT: tools/test/harness/build.sh builds it" >&2; exit 1; }
DIR="${1:-}"
[[ -n "$DIR" ]] || { DIR="$(mktemp -d)"; trap 'rm -rf "$DIR"' EXIT; }
mkdir -p "$DIR"
python3 "$REPO/tools/test/harness/toonballs.py" "$DIR" > /dev/null || exit 1

# a camera looking along (0.74, 0, 0.67) sees the sun (world.hpp's sunDirection()) side on; yaw -47.6° puts it in front
# of the row, whose +x is then (-0.674, 0, 0.739) (VRM 0.x is turned to face +Z as VRM 1.0, so the plain ball is on the
# right). Each ball is centred in its own 200x200 frame (N·L 0 through its middle): 40° high, 1.6 m away, 172 px a metre
shots() { # PREFIX OPTIONS...: a frame per ball
    local out="$1" args=() k=0
    shift
    for x in -1.0 -0.6 -0.2 0.2 0.6 1.0; do
        args+=(--shift "$(awk -v x=$x 'BEGIN {print -x * 0.674}')" 0 "$(awk -v x=$x 'BEGIN {print x * 0.739}')" --out "$out$k.png")
        k=$((k + 1))
    done
    "$SHOT" --avatar "$DIR/ToonBalls.glb" --yaw -47.6 --view 0 --pitch 0 --dist 1.6 --fov 40 --target 1.2 --size 200x200 \
        --frames 2 "$@" "${args[@]}" > "$out.log" 2>&1 || { echo "FAIL the harness: $(tail -3 "$out.log")"; exit 1; }
}
shots "$DIR/sun"
# in the shadow of the courtyard's south wall (5 m high at z 22; at 1.2 m up it shades 2.1 m of the yard)
shots "$DIR/shade" --pos 0 0 20.8

python3 - "$DIR" << 'EOF'
import math, struct, sys, zlib


def png(path):
    """(width, height, rows of RGB bytes) of an 8 bit RGB or RGBA PNG"""
    data = open(path, 'rb').read()
    pos, idat, w = 8, b'', 0
    while pos < len(data):
        n, kind = struct.unpack('>I4s', data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if kind == b'IHDR':
            w, h, depth, ctype = struct.unpack('>IIBB', body[:10])
            assert depth == 8 and ctype in (2, 6), 'an 8 bit RGB(A) PNG'
            bpp = 3 if ctype == 2 else 4
        elif kind == b'IDAT':
            idat += body
        pos += 12 + n
    raw, stride, rows, prev = zlib.decompress(idat), w * bpp, [], bytearray(w * bpp)
    for y in range(h):
        f, line = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b, c = prev[i], prev[i - bpp] if i >= bpp else 0
            if f == 1:
                line[i] = (line[i] + a) & 255
            elif f == 2:
                line[i] = (line[i] + b) & 255
            elif f == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append([tuple(line[x * bpp:x * bpp + 3]) for x in range(w)])
        prev = line
    return w, h, rows


W = H = 200
F = (H / 2) / math.tan(math.radians(20)) / 1.6  # px a metre at the balls
L = (0.623, -0.782)                             # towards the sun on the screen (y down)
BALLS = ['Plain', 'MToon', 'Toon', 'Matcap', 'PlainMatcap', 'MToon0']
fails = 0


def at(frames, ball, s):
    """a ball's color s of its radius from its middle towards the sun (3x3 mean)"""
    rows = frames[BALLS.index(ball)][2]
    x, y = round(W / 2 + s * 0.15 * F * L[0]), round(H / 2 + s * 0.15 * F * L[1])
    px = [rows[y + j][x + i] for j in (-1, 0, 1) for i in (-1, 0, 1)]
    return tuple(sum(p[c] for p in px) / 9 for c in range(3))


def luma(c):
    return 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2]


def check(what, ok, got):
    global fails
    print(('ok   ' if ok else 'FAIL ') + what + f' ({got})')
    fails += not ok


sun = [png(f'{sys.argv[1]}/sun{k}.png') for k in range(len(BALLS))]
shade = [png(f'{sys.argv[1]}/shade{k}.png') for k in range(len(BALLS))]
p = {b: [luma(at(sun, b, s)) for s in (0.75, 0.35, -0.35, -0.75)] for b in BALLS}
fmt = lambda v: ' '.join(f'{x:.0f}' for x in v)
check('the plain ball: its light falls off with N·L', p['Plain'][0] - p['Plain'][1] > 10 and p['Plain'][1] - p['Plain'][2] > 10, fmt(p['Plain']))
for b in ('MToon', 'Toon', 'MToon0'):
    v = p[b]
    check(f'{b}: flat on the lit side', abs(v[0] - v[1]) < 8, fmt(v))
    check(f'{b}: flat on the shade side', abs(v[2] - v[3]) < 8, fmt(v))
    check(f'{b}: a step between them', v[1] - v[2] > 25, fmt(v))
m, t = at(sun, 'MToon', -0.55), at(sun, 'Toon', -0.55)
check("MToon's shade: its own color (0.1, 0.03, 0.01), redder than the base's", m[0] / max(m[2], 1) > 1.3 * t[0] / max(t[2], 1),
      f'{fmt(m)} vs {fmt(t)}')
m = at(sun, 'MToon0', -0.55)
check("VRM 0.x MToon's shade: its _ShadeColor (0.1, 0.4, 0.1), greener than the base's", m[1] / max(m[0], 1) > 1.3 * t[1] / max(t[0], 1),
      f'{fmt(m)} vs {fmt(t)}')
for b, base in (('Matcap', 'Toon'), ('PlainMatcap', 'Plain')):
    a, c = luma(at(sun, b, 0)), luma(at(sun, base, 0))
    check(f'{b}: the matcap brightens its middle', a > c + 25, f'{a:.0f} vs {c:.0f}')
q = [luma(at(shade, 'Toon', s)) for s in (0.75, -0.75)]
check("in the wall's shadow, the toon ball is shade all round, as its shade side is in the sun",
      abs(q[0] - q[1]) < 8 and abs(q[0] - p['Toon'][3]) < 12, f'{fmt(q)}; in the sun {fmt(p["Toon"])}')
sys.exit(1 if fails else 0)
EOF
FAILS=$?
[[ $FAILS == 0 ]] && echo "all toon checks passed" || echo "toon checks failed"
exit $FAILS
