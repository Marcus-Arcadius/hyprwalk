"""h3d_walk: the walk and run cycles of hypr3d's avatars, made in Blender: assets/walk.vrma and assets/run.vrma.

hypr3d steps the feet itself; these cycles move the rest of the body in step. Each is one stride in place, the left
heel landing at its start and the right half way; its legs step like hypr3d's, for previewing, but hypr3d keeps its own.

In Blender, with a humanoid armature in the scene (bones named by its hypr3d settings file's "humanoid"):
    import h3d_walk
    h3d_walk.build(settings)        # key the actions "Walk" and "Run" on the armature from CYCLES
    h3d_walk.show("Run")            # that one on the armature, looping
    h3d_walk.export()               # both as VRM animations into the repo's assets/ (then ./build.sh)
    h3d_walk.export(folder, prefix="Miku.")   # or next to an avatar, for its settings file's "walk" (no rebuild)

Edit the keys and export again, or change the numbers below and build again. Angles in degrees, distances in meters,
phases in strides; her left is +X, ahead -Y.
"""
import json
import math
import os
import sys

import bpy
from mathutils import Matrix, Quaternion, Vector

HERE = os.path.dirname(os.path.abspath(__file__))
ASSETS = os.path.normpath(os.path.join(HERE, "..", "..", "assets"))
FPS = 60

# ---------------------------------------------------------------- the cycles

# frames: stride length (hypr3d plays it by its own step phase, at any speed); duty: share of it a foot is down; stride:
# foot travel (m); drop: mean hips drop; bob: twice a stride, highest at bobTop; sway: toward the standing foot; wide:
# feet out from the middle; hips yaw (the leading leg's hip ahead), roll (down on the swing side, most at rollAt),
# pitch; arms hang `out`, swing `ahead` and `back` `lag` after the legs, elbow `bend0` to `bend1` coming ahead, forearm
# and hand trailing by `follow` and `drag`, `inward` toward the middle coming ahead (running)
CYCLES = {
    "Walk": dict(
        frames=54, duty=0.6, stride=1.4, drop=0.028, bob=0.011, bobTop=0.3, sway=0.024, wide=0.064,
        hips=dict(yaw=7.0, roll=5.0, rollAt=0.15, pitch=4.0),
        spine=dict(pitch=3.0, turn=0.25), chest=dict(pitch=4.0, turn=-0.85, roll=2.0),
        neck=dict(pitch=1.5), head=dict(pitch=-1.0, nod=1.2, tilt=1.6),
        arm=dict(out=11.0, ahead=19.0, back=13.0, lag=0.05, bend0=11.0, bend1=30.0, follow=0.07, drag=0.1,
                 flex=10.0, inward=0.0),
        collar=dict(ahead=3.5, up=1.5),
        legs="walk",
    ),
    "Run": dict(
        frames=38, duty=0.35, stride=2.8, drop=0.062, bob=0.022, bobTop=0.37, sway=0.011, wide=0.05,
        hips=dict(yaw=12.0, roll=4.0, rollAt=0.12, pitch=7.0),
        spine=dict(pitch=10.0, turn=0.2), chest=dict(pitch=12.0, turn=-0.95, roll=2.5),
        neck=dict(pitch=6.0), head=dict(pitch=0.0, nod=1.5, tilt=0.8),
        arm=dict(out=13.0, ahead=36.0, back=32.0, lag=0.03, bend0=95.0, bend1=80.0, follow=0.05, drag=0.08,
                 flex=4.0, inward=0.45),
        collar=dict(ahead=6.0, up=3.0),
        legs="run",
    ),
}

# hypr3d's step curves (src/avatar.cpp): foot pitch (radians, toes down > 0) in stance and swing; lift in leg lengths
CURVES = {
    "walk": dict(
        stance=[(0, -0.26), (0.12, -0.03), (0.2, 0), (0.5, 0), (0.75, 0.24), (1, 0.9)],
        swing=[(0, 0.9), (0.2, 0.4), (0.42, 0.03), (0.62, -0.1), (0.85, -0.16), (1, -0.26)],
        lift=[(0, 0), (0.15, 0.07), (0.3, 0.065), (0.5, 0.035), (0.75, 0.025), (0.9, 0.015), (1, 0)],
    ),
    "run": dict(
        stance=[(0, -0.13), (0.1, 0), (0.4, 0), (0.7, 0.32), (1, 0.95)],
        swing=[(0, 0.95), (0.25, 0.75), (0.5, 0.38), (0.75, 0.02), (0.9, -0.08), (1, -0.13)],
        lift=[(0, 0), (0.2, 0.2), (0.38, 0.27), (0.55, 0.22), (0.75, 0.11), (0.9, 0.04), (1, 0)],
    ),
}
KEYS = 16  # keys per stride, plus the end (= the start)

# ---------------------------------------------------------------- the frame above and Blender's
# hypr3d poses in its own frame: +x the body's left, +y up, +z ahead; Blender's here: +X left, +Z up, -Y ahead

M_FB = Matrix(((1, 0, 0), (0, 0, -1), (0, 1, 0)))  # frame above -> Blender: x -> X, y -> Z, z -> -Y


def fb(v):
    return M_FB @ Vector(v)


def fbq(q):
    """a turn in the frame above, as Blender's"""
    m = M_FB @ q.to_matrix() @ M_FB.transposed()
    return m.to_quaternion()


def axis_angle(axis, a):
    return Quaternion(Vector(axis).normalized(), a)


def turn(yaw=0.0, pitch=0.0, roll=0.0):
    """a turn in degrees (frame above): yaw to her left, pitch bending ahead, roll leaning to her left"""
    r = math.radians
    return axis_angle((0, 1, 0), r(yaw)) @ axis_angle((1, 0, 0), r(pitch)) @ axis_angle((0, 0, -1), r(roll))


def arc(a, b):
    a, b = Vector(a).normalized(), Vector(b).normalized()
    return a.rotation_difference(b)


def frame_to(t0, h0, t, h):
    """the turn taking t0 to t, and h0 (square to t0) as near to h as it goes"""
    t = Vector(t).normalized()
    hp = Vector(h) - t * Vector(h).dot(t)
    if hp.length < 1e-6:
        return arc(t0, t)
    hp.normalize()
    A = Matrix((t, hp, t.cross(hp))).transposed()
    t0, h0 = Vector(t0).normalized(), Vector(h0).normalized()
    B = Matrix((t0, h0, t0.cross(h0))).transposed()
    return (A @ B.transposed()).to_quaternion()


def through(keys, s):
    """Catmull-Rom through (t, v) keys, level at the ends (as hypr3d's through())"""
    if s <= keys[0][0]:
        return keys[0][1]
    if s >= keys[-1][0]:
        return keys[-1][1]
    i = 0
    while i + 2 < len(keys) and s > keys[i + 1][0]:
        i += 1
    (t0, v0), (t1, v1) = keys[i], keys[i + 1]
    dt = t1 - t0
    u = (s - t0) / dt

    def slope(j):
        if j == 0 or j + 1 >= len(keys):
            return 0.0
        return (keys[j + 1][1] - keys[j - 1][1]) / (keys[j + 1][0] - keys[j - 1][0])

    u2, u3 = u * u, u * u * u
    return (2 * u3 - 3 * u2 + 1) * v0 + (u3 - 2 * u2 + u) * slope(i) * dt + (-2 * u3 + 3 * u2) * v1 + (u3 - u2) * slope(i + 1) * dt


def min_jerk(s):
    s = min(max(s, 0.0), 1.0)
    return s * s * s * (10 + s * (-15 + 6 * s))


# ---------------------------------------------------------------- the armature

HUMAN = ["Hips", "Spine", "Chest", "UpperChest", "Neck", "Head"] + [
    side + b for side in ("Left", "Right") for b in ("Shoulder", "UpperArm", "LowerArm", "Hand", "UpperLeg", "LowerLeg",
                                                     "Foot", "Toes")]


class Body:
    """an armature's humanoid bones (by a settings file's "humanoid"), their rest positions (frame above, meters from
    the hips' joint) and leg and foot measures"""

    def __init__(self, armature, humanoid):
        self.ob = bpy.data.objects[armature]
        bones = self.ob.data.bones
        self.name = {}
        for k, v in humanoid.items():
            key = k.replace(" ", "")
            for h in HUMAN:
                if key.lower() == h.lower() and v in bones:
                    self.name[h] = v
        W = self.ob.matrix_world
        self.rest_m = {h: W @ bones[n].matrix_local for h, n in self.name.items()}
        hips = self.rest_m["Hips"].to_translation()
        self.hips = hips
        inv = M_FB.transposed()
        self.at = {h: inv @ (m.to_translation() - hips) for h, m in self.rest_m.items()}
        self.tail = {h: inv @ (W @ bones[n].tail_local - hips) for h, n in self.name.items()}
        L = lambda a, b: (self.at[b] - self.at[a]).length
        self.thigh = [L("LeftUpperLeg", "LeftLowerLeg"), L("RightUpperLeg", "RightLowerLeg")]
        self.shin = [L("LeftLowerLeg", "LeftFoot"), L("RightLowerLeg", "RightFoot")]
        # ground (z = 0), ankle heights, foot balls (toe joints or foot ends) ahead of the ankles, heels 1 cm behind
        self.ground = -hips.z
        self.ankle_up, self.ball = [], []
        for s in ("Left", "Right"):
            f = self.at[s + "Foot"]
            t = self.at.get(s + "Toes", self.tail[s + "Foot"])
            self.ankle_up.append(max(0.02, f.y - self.ground))
            self.ball.append(Vector((0, -self.ankle_up[-1], max(0.03, t.z - f.z))))
        self.heel = [Vector((0, -u, -0.01)) for u in self.ankle_up]

    def pose_bone(self, h):
        return self.ob.pose.bones[self.name[h]]


def humanoid_of(settings):
    with open(settings, encoding="utf-8") as f:
        return json.load(f)["humanoid"]


# ---------------------------------------------------------------- posing (the frame above)

PARENT = {"Spine": "Hips", "Chest": "Spine", "UpperChest": "Chest", "Neck": "UpperChest", "Head": "Neck"}


def up_chain(B, h):
    """the humanoid bone above h that the armature has"""
    p = {"Spine": "Hips", "Chest": "Spine", "UpperChest": "Chest", "Neck": "UpperChest", "Head": "Neck",
         "LeftShoulder": "UpperChest", "RightShoulder": "UpperChest", "LeftUpperLeg": "Hips", "RightUpperLeg": "Hips"}
    for s in ("Left", "Right"):
        for a, c in (("Shoulder", "UpperArm"), ("UpperArm", "LowerArm"), ("LowerArm", "Hand"), ("UpperLeg", "LowerLeg"),
                     ("LowerLeg", "Foot"), ("Foot", "Toes")):
            p[s + c] = s + a
    u = p.get(h)
    while u is not None and u not in B.name:
        u = p.get(u)
    return u


def pose_at(B, c, p, extra=(0.0, 0.0)):
    """the body at phase p of cycle c: per-bone turns from rest (frame above) and the hips' move; `extra`: radians
    further out per arm, to clear the dress"""
    r = math.radians
    T = {}
    sin, cos, tau = math.sin, math.cos, 2 * math.pi
    hp = c["hips"]
    # hips: yaw (the leading leg's hip ahead), roll (down on the swinging leg's side), pitch ahead
    T["Hips"] = turn(-hp["yaw"] * cos(tau * p), hp["pitch"], -hp["roll"] * sin(tau * (p + 0.25 - hp["rollAt"])))
    move = Vector((c["sway"] * sin(tau * p), -c["drop"] + c["bob"] * cos(2 * tau * (p - c["bobTop"])), 0))
    # trunk: the chest counter-turns the hips (shoulders square), leans ahead and rolls against the hips' drop; the head
    # nods just after the bob and tilts with the sway
    hy = -hp["yaw"] * cos(tau * p)
    T["Spine"] = turn(hy * c["spine"]["turn"], c["spine"]["pitch"], hp["roll"] * 0.3 * sin(tau * (p + 0.25 - hp["rollAt"])))
    T["Chest"] = turn(hy * c["chest"]["turn"], c["chest"]["pitch"], c["chest"]["roll"] * sin(tau * (p + 0.25 - hp["rollAt"])))
    T["UpperChest"] = T["Chest"]
    T["Neck"] = turn(hy * c["chest"]["turn"] * 0.3, c["neck"]["pitch"], 0)
    T["Head"] = turn(0, c["head"]["pitch"] + c["head"]["nod"] * cos(2 * tau * (p - c["bobTop"] - 0.04)),
                     c["head"]["tilt"] * sin(tau * (p - 0.05)))
    # arms swing just after the legs (left ahead as the right foot lands); elbows bend coming ahead; forearm, hand trail
    a = c["arm"]
    for s, side in enumerate(("Left", "Right")):
        sx = 1.0 if s == 0 else -1.0
        sw = lambda q: -cos(tau * (q + 0.5 * s - a["lag"]))  # 1: right ahead
        w = sw(p)
        # linear in w, so further ahead than back without a kink at the middle
        ahead = r(0.5 * (a["ahead"] + a["back"]) * w + 0.5 * (a["ahead"] - a["back"]))
        out = r(a["out"]) * (1 - a["inward"] * 0.5 * (1 + w))
        u = axis_angle((1, 0, 0), -ahead) @ Vector((sx * sin(out), -cos(out), 0))
        wf = sw(p - a["follow"])
        bend = r(a["bend0"] + (a["bend1"] - a["bend0"]) * 0.5 * (1 + wf))
        # forearm bent ahead from the upper arm (in toward her middle when running)
        v = Vector((0, 0, 1)) - u * u.z
        v.normalize()
        mid = Vector((-sx, 0, 0)) - u * (-sx * u.x)
        mid.normalize()
        tilt = a["inward"] * 0.5 * (1 + wf)
        v = (v * math.cos(tilt) + mid * math.sin(tilt)).normalized()
        f = (u * math.cos(bend) + v * math.sin(bend)).normalized()
        # hand flexed toward the palm (facing her side), more as it trails
        wd = sw(p - a["drag"])
        palm = Vector((-sx, 0, 0))
        palm = (palm - f * palm.dot(f)).normalized()
        flex = r(a["flex"] * (0.6 + 0.4 * wd))
        along = (f * math.cos(flex) + palm * math.sin(flex)).normalized()
        if extra[s]:  # rotate out about the ahead axis
            q = axis_angle((0, 0, 1), sx * extra[s])
            u, f, along, palm = q @ u, q @ f, q @ along, q @ palm
        T[side + "UpperArm"], T[side + "LowerArm"], T[side + "Hand"] = arm_turns(s, u, f, along, palm, T["Chest"])
        col = c["collar"]
        T[side + "Shoulder"] = T["Chest"] @ axis_angle((0, 1, 0), -sx * r(col["ahead"]) * w) @ \
            axis_angle((0, 0, 1), sx * r(col["up"]) * w * w)
    # legs: feet placed as hypr3d steps them; hip joints from the posed pelvis
    curves = CURVES[c["legs"]]
    pelvis = move
    for s, side in enumerate(("Left", "Right")):
        sx = 1.0 if s == 0 else -1.0
        q = (p + 0.5 * s) % 1.0  # 0: this foot lands
        duty = c["duty"]
        if q < duty:  # stance: slides back stride * duty under the body
            u_ = q / duty
            z = c["stride"] * duty * (0.5 - u_)
            pitch = through(curves["stance"], u_)
            lift = 0.0
        else:  # in the air: ahead again
            u_ = (q - duty) / (1 - duty)
            z0, z1 = -c["stride"] * duty * 0.5, c["stride"] * duty * 0.5
            z = z0 + (z1 - z0) * min_jerk(u_)
            pitch = through(curves["swing"], u_)
            lift = through(curves["lift"], u_) * (B.thigh[s] + B.shin[s])
        spot = Vector((sx * c["wide"], B.ground, z))
        T[side + "UpperLeg"], T[side + "LowerLeg"], T[side + "Foot"] = leg_turns(B, s, spot, pitch, lift, pelvis, T["Hips"])
        if side + "Toes" in B.name:
            # toes stay flat as the heel rises, then go with the foot
            T[side + "Toes"] = axis_angle((1, 0, 0), min(pitch, max(0.0, pitch - 0.9))) if q < duty else T[side + "Foot"]
    return T, move


def arm_turns(s, u, f, along, palm, chest):
    """upper arm, forearm and hand turns from the T pose for directions in the chest's frame (as hypr3d's CPoser: the
    forearm takes half the hand's twist)"""
    t0 = Vector((1.0 if s == 0 else -1.0, 0, 0))
    h0 = Vector((0, -1.0 if s == 0 else 1.0, 0))
    hinge = u.cross(f)
    if hinge.length < 1e-6:  # straight: the rest hinge turned with the arm
        hinge = arc(t0, u) @ h0
        hinge = hinge - u * hinge.dot(u)
    hinge.normalize()
    ua = frame_to(t0, h0, u, hinge)
    bend = u.angle(f)
    la = axis_angle(hinge, bend) @ ua
    n0 = Vector((0, -1, 0))
    hand = frame_to(t0, n0, along, palm)
    rel = la.inverted() @ hand
    phi = 2 * math.atan2(Vector((rel.x, rel.y, rel.z)).dot(t0), rel.w)
    la2 = la @ axis_angle(t0, phi * 0.5)
    return chest @ ua, chest @ la2, chest @ hand


def leg_turns(B, s, spot, pitch, lift, pelvis, hipsT):
    """thigh, shin and foot turns (two-bone IK): ankle over the foot's spot, pivoting on heel or ball, knee ahead"""
    side = "Left" if s == 0 else "Right"
    hip = pelvis + hipsT @ (B.at[side + "UpperLeg"])
    pivot = (B.heel[s] if pitch < 0 else B.ball[s]).copy()
    pivot.x = 0
    foot = axis_angle((1, 0, 0), pitch)  # toes down > 0 (ahead is +z)
    ankle = Vector((spot.x, spot.y + B.ankle_up[s] + lift, spot.z)) + pivot - foot @ pivot
    a, b = B.thigh[s], B.shin[s]
    d = ankle - hip
    dist = min(max(d.length, abs(a - b) + 1e-4), (a + b) * 0.999)
    dirn = d.normalized()
    cosA = (a * a + dist * dist - b * b) / (2 * a * dist)
    knee_hint = hipsT @ Vector((0, 0, 1))
    n = knee_hint - dirn * knee_hint.dot(dirn)
    n.normalize()
    mid = hip + (dirn * cosA + n * math.sqrt(max(0.0, 1 - cosA * cosA))) * a
    uth = (mid - hip).normalized()
    ush = (hip + dirn * dist - mid).normalized()
    t0, h0 = Vector((0, -1, 0)), Vector((1, 0, 0))
    hinge = uth.cross(ush)
    hinge = hinge.normalized() if hinge.length > 1e-6 else (arc(t0, uth) @ h0)
    if (arc(t0, uth) @ h0).dot(hinge) < 0:
        hinge = -hinge
    th = frame_to(t0, h0, uth, hinge)
    sh = axis_angle(hinge, uth.angle(ush)) @ th
    return th, sh, foot


# ---------------------------------------------------------------- keeping the arms out of the dress

ARMS = ("Shoulder", "UpperArm", "LowerArm", "Hand", "Thumb", "Index", "Middle", "Ring", "Little", "Mic")


class Dress:
    """body radius round the hips' vertical line per height and bearing (frame above), from vertices weighted most to
    hips, spine, chest or legs below the chest; matches hypr3d's SBodyClearance"""

    ROW, BINS = 0.02, 72

    def __init__(self, B):
        ob = B.ob
        top = B.at["Chest"].y if "Chest" in B.at else 0.3
        self.y0 = -1.2
        rows = int(round((top - self.y0) / self.ROW)) + 1
        self.R = [[0.0] * self.BINS for _ in range(rows)]
        inv = M_FB.transposed()
        keep = set()
        for h, n in B.name.items():
            if h in ("Hips", "Spine", "Chest", "UpperChest") or "Leg" in h:
                keep.add(n)
        for child in ob.children:
            if child.type != "MESH":
                continue
            names = {g.index: g.name for g in child.vertex_groups}
            W = child.matrix_world
            for v in child.data.vertices:
                if not v.groups:
                    continue
                g = max(v.groups, key=lambda x: x.weight)
                n = names.get(g.group, "")
                if n not in keep and not n.startswith("Strap"):
                    continue
                p = inv @ (W @ v.co - B.hips)
                if p.y > top:
                    continue
                i = int((p.y - self.y0) / self.ROW)
                if not 0 <= i < rows:
                    continue
                k = int((math.atan2(p.z, p.x) + math.pi) / (2 * math.pi) * self.BINS) % self.BINS
                self.R[i][k] = max(self.R[i][k], math.hypot(p.x, p.z))

    def depth(self, points):
        """deepest penetration of (point, radius) pairs into the body, meters (< 0: clear)"""
        most = -1.0
        for p, r in points:
            rho = math.hypot(p.x, p.z)
            i0, i1 = int((p.y - r - self.y0) / self.ROW), int((p.y + r - self.y0) / self.ROW)
            k = int((math.atan2(p.z, p.x) + math.pi) / (2 * math.pi) * self.BINS)
            dk = 1 + int(math.asin(min(1.0, r / max(rho, 1e-3))) / (2 * math.pi) * self.BINS)
            R = 0.0
            for i in range(max(i0, 0), min(i1, len(self.R) - 1) + 1):
                for d in range(-dk, dk + 1):
                    R = max(R, self.R[i][(k + d) % self.BINS])
            if R > 0:
                most = max(most, R + 0.01 + r - rho)
        return most


def joints_of(B, T, move):
    """posed joint positions (frame above, from the hips' rest joint)"""
    at = {"Hips": Vector(move)}

    def put(h):
        if h in at:
            return at[h]
        up = up_chain(B, h)
        if up is None:
            at[h] = B.at[h].copy()
        else:
            at[h] = put(up) + T.get(up, Quaternion()) @ (B.at[h] - B.at[up])
        return at[h]

    for h in B.name:
        put(h)
    return at


def arm_points(B, T, move, s, forearm=0.035, hand=0.03):
    """forearm and hand as (point, radius) pairs in the hips' frame"""
    side = "Left" if s == 0 else "Right"
    J = joints_of(B, T, move)
    e, w = J[side + "LowerArm"], J[side + "Hand"]
    length = (B.tail[side + "Hand"] - B.at[side + "Hand"]).length
    tip = w + T[side + "Hand"] @ (B.tail[side + "Hand"] - B.at[side + "Hand"]) * (0.09 / max(length, 1e-3))
    pts = [(e + (w - e) * (k / 4), forearm) for k in range(5)] + [(w + (tip - w) * (k / 2), hand) for k in (1, 2)]
    back = T["Hips"].inverted()
    return [(back @ (p - Vector(move)), r) for p, r in pts]


# ---------------------------------------------------------------- keying

def basis(B, h, T, P):
    """pose bone rotation for turn T from rest (frame above), given the parent's turn P"""
    r = B.rest_m[h].to_quaternion()
    rp = B.rest_m[up_chain(B, h)].to_quaternion() if up_chain(B, h) else Quaternion()
    tb, tp = fbq(T), fbq(P)
    return ((rp.inverted() @ r).inverted() @ (tp @ rp).inverted() @ (tb @ r)).normalized()


def key_cycle(B, name, c, log=print):
    ob = B.ob
    old = bpy.data.actions.get(name)
    if old:
        bpy.data.actions.remove(old)
    act = bpy.data.actions.new(name)
    act.use_fake_user = True
    ob.animation_data_create()
    ob.animation_data.action = act
    n = c["frames"]
    for h in B.name:
        pb = B.pose_bone(h)
        pb.rotation_mode = "QUATERNION"
    # each arm's extra outward angle: the most any key needs to clear the dress, so it doesn't move in and out per swing
    extra = [0.0, 0.0]
    dress = Dress(B)
    for s in (0, 1):
        for k in range(KEYS):
            p = k / KEYS

            def deep(a):
                e = [0.0, 0.0]
                e[s] = a
                T, mv = pose_at(B, c, p, e)
                return dress.depth(arm_points(B, T, mv, s))

            if deep(extra[s]) <= 0:
                continue
            lo, hi = extra[s], 0.8
            for _ in range(14):
                mid = 0.5 * (lo + hi)
                lo, hi = (mid, hi) if deep(mid) > 0 else (lo, mid)
            extra[s] = hi
    log("%s: arms out %.1f° (left) and %.1f° (right) more, to clear the dress" % (name, math.degrees(extra[0]), math.degrees(extra[1])))
    prev = {}
    for k in range(KEYS + 1):
        p = (k % KEYS) / KEYS
        frame = n * k / KEYS
        T, move = pose_at(B, c, p, extra)
        for h in B.name:
            if h not in T:
                continue
            up = up_chain(B, h)
            P = T.get(up, Quaternion()) if up else Quaternion()
            q = basis(B, h, T[h], P)
            if h in prev and prev[h].dot(q) < 0:
                q = -q
            prev[h] = q
            pb = B.pose_bone(h)
            pb.rotation_quaternion = q
            pb.keyframe_insert("rotation_quaternion", frame=frame, group=pb.name)
        hb = B.pose_bone("Hips")
        rq = B.rest_m["Hips"].to_quaternion()
        hb.location = rq.inverted() @ fb(move)
        hb.keyframe_insert("location", frame=frame, group=hb.name)
    for fc in fcurves(ob, act):
        for kp in fc.keyframe_points:
            kp.interpolation = "BEZIER"
            kp.handle_left_type = kp.handle_right_type = "AUTO_CLAMPED"
        if not any(m.type == "CYCLES" for m in fc.modifiers):
            fc.modifiers.new("CYCLES")
    act.frame_range = (0, n)
    log("%s: %d frames (%.2f s), %d keys a bone" % (name, n, n / FPS, KEYS + 1))
    return act


def fcurves(ob, act):
    """an action's F-curves on the armature (Blender 4.4+ keeps them per slot)"""
    if hasattr(act, "fcurves"):
        return list(act.fcurves)
    from bpy_extras import anim_utils
    cb = anim_utils.action_get_channelbag_for_slot(act, ob.animation_data.action_slot)
    return list(cb.fcurves) if cb else []


def build(settings, armature="Armature", log=print):
    """the actions "Walk" and "Run" on the armature, keyed anew from CYCLES"""
    B = Body(armature, humanoid_of(settings))
    sc = bpy.context.scene
    sc.render.fps, sc.render.fps_base = FPS, 1
    out = {}
    for name, c in CYCLES.items():
        out[name] = key_cycle(B, name, c, log)
    show("Walk", armature)
    return out


def show(name, armature="Armature"):
    """puts that cycle on the armature, the scene's frame range set to it"""
    ob = bpy.data.objects[armature]
    act = bpy.data.actions[name]
    ob.animation_data_create()
    ob.animation_data.action = act
    sc = bpy.context.scene
    sc.frame_start, sc.frame_end = 0, int(act.frame_range[1]) - 1
    sc.frame_set(0)
    return act


def export(folder=ASSETS, prefix="", settings=None, armature="Armature"):
    """both cycles as VRM animations (tools/blend2vrma.py): folder/PREFIXwalk.vrma, folder/PREFIXrun.vrma"""
    if os.path.join(HERE, "..") not in sys.path:
        sys.path.insert(0, os.path.join(HERE, ".."))
    import importlib
    import blend2vrma
    importlib.reload(blend2vrma)
    sc = bpy.context.scene
    keep = (sc.frame_start, sc.frame_end, sc.frame_current)
    out = []
    for name in CYCLES:
        act = show(name, armature)
        n = int(act.frame_range[1])
        out.append(blend2vrma.export(os.path.join(folder, prefix + name.lower() + ".vrma"), armature=armature,
                                     humanoid=settings, frames=(0, n), bones="all", name=name))
    sc.frame_start, sc.frame_end = keep[0], keep[1]
    sc.frame_set(keep[2])
    return out
