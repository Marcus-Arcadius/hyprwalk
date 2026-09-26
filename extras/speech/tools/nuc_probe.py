import sys, os, json
sys.path.insert(0, os.path.dirname(__file__))
from ana import *
argv = sys.argv[sys.argv.index('--') + 1:]
for fn in argv:
    sr, x = read_wav(fn)
    nu, (t, db, per, f0, v) = find_nuclei(x, sr)
    fl = np.percentile(db, 10)
    env = ''.join((str(int(max(0, min(9, (db[i] - fl) / 6)))) if v[i] else '.') for i in range(0, len(t), 4))
    print(os.path.basename(fn))
    print('   ' + env)
    print('   ' + ''.join(('^' if any(n['t0'] <= t[i] <= n['t1'] for n in nu) else ' ') for i in range(0, len(t), 4)))
    for k, n in enumerate(nu):
        print(f"   #{k}: {n['t0']*1000:5.0f}-{n['t1']*1000:5.0f} ms ({(n['t1']-n['t0'])*1000:4.0f} ms) peak {n['peak_db']:6.1f} dB f0 {n['f0']:5.0f}")
