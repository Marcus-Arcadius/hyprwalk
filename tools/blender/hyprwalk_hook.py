"""hyprwalk_hook: the attack, a right hook at 60 frames a second, as hyprwalk_rig key poses: third person's at frames 0-45, first
person's at 100-130 (CLIPS); hyprwalk_export writes them for the plugin. See README.md here.

She winds up to her right, the fist wide at shoulder height ahead of her twin tails (so both cameras see it), then the
trunk turns left and the fist sweeps round in front of her face, elbow up, palm down, and back to the guard. The arms
stay ahead of the twin tails, which hang behind her shoulders.
"""
import math

import bpy

import hyprwalk_rig as R
from hyprwalk_rig import mix

W = "world"


def pose(**kw):
    return {k.replace("_R", ".R").replace("_L", ".L"): v for k, v in kw.items()}


# the plugin's idle stance: arms hanging a little out from the skirt, palms in
DOWN = pose(wrist_R=(W, (0.25, -0.25, 0.0)), wrist_L=(W, (0.25, -0.25, 0.0)),
            elbow_R=(W, (0.6, 0.0, -0.05)), elbow_L=(W, (0.6, 0.0, -0.05)),
            along_R=(W, (0.08, -1, 0.0)), along_L=(W, (0.08, -1, 0.0)),
            palm_R=(W, (-1, 0, 0)), palm_L=(W, (-1, 0, 0)))
# left fist by her chin, elbow down in front
GUARD_L = dict(wrist_L=(0.06, 0.18, 0.17), elbow_L=(0.25, -0.4, 0.05), along_L=(-0.1, 0.8, 0.6), palm_L=(-1, 0, 0.1))
TIGHT_L = dict(wrist_L=(0.045, 0.175, 0.13), elbow_L=(0.25, -0.4, 0.05), along_L=(-0.05, 0.85, 0.5), palm_L=(-1, 0, 0.1))


def side(d, s):
    """an arm's numbers for the other arm (each arm's numbers are in its own side's terms)"""
    return {k.replace("_R", "_" + s): v for k, v in d.items()}


def swing(deg, height, reach=0.38, ahead=0.0):
    """right wrist on the hook's arc round her middle: deg from straight ahead (her right > 0), height in meters above
    the chest's joint; elbow out and up, fist along the arc, palm down"""
    a = math.radians(deg)
    out, fwd = reach * math.sin(a), reach * math.cos(a) + ahead
    # the hand points along the arc's tangent, a little in toward the middle
    t = (-math.cos(a) - 0.25 * math.sin(a), 0.0, math.sin(a) - 0.25 * math.cos(a))
    return dict(wrist_R=(W, (out, height, fwd)), elbow_R=(W, (0.65, height + 0.05, fwd * 0.4 - 0.02)),
                along_R=(W, t), palm_R=(W, (0, -1, 0)))


# fists come up in front (inside the twin tails), the left on its way to her chin
RAISE_R = dict(wrist_R=(W, (0.22, -0.03, 0.22)), elbow_R=(W, (0.5, -0.3, 0.1)), along_R=(W, (0.1, 0.4, 1.0)), palm_R=(W, (-1, -0.3, 0)))
RAISE = mix(pose(spine=(-3, 3, 0), chest=(-8, 4, 0), neck=(-5, 1, 0), head=(-1, 1, 0), **RAISE_R, **{k: v for k, v in DOWN.items() if k.endswith(".L")}),
            pose(spine=(-3, 3, 0), chest=(-8, 4, 0), neck=(-5, 1, 0), head=(-1, 1, 0), **RAISE_R, **GUARD_L), trunk=1, R=1, L=0.45)
READY = pose(spine=(-6, 5, 0), chest=(-15, 7, 0), neck=(-10, 2, 0), head=(-2, 2, 0), shoulder_R=(4, 3),
             **swing(56, 0.17), **GUARD_L)
# loaded: wound a little further, dipped, the fist drawn back a little
LOAD = pose(spine=(-8, 7, 0), chest=(-19, 9, 0), neck=(-12, 3, 0), head=(-3, 3, 0), shoulder_R=(3, 3),
            **swing(60, 0.165, 0.37), **GUARD_L)
MID1 = pose(spine=(0, 6, 0), chest=(2, 7, 1), neck=(0, 2, 0), head=(1, 2, 0), shoulder_R=(6, 10), shoulder_L=(-2, 0),
            **swing(42, 0.215), **TIGHT_L)
MID2 = pose(spine=(8, 6, 0), chest=(22, 8, 2), neck=(15, 3, 0), head=(4, 2, 0), shoulder_R=(12, 10), shoulder_L=(-4, 0),
            **swing(18, 0.23), **TIGHT_L)
HIT = pose(spine=(13, 6, 0), chest=(33, 8, 3), neck=(23, 3, 0), head=(6, 2, 0), shoulder_R=(18, 10), shoulder_L=(-6, 0),
           **swing(-4, 0.24), **TIGHT_L)
OVER = pose(spine=(15, 6, 0), chest=(36, 8, 3), neck=(25, 3, 0), head=(7, 2, 0), shoulder_R=(20, 10), shoulder_L=(-6, 0),
            **swing(-12, 0.24), **TIGHT_L)
GUARD = pose(spine=(2, 5, 0), chest=(4, 6, 0), neck=(2, 2, 0), head=(0, 2, 0),
             wrist_R=(0.075, 0.165, 0.12), elbow_R=(0.25, -0.4, 0.0), along_R=(0.0, 0.85, 0.5), palm_R=(-1, 0, 0.1), **GUARD_L)

KEYS = [  # (frame, pose or mix); the trunk leads, the fist follows
    (0, DOWN),
    (4, RAISE),
    (8, READY),
    (10, LOAD),
    (11, mix(LOAD, MID1, trunk=0.55, R=0.35, L=0.6)),
    (12, MID1),
    (13, MID2),
    (14, HIT),
    (16, OVER),
    (19, mix(OVER, HIT, trunk=0.6, R=0.7, L=0.5)),
    (25, mix(HIT, GUARD, trunk=0.6, R=0.6, L=0.6)),
    (31, GUARD),
    (45, DOWN),
]
# ready: where a chained swing starts (blending in until it strikes); next: when another may start; out: when it starts
# releasing the body
MARKERS = {"ready": 4, "hit": 14, "next": 16, "out": 31}

# --- first person's (frames 100 on): from her eyes (30 cm above the chest's joint, 4 cm ahead) the fist rises from the
# view's bottom right corner to just under the middle, the forearm seen side on (from behind, the sleeves' bell cuffs
# hide a fist pointing away); a smaller trunk turn


def rise(out, up, ahead, lead=0.0):
    """right wrist rising at (out, up, ahead) from the chest's joint, not turned; elbow down and out below it; fist up
    and in (lead: more in), palm in"""
    return dict(wrist_R=(W, (out, up, ahead)), elbow_R=(W, (out + 0.35, up - 0.45, ahead - 0.05)),
                along_R=(W, (-0.35 - lead, 1.0, 0.45)), palm_R=(W, (-1, 0.2, -0.2)))


# first person's ready hands, as the plugin holds them (the view's bottom corners)
FP_HANDS_R = dict(wrist_R=(W, (0.155, 0.13, 0.39)), elbow_R=(W, (0.5, -0.35, 0.1)), along_R=(W, (-0.15, 0.55, 1.0)), palm_R=(W, (-1, 0, 0)))
FP_HANDS = pose(**FP_HANDS_R, **side(FP_HANDS_R, "L"))
FP_GUARD_L = side(dict(FP_HANDS_R, wrist_R=(W, (0.13, 0.12, 0.33))), "L")
FP_READY = pose(spine=(-4, 4, 0), chest=(-9, 5, 0), neck=(-6, 1, 0), head=(-1, 1, 0), **rise(0.21, 0.13, 0.32), **FP_GUARD_L)
FP_LOAD = pose(spine=(-5, 5, 0), chest=(-11, 6, 0), neck=(-7, 2, 0), head=(-1, 2, 0), **rise(0.22, 0.115, 0.30), **FP_GUARD_L)
FP_MID = pose(spine=(3, 5, 0), chest=(6, 6, 1), neck=(4, 2, 0), head=(1, 1, 0), shoulder_R=(8, 4), **rise(0.13, 0.17, 0.35, 0.2), **FP_GUARD_L)
FP_HIT = pose(spine=(8, 5, 0), chest=(17, 6, 2), neck=(12, 2, 0), head=(3, 1, 0), shoulder_R=(14, 6), shoulder_L=(-4, 0),
              **rise(0.02, 0.23, 0.36, 0.45), **FP_GUARD_L)
FP_OVER = pose(spine=(9, 5, 0), chest=(19, 6, 2), neck=(13, 2, 0), head=(3, 1, 0), shoulder_R=(15, 7), shoulder_L=(-4, 0),
               **rise(-0.02, 0.255, 0.355, 0.5), **FP_GUARD_L)
# starts and ends at the ready hands, where a first person swing begins
FP_KEYS = [(100 + f, p) for f, p in [
    (0, FP_HANDS),
    (4, FP_READY),
    (6, FP_LOAD),
    (7, mix(FP_LOAD, FP_MID, trunk=0.55, R=0.35, L=0.6)),
    (8, FP_MID),
    (9, mix(FP_MID, FP_HIT, trunk=0.7, R=0.75, L=1.0)),
    (10, FP_HIT),
    (12, FP_OVER),
    (15, mix(FP_OVER, FP_HIT, trunk=0.6, R=0.7, L=0.5)),
    (21, mix(FP_HIT, FP_HANDS, trunk=0.6, R=0.6, L=0.6)),
    (27, FP_HANDS),
    (30, FP_HANDS),
]]
FP_MARKERS = {"ready": 100, "hit": 110, "next": 112, "out": 127}
CLIPS = {"third": (0, 45), "first": (100, 130)}  # frame ranges


def build():
    R.clear()
    R.setup(start=KEYS[0][0], end=FP_KEYS[-1][0])
    for frame, p in KEYS + FP_KEYS:
        R.key(frame, p)
    sc = bpy.context.scene
    for m in list(sc.timeline_markers):
        sc.timeline_markers.remove(m)
    for marks in (MARKERS, FP_MARKERS):
        for name, f in marks.items():
            sc.timeline_markers.new(name, frame=f)
    sc.frame_start, sc.frame_end = CLIPS["third"]
    sc.frame_set(0)
