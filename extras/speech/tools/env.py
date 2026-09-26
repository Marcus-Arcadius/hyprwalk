import sys, wave, array, math
for fn in sys.argv[1:]:
    w = wave.open(fn)
    sr, n, ch, sw = w.getframerate(), w.getnframes(), w.getnchannels(), w.getsampwidth()
    a = array.array('h', w.readframes(n))
    if ch > 1:
        a = array.array('h', [int(sum(a[i:i+ch])/ch) for i in range(0, len(a), ch)])
    hop = sr // 20  # 50 ms
    line = []
    peak = max(abs(x) for x in a) if a else 0
    for i in range(0, len(a), hop):
        seg = a[i:i+hop]
        r = math.sqrt(sum(x*x for x in seg)/len(seg)) if seg else 0
        db = 20*math.log10(r/32768) if r > 0 else -120
        line.append('%d' % max(0, min(9, int((db+70)/7))))
    print(f"{fn.split('/')[-1]}: {sr} Hz {n/sr:.2f}s peak {20*math.log10(peak/32768) if peak else -120:.1f} dBFS")
    print('  ' + ''.join(line))
