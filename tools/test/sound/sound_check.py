# sound_check.py: sound_check.sh's comparisons, run in Blender for its numpy:
#   blender -b --factory-startup --python-exit-code 1 -P sound_check.py -- CASES.jsonl
# A case a line: "decode" (ours against ffmpeg's or libvorbis's decoding), "exact" (what PipeWire got from the speaker,
# sample by sample), "level" (resampled: loudness and length), "unlinked" (never heard: no clock).
# Prints "ok ..." or "FAIL ..." per check, then "done".
import json, sys

import numpy as np

RATE = 48000
RAMP = 0.02  # as in src/speaker.cpp: s for gain 0 to 1
fails = 0


def check(what, good, detail=''):
    global fails
    print('%s %s%s' % ('ok  ' if good else 'FAIL', what, ': ' + detail if detail else ''), flush=True)
    fails += 0 if good else 1


def read_log(path):
    rows = []
    for line in open(path, encoding='utf-8'):
        t, at, stream, pos, start, latency, frames = line.split()
        rows.append((float(t), float(at), stream, float(pos), float(start), float(latency), int(frames)))
    return rows


def decode_case(c):
    ours, ref = np.fromfile(c['ours'], np.int16), np.fromfile(c['ref'], np.int16)
    ch = int(c['said'].split()[3]) if c['said'].startswith('rate') else 1
    frames = len(ours) // ch
    check('%s: as long as the file says' % c['name'], frames == c['frames'],
          '%d frames (it says %d; %s decodes %d)' % (frames, c['frames'], c['name'].split('(')[-1].rstrip(')'), len(ref) // ch))
    n = min(len(ours), len(ref))
    d = np.abs(ours[:n].astype(np.int32) - ref[:n].astype(np.int32))
    check('%s: the same samples, but for rounding' % c['name'], n > 0 and d.max() <= 2,
          'at most %d apart (16 bit), %.3f on average' % (d.max(), d.mean()) if n else 'nothing')


def stopped_status(c):
    return json.loads(c['said'][0]) if c['said'] and c['said'][0].startswith('{') else None


def clock_checks(c, rows):
    name, heard = c['name'], [r for r in rows if r[1] >= 0]
    check('%s: a clock once heard' % name, len(heard) > 10, '%d of %d log lines' % (len(heard), len(rows)))
    if len(heard) <= 10:
        return
    t = np.array([r[0] for r in heard])
    at = np.array([r[1] for r in heard])
    back = np.diff(at).min()
    check('%s: the clock never goes back' % name, back >= 0, 'its least step %.6f s' % back)
    # clock = the dance's time since its start; sound_test's t starts as play() returns, and log lines can be late
    lead = at - (c['from'] + t)
    check('%s: the clock is where the dance is by its own time' % name, -0.005 < lead.min() and lead.max() < 0.03,
          'ahead by %.1f to %.1f ms' % (lead.min() * 1000, lead.max() * 1000))
    slope, icept = np.polyfit(t, at, 1)
    jitter = np.abs(at - (slope * t + icept)).max()
    check('%s: ... going at its rate, smoothly' % name, abs(slope - 1) < 0.005 and jitter < 0.004,
          '%.5f s a second, %.2f ms off a straight line at most' % (slope, jitter * 1000))


def exact_case(c):
    name, ch, vol = c['name'], c['channels'], c['volume']
    dec = np.fromfile(c['decoded'], np.int16).reshape(-1, ch)
    rec = np.fromfile(c['rec'], np.float32)
    rec = rec[: len(rec) // ch * ch].reshape(-1, ch)
    rows = read_log(c['log'])
    st = stopped_status(c)
    check('%s: heard and closed' % name, c['status'] == 0 and st is not None, ' / '.join(c['said']))
    if st is None or not len(rec):
        check('%s: something recorded' % name, len(rec) > 0, '%d frames' % len(rec))
        return
    clock_checks(c, rows)
    F = len(dec)
    first = int(round(st['start'] * RATE))
    want_first = c['from'] * RATE
    check('%s: came in where the dance was by then' % name, want_first <= first < want_first + 0.25 * RATE,
          '%.4f s in (asked from %.2f s)' % (first / RATE, c['from']))

    # expected: the gain rising a step a frame to the volume, in floats as the speaker does it
    n = len(rec) + RATE
    idx = first + np.arange(n)
    if c['loop']:
        idx %= F
    gain = np.empty(n, np.float32)
    g, step, v = np.float32(0), np.float32(1 / (RAMP * RATE)), np.float32(vol)
    for i in range(n):
        g = min(v, np.float32(g + step)) if g < v else g
        gain[i] = g
        if g == v:
            gain[i:] = v
            break
    inside = idx < F
    want = np.zeros((n, ch), np.float32)
    want[inside] = dec[idx[inside]].astype(np.float32) * (gain[inside] / np.float32(32768))[:, None]

    # find the start in the recording: the best match within 64 frames of its first sound
    nz = np.flatnonzero(np.abs(rec).max(axis=1) > 0)
    if not len(nz):
        check('%s: something but silence recorded' % name, False)
        return
    w0 = np.flatnonzero(np.abs(want).max(axis=1) > 0)[0]
    best, k0 = None, 0
    for k in range(max(0, nz[0] - w0 - 64), nz[0] - w0 + 65):
        m = min(4096, len(rec) - k)
        e = np.abs(rec[k: k + m] - want[:m]).max()
        if best is None or e < best:
            best, k0 = e, k
    got = rec[k0:]
    m = min(len(got), len(want))
    err = np.abs(got[:m] - want[:m]).max(axis=1)
    bad = np.flatnonzero(err > 1e-6)
    same = bad[0] if len(bad) else m  # where it fades out, ends or stops
    played = same / RATE
    if not c['loop'] and first + same >= F:
        check('%s: exactly what it should be, to the sound\'s end' % name, first + same == F or (len(bad) == 0),
              '%.4f s of it the same, the sample for sample' % played)
        tail = np.abs(got[same:]).max() if same < len(got) else 0.0
        check('%s: ... then silence' % name, tail == 0, 'at most %.2g after' % tail)
        return
    check('%s: exactly what it should be, till it stopped' % name, played >= c['for'] - 0.02,
          '%.4f s the same, the sample for sample (asked to play %.2f s); %s' % (
              played, c['for'], 'round %d times' % ((first + same) // F) if c['loop'] else 'once'))
    if c['loop'] and c['for'] + c['from'] > F / RATE:
        check('%s: ... round its end to its start and on' % name, (first + same) // F >= 1,
              'from %.3f s to %.3f s on its time line, %.3f s long' % (first / RATE, (first + same) / RATE, F / RATE))
    # fading out: the gain down by a step a frame from the volume, then silence
    fade = c['fade'] * RATE
    ref = want[same: same + int(fade) + 2 * 1024]
    out = got[same: same + len(ref)]
    ref = ref[: len(out)]
    loud = np.abs(ref).max(axis=1) > 0.05 * max(vol, 1e-3)
    ratio = np.where(loud, np.abs(out).max(axis=1) / np.maximum(np.abs(ref).max(axis=1), 1e-9), np.nan)
    j = np.arange(len(ref))
    line = np.clip(1 - (j + 1) / fade, 0, 1)
    dev = np.nanmax(np.abs(ratio - line)) if np.any(loud) else 0.0
    quiet = np.flatnonzero(np.abs(got[same:]).max(axis=1) > 0)
    length = (quiet[-1] + 1) / RATE if len(quiet) else 0.0
    check('%s: faded out over %.2f s, evenly' % (name, c['fade']), abs(length - c['fade']) < 0.003 and dev < 0.01,
          'sound for %.4f s after, %.4f off the line at most' % (length, dev))
    rest = np.abs(got[same + int(fade) + 2:]).max() if len(got) > same + int(fade) + 2 else 0.0
    check('%s: ... then silence' % name, rest == 0, 'at most %.2g after' % rest)


def level_case(c):
    name, ch = c['name'], c['channels']
    dec = np.fromfile(c['decoded'], np.int16).reshape(-1, ch).astype(np.float64) / 32768
    rec = np.fromfile(c['rec'], np.float32)
    rec = rec[: len(rec) // ch * ch].reshape(-1, ch).astype(np.float64)
    st = stopped_status(c)
    check('%s: heard and closed' % name, c['status'] == 0 and st is not None, ' / '.join(c['said']))
    clock_checks(c, read_log(c['log']))
    nz = np.flatnonzero(np.abs(rec).max(axis=1) > 0)
    length = (nz[-1] - nz[0] + 1) / RATE if len(nz) else 0.0
    check('%s: played as long as asked (and the fade)' % name, abs(length - (c['for'] + c['fade'])) < 0.06,
          '%.3f s of sound' % length)
    if len(nz) > RATE // 2:
        a, b = nz[0] + RATE // 10, nz[0] + RATE // 10 + RATE // 2
        got = np.sqrt(np.mean(rec[a:b] ** 2, axis=0))
        want = np.sqrt(np.mean(dec[: len(dec) * 3 // 4] ** 2, axis=0)) * c['volume']
        db = 20 * np.log10(got / want)
        check('%s: as loud as it is, each channel' % name, np.all(np.abs(db) < 0.5), ', '.join('%+.2f dB' % x for x in db))


def unlinked_case(c):
    name, rows = c['name'], read_log(c['log'])
    check('%s: never heard, no clock' % name, all(r[1] < 0 for r in rows) and c['status'] == 1, ' / '.join(c['said']))
    check('%s: ... and it closed' % name, any('closed at' in s for s in c['said']))
    states = sorted({r[2] for r in rows})
    check('%s: ... its stream waiting, not in error' % name, 'error' not in states, ', '.join(states))


argv = sys.argv[sys.argv.index('--') + 1:] if '--' in sys.argv else sys.argv[1:]
for line in open(argv[0], encoding='utf-8'):
    case = json.loads(line)
    {'decode': decode_case, 'exact': exact_case, 'level': level_case, 'unlinked': unlinked_case}[case['kind']](case)
print('%d failed' % fails)
print('done')
