# runs inside Blender: blender -b --factory-startup --python-exit-code 1 -P lighttool.py -- job.json
# converts CS2's decompiled lightmaps and probe atlases to the PNGs hyprwalk reads: HDR as RGBE (8-bit mantissas, shared
# exponent in alpha), single channels as grey, 3D atlases as a grid of slices; rows top first
import bpy, sys, json, zlib, struct, time
import numpy as np


def load(path):
    img = bpy.data.images.load(path)
    img.colorspace_settings.name = 'Non-Color'
    w, h = img.size
    px = np.empty(w * h * 4, dtype=np.float32)
    img.pixels.foreach_get(px)
    bpy.data.images.remove(img)
    return px.reshape(h, w, 4)[::-1]  # Blender's rows go bottom up


def write_png(path, arr):
    """uint8 (h, w, c): c = 1 grey, 3 rgb, 4 rgba; every row "Up" filtered"""
    h, w, c = arr.shape
    rows = arr.reshape(h, w * c)
    filt = np.empty_like(rows)
    filt[0] = rows[0]
    filt[1:] = rows[1:] - rows[:-1]  # uint8 wraps around, as PNG wants
    raw = np.concatenate([np.full((h, 1), 2, np.uint8), filt], axis=1)

    def chunk(tag, data):
        return struct.pack('>I', len(data)) + tag + data + struct.pack('>I', zlib.crc32(tag + data) & 0xffffffff)

    ihdr = struct.pack('>IIBBBBB', w, h, 8, {1: 0, 3: 2, 4: 6}[c], 0, 0, 0)
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', ihdr) + chunk(b'IDAT', zlib.compress(raw.tobytes(), 6)) + chunk(b'IEND', b''))


def rgbe(rgb):
    """linear float (h, w, 3) -> uint8 (h, w, 4); decodes as (m + 0.5) * 2^(e - 136)"""
    rgb = np.maximum(rgb, 0.0)
    m = rgb.max(axis=-1)
    lit = m > 1e-9
    e = np.zeros(m.shape, np.int32)
    e[lit] = np.floor(np.log2(m[lit])).astype(np.int32) + 1
    e = np.clip(e, -127, 127)
    mant = np.floor(rgb * np.ldexp(np.float32(256.0), -e)[..., None])
    out = np.empty(m.shape + (4,), np.uint8)
    out[..., :3] = np.clip(mant, 0, 255)
    out[..., 3] = (e + 128).astype(np.uint8)
    out[~lit] = 0
    return out


def shrink(px, maxsize):
    """halves until both sides fit, averaging 2x2 blocks"""
    while maxsize and max(px.shape[0], px.shape[1]) > maxsize and px.shape[0] % 2 == 0 and px.shape[1] % 2 == 0:
        h, w, c = px.shape
        px = px.reshape(h // 2, 2, w // 2, 2, c).mean(axis=(1, 3))
    return px


def grid(slices, cols):
    """(n, h, w, c) -> one image, slice k at column k % cols, row k // cols"""
    n, h, w, c = slices.shape
    rows = (n + cols - 1) // cols
    out = np.zeros((rows * h, cols * w, c), slices.dtype)
    for k in range(n):
        r, q = divmod(k, cols)
        out[r * h:(r + 1) * h, q * w:(q + 1) * w] = slices[k]
    return out


def to8(px):
    return np.clip(np.round(px * 255.0), 0, 255).astype(np.uint8)


args = sys.argv[sys.argv.index('--') + 1:]
job = json.load(open(args[0]))
stats = {}
for j in job['jobs']:
    t0 = time.time()
    op, dst = j['op'], j['dst']
    if op in ('rgbe', 'channel', 'rgba8'):
        px = shrink(load(j['src']), j.get('maxsize', 0))
        if op == 'rgbe':
            out = rgbe(px[..., :3])
            stats[dst] = {'mean': px[..., :3].reshape(-1, 3).mean(0).tolist(), 'max': float(px[..., :3].max())}
        elif op == 'channel':
            out = to8(px[..., j['channel']:j['channel'] + 1])
            stats[dst] = {'mean': float(px[..., j['channel']].mean())}
        else:
            out = to8(px)
    elif op in ('atlas_rgbe', 'atlas_channel'):
        sl = np.stack([load(p) for p in j['src']])
        if op == 'atlas_rgbe':
            out = grid(rgbe(sl[..., :3]), j['cols'])
            stats[dst] = {'mean': sl[..., :3].reshape(-1, 3).mean(0).tolist(), 'max': float(sl[..., :3].max())}
        else:
            out = grid(to8(sl[..., j['channel']:j['channel'] + 1]), j['cols'])
        stats.setdefault(dst, {})['slice'] = [int(sl.shape[2]), int(sl.shape[1]), int(sl.shape[0])]
    else:
        raise SystemExit(f'unknown op {op}')
    write_png(dst, out)
    stats.setdefault(dst, {}).update(size=[int(out.shape[1]), int(out.shape[0])], seconds=round(time.time() - t0, 1))
    print('lighttool:', op, dst, stats[dst], flush=True)
json.dump(stats, open(job['stats'], 'w'))
