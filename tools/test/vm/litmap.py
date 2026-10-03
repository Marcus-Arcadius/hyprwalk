# litmap.py OUTDIR: LitCourt.glb, a made-up map with everything tools/cs2map.py writes for a game's own lighting, and
# LitCourtRuntimeSun.glb, the same with a sun lacking a baked shadow channel (cs2map's Dynamic or Stationary sun: realtime
# shadow map only). A 20 m court with 5 m walls, open to the sky: HYPR3D_lighting with two lighting sets (lightmaps,
# probe volumes of different priority), fog, exposure range and tone curve; _LIGHTMAP_UV on world meshes ("node000_..."
# as Source 2 Viewer names them) while probes light the props; HYPR3D_materials_source2 and HYPR3D_materials_blend
# (the floor's red COLOR_0 must not show); a hypr3d_backdrop 340-430 m out, past the far plane (200 m), shown only if
# its own depth range works; BC1/BC3/BC5 textures, EXT_mesh_gpu_instancing, a trigger brush the loader drops, and
# hypr3d_spawn and hypr3d_desktop (north wall, 13 m ahead).
# Correct render from the spawn (shot --size 1280x800 --map LitCourt.glb --spawn --autoexp 1), as frame fractions
# x0 y0 x1 y1 and sRGB on NVIDIA: exposure 0.35 (the range's bottom); floor checker orange .15 .80 .40 .88 (188 155 117)
# and teal .60 .80 .85 .88 (141 160 166); baked shadow band .10 .735 .42 .775 (171 122 72); moss .28 .635 .42 .67
# (92 109 41); violet sky .02 .02 .30 .08 (162 100 150); hills .20 .20 .33 .29 (108 145 67); towers lit by the
# backdrop's probes .375 .12 .405 .28 (198 169 100); magenta pillar (indoor probes) .615 .42 .643 .58 (157 73 146);
# glowing cyan sign in .40 .34 .60 .40; the decal's dark ring and bright spot in .567 .649 .74 .721
import math, os, struct, sys, zlib

sys.dont_write_bytecode = True  # no __pycache__ next to it
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from assets import GLB  # noqa: E402

H = 5.0                      # court wall height, m
SUN = (0.3687, 0.7660, 0.5265)  # towards the sun: 50 degrees up, from the south-south-east
BACKDROP_SCALE = 16.0
BACKDROP_Y = -8.0


# ---------------------------------------------------------------- small maths

def add(a, b):
    return (a[0] + b[0], a[1] + b[1], a[2] + b[2])


def mul(a, s):
    return (a[0] * s, a[1] * s, a[2] * s)


def dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def unit(a):
    n = math.sqrt(dot(a, a))
    return (a[0] / n, a[1] / n, a[2] / n)


def mix(a, b, t):
    return tuple(x + (y - x) * t for x, y in zip(a, b))


def clamp01(x):
    return 0.0 if x < 0 else 1.0 if x > 1 else x


def smooth(e0, e1, x):
    t = clamp01((x - e0) / (e1 - e0))
    return t * t * (3 - 2 * t)


def byte(x):
    return int(round(clamp01(x) * 255))


def srgb_to_linear(c):
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


# ---------------------------------------------------------------- images

def png(w, h, channels, px):
    """8-bit PNG from rows of bytes, top first (1, 3 or 4 channels); Sub or Up filter, whichever deflates smaller"""
    stride = w * channels
    rows = [bytes(px[y * stride:(y + 1) * stride]) for y in range(h)]
    sub = [b'\1' + bytes((a - b) & 255 for a, b in zip(r, bytes(channels) + r[:-channels])) for r in rows]
    up = [b'\2' + bytes((a - b) & 255 for a, b in zip(r, p)) for r, p in zip(rows, [bytes(stride)] + rows[:-1])]
    data = min((zlib.compress(b''.join(f), 9) for f in (sub, up)), key=len)

    def chunk(kind, body):
        return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body) & 0xffffffff)
    head = struct.pack('>IIBBBBB', w, h, 8, {1: 0, 3: 2, 4: 6}[channels], 0, 0, 0)
    return b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', head) + chunk(b'IDAT', data) + chunk(b'IEND', b'')


def image(w, h, channels, fn):
    """fn(x, y) -> the pixel's channels in 0..1"""
    px = bytearray(w * h * channels)
    i = 0
    for y in range(h):
        for x in range(w):
            for c in fn(x, y):
                px[i] = byte(c)
                i += 1
    return png(w, h, channels, px)


def rgbe(c):
    """linear RGB as cs2map's RGBE bytes: mantissas, then exponent + 128; decodes as (m + 0.5) 2^(e - 136)"""
    m = max(c)
    if m <= 1e-9:
        return (0, 0, 0, 0)
    e = math.frexp(m)[1]
    k = math.ldexp(256.0, -e)
    return tuple(min(255, int(v * k)) for v in c) + (e + 128,)


def hash01(i, j, seed):
    h = (i * 374761393 + j * 668265263 + seed * 144269477) & 0xffffffff
    h = ((h ^ (h >> 13)) * 1274126177) & 0xffffffff
    return ((h ^ (h >> 16)) & 0xffff) / 65536.0


def fbm(size, seed):
    """tileable size x size value noise in 0..1, octaves from 1/4 of the size down"""
    out = [0.0] * (size * size)
    for period, weight in ((4, 0.45), (8, 0.25), (16, 0.18), (32, 0.12)):
        lat = [[hash01(i, j, seed + period) for i in range(period)] for j in range(period)]
        cell = size / period
        cols = []
        for x in range(size):
            f = (x + 0.5) / cell
            i0 = int(f)
            f -= i0
            cols.append((i0 % period, (i0 + 1) % period, f * f * (3 - 2 * f)))
        for y in range(size):
            g = (y + 0.5) / cell
            j0 = int(g)
            g -= j0
            g = g * g * (3 - 2 * g)
            r0, r1 = lat[j0 % period], lat[(j0 + 1) % period]
            base = y * size
            for x, (i0, i1, f) in enumerate(cols):
                a = r0[i0] + (r0[i1] - r0[i0]) * f
                out[base + x] += (a + (r1[i0] + (r1[i1] - r1[i0]) * f - a) * g) * weight
    return out


N256 = N128 = None


def n256(x, y, shift=0):
    return N256[((y + shift) % 256) * 256 + (x + 3 * shift) % 256]


def n128(x, y, shift=0):
    return N128[((y + shift) % 128) * 128 + (x + 3 * shift) % 128]


def normal_px(height, x, y, size, k):
    """tangent-space normal from a tileable height field, green pointing down as in Source"""
    dx = height((x + 1) % size, y) - height((x - 1) % size, y)
    dy = height(x, (y + 1) % size) - height(x, (y - 1) % size)
    n = unit((-dx * k, -dy * k, 1.0))
    return (n[0] * 0.5 + 0.5, n[1] * 0.5 + 0.5, n[2] * 0.5 + 0.5)


# the sign's letters, 5 x 7
GLYPHS = {
    'L': ['#....', '#....', '#....', '#....', '#....', '#....', '#####'],
    'I': ['#####', '..#..', '..#..', '..#..', '..#..', '..#..', '#####'],
    'T': ['#####', '..#..', '..#..', '..#..', '..#..', '..#..', '..#..'],
    'C': ['.####', '#....', '#....', '#....', '#....', '#....', '.####'],
    'O': ['.###.', '#...#', '#...#', '#...#', '#...#', '#...#', '.###.'],
    'U': ['#...#', '#...#', '#...#', '#...#', '#...#', '#...#', '.###.'],
    'R': ['####.', '#...#', '#...#', '####.', '#.#..', '#..#.', '#...#'],
    ' ': ['.....'] * 7,
}


def lettering(text, w, h, scale):
    """set of pixels covered by the text, centred in w x h"""
    on = set()
    x0, y0 = (w - (len(text) * 6 - 1) * scale) // 2, (h - 7 * scale) // 2
    for k, ch in enumerate(text):
        for r, row in enumerate(GLYPHS[ch]):
            for c, v in enumerate(row):
                if v == '#':
                    for dy in range(scale):
                        for dx in range(scale):
                            on.add((x0 + (k * 6 + c) * scale + dx, y0 + r * scale + dy))
    return on


def textures():
    """the materials' images: name -> PNG bytes"""
    t = {}

    # floor: stone tiles, 1 m, 2 x 2 in the image; bevelled into the grout
    def tile_height(x, y):
        d = min(x % 128, 127 - x % 128, y % 128, 127 - y % 128)
        return smooth(2.5, 12.0, d) + n256(x, y, 7) * 0.08

    def floor(x, y):
        v = 0.9 + 0.2 * hash01(x // 128, y // 128, 5) + 0.25 * (n256(x, y) - 0.5)
        grout = 1.0 - smooth(2.0, 4.5, min(x % 128, 127 - x % 128, y % 128, 127 - y % 128))
        return mix((0.76 * v, 0.73 * v, 0.68 * v), (0.42, 0.40, 0.37), grout)
    t['floor_base'] = image(256, 256, 3, floor)
    t['floor_normal'] = image(256, 256, 3, lambda x, y: normal_px(tile_height, x, y, 256, 3.0))

    # floor's moss layer; mask g = where it meets the tiles, r = how softly; normal map with roughness in alpha
    t['moss'] = image(128, 128, 3, lambda x, y: mix((0.16, 0.30, 0.07), (0.45, 0.62, 0.16), n128(x, y) ** 1.5))
    t['moss_mask'] = image(128, 128, 3, lambda x, y: (0.08 + 0.12 * n128(x, y, 40), 0.2 + 0.6 * n128(x, y, 11), 0.0))
    t['moss_normal'] = image(128, 128, 4, lambda x, y: normal_px(lambda a, b: n128(a, b, 23), x, y, 128, 6.0) + (0.92,))

    # walls: sandstone blocks in running bond (0.5 x 0.25 m), a mod2x detail of stains
    def wall(x, y):
        row = y // 32
        bx = (x + (16 if row % 2 else 0)) % 64
        by = y % 32
        mortar = 1.0 - smooth(1.0, 3.0, min(bx, 63 - bx, by, 31 - by))
        v = 0.88 + 0.2 * hash01((x + (16 if row % 2 else 0)) // 64, row, 9) + 0.18 * (n256(x, y, 50) - 0.5)
        return mix((0.84 * v, 0.74 * v, 0.58 * v), (0.60, 0.55, 0.47), mortar)
    t['wall_base'] = image(256, 256, 3, wall)
    t['wall_detail'] = image(128, 128, 3, lambda x, y: (0.5 + 0.9 * (n128(x, y, 70) - 0.5),) * 3)

    # crates: planks, overlay scratches, and a mask on the second uv set showing them at the face edges
    def crate(x, y):
        frame = min(x, 127 - x, y, 127 - y) < 12
        g = n128(x * 4 % 128, y // 4, 90)
        plank = hash01(0, y // 26, 17)
        c = (0.55 + 0.2 * g + 0.1 * plank, 0.37 + 0.14 * g + 0.06 * plank, 0.20 + 0.08 * g)
        return mul(c, 0.8) if frame else c
    t['crate_base'] = image(128, 128, 3, crate)
    def scratches(x, y):
        return (0.9 if (x * 3 + y * 7) % 41 < 2 else 0.2 if (x * 5 - y * 2) % 53 < 2 else 0.5,) * 3
    t['crate_detail'] = image(128, 128, 3, scratches)
    t['crate_mask'] = image(64, 64, 3, lambda x, y: (1.0 - smooth(2.0, 14.0, min(x, 63 - x, y, 63 - y)), 0.0, 0.0))

    # concrete (the pillar, the bollards, the glass's frame), cast in 0.5 m lifts
    def concrete(x, y):
        v = 0.6 + 0.2 * (n128(x, y, 120) - 0.5)
        return (v - (0.08 if y % 32 < 2 else 0.0),) * 2 + (v + 0.02,)
    t['concrete'] = image(128, 128, 3, concrete)

    # the sign: a dark panel with light letters, and the letters as its self-illumination mask
    letters = lettering('LIT COURT', 256, 64, 4)

    def border(x, y):
        return min(x, 255 - x, y, 63 - y) < 4
    t['sign_base'] = image(256, 64, 3, lambda x, y: (0.75, 0.78, 0.8) if (x, y) in letters or border(x, y) else (0.09, 0.1, 0.12))
    t['sign_glow'] = image(256, 64, 3, lambda x, y: (1.0,) * 3 if (x, y) in letters else (0.35,) * 3 if border(x, y) else (0.0,) * 3)

    # the decal, multiplied in (mod2x): a dark ring round a bright spot, no change outside
    def decal(x, y):
        r = math.hypot(x - 63.5, y - 63.5) / 64.0
        ring = smooth(0.62, 0.7, r) * (1.0 - smooth(0.86, 0.94, r))
        spot = 1.0 - smooth(0.18, 0.24, r)
        c = 0.1 if ring > 0.0 else 0.92
        return (c, c, c, max(ring, spot))
    t['decal'] = image(128, 128, 4, decal)

    # the effect's mask: rays, fading out at the top and the bottom
    def rays(x, y):
        return ((0.5 + 0.5 * math.sin(x / 64 * 6 * math.pi + math.sin(x * 0.7))) * smooth(0, 10, y) * smooth(63, 30, y),) * 3
    t['rays'] = image(64, 64, 3, rays)

    # the backdrop's hills: grass and rock
    t['hills'] = image(64, 64, 3, lambda x, y: mix((0.36, 0.40, 0.22), (0.55, 0.50, 0.40), n128(x * 2, y * 2, 150)))

    # sky: violet zenith, coral horizon, dusty pink below (the fog's colour), sun glow
    zenith, horizon, ground = (0.30, 0.20, 0.62), (0.9, 0.5, 0.45), (0.55, 0.33, 0.36)

    def sky(x, y):
        a, th = (x + 0.5) / 256 * 2 * math.pi, (y + 0.5) / 128 * math.pi
        d = (math.sin(th) * math.cos(a), math.cos(th), math.sin(th) * math.sin(a))
        el = math.pi / 2 - th
        c = mix(horizon, zenith, (el / (math.pi / 2)) ** 0.55) if el >= 0 else mix(horizon, ground, min(1.0, -el / 0.3))
        s = max(0.0, dot(d, SUN))
        return add(c, mul((1.0, 0.85, 0.6), s ** 24 * 0.5 + s ** 600 * 1.5))
    t['sky'] = image(256, 128, 3, sky)
    return t


# ---------------------------------------------------------------- the baked lighting

def sunlit(p, n):
    """whether the sun gets to p, on a surface facing n, past the court's walls"""
    if dot(n, SUN) <= 0.0:
        return False
    for axis, at in ((2, -10.0), (2, 10.0), (0, 10.0), (0, -10.0)):
        if abs(SUN[axis]) < 1e-6 or abs(p[axis] - at) < 1e-3:
            continue
        t = (at - p[axis]) / SUN[axis]
        if t <= 0:
            continue
        q = add(p, mul(SUN, t))
        if 0.0 <= q[1] <= H and -10.0 <= q[2 - axis] <= 10.0:
            return False
    return True


class Lightmap:
    """a lightmap's three images in charts: a texel rectangle per surface, its edge repeated into a PAD border so
    filtering and mips don't bleed"""
    PAD = 6

    def __init__(self, w, h):
        self.w, self.h = w, h
        self.irr = [(0.0, 0.0, 0.0)] * (w * h)
        self.dir = [(0.5, 0.5, 1.0, 1.0)] * (w * h)
        self.shd = [0.0] * (w * h)

    def chart(self, u0, v0, cw, ch, fn):
        """fn(s, t) -> (irradiance, directional, shadow) for s, t in 0..1 over the surface; returns its uv mapping"""
        assert u0 >= self.PAD and v0 >= self.PAD and u0 + cw + self.PAD <= self.w and v0 + ch + self.PAD <= self.h
        for y in range(v0 - self.PAD, v0 + ch + self.PAD):
            for x in range(u0 - self.PAD, u0 + cw + self.PAD):
                s, t = clamp01((x + 0.5 - u0) / cw), clamp01((y + 0.5 - v0) / ch)
                i = y * self.w + x
                self.irr[i], self.dir[i], self.shd[i] = fn(s, t)
        return lambda s, t: ((u0 + s * cw) / self.w, (v0 + t * ch) / self.h)

    def images(self):
        irr = bytearray(c for v in self.irr for c in rgbe(v))
        dr = bytearray(byte(c) for v in self.dir for c in v)
        shd = bytearray(byte(v) for v in self.shd)
        return png(self.w, self.h, 4, irr), png(self.w, self.h, 4, dr), png(self.w, self.h, 1, shd)


ORANGE, TEAL = (1.6, 0.7, 0.22), (0.15, 0.8, 1.4)


def floor_light(s, t):
    """floor: 5 m orange/teal checker, darker by the walls; baked sun shadow plus a band (z -2.2..-1.2) only it has;
    directional light from +x on the west half, -x on the east, less specular near walls"""
    x, z = -10.0 + 20.0 * s, -10.0 + 20.0 * t
    cell = (int((x + 10.0) // 5.0) + int((z + 10.0) // 5.0)) % 2
    d = min(10.0 - abs(x), 10.0 - abs(z))
    k = 0.55 + 0.45 * smooth(0.0, 1.5, d)
    irr = mul(TEAL if cell else ORANGE, k)
    lean = 0.55 if x < 0 else -0.55
    dr = (0.5 + 0.5 * lean, 0.5 + 0.5 * 0.15, 0.3, 0.35 + 0.65 * smooth(0.3, 1.5, d))
    shadow = 0.0 if sunlit((x, 0.0, z), (0.0, 1.0, 0.0)) else 1.0
    if -2.2 <= z <= -1.2:
        shadow = 1.0
    return irr, dr, shadow


# per wall: left end seen from inside, direction along, facing, light at the top and at the bottom
WALLS = {
    'north': ((-10.0, 0.0, -10.0), (1.0, 0.0, 0.0), (0.0, 0.0, 1.0), (1.8, 1.55, 1.0), (0.55, 0.42, 0.32)),
    'east': ((10.0, 0.0, -10.0), (0.0, 0.0, 1.0), (-1.0, 0.0, 0.0), (1.5, 0.6, 1.1), (0.45, 0.2, 0.36)),
    'south': ((10.0, 0.0, 10.0), (-1.0, 0.0, 0.0), (0.0, 0.0, -1.0), (0.6, 0.85, 1.7), (0.2, 0.28, 0.5)),
    'west': ((-10.0, 0.0, 10.0), (0.0, 0.0, -1.0), (1.0, 0.0, 0.0), (0.75, 1.5, 0.7), (0.25, 0.45, 0.22)),
}


def wall_light(name):
    start, along, facing, top, bottom = WALLS[name]

    def fn(s, t):
        p = add(add(start, mul(along, 20.0 * s)), (0.0, H * (1.0 - t), 0.0))
        return mix(top, bottom, t ** 0.8), (0.5, 0.5, 1.0, 1.0), 0.0 if sunlit(add(p, mul(facing, 0.01)), facing) else 1.0
    return fn


def hills_light(s, t):
    """backdrop hills: lime and sea-green terraces from crest (t 0) to foot (t 1), the foot in baked shadow"""
    band = int(t * 5.0) % 2
    return mul((0.35, 1.9, 1.1) if band else (0.8, 2.6, 0.45), 1.0 - 0.45 * t), (0.5, 0.5, 1.0, 1.0), 1.0 if t > 0.72 else 0.0


# probe atlas blocks hold the light along Source's +x +y +z -x -y -z, i.e. onto surfaces facing glTF's +z (south), +x
# (east), +y (up), -z (north), -x (west) and -y (down)
CUBE_OUTDOOR = ((0.3, 1.5, 0.35), (1.6, 0.3, 0.25), (0.8, 0.6, 1.7), (1.3, 1.1, 0.15), (0.25, 0.5, 1.8), (0.5, 0.35, 0.25))
CUBE_INDOOR = ((1.5, 0.25, 1.3), (1.5, 0.25, 1.3), (1.8, 0.3, 1.55), (1.5, 0.25, 1.3), (1.5, 0.25, 1.3), (0.4, 0.07, 0.35))
CUBE_BACKDROP = ((1.8, 1.2, 0.35), (1.6, 0.7, 0.3), (1.2, 1.2, 1.4), (0.5, 0.45, 0.4), (0.9, 0.6, 0.35), (0.2, 0.18, 0.15))


def volume(lo, hi, offset, size, priority):
    """probe volume as cs2map writes it: column-major glTF world -> 0..1 box matrix (Source axes: x = glTF z, y = glTF
    x, z up), atlas place, corners"""
    sx, sy, sz = hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]
    rows = [[0, 0, 1 / sz, -lo[2] / sz], [1 / sx, 0, 0, -lo[0] / sx], [0, 1 / sy, 0, -lo[1] / sy], [0, 0, 0, 1]]
    return {'matrix': [float(rows[r][c]) for c in range(4) for r in range(4)], 'min': list(lo), 'max': list(hi),
            'atlasOffset': list(offset), 'atlasSize': list(size), 'priority': priority}


def probe_atlas(dims, vols, cube, shadow, cols=16):
    """RGBE irradiance atlas (6 blocks of z slices, cols wide) and baked sun shadow (one block); cube(volume, p, block)
    and shadow(volume, p) get each probe's glTF position"""
    W, Hh, D = dims
    rows = (6 * D + cols - 1) // cols
    irr = bytearray(cols * W * rows * Hh * 4)
    shd = bytearray(cols * W * ((D + cols - 1) // cols) * Hh)
    for k, v in enumerate(vols):
        lo, hi, off, size = v['min'], v['max'], v['atlasOffset'], v['atlasSize']
        for az in range(size[2]):
            for ay in range(size[1]):
                for ax in range(size[0]):
                    q = ((ax + 0.5) / size[0], (ay + 0.5) / size[1], (az + 0.5) / size[2])
                    p = (lo[0] + q[1] * (hi[0] - lo[0]), lo[1] + q[2] * (hi[1] - lo[1]), lo[2] + q[0] * (hi[2] - lo[2]))
                    x, y, z = ax + off[0], ay + off[1], az + off[2]
                    for b in range(6):
                        sl = b * D + z
                        i = ((sl // cols * Hh + y) * cols * W + sl % cols * W + x) * 4
                        irr[i:i + 4] = bytes(rgbe(cube(k, p, b)))
                    shd[(z // cols * Hh + y) * cols * W + z % cols * W + x] = byte(shadow(k, p))
    return png(cols * W, rows * Hh, 4, irr), png(cols * W, len(shd) // (cols * W), 1, shd)


# ---------------------------------------------------------------- geometry

def rect(origin, du, dv, us, vs, attrs):
    """grid over origin + s du + t dv at fractions us x vs, facing cross(du, dv); attrs(s, t, p) adds vertex attributes"""
    n = unit(cross(du, dv))
    verts = []
    for t in vs:
        for s in us:
            p = add(origin, add(mul(du, s), mul(dv, t)))
            v = {'POSITION': p, 'NORMAL': n}
            v.update(attrs(s, t, p))
            verts.append(v)
    tris, nu = [], len(us)
    for j in range(len(vs) - 1):
        for i in range(nu - 1):
            a = j * nu + i
            tris += [(a, a + 1, a + nu + 1), (a, a + nu + 1, a + nu)]
    return verts, tris


def merge(*parts):
    verts, tris = [], []
    for v, t in parts:
        base = len(verts)
        verts += v
        tris += [(a + base, b + base, c + base) for a, b, c in t]
    return verts, tris


def box(lo, hi, attrs):
    """a box's six faces, each seen from outside with s to the right and t up; attrs(face, s, t, p)"""
    x0, y0, z0 = lo
    x1, y1, z1 = hi
    faces = [((x0, y0, z1), (x1 - x0, 0, 0), (0, y1 - y0, 0)), ((x1, y0, z0), (x0 - x1, 0, 0), (0, y1 - y0, 0)),
             ((x1, y0, z1), (0, 0, z0 - z1), (0, y1 - y0, 0)), ((x0, y0, z0), (0, 0, z1 - z0), (0, y1 - y0, 0)),
             ((x0, y1, z1), (x1 - x0, 0, 0), (0, 0, z0 - z1)), ((x0, y0, z0), (x1 - x0, 0, 0), (0, 0, z1 - z0))]
    return merge(*(rect(o, du, dv, (0, 1), (0, 1), lambda s, t, p, f=f: attrs(f, s, t, p)) for f, (o, du, dv) in enumerate(faces)))


def face_uv(f, s, t, p):
    return {'TEXCOORD_0': (s, 1.0 - t)}


class Map(GLB):
    """assets.py's GLB plus images, textures, any vertex attribute and a node tree"""

    def __init__(self):
        super().__init__()
        self.js['asset']['generator'] = 'hypr3d tools/test/vm/litmap.py'
        self.js.update(images=[], textures=[], samplers=[
            {'magFilter': 9729, 'minFilter': 9987, 'wrapS': 10497, 'wrapT': 10497},
            {'magFilter': 9729, 'minFilter': 9987, 'wrapS': 33071, 'wrapT': 33071}])
        self.js['scenes'][0]['nodes'] = []

    def data(self, values, kind, component=5126, normalized=False, minmax=False, target=34962):
        n = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4}[kind]
        flat = [c for v in values for c in (v if n > 1 else (v,))]
        if component == 5121:
            flat = [byte(c) for c in flat]
        fmt = {5126: 'f', 5121: 'B', 5125: 'I'}[component]
        acc = {'bufferView': self.view(struct.pack('<%d%s' % (len(flat), fmt), *flat), target), 'componentType': component,
               'count': len(values), 'type': kind}
        if normalized:
            acc['normalized'] = True
        if minmax:
            acc['min'] = [min(v[k] for v in values) for k in range(n)]
            acc['max'] = [max(v[k] for v in values) for k in range(n)]
        self.js['accessors'].append(acc)
        return len(self.js['accessors']) - 1

    def image(self, name, data):
        while len(self.bin) % 4:
            self.bin.append(0)
        self.js['bufferViews'].append({'buffer': 0, 'byteOffset': len(self.bin), 'byteLength': len(data)})
        self.bin += data
        self.js['images'].append({'name': name, 'mimeType': 'image/png', 'bufferView': len(self.js['bufferViews']) - 1})
        return len(self.js['images']) - 1

    def texture(self, img, sampler=0):
        self.js['textures'].append({'source': img, 'sampler': sampler})
        return {'index': len(self.js['textures']) - 1}

    def mat(self, m):
        self.js['materials'].append(m)
        return len(self.js['materials']) - 1

    def prim(self, part, material, colors=5126):
        """a primitive of (vertices, triangles); COLOR_0 as floats, or normalized bytes (colors 5121)"""
        verts, tris = part
        at = {}
        for k in verts[0]:
            vals = [v[k] for v in verts]
            kind = {1: 'SCALAR', 2: 'VEC2', 3: 'VEC3', 4: 'VEC4'}[len(vals[0])]
            if k == 'COLOR_0' and colors == 5121:
                at[k] = self.data(vals, kind, 5121, normalized=True)
            else:
                at[k] = self.data(vals, kind, minmax=k in ('POSITION', '_LIGHTMAP_UV'))
        return {'attributes': at, 'indices': self.data([i for t in tris for i in t], 'SCALAR', 5125, target=34963),
                'material': material}

    def add_mesh(self, name, prims):
        self.js['meshes'].append({'name': name, 'primitives': prims})
        return len(self.js['meshes']) - 1

    def place(self, name, parent=None, **kw):
        """a node, at the scene's root or under parent; kw: mesh, translation, rotation, matrix, extensions"""
        n = {'name': name}
        n.update((k, v) for k, v in kw.items() if v is not None)
        self.js['nodes'].append(n)
        i = len(self.js['nodes']) - 1
        if parent is None:
            self.js['scenes'][0]['nodes'].append(i)
        else:
            self.js['nodes'][parent].setdefault('children', []).append(i)
        return i


def source2(**kw):
    """HYPR3D_materials_source2, with what cs2map gives every material"""
    return dict({'normalYDown': True}, **kw)


def litcourt(path, baked_shadow=True):
    global N256, N128
    N256, N128 = fbm(256, 1), fbm(128, 2)
    g = Map()
    img = {name: g.image(name, data) for name, data in textures().items()}
    tex = {name: g.texture(i, 1 if name == 'decal' else 0) for name, i in img.items()}

    # ------------------------------------------------ the court's lightmap and probes (lighting set 0)
    lm = Lightmap(256, 256)
    floor_uv = lm.chart(6, 6, 100, 100, floor_light)
    wall_uv = {name: lm.chart(118, 6 + 37 * k, 100, 25, wall_light(name)) for k, name in enumerate(('north', 'east', 'south', 'west'))}
    kinds = ('irradiance', 'directional', 'shadows') if baked_shadow else ('irradiance', 'directional')
    lm0 = [g.image('map_' + k, d) for k, d in zip(kinds, lm.images())]

    # the pillar is in both volumes, nearer the outdoor one's middle: the indoor one wins only by priority
    outdoor = volume((-10.5, -0.5, -10.5), (10.5, 7.5, 10.5), (0, 0, 0), (6, 6, 3), 0)
    indoor = volume((2.0, -0.5, -10.5), (10.5, 7.5, 10.5), (6, 0, 0), (2, 2, 2), 1)

    def court_cube(k, p, b):
        if k == 1:
            return CUBE_INDOOR[b]
        return mul(CUBE_OUTDOOR[b], (0.75 + 0.25 * p[1] / 7.5) * (0.85 + 0.3 * (p[0] + 10.0) / 20.0))
    probes0 = probe_atlas((8, 6, 3), [outdoor, indoor], court_cube,
                          lambda k, p: 1.0 if k == 1 or not sunlit(p, (0.0, 1.0, 0.0)) else 0.0)
    pr0 = [g.image('map_probes', probes0[0])] + ([g.image('map_probe_shadows', probes0[1])] if baked_shadow else [])

    # ------------------------------------------------ the backdrop's (lighting set 1), in world space
    lm1 = Lightmap(128, 64)
    hills_uv = lm1.chart(6, 6, 116, 26, hills_light)
    lb = [g.image('skybox_' + k, d) for k, d in zip(kinds, lm1.images())]
    far = volume((-560.0, -40.0, -560.0), (560.0, 400.0, 560.0), (0, 0, 0), (4, 4, 2), 0)
    probes1 = probe_atlas((4, 4, 2), [far], lambda k, p, b: CUBE_BACKDROP[b], lambda k, p: 0.0)
    pr1 = [g.image('skybox_probes', probes1[0])] + ([g.image('skybox_probe_shadows', probes1[1])] if baked_shadow else [])

    # ------------------------------------------------ materials, as cs2map leaves them
    floor = g.mat({'name': 'materials/litcourt/floor_tiles', 'normalTexture': dict(tex['floor_normal']),
                   'pbrMetallicRoughness': {'baseColorTexture': tex['floor_base'], 'metallicFactor': 0.0, 'roughnessFactor': 0.7},
                   'extensions': {'HYPR3D_materials_blend': {'texture': tex['moss'], 'maskTexture': tex['moss_mask'],
                                                             'normalTexture': tex['moss_normal'], 'factor': [0.9, 1.0, 0.8, 1.0],
                                                             'uvScale': [2.0, 2.0]},
                                  'HYPR3D_materials_source2': source2(specular=[False, True], vertexColor='none')}})
    wall = g.mat({'name': 'materials/litcourt/sandstone_wall',
                  'pbrMetallicRoughness': {'baseColorTexture': tex['wall_base'], 'metallicFactor': 0.0, 'roughnessFactor': 0.9},
                  'extensions': {'HYPR3D_materials_source2': source2(
                      specular=[False, False], vertexColor='srgb',
                      detail={'texture': tex['wall_detail'], 'mode': 'mod2x', 'blend': 1.0, 'tint': [1.0, 0.85, 0.7],
                              'transform': [0.5, 0.0, 0.0, 0.5, 0.0, 0.0]})}})
    decal = g.mat({'name': 'materials/litcourt/decal_ring', 'alphaMode': 'BLEND',
                   'pbrMetallicRoughness': {'baseColorTexture': tex['decal'], 'metallicFactor': 0.0},
                   'extensions': {'KHR_materials_unlit': {},
                                  'HYPR3D_materials_source2': source2(specular=[False, False], blendMode='mod2x')}})
    crate = g.mat({'name': 'materials/litcourt/crate_painted',
                   'pbrMetallicRoughness': {'baseColorTexture': tex['crate_base'], 'metallicFactor': 0.0, 'roughnessFactor': 0.85},
                   'extensions': {'HYPR3D_materials_source2': source2(
                       specular=[True, True], vertexColor='paint',
                       detail={'texture': tex['crate_detail'], 'maskTexture': tex['crate_mask'], 'mode': 'overlay', 'blend': 1.0,
                               'blendToFull': 0.1, 'transform': [2.0, 0.0, 0.0, 2.0, 0.0, 0.0], 'maskUV': 1})}})
    concrete = g.mat({'name': 'materials/litcourt/concrete',
                      'pbrMetallicRoughness': {'baseColorTexture': tex['concrete'], 'metallicFactor': 0.0, 'roughnessFactor': 0.55},
                      'extensions': {'HYPR3D_materials_source2': source2(specular=[True, True])}})
    sign = g.mat({'name': 'materials/litcourt/sign_selfillum', 'emissiveTexture': tex['sign_glow'], 'emissiveFactor': [0.1, 1.0, 0.9],
                  'pbrMetallicRoughness': {'baseColorTexture': tex['sign_base'], 'metallicFactor': 0.0, 'roughnessFactor': 0.8},
                  'extensions': {'KHR_materials_emissive_strength': {'emissiveStrength': 3.0},
                                 'HYPR3D_materials_source2': source2(specular=[False, False], vertexColor='none',
                                                                     selfIllumAlbedo=0.25)}})
    glass = g.mat({'name': 'materials/litcourt/glass_green', 'alphaMode': 'BLEND',
                   'pbrMetallicRoughness': {'baseColorFactor': [srgb_to_linear(0.45), srgb_to_linear(0.9), srgb_to_linear(0.7), 0.2],
                                            'metallicFactor': 0.0, 'roughnessFactor': 0.05},
                   'extensions': {'HYPR3D_materials_source2': source2(specular=[True, True], glass=True)}})
    rays = g.mat({'name': 'materials/litcourt/light_rays', 'alphaMode': 'BLEND',
                  'pbrMetallicRoughness': {'baseColorFactor': [1.0, 0.75, 0.45, 0.7], 'metallicFactor': 0.0},
                  'extensions': {'KHR_materials_unlit': {}, 'HYPR3D_materials_source2': source2(specular=[True, True], effect={
                      'masks': [{'texture': tex['rays'], 'scale': [1.0, 1.0], 'pan': [0.02, 0.0]}], 'colorBoost': 1.6, 'opacity': 0.8,
                      'additive': True, 'fog': True, 'fade': [0.05, 1.0, 0.0, 1.0], 'fresnel': [0.001, 1.0, 0.0, 1.0]})}})
    trigger = g.mat({'name': 'materials/tools/toolstrigger', 'pbrMetallicRoughness': {'baseColorFactor': [1.0, 1.0, 1.0, 1.0]}})
    hills = g.mat({'name': 'materials/litcourt/skybox_hills',
                   'pbrMetallicRoughness': {'baseColorTexture': tex['hills'], 'metallicFactor': 0.0, 'roughnessFactor': 1.0},
                   'extensions': {'HYPR3D_materials_source2': source2(specular=[False, False], vertexColor='none')}})
    tower = g.mat({'name': 'materials/litcourt/skybox_tower',
                   'pbrMetallicRoughness': {'baseColorTexture': tex['wall_base'], 'metallicFactor': 0.0, 'roughnessFactor': 0.9},
                   'extensions': {'HYPR3D_materials_source2': source2(specular=[True, True])}})
    skydome = g.mat({'name': 'skydome', 'pbrMetallicRoughness': {'baseColorTexture': tex['sky'], 'metallicFactor': 0.0},
                     'extensions': {'KHR_materials_unlit': {}}})

    # ------------------------------------------------ the court (world geometry, lightmapped)
    def floor_attrs(s, t, p):
        x, z = p[0], p[2]
        w = smooth(2.3, 1.0, math.hypot(x + 2.6, z + 4.6))  # the moss, round (-2.6, -4.6)
        return {'TEXCOORD_0': (x / 2.0, z / 2.0), 'TANGENT': (1.0, 0.0, 0.0, 1.0), 'COLOR_0': (1.0, 0.0, 0.0, 1.0),
                '_BLEND': (w, 0.0, 0.0, 0.0), '_LIGHTMAP_UV': floor_uv((x + 10.0) / 20.0, (z + 10.0) / 20.0)}
    cells = [k / 20.0 for k in range(21)]
    part = rect((-10.0, 0.0, 10.0), (20.0, 0.0, 0.0), (0.0, 0.0, -20.0), cells, cells, floor_attrs)
    g.place('node000_floor', mesh=g.add_mesh('node000_floor', [g.prim(part, floor)]))

    walls = []
    for name, (start, along, facing, _, _) in WALLS.items():
        def attrs(s, t, p, uv=wall_uv[name]):
            dirt = mix((0.5, 0.42, 0.36), (1.0, 1.0, 1.0), smooth(0.0, 1.2, p[1]))
            return {'TEXCOORD_0': (s * 10.0, (H - p[1]) / 2.0), 'COLOR_0': dirt + (1.0,), '_LIGHTMAP_UV': uv(s, 1.0 - p[1] / H)}
        walls.append(rect(start, mul(along, 20.0), (0.0, H, 0.0), (0, 0.25, 0.5, 0.75, 1), (0, 0.5 / H, 1.2 / H, 1), attrs))
    g.place('node001_walls', mesh=g.add_mesh('node001_walls', [g.prim(merge(*walls), wall)]))

    # a mod2x decal on the floor, east of the middle (unlit, as Source's overlays)
    part = rect((1.0, 0.012, -2.4), (2.6, 0.0, 0.0), (0.0, 0.0, -2.6), (0, 1), (0, 1), lambda s, t, p: {'TEXCOORD_0': (s, t)})
    g.place('node002_overlay', mesh=g.add_mesh('node002_overlay', [g.prim(part, decal)]))

    # ------------------------------------------------ props (lit by the probes)
    def crate_attrs(paint):
        return lambda f, s, t, p: {'TEXCOORD_0': (s, 1.0 - t), 'TEXCOORD_1': (s, 1.0 - t), 'COLOR_0': paint}
    part = merge(box((-7.2, 0.0, -6.6), (-6.2, 1.0, -5.6), crate_attrs((1.0, 1.0, 1.0, 0.0))),
                 box((-6.0, 0.0, -6.4), (-5.0, 1.0, -5.4), crate_attrs((1.0, 1.0, 1.0, 0.0))),
                 box((-6.7, 1.0, -6.5), (-5.7, 2.0, -5.5), crate_attrs((0.8, 0.15, 0.1, 0.7))))
    g.place('props/crates', mesh=g.add_mesh('crates', [g.prim(part, crate, colors=5121)]))

    sides = []
    for k in range(8):
        a0, a1 = k / 8 * 2 * math.pi, (k + 1) / 8 * 2 * math.pi
        c0 = (2.6 + 0.35 * math.cos(a0), 0.0, -6.0 + 0.35 * math.sin(a0))
        c1 = (2.6 + 0.35 * math.cos(a1), 0.0, -6.0 + 0.35 * math.sin(a1))
        sides.append(rect(c1, (c0[0] - c1[0], 0.0, c0[2] - c1[2]), (0.0, 3.2, 0.0), (0, 1), (0, 1),
                          lambda s, t, p, k=k: {'TEXCOORD_0': ((k + 1 - s) / 4.0, (3.2 - p[1]) / 1.6)}))
    sides.append(box((2.2, 3.2, -6.4), (3.0, 3.35, -5.6), face_uv))
    g.place('props/pillar', mesh=g.add_mesh('pillar', [g.prim(merge(*sides), concrete)]))

    # the glass, in a concrete frame, west of the desktop
    part = rect((-4.6, 0.15, -6.0), (2.0, 0.0, 0.0), (0.0, 2.5, 0.0), (0, 1), (0, 1), lambda s, t, p: {'TEXCOORD_0': (s, 1.0 - t)})
    g.place('props/glass', mesh=g.add_mesh('glass', [g.prim(part, glass)]))
    part = merge(box((-4.7, 0.0, -6.05), (-4.6, 2.8, -5.95), face_uv), box((-2.6, 0.0, -6.05), (-2.5, 2.8, -5.95), face_uv),
                 box((-4.7, 2.65, -6.05), (-2.5, 2.8, -5.95), face_uv))
    g.place('props/glass_frame', mesh=g.add_mesh('glass_frame', [g.prim(part, concrete)]))

    # the sign over the desktop, glowing
    def sign_attrs(f, s, t, p):
        return {'TEXCOORD_0': (s, 1.0 - t) if f == 0 else (0.005, 0.005)}
    part = box((-3.0, 3.55, -9.99), (3.0, 4.55, -9.9), sign_attrs)
    g.place('props/sign', mesh=g.add_mesh('sign', [g.prim(part, sign)]))

    # light rays by the east wall (csgo_effects, added)
    part = rect((5.0, 0.0, -8.6), (3.6, 0.0, 0.0), (0.0, 4.6, 0.0), (0, 1), (0, 1), lambda s, t, p: {'TEXCOORD_0': (s, 1.0 - t)})
    g.place('props/light_rays', mesh=g.add_mesh('light_rays', [g.prim(part, rays)]))

    # bollards along the west wall, one mesh drawn six times (EXT_mesh_gpu_instancing)
    part = box((-0.12, 0.0, -0.12), (0.12, 0.9, 0.12), face_uv)
    at = [(-8.8, 0.0, z) for z in (-8.0, -6.0, -4.0, -2.0, 0.0, 2.0)]
    turn = [(0.0, math.sin(k * math.pi / 8), 0.0, math.cos(k * math.pi / 8)) for k in range(len(at))]
    grow = [(1.0, 1.0 + 0.1 * k, 1.0) for k in range(len(at))]
    inst = {'TRANSLATION': g.data(at, 'VEC3'), 'ROTATION': g.data(turn, 'VEC4'), 'SCALE': g.data(grow, 'VEC3')}
    g.place('props/bollards', mesh=g.add_mesh('bollard', [g.prim(part, concrete)]),
            extensions={'EXT_mesh_gpu_instancing': {'attributes': inst}})

    # a trigger brush the loader leaves out (it would stand left of the spawn, white)
    part = box((-3.5, 0.0, -0.5), (-2.5, 2.5, 0.5), face_uv)
    g.place('trigger_multiple', mesh=g.add_mesh('trigger_multiple', [g.prim(part, trigger)]))

    # ------------------------------------------------ the backdrop: a 3D skybox's scenery, in its own units
    sc = BACKDROP_SCALE
    bd = g.place('hypr3d_backdrop', matrix=[sc, 0, 0, 0, 0, sc, 0, 0, 0, 0, sc, 0, 0, BACKDROP_Y, 0, 1])
    verts, tris, segs = [], [], 48
    for k in range(segs + 1):
        a = k / segs * 2 * math.pi
        crest = 9.0 + 3.0 * math.sin(3 * a + 0.6) + 1.5 * math.sin(7 * a + 2.1)
        for r, (radius, y) in enumerate(((25.5, crest), (23.2, crest * 0.62), (21.0, 0.3))):
            p = (radius * math.cos(a), y, radius * math.sin(a))
            n = unit((-math.cos(a) * (crest - 0.3) / 4.5, 1.0, -math.sin(a) * (crest - 0.3) / 4.5))
            verts.append({'POSITION': p, 'NORMAL': n, 'TEXCOORD_0': (a * 4.0, r * 0.5), '_LIGHTMAP_UV': hills_uv(k / segs, r / 2.0)})
    for k in range(segs):
        for r in range(2):
            a, b = k * 3 + r, (k + 1) * 3 + r
            tris += [(a, a + 1, b + 1), (a, b + 1, b)]
    g.place('node100_skybox_hills', bd, mesh=g.add_mesh('node100_skybox_hills', [g.prim((verts, tris), hills)]))
    part = merge(*(box((x - w, 0.0, z - w), (x + w, h, z + w), lambda f, s, t, p: {'TEXCOORD_0': (s * 4.0, (1.0 - t) * 12.0)})
                   for x, z, w, h in ((-5.5, -22.5, 1.2, 14.0), (3.0, -21.0, 1.0, 18.0), (10.5, -25.0, 1.5, 12.0))))
    g.place('props/skybox_towers', bd, mesh=g.add_mesh('skybox_towers', [g.prim(part, tower)]))

    # ------------------------------------------------ the sky dome (as cs2map's sky_dome)
    verts, tris, cols, rows = [], [], 32, 16
    for r in range(rows + 1):
        for c in range(cols + 1):
            u, v = c / cols, r / rows
            a, b = u * 2 * math.pi, v * math.pi
            verts.append({'POSITION': (2000.0 * math.sin(b) * math.cos(a), 2000.0 * math.cos(b), 2000.0 * math.sin(b) * math.sin(a)),
                          'TEXCOORD_0': (u, v)})
    for r in range(rows):
        for c in range(cols):
            i = r * (cols + 1) + c
            tris += [(i, i + cols + 1, i + 1), (i + 1, i + cols + 1, i + cols + 2)]
    g.place('skydome', mesh=g.add_mesh('skydome', [g.prim((verts, tris), skydome)]))

    # ------------------------------------------------ where to start and where the desktop hangs
    g.place('hypr3d_spawn', translation=[0.0, 0.0, 3.0])             # facing -Z, the north wall
    g.place('hypr3d_desktop', translation=[0.0, 2.1, -9.99])         # front faces +Z

    # ------------------------------------------------ HYPR3D_lighting
    def lightmaps(ims):
        return {k: {'image': i} for k, i in zip(('irradiance', 'directional', 'shadows'), ims)}

    def probes(ims, size, volumes):
        return dict({'irradiance': {'image': ims[0]}, 'size': size, 'columns': 16, 'volumes': volumes},
                    **({'shadows': {'image': ims[1]}} if len(ims) > 1 else {}))
    g.js['extensions'] = {'HYPR3D_lighting': {
        'sets': [{'name': 'map', 'lightmaps': lightmaps(lm0), 'probes': probes(pr0, [8, 6, 3], [outdoor, indoor])},
                 {'name': 'skybox', 'lightmaps': lightmaps(lb), 'probes': probes(pr1, [4, 4, 2], [far])}],
        'sun': {'color': [1.0, 0.9, 0.7], 'direction': list(SUN)},
        'fog': {'start': 3.0, 'end': 50.0, 'exponent': 1.2, 'maxOpacity': 0.7, 'lodBias': 0.5,
                'heightStart': 0.0, 'heightEnd': 30.0, 'heightExponent': 1.0},
        'sky': {'image': img['sky'], 'color': [2.0, 2.0, 2.0]},
        'exposure': {'min': 0.35, 'max': 0.7, 'speedUp': 2.0, 'speedDown': 1.5},
        'tonemap': {'shoulderStrength': 0.22, 'linearStrength': 0.3, 'linearAngle': 0.1, 'toeStrength': 0.2, 'toeNum': 0.01,
                    'toeDenom': 0.3, 'whitePoint': 8.0, 'exposureBias': 0.3}}}
    g.js['extensionsUsed'] = ['HYPR3D_lighting', 'HYPR3D_materials_source2', 'HYPR3D_materials_blend', 'KHR_materials_unlit',
                              'KHR_materials_emissive_strength', 'EXT_mesh_gpu_instancing']
    g.write(path)


if __name__ == '__main__':
    out = sys.argv[1] if len(sys.argv) > 1 else '.'
    os.makedirs(out, exist_ok=True)
    for name, baked in (('LitCourt', True), ('LitCourtRuntimeSun', False)):
        litcourt(os.path.join(out, name + '.glb'), baked)
        print('wrote', os.path.join(out, name + '.glb'))
