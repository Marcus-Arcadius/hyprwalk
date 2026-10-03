import sys, os, glob
sys.path.insert(0, os.path.dirname(__file__))
import numpy as np
from ana import *
argv = sys.argv[sys.argv.index('--') + 1:]
files = []
for a in argv:
    files += sorted(glob.glob(a)) if any(c in a for c in '*?[') else [a]
for fn in files:
    sr, x = read_wav(fn)
    t, db, per, f0 = analyse(x, sr)
    if len(db) == 0:
        print(os.path.basename(fn), 'empty'); continue
    peak, floor = db.max(), np.percentile(db, 10)
    act = (db > max(peak - 30, floor + 10)) & (per > 0.6)
    f0v = f0[act & (f0 > 0)]
    med = np.median(f0v) if len(f0v) else 0
    # 20 ms envelope: digit = dB above floor / 6; unvoiced: '.' or a letter
    step = 4
    env = ''
    for i in range(0, len(db), step):
        v = int(max(0, min(9, (db[i] - floor) / 6)))
        env += str(v) if act[i] else ('.' if v < 2 else chr(ord('a') + v))
    print(f'{os.path.basename(fn)[:40]:40s} {len(x)/sr:5.2f}s peak {20*np.log10(np.abs(x).max()+1e-12):6.1f} dBFS floor {floor:6.1f} f0 {med:5.0f} Hz voiced {act.sum()*0.005:4.2f}s')
    print('   ' + env)
