#!/usr/bin/env bash
# cs2mat_check.sh: CS2 material details as tools/cs2map.py writes them (HYPR3D_materials_source2's tintMask, decal,
# texture2, blendMode "add" and fog false), drawn by the harness on cs2mats.py's panels: tint masks, decals on the
# second uv set, unlit and additive blends, vertex paint, mod2x and fog (cs2mats.py has the layout).
#
#   tools/test/harness/cs2mat_check.sh [DIR]   (DIR: where the map and the frame go; a temporary one by default)
#
# Needs build/test/shot (tools/test/harness/build.sh).
set -uo pipefail
REPO="$(cd "$(dirname "$0")/../../.." && pwd)"
SHOT="$REPO/build/test/shot"
[[ -x "$SHOT" ]] || { echo "no $SHOT: tools/test/harness/build.sh builds it" >&2; exit 1; }
DIR="${1:-}"
[[ -n "$DIR" ]] || { DIR="$(mktemp -d)"; trap 'rm -rf "$DIR"' EXIT; }
mkdir -p "$DIR"
python3 "$REPO/tools/test/harness/cs2mats.py" "$DIR" > /dev/null || exit 1
# CS2's point_camera view (fov 90 at 4:3: 73.74 degrees high), 1.5 m up, looking north at the wall
"$SHOT" --map "$DIR/Cs2Mats.glb" --size 1280x720 --fov 73.74 --autoexp 1 --eye 0 1.5 0 0 0 --out "$DIR/view.png" > "$DIR/view.log" 2>&1 ||
    { echo "FAIL the harness: $(tail -3 "$DIR/view.log")"; exit 1; }

python3 - "$DIR/view.png" << 'EOF'
import struct, sys, zlib


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


W, H, rows = png(sys.argv[1])
fails = 0


def mean(u0, u1, v0=0.45, v1=0.55):
    """the mean color of a box, in fractions of the frame"""
    px = [rows[y][x] for y in range(int(v0 * H), int(v1 * H)) for x in range(int(u0 * W), int(u1 * W))]
    return tuple(sum(p[c] for p in px) / len(px) for c in range(3))


def half(x0, x1, side):
    """the middle of a panel's half, from its left and right edge in metres (4.4 m out: u = 0.5 + x / 11.73)"""
    xm = (x0 + x1) / 2
    a, b = (x0, xm) if side == 0 else (xm, x1)
    q = (b - a) / 4
    return 0.5 + (a + q) / 11.733, 0.5 + (b - q) / 11.733


def luma(c):
    return 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2]


def check(what, ok, got):
    global fails
    print(('ok   ' if ok else 'FAIL ') + what + ' (' + ', '.join(' '.join(f'{x:.0f}' for x in c) for c in got) + ')')
    fails += not ok


PANEL = {'A': (-2.7, -1.5), 'B': (-1.3, -0.1), 'C': (0.1, 1.3), 'D': (1.5, 2.7)}
l, r = (mean(*half(*PANEL['A'], 0)), mean(*half(*PANEL['A'], 1)))
check('the tint mask: the tint (red) on its left half', l[0] > l[1] + 60 and l[0] > l[2] + 60, (l,))
check('and none on its right half', abs(r[0] - r[1]) < 20 and abs(r[1] - r[2]) < 20 and r[1] > l[1] + 60, (r,))
l, r = (mean(*half(*PANEL['B'], 0)), mean(*half(*PANEL['B'], 1)))
check('a decal multiplied on the second uv set (it runs the other way): dark on the right', luma(r) < 0.5 * luma(l), (l, r))
l, r = (mean(*half(*PANEL['C'], 0)), mean(*half(*PANEL['C'], 1)))
check('a decal mixed in by its alpha: blue on the left', l[2] > l[0] + 60, (l,))
check('and nothing where its alpha is 0', abs(r[0] - r[2]) < 20 and luma(r) > luma(l) + 40, (r,))
l, r = (mean(*half(*PANEL['D'], 0)), mean(*half(*PANEL['D'], 1)))
check('unlit, times its second color texture: green on the left', l[1] > l[0] + 60 and l[1] > l[2] + 60, (l,))
check('and white on the right', min(r) > 150 and abs(r[0] - r[1]) < 20, (r,))
added, wall = mean(0.5 + 3.2 / 11.733, 0.5 + 4.0 / 11.733), mean(0.5 + 4.4 / 11.733, 0.5 + 5.4 / 11.733)
check('unlit and added: brighter than the wall beside it', all(a > b + 40 for a, b in zip(added, wall)), (added, wall))
far_off, far_on = mean(0.55, 0.67, 0.10, 0.18), mean(0.425, 0.465, 0.10, 0.18)  # (G, beside L)
check('60 m out, past the fog, with its fog off: red', far_off[0] > far_off[1] + 80 and far_off[0] > far_off[2] + 80, (far_off,))
check('beside it, with its fog on: all fog (the sky, grey), not red', abs(far_on[0] - far_on[1]) < 25 and abs(far_on[1] - far_on[2]) < 25,
      (far_on,))
ROW2 = (0.27, 0.34)  # the second row, 2.3 to 3.3 m up
l, r = (mean(*half(-2.7, -1.5, 0), *ROW2), mean(*half(-2.7, -1.5, 1), *ROW2))
check('vertex paint in the tint: blue where the tint mask is', l[2] > l[0] + 60, (l,))
check('and none where it is not', abs(r[0] - r[2]) < 20 and r[0] > l[0] + 60, (r,))
i = mean(*half(-1.3, -0.1, 0), *ROW2)
check('all-0 paint is none (not black)', abs(i[0] - i[2]) < 20 and luma(i) > luma(l) + 30, (i,))
jl, jr, wall2 = mean(*half(0.1, 1.3, 0), *ROW2), mean(*half(0.1, 1.3, 1), *ROW2), mean(0.5 + 4.4 / 11.733, 0.5 + 5.4 / 11.733, *ROW2)
check('unlit mod2x in linear light: linear 0.5 leaves the wall as it is', abs(luma(jl) - luma(wall2)) < 12, (jl, wall2))
check('and sRGB 128 darkens it', luma(jr) < 0.8 * luma(wall2), (jr, wall2))
lf, behind = mean(0.35, 0.40, 0.10, 0.17), mean(0.425, 0.465, 0.10, 0.17)
check("past the fog: unlitgeneric's added light fades out (the fogged quad behind it, beside it)",
      all(abs(a - b) < 8 for a, b in zip(lf, behind)), (lf, behind))
sys.exit(1 if fails else 0)
EOF
FAILS=$?
[[ $FAILS == 0 ]] && echo "all cs2 material checks passed" || echo "cs2 material checks failed"
exit $FAILS
