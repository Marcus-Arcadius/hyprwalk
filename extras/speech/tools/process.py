# converts, segments and loops the candidates; run inside Blender's Python:
#   blender -b --factory-startup --python-exit-code 1 -P tools/process.py -- tools/cands.json tools/results.json [ids...]
import sys, os, json, subprocess, math
sys.path.insert(0, os.path.dirname(__file__))
import numpy as np
from ana import *

SP = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
argv = sys.argv[sys.argv.index('--') + 1:]
cands = json.load(open(argv[0]))
out_json = argv[1]
only = set(argv[2:])
SR = 48000
HOP = 0.005
LOOP_MIN = 5.0
FADE = 0.005

def probe(path):
    r = subprocess.run(['ffprobe', '-v', 'error', '-show_entries',
                        'stream=codec_name,sample_rate,channels,bit_rate,bits_per_sample,bits_per_raw_sample,sample_fmt:format=duration,bit_rate,format_name',
                        '-of', 'json', path], capture_output=True, text=True, check=True)
    j = json.loads(r.stdout)
    s = j['streams'][0]; f = j['format']
    return dict(container=f.get('format_name'), codec=s.get('codec_name'), sample_rate=int(s.get('sample_rate', 0)),
                channels=int(s.get('channels', 0)), sample_fmt=s.get('sample_fmt'),
                bits_per_sample=int(s.get('bits_per_raw_sample') or s.get('bits_per_sample') or 0) or None,
                stream_bit_rate=int(s['bit_rate']) if s.get('bit_rate') else None,
                avg_bit_rate=int(f['bit_rate']) if f.get('bit_rate') else None,
                duration_s=round(float(f.get('duration', 0)), 3))

def decode_native(path, ch):
    r = subprocess.run(['ffmpeg', '-nostdin', '-v', 'error', '-i', path, '-f', 'f32le', '-acodec', 'pcm_f32le', '-'],
                       capture_output=True, check=True)
    x = np.frombuffer(r.stdout, dtype='<f4').astype(np.float64)
    return x.reshape(-1, ch) if ch > 1 else x[:, None]

def to_wav(path, dst):
    subprocess.run(['ffmpeg', '-nostdin', '-v', 'error', '-y', '-i', path, '-ac', '1', '-ar', str(SR), '-c:a', 'pcm_s16le',
                    '-map_metadata', '-1', '-fflags', '+bitexact', '-flags:a', '+bitexact', dst], check=True)

def runs(mask, gap=3, minlen=8):
    idx = np.nonzero(mask)[0]
    if len(idx) == 0:
        return []
    out = [[idx[0], idx[0]]]
    for i in idx[1:]:
        if i - out[-1][1] <= gap + 1:
            out[-1][1] = i
        else:
            out.append([i, i])
    return [(a, b) for a, b in out if b - a + 1 >= minlen]

def segment(x, sr, set_):
    t, db, per, f0 = analyse(x, sr, HOP)
    n = len(t)
    N = int(0.025 * sr); H = int(HOP * sr)
    xl = fft_filter(x, sr, lo=60, hi=1000)
    xh = fft_filter(x, sr, lo=60)
    FL = frame_matrix(xl, N, H)[:n]; FA = frame_matrix(xh, N, H)[:n]
    ratio = (FL ** 2).sum(1) / ((FA ** 2).sum(1) + 1e-20)
    peak, floor = db.max(), np.percentile(db, 10)
    thr = max(peak - 25, min(floor + 15, peak - 12))  # short files have little silence: the floor estimate runs high
    cand = (db > thr) & (ratio > 0.3) & (per > 0.3)
    rs = runs(cand)
    if set_ == 'word':
        rs = rs[:1]
    else:
        rs = [r for r in rs if (r[1] - r[0] + 1) * HOP >= 0.05]
    segs = []
    # trim the edges: onsets/offsets; a word's vowel loses more at its end (the move into the stop closure)
    trim0, trim1 = (0.010, 0.010) if set_ == 'isolated' else (0.005, 0.020)
    for a, b in rs:
        m = db[a:b + 1].max()
        # the contiguous core within 20 dB of the run's maximum, around it
        k = a + int(np.argmax(db[a:b + 1]))
        lo = k
        while lo > a and db[lo - 1] >= m - 20:
            lo -= 1
        hi = k
        while hi < b and db[hi + 1] >= m - 20:
            hi += 1
        s0 = t[lo] - HOP / 2 + trim0
        s1 = t[hi] + HOP / 2 - trim1
        if s1 - s0 < 0.04:  # keep at least 40 ms
            c = (s0 + s1) / 2; s0, s1 = c - 0.02, c + 0.02
        segs.append((max(0.0, s0), min(len(x) / sr, s1)))
    info = dict(peak_frame_db=float(peak), floor_db=float(floor), thr_db=float(thr))
    return segs, (t, db, per, f0, cand), info

def envelope(t, db, per, cand, segs, floor):
    s, m = '', ''
    for i in range(0, len(t), 4):
        inseg = any(a <= t[i] <= b for a, b in segs)
        v = int(max(0, min(9, (db[i] - floor) / 6)))
        s += str(v) if cand[i] else ('.' if v < 2 else chr(ord('a') + v))
        m += '^' if inseg else ' '
    return s + '\n    ' + m

def make_loop(x, sr, segs):
    fade = int(FADE * sr)
    w = 0.5 - 0.5 * np.cos(np.pi * np.arange(fade) / fade)
    parts = []
    for a, b in segs:
        p = x[int(round(a * sr)):int(round(b * sr))].copy()
        if len(p) > 2 * fade:
            p[:fade] *= w; p[-fade:] *= w[::-1]
        parts.append(p)
    cyc = np.concatenate(parts)
    reps = max(1, math.ceil(LOOP_MIN * sr / len(cyc)))
    return np.tile(cyc, reps), reps, len(cyc) / sr

results = {}
if os.path.exists(out_json):
    results = json.load(open(out_json))
for c in cands:
    if only and c['id'] not in only:
        continue
    raw = os.path.join(SP, c['raw'])
    if not os.path.exists(raw):
        print('MISSING', c['id'], raw); continue
    pr = probe(raw)
    nat = decode_native(raw, pr['channels'])
    clip = dict(max_abs=float(np.abs(nat).max()), n_full_scale=int((np.abs(nat) >= 0.999).sum()),
                channel_corr=float(np.corrcoef(nat[:, 0], nat[:, 1])[0, 1]) if nat.shape[1] > 1 and nat[:, 0].std() > 0 and nat[:, 1].std() > 0 else None)
    wav = os.path.join(SP, 'wav', c['id'] + '.wav')
    to_wav(raw, wav)
    sr, x = read_wav(wav)
    if c['set'] == 'word':
        nu, (t, db, per, f0, cand) = find_nuclei(x, sr, HOP)
        take = c.get('take') or [0]
        if max(take) >= len(nu):
            print('!! not enough nuclei', c['id'], len(nu)); continue
        segs = []
        for k in take:
            n = nu[k]
            # a word-initial vowel has no consonant transition at its start; later ones follow a stop release
            s0 = n['t0'] + (0.005 if k == 0 and c['reading'][0] in 'あいうえおアイウエオ' else 0.015)
            s1 = n['t1'] - 0.015
            segs.append((s0, s1))
        sinfo = dict(floor_db=float(np.percentile(db, 10)))
        nuclei_all = [[round(n['t0'], 3), round(n['t1'], 3)] for n in nu]
    else:
        segs, (t, db, per, f0, cand), sinfo = segment(x, sr, c['set'])
        nuclei_all = None
    # stats over the chosen segments
    inseg = np.zeros(len(t), bool)
    for a, b in segs:
        inseg |= (t >= a) & (t <= b)
    f0s = f0[inseg & (per > 0.3) & (f0 > 0)]
    # the whole file's voiced frames, for a sex guess that doesn't hang on one syllable's pitch accent
    f0all = f0[(db > db.max() - 25) & (per > 0.5) & (f0 > 0)]
    # noise floor: the 10th percentile of the frame levels when the file has enough quiet in it, else its quietest 15 ms
    if (db < db.max() - 30).mean() >= 0.2:
        floor_db = float(np.percentile(db, 10))
    else:
        floor_db = float(np.convolve(db, np.ones(3) / 3, mode='valid').min())
    vox = np.concatenate([x[int(a * sr):int(b * sr)] for a, b in segs])
    fm = lpc_formants(vox, sr)
    loop, reps, cyc = make_loop(x, sr, segs)
    lp = os.path.join(SP, 'in', c['id'] + '.wav')
    write_wav(lp, sr, loop)
    r = dict(probe=pr, clip=clip, wav=f'wav/{c["id"]}.wav', looped=f'in/{c["id"]}.wav',
             duration_s=round(len(x) / sr, 3), voiced_s=round(sum(b - a for a, b in segs), 3),
             segments_s=[[round(a, 3), round(b, 3)] for a, b in segs], loop_s=round(len(loop) / sr, 3), loop_repeats=reps, cycle_s=round(cyc, 3),
             f0_median_hz=round(float(np.median(f0s)), 1) if len(f0s) else None,
             f0_p10_p90_hz=[round(float(np.percentile(f0s, 10)), 1), round(float(np.percentile(f0s, 90)), 1)] if len(f0s) else None,
             rough_f1_f2_hz=[round(float(np.median(fm[:, 0]))), round(float(np.median(fm[:, 1])))] if len(fm) else None,
             peak_dbfs=round(20 * math.log10(np.abs(x).max() + 1e-12), 1),
             voiced_rms_dbfs=round(20 * math.log10(np.sqrt((vox ** 2).mean()) + 1e-12), 1),
             noise_floor_dbfs=round(floor_db, 1),
             f0_file_median_hz=round(float(np.median(f0all)), 1) if len(f0all) else None,
             f0_file_p10_hz=round(float(np.percentile(f0all, 10)), 1) if len(f0all) else None)
    r['snr_db'] = round(r['voiced_rms_dbfs'] - r['noise_floor_dbfs'], 1)
    r['nuclei_s'] = nuclei_all
    r['take'] = c.get('take')
    results[c['id']] = r
    print(f"{c['id']:26s} {pr['codec']:9s} {pr['sample_rate']:6d} ch{pr['channels']} {str(pr['stream_bit_rate'] or pr['avg_bit_rate']):>7s}  dur {r['duration_s']:5.2f} voiced {r['voiced_s']:5.3f} ({len(segs)} seg) f0 {r['f0_median_hz']} F1/F2 {r['rough_f1_f2_hz']} peak {r['peak_dbfs']} rms {r['voiced_rms_dbfs']} floor {r['noise_floor_dbfs']} clip {clip['n_full_scale']}")
    print('    ' + envelope(t, db, per, cand, segs, sinfo['floor_db']))
json.dump(results, open(out_json, 'w'), ensure_ascii=False, indent=1)
