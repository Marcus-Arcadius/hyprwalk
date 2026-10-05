# assets.py OUTDIR: two small glTF files for tools/test/vm, built from boxes and quads:
#   ToonTest.glb  no skeleton; cyan eyes write the stencil (UnlitWF's way, queue 2448) and show through the magenta
#                 fringe (queue 2449, stencil notequal) on both faces; body with a green inverted-hull outline
#   TestRoom.glb  a 20 m square room with walls, a pillar, hyprwalk_spawn and hyprwalk_desktop (on the north wall)
import json, math, os, struct, sys


class GLB:
    def __init__(self):
        self.bin = bytearray()
        self.js = {'asset': {'version': '2.0', 'generator': 'hyprwalk tools/test/vm/assets.py'}, 'scene': 0,
                   'scenes': [{'nodes': []}], 'nodes': [], 'meshes': [], 'materials': [], 'accessors': [],
                   'bufferViews': []}

    def view(self, data, target):
        while len(self.bin) % 4:
            self.bin.append(0)
        self.js['bufferViews'].append({'buffer': 0, 'byteOffset': len(self.bin), 'byteLength': len(data),
                                       'target': target})
        self.bin += data
        return len(self.js['bufferViews']) - 1

    def accessor(self, values, kind):
        if kind == 'SCALAR':
            data = struct.pack('<%dI' % len(values), *values)
            acc = {'componentType': 5125, 'count': len(values), 'type': 'SCALAR'}
            acc['bufferView'] = self.view(data, 34963)
        else:
            flat = [c for v in values for c in v]
            data = struct.pack('<%df' % len(flat), *flat)
            acc = {'componentType': 5126, 'count': len(values), 'type': kind}
            acc['bufferView'] = self.view(data, 34962)
            if kind == 'VEC3':
                acc['min'] = [min(v[i] for v in values) for i in range(3)]
                acc['max'] = [max(v[i] for v in values) for i in range(3)]
        self.js['accessors'].append(acc)
        return len(self.js['accessors']) - 1

    def material(self, name, color, emissive=None, extras=None, blend=False):
        m = {'name': name, 'pbrMetallicRoughness': {'baseColorFactor': list(color), 'metallicFactor': 0.0,
                                                    'roughnessFactor': 0.9}}
        if emissive:
            m['emissiveFactor'] = list(emissive)
        if extras:
            m['extras'] = extras
        if blend:
            m['alphaMode'] = 'BLEND'
        self.js['materials'].append(m)
        return len(self.js['materials']) - 1

    def mesh(self, name, parts, parent_nodes=None, translation=None):
        """parts: [(material, [(positions, normal), ...] quads)]"""
        prims = []
        for mat, quads in parts:
            pos, nrm, idx = [], [], []
            for corners, n in quads:
                base = len(pos)
                pos += corners
                nrm += [n] * 4
                idx += [base, base + 1, base + 2, base, base + 2, base + 3]
            prims.append({'attributes': {'POSITION': self.accessor(pos, 'VEC3'),
                                         'NORMAL': self.accessor(nrm, 'VEC3')},
                          'indices': self.accessor(idx, 'SCALAR'), 'material': mat})
        self.js['meshes'].append({'name': name, 'primitives': prims})
        return self.node(name, mesh=len(self.js['meshes']) - 1, translation=translation)

    def node(self, name, mesh=None, translation=None, rotation=None):
        n = {'name': name}
        if mesh is not None:
            n['mesh'] = mesh
        if translation:
            n['translation'] = list(translation)
        if rotation:
            n['rotation'] = list(rotation)
        self.js['nodes'].append(n)
        self.js['scenes'][0]['nodes'].append(len(self.js['nodes']) - 1)
        return len(self.js['nodes']) - 1

    def write(self, path):
        while len(self.bin) % 4:
            self.bin.append(0)
        self.js['buffers'] = [{'byteLength': len(self.bin)}]
        js = json.dumps(self.js, separators=(',', ':')).encode()
        while len(js) % 4:
            js += b' '
        with open(path, 'wb') as f:
            f.write(struct.pack('<III', 0x46546C67, 2, 12 + 8 + len(js) + 8 + len(self.bin)))
            f.write(struct.pack('<II', len(js), 0x4E4F534A) + js)
            f.write(struct.pack('<II', len(self.bin), 0x004E4942) + bytes(self.bin))


def box(x0, y0, z0, x1, y1, z1):
    """the six faces of a box, counter-clockwise seen from outside"""
    return [
        ([(x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1)], (0, 0, 1)),
        ([(x1, y0, z0), (x0, y0, z0), (x0, y1, z0), (x1, y1, z0)], (0, 0, -1)),
        ([(x1, y0, z1), (x1, y0, z0), (x1, y1, z0), (x1, y1, z1)], (1, 0, 0)),
        ([(x0, y0, z0), (x0, y0, z1), (x0, y1, z1), (x0, y1, z0)], (-1, 0, 0)),
        ([(x0, y1, z1), (x1, y1, z1), (x1, y1, z0), (x0, y1, z0)], (0, 1, 0)),
        ([(x0, y0, z0), (x1, y0, z0), (x1, y0, z1), (x0, y0, z1)], (0, -1, 0)),
    ]


def quad_z(x0, y0, x1, y1, z, facing):
    """a quad in the plane z, seen from +Z (facing 1) or -Z (facing -1)"""
    if facing > 0:
        return ([(x0, y0, z), (x1, y0, z), (x1, y1, z), (x0, y1, z)], (0, 0, 1))
    return ([(x1, y0, z), (x0, y0, z), (x0, y1, z), (x1, y1, z)], (0, 0, -1))


def toon(path):
    g = GLB()
    body = g.material('Body', (1.0, 0.45, 0.08, 1), extras={
        'hyprwalk_outline': {'width': 0.02, 'space': 'world', 'color': [0.0, 1.0, 0.0, 1.0], 'lit': 0.0}})
    skin = g.material('Skin', (0.95, 0.8, 0.7, 1))
    eye = g.material('Eye', (0.0, 1.0, 1.0, 1), emissive=(0.0, 1.0, 1.0), extras={
        'hyprwalk_queue': 2448,
        'hyprwalk_stencil': {'ref': 10, 'read': 255, 'write': 10, 'comp': 'always', 'pass': 'replace', 'fail': 'keep',
                           'zfail': 'keep'}})
    fringe = g.material('Fringe', (0.55, 0.0, 0.45, 1), extras={
        'hyprwalk_queue': 2449,
        'hyprwalk_stencil': {'ref': 10, 'read': 255, 'write': 255, 'comp': 'notequal', 'pass': 'keep', 'fail': 'keep',
                           'zfail': 'keep'}})
    g.mesh('Body', [(body, box(-0.25, 0.0, -0.15, 0.25, 1.0, 0.15))])
    g.mesh('Head', [(skin, box(-0.2, 1.05, -0.15, 0.2, 1.45, 0.15))])
    eyes, fringes = [], []
    for s in (1, -1):
        for x in (-0.09, 0.09):
            eyes.append(quad_z(x - 0.045, 1.235, x + 0.045, 1.325, s * 0.152, s))
        fringes.append(quad_z(-0.2, 1.2, 0.2, 1.46, s * 0.165, s))
    g.mesh('Eyes', [(eye, eyes)])
    g.mesh('Fringe', [(fringe, fringes)])
    g.write(path)


def room(path):
    g = GLB()
    floor = g.material('Floor', (0.45, 0.47, 0.5, 1))
    wall = g.material('Wall', (0.85, 0.82, 0.75, 1))
    pillar = g.material('Pillar', (0.3, 0.45, 0.7, 1))
    H, S = 3.0, 10.0
    g.mesh('Floor', [(floor, [([(-S, 0, S), (S, 0, S), (S, 0, -S), (-S, 0, -S)], (0, 1, 0))])])
    walls = [
        ([(-S, 0, -S), (S, 0, -S), (S, H, -S), (-S, H, -S)], (0, 0, 1)),   # north, facing in
        ([(S, 0, S), (-S, 0, S), (-S, H, S), (S, H, S)], (0, 0, -1)),      # south
        ([(S, 0, -S), (S, 0, S), (S, H, S), (S, H, -S)], (-1, 0, 0)),      # east
        ([(-S, 0, S), (-S, 0, -S), (-S, H, -S), (-S, H, S)], (1, 0, 0)),   # west
    ]
    g.mesh('Walls', [(wall, walls)])
    g.mesh('Pillar', [(pillar, box(3.0, 0.0, -2.0, 3.8, H, -1.2))])
    g.node('hyprwalk_spawn', translation=(0, 0, 4.0))           # facing -Z, the north wall
    g.node('hyprwalk_desktop', translation=(0, 1.7, -S + 0.01))  # front faces +Z
    g.write(path)


if __name__ == '__main__':
    out = sys.argv[1] if len(sys.argv) > 1 else '.'
    os.makedirs(out, exist_ok=True)
    toon(os.path.join(out, 'ToonTest.glb'))
    room(os.path.join(out, 'TestRoom.glb'))
    print('wrote', os.path.join(out, 'ToonTest.glb'), os.path.join(out, 'TestRoom.glb'))
