# vowels.py OUTDIR: sung vowels for testing lip sync (src/lipsync.cpp), made by a source-filter model: a pulse train at
# the voice's pitch, tilted like a glottis, through resonators at the vowel's formants (Japanese a, i, u, e, o; a man's
# and a woman's, from the usual measurements), then as radiated from the lips. 0.8 s each, 48 kHz 16-bit mono WAVs:
# man_a.wav ... woman_o.wav, and silence.wav, hiss.wav (white noise, as an s is), quiet_a.wav (a man's a, 40 dB down)
#   python3 vowels.py OUTDIR
import math, os, random, struct, sys, wave

RATE = 48000
VOICES = {  # pitch (Hz), then per vowel its first four formants (Hz)
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
print('wrote %d files to %s' % (len(VOICES) * 5 + 3, out))
