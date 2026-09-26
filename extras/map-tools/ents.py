# parses VRF's decompiled .vents dump into a list of dicts
import re, sys, json
def parse(path):
    ents, cur = [], None
    for line in open(path, encoding="utf-8", errors="replace"):
        line = line.rstrip("\n")
        if re.match(r"^====\d+====$", line):
            cur = {}; ents.append(cur); continue
        if cur is None or not line.strip(): continue
        m = re.match(r"^(\S+)\s+(.*)$", line)
        if not m: continue
        k, v = m.group(1), m.group(2).strip()
        if v.startswith('"') and v.endswith('"'): v = v[1:-1]
        elif v.startswith("["):
            try: v = [float(x) for x in v.strip("[] ").split(",")]
            except ValueError: pass
        cur[k] = v
    return ents
if __name__ == "__main__":
    ents = parse(sys.argv[1])
    import collections
    c = collections.Counter(e.get("classname") for e in ents)
    for k, n in c.most_common(): print(n, k)
