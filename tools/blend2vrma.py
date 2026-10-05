#!/usr/bin/env python3
"""blend2vrma: a humanoid's Blender animation as a VRM animation (.vrma) for hyprwalk: an emote, or its attacks.

In Blender (Python console, Text Editor, Blender's MCP, or in the background):
    blender -b FILE.blend --python tools/blend2vrma.py -- OUT.vrma [--armature NAME] [--humanoid SETTINGS.json]
            [--frames START END] [--bones all|upper] [--fingers] [--name NAME]
    import blend2vrma; blend2vrma.export("OUT.vrma", armature="Armature", humanoid="Miku.hyprwalk.json")

  --armature NAME           default: the scene's first armature
  --humanoid SETTINGS.json  bones from a hyprwalk settings file (default: guessed from VRM/Unity/.L.R/Mixamo names)
  --frames START END        default: the scene's range
  --bones upper             no hips or legs (an attack, played over the walking)
  --fingers                 keep the fingers (else the avatar's own gestures make the hands)
  --name NAME               clip name (default: the action's)

Frames are evaluated with constraints and IK at the scene's frame rate. Bone turns from the armature's T pose (as
hyprwalk works it out) go on a straightened T pose of its own proportions, so the clip plays the same on any humanoid.
Scene markers go in the extras as seconds from START: {"markers": {"hit": 0.2, ...}}; attacks read ready, hit, next.
"""
import json
import math
import os
import re
import struct
import sys

try:
    import bpy
    from mathutils import Matrix, Quaternion, Vector
except ImportError:  # only inside Blender
    bpy = None

# VRM 1.0 bone names in hyprwalk's order (avatar.hpp's eHumanBone)
BONES = ['hips', 'spine', 'chest', 'upperChest', 'neck', 'head',
         'leftUpperLeg', 'leftLowerLeg', 'leftFoot', 'rightUpperLeg', 'rightLowerLeg', 'rightFoot',
         'leftShoulder', 'leftUpperArm', 'leftLowerArm', 'leftHand', 'rightShoulder', 'rightUpperArm', 'rightLowerArm',
         'rightHand', 'leftEye', 'rightEye', 'jaw', 'leftToes', 'rightToes']
FINGERS = [f'{s}{f}{p}' for s in ('left', 'right') for f, ps in (('Thumb', ('Metacarpal', 'Proximal', 'Distal')),) +
           tuple((f, ('Proximal', 'Intermediate', 'Distal')) for f in ('Index', 'Middle', 'Ring', 'Little')) for p in ps]
UPPER = {'spine', 'chest', 'upperChest', 'neck', 'head', 'leftShoulder', 'leftUpperArm', 'leftLowerArm', 'leftHand',
         'rightShoulder', 'rightUpperArm', 'rightLowerArm', 'rightHand'} | set(FINGERS)
FACE = {'leftEye', 'rightEye', 'jaw'}  # left to the avatar's gaze and lip sync


def parent_of(b, has):
    """the humanoid bone above b that the rig has"""
    up = {'spine': 'hips', 'chest': 'spine', 'upperChest': 'chest', 'neck': 'upperChest', 'head': 'neck',
          'leftUpperLeg': 'hips', 'rightUpperLeg': 'hips', 'leftShoulder': 'upperChest', 'rightShoulder': 'upperChest',
          'leftEye': 'head', 'rightEye': 'head', 'jaw': 'head', 'leftToes': 'leftFoot', 'rightToes': 'rightFoot'}
    for side in ('left', 'right'):
        for chain in (('UpperLeg', 'LowerLeg', 'Foot'), ('Shoulder', 'UpperArm', 'LowerArm', 'Hand')):
            for a, c in zip(chain, chain[1:]):
                up[side + c] = side + a
    if b in FINGERS:
        side = 'left' if b.startswith('left') else 'right'
        segs = ('Metacarpal', 'Proximal', 'Distal') if 'Thumb' in b else ('Proximal', 'Intermediate', 'Distal')
        seg = next(s for s in segs if b.endswith(s))
        i = segs.index(seg)
        up[b] = side + 'Hand' if i == 0 else b[:-len(seg)] + segs[i - 1]
    p = up.get(b)
    while p is not None and p not in has:
        p = up.get(p)
    return p


# --- which bone is which

def vrm_name(name, vrm1=False):
    """a settings file's, Unity's or VRM's humanoid bone name ("Left Thumb Proximal") as VRM 1.0's, or None. Unless
    vrm1, thumb proximal/intermediate/distal are the old names for VRM 1.0's metacarpal/proximal/distal"""
    n = re.sub(r'[\s_]', '', name)
    if not vrm1 and 'thumb' in n.lower():
        n = re.sub('(?i)thumbproximal', 'ThumbMetacarpal', n)
        n = re.sub('(?i)thumbintermediate', 'ThumbProximal', n)
    return {b.lower(): b for b in BONES + FINGERS}.get(n.lower())


GUESS = [  # (name pattern without side, lower case; bone)
    (r'^(hips|pelvis|hip)$', 'hips'), (r'^(spine|spine1|abdomen)$', 'spine'), (r'^(chest|spine2)$', 'chest'),
    (r'^(upperchest|spine3)$', 'upperChest'), (r'^neck$', 'neck'), (r'^head$', 'head'),
    (r'^(upperleg|thigh|upleg)$', 'UpperLeg'), (r'^(lowerleg|calf|shin|leg|knee)$', 'LowerLeg'),
    (r'^(foot|ankle)$', 'Foot'), (r'^(toes?|toebase)$', 'Toes'), (r'^(shoulder|clavicle)$', 'Shoulder'),
    (r'^(upperarm|arm)$', 'UpperArm'), (r'^(lowerarm|forearm|elbow)$', 'LowerArm'), (r'^(hand|wrist)$', 'Hand'),
    (r'^eye$', 'Eye'), (r'^jaw$', 'jaw'),
]


def guess(names):
    """{VRM 1.0 bone: the rig's bone} by names"""
    out = {}
    for nm in names:
        n = re.sub(r'^.*:', '', nm).lower()  # drop the namespace (mixamorig:LeftArm)
        side = None
        for pat, s in ((r'(^left|^l_|_l$|\.l$|\bl$| l$)', 'left'), (r'(^right|^r_|_r$|\.r$|\br$| r$)', 'right')):
            if re.search(pat, n):
                side = s
                n = re.sub(pat, '', n)
                break
        n = re.sub(r'[\s_.]', '', n)
        for pat, b in GUESS:
            if re.match(pat, n):
                key = b if b[0].islower() else (side + b if side else None)
                if key and key not in out:
                    out[key] = nm
                break
    return out


# --- space: Blender's (z up) and glTF's (y up)

def to_gltf_m():
    return Matrix(((1, 0, 0), (0, 0, 1), (0, -1, 0)))


def arc(a, b):
    """the shortest turn taking direction a to b"""
    a, b = a.normalized(), b.normalized()
    return a.rotation_difference(b)


def tpose_of(rest, has, facing):
    """per bone, the model-space turn from rest to a T pose, as hyprwalk's rigOf(): arms out sideways, legs down, hands
    toward the middle finger (else another); bones below a limb bone turn with it, the rest not at all"""
    tp = {b: Quaternion() for b in has}
    own = set()
    for side, sx in (('left', 1.0), ('right', -1.0)):
        def along(b, to):
            if b in has and to in has:
                d = rest[to] - rest[b]
                if d.length > 1e-6:
                    return d.normalized()
            return None
        fingers = None
        for f in ('Middle', 'Index', 'Ring', 'Little'):
            fingers = fingers or along(side + 'Hand', side + f + 'Proximal')
        limbs = [(side + 'UpperArm', along(side + 'UpperArm', side + 'LowerArm')),
                 (side + 'LowerArm', along(side + 'LowerArm', side + 'Hand')), (side + 'Hand', fingers),
                 (side + 'UpperLeg', along(side + 'UpperLeg', side + 'LowerLeg')),
                 (side + 'LowerLeg', along(side + 'LowerLeg', side + 'Foot'))]
        for b, d in limbs:
            if d is not None:
                want = Vector((sx, 0, 0)) if 'Arm' in b or 'Hand' in b else Vector((0, -1, 0))
                tp[b] = arc(d, facing @ want)
                own.add(b)
    torso = {'hips', 'spine', 'chest', 'upperChest', 'neck', 'head'}
    for b in BONES + FINGERS:  # BONES lists parents first
        if b in has and b not in own:
            p = parent_of(b, has)
            limb = p is not None and p not in torso and not b.endswith('Shoulder') and b not in ('leftEye', 'rightEye', 'jaw')
            tp[b] = tp[p] if limb else Quaternion()
    return tp


def straight_tpose(rest, has, tp, facing_inv):
    """T pose the clip goes on: rest offsets turned by the parent's T pose turn (straight limbs), facing +Z, to 1 cm"""
    pos = {}
    for b in [b for b in BONES + FINGERS if b in has]:
        p = parent_of(b, has)
        pos[b] = facing_inv @ rest[b] if p is None else pos[p] + facing_inv @ (tp[p] @ (rest[b] - rest[p]))
    return {b: Vector([round(v, 2) for v in pos[b]]) for b in pos}


# --- writing

def write_glb(path, js, binc):
    j = json.dumps(js, separators=(',', ':'), ensure_ascii=False).encode('utf-8')
    j += b' ' * (-len(j) % 4)
    b = binc + b'\0' * (-len(binc) % 4)
    body = struct.pack('<II', len(j), 0x4E4F534A) + j + (struct.pack('<II', len(b), 0x004E4942) + b if b else b'')
    tmp = path + '.part'
    with open(tmp, 'wb') as f:
        f.write(struct.pack('<III', 0x46546C67, 2, 12 + len(body)))
        f.write(body)
    os.replace(tmp, path)


def export(path, armature=None, humanoid=None, frames=None, bones='all', fingers=False, name=None, log=print):
    """writes the armature's scene animation as a VRM animation at `path`; returns a summary"""
    sc = bpy.context.scene
    ob = bpy.data.objects[armature] if armature else next(o for o in sc.objects if o.type == 'ARMATURE')
    names = [b.name for b in ob.data.bones]
    if humanoid:
        with open(humanoid, encoding='utf-8') as f:
            hmap = json.load(f).get('humanoid', {})
        rig = {}
        vrm1 = any('thumbmetacarpal' in re.sub(r'[\s_]', '', k).lower() for k in hmap)
        for k, v in hmap.items():
            b = vrm_name(k, vrm1)
            if b and v in names:
                rig[b] = v
    else:
        rig = guess(names)
    for need in ('hips', 'spine', 'head', 'leftUpperArm', 'rightUpperArm', 'leftUpperLeg', 'rightUpperLeg'):
        if need not in rig:
            raise SystemExit(f'blend2vrma: no {need} bone in {ob.name} (a humanoid? --humanoid SETTINGS.json says which is which)')
    has = set(rig)
    C = to_gltf_m()
    Cq = C.to_quaternion()
    W = ob.matrix_world

    def rest_world(b):
        m = W @ ob.data.bones[rig[b]].matrix_local
        return C @ m.to_translation(), (Cq @ m.to_quaternion() @ Cq.inverted()).normalized()

    rest = {b: rest_world(b)[0] for b in has}
    rest_q = {b: rest_world(b)[1] for b in has}
    across = rest['leftUpperLeg'] - rest['rightUpperLeg']
    across.y = 0
    facing_dir = across.cross(Vector((0, 1, 0))).normalized()
    facing = Quaternion((0, 1, 0), math.atan2(facing_dir.x, facing_dir.z))
    tp = tpose_of(rest, has, facing)
    feet = min(rest[b].y for b in ('leftFoot', 'rightFoot', 'leftLowerLeg', 'rightLowerLeg') if b in has)
    legs = max((rest[s + 'LowerLeg'] - rest[s + 'UpperLeg']).length * 2 for s in ('left', 'right') if s + 'LowerLeg' in has)
    height = rest['hips'].y - feet if rest['hips'].y - feet > 0.1 * legs else max(legs, 1e-4)

    keep = [b for b in BONES + FINGERS if b in has and b not in FACE and (fingers or b not in FINGERS) and (bones == 'all' or b in UPPER)]
    # T pose nodes: the whole humanoid (players recognize it by its legs and arms), the eyes (hyprwalk places attack hands
    # in the first person view by them) and kept fingers; bones not kept get no curves
    nodes_b = {b for b in has if (b not in FACE or b.endswith('Eye')) and (fingers or b not in FINGERS)}
    order = [b for b in BONES + FINGERS if b in nodes_b]
    tpos = straight_tpose(rest, nodes_b, tp, facing.inverted())
    vheight = height  # T pose ~ the rig's hips height; moves in hip heights

    start, end = frames if frames else (sc.frame_start, sc.frame_end)
    fps = sc.render.fps / sc.render.fps_base
    times, turns, moves = [], {b: [] for b in keep}, []
    was = sc.frame_current
    Rc = facing.inverted()
    for f in range(start, end + 1):
        sc.frame_set(f)
        dg = bpy.context.evaluated_depsgraph_get()
        ev = ob.evaluated_get(dg)
        times.append((f - start) / fps)
        world = {}
        for b in nodes_b:
            m = W @ ev.pose.bones[rig[b]].matrix
            world[b] = (C @ m.to_translation(), (Cq @ m.to_quaternion() @ Cq.inverted()).normalized())
        # each bone's turn from the T pose, avatar facing +Z (hyprwalk's canonical())
        t = {b: (Rc @ world[b][1] @ rest_q[b].inverted() @ tp[b].inverted() @ facing).normalized() for b in nodes_b}
        for b in keep:
            p = parent_of(b, nodes_b)
            q = (t[p].inverted() @ t[b]).normalized() if p else t[b]
            prev = turns[b][-1] if turns[b] else None
            if prev is not None and prev.dot(q) < 0:
                q = -q
            turns[b].append(q)
        moves.append(Rc @ (world['hips'][0] - rest['hips']) / height)
    sc.frame_set(was)
    moving = any(m.length > 1e-4 for m in moves) and 'hips' in keep

    buf = bytearray()
    views, accessors = [], []

    def add(data, typ, count, minmax=None):
        while len(buf) % 4:
            buf.append(0)
        off = len(buf)
        buf.extend(struct.pack('<%df' % len(data), *data))
        views.append({'buffer': 0, 'byteOffset': off, 'byteLength': len(buf) - off})
        a = {'bufferView': len(views) - 1, 'componentType': 5126, 'count': count, 'type': typ}
        if minmax:
            a['min'], a['max'] = minmax
        accessors.append(a)
        return len(accessors) - 1

    node_of = {b: i for i, b in enumerate(order)}
    nodes = []
    for b in order:
        p = parent_of(b, nodes_b)
        off = tpos[b] - (tpos[p] if p else Vector())
        nd = {'name': b, 'translation': [round(v, 4) for v in off]}
        kids = [node_of[c] for c in order if parent_of(c, nodes_b) == b]
        if kids:
            nd['children'] = kids
        nodes.append(nd)
    ta = add(times, 'SCALAR', len(times), ([times[0]], [times[-1]]))
    channels, samplers = [], []
    for b in keep:
        q = turns[b]
        a = add([v for x in q for v in (x.x, x.y, x.z, x.w)], 'VEC4', len(q))
        samplers.append({'input': ta, 'output': a, 'interpolation': 'LINEAR'})
        channels.append({'sampler': len(samplers) - 1, 'target': {'node': node_of[b], 'path': 'rotation'}})
    if moving:
        base = tpos['hips']
        a = add([v for m in moves for v in (base + m * vheight)], 'VEC3', len(moves))
        samplers.append({'input': ta, 'output': a, 'interpolation': 'LINEAR'})
        channels.append({'sampler': len(samplers) - 1, 'target': {'node': node_of['hips'], 'path': 'translation'}})
    anim = {'name': name or (ob.animation_data.action.name if ob.animation_data and ob.animation_data.action else 'clip'),
            'channels': channels, 'samplers': samplers}
    marks = {m.name: round((m.frame - start) / fps, 4) for m in sc.timeline_markers if start <= m.frame <= end}
    if marks:
        anim['extras'] = {'markers': marks}
    ext = {'specVersion': '1.0', 'humanoid': {'humanBones': {b: {'node': node_of[b]} for b in order}}}
    js = {'asset': {'version': '2.0', 'generator': 'hyprwalk blend2vrma'}, 'scene': 0,
          'scenes': [{'nodes': [node_of[b] for b in order if parent_of(b, nodes_b) is None]}],
          'nodes': nodes, 'animations': [anim], 'accessors': accessors, 'bufferViews': views,
          'buffers': [{'byteLength': len(buf)}], 'extensionsUsed': ['VRMC_vrm_animation'],
          'extensions': {'VRMC_vrm_animation': ext}}
    write_glb(path, js, bytes(buf))
    done = {'path': path, 'frames': len(times), 'seconds': round(times[-1], 4), 'fps': fps, 'bones': keep,
            'moves': moving, 'markers': marks}
    log(f"blend2vrma: {path}: {len(times)} frames ({times[-1]:.3f} s at {fps:g} a second), {len(keep)} bones"
        f"{', the hips moving' if moving else ''}{', markers ' + json.dumps(marks) if marks else ''}")
    return done


def main(argv):
    import argparse
    ap = argparse.ArgumentParser(prog='blend2vrma', description=__doc__.split('\n\n')[0])
    ap.add_argument('out')
    ap.add_argument('--armature')
    ap.add_argument('--humanoid', help="a hyprwalk settings file (its \"humanoid\")")
    ap.add_argument('--frames', nargs=2, type=int)
    ap.add_argument('--bones', choices=('all', 'upper'), default='all')
    ap.add_argument('--fingers', action='store_true')
    ap.add_argument('--name')
    a = ap.parse_args(argv)
    export(a.out, a.armature, a.humanoid, a.frames, a.bones, a.fingers, a.name)


if __name__ == '__main__' and bpy is not None:
    main(sys.argv[sys.argv.index('--') + 1:] if '--' in sys.argv else [])
elif __name__ == '__main__':
    sys.exit('blend2vrma runs in Blender: blender -b FILE.blend --python tools/blend2vrma.py -- OUT.vrma ...')
