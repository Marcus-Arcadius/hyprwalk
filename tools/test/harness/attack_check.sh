#!/usr/bin/env bash
# attack_check.sh: the avatar's attacks (the plugin's left click on nothing, hyprctl hypr3d avatar attack; the harness's
# --attack, the plugin's own animator), as the built in clips have them (assets/attack.vrma and
# attack-first-person.vrma: a hook made in Blender, see tools/blender/README.md): a click swings the right arm, the fists
# closed: wound up with the right fist out past the shoulder (seen from behind, past long hair), ahead of the body, the
# chest turned a little right; then swept round in front of the face as the chest turns left, the left fist up by the
# chin; held, back to a guard and down to where they hung. Clicks while one swings are the other arm's (the mirror
# image), one after the other (R L R L), a click while one's waiting is dropped, after a pause the right's again; nothing
# jumps (each wrist frame to frame); walking it goes on walking; the hair the springs swing (long twin tails) stays out of
# the arms; in first person the fist drawn back at the bottom right of the view, struck to the crosshair and back to
# where it was held ready, the left held ready, no further ahead than there's room (a wall); an emote stops; a model that
# isn't a humanoid has no arms to swing. From the harness's --attackstatus, --wrists, --fpstatus and --hairclip lines (the
# wrists from the feet in the avatar's frame; in first person where they are in the view, 0..1 across and down) and two
# frames from behind.
#   tools/test/harness/attack_check.sh AVATAR [DIR]   (a humanoid: a VRM, or regress.sh --keep's OUT/new/BoothAccessories.glb;
#                                                     DIR: where the frames and logs go, a temporary one by default)
# Needs build/test/shot (tools/test/harness/build.sh).
set -uo pipefail
REPO="$(cd "$(dirname "$0")/../../.." && pwd)"
SHOT="$REPO/build/test/shot"
[[ -x "$SHOT" ]] || { echo "no $SHOT: tools/test/harness/build.sh builds it" >&2; exit 1; }
AVATAR="${1:-}"
[[ -f "$AVATAR" ]] || { echo "usage: attack_check.sh AVATAR [DIR]" >&2; exit 2; }
DIR="${2:-}"
[[ -n "$DIR" ]] || { DIR="$(mktemp -d)"; trap 'rm -rf "$DIR"' EXIT; }
mkdir -p "$DIR"

run() { # name, avatar, then the harness's options (640x360 as a 16:10-ish monitor's view)
    local name="$1" avatar="$2"
    shift 2
    "$SHOT" --size 640x360 --avatar "$avatar" "$@" > "$DIR/$name.log" 2>&1 || { echo "FAIL the harness ($name): $(tail -3 "$DIR/$name.log")"; exit 1; }
}
frames() { # n, then what to print each frame
    local n="$1" i
    shift
    for i in $(seq "$n"); do printf '%s\n' --frames 1 "$@"; done
}
st=(--wrists --attackstatus)
mapfile -t each < <(frames 60 "${st[@]}")
# one click, standing, the hair inside the arms measured from half a second before it; then from behind (as third
# person's camera is, over the right shoulder), at rest and wound up
run single "$AVATAR" --frames 30 --hairclip "$DIR/hair.txt" --frames 30 --wrists --attack next "${each[@]}"
run behind "$AVATAR" --view 180 --pitch 15 --dist 2.6 --shift 0.4 0 0 --frames 60 --out "$DIR/rest.png" --attack next --frames 8 --out "$DIR/wound.png"
# clicks every quarter second (15 frames): R L R L ...; every 3 frames (quicker than they go); a pause between two
mapfile -t combo < <(for k in $(seq 8); do printf '%s\n' --attack next; frames 15 "${st[@]}"; done; frames 50 "${st[@]}")
run combo "$AVATAR" --frames 60 --hairclip "$DIR/combohair.txt" "${combo[@]}"
mapfile -t quick < <(for k in $(seq 12); do printf '%s\n' --attack next; frames 3 --attackstatus; done; frames 40 --attackstatus)
run quick "$AVATAR" --frames 60 "${quick[@]}"
run pause "$AVATAR" --frames 60 --attack next --frames 70 --attackstatus --attack next --frames 1 --attackstatus
# walking ahead, a click
mapfile -t walk < <(frames 30 --wrists --attackstatus --gait)
run walk "$AVATAR" --accel 10 --decel 14 --move 0 -1.6 --frames 90 --wrists --attack next "${walk[@]}"
# first person: a click, looking ahead; quick clicks (nothing jumps); a wall 30 cm ahead
mapfile -t fpeach < <(frames 45 --fpstatus --attackstatus)
run fp "$AVATAR" --fpbody 0 0 --frames 60 --fpstatus --attack next "${fpeach[@]}"
mapfile -t fpcombo < <(for k in $(seq 6); do printf '%s\n' --attack next; frames 15 --wrists; done; frames 50 --wrists)
run fpcombo "$AVATAR" --fpbody 0 0 --frames 60 "${fpcombo[@]}"
run fproom "$AVATAR" --fpbody 0 0 --fproom 0.3 --frames 60 --attack next "${fpeach[@]}"
# an emote, then a click
run emote "$AVATAR" --frames 30 --emote 1 --frames 30 --status --attack next --frames 1 --status
# a model that isn't a humanoid: a triangle
python3 - "$DIR/thing.gltf" << 'EOF'
import base64, json, struct, sys
pos = struct.pack('<9f', 0, 0, 0, 1, 0, 0, 0, 1, 0)
json.dump({'asset': {'version': '2.0'}, 'scene': 0, 'scenes': [{'nodes': [0]}], 'nodes': [{'mesh': 0, 'name': 'thing'}],
           'meshes': [{'primitives': [{'attributes': {'POSITION': 0}}]}],
           'buffers': [{'byteLength': len(pos), 'uri': 'data:application/octet-stream;base64,' + base64.b64encode(pos).decode()}],
           'bufferViews': [{'buffer': 0, 'byteLength': len(pos)}],
           'accessors': [{'bufferView': 0, 'componentType': 5126, 'count': 3, 'type': 'VEC3', 'min': [0, 0, 0], 'max': [1, 1, 0]}]}, open(sys.argv[1], 'w'))
EOF
run thing "$DIR/thing.gltf" --frames 10 --attack next --ctl "avatar attack" --ctl "avatar attack left"
run args "$AVATAR" --frames 10 --ctl "avatar attack sideways" --ctl "avatar attack left" --frames 1 --attackstatus

python3 - "$DIR" << 'EOF'
import json, math, re, struct, sys, zlib

D = sys.argv[1]
fails = 0


def check(what, ok, detail=''):
    global fails
    print(f"{'ok  ' if ok else 'FAIL'} {what}" + (f'  [{detail}]' if detail else ''))
    fails += 0 if ok else 1


def lines(name):
    return open(f'{D}/{name}.log', encoding='utf-8', errors='replace').read().splitlines()


def wrists(name):
    return [tuple(map(float, l.split()[1:7])) for l in lines(name) if l.startswith('wrists:')]


def shoulders(name):
    return [tuple(map(float, l.split('shoulders')[1].split()[0:6])) for l in lines(name) if l.startswith('wrists:') and 'shoulders' in l]


def attacks(name):
    return [json.loads(l[len('attack: '):]) for l in lines(name) if l.startswith('attack: ')]


def hair(name):
    """per frame: per arm (left, right) how many hair vertices are inside it, the deepest (m) and its node"""
    out = []
    for l in open(f'{D}/{name}.txt'):
        if not l.startswith('#'):
            w = l.split()
            out.append(((int(w[1]), float(w[2]), w[3]), (int(w[4]), float(w[5]), w[6])))
    return out


def png(path):
    """(width, height, rows of RGB bytes) of the harness's PNGs (8 bit RGB, stored)"""
    data = open(path, 'rb').read()
    pos, idat = 8, b''
    while pos < len(data):
        n, kind = struct.unpack('>I4s', data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if kind == b'IHDR':
            w, h = struct.unpack('>II', body[:8])
        elif kind == b'IDAT':
            idat += body
        pos += 12 + n
    raw = zlib.decompress(idat)
    return w, h, [raw[y * (w * 3 + 1) + 1:(y + 1) * (w * 3 + 1)] for y in range(h)]


def motion(ws):
    """each wrist's fastest (m/s) and its sharpest change of speed (m/s²) from frame to frame (60 a second)"""
    out = []
    for s in (0, 1):
        v = [math.dist(a[3 * s:3 * s + 3], b[3 * s:3 * s + 3]) * 60 for a, b in zip(ws, ws[1:])]
        out.append((max(v, default=0), max((abs(b - a) * 60 for a, b in zip(v, v[1:])), default=0)))
    return out


def order(at):
    """the arms in the order their swings started"""
    out, seen = '', 0
    for a in at:
        if a['swings'] != seen:
            out += 'L' if a['last'] == 'left' else 'R'
            seen = a['swings']
    return out


m = re.search(r'avatar .*?([\d.]+) m(,| tall)', '\n'.join(lines('single')))
H = float(m.group(1)) if m else 1.6
ws, at = wrists('single'), attacks('single')
rest, during = ws[0], ws[1:]
R, L = [w[3:6] for w in during], [w[0:3] for w in during]
sl, sr = shoulders('single')[0][0:3], shoulders('single')[0][3:6]
# (an arm's length, shoulder to wrist, hanging at rest: what the swing's measured in; a hook's wrist is at most FAST arm
# lengths a second (it strikes at 22 to 26), faster, or a sharper change of speed, is a jump)
A = math.dist(sr, rest[3:6])
FAST, SHARP = 28 * A, 1100 * A
check('a click swings the right arm, the fists closed (the status), the left none',
      at and at[0]['right'] and at[0]['left'] is None and at[0]['last'] == 'right' and any(a['right'] and a['right']['fist'] for a in at), at[0] if at else None)
up = [i for i in range(12) if R[i][1] > sr[1] - 0.25 * A]
wound = max(up, key=lambda i: R[i][0]) if up else max(range(12), key=lambda i: R[i][1])
check('wound up early on (in the first 0.2 s): the right wrist out past its shoulder (a third of the arm\'s length), about as high as it, '
      'ahead of it, not behind (where long hair hangs)', up and R[wound][0] > sr[0] + 0.33 * A and R[wound][2] < sr[2] + 0.05 * A,
      f'{R[wound]} at frame {wound + 1}, the shoulder at {sr}, the arm {A:.2f} m')
struck = next((i for i in range(wound + 1, len(R)) if R[i][2] < sr[2] - 0.6 * A and abs(R[i][0]) < 0.15 * A), None)
check('struck after it: the right wrist swept round in front of the face, near the middle (within 0.15 arm lengths), well ahead of the shoulder '
      '(0.6 of the arm), about as high as it, within 0.3 s of the click', struck is not None and R[struck][1] > sr[1] - 0.25 * A and struck < 18,
      f'{R[struck]} at frame {struck + 1}' if struck is not None else [tuple(round(v, 2) for v in r) for r in R[wound:wound + 10]])
turn = [a['turn'] for a in at]
check('... the chest turned into it: first a little to its right as it winds up (over 0.1 rad), then well to its left (over 0.4 rad) as it strikes',
      max(turn[:10]) > 0.1 and min(turn) < -0.4, f'{max(turn[:10]):.2f}, {min(turn):.2f}')
check('the left fist up by the chin meanwhile (the wrist about as high as its shoulder, the fist over it; in from the shoulder)',
      any(l[1] > sl[1] - 0.1 * A and abs(l[0]) < abs(sl[0]) for l in L[8:20]), [tuple(round(v, 2) for v in l) for l in L[8:20:3]] + [f'shoulder {sl}'])
check('done: both arms back where they hung (within 2 cm) after a second, the status with no swing',
      math.dist(R[-1], rest[3:6]) < 0.02 and math.dist(L[-1], rest[0:3]) < 0.02 and at[-1]['right'] is None,
      f'{math.dist(R[-1], rest[3:6]) * 100:.1f}, {math.dist(L[-1], rest[0:3]) * 100:.1f} cm off; {at[-1]}')
(lv, la), (rv, ra) = motion(ws)
check(f'nothing jumps: no wrist faster than {FAST:.1f} m/s (28 arm lengths a second), no speed changing by more than {SHARP:.0f} m/s² (the strike is the fastest)',
      max(lv, rv) < FAST and max(la, ra) < SHARP, f'left {lv:.1f} m/s {la:.0f} m/s², right {rv:.1f} m/s {ra:.0f} m/s²')

w, h, a = png(f'{D}/rest.png')
_, _, b = png(f'{D}/wound.png')
up = sum(1 for y in range(h // 2) for x in range(w) if max(abs(a[y][3 * x + c] - b[y][3 * x + c]) for c in range(3)) > 24)
check('from behind (as third person\'s camera is): the fist wound up is seen (the top half of the view differs)', up > 0.002 * w * h, f'{up} px')

# the hair: while a swing has the body (from 0.05 s after the click till it starts letting go), no hair deeper inside an
# arm than 3 cm (where the arm swinging through long twin tails sank 7 cm into them)
hs = hair('hair')
swing = [k for k in range(len(at)) if at[k]['weight'] >= 0.99 and 30 + k < len(hs)]
deep = [max(hs[30 + k][s][1] for s in (0, 1)) for k in swing]
worst = max(range(len(deep)), key=lambda k: deep[k]) if deep else 0
# (and more than a strand's tip, a few vertices of it, deeper than 3.5 cm; none deeper than 5)
many = [k for k in swing if any(hs[30 + k][s][1] > 0.035 and hs[30 + k][s][0] > 5 for s in (0, 1))]
cmany = [1 for f, x in zip(hair('combohair'), attacks('combo')) if x['weight'] >= 0.99 and any(f[s][1] > 0.035 and f[s][0] > 5 for s in (0, 1))]
cdeep = max((max(f[s][1] for s in (0, 1)) for f, x in zip(hair('combohair'), attacks('combo')) if x['weight'] >= 0.99), default=0)
check('the hair (what springs swing under the head) stays out of the arms: while a swing has the body, no more of it than a strand\'s tip (5 vertices) '
      'deeper inside an arm (as round as its skin goes out, a sleeve with it) than 3.5 cm, none deeper than 5, one swing or eight',
      not many and not cmany and max(deep, default=0) <= 0.05 and cdeep <= 0.05,
      f'deepest {max(deep, default=0) * 100:.1f} cm ({hs[30 + swing[worst]] if swing else None}); in the combo {cdeep * 100:.1f} cm; '
      f'frames with more: {len(many)}, {len(cmany)}')

c = attacks('combo')
(lv, la), (rv, ra) = motion(wrists('combo'))
check('clicks a quarter of a second apart: one arm then the other (R L R L R L R L)', order(c) == 'RLRLRLRL', order(c))
check(f'... nothing jumps: no wrist faster than {FAST:.1f} m/s, no speed changing by more than {SHARP:.0f} m/s² (one swing taking over from the other)',
      max(lv, rv) < FAST and max(la, ra) < SHARP, f'left {lv:.1f} m/s {la:.0f} m/s², right {rv:.1f} m/s {ra:.0f} m/s²')
check('... all done after the last: no swing, the fists open', c[-1]['left'] is None and c[-1]['right'] is None, c[-1])

q = attacks('quick')
starts = [i for i in range(1, len(q)) if q[i]['swings'] != q[i - 1]['swings']]
gaps = [(b - a) / 60 for a, b in zip(starts, starts[1:])]
check('clicks quicker than the swings go: one at a time, each at least 0.18 s after the one before (once it has struck), one waiting at most',
      order(q).startswith('RLRL') and gaps and min(gaps) >= 0.18 and q[-1]['swings'] < 12, f'{order(q)}, {q[-1]["swings"]} swings for 12 clicks, gaps {[round(g, 2) for g in gaps]}')

p = attacks('pause')
check('after a pause (over 0.9 s), the right arm again', len(p) == 2 and p[0]['last'] == 'right' and p[1]['last'] == 'right' and p[1]['swings'] == 2, [x['last'] for x in p])

wl = lines('walk')
gait = [json.loads(l[len('gait '):]) for l in wl if l.startswith('gait ')]
ww = wrists('walk')
moving = [bool(g and g.get('moving')) for g in gait]
check('walking: it goes on walking (the gait\'s, all through the swing)', gait and all(moving), f'{len(gait)} frames, moving {moving.count(True)}')
check('... and the right fist swings round ahead', min(w[5] for w in ww[1:]) < ww[0][5] - 0.15, f'{min(w[5] for w in ww[1:]):.2f} (was {ww[0][5]:.2f})')

FP = re.compile(r'(left|right)(wrist|elbow) ([-\d.na]+) ([-\d.na]+) \(([\d.]+) m\)')


def fp(name):
    out = []
    for l in lines(name):
        if l.startswith('fp:'):
            out.append({side + part: (float(x), float(y), float(d)) for side, part, x, y, d in FP.findall(l)})
    return out


f = fp('fp')
ready, swing = f[0], f[1:]
rw = [s['rightwrist'] for s in swing]
lw = [s['leftwrist'] for s in swing]
back = max(range(8), key=lambda i: rw[i][0])
check('first person: drawn back to the bottom right of the view from where the hand was held ready, in it (the wrist further right, over 70% across, '
      'at most at the bottom)', rw[back][0] > max(0.7, ready['rightwrist'][0] + 0.05) and rw[back][1] <= 1.0,
      f'{rw[back][:2]} at frame {back + 1}, held at {ready["rightwrist"][:2]}')
mid = [i for i in range(back, len(rw)) if 0.35 < rw[i][0] < 0.65 and 0.4 < rw[i][1] < 0.75]
check('... struck to the crosshair (the wrist just under the middle of the view, the fist up over it) within 0.25 s', mid and mid[0] < 15,
      [tuple(round(v, 2) for v in r[:2]) for r in rw[back:back + 10]])
check('... and back to where it was held ready (within 5% of the view)', math.dist(rw[-1][:2], ready['rightwrist'][:2]) < 0.05,
      f'{rw[-1][:2]} (held at {ready["rightwrist"][:2]})')
check('... the left held ready meanwhile, in the view (within 11% of where it was: pulled in a little; at most at the bottom)',
      max(math.dist(l[:2], ready['leftwrist'][:2]) for l in lw) < 0.11 and max(l[1] for l in lw) <= 1.0,
      f"{max(math.dist(l[:2], ready['leftwrist'][:2]) for l in lw):.3f}, lowest {max(l[1] for l in lw):.2f}")
(lv, la), (rv, ra) = motion(wrists('fpcombo'))
check(f'... quick clicks: nothing jumps (no wrist faster than {FAST:.1f} m/s, no speed changing by more than {SHARP:.0f} m/s²)', max(lv, rv) < FAST and max(la, ra) < SHARP,
      f'left {lv:.1f} m/s {la:.0f} m/s², right {rv:.1f} m/s {ra:.0f} m/s²')
# (how far ahead of the eye: the distance along the view, from where it is in the view, 70° high, 640x360)
TV = math.tan(math.radians(35))
TH = TV * 640 / 360
room = [s['rightwrist'][2] / math.hypot((2 * s['rightwrist'][0] - 1) * TH, (2 * s['rightwrist'][1] - 1) * TV, 1) for s in fp('fproom')]
full = [r for r, a in zip(room[1:], attacks('fproom')) if a['right'] and a['right']['weight'] >= 0.99]
check('... a wall 30 cm ahead: the fist kept short of it (the wrist, the fist beyond it, no further ahead of the eye than the hands held ready are, '
      '25 cm, swinging or not)', full and max(full) <= 0.255 and max(room) <= 0.255, f'{max(full):.3f} m swinging, {max(room):.3f} m at all')

e = [l for l in lines('emote') if l.startswith('playing:')]
check('an emote stops', len(e) == 2 and 'emote' in e[0] and 'emote' not in e[1], e)

t = '\n'.join(lines('thing'))
check('not a humanoid: nothing to swing (the harness\'s, hyprctl\'s an error)', 'attack next: none' in t and t.count('no arms to swing') == 2, re.findall(r'attack.*', t))
g = '\n'.join(lines('args'))
check('hyprctl hypr3d avatar attack: left or right, else an error saying so', 'error: avatar attack [left|right]' in g and '"last": "left"' in g, re.findall(r'ctl avatar attack.*', g))
print(f'{fails} failed' if fails else 'all passed')
sys.exit(1 if fails else 0)
EOF
