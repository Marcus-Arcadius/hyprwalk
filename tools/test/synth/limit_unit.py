# limit_unit.py: tools/unity2hypr3d.py's PhysBone limits and Immobile, on booth.py's SynthChan: Angle, Hinge and Polar
# limits come out as VRMC_springBone_limit's cone, hinge and spherical ones (radians), a limit's Rotation (Unity's Euler
# angles) as the GLB's turn (Unity's mirrored: x the same, y and z the other way), and with it the hemisphere the back
# hair may swing in lies behind it, off her back, tilted the 15° of its yaw; an All Motion Immobile is the spring's
# "parentImmobile", a World one isn't.
#   python3 tools/test/synth/limit_unit.py [BOOTHDIR]   (booth.py's packages; made in a temporary directory if not given)
import sys, os, json, math, struct, shutil, subprocess, tempfile
sys.dont_write_bytecode = True  # (no __pycache__ left in tools/)
HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.join(HERE, '..', '..')

FAILS = []


def check(what, got, want, tol=None):
    ok = abs(got - want) <= tol if tol is not None else got == want
    print('%s %s: %r%s' % ('ok  ' if ok else 'FAIL', what, got, '' if ok else ' (want %r)' % (want,)))
    if not ok:
        FAILS.append(what)


def qmul(a, b):  # (w, x, y, z)
    w1, x1, y1, z1 = a
    w2, x2, y2, z2 = b
    return (w1 * w2 - x1 * x2 - y1 * y2 - z1 * z2, w1 * x2 + x1 * w2 + y1 * z2 - z1 * y2,
            w1 * y2 - x1 * z2 + y1 * w2 + z1 * x2, w1 * z2 + x1 * y2 - y1 * x2 + z1 * w2)


def qrot(q, v):
    return qmul(qmul(q, (0.0,) + tuple(v)), (q[0], -q[1], -q[2], -q[3]))[1:]


def axis_angle(axis, deg):
    s = math.sin(math.radians(deg) / 2)
    return (math.cos(math.radians(deg) / 2), axis[0] * s, axis[1] * s, axis[2] * s)


W = tempfile.mkdtemp(prefix='limit_unit')
booth = sys.argv[1] if len(sys.argv) > 1 else os.path.join(W, 'booth')
if not os.path.isfile(os.path.join(booth, 'SynthChan_v1.0.unitypackage')):
    subprocess.run(['blender', '-b', '--factory-startup', '--python-exit-code', '1', '-P', os.path.join(HERE, 'booth.py'),
                    '--', booth], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
glb = os.path.join(W, 'SynthChan.glb')
subprocess.run([sys.executable, os.path.join(TOOLS, 'unity2hypr3d.py'), os.path.join(booth, 'SynthChan_v1.0.unitypackage'),
                '-o', glb], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
st = json.load(open(glb[:-4] + '.hypr3d.json'))
data = open(glb, 'rb').read()
js = json.loads(data[20:20 + struct.unpack_from('<I', data, 12)[0]])

springs = {s['bones'][0]: s for s in st['springs']}
deg = math.radians
check('the ears: a hinge (Hinge, 25°)', springs['Ear_L'].get('limit'), {'hinge': {'angle': round(deg(25), 5)}})
check('the tail: a spherical limit (Polar: 40° pitch, 60° yaw)', springs['Tail'].get('limit'),
      {'spherical': {'pitch': round(deg(40), 5), 'yaw': round(deg(60), 5)}})
check('the skirt: a cone (Angle, 60°), no rotation', springs['Skirt_F'].get('limit'), {'cone': {'angle': round(deg(60), 5)}})
check('the twin tails: a cone (45°)', springs['TwinTail_L'].get('limit'), {'cone': {'angle': round(deg(45), 5)}})
hb = springs['HairBack'].get('limit', {}).get('cone', {})
check('the back hair: a hemisphere (Angle, 90°)', hb.get('angle'), round(deg(90), 5))
# Unity's Quaternion.Euler(90, 15, 0): z, then x, then y; mirrored for the GLB: (x, -y, -z, w)
e = qmul(axis_angle((0, 1, 0), 15), axis_angle((1, 0, 0), 90))
want = (e[1], -e[2], -e[3], e[0])
got = hb.get('rotation', [0, 0, 0, 1])
for k, c in enumerate('xyzw'):
    check('the back hair limit\'s rotation, %s (Unity\'s Euler 90, 15, 0 mirrored)' % c, got[k], want[k], 1e-4)

# where its hemisphere is, in the GLB's world (VRMC_springBone_limit: the node's frame, turned the shortest way from y
# to its child, then by the rotation): behind it, off her back, and out to a side by sin 15°
nodes = js['nodes']
parent = {c: i for i, n in enumerate(nodes) for c in n.get('children', [])}
index = {n.get('name'): i for i, n in enumerate(nodes)}


def world_rot(i):
    q = (1.0, 0.0, 0.0, 0.0)
    chain = []
    while i is not None:
        chain.append(i)
        i = parent.get(i)
    for k in reversed(chain):
        r = nodes[k].get('rotation', [0, 0, 0, 1])
        q = qmul(q, (r[3], r[0], r[1], r[2]))
    return q


n = index['HairBack']
t = nodes[nodes[n]['children'][0]].get('translation', [0, 1, 0])
length = math.sqrt(sum(a * a for a in t))
d = [a / length for a in t]
from_y = (1 + d[1], d[2], 0.0, -d[0])
from_y = tuple(a / math.sqrt(sum(b * b for b in from_y)) for a in from_y)
frame = qmul(qmul(world_rot(n), from_y), (got[3], got[0], got[1], got[2]))
ax = qrot(frame, (0, 1, 0))
bone = qrot(world_rot(n), d)
check('the hemisphere is behind the back hair (the GLB\'s -z: she faces +z)', ax[2] < -0.9, True)
check('... out to a side by its yaw (|x| sin 15°)', abs(ax[0]), math.sin(deg(15)), 0.02)
check('... and square to the bone (its edge through where it hangs)', abs(sum(a * b for a, b in zip(ax, bone))), 0.0, 1e-3)

check('the twin tails\' Immobile (All Motion 0.3) is their parentImmobile', springs['TwinTail_L'].get('parentImmobile'), 0.3)
check('the back hair\'s (World 0.5) isn\'t', 'parentImmobile' in springs['HairBack'], False)
check('nor is a 0 one (the skirt)', 'parentImmobile' in springs['Skirt_F'], False)
check('their own "immobile" (of where she goes) stays the default: 0.3 is under it', 'immobile' in springs['TwinTail_L'], False)

shutil.rmtree(W, ignore_errors=True)
print('%d failed' % len(FAILS) if FAILS else 'all ok')
sys.exit(1 if FAILS else 0)
