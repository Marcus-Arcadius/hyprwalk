# decal_unit.py: tools/cs2map.py's decal fix on small hand-made glTF scenes. Source 2 Viewer lifts decals (overlay
# materials) 1 cm off what they're on; since release 20 it lifts them 0.01 / 0.0254 m (39 cm) instead. cs2map measures
# which it did and puts 39 cm ones back to 1 cm, along the same normals: a floor decal, a wall decal and one under a
# 16x node (as the 3D skybox is), in place, with their accessors' min and max. A material that isn't a decal stays
# where it is, as do decals already 1 cm off (Source 2 Viewer 19), decals with nothing behind them, and a lone 39 cm
# decal among 1 cm ones. The result is written as a GLB and read back.
#   python3 tools/test/synth/decal_unit.py
import sys, os, json, struct, tempfile
sys.dont_write_bytecode = True  # (no __pycache__ left in tools/)
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))  # tools/
import cs2map

FAILS = []
WRONG, RIGHT = 0.01 / cs2map.INCH, 0.01
EX = object.__new__(cs2map.Export)  # its decal methods need no game


def check(what, got, want):
    ok = got == want
    print('%s %s: %r%s' % ('ok  ' if ok else 'FAIL', what, got, '' if ok else ' (want %r)' % (want,)))
    if not ok:
        FAILS.append(what)


def same(what, got, want):
    ok = got == want
    print('%s %s%s' % ('ok  ' if ok else 'FAIL', what, '' if ok else ': %r (want %r)' % (got, want)))
    if not ok:
        FAILS.append(what)


def near(what, got, want, tol=1e-5):
    ok = all(abs(g - w) <= tol for g, w in zip(got, want)) and len(got) == len(want)
    print('%s %s: %s%s' % ('ok  ' if ok else 'FAIL', what, [round(g, 6) for g in got], '' if ok else ' (want %s)' % (want,)))
    if not ok:
        FAILS.append(what)


def material(doc, name, decal):
    ints = {'F_OVERLAY': 1, 'F_TRANSLUCENT': 1} if decal else {'F_TRANSLUCENT': 1}
    return doc.add('materials', {'name': name, 'extras': {'vmat': {'ShaderName': 'csgo_lightmappedgeneric.vfx', 'IntParams': ints}}})


def quad(doc, corners, normal, mat, normals=True):
    """a mesh of one quad (two triangles), returns the mesh"""
    attrs = {'POSITION': doc.add_accessor('3f', corners, 'VEC3', 5126, minmax=True)}
    if normals:
        attrs['NORMAL'] = doc.add_accessor('3f', [normal] * 4, 'VEC3', 5126)
    idx = doc.add_accessor('H', [(i,) for i in (0, 1, 2, 0, 2, 3)], 'SCALAR', 5123)
    return doc.add('meshes', {'primitives': [{'attributes': attrs, 'indices': idx, 'material': mat}]})


def floor_quad(y, half):
    return [(-half, y, -half), (half, y, -half), (half, y, half), (-half, y, half)]


def wall_quad(x, half):
    return [(x, 0.5, -half), (x, 0.5 + 2 * half, -half), (x, 0.5 + 2 * half, half), (x, 0.5, half)]


def node(doc, mesh, parent=None, name='n0_mesh', matrix=None):
    nd = {'name': name, 'mesh': mesh} if mesh is not None else {'name': name}
    if matrix:
        nd['matrix'] = matrix
    n = doc.add('nodes', nd)
    if parent is None:
        doc.roots.append(n)
    else:
        doc.j['nodes'][parent].setdefault('children', []).append(n)
    return n


def scene(lift, extra=()):
    """a floor and a wall (not decals) with a decal on each at `lift`, a sign that isn't a decal 39 cm off the wall,
    and a floor with a decal under a node 16 times bigger; `extra` adds decals at other lifts (floor decals over
    x = 10, 20, ...)"""
    doc = cs2map.Doc()
    stone, paint, sign = material(doc, 'stone', False), material(doc, 'paint', True), material(doc, 'sign', False)
    node(doc, quad(doc, floor_quad(0, 3), (0, 1, 0), stone), name='n0_floor')
    node(doc, quad(doc, wall_quad(3, 1.5), (-1, 0, 0), stone), name='n0_wall')
    parts = {'floor decal': node(doc, quad(doc, floor_quad(lift, 0.5), (0, 1, 0), paint), name='n0_overlay1'),
             'wall decal': node(doc, quad(doc, wall_quad(3 - lift, 0.4), (-1, 0, 0), paint), name='n0_overlay2'),
             'sign': node(doc, quad(doc, [(3 - WRONG, 2.5, 1), (3 - WRONG, 2.9, 1), (3 - WRONG, 2.9, 1.4), (3 - WRONG, 2.5, 1.4)],
                                    (-1, 0, 0), sign), name='n0_sign')}
    big = node(doc, None, name='hypr3d_backdrop', matrix=[16, 0, 0, 0, 0, 16, 0, 0, 0, 0, 16, 0, 200, -8, 0, 1])
    node(doc, quad(doc, floor_quad(0, 2), (0, 1, 0), stone), parent=big, name='node000_ground')
    parts['16x decal'] = node(doc, quad(doc, floor_quad(lift, 0.3), (0, 1, 0), paint), parent=big, name='node000_overlay')
    for i, l in enumerate(extra):
        x = 10.0 * (i + 1)
        node(doc, quad(doc, [(c[0] + x, c[1], c[2]) for c in floor_quad(0, 1)], (0, 1, 0), stone), name='n0_more')
        parts[f'decal at x={x:g}'] = node(doc, quad(doc, [(c[0] + x, c[1], c[2]) for c in floor_quad(l, 0.4)], (0, 1, 0), paint),
                                          name='n0_overlay_more')
    return doc, parts


def positions(doc, n):
    return doc.read(doc.j['meshes'][doc.j['nodes'][n]['mesh']]['primitives'][0]['attributes']['POSITION'])


def column(values, k):
    return list(values[k::3])


# ---------------------------------------------------------------- lifted 39 cm: put back to 1 cm

doc, parts = scene(WRONG)
before = {k: positions(doc, n) for k, n in parts.items()}
EX.fix_decals(doc)
near('floor decal: y back to 1 cm', column(positions(doc, parts['floor decal']), 1), [RIGHT] * 4)
near('floor decal: x and z as they were', column(positions(doc, parts['floor decal']), 0) + column(positions(doc, parts['floor decal']), 2),
     column(before['floor decal'], 0) + column(before['floor decal'], 2))
near('wall decal: x 1 cm off the wall', column(positions(doc, parts['wall decal']), 0), [3 - RIGHT] * 4)
near('16x decal: 1 cm in its own units', column(positions(doc, parts['16x decal']), 1), [RIGHT] * 4)
same('the sign (not a decal) stays 39 cm off the wall', positions(doc, parts['sign']), before['sign'])
acc = doc.j['accessors'][doc.j['meshes'][doc.j['nodes'][parts['floor decal']]['mesh']]['primitives'][0]['attributes']['POSITION']]
vals = positions(doc, parts['floor decal'])
check("the floor decal's min and max are its stored numbers", (acc['min'], acc['max']),
      ([min(vals[k::3]) for k in range(3)], [max(vals[k::3]) for k in range(3)]))

# measured again, from each decal back to what it's on
rays, owner, targets = [], [], []
decal = {i for i, m in enumerate(doc.list('materials')) if EX.is_decal(cs2map.vmat(m))}
for nd, m in doc.placed():
    for p in doc.j['meshes'][nd['mesh']]['primitives'] if 'mesh' in nd else []:
        if p['material'] in decal:
            r = EX.decal_rays(doc, p, m)
            rays += r
            owner += [nd['name']] * len(r)
        else:
            targets.append((p, m))
gaps = EX.decal_gaps(doc, rays, targets)
check('every decal ray finds what it is on', sum(1 for g in gaps if g is None), 0)
near('and it is 1 cm behind each', gaps, [RIGHT] * len(gaps), 1e-6)

# written as a GLB and read back
out = os.path.join(tempfile.mkdtemp(prefix='decal_unit'), 'decals.glb')
j = doc.write_glb(out)
data = open(out, 'rb').read()
n = struct.unpack('<I', data[12:16])[0]
gj = json.loads(data[20:20 + n])
binary = data[20 + n + 8:]
acc = gj['accessors'][gj['meshes'][gj['nodes'][[i for i, nd in enumerate(gj['nodes']) if nd['name'] == 'n0_overlay1'][0]]['mesh']]['primitives'][0]['attributes']['POSITION']]
bv = gj['bufferViews'][acc['bufferView']]
ys = struct.unpack_from('<12f', binary, bv.get('byteOffset', 0) + acc.get('byteOffset', 0))[1::3]
near('the GLB has the floor decal 1 cm up', list(ys), [RIGHT] * 4)

# ---------------------------------------------------------------- left alone

doc, parts = scene(RIGHT)
before = {k: positions(doc, n) for k, n in parts.items()}
EX.fix_decals(doc)
same('decals 1 cm off (Source 2 Viewer 19) stay where they are', {k: positions(doc, n) for k, n in parts.items()}, before)

doc = cs2map.Doc()
paint = material(doc, 'paint', True)
lone = node(doc, quad(doc, floor_quad(WRONG, 0.5), (0, 1, 0), paint), name='n0_overlay')
was = positions(doc, lone)
EX.fix_decals(doc)
same('a decal with nothing behind it stays where it is', positions(doc, lone), was)

doc, parts = scene(RIGHT, extra=(WRONG,))
before = {k: positions(doc, n) for k, n in parts.items()}
EX.fix_decals(doc)
same('one decal 39 cm off among three 1 cm off: all stay', {k: positions(doc, n) for k, n in parts.items()}, before)

# Source 2 Viewer lifts every decal alike, so one with something else in the gap under it (a curb 5.7 cm high, as
# under a parking stripe on de_mirage) goes down with the rest, to 1 cm off the floor it's painted on
doc, parts = scene(WRONG)
node(doc, quad(doc, floor_quad(0.057, 0.2), (0, 1, 0), 0), name='n0_curb')
EX.fix_decals(doc)
near('a decal over a curb (measured 34 cm) goes down with the rest', column(positions(doc, parts['floor decal']), 1), [RIGHT] * 4)
near('as do the others', column(positions(doc, parts['wall decal']), 0) + column(positions(doc, parts['16x decal']), 1),
     [3 - RIGHT] * 4 + [RIGHT] * 4)

# decals that stood 2 cm off their surfaces before the lift (de_dust2's window insets) count as lifted too, and keep
# their 2 cm
doc, parts = scene(WRONG + 0.02)
EX.fix_decals(doc)
near('decals 2 cm proud and then lifted come down to 3 cm', column(positions(doc, parts['floor decal']), 1) +
     column(positions(doc, parts['wall decal']), 0) + column(positions(doc, parts['16x decal']), 1),
     [RIGHT + 0.02] * 4 + [3 - RIGHT - 0.02] * 4 + [RIGHT + 0.02] * 4)

doc = cs2map.Doc()
stone, paint = material(doc, 'stone', False), material(doc, 'paint', True)
node(doc, quad(doc, floor_quad(0, 3), (0, 1, 0), stone), name='n0_floor')
lifted = node(doc, quad(doc, floor_quad(WRONG, 0.5), (0, 1, 0), paint), name='n0_overlay')
bare = node(doc, quad(doc, [(c[0] + 1.5, c[1], c[2]) for c in floor_quad(WRONG, 0.3)], (0, 1, 0), paint, normals=False), name='n0_overlay_bare')
was = positions(doc, bare)
EX.fix_decals(doc)
near('a decal with normals goes down', column(positions(doc, lifted), 1), [RIGHT] * 4)
same('one without normals (Source 2 Viewer lifts only those with) stays', positions(doc, bare), was)

print('%d failed' % len(FAILS) if FAILS else 'all ok')
sys.exit(1 if FAILS else 0)
