"""h3d_rig: the Blender rig the attacks are made with: IK controls on a humanoid imported from a glTF (Hatsune Miku NT's
GLB; BONES names her bones), and key poses in her own terms. See README.md here.

Her right is -X, ahead -Y, up +Z (an imported glTF facing +Z). A pose is a dict; what it leaves out is at rest:
  spine, chest, neck, head: (yaw, pitch, roll) degrees, the bone's whole turn in the world: yaw to her left, pitch
      bending ahead, roll leaning to her left
  shoulder.R/.L: (ahead, up) degrees, the collarbone's own turn on the chest's
  wrist.R/.L: (out, up, ahead) meters from the chest's joint, in the chest's turned frame (out: that arm's side);
      ("world", v): the same, not turned; ("reach", v, f): f of the arm's length from its shoulder toward v (not turned)
  elbow.R/.L: where the elbow points (the IK pole), given like a wrist
  along.R/.L, palm.R/.L: the directions the hand points (wrist to knuckles) and its palm faces, given like a wrist
Keys: key(frame, pose) or key(frame, mix(A, B, trunk=u, R=u, L=u)): A to B, each part that far along (arms in straight
lines in the world, hands turning the short way)
"""
import math

import bpy
from mathutils import Matrix, Quaternion, Vector

ARMATURE = "Armature"
SIDE = {"R": -1.0, "L": 1.0}  # the side's x sign
BONES = {"hips": "Hips", "spine": "Spine", "chest": "Chest", "neck": "Neck", "head": "Head"}
for _s in "RL":
    BONES.update({f"shoulder.{_s}": f"Shoulder.{_s}", f"upper.{_s}": f"Upper Arm.{_s}", f"lower.{_s}": f"Lower Arm.{_s}",
                  f"hand.{_s}": f"Hand.{_s}"})
TRUNK = ["spine", "chest", "neck", "head"]
KEYED = TRUNK + ["shoulder.R", "shoulder.L"]
PARENT = {"spine": "hips", "chest": "spine", "neck": "chest", "head": "neck", "shoulder.R": "chest", "shoulder.L": "chest"}
FINGERS = ["Thumb", "Index", "Middle", "Ring", "Little"]
SEGS = ["Proximal", "Intermediate", "Distal"]


def ob():
    return bpy.data.objects[ARMATURE]


def bone(k):
    return ob().data.bones[BONES[k]]


def pbone(k):
    return ob().pose.bones[BONES[k]]


def rest_q(k):
    return bone(k).matrix_local.to_quaternion()


def body(v, side="R"):
    """(out, up, ahead) in her terms -> Blender's axes"""
    o, u, f = v
    return Vector((SIDE[side] * o, -f, u))


def turn(yaw=0.0, pitch=0.0, roll=0.0):
    """a turn in her terms (degrees): yaw to her left, pitch ahead, roll to her left"""
    r = math.radians
    return Quaternion((0, 0, 1), r(yaw)) @ Quaternion((1, 0, 0), r(pitch)) @ Quaternion((0, 1, 0), r(roll))


def collarbone(side, ahead=0.0, up=0.0):
    s = SIDE[side]
    return Quaternion((0, 0, 1), math.radians(-s * ahead)) @ Quaternion((0, 1, 0), math.radians(-s * up))


def frame_from(along, palm):
    """rotation from the hand bone's rest axes (y along it, palm -z: palms down in the T pose) to these"""
    y = along.normalized()
    z = -(palm - y * palm.dot(y)).normalized()
    x = y.cross(z)
    return Matrix((x, y, z)).transposed().to_quaternion()


def arm_length(s):
    """shoulder to wrist, at rest (meters)"""
    return (bone(f"lower.{s}").head_local - bone(f"upper.{s}").head_local).length + \
        (bone(f"hand.{s}").head_local - bone(f"lower.{s}").head_local).length


def ctl_name(kind, side):
    return f"CTL.{kind}.{side}"


def setup(fps=60, start=0, end=45):
    """creates per arm an IK target at the wrist (its rotation drives the hand) and an elbow pole"""
    sc = bpy.context.scene
    sc.render.fps = fps
    sc.render.fps_base = 1
    sc.frame_start, sc.frame_end = start, end
    a = ob()
    coll = a.users_collection[0]
    for s in "RL":
        for kind, shape, size in (("wrist", "CUBE", 0.035), ("elbow", "SPHERE", 0.03)):
            n = ctl_name(kind, s)
            e = bpy.data.objects.get(n)
            if e is None:
                e = bpy.data.objects.new(n, None)
                coll.objects.link(e)
            e.empty_display_type = shape
            e.empty_display_size = size
            e.rotation_mode = 'QUATERNION'
            e.parent = a
            e.show_in_front = True
        lower = pbone(f"lower.{s}")
        ik = lower.constraints.get("IK") or lower.constraints.new('IK')
        ik.name = "IK"
        ik.target = bpy.data.objects[ctl_name("wrist", s)]
        ik.pole_target = bpy.data.objects[ctl_name("elbow", s)]
        ik.chain_count = 2
        ik.use_tail = True
        ik.use_stretch = False
        # pre-bent: a straight arm has no bend direction; the pole angles match this rig's bone rolls
        lower.rotation_quaternion = Quaternion((0, 0, 1), math.radians(25 if s == "R" else -25))
        ik.pole_angle = 0.0 if s == "R" else math.pi
        hand = pbone(f"hand.{s}")
        cr = hand.constraints.get("Copy Rotation") or hand.constraints.new('COPY_ROTATION')
        cr.name = "Copy Rotation"
        cr.target = bpy.data.objects[ctl_name("wrist", s)]
        cr.mix_mode = 'REPLACE'
        cr.target_space = cr.owner_space = 'WORLD'
    for k in BONES:
        pbone(k).rotation_mode = 'QUATERNION'


def basis(k, q_world, q_parent):
    """pose channel rotation for world turn q_world, given the parent's q_parent"""
    r = rest_q(k)
    return (r.inverted() @ q_parent.inverted() @ q_world @ r).normalized()


# --- poses

def trunk_of(pose):
    """the trunk's numbers: per bone its angles, per collarbone its turn"""
    out = {k: tuple(pose.get(k, (0, 0, 0))) for k in TRUNK}
    for s in "RL":
        out[f"shoulder.{s}"] = tuple(pose.get(f"shoulder.{s}", (0, 0)))
    return out


def solve_trunk(t):
    """(world turns, joint positions) of the trunk's bones for its numbers"""
    q = {"hips": Quaternion()}
    for k in TRUNK:
        q[k] = turn(*t[k])
    for s in "RL":
        q[f"shoulder.{s}"] = q["chest"] @ collarbone(s, *t[f"shoulder.{s}"])
    j = {"hips": bone("hips").head_local.copy()}
    for k in KEYED:
        p = PARENT[k]
        j[k] = j[p] + q[p] @ (bone(k).head_local - bone(p).head_local)
    for s in "RL":
        j[f"upper.{s}"] = j[f"shoulder.{s}"] + q[f"shoulder.{s}"] @ (bone(f"upper.{s}").head_local - bone(f"shoulder.{s}").head_local)
    return q, j


def arm_of(pose, s, q, j):
    """(wrist, elbow pole, along, palm) in the world for arm s of the pose, given the trunk's q, j"""
    o, qc = j["chest"], q["chest"]

    def at(key, point=True):
        v = pose[f"{key}.{s}"]
        if isinstance(v[0], str) and v[0] == "reach":
            return j[f"upper.{s}"] + body(v[1], s).normalized() * (arm_length(s) * v[2])
        world = isinstance(v[0], str)
        d = body(v[1] if world else v, s)
        d = d if world else qc @ d
        return o + d if point else d.normalized()

    return at("wrist"), at("elbow"), at("along", False), at("palm", False)


class mix:
    """a pose part way from A to B: each part (trunk, R, L) that far along"""

    def __init__(self, a, b, trunk=0.0, R=0.0, L=0.0):
        self.a, self.b, self.u = a, b, {"trunk": trunk, "R": R, "L": L}


def lerp(a, b, u):
    return tuple(x + (y - x) * u for x, y in zip(a, b))


def resolve(p):
    """(the trunk's numbers, per arm (wrist, elbow, along, palm) in the world) for a pose or a mix"""
    if not isinstance(p, mix):
        t = trunk_of(p)
        q, j = solve_trunk(t)
        return t, {s: arm_of(p, s, q, j) for s in "RL"}
    ta, tb = trunk_of(p.a), trunk_of(p.b)
    t = {k: lerp(ta[k], tb[k], p.u["trunk"]) for k in ta}
    q, j = solve_trunk(t)
    arms = {}
    for s in "RL":
        u = p.u[s]
        wa, ea, la, pa = arm_of(p.a, s, q, j)
        wb, eb, lb, pb = arm_of(p.b, s, q, j)
        arms[s] = (wa.lerp(wb, u), ea.lerp(eb, u), la.slerp(lb, u), pa.slerp(pb, u))
    return t, arms


def key(frame, p, interp='BEZIER'):
    """set a pose (or a mix) and key it at frame"""
    t, arms = resolve(p)
    q, _ = solve_trunk(t)
    keyed = []
    for k in KEYED:
        pb = pbone(k)
        pb.rotation_quaternion = basis(k, q[k], q[PARENT[k]])
        keyed.append((pb, "rotation_quaternion"))
    for s in "RL":
        w = bpy.data.objects[ctl_name("wrist", s)]
        e = bpy.data.objects[ctl_name("elbow", s)]
        wrist, elbow, along, palm = arms[s]
        w.location = wrist
        e.location = elbow
        r = frame_from(along, palm)
        # take the short way from the previous key: quaternion curves interpolate per component
        if r.dot(w.rotation_quaternion) < 0:
            r.negate()
        w.rotation_quaternion = r
        keyed += [(w, "location"), (w, "rotation_quaternion"), (e, "location")]
    for thing, path in keyed:
        thing.keyframe_insert(path, frame=frame, group="attack")
    set_interp(frame, interp)


def fcurves():
    out = []
    for thing in [ob()] + [bpy.data.objects[ctl_name(k, s)] for k in ("wrist", "elbow") for s in "RL"]:
        ad = thing.animation_data
        if ad and ad.action:
            out += list(action_fcurves(ad.action))
    return out


def action_fcurves(action):
    """the action's F-curves (Blender 5's layered actions keep them in channelbags)"""
    if hasattr(action, "layers") and action.layers:
        for layer in action.layers:
            for strip in layer.strips:
                for bag in strip.channelbags:
                    yield from bag.fcurves
    elif hasattr(action, "fcurves"):
        yield from action.fcurves


def set_interp(frame, interp, easing=None):
    for fc in fcurves():
        for kp in fc.keyframe_points:
            if abs(kp.co.x - frame) < 0.01:
                kp.interpolation = interp
                if easing:
                    kp.easing = easing


def clear():
    for thing in [ob()] + [bpy.data.objects.get(ctl_name(k, s)) for k in ("wrist", "elbow") for s in "RL"]:
        if thing and thing.animation_data:
            thing.animation_data_clear()
    for k in KEYED:
        pbone(k).rotation_quaternion = Quaternion()
    for s in "RL":
        w = bpy.data.objects.get(ctl_name("wrist", s))
        if w:
            w.rotation_quaternion = Quaternion()


def fist(curl=(80, 100, 70), thumb=(20, 35, 35)):
    """curls both hands into rough fists for viewing, unkeyed (the plugin makes the avatar's own fists)"""
    for s in "RL":
        axis = Vector((0, -1, 0)) * (1 if s == "R" else -1)
        for f in FINGERS:
            for i, seg in enumerate(SEGS):
                pb = ob().pose.bones.get(f"{f} {seg}.{s}")
                if pb:
                    pb.rotation_mode = 'QUATERNION'
                    r = pb.bone.matrix_local.to_quaternion()
                    pb.rotation_quaternion = Quaternion(r.inverted() @ axis, math.radians((thumb if f == "Thumb" else curl)[i]))


def eval_points():
    """where the wrists and elbows are as posed now (Blender world)"""
    bpy.context.view_layer.update()
    a = ob()
    out = {}
    for s in "RL":
        out[f"wrist.{s}"] = tuple(round(v, 4) for v in a.matrix_world @ a.pose.bones[BONES[f"hand.{s}"]].head)
        out[f"elbow.{s}"] = tuple(round(v, 4) for v in a.matrix_world @ a.pose.bones[BONES[f"lower.{s}"]].head)
    return out


def fp_view(fov_y=70.0, aspect=16 / 9):
    """where the wrists and fists show in the plugin's first person view (from her eyes, looking ahead), 0..1 across and
    down, and the eye position"""
    bpy.context.view_layer.update()
    a = ob()
    pb = a.pose.bones
    eyes = (a.matrix_world @ pb["Left Eye"].head + a.matrix_world @ pb["Right Eye"].head) * 0.5
    t = math.tan(math.radians(fov_y) / 2)
    out = {"eyes": tuple(round(v, 3) for v in eyes)}
    for s in "RL":
        w = a.matrix_world @ pb[BONES[f"hand.{s}"]].head
        f = a.matrix_world @ pb[f"Middle Proximal.{s}"].head
        fist = w + (f - w).normalized() * 0.09
        for name, p in (("wrist", w), ("fist", fist)):
            d = p - eyes
            right, up, fwd = -d.x, d.z, -d.y
            out[f"{name}.{s}"] = (round(0.5 + 0.5 * right / fwd / (t * aspect), 3), round(0.5 - 0.5 * up / fwd / t, 3)) if fwd > 0.02 else None
    return out
