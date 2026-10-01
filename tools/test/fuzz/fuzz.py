#!/usr/bin/env python3
# fuzz.py: feeds hypr3d's loaders broken maps and avatars, and keeps whatever crashes them, trips a sanitizer, hangs
# or runs away with memory. They run inside Hyprland, on its loader thread (and what they load is uploaded and drawn
# on its main thread): any of that takes the compositor down. Plain python3.
#
#   fuzz.py map OUTDIR [-n N] [--seed S] [--jobs J] [--timeout T] [--harness BIN]
#   fuzz.py avatar OUTDIR --avatars DIR [the same]
#   fuzz.py replay CASE [--harness BIN]      one case again (a .glb, or an avatar case's folder), with the output
#
# The harness is tools/test/harness's shot built with AddressSanitizer and UndefinedBehaviorSanitizer (`./build.sh -f
# tools/test/fuzz/asan.mk` makes build-asan/shot), drawing on Mesa's llvmpipe. It loads each case as the plugin does
# (an exception is a failed load, as on the loader thread), draws it, and for an avatar runs its physics, an emote
# from its settings, one from a file, gestures, a face, the look-at, lip sync and the Action Menu.
#
# Seeds: map: tools/test/vm/litmap.py's LitCourt.glb (everything tools/cs2map.py writes). avatar: BoothAccessories
# (tools/unity2hypr3d.py's output: skins, morphs, variants, and its settings file) from --avatars DIR (as run.sh
# --avatars takes it), as it is, as a VRM 0.x and as a VRM 1.0 (VRMC_vrm, VRMC_springBone with extended colliders and
# limits, VRMC_node_constraint, VRMC_materials_mtoon), with BoothGimmicks.hands.vrma as its emote file; ToonTest.glb
# (tools/test/vm/assets.py: outlines, stencils); and ToonBalls.glb (tools/test/harness/toonballs.py: MToon 1.0 and
# 0.x shading, the converter's toon and matcap extras). A case is a seed with one to three mutations of its JSON (numbers
# made negative, zero, NaN, infinite or huge, indices out of range or pointing back up the tree, wrong types, keys
# gone, arrays emptied or grown, strings made long) or of its binary chunk (indices, joints and floats in the
# accessors' data, images' bytes, the chunk or the whole file cut short, the GLB header's lengths), or, for an avatar,
# of its settings file or its emote file.
#
# OUTDIR gets seeds/, cases/ (every case, and what the harness printed), findings/<kind>-<where>/ (the first case of
# each distinct problem: the input and the log) and summary.txt. Kinds: asan, ubsan, alloc (a runaway allocation:
# over 2 GB asked for, or 4 GB resident), crash (a signal with no report), hang (over the timeout), error (any other
# exit). A clean load, or a load that fails with a message, is fine.
import argparse
import concurrent.futures
import copy
import json
import math
import os
import random
import re
import shutil
import struct
import subprocess
import sys
import time

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', '..'))
HARNESS = os.path.join(REPO, 'build-asan', 'shot')
MESA = {'__EGL_VENDOR_LIBRARY_FILENAMES': '/run/opengl-driver/share/glvnd/egl_vendor.d/50_mesa.json', 'LIBGL_ALWAYS_SOFTWARE': '1',
        'LP_NUM_THREADS': '2'}
SAN = {'ASAN_OPTIONS': 'detect_leaks=0:abort_on_error=1:allocator_may_return_null=0:max_allocation_size_mb=2048:hard_rss_limit_mb=4096:'
                       'detect_stack_use_after_return=0:symbolize=1',
       'UBSAN_OPTIONS': 'print_stacktrace=1:halt_on_error=1:symbolize=1'}


# ------------------------------------------------------------------ GLB

def read_glb(path):
    """(the JSON, the binary chunk)"""
    data = open(path, 'rb').read()
    magic, version, length = struct.unpack_from('<III', data, 0)
    assert magic == 0x46546C67, f'{path} is not a GLB'
    off, js, bin_ = 12, None, b''
    while off + 8 <= len(data):
        n, kind = struct.unpack_from('<II', data, off)
        body = data[off + 8:off + 8 + n]
        if kind == 0x4E4F534A:
            js = json.loads(body)
        elif kind == 0x004E4942:
            bin_ = bytes(body)
        off += 8 + n + (-n % 4)
    return js, bin_


def glb_bytes(js, bin_, raw_json=None):
    text = raw_json if raw_json is not None else json.dumps(js, separators=(',', ':'), ensure_ascii=False, allow_nan=True).encode()
    text += b' ' * (-len(text) % 4)
    out = struct.pack('<II', len(text), 0x4E4F534A) + text
    if bin_ is not None:
        pad = bin_ + b'\0' * (-len(bin_) % 4)
        out += struct.pack('<II', len(pad), 0x004E4942) + pad
    return struct.pack('<III', 0x46546C67, 2, 12 + len(out)) + out


# ------------------------------------------------------------------ seeds

def map_seeds(out):
    subprocess.run([sys.executable, os.path.join(REPO, 'tools', 'test', 'vm', 'litmap.py'), out], check=True, stdout=subprocess.DEVNULL)
    return {'LitCourt': os.path.join(out, 'LitCourt.glb')}


LIMITS = [{'cone': {'angle': 0.8, 'rotation': [-0.70711, 0, 0, 0.70711]}}, {'hinge': {'angle': 1.2}},  # (VRMC_springBone_limit's)
          {'spherical': {'pitch': 0.6, 'yaw': 0.9, 'rotation': [0, 0.38268, 0, 0.92388]}}]
VRM0_BONES = {  # the settings file's humanoid names (Unity's) as VRM 0.x's
    'Hips': 'hips', 'Spine': 'spine', 'Chest': 'chest', 'UpperChest': 'upperChest', 'Neck': 'neck', 'Head': 'head',
    'LeftEye': 'leftEye', 'RightEye': 'rightEye', 'Jaw': 'jaw', 'LeftShoulder': 'leftShoulder', 'LeftUpperArm': 'leftUpperArm',
    'LeftLowerArm': 'leftLowerArm', 'LeftHand': 'leftHand', 'RightShoulder': 'rightShoulder', 'RightUpperArm': 'rightUpperArm',
    'RightLowerArm': 'rightLowerArm', 'RightHand': 'rightHand', 'LeftUpperLeg': 'leftUpperLeg', 'LeftLowerLeg': 'leftLowerLeg',
    'LeftFoot': 'leftFoot', 'LeftToes': 'leftToes', 'RightUpperLeg': 'rightUpperLeg', 'RightLowerLeg': 'rightLowerLeg',
    'RightFoot': 'rightFoot', 'RightToes': 'rightToes'}
for side, s in (('Left', 'left'), ('Right', 'right')):
    for f in ('Thumb', 'Index', 'Middle', 'Ring', 'Little'):
        for p in ('Proximal', 'Intermediate', 'Distal'):
            VRM0_BONES[f'{side} {f} {p}'] = f'{s}{f}{p}'


def chains(js, names):
    """spring chains: each named node and its descendants down its first children"""
    idx = {n.get('name'): i for i, n in enumerate(js['nodes'])}
    out = []
    for name in names:
        i, chain = idx.get(name), []
        while i is not None and len(chain) < 6:
            chain.append(i)
            kids = js['nodes'][i].get('children') or []
            i = kids[0] if kids else None
        if chain:
            out.append(chain)
    return out


def avatar_seeds(out, avatars):
    """{name: (glb, settings or None, emote file)}"""
    os.makedirs(out, exist_ok=True)
    src = os.path.join(avatars, 'BoothAccessories.glb')
    shutil.copy(src, os.path.join(out, 'plain.glb'))
    settings = json.load(open(os.path.join(avatars, 'BoothAccessories.hypr3d.json'), encoding='utf-8'))
    # an emote from the settings file too, from the emote file every case has next to it
    settings['emotes'] = settings.get('emotes', []) + [{'name': 'Fuzz', 'file': 'dance.vrma', 'speed': 1.5, 'loop': True}]
    json.dump(settings, open(os.path.join(out, 'plain.hypr3d.json'), 'w', encoding='utf-8'), ensure_ascii=False)
    emote = os.path.join(out, 'dance.vrma')
    shutil.copy(os.path.join(avatars, 'BoothGimmicks.hands.vrma'), emote)
    js, bin_ = read_glb(src)
    idx = {n.get('name'): i for i, n in enumerate(js['nodes'])}
    human = {VRM0_BONES[k]: idx[v] for k, v in settings.get('humanoid', {}).items() if k in VRM0_BONES and v in idx}
    body = next(i for i, m in enumerate(js['meshes']) if m.get('primitives', [{}])[0].get('targets'))
    body_node = next(i for i, n in enumerate(js['nodes']) if n.get('mesh') == body)
    targets = js['meshes'][body].get('extras', {}).get('targetNames') or [f't{k}' for k in range(len(js['meshes'][body]['primitives'][0]['targets']))]
    springs = chains(js, ['TwinTail_L', 'TwinTail_R', 'HairBack', 'Skirt_F', 'Tail'])
    head, hips = human.get('head', 0), human.get('hips', 0)

    # VRM 0.x
    v0 = copy.deepcopy(js)
    v0.setdefault('extensionsUsed', []).append('VRM')
    v0.setdefault('extensions', {})['VRM'] = {
        'exporterVersion': 'hypr3d fuzz', 'specVersion': '0.0', 'meta': {'title': 'fuzz', 'version': '1', 'author': 'fuzz'},
        'humanoid': {'humanBones': [{'bone': b, 'node': n, 'useDefaultValues': True} for b, n in human.items()]},
        'firstPerson': {'firstPersonBone': head, 'firstPersonBoneOffset': {'x': 0, 'y': 0.06, 'z': 0},
                        'meshAnnotations': [{'mesh': body, 'firstPersonFlag': 'Auto'}], 'lookAtTypeName': 'Bone',
                        'lookAtHorizontalInner': {'curve': [0, 0, 0, 1, 1, 1, 1, 0], 'xRange': 90, 'yRange': 10},
                        'lookAtVerticalUp': {'xRange': 90, 'yRange': 10}, 'lookAtVerticalDown': {'xRange': 90, 'yRange': 10}},
        'blendShapeMaster': {'blendShapeGroups': [
            {'name': p, 'presetName': p, 'isBinary': p == 'blink', 'binds': [{'mesh': body, 'index': k % len(targets), 'weight': 100}],
             'materialValues': [{'materialName': js['materials'][0]['name'], 'propertyName': '_Color', 'targetValue': [1, 0.5, 0.5, 1]}]}
            for k, p in enumerate(['a', 'i', 'u', 'e', 'o', 'blink', 'joy', 'angry', 'sorrow', 'fun', 'blink_l', 'blink_r'])]},
        'secondaryAnimation': {
            'boneGroups': [{'comment': f'chain{k}', 'stiffiness': 1.0, 'gravityPower': 0.2, 'gravityDir': {'x': 0, 'y': -1, 'z': 0},
                            'dragForce': 0.4, 'center': -1, 'hitRadius': 0.02, 'bones': [c[0]], 'colliderGroups': [0, 1]}
                           for k, c in enumerate(springs)],
            'colliderGroups': [{'node': head, 'colliders': [{'offset': {'x': 0, 'y': 0.05, 'z': 0}, 'radius': 0.1}]},
                               {'node': hips, 'colliders': [{'offset': {'x': 0, 'y': 0, 'z': 0}, 'radius': 0.15},
                                                             {'offset': {'x': 0, 'y': -0.1, 'z': 0}, 'radius': 0.1}]}]},
        'materialProperties': [{'name': m.get('name', ''), 'shader': 'VRM/MToon', 'renderQueue': 2000,
                                'floatProperties': {'_OutlineWidth': 0.2, '_OutlineWidthMode': 1, '_Cutoff': 0.5, '_ShadeShift': -0.1,
                                                    '_ShadeToony': 0.9},
                                'vectorProperties': {'_OutlineColor': [0, 0, 0, 1], '_Color': [1, 1, 1, 1], '_ShadeColor': [0.8, 0.7, 0.7, 1]},
                                'textureProperties': {'_ShadeTexture': 0, '_SphereAdd': 0} if js.get('textures') else {},
                                'keywordMap': {}, 'tagMap': {}} for m in js['materials']]}
    open(os.path.join(out, 'vrm0.glb'), 'wb').write(glb_bytes(v0, bin_))

    # VRM 1.0
    v1 = copy.deepcopy(js)
    v1.setdefault('extensionsUsed', []).extend(['VRMC_vrm', 'VRMC_springBone', 'VRMC_springBone_extended_collider', 'VRMC_springBone_limit',
                                                'VRMC_node_constraint', 'VRMC_materials_mtoon'])
    exprs = {p: {'morphTargetBinds': [{'node': body_node, 'index': k % len(targets), 'weight': 1.0}], 'isBinary': p == 'blink',
                 'overrideBlink': 'block' if p == 'happy' else 'none', 'overrideMouth': 'blend' if p == 'surprised' else 'none',
                 'overrideLookAt': 'none',
                 'materialColorBinds': [{'material': 0, 'type': 'color', 'targetValue': [1, 0.5, 0.5, 1]}],
                 'textureTransformBinds': [{'material': 0, 'scale': [1, 1], 'offset': [0, 0]}]}
             for k, p in enumerate(['aa', 'ih', 'ou', 'ee', 'oh', 'blink', 'blinkLeft', 'blinkRight', 'happy', 'angry', 'sad', 'relaxed',
                                    'surprised', 'lookUp', 'lookDown', 'lookLeft', 'lookRight'])}
    v1.setdefault('extensions', {})['VRMC_vrm'] = {
        'specVersion': '1.0', 'meta': {'name': 'fuzz', 'version': '1', 'authors': ['fuzz'], 'licenseUrl': 'https://vrm.dev/licenses/1.0/'},
        'humanoid': {'humanBones': {b: {'node': n} for b, n in human.items() if not b[-1].isdigit()}},
        'firstPerson': {'meshAnnotations': [{'node': body_node, 'type': 'auto'}]},
        'lookAt': {'type': 'bone', 'offsetFromHeadBone': [0, 0.06, 0],
                   'rangeMapHorizontalInner': {'inputMaxValue': 90, 'outputScale': 10}, 'rangeMapHorizontalOuter': {'inputMaxValue': 90, 'outputScale': 10},
                   'rangeMapVerticalDown': {'inputMaxValue': 90, 'outputScale': 10}, 'rangeMapVerticalUp': {'inputMaxValue': 90, 'outputScale': 10}},
        'expressions': {'preset': exprs, 'custom': {'fuzzface': exprs['happy']}}}
    colliders = [{'node': head, 'shape': {'sphere': {'offset': [0, 0.05, 0], 'radius': 0.1}}},
                 {'node': hips, 'shape': {'capsule': {'offset': [0, 0, 0], 'radius': 0.12, 'tail': [0, -0.3, 0]}}},
                 {'node': hips, 'shape': {'sphere': {'offset': [0, 0, 0], 'radius': 0.3}},
                  'extensions': {'VRMC_springBone_extended_collider': {'specVersion': '1.0', 'shape': {'sphere': {'offset': [0, 0, 0], 'radius': 0.4, 'inside': True}}}}},
                 {'node': hips, 'shape': {'sphere': {'offset': [0, 0, 0], 'radius': 0.01}},
                  'extensions': {'VRMC_springBone_extended_collider': {'specVersion': '1.0', 'shape': {'plane': {'offset': [0, -0.8, 0], 'normal': [0, 1, 0]}}}}}]
    v1['extensions']['VRMC_springBone'] = {
        'specVersion': '1.0', 'colliders': colliders, 'colliderGroups': [{'name': 'body', 'colliders': [0, 1, 2, 3]}],
        'springs': [{'name': f'chain{k}', 'center': hips, 'colliderGroups': [0],
                     'joints': [{'node': n, 'hitRadius': 0.02, 'stiffness': 1.0, 'gravityPower': 0.1, 'gravityDir': [0, -1, 0], 'dragForce': 0.4,
                                 'extensions': {'VRMC_springBone_limit': {'specVersion': '1.0-draft', 'limit': LIMITS[(k + i) % len(LIMITS)]}}}
                                for i, n in enumerate(c)]} for k, c in enumerate(springs)]}
    cons = [('roll', {'rollAxis': 'X'}), ('aim', {'aimAxis': 'PositiveY'}), ('rotation', {})]
    for k, c in enumerate(springs[:3]):
        kind, extra = cons[k]
        v1['nodes'][c[-1]].setdefault('extensions', {})['VRMC_node_constraint'] = {
            'specVersion': '1.0', 'constraint': {kind: dict(extra, source=c[0], weight=0.5)}}
    for m in v1['materials']:
        m.setdefault('extensions', {})['VRMC_materials_mtoon'] = {
            'specVersion': '1.0', 'shadeColorFactor': [0.8, 0.7, 0.7], 'outlineWidthMode': 'worldCoordinates', 'outlineWidthFactor': 0.005,
            'outlineColorFactor': [0.1, 0.1, 0.1], 'outlineLightingMixFactor': 1.0, 'renderQueueOffsetNumber': 0, 'transparentWithZWrite': False,
            'shadingShiftFactor': -0.05, 'shadingToonyFactor': 0.9, 'rimLightingMixFactor': 0.5}
        if v1.get('textures'):
            m['extensions']['VRMC_materials_mtoon'].update(shadeMultiplyTexture={'index': 0}, matcapTexture={'index': 0},
                                                           matcapFactor=[1, 1, 1])
    open(os.path.join(out, 'vrm1.glb'), 'wb').write(glb_bytes(v1, bin_))

    subprocess.run([sys.executable, os.path.join(REPO, 'tools', 'test', 'vm', 'assets.py'), out], check=True, stdout=subprocess.DEVNULL)
    # (the VRMs' own humanoid, faces and springs, not the settings file's, which would come first)
    mine = {k: v for k, v in settings.items() if k not in ('humanoid', 'expressions', 'springs', 'colliders', 'gestures')}
    for name in ('vrm0', 'vrm1'):
        json.dump(mine, open(os.path.join(out, f'{name}.hypr3d.json'), 'w', encoding='utf-8'), ensure_ascii=False)
    json.dump({'emotes': settings['emotes'][-1:]}, open(os.path.join(out, 'toon.hypr3d.json'), 'w', encoding='utf-8'))
    shutil.move(os.path.join(out, 'ToonTest.glb'), os.path.join(out, 'toon.glb'))
    os.remove(os.path.join(out, 'TestRoom.glb'))
    subprocess.run([sys.executable, os.path.join(REPO, 'tools', 'test', 'harness', 'toonballs.py'), out], check=True, stdout=subprocess.DEVNULL)
    shutil.move(os.path.join(out, 'ToonBalls.glb'), os.path.join(out, 'balls.glb'))
    json.dump({'emotes': settings['emotes'][-1:]}, open(os.path.join(out, 'balls.hypr3d.json'), 'w', encoding='utf-8'))
    return {n: (os.path.join(out, n + '.glb'), os.path.join(out, n + '.hypr3d.json'), emote)
            for n in ('plain', 'vrm0', 'vrm1', 'toon', 'balls')}


# ------------------------------------------------------------------ mutations

NAN, INF = float('nan'), float('inf')
NUMBERS = [0, -1, 1, -2147483648, 2147483647, 4294967295, 4294967296, 2 ** 53 + 1, 1e308, -1e308, 1e-310, 0.5, -0.5, 65535, 65536,
           1e9, 1e20, NAN, INF, -INF]
OTHERS = ['', 'x', [], {}, None, True, False, [0], [NAN, INF], {'a': 1}, [[[]]], 'data:application/octet-stream;base64,####']
# keys whose numbers are indices into a top-level array
INDEX = {'node': 'nodes', 'mesh': 'meshes', 'material': 'materials', 'image': 'images', 'source': 'images', 'sampler': 'samplers',
         'texture': 'textures', 'index': None, 'bufferView': 'bufferViews', 'buffer': 'buffers', 'accessor': 'accessors',
         'indices': 'accessors', 'skin': 'skins', 'inverseBindMatrices': 'accessors', 'skeleton': 'nodes', 'center': 'nodes',
         'input': 'accessors', 'output': 'accessors', 'firstPersonBone': 'nodes', 'children': 'nodes', 'joints': 'nodes',
         'bones': 'nodes', 'colliders': None, 'colliderGroups': None, 'nodes': 'nodes', 'scene': 'scenes'}


def paths(v, at=()):
    """every place in a JSON tree: (path, value)"""
    yield at, v
    if isinstance(v, dict):
        for k, x in v.items():
            yield from paths(x, at + (k,))
    elif isinstance(v, list):
        for i, x in enumerate(v[:64]):  # (big arrays: their first elements)
            yield from paths(x, at + (i,))


def get(root, path):
    for k in path:
        root = root[k]
    return root


def number(rng, v, key, js):
    """a bad number for this one"""
    table = INDEX.get(key) if isinstance(key, str) else None
    if table and isinstance(js.get(table), list) and rng.random() < 0.6:
        n = len(js[table])
        return rng.choice([-1, n, n + 1, n + 1000, 2147483647, -2147483648] + ([rng.randrange(n)] if n else []))
    r = rng.random()
    if r < 0.5:
        return rng.choice(NUMBERS)
    if r < 0.7 and isinstance(v, (int, float)) and not isinstance(v, bool) and math.isfinite(v):
        return type(v)(v * rng.choice([-1, 1000, 1e6, 1e12])) if isinstance(v, float) else int(v * rng.choice([-1, 1000, 1000000]))
    return rng.choice(OTHERS)


def mutate_json(js, rng, roots, desc):
    """one change somewhere under one of `roots` (paths; () is anywhere)"""
    root = rng.choice(roots)
    try:
        base = get(js, root)
    except (KeyError, IndexError, TypeError):
        base = js
        root = ()
    places = [p for p in paths(base, root)][1:] or [(root, base)]
    path, v = rng.choice(places)
    parent, key = (get(js, path[:-1]), path[-1]) if path else (None, None)
    op = rng.random()
    if parent is None:
        return
    if op < 0.12:
        # gone
        if isinstance(parent, dict):
            del parent[key]
        else:
            del parent[key]
        desc.append(f'del {path}')
        return
    if isinstance(v, (int, float)) and not isinstance(v, bool):
        new = number(rng, v, key if isinstance(key, str) else (path[-2] if len(path) > 1 else None), js)
    elif isinstance(v, str):
        new = rng.choice(['', 'x' * 100000, '../' * 30 + 'etc/passwd', 'data:image/png;base64,' + 'A' * 7, 'ÿ' * 10, 12, None])
    elif isinstance(v, list):
        r = rng.random()
        if r < 0.25:
            new = []
        elif r < 0.45 and v:
            new = v[:rng.randrange(len(v))]
        elif r < 0.6 and v:
            new = v + [copy.deepcopy(rng.choice(v)) for _ in range(rng.choice([1, 3, 100]))]
        elif r < 0.8 and v:
            new = list(v)
            i = rng.randrange(len(v))
            new[i] = number(rng, v[i], path[-1] if isinstance(path[-1], str) else None, js) if not isinstance(v[i], (dict, list)) else rng.choice(OTHERS)
        else:
            new = rng.choice(OTHERS)
    elif isinstance(v, dict):
        r = rng.random()
        if r < 0.3:
            new = {}
        elif r < 0.6 and v:
            new = dict(v)
            del new[rng.choice(list(v))]
        else:
            new = rng.choice(OTHERS + [NAN, -1, 1e308])
    else:
        new = rng.choice(NUMBERS + OTHERS)
    parent[key] = new
    desc.append(f'set {path} = {str(new)[:60]}')


# accessor component types: struct code and extreme values
COMP = {5120: ('b', [127, -128]), 5121: ('B', [255]), 5122: ('h', [32767, -32768]), 5123: ('H', [65535]), 5125: ('I', [4294967295, 2 ** 31]),
        5126: ('f', [NAN, INF, -INF, 1e30, -1e30, 0.0])}
NCOMP = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4, 'MAT2': 4, 'MAT3': 9, 'MAT4': 16}


def dicts(v):
    """the dicts in what should be a list of them (a mutation may have left anything there)"""
    return [x for x in v if isinstance(x, dict)] if isinstance(v, list) else []


def accessor_uses(js):
    """accessor -> what it is (indices, JOINTS_0, POSITION, ...), for weighing"""
    use = {}
    for m in dicts(js.get('meshes')):
        for p in dicts(m.get('primitives')):
            if isinstance(p.get('indices'), int):
                use[p['indices']] = 'indices'
            for k, a in (p.get('attributes') if isinstance(p.get('attributes'), dict) else {}).items():
                if isinstance(a, int):
                    use[a] = k
    for s in dicts(js.get('skins')):
        if isinstance(s.get('inverseBindMatrices'), int):
            use[s['inverseBindMatrices']] = 'ibm'
    for a in dicts(js.get('animations')):
        for s in dicts(a.get('samplers')):
            for k in ('input', 'output'):
                if isinstance(s.get(k), int):
                    use[s[k]] = 'anim ' + k
    return use


def mutate_bin(js, bin_, rng, desc):
    """a change to the binary chunk: (json, bin, raw bytes of the whole file or None)"""
    try:
        return mutate_bin_(js, bin_, rng, desc)
    except (KeyError, IndexError, TypeError, ValueError, AttributeError, struct.error):
        return js, bin_, None  # (earlier mutations left nothing sensible to change there)


def mutate_bin_(js, bin_, rng, desc):
    r = rng.random()
    b = bytearray(bin_)
    if r < 0.45 and js.get('accessors'):
        # values in an accessor's data: indices and joints past the ends, NaN and huge floats
        use = accessor_uses(js)
        wanted = [i for i, u in use.items() if u in ('indices', 'JOINTS_0', 'POSITION', 'ibm', 'anim input', 'anim output', 'WEIGHTS_0')]
        i = rng.choice(wanted) if wanted and rng.random() < 0.8 else rng.randrange(len(js['accessors']))
        a = js['accessors'][i]
        bv = js.get('bufferViews', [])[a['bufferView']] if isinstance(a.get('bufferView'), int) and a['bufferView'] < len(js.get('bufferViews', [])) else None
        if not bv or a.get('componentType') not in COMP:
            return js, bin_, None
        code, bad = COMP[a['componentType']]
        size = struct.calcsize(code)
        stride = bv.get('byteStride') or size * NCOMP.get(a.get('type'), 1)
        base = bv.get('byteOffset', 0) + a.get('byteOffset', 0)
        n = max(1, a.get('count', 1))
        for _ in range(rng.choice([1, 3, 20])):
            at = base + rng.randrange(n) * stride + rng.randrange(NCOMP.get(a.get('type'), 1)) * size
            if 0 <= at and at + size <= len(b):
                struct.pack_into('<' + code, b, at, rng.choice(bad))
        desc.append(f'bin: accessor {i} ({use.get(i, "?")}) values')
        return js, bytes(b), None
    if r < 0.65 and js.get('images'):
        i = rng.randrange(len(js['images']))
        im = js['images'][i]
        bv = js['bufferViews'][im['bufferView']] if isinstance(im.get('bufferView'), int) and im['bufferView'] < len(js.get('bufferViews', [])) else None
        if not bv:
            return js, bin_, None
        o, n = bv.get('byteOffset', 0), bv.get('byteLength', 0)
        k = rng.random()
        if k < 0.4:
            for _ in range(rng.choice([1, 4, 32])):  # (a PNG's header, its chunks, the data)
                at = o + min(n - 1, int(rng.expovariate(1 / 64))) if n else o
                if at < len(b):
                    b[at] = rng.randrange(256)
            desc.append(f'bin: image {i} bytes')
        elif k < 0.7:
            # its size in the header: huge
            if n > 24 and b[o:o + 8] == b'\x89PNG\r\n\x1a\n':
                struct.pack_into('>II', b, o + 16, rng.choice([0, 1, 65535, 1 << 24, 0x7fffffff]), rng.choice([0, 1, 65535, 1 << 24, 0x7fffffff]))
            desc.append(f'bin: image {i} header size')
        else:
            bv['byteLength'] = rng.choice([0, 1, 8, 33, max(0, n // 2)])
            desc.append(f'bin: image {i} cut to {bv["byteLength"]} bytes')
        return js, bytes(b), None
    if r < 0.8:
        cut = rng.randrange(len(b) + 1) if b else 0
        desc.append(f'bin: the chunk cut to {cut} of {len(b)}')
        return js, bytes(b[:cut]), None
    # the file itself: cut short, or its header's lengths wrong
    whole = bytearray(glb_bytes(js, bin_))
    k = rng.random()
    if k < 0.5:
        cut = rng.randrange(len(whole))
        desc.append(f'file cut to {cut} of {len(whole)}')
        return js, bin_, bytes(whole[:cut])
    field = rng.choice([8, 12, 12 + 8 + struct.unpack_from('<I', whole, 12)[0]])
    if field + 4 <= len(whole):
        struct.pack_into('<I', whole, field, rng.choice([0, 1, 3, 0x7fffffff, 0xffffffff, len(whole) * 2]))
    desc.append(f'header length at {field}')
    return js, bin_, bytes(whole)


def raw_json_cut(js, rng, desc):
    text = json.dumps(js, separators=(',', ':'), allow_nan=True).encode()
    k = rng.random()
    if k < 0.5:
        cut = rng.randrange(len(text))
        desc.append(f'JSON text cut at {cut}')
        return text[:cut]
    at = rng.randrange(len(text))
    junk = rng.choice([b'{', b'[', b'"', b'\\', b'\x00', b'}}}}', b'1e99999', b'-', b'[' * 5000])
    desc.append(f'JSON text: {junk[:8]!r} at {at}')
    return text[:at] + junk + text[at:]


MAP_ROOTS = [('extensions', 'HYPR3D_lighting')] * 6 + [('materials',)] * 3 + [('accessors',), ('bufferViews',), ('images',), ('textures',),
                                                                                ('samplers',), ('buffers',), ('meshes',), ('nodes',), ('scenes',), ()]


def mutate_map(seed_js, seed_bin, rng):
    js, bin_, raw, desc = copy.deepcopy(seed_js), seed_bin, None, []
    for _ in range(rng.choice([1, 1, 2, 3])):
        r = rng.random()
        if r < 0.72:
            roots = MAP_ROOTS
            if rng.random() < 0.3:
                # the material extensions and texture transforms, and the backdrop's node
                roots = [('materials', i, 'extensions') for i, m in enumerate(js.get('materials', [])) if isinstance(m, dict) and m.get('extensions')]
                roots += [('nodes', i) for i, n in enumerate(js.get('nodes', [])) if isinstance(n, dict) and n.get('name') in ('hypr3d_backdrop', 'hypr3d_spawn', 'hypr3d_desktop')]
                roots = roots or MAP_ROOTS
            mutate_json(js, rng, roots, desc)
        elif r < 0.95:
            js, bin_, raw = mutate_bin(js, bin_, rng, desc)
            if raw is not None:
                return raw, desc
        else:
            text = raw_json_cut(js, rng, desc)
            return glb_bytes(None, bin_, raw_json=text), desc
    return glb_bytes(js, bin_), desc


AVATAR_ROOTS = [('extensions',)] * 4 + [('nodes',)] * 2 + [('skins',), ('meshes',), ('accessors',), ('bufferViews',), ('images',),
                                                          ('materials',), ('animations',), ()]


def mutate_avatar(seed, rng, case_dir):
    """writes avatar.glb, avatar.hypr3d.json and dance.vrma into case_dir"""
    glb, settings, emote = seed
    js, bin_ = read_glb(glb)
    sjs = json.load(open(settings, encoding='utf-8'))
    ejs, ebin = read_glb(emote)
    desc, raw, sraw, eraw = [], None, None, None
    for _ in range(rng.choice([1, 1, 2, 3])):
        r = rng.random()
        if r < 0.5:
            roots = AVATAR_ROOTS
            if rng.random() < 0.5:
                roots = [('extensions', k) for k in (js.get('extensions') or {})] or AVATAR_ROOTS
            mutate_json(js, rng, roots, desc)
        elif r < 0.65:
            js, bin_, raw = mutate_bin(js, bin_, rng, desc)
        elif r < 0.85:
            if rng.random() < 0.1:
                sraw = json.dumps(sjs)[:rng.randrange(len(json.dumps(sjs)))]
                desc.append('settings: cut short')
            else:
                d = []
                mutate_json(sjs, rng, [()] + [(k,) for k in sjs], d)
                desc += ['settings: ' + x for x in d]
        else:
            d = []
            if rng.random() < 0.6:
                mutate_json(ejs, rng, [('extensions',), ('animations',), ('accessors',), ('nodes',)], d)
            else:
                ejs, ebin, eraw = mutate_bin(ejs, ebin, rng, d)
            desc += ['emote: ' + x for x in d]
    os.makedirs(case_dir, exist_ok=True)
    open(os.path.join(case_dir, 'avatar.glb'), 'wb').write(raw if raw is not None else glb_bytes(js, bin_))
    with open(os.path.join(case_dir, 'avatar.hypr3d.json'), 'w', encoding='utf-8') as f:
        f.write(sraw if sraw is not None else json.dumps(sjs, ensure_ascii=False, allow_nan=True))
    open(os.path.join(case_dir, 'dance.vrma'), 'wb').write(eraw if eraw is not None else glb_bytes(ejs, ebin))
    return desc


# ------------------------------------------------------------------ running

def command(harness, case, avatar, png, wav):
    if not avatar:
        return [harness, '--map', case, '--size', '256x144', '--spawn', '--frames', '3', '--out', png]
    return [harness, '--avatar', os.path.join(case, 'avatar.glb'), '--size', '192x108', '--frames', '10', '--parts', '--emotes',
            '--emote', 'Fuzz', '--frames', '8', '--emote', os.path.join(case, 'dance.vrma'), 'once', '--frames', '8',
            '--gesture', 'both', '3', '--expr', 'happy', '1', '--look', '30', '-10', '--blink', '1', '--frames', '4',
            '--audio', wav, '--frames', '10', '--visemes', '--menu', 'outfit', '--menu-pick', '1', '--menu', 'emotes', '--menu-close', '--out', png]


def classify(code, out, timed_out):
    """(kind, where) or None when it's fine"""
    if timed_out:
        return 'hang', ''
    m = re.search(r'ERROR: AddressSanitizer: ([\w-]+)', out)
    if m or 'hard rss limit' in out:
        kind = 'alloc' if (m and m.group(1) in ('allocation-size-too-big', 'out-of-memory', 'requested-allocation-size-exceeds-maximum-supported-size'))\
            or 'hard rss limit' in out or 'allocation-size-too-big' in out else 'asan'
        # (the access's stack: the first, before where the memory was allocated or freed)
        stack = re.split(r'\n(?:0x[0-9a-f]+ is located|freed by|previously allocated|allocated by)', out[m.start() if m else 0:])[0]
        frames = re.findall(r'#\d+ 0x[0-9a-f]+ in .*? (?:\S*/)?((?:src|tools)/[\w/.]+:\d+)', stack)
        where = next((f.split('/')[-1] for f in frames if 'third_party' not in f and 'harness' not in f), frames[0].split('/')[-1] if frames else '')
        return kind + '-' + (m.group(1) if m else 'rss'), where
    m = re.search(r'((?:src|tools)/[\w/.]+):(\d+):\d+: runtime error: ([^\n]+)', out)
    if m:
        what = re.sub(r"[^a-z]+", '-', m.group(3).split(' of ')[0].split(' for ')[0].lower())[:40].strip('-')
        return 'ubsan-' + what, f'{m.group(1).split("/")[-1]}:{m.group(2)}'
    if code < 0 or code >= 128:
        return 'crash', f'signal {-code if code < 0 else code - 128}'
    if code not in (0, 1):
        return 'error', f'exit {code}'
    if code == 1 and not re.search(r'(map|avatar) failed:', out):
        return 'error', 'exit 1 without a message'
    return None


def run_one(harness, case, avatar, timeout, wav, log):
    png = (case if not avatar else os.path.join(case, 'out')) + '.png'
    env = dict(os.environ, **MESA, **SAN)
    t0 = time.time()
    try:
        p = subprocess.run(command(harness, case, avatar, png, wav), env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           timeout=timeout, cwd=os.path.dirname(case))
        code, out, timed_out = p.returncode, p.stdout.decode('utf-8', 'replace'), False
    except subprocess.TimeoutExpired as e:
        code, out, timed_out = -9, (e.stdout or b'').decode('utf-8', 'replace'), True
    if os.path.exists(png):
        os.remove(png)
    open(log, 'w', encoding='utf-8').write(out)
    return classify(code, out, timed_out), time.time() - t0, out


def fuzz(args):
    avatar = args.what == 'avatar'
    out = os.path.abspath(args.out)
    for d in ('seeds', 'cases', 'findings'):
        os.makedirs(os.path.join(out, d), exist_ok=True)
    wav = os.path.join(out, 'seeds', 'wav', 'woman_a.wav')  # (a vowel for lip sync to open the mouth with)
    if not os.path.exists(wav):
        subprocess.run([sys.executable, os.path.join(REPO, 'tools', 'test', 'synth', 'vowels.py'), os.path.dirname(wav)], check=True,
                       stdout=subprocess.DEVNULL)
    if avatar:
        if not args.avatars:
            sys.exit('fuzz.py avatar needs --avatars DIR (BoothAccessories.glb, its .hypr3d.json and BoothGimmicks.hands.vrma)')
        seeds = avatar_seeds(os.path.join(out, 'seeds'), args.avatars)
    else:
        seeds = {k: read_glb(v) for k, v in map_seeds(os.path.join(out, 'seeds')).items()}
    names = sorted(seeds)

    # the seeds themselves must be fine
    for n in names:
        case = seeds[n][0] if avatar else os.path.join(out, 'seeds', n + '.glb')
        if avatar:
            d = os.path.join(out, 'seeds', 'case-' + n)
            os.makedirs(d, exist_ok=True)
            for src, dst in ((seeds[n][0], 'avatar.glb'), (seeds[n][1], 'avatar.hypr3d.json'), (seeds[n][2], 'dance.vrma')):
                shutil.copy(src, os.path.join(d, dst))
            case = d
        res, secs, text = run_one(args.harness, case, avatar, args.timeout * 2, wav, os.path.join(out, 'seeds', n + '.log'))
        print(f'seed {n}: {res or "fine"} ({secs:.1f} s)', flush=True)
        if res:
            print(text[-3000:])
            sys.exit(f'the seed {n} already fails: fix that first')

    def make(i):
        rng = random.Random(f'{args.seed}:{i}')
        n = names[i % len(names)]
        if avatar:
            case = os.path.join(out, 'cases', f'case-{i:05d}-{n}')
            desc = mutate_avatar(seeds[n], rng, case)
        else:
            case = os.path.join(out, 'cases', f'case-{i:05d}-{n}.glb')
            data, desc = mutate_map(*seeds[n], rng)
            open(case, 'wb').write(data)
        return case, desc

    counts, found, t0 = {}, {}, time.time()
    with open(os.path.join(out, 'cases.jsonl'), 'a', encoding='utf-8') as index, \
            concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        def job(i):
            case, desc = make(i)
            res, secs, text = run_one(args.harness, case, avatar, args.timeout, wav, case.rstrip('/') + '.log')
            return i, case, desc, res, secs, text
        for i, case, desc, res, secs, text in pool.map(job, range(args.start, args.start + args.n)):
            kind = f'{res[0]} {res[1]}' if res else 'fine'
            counts[res[0] if res else 'fine'] = counts.get(res[0] if res else 'fine', 0) + 1
            index.write(json.dumps({'case': os.path.basename(case), 'mutations': desc, 'result': kind, 'seconds': round(secs, 2)}) + '\n')
            if res:
                key = re.sub(r'[^\w.:-]+', '_', f'{res[0]}-{res[1]}')[:80]
                if key not in found:
                    found[key] = os.path.basename(case)
                    d = os.path.join(out, 'findings', key)
                    os.makedirs(d, exist_ok=True)
                    (shutil.copytree if avatar else shutil.copy)(case, os.path.join(d, os.path.basename(case)))
                    open(os.path.join(d, 'log.txt'), 'w', encoding='utf-8').write(text)
                    open(os.path.join(d, 'mutations.txt'), 'w', encoding='utf-8').write('\n'.join(desc) + '\n')
                print(f'{os.path.basename(case)}: {kind} <- {"; ".join(desc)[:200]}', flush=True)
            elif (i + 1) % 50 == 0:
                print(f'{i + 1 - args.start} cases, {time.time() - t0:.0f} s', flush=True)
    summary = [f'{args.what}: {args.n} cases (seed {args.seed!r}, from {args.start}) in {time.time() - t0:.0f} s: ' +
               ', '.join(f'{v} {k}' for k, v in sorted(counts.items()))] + [f'  {k}: first {v}' for k, v in sorted(found.items())]
    open(os.path.join(out, 'summary.txt'), 'a', encoding='utf-8').write('\n'.join(summary) + '\n')
    print('\n'.join(summary))
    return 1 if found else 0


def replay(args):
    case = os.path.abspath(args.case)
    avatar = os.path.isdir(case)
    wav = os.path.join(REPO, 'build-asan', 'wav', 'woman_a.wav')
    if not os.path.exists(wav):
        subprocess.run([sys.executable, os.path.join(REPO, 'tools', 'test', 'synth', 'vowels.py'), os.path.dirname(wav)], check=True,
                       stdout=subprocess.DEVNULL)
    res, secs, text = run_one(args.harness, case, avatar, args.timeout, wav, os.path.join(os.path.dirname(case), 'replay.log'))
    print(text[-args.tail:] if args.tail else text)
    print(f'-> {res or "fine"} ({secs:.1f} s)')
    return 1 if res else 0


def main():
    ap = argparse.ArgumentParser(description='fuzz hypr3d\'s map and avatar loaders (see the top of this file)')
    ap.add_argument('what', choices=['map', 'avatar', 'replay'])
    ap.add_argument('out', help='OUTDIR, or the case to replay')
    ap.add_argument('-n', type=int, default=300, help='cases (300)')
    ap.add_argument('--start', type=int, default=0, help='the first case number (cases are numbered, each from its own seed)')
    ap.add_argument('--seed', default='hypr3d')
    ap.add_argument('--jobs', type=int, default=max(1, (os.cpu_count() or 2) // 3))
    ap.add_argument('--timeout', type=float, default=90.0, help='seconds a case may take (90)')
    ap.add_argument('--avatars', help='BoothAccessories.glb, BoothAccessories.hypr3d.json and BoothGimmicks.hands.vrma (run.sh --avatars)')
    ap.add_argument('--harness', default=HARNESS)
    ap.add_argument('--tail', type=int, default=6000, help='replay: the last this many characters of the output (0: all)')
    args = ap.parse_args()
    if not os.path.exists(args.harness):
        sys.exit(f'no {args.harness}: build it with ./build.sh -f tools/test/fuzz/asan.mk')
    if args.what == 'replay':
        args.case = args.out
        sys.exit(replay(args))
    sys.exit(fuzz(args))


if __name__ == '__main__':
    main()
