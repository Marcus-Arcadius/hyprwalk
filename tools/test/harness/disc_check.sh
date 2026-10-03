#!/usr/bin/env bash
# disc_check.sh: disc colliders and per-bone spring radii (the plugin's physics, through the harness's --springdump).
# Short chains fall by their own gravity onto, up into and sideways into flat discs (0.4 m radius, 2 cm thick each
# side; a tail over a tutu, which a ring of capsules lets through): each joint must stay out by its own radius, on the
# side it started, for 3 s.
#
#   tools/test/harness/disc_check.sh [DIR]   (DIR: where the model and logs go, a temporary one by default)
#
# Needs build/test/shot (tools/test/harness/build.sh).
set -uo pipefail
REPO="$(cd "$(dirname "$0")/../../.." && pwd)"
SHOT="$REPO/build/test/shot"
[[ -x "$SHOT" ]] || { echo "no $SHOT: tools/test/harness/build.sh builds it" >&2; exit 1; }
DIR="${1:-}"
[[ -n "$DIR" ]] || { DIR="$(mktemp -d)"; trap 'rm -rf "$DIR"' EXIT; }
mkdir -p "$DIR"

# the model: chains of three 25 cm bones with level discs on "Root", and a triangle to draw; discs.design.json gives the
# side each chain must stay on (1 over, -1 under, 0 any)
python3 - "$DIR" << 'EOF'
import base64, json, struct, sys
D = sys.argv[1]
CHAINS = {  # name: (where it starts, which way its bones go, its discs' middles, the side it stays, gravityDir)
    'Down': ((0, 1.5, 0), (1, 0, 0), [(0.35, 1.45, 0)], 1, (0, -1, 0)),       # lying 5 cm over it, pulled down
    'Up': ((0, 1.0, 0.8), (1, 0, 0), [(0.35, 1.05, 0.8)], -1, (0, 1, 0)),     # 5 cm under it, pulled up
    'Edge': ((0, 1.05, -0.8), (0, 0, 1), [(0.5, 1.05, -0.8), (-0.5, 1.05, -0.8)], 0, (-1, 0, 0)),  # 8 cm off each
    'Free': ((0, 1.5, -1.6), (1, 0, 0), [], 0, (0, -1, 0)),                    # no disc: it hangs on down
}
nodes = [{'name': 'Root', 'children': [1]}, {'name': 'Mesh', 'mesh': 0}]
for name, (start, way, _, _, _) in CHAINS.items():
    first = len(nodes)
    nodes[0]['children'].append(first)
    for k in range(3):
        nd = {'name': '%s.%d' % (name, k + 1), 'translation': list(start) if k == 0 else [0.25 * x for x in way]}
        if k < 2:
            nd['children'] = [first + k + 1]
        nodes.append(nd)
pos = struct.pack('<9f', -0.2, 0, 0, 0.2, 0, 0, 0, 1.6, 0)
gltf = {'asset': {'version': '2.0'}, 'scene': 0, 'scenes': [{'nodes': [0]}], 'nodes': nodes,
        'meshes': [{'primitives': [{'attributes': {'POSITION': 0}}]}],
        'buffers': [{'byteLength': len(pos), 'uri': 'data:application/octet-stream;base64,' + base64.b64encode(pos).decode()}],
        'bufferViews': [{'buffer': 0, 'byteLength': len(pos)}],
        'accessors': [{'bufferView': 0, 'componentType': 5126, 'count': 3, 'type': 'VEC3', 'min': [-0.2, 0, 0], 'max': [0.2, 1.6, 0]}]}
json.dump(gltf, open(D + '/discs.gltf', 'w'))
settings = {'colliders': [], 'springs': []}
for name, (start, way, middles, side, down) in CHAINS.items():
    for m in middles:
        settings['colliders'].append({'name': name, 'node': 'Root', 'offset': list(m), 'disc': {'normal': [0, 1, 0], 'radius': 0.4},
                                      'radius': 0.02})
    settings['springs'].append({'name': name, 'bones': [name + '.1'], 'stiffness': 0.1, 'drag': 0.4, 'gravity': 1.0,
                                'gravityDir': list(down), 'radius': [0.04, 0.05], 'colliders': [name] if middles else []})
json.dump(settings, open(D + '/discs.hypr3d.json', 'w'), indent=1)
json.dump({name: c[3] for name, c in CHAINS.items()}, open(D + '/discs.design.json', 'w'))
EOF
"$SHOT" --size 64x64 --avatar "$DIR/discs.gltf" --springdump "$DIR/discs.dump" --frames 180 > "$DIR/discs.log" 2>&1 ||
    { echo "FAIL the harness: $(tail -3 "$DIR/discs.log")"; exit 1; }

python3 - "$DIR/discs.dump" "$DIR/discs.design.json" << 'EOF'
import json, math, sys
side = json.load(open(sys.argv[2]))
joints, colliders, springs, frames = [], [], {}, []
for line in open(sys.argv[1]):
    p = line.split()
    if line.startswith('# joint'):
        joints.append({'name': p[2], 'spring': p[3], 'radius': float(p[4])})
    elif line.startswith('# collider'):
        colliders.append({'kind': int(p[3]), 'radius': float(p[4]), 'disc': float(p[6])})
    elif line.startswith('# spring'):
        springs[p[2]] = [int(k) for k in p[3:]]
    elif line.strip():
        v = list(map(float, p))
        J = [v[1 + 6 * i:7 + 6 * i] for i in range(len(joints))]
        o = 1 + 6 * len(joints)
        frames.append((v[0], J, [v[o + 6 * i:o + 6 + 6 * i] for i in range(len(colliders))]))
fails = 0


def check(what, ok, got=''):
    global fails
    print('%s %s%s' % ('ok  ' if ok else 'FAIL', what, '  [%s]' % got if got != '' else ''))
    fails += 0 if ok else 1


check('four discs (kind 3, 0.4 m to where their edge starts, 2 cm round that)',
      [(c['kind'], round(c['disc'], 3), round(c['radius'], 3)) for c in colliders] == [(3, 0.4, 0.02)] * 4,
      [(c['kind'], c['disc'], c['radius']) for c in colliders])
check("each chain's radius bone by bone: 4 cm, then 5 for the rest", all(
    [round(j['radius'], 3) for j in joints if j['spring'] == s] == [0.04, 0.05, 0.05] for s in side),
    [(j['name'], j['radius']) for j in joints])
# each joint's tail from each of its chain's discs (level, whichever way the model is turned; its middle from the dump,
# its size as the model has it): how far out of it as far as the joint's radius (< 0: in it), on the side it must stay
for name, what in (('Down', 'pulled down onto its top: on it'), ('Up', 'pulled up into its underside: under it'),
                   ('Edge', 'pulled sideways into the edge of one: out of it')):
    worst, at = 1e9, None
    for t, J, C in frames[1:]:  # (from the first step on: the first frame is as the model has it, in it a little)
        for k in springs[name]:
            a = C[k][:3]
            for i, j in enumerate(joints):
                if j['spring'] != name:
                    continue
                d = [J[i][3 + q] - a[q] for q in range(3)]
                h, r = d[1], math.hypot(d[0], d[2])
                out = math.hypot(h, max(r - 0.4, 0)) - 0.02 - j['radius']
                if side[name] and h * side[name] < 0 and r < 0.42 + j['radius']:
                    out = -abs(h) - 1  # (through it)
                if out < worst:
                    worst, at = out, (round(t, 2), j['name'], round(h, 3), round(r, 3))
    check('%s chain, %s (each joint out of it as far as its radius, within 5 mm, for 3 s)' % (name, what), worst > -0.005,
          'the least %.3f m, at %s (s, joint, over, out from its middle)' % (worst, at))
# the edge chain went to a disc's edge (else it wasn't pulled into one), and the one with no disc hangs on down
near = min(math.hypot(J[i][3] - C[k][0], J[i][5] - C[k][2]) for t, J, C in frames for k in springs['Edge']
           for i, j in enumerate(joints) if j['spring'] == 'Edge')
check('... and it was pulled against it (a joint within 10 cm of its edge)', near < 0.52, '%.3f m from a middle' % near)
tip = [i for i, j in enumerate(joints) if j['name'] == 'Free.3'][0]
drop = 1.5 - frames[-1][1][tip][4]
check('with no disc the same chain hangs on down (its tip over 40 cm below the line it lay on)', drop > 0.4, '%.2f m' % drop)
print('%d failed' % fails if fails else 'all ok')
sys.exit(1 if fails else 0)
EOF
