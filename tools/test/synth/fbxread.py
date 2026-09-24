# fbxread.py FILE.fbx: prints a binary FBX's model hierarchy (a debugging aid; plain python3)
import struct, zlib, sys

def read_fbx(path):
    data = open(path, 'rb').read()
    if not data.startswith(b'Kaydara FBX Binary  \x00'):
        raise ValueError('not a binary FBX')
    version = struct.unpack_from('<I', data, 23)[0]
    wide = version >= 7500
    pos = 27

    def prop(pos):
        t = chr(data[pos]); pos += 1
        if t == 'Y': return struct.unpack_from('<h', data, pos)[0], pos + 2
        if t == 'C': return bool(data[pos]), pos + 1
        if t == 'I': return struct.unpack_from('<i', data, pos)[0], pos + 4
        if t == 'F': return struct.unpack_from('<f', data, pos)[0], pos + 4
        if t == 'D': return struct.unpack_from('<d', data, pos)[0], pos + 8
        if t == 'L': return struct.unpack_from('<q', data, pos)[0], pos + 8
        if t in 'fdlib':
            n, enc, clen = struct.unpack_from('<III', data, pos); pos += 12
            raw = data[pos:pos + clen]; pos += clen
            if enc == 1: raw = zlib.decompress(raw)
            fmt = {'f': 'f', 'd': 'd', 'l': 'q', 'i': 'i', 'b': 'B'}[t]
            return list(struct.unpack('<%d%s' % (n, fmt), raw[:n * struct.calcsize(fmt)])), pos
        if t in 'SR':
            n = struct.unpack_from('<I', data, pos)[0]; pos += 4
            raw = data[pos:pos + n]; pos += n
            return (raw if t == 'R' else raw.decode('utf-8', 'replace')), pos
        raise ValueError('bad property type %r at %d' % (t, pos - 1))

    def node(pos):
        if wide:
            end, nprops, plen = struct.unpack_from('<QQQ', data, pos); pos += 24
        else:
            end, nprops, plen = struct.unpack_from('<III', data, pos); pos += 12
        if end == 0:
            return None, pos + 1
        nlen = data[pos]; pos += 1
        name = data[pos:pos + nlen].decode(); pos += nlen
        props = []
        for _ in range(nprops):
            v, pos = prop(pos)
            props.append(v)
        kids = []
        while pos < end:
            k, pos = node(pos)
            if k is None:
                break
            kids.append(k)
        return (name, props, kids), end

    top = []
    while pos < len(data) - 13:
        n, pos = node(pos)
        if n is None:
            break
        top.append(n)
    return version, top

def child(n, name):
    for k in n[2]:
        if k[0] == name:
            return k
    return None

def models(path):
    """[(id, name, type, parent id)] of the Model nodes, parent 0 = the scene root"""
    version, top = read_fbx(path)
    objs = next(n for n in top if n[0] == 'Objects')
    conns = next(n for n in top if n[0] == 'Connections')
    out = {}
    for n in objs[2]:
        if n[0] == 'Model':
            nm = n[1][1].split('\x00\x01')[0]
            out[n[1][0]] = [nm, n[1][2], 0]
    for c in conns[2]:
        # a bone is also linked to its skin cluster: only a model (or 0, the scene) is a parent
        if c[0] == 'C' and c[1][0] == 'OO' and c[1][1] in out and (c[1][2] == 0 or c[1][2] in out):
            out[c[1][1]][2] = c[1][2]
    return version, out

if __name__ == '__main__':
    version, ms = models(sys.argv[1])
    print('FBX', version, len(ms), 'models')
    kids = {}
    for i, (nm, t, p) in ms.items():
        kids.setdefault(p, []).append(i)
    def show(p, d):
        for i in kids.get(p, []):
            print('  ' * d + ms[i][0], '(' + ms[i][1] + ')')
            show(i, d + 1)
    show(0, 0)
