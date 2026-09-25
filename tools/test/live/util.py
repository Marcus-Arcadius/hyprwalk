# util.py: what tools/test/live/check.sh needs that bash doesn't do (plain python3, no numpy):
#   util.py png IN.ppm OUT.png            a grim PPM as a PNG
#   util.py diff A.ppm B.ppm [OUT.png]    the fraction of pixels that differ (any channel by more than 32), and a
#                                         picture of where: red where they differ, the rest dimmed
#   util.py count IN.ppm COLOUR X0 Y0 X1 Y1  how many pixels in the box are that colour (orange: a notification's
#                                         bar; red: the lip sync badge's dot); fractions of the width and height
#   util.py json PATH < JSON               a value out of hyprctl's JSON: "mode", "aimed.kind", "visemes.aa", "0.name"
import json
import re
import struct
import sys
import zlib

COLOURS = {
    'orange': lambda r, g, b: r > 200 and 100 < g < 180 and b < 80,
    'red': lambda r, g, b: r > 170 and g < 90 and b < 90,
}


def read_ppm(path):
    data = open(path, 'rb').read()
    m = re.match(rb'P6\s+(\d+)\s+(\d+)\s+(\d+)\s', data)
    if not m:
        raise SystemExit(f'{path}: not a binary PPM')
    return int(m.group(1)), int(m.group(2)), memoryview(data)[m.end():]


def write_png(path, w, h, rgb):
    rows = b''.join(b'\0' + bytes(rgb[y * w * 3:(y + 1) * w * 3]) for y in range(h))

    def chunk(kind, body):
        return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body) & 0xffffffff)
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                chunk(b'IDAT', zlib.compress(rows, 6)) + chunk(b'IEND', b''))


def diff(a, b, out=None, step=2, thresh=32):
    wa, ha, pa = read_ppm(a)
    wb, hb, pb = read_ppm(b)
    if (wa, ha) != (wb, hb):
        print(f'{a} and {b} differ in size: {wa}x{ha} and {wb}x{hb}', file=sys.stderr)
        print('1.00000')
        return
    n = tot = 0
    # the second picture dimmed, and red where it differs
    pic = bytearray(pb).translate(bytes(v // 3 for v in range(256))) if out else None
    for y in range(0, ha, step):
        row = y * wa * 3
        for x in range(0, wa, step):
            i = row + x * 3
            tot += 1
            if abs(pa[i] - pb[i]) > thresh or abs(pa[i + 1] - pb[i + 1]) > thresh or abs(pa[i + 2] - pb[i + 2]) > thresh:
                n += 1
                if pic is not None:
                    for dy in range(min(step, ha - y)):
                        k = ((y + dy) * wa + x) * 3
                        pic[k:k + 3 * min(step, wa - x)] = b'\xff\x00\x00' * min(step, wa - x)
    if pic is not None:
        write_png(out, wa, ha, pic)
    print(f'{n / max(tot, 1):.5f}')


def count(path, colour, box):
    w, h, p = read_ppm(path)
    pred = COLOURS[colour]
    x0, y0, x1, y1 = int(box[0] * w), int(box[1] * h), int(box[2] * w), int(box[3] * h)
    n = 0
    for y in range(max(0, y0), min(h, y1)):
        row = y * w * 3
        for x in range(max(0, x0), min(w, x1)):
            i = row + x * 3
            if pred(p[i], p[i + 1], p[i + 2]):
                n += 1
    print(n)


def value(path):
    v = json.load(sys.stdin)
    for part in path.split('.') if path else []:
        v = v[int(part)] if isinstance(v, list) else v.get(part)
        if v is None:
            break
    print(json.dumps(v) if isinstance(v, (dict, list)) else ('null' if v is None else str(v).lower() if isinstance(v, bool) else v))


if __name__ == '__main__':
    cmd, args = sys.argv[1], sys.argv[2:]
    if cmd == 'png':
        w, h, p = read_ppm(args[0])
        write_png(args[1], w, h, p)
    elif cmd == 'diff':
        diff(args[0], args[1], args[2] if len(args) > 2 else None)
    elif cmd == 'count':
        count(args[0], args[1], [float(a) for a in args[2:6]])
    elif cmd == 'json':
        value(args[0] if args else '')
    else:
        raise SystemExit(f'unknown command {cmd}')
