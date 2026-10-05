# human_unit.py: tools/unity2hyprwalk.py's humanoid muscle maths (Unity muscle clips as bone turns) on a small T-posed
# skeleton: swing-twist, muscle signs, the arm's twist shared with the forearm, RootT/RootQ, Unity curves with weighted
# keys, Foot IK to goals and a clip read. Given Unity T pose clips (VRChat's SDK has proxy_tpose.anim), it also poses the
# skeleton with their muscle values.
#   blender -b --factory-startup --python-exit-code 1 -P human_unit.py [-- T_POSE.anim...]
import sys, os, math
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))  # tools/
import unity2hyprwalk as u
from mathutils import Matrix, Vector, Quaternion

FAILS = []


def check(what, got, want):
    ok = got == want
    print('%s %s: %r%s' % ('ok  ' if ok else 'FAIL', what, got, '' if ok else ' (want %r)' % (want,)))
    if not ok:
        FAILS.append(what)


def within(what, got, most):
    ok = got <= most
    print('%s %s: %.4f%s' % ('ok  ' if ok else 'FAIL', what, got, '' if ok else ' (want at most %g)' % most))
    if not ok:
        FAILS.append(what)


def qclose(a, b):
    return 1.0 - abs(a.dot(b))  # 0 for the same turn


# a humanoid in Unity's T pose (Unity space: y up, facing +z, its left at -x), every bone unturned
HB = {n: i for i, n in enumerate(u.HUMAN_BONES)}
BONES = [('Root', None, (0, 0, 0), None), ('Hips', 'Root', (0, 1.0, 0), 'Hips'),
         ('Spine', 'Hips', (0, 1.1, 0), 'Spine'), ('Chest', 'Spine', (0, 1.25, 0), 'Chest'),
         ('Neck', 'Chest', (0, 1.45, 0), 'Neck'), ('Head', 'Neck', (0, 1.55, 0), 'Head')]
for s, x in (('Left', -1), ('Right', 1)):
    BONES += [(s + 'UpperLeg', 'Hips', (0.1 * x, 0.95, 0), s + 'UpperLeg'),
              (s + 'LowerLeg', s + 'UpperLeg', (0.1 * x, 0.52, 0), s + 'LowerLeg'),
              (s + 'Foot', s + 'LowerLeg', (0.1 * x, 0.08, 0), s + 'Foot'),
              (s + 'Toes', s + 'Foot', (0.1 * x, 0.0, 0.12), s + 'Toes'),
              (s + 'Shoulder', 'Chest', (0.03 * x, 1.4, 0), s + 'Shoulder'),
              (s + 'UpperArm', s + 'Shoulder', (0.15 * x, 1.4, 0), s + 'UpperArm'),
              (s + 'LowerArm', s + 'UpperArm', (0.43 * x, 1.4, 0), s + 'LowerArm'),
              (s + 'Hand', s + 'LowerArm', (0.68 * x, 1.4, 0), s + 'Hand'),
              (s + 'Index1', s + 'Hand', (0.76 * x, 1.4, 0.02), s + ' Index Proximal'),
              (s + 'Index2', s + 'Index1', (0.8 * x, 1.4, 0.02), s + ' Index Intermediate'),
              (s + 'Index3', s + 'Index2', (0.83 * x, 1.4, 0.02), s + ' Index Distal'),
              (s + 'Index4', s + 'Index3', (0.85 * x, 1.4, 0.02), None)]
AT = {n: Vector(p) for n, _, p, _ in BONES}
IDX = {n: i for i, (n, *_) in enumerate(BONES)}
TPOSE = [(n, IDX[p] if p else -1, Matrix.Translation(AT[n] - (AT[p] if p else Vector()))) for n, p, _, _ in BONES]
HUMAN = {HB[h]: IDX[n] for n, _, _, h in BONES if h}
axes = u.HumanAxes(TPOSE, HUMAN)


def posed(muscles=None, root_t=(0, 1, 0), root_q=Quaternion()):
    m = {u.MUSCLE_OF[k]: v for k, v in (muscles or {}).items()}
    return axes.world_of(axes.pose(m, Vector(root_t), root_q))


def at(w, n):
    return w[IDX[n]].translation


def along(w, a, b):
    return (at(w, b) - at(w, a)).normalized()


def angle(v, w):
    return math.degrees(v.angle(w, 0.0))


print('== swing-twist: tan(angle / 2) parts, the twist about x first')
check('no angles: no turn', u.swing_twist(0, 0, 0), Quaternion())
for k, axis in enumerate(('X', 'Y', 'Z')):
    ang = [0.0, 0.0, 0.0]
    ang[k] = 0.7
    within('%s alone: a turn about %s' % ('xyz'[k], axis), qclose(u.swing_twist(*ang), Quaternion(Vector(
        [1.0 if j == k else 0.0 for j in range(3)]), 0.7)), 1e-9)
within('twist then swing', qclose(u.swing_twist(0.4, 0.3, -0.5), u.swing_twist(0, 0.3, -0.5) @ u.swing_twist(0.4, 0, 0)),
       1e-9)
q = u.swing_twist(0, 0.5, 0.5)
within('a swing of both: about their bisector, by 2 atan(|tan halves|)', abs(math.degrees(q.angle) - math.degrees(
    2 * math.atan(math.hypot(math.tan(0.25), math.tan(0.25))))), 1e-4)

print('== the T pose: Unity\'s own T pose muscle values (its T pose clips\', rounded) give it back')
T_MUSCLES = {}
for s in ('Left', 'Right'):
    T_MUSCLES.update({s + ' Arm Down-Up': 0.392, s + ' Arm Front-Back': 0.311, s + ' Arm Twist In-Out': 0.112,
                      s + ' Forearm Stretch': 0.965, s + ' Forearm Twist In-Out': -0.085,
                      s + ' Upper Leg Front-Back': 0.6, s + ' Lower Leg Stretch': 1.0,
                      s + 'Hand.Index.1 Stretched': 0.669, s + 'Hand.Index.2 Stretched': 0.84,
                      s + 'Hand.Index.3 Stretched': 0.812, s + 'Hand.Index.Spread': -0.447})
LIMBS = [('UpperArm', 'LowerArm'), ('LowerArm', 'Hand'), ('UpperLeg', 'LowerLeg'), ('LowerLeg', 'Foot'),
         ('Index1', 'Index2'), ('Index2', 'Index3'), ('Index3', 'Index4')]


def tpose_errors(muscles):
    w, t = posed(muscles), axes.world_of(axes.local)
    return max(angle(along(w, s + a, s + b), along(t, s + a, s + b)) for s in ('Left', 'Right') for a, b in LIMBS)


within('the limbs\' worst, degrees', tpose_errors(T_MUSCLES), 6.0)
w0, t = posed(T_MUSCLES), axes.world_of(axes.local)
within('the hips where the T pose has them, m', (at(w0, 'Hips') - at(t, 'Hips')).length, 0.02)
w = posed()
arm, leg = along(w, 'LeftUpperArm', 'LeftLowerArm'), along(w, 'LeftUpperLeg', 'LeftLowerLeg')
check('all muscles 0, Unity\'s neutral pose: the arms down and forward, the legs forward',
      (arm.y < -0.5, arm.z > 0.3, leg.z > 0.3), (True, True, True))

print('== left and right alike: the same values mirror each other')
worst = 0.0
for name, b, axis, lo, hi in u.MUSCLES:
    if not name.startswith('Left') or u.HUMAN_BONES[b].replace(' ', '') not in \
            ''.join(u.HUMAN_BONES[x].replace(' ', '') + '|' for x in HUMAN):
        continue
    other = 'Right' + name[4:]
    for v in (-0.7, 0.7):
        w = posed({name: v, other: v})
        for n, *_ in BONES:
            if n.startswith('Left'):
                a, c = at(w, n), at(w, 'Right' + n[4:])
                worst = max(worst, (Vector((-a.x, a.y, a.z)) - c).length)
within('the worst mismatch, m (a tenth of a millimetre, in 32-bit floats)', worst, 1e-3)

print('== what the signs do: -1 is the first word of the muscle\'s name, +1 the second')
t = axes.world_of(axes.local)


def higher(m, v, a, b):
    w = posed({m: v})
    return at(w, b).y - at(w, a).y


check('Arm Down-Up + raises the arm, - lowers it', (higher('Left Arm Down-Up', 1, 'LeftUpperArm', 'LeftLowerArm') > 0.1,
                                                    higher('Left Arm Down-Up', -1, 'LeftUpperArm', 'LeftLowerArm') < -0.2),
      (True, True))
w1, w2 = posed({'Left Forearm Stretch': 1}), posed({'Left Forearm Stretch': -1})
check('Forearm Stretch + straightens the elbow, - bends it', (angle(along(w1, 'LeftUpperArm', 'LeftLowerArm'), along(
    w1, 'LeftLowerArm', 'LeftHand')) < 20, angle(along(w2, 'LeftUpperArm', 'LeftLowerArm'), along(
        w2, 'LeftLowerArm', 'LeftHand')) > 100), (True, True))
w1, w0, w2 = (posed({'Left Upper Leg Front-Back': 0.6, 'Left Lower Leg Stretch': v}) for v in (1, 0, -1))
check('Lower Leg Stretch (the thigh down): + straight, 0 the knee bent back 80 degrees, - the foot up behind',
      (abs(at(w1, 'LeftFoot').z - at(w1, 'LeftLowerLeg').z) < 0.03, at(w0, 'LeftFoot').z < at(w0, 'LeftLowerLeg').z - 0.3,
       at(w2, 'LeftFoot').y > at(w2, 'LeftLowerLeg').y + 0.2), (True, True, True))
w1, w2 = posed({'Left Upper Leg Front-Back': -1}), posed({'Left Upper Leg Front-Back': 1})
check('Upper Leg Front-Back - lifts the leg forward, + swings it back', (
    at(w1, 'LeftLowerLeg').z > at(w1, 'LeftUpperLeg').z + 0.3, at(w2, 'LeftLowerLeg').z < at(w2, 'LeftUpperLeg').z - 0.1),
      (True, True))
def bent(w):  # chest direction in the hips' frame (RootQ keeps the body upright)
    return w[IDX['Hips']].to_quaternion().inverted() @ along(w, 'Spine', 'Chest')


check('Spine Front-Back - bends forward, + back', (bent(posed({'Spine Front-Back': -1})).z > 0.5,
                                                   bent(posed({'Spine Front-Back': 1})).z < -0.5), (True, True))
w0, w1 = posed(), posed({'Left Arm Front-Back': 1})
check('Arm Front-Back + takes the arm back', at(w1, 'LeftLowerArm').z < at(w0, 'LeftLowerArm').z - 0.1, True)
w1, w2 = posed({'LeftHand.Index.1 Stretched': -1}), posed({'LeftHand.Index.1 Stretched': 1})
check('a finger\'s Stretched - curls it (down, to the palm), + opens it',
      (at(w1, 'LeftIndex2').y < at(w1, 'LeftIndex1').y - 0.02, at(w2, 'LeftIndex2').y > at(w1, 'LeftIndex2').y), (True, True))
w = posed({'Head Turn Left-Right': 1})
check('Head Turn Left-Right + looks right (+x)', (w[IDX['Head']].to_quaternion() @ Vector((0, 0, 1))).x > 0.3, True)

print('== the arm\'s twist: shared with the forearm (half each by default)')
w0, w1 = posed(), posed({'Left Arm Twist In-Out': 1})
d = along(w0, 'LeftUpperArm', 'LeftLowerArm')


def twist(w_a, w_b, n):  # the turn about the arm between two poses
    q = w_b[IDX[n]].to_quaternion() @ w_a[IDX[n]].to_quaternion().inverted()
    return math.degrees(2 * math.atan2(Vector(q[1:]).dot(d), q[0]))


check('the upper arm and the hand turn by half and all of it', (round(abs(twist(w0, w1, 'LeftUpperArm')), 1),
                                                               round(abs(twist(w0, w1, 'LeftHand')), 1)), (45.0, 90.0))
bend = [angle(along(w, 'LeftUpperArm', 'LeftLowerArm'), along(w, 'LeftLowerArm', 'LeftHand')) for w in (w0, w1)]
within('the elbow bent as much, degrees', abs(bend[0] - bend[1]), 0.01)

print('== the body: RootT is its centre of mass in human scales, RootQ its turn')
w0, w1 = posed(root_t=(0, 1, 0)), posed(root_t=(0.5, 1.2, -0.3))
within('RootT moves the hips by its change times the human scale', (at(w1, 'Hips') - at(w0, 'Hips') - Vector(
    (0.5, 0.2, -0.3)) * axes.scale).length, 1e-5)
within('the centre of mass at RootT', (axes.com(w1) - Vector((0.5, 1.2, -0.3)) * axes.scale).length, 1e-5)
w = posed(root_q=Quaternion((0, 1, 0), math.pi / 2))
check('RootQ a quarter turn about y: facing +x (its left toward +z)', along(w, 'RightUpperArm', 'LeftUpperArm').z > 0.9,
      True)

print('== Unity\'s curves: Hermite between keys, flat beyond, an infinite slope steps')
keys = [(0.0, 0.0, 0.0, 0.0), (1.0, 1.0, 0.0, 0.0)]
check('flat slopes: smoothstep', [round(u.unity_curve(keys, t), 5) for t in (-1, 0.25, 0.5, 1.5)],
      [0.0, 0.15625, 0.5, 1.0])
check('straight slopes: a line', round(u.unity_curve([(0.0, 0.0, 1.0, 1.0), (2.0, 2.0, 1.0, 1.0)], 0.5), 6), 0.5)
check('an infinite slope: held', u.unity_curve([(0.0, 3.0, 0.0, float('inf')), (1.0, 5.0, 0.0, 0.0)], 0.9), 3.0)

print('== weighted keys: a cubic Bezier in time and value, its handles that share of the span along the slopes')


def bezier_ref(k0, k1, t, a, b, n=20000):
    """the span as Unity draws it: the densely sampled Bezier's value at the time nearest t"""
    t0, v0, o0, t1, v1, i1 = k0[0], k0[1], k0[3], k1[0], k1[1], k1[2]
    dt = t1 - t0
    P = [(t0, v0), (t0 + a * dt, v0 + a * dt * o0), (t1 - b * dt, v1 - b * dt * i1), (t1, v1)]
    best = None
    for j in range(n + 1):
        q = j / n
        c = ((1 - q) ** 3, 3 * (1 - q) ** 2 * q, 3 * (1 - q) * q * q, q ** 3)
        x = sum(w * p[0] for w, p in zip(c, P))
        y = sum(w * p[1] for w, p in zip(c, P))
        if best is None or abs(x - t) < best[0]:
            best = (abs(x - t), y)
    return best[1]


h0, h1 = (0.0, 0.0, 0.0, 2.0), (1.0, 1.0, -1.0, 0.0)
w0, w1 = h0 + (1 / 3, 1 / 3, 3), h1 + (1 / 3, 1 / 3, 3)
check('both weights a third: the Hermite', [round(u.unity_curve([w0, w1], t) - u.unity_curve([h0, h1], t), 9)
                                            for t in (0.1, 0.5, 0.9)], [0.0, 0.0, 0.0])
k0, k1 = (0.0, 0.0, 0.0, 3.0, 0.0, 0.8, 2), (2.0, 1.0, 0.5, 0.0, 0.1, 0.0, 1)
within('out 0.8, in 0.1: as the Bezier drawn, worst of five', max(
    abs(u.unity_curve([k0, k1], t) - bezier_ref(k0, k1, t, 0.8, 0.1)) for t in (0.1, 0.5, 1.0, 1.5, 1.9)), 1e-3)
k0 = (0.0, 0.0, 0.0, 3.0, 0.0, 0.8, 2)
k1 = (2.0, 1.0, 0.5, 0.0, 0.9, 0.0, 0)  # its in weight not in effect: a third
within('only one side weighted: the other a third', max(
    abs(u.unity_curve([k0, k1], t) - bezier_ref(k0, k1, t, 0.8, 1 / 3)) for t in (0.2, 1.0, 1.8)), 1e-3)
check('not the Hermite when weighted', abs(u.unity_curve([k0, k1], 0.5) - u.unity_curve([k0[:4], k1[:4]], 0.5)) > 0.01,
      True)
keys = u.curve_keys({'curve': {'m_Curve': [
    {'time': '0', 'value': '1', 'inSlope': '0', 'outSlope': '0', 'weightedMode': '0', 'inWeight': '0.3333',
     'outWeight': '0.3333'},
    {'time': '1', 'value': '2', 'inSlope': '0', 'outSlope': '0', 'weightedMode': '3', 'inWeight': '0.5',
     'outWeight': '0.25'}]}})
check('read: the weights kept on weighted keys only', keys, [(0.0, 1.0, 0.0, 0.0), (1.0, 2.0, 0.0, 0.0, 0.5, 0.25, 3)])

print('== Foot IK: the legs bent so each foot is where the clip\'s goal says (the body\'s frame, the sole)')


def goal_of(w, foot, root_t, root_q):
    """the goal a clip would have for a foot as posed: in the body's frame and human scales, its sole"""
    W = w[axes.human[foot]]
    turn = W.to_quaternion() @ axes.post[foot]
    sole = W.translation + turn @ Vector((axes.world[axes.human[foot]].translation.y, 0.0, 0.0))
    return (root_q.inverted() @ (sole - Vector(root_t) * axes.scale)) / axes.scale, root_q.inverted() @ turn


rt, rq = (0.05, 0.93, 0.1), Quaternion((0, 1, 0), 0.4)
want = {'Left Upper Leg Front-Back': 0.5, 'Left Lower Leg Stretch': -0.4, 'Right Upper Leg In-Out': 0.3,
        'Right Foot Up-Down': 0.5}
w_want = posed(want, rt, rq)
goals = {b: goal_of(w_want, b, rt, rq) for b in (5, 6)}
other = {'Left Upper Leg Front-Back': 0.1, 'Left Lower Leg Stretch': 0.2, 'Right Upper Leg Front-Back': -0.3}
m = {u.MUSCLE_OF[k]: v for k, v in other.items()}
local = axes.plant(axes.pose(m, Vector(rt), rq), Vector(rt), rq, goals)
w = axes.world_of(local)
within('the ankles where the goals put them, metres', max((at(w, s + 'Foot') - at(w_want, s + 'Foot')).length
                                                         for s in ('Left', 'Right')), 1e-4)
within('... the feet turned as they say', max(qclose(w[IDX[s + 'Foot']].to_quaternion(),
                                                     w_want[IDX[s + 'Foot']].to_quaternion()) for s in ('Left', 'Right')),
       1e-8)
check('... the knees still bend forwards', all(
    (at(w, s + 'LowerLeg') - (at(w, s + 'UpperLeg') + at(w, s + 'Foot')) / 2).dot(rq @ Vector((0, 0, 1))) > 0
    for s in ('Left', 'Right')), True)
check('... the legs keep their lengths', max(abs((at(w, s + 'LowerLeg') - at(w, s + 'UpperLeg')).length - 0.43)
                                            for s in ('Left', 'Right')) < 1e-6, True)
far = {5: (goals[5][0] + Vector((0, -1.0, 0)), goals[5][1])}
w = axes.world_of(axes.plant(axes.pose({}, Vector(rt), rq), Vector(rt), rq, far))
within('a goal out of reach: the leg straight towards it, the knee\'s degrees', angle(
    along(w, 'LeftUpperLeg', 'LeftLowerLeg'), along(w, 'LeftLowerLeg', 'LeftFoot')), 1.0)

print('== a clip read: muscles, the body, faces; its length and loop')


def curve(attr, cls, keys):
    return {'attribute': attr, 'classID': str(cls), 'path': '' if cls == 95 else 'Body', 'curve': {'m_Curve': [
        {'time': str(t), 'value': str(v), 'inSlope': '0', 'outSlope': '0'} for t, v in keys]}}


clip = u.HumanClip({'m_Name': 'Wave', 'm_SampleRate': '120', 'm_AnimationClipSettings': {
    'm_LoopTime': '1', 'm_StopTime': '2'}, 'm_FloatCurves': [
    curve('Left Arm Down-Up', 95, [(0, 0), (1, 1)]), curve('RootT.y', 95, [(0, 1)]),
    curve('blendShape.あ', 137, [(0, 0), (0.5, 100)]), curve('m_LocalPosition.x', 4, [(0, 1)])]})
check('its parts', (sorted(u.MUSCLES[i][0] for i in clip.muscles), sorted(clip.root), sorted(clip.faces)),
      (['Left Arm Down-Up'], ['RootT.y'], ['あ']))
check('humanoid, looping, 2 s, sampled at 60 at most', (clip.humanoid, clip.loop, clip.length, clip.rate),
      (True, True, 2.0, 60.0))
mus, rt, rq = clip.at(0.5)
check('at 0.5 s', (round(mus[u.MUSCLE_OF['Left Arm Down-Up']], 3), tuple(rt), tuple(rq)),
      (0.5, (0.0, 1.0, 0.0), (1.0, 0.0, 0.0, 0.0)))
clip = u.HumanClip({'m_Name': 'Step', 'm_AnimationClipSettings': {'m_StopTime': '1'}, 'm_FloatCurves': [
    curve('Left Arm Down-Up', 95, [(0, 0), (1, 1)]), curve('LeftFootT.y', 95, [(0, 0.1), (1, 0.3)]),
    curve('LeftFootQ.w', 95, [(0, 1)]), curve('RightHandT.x', 95, [(0, 0.5)]), curve('ChestTDOF.z', 95, [(0, 0.01)])]})
g = clip.goals_at(0.5)
check('the goals (feet, hands), Translation DoF bones named', (sorted(g), tuple(round(x, 3) for x in g[5][0]),
                                                              sorted(clip.tdof), clip.foot_ik),
      ([5, 18], (0.0, 0.2, 0.0), ['Chest'], False))

for f in sys.argv[sys.argv.index('--') + 1:] if '--' in sys.argv else []:
    print('== %s' % f)
    uf = u.UFile(f)
    c = u.HumanClip(uf.get(uf.main(74))[1])
    mus, rt, rq = c.at(0.0)
    names = {u.MUSCLES[i][0]: v for i, v in mus.items()}
    within('its muscles give the T pose back: the limbs\' worst, degrees', tpose_errors(names), 6.0)

print('all passed' if not FAILS else '%d FAILED: %s' % (len(FAILS), ', '.join(FAILS)))
sys.exit(1 if FAILS else 0)
