# attenuate.py OUT.wav DB PART... [--noise DBFS] [--seed N]: a recording as a quieter microphone would give it, for lip
# sync's tests: the parts one after the other (WAVs, mono or their channels mixed, and silence:SECONDS), DB decibels
# down, as a 32-bit float WAV (no rounding to 16 bits, however far down). --noise adds white noise all along at that
# level (dBFS RMS, a full scale sine 0 dBFS as lip sync measures it): a microphone's own hiss and the room's, which
# don't get quieter with the voice. Put silence:1 first for what lip sync hears before you speak.
#   python3 attenuate.py a-30.wav 30 silence:1 man_a.wav --noise -75
#   python3 attenuate.py whisper.wav 0 silence:1 man_a.wav quiet_a.wav
import random, struct, sys


def read(path):
    """mono floats and the rate, from 16/24/32-bit PCM or 32-bit float WAVs"""
    data = open(path, 'rb').read()
    pos, fmt, ch, rate, bits, out = 12, 1, 1, 48000, 16, []
    while pos + 8 <= len(data):
        cid, size = data[pos:pos + 4], struct.unpack('<I', data[pos + 4:pos + 8])[0]
        body = data[pos + 8:pos + 8 + size]
        if cid == b'fmt ':
            fmt, ch, rate = struct.unpack('<HHI', body[:8])
            bits = struct.unpack('<H', body[14:16])[0]
            if fmt == 0xFFFE:
                fmt = struct.unpack('<H', body[24:26])[0]
        elif cid == b'data':
            width = bits // 8
            n = len(body) // (width * ch)
            if fmt == 3 and bits == 32:
                vals = struct.unpack('<%df' % (n * ch), body[:n * ch * 4])
            elif bits == 16:
                vals = [v / 32768 for v in struct.unpack('<%dh' % (n * ch), body[:n * ch * 2])]
            elif bits == 32:
                vals = [v / 2147483648 for v in struct.unpack('<%di' % (n * ch), body[:n * ch * 4])]
            else:  # 24-bit
                vals = [int.from_bytes(body[k * 3:k * 3 + 3], 'little', signed=True) / 8388608 for k in range(n * ch)]
            out += [sum(vals[k * ch:(k + 1) * ch]) / ch for k in range(n)]
        pos += 8 + size + (size & 1)
    return out, rate


def write(path, xs, rate):
    body = struct.pack('<%df' % len(xs), *xs)
    fmt = struct.pack('<HHIIHH', 3, 1, rate, rate * 4, 4, 32)
    with open(path, 'wb') as f:
        f.write(b'RIFF' + struct.pack('<I', 4 + 8 + len(fmt) + 8 + len(body)) + b'WAVE')
        f.write(b'fmt ' + struct.pack('<I', len(fmt)) + fmt + b'data' + struct.pack('<I', len(body)) + body)


def main(argv):
    args, opts, i = [], {'noise': None, 'seed': 1}, 0
    while i < len(argv):
        if argv[i].startswith('--'):
            opts[argv[i][2:]] = float(argv[i + 1])
            i += 2
        else:
            args.append(argv[i])
            i += 1
    dst, db, parts = args[0], float(args[1]), args[2:]
    xs, rate, pending = [], None, 0.0
    for p in parts:  # (silence waits for the rate of the first WAV)
        if p.startswith('silence:'):
            if rate:
                xs += [0.0] * int(float(p[8:]) * rate)
            else:
                pending += float(p[8:])
            continue
        v, r = read(p)
        if rate is None:
            rate = r
            xs = [0.0] * int(pending * rate)
        elif r != rate:
            raise SystemExit(f'{p}: {r} Hz, not {rate} Hz like the parts before it')
        xs += v
    rate = rate or 48000
    if not parts or all(p.startswith('silence:') for p in parts):
        xs = [0.0] * int(pending * rate)
    g = 10 ** (-db / 20)
    xs = [x * g for x in xs]
    if opts['noise'] is not None:
        rnd = random.Random(int(opts['seed']))
        sd = 10 ** ((opts['noise'] - 3.01) / 20)  # (RMS: the level lip sync gives a full scale sine as 0 dBFS)
        xs = [x + rnd.gauss(0.0, sd) for x in xs]
    write(dst, xs, rate)


if __name__ == '__main__':
    main(sys.argv[1:])
