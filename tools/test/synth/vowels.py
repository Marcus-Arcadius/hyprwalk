# vowels.py OUTDIR: sung vowels for testing lip sync (src/lipsync.cpp) from a source-filter model: a pulse train with a
# glottal tilt through formant resonators (Japanese a i u e o, a man's and a woman's), radiated from the lips. 0.8 s,
# 48 kHz 16-bit mono WAVs: man_a.wav ... woman_o.wav, silence.wav, hiss.wav (white noise, like an s), quiet_a.wav (a
# man's a, 40 dB down); and consonants between two a's (0.3 s each, 0.2 s of silence around): man_asa, _asha, _afa
# (noise shaped as s, sh, f) and _ama, _ana (nasal murmur: 250 Hz resonance, a dip near 1 kHz for m and 1.8 kHz for n,
# 12 dB down), likewise woman_*.
#   python3 vowels.py OUTDIR
import math, os, random, struct, sys, wave

RATE = 48000
VOICES = {  # pitch, then each vowel's first four formants (Hz)
    'man': (125, {'a': (750, 1180, 2600, 3500), 'i': (290, 2250, 3000, 3700), 'u': (340, 1300, 2300, 3400),
                  'e': (470, 1880, 2550, 3500), 'o': (490, 840, 2500, 3400)}),
    'woman': (225, {'a': (900, 1450, 2900, 4000), 'i': (340, 2750, 3300, 4200), 'u': (390, 1600, 2700, 3900),
                    'e': (560, 2280, 2900, 4000), 'o': (560, 1000, 2800, 3900)}),
}
BANDWIDTHS = (60, 90, 120, 150)


def resonator(xs, f, bw):
    r = math.exp(-math.pi * bw / RATE)
    c = 2 * r * math.cos(2 * math.pi * f / RATE)
    g = 1 - c + r * r  # unit gain at 0 Hz
    y1 = y2 = 0.0
    out = []
    for x in xs:
        y = g * x + c * y1 - r * r * y2
        out.append(y)
        y1, y2 = y, y1
    return out


def voice(f0, formants, seconds=0.8, gain=1.0):
    n = int(seconds * RATE)
    src, phase = [], 0.0
    for k in range(n):
        f = f0 * (1 + 0.01 * math.sin(2 * math.pi * 5.5 * k / RATE))  # a little vibrato
        phase += f / RATE
        if phase >= 1:
            phase -= 1
            src.append(1.0)
        else:
            src.append(0.0)
    # the glottis: two poles near 0 Hz (-12 dB an octave above 100 Hz or so)
    s = resonator(src, 0.0001, 200)
    for f, bw in zip(formants, BANDWIDTHS):
        s = resonator(s, f, bw)
    s = [b - a for a, b in zip([0.0] + s[:-1], s)]  # radiated from the lips
    peak = max(abs(x) for x in s) or 1.0
    ramp = int(0.03 * RATE)
    return [x / peak * 0.5 * gain * min(1.0, k / ramp, (n - 1 - k) / ramp) for k, x in enumerate(s)]


def biquad(xs, kind, fc, q):
    """RBJ's low-pass (0), high-pass (1), band-pass (2) or notch (3)"""
    w = 2 * math.pi * fc / RATE
    c, al = math.cos(w), math.sin(w) / (2 * q)
    b = {0: ((1 - c) / 2, 1 - c, (1 - c) / 2), 1: ((1 + c) / 2, -(1 + c), (1 + c) / 2), 2: (al, 0.0, -al), 3: (1.0, -2 * c, 1.0)}[kind]
    a0, a1, a2 = 1 + al, -2 * c, 1 - al
    b0, b1, b2 = (v / a0 for v in b)
    a1, a2 = a1 / a0, a2 / a0
    x1 = x2 = y1 = y2 = 0.0
    out = []
    for x in xs:
        y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2
        out.append(y)
        x2, x1, y2, y1 = x1, x, y1, y
    return out


def fricative(kind, seconds, level, rnd):
    """shaped noise at a peak level (0.5 = the vowels'): s, sh or f"""
    n = int(seconds * RATE)
    x = [rnd.gauss(0.0, 1.0) for _ in range(n)]
    if kind == 's':
        x = biquad(biquad(x, 1, 4000, 0.7), 2, 6500, 0.8)
    elif kind == 'sh':
        x = biquad(biquad(x, 1, 1500, 0.7), 2, 3000, 1.2)
    else:
        x = biquad(biquad(x, 1, 1000, 0.7), 0, 9000, 0.7)
    peak = max(abs(v) for v in x) or 1.0
    ramp = int(0.01 * RATE)
    return [v / peak * level * min(1.0, k / ramp, (n - 1 - k) / ramp) for k, v in enumerate(x)]


def nasal(f0, dip, seconds, gain):
    """nasal murmur: voice with a 250 Hz resonance, weaker ones above, and a notch at `dip`"""
    s = voice(f0, (250, 1100 if dip > 1400 else 1350, 2200, 3300), seconds, 1.0)
    s = biquad(s, 3, dip, 2.0)
    s = biquad(s, 0, 1200, 0.7)  # the nose damps the highs
    peak = max(abs(x) for x in s) or 1.0
    return [x / peak * 0.5 * gain for x in s]


def vcv(f0, vowel, middle):
    """a vowel, a consonant, the vowel: crossfaded over 10 ms each way"""
    a = voice(f0, vowel, 0.3)
    fade = int(0.01 * RATE)
    out = a[:-fade] + [x * (1 - k / fade) + middle[k] * k / fade for k, x in enumerate(a[-fade:])] + middle[fade:-fade]
    b = voice(f0, vowel, 0.3)
    return out + [middle[len(middle) - fade + k] * (1 - k / fade) + x * k / fade for k, x in enumerate(b[:fade])] + b[fade:]


def write(path, xs):
    with wave.open(path, 'wb') as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(b''.join(struct.pack('<h', max(-32767, min(32767, int(x * 32767)))) for x in xs))


out = sys.argv[1]
os.makedirs(out, exist_ok=True)
for who, (f0, table) in VOICES.items():
    for v, fs in table.items():
        write(os.path.join(out, '%s_%s.wav' % (who, v)), voice(f0, fs))
write(os.path.join(out, 'silence.wav'), [0.0] * int(0.8 * RATE))
rnd = random.Random(1)
write(os.path.join(out, 'hiss.wav'), [rnd.uniform(-0.2, 0.2) for _ in range(int(0.8 * RATE))])
write(os.path.join(out, 'quiet_a.wav'), voice(125, VOICES['man'][1]['a'], gain=0.01))
n = 0
for who, (f0, table) in VOICES.items():
    rnd = random.Random(2 if who == 'man' else 3)
    for name, middle in (('asa', fricative('s', 0.16, 0.25, rnd)), ('asha', fricative('sh', 0.16, 0.25, rnd)),
                         ('afa', fricative('f', 0.14, 0.04, rnd)), ('ama', nasal(f0, 1000, 0.12, 0.25)),
                         ('ana', nasal(f0, 1800, 0.12, 0.25))):
        quiet = [0.0] * int(0.2 * RATE)
        write(os.path.join(out, '%s_%s.wav' % (who, name)), quiet + vcv(f0, table['a'], middle) + quiet)
        n += 1
print('wrote %d files to %s' % (len(VOICES) * 5 + 3 + n, out))
