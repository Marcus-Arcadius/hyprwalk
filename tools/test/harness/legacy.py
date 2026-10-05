# legacy.py DIR: for legacy_check.sh, from toonballs.py's ToonBalls.glb and litmap.py's LitCourt.glb in DIR: DIR/new and
# DIR/old, the same files with today's names and with the names they had when hyprwalk was hypr3d. Balls.glb tags the
# PlainMatcap ball as part "Gone" (extras' hyprwalk_part) and its settings file (Balls.hyprwalk.json, old:
# Balls.hypr3d.json) hides it; LitCourt.glb's markers and extensions become hypr3d_* and HYPR3D_*.
import json, os, struct, sys


def read(path):
    b = open(path, 'rb').read()
    magic, _, _ = struct.unpack_from('<4sII', b, 0)
    assert magic == b'glTF', path
    jlen, _ = struct.unpack_from('<II', b, 12)
    js = json.loads(b[20:20 + jlen])
    return js, b[20 + jlen:]  # the BIN chunk as it is, header and all


def write(path, js, rest):
    j = json.dumps(js, separators=(',', ':')).encode()
    j += b' ' * (-len(j) % 4)
    open(path, 'wb').write(struct.pack('<4sII', b'glTF', 2, 12 + 8 + len(j) + len(rest)) + struct.pack('<II', len(j), 0x4E4F534A)
                           + j + rest)


def old(js):
    """the names from before the rename"""
    return json.loads(json.dumps(js).replace('hyprwalk_', 'hypr3d_').replace('HYPRWALK_', 'HYPR3D_'))


def main(d):
    for sub in ('new', 'old'):
        os.makedirs(os.path.join(d, sub), exist_ok=True)

    js, rest = read(os.path.join(d, 'ToonBalls.glb'))
    mat = next(i for i, m in enumerate(js['materials']) if m['name'] == 'PlainMatcap')
    for mesh in js['meshes']:
        for prim in mesh['primitives']:
            if prim.get('material') == mat:
                prim['extras'] = {'hyprwalk_part': 'Gone'}
    write(os.path.join(d, 'new', 'Balls.glb'), js, rest)
    write(os.path.join(d, 'old', 'Balls.glb'), old(js), rest)
    for sub, ext in (('new', 'hyprwalk'), ('old', 'hypr3d')):
        json.dump({'hidden': ['Gone']}, open(os.path.join(d, sub, f'Balls.{ext}.json'), 'w'))

    js, rest = read(os.path.join(d, 'LitCourt.glb'))
    write(os.path.join(d, 'new', 'LitCourt.glb'), js, rest)
    write(os.path.join(d, 'old', 'LitCourt.glb'), old(js), rest)

    for sub, gone in (('new', ('hypr3d_', 'HYPR3D_')), ('old', ('hyprwalk_', 'HYPRWALK_'))):
        for f in ('Balls.glb', 'LitCourt.glb'):
            s = json.dumps(read(os.path.join(d, sub, f))[0])
            assert not any(g in s for g in gone), f'{sub}/{f} still has {gone}'


if __name__ == '__main__':
    main(sys.argv[1])
