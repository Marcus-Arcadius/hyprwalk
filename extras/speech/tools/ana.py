# numpy helpers for the vowel survey; run inside Blender's Python (system python3 has no working numpy)
import numpy as np, wave, math

def read_wav(path):
    with wave.open(path) as w:
        sr, n, ch, sw = w.getframerate(), w.getnframes(), w.getnchannels(), w.getsampwidth()
        raw = w.readframes(n)
    if sw == 2:
        x = np.frombuffer(raw, dtype='<i2').astype(np.float64) / 32768.0
    elif sw == 4:
        x = np.frombuffer(raw, dtype='<i4').astype(np.float64) / 2147483648.0
    else:
        raise ValueError(f'{path}: {sw * 8}-bit')
    if ch > 1:
        x = x.reshape(-1, ch).mean(axis=1)
    return sr, x

def write_wav(path, sr, x):
    y = np.clip(np.round(np.asarray(x) * 32767.0), -32768, 32767).astype('<i2')
    with wave.open(path, 'wb') as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(sr); w.writeframes(y.tobytes())

def fft_filter(x, sr, lo=None, hi=None):
    """zero-phase brick-wall band limit (with a short cosine taper) through the FFT"""
    n = len(x)
    nfft = 1 << (n + 2048 - 1).bit_length()
    X = np.fft.rfft(x, nfft)
    f = np.fft.rfftfreq(nfft, 1 / sr)
    g = np.ones_like(f)
    if lo:
        g *= np.clip((f - lo * 0.7) / (lo * 0.3), 0, 1)
    if hi:
        g *= np.clip((hi * 1.1 - f) / (hi * 0.1), 0, 1)
    return np.fft.irfft(X * g, nfft)[:n]

def frame_matrix(x, N, H):
    n = max(0, 1 + (len(x) - N) // H)
    if n == 0:
        return np.zeros((0, N))
    idx = np.arange(N)[None, :] + H * np.arange(n)[:, None]
    return x[idx]

def analyse(x, sr, hop=0.005):
    """per 5 ms frame: rms dB (25 ms), periodicity and F0 (40 ms, 12 kHz), frame centre times"""
    xh = fft_filter(x, sr, lo=60)
    N, H = int(0.025 * sr), int(hop * sr)
    F = frame_matrix(xh, N, H)
    rms = np.sqrt((F ** 2).mean(axis=1) + 1e-20)
    db = 20 * np.log10(rms + 1e-12)
    t = (np.arange(len(db)) * H + N / 2) / sr
    # periodicity on a 12 kHz, <1.5 kHz copy
    d = int(round(sr / 12000)) or 1
    fs = sr / d
    xl = fft_filter(xh, sr, hi=1500)[::d]
    M, Hd = int(0.040 * fs), max(1, int(round(hop * fs)))
    G = frame_matrix(xl, M, Hd)
    lmin, lmax = int(fs / 500), int(fs / 60)
    r = np.zeros((len(G), lmax + 1))
    for lag in range(lmin, lmax + 1):
        a, b = G[:, :M - lag], G[:, lag:]
        r[:, lag] = (a * b).sum(1) / np.sqrt((a * a).sum(1) * (b * b).sum(1) + 1e-20)
    best = r[:, lmin:].max(axis=1)
    f0 = np.zeros(len(G))
    for i in range(len(G)):
        row = r[i]
        if best[i] <= 0:
            continue
        # the shortest lag that is nearly as good as the best: avoids sub-octave picks
        cand = np.nonzero(row[lmin:] >= 0.9 * best[i])[0]
        k = lmin + cand[0]
        # walk to the local peak
        while k + 1 <= lmax and row[k + 1] > row[k]:
            k += 1
        if lmin < k < lmax:
            y0, y1, y2 = row[k - 1], row[k], row[k + 1]
            den = y0 - 2 * y1 + y2
            k = k + (0.5 * (y0 - y2) / den if den != 0 else 0)
        f0[i] = fs / k
    m = min(len(db), len(best))
    return t[:m], db[:m], best[:m], f0[:m]

def lpc_formants(seg, sr, order=12, fs_target=10000):
    """rough F1/F2 per 25 ms frame (independent sanity check, not the lip sync's code)"""
    d = max(1, int(round(sr / fs_target)))
    fs = sr / d
    y = fft_filter(seg, sr, lo=60, hi=0.45 * fs)[::d]
    N, H = int(0.025 * fs), int(0.010 * fs)
    F = frame_matrix(y, N, H)
    out = []
    w = np.hamming(N)
    for fr in F:
        e = np.append(fr[0], fr[1:] - 0.97 * fr[:-1]) * w
        R = np.array([np.dot(e[:N - k], e[k:]) for k in range(order + 1)])
        if R[0] <= 1e-12:
            continue
        a = np.zeros(order + 1); a[0] = 1; E = R[0]
        for i in range(1, order + 1):
            k = -(R[i] + np.dot(a[1:i], R[i - 1:0:-1])) / E
            a[1:i + 1] = a[1:i + 1] + k * np.append(a[i - 1:0:-1], 1)
            E *= 1 - k * k
            if E <= 0:
                break
        roots = np.roots(a)
        roots = roots[np.imag(roots) > 0]
        fr_hz = np.angle(roots) * fs / (2 * np.pi)
        bw = -np.log(np.abs(roots)) * fs / np.pi
        ok = (fr_hz > 150) & (bw < 400) & (fr_hz < fs / 2 - 100)
        f = np.sort(fr_hz[ok])
        if len(f) >= 2:
            out.append((f[0], f[1]))
    return np.array(out)

def lowband_ratio(x, sr, n, hop=0.005, cut=1000):
    N, H = int(0.025 * sr), int(hop * sr)
    FL = frame_matrix(fft_filter(x, sr, lo=60, hi=cut), N, H)[:n]
    FA = frame_matrix(fft_filter(x, sr, lo=60), N, H)[:n]
    return (FL ** 2).sum(1) / ((FA ** 2).sum(1) + 1e-20)

def find_nuclei(x, sr, hop=0.005, dip=8.0, extent=12.0):
    """vowel nuclei: voiced runs split at dips >= dip dB; each spans the frames within extent dB of its peak"""
    t, db, per, f0 = analyse(x, sr, hop)
    n = len(t)
    ratio = lowband_ratio(x, sr, n, hop)
    floor, peak = np.percentile(db, 10), db.max()
    v = (db > max(floor + 12, peak - 40)) & (per > 0.3) & (ratio > 0.3)
    dbs = np.convolve(db, np.ones(3) / 3, mode='same')
    # voiced runs, gaps up to 2 frames bridged
    idx = np.nonzero(v)[0]
    runs = []
    for i in idx:
        if runs and i - runs[-1][1] <= 3:
            runs[-1][1] = i
        else:
            runs.append([i, i])
    parts = []
    for a, b in runs:
        # split at valleys: a fall of >= dip below the running max, then a rise of >= dip above the valley
        cuts, runmax, valley = [a], dbs[a], None
        for j in range(a, b + 1):
            if valley is None:
                if dbs[j] > runmax:
                    runmax = dbs[j]
                elif dbs[j] <= runmax - dip:
                    valley = j
            else:
                if dbs[j] < dbs[valley]:
                    valley = j
                if dbs[j] >= dbs[valley] + dip:
                    cuts.append(valley)
                    valley, runmax = None, dbs[j]
        # the peaks between cuts, left to right
        bounds = cuts + [b + 1]
        for s, e in zip(bounds[:-1], bounds[1:]):
            if e - s < 2:
                continue
            k = s + int(np.argmax(dbs[s:e]))
            p = dbs[k]
            lo, hi = k, k
            while lo > s and dbs[lo - 1] >= p - extent and v[lo - 1]:
                lo -= 1
            while hi < e - 1 and dbs[hi + 1] >= p - extent and v[hi + 1]:
                hi += 1
            if (hi - lo + 1) * hop >= 0.03:
                parts.append(dict(t0=float(t[lo] - hop / 2), t1=float(t[hi] + hop / 2), peak_db=float(p),
                                  f0=float(np.median(f0[lo:hi + 1][f0[lo:hi + 1] > 0])) if (f0[lo:hi + 1] > 0).any() else 0.0))
    # a nucleus has to reach within 25 dB of the loudest one
    top = max([p['peak_db'] for p in parts], default=0)
    return [p for p in parts if p['peak_db'] >= top - 25], (t, db, per, f0, v)
