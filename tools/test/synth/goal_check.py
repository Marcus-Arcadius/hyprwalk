# goal_check.py: how far the converter's humanoid maths (HumanAxes.pose) puts an avatar's feet and hands from a humanoid
# clip's own IK goal curves (LeftFootT/Q ... RightHandT/Q). Unity computes those on import, so they're the nearest thing
# to ground truth without Unity. Goals are in the body frame (RootT, RootQ) and human scale; a foot's is its sole, the
# T-pose ankle height below the ankle (HumanAxes.plant). The avatar's own proportions add differences too. Clips and
# avatars aren't in this repo.
#   blender -b --factory-startup --python-exit-code 1 -P goal_check.py -- AVATAR_INPUT CLIP.anim... [--frames N]
import sys, os, math, tempfile
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))  # tools/
import unity2hypr3d as u
from mathutils import Vector

argv = sys.argv[sys.argv.index('--') + 1:]
frames = 120
if '--frames' in argv:
    k = argv.index('--frames')
    frames = int(argv[k + 1])
    del argv[k:k + 2]
db = u.DB(tempfile.mkdtemp(prefix='goal_check'))
db.add_input(argv[0])
found = u.find_avatars(db)[0]
av = u.Avatar(db, found)
human = u.Analysis(db, av).humanoid()
try:
    axes, names = u.human_tpose(db, av, human, None)
except TypeError:  # no skeleton in model settings: import the models
    from types import SimpleNamespace
    bd = u.Build(db, av, SimpleNamespace(max_texture=256, blend=None, output=None))
    bd.import_models()
    bd.compute(human)
    axes, names = u.human_tpose(db, av, human, bd.U)
if axes is None:
    sys.exit('%s: no humanoid T pose in its model settings' % argv[0])
print('avatar %s: human scale %.3f m' % (av.name, axes.scale))
for f in argv[1:]:
    uf = u.UFile(f)
    clip = u.HumanClip(uf.get(uf.main(74))[1])
    print('== %s: %.2f s, goals for %s' % (os.path.basename(f), clip.length, ', '.join(
        n for n, b in u.GOALS.items() if b in clip.goals_at(0.0)) or 'nothing'))
    stats = {}
    for k in range(frames + 1):
        t = clip.length * k / frames
        mus, rt, rq = clip.at(t)
        world = axes.world_of(axes.pose(mus, rt, rq))
        for b, (T, Q) in clip.goals_at(t).items():
            if b not in axes.human or b not in axes.post:
                continue
            W = world[axes.human[b]]
            turn = W.to_quaternion() @ axes.post[b]  # its muscle frame, as the goal's turn
            p = W.translation
            if b in (5, 6):  # a foot: its sole
                p = p + turn @ Vector((axes.world[axes.human[b]].translation.y, 0.0, 0.0))
            ours = rq.inverted() @ (p - rt * axes.scale)
            d = (ours - T * axes.scale).length
            a = math.degrees(2 * math.acos(min(1.0, abs((rq.inverted() @ turn).dot(Q)))))
            s = stats.setdefault(b, [0.0, 0.0, 0.0, 0.0, 0])
            s[0] += d
            s[1] = max(s[1], d)
            s[2] += a
            s[3] = max(s[3], a)
            s[4] += 1
    for b, (sd, wd, sa, wa, n) in sorted(stats.items()):
        print('  %-9s %5.1f cm on average, %5.1f at worst; turned %4.1f degrees on average, %5.1f at worst' % (
            u.HUMAN_BONES[b], 100 * sd / n, 100 * wd, sa / n, wa))
