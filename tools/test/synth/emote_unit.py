# emote_unit.py: tools/unity2hypr3d.py's --emote on small hand-made clips: a motion sold as bare humanoid clips, with
# no prefab, controller or menu, for the buyer to put in their Action layer. The same package is read as a
# .unitypackage (twice, as when it is an --outfit too), inside a Booth-style .zip (twice), as a folder, clip by clip
# as loose .anim files, and by a clip's name. A package's still poses are left out when it has clips that move, but
# named on their own they are kept; a clip with no muscle or body curves is not an emote; each clip knows where it was
# read from, so convert() adds none twice.
#   blender -b --factory-startup --python-exit-code 1 -P emote_unit.py
import sys, os, shutil, tempfile, zipfile
sys.dont_write_bytecode = True  # (no __pycache__ left in tools/)
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', '..'))  # tools/
sys.path.insert(0, HERE)
import unity2hypr3d as u
import unitygen as g

FAILS = []


def check(what, got, want):
    ok = got == want
    print('%s %s: %r%s' % ('ok  ' if ok else 'FAIL', what, got, '' if ok else ' (want %r)' % (want,)))
    if not ok:
        FAILS.append(what)


def fails(what, fn, text):
    try:
        fn()
    except u.Fail as e:
        check(what, text in str(e), True)
        return
    check(what, 'no error', text)


W = tempfile.mkdtemp(prefix='emote_unit')
PROJ = os.path.join(W, 'proj')
DANCE = 'Assets/しんせ工房/Bare Dances'
os.makedirs(os.path.join(PROJ, 'ProjectSettings'))
CLIPS = {  # name: (curves, loops)
    'Bare_Dance': ([('', 95, 'Spine Front-Back', [(0, 0), (0.5, 0.4), (1, 0)]),
                    ('', 95, 'RootT.y', [(0, 1), (0.5, 0.95), (1, 1)]),
                    ('Body', 137, 'blendShape.あ', [(0, 0), (0.5, 100), (1, 0)])], True),
    'Wave_Once': ([('', 95, 'Right Arm Down-Up', [(0, 0), (1, 0.8)])], False),
    'Proxy_Stand': ([('', 95, 'Spine Front-Back', [(0, 0.1), (0.033, 0.1)]),
                     ('', 95, 'RootT.y', [(0, 1), (0.033, 1)])], False),
    'Face_Only': ([('Body', 137, 'blendShape.まばたき', [(0, 0), (1, 100)])], True),
}
for name, (curves, loops) in CLIPS.items():
    text = g.clip_yaml(name, curves)
    if loops:
        text = text.replace('m_LoopTime: 0', 'm_LoopTime: 1')
    g.write_asset(os.path.join(PROJ, DANCE, name + '.anim'), text, g.guid_of('emote/' + name), 'NativeFormatImporter',
                  g.native(7400000))
g.folder_metas(PROJ, 'emote')
PKG = os.path.join(W, 'BareDances_v1.0.unitypackage')
g.unitypackage(PROJ, [DANCE], PKG)
ZIP = os.path.join(W, 'BareDances_v1.0.zip')
with zipfile.ZipFile(ZIP, 'w') as zf:
    zf.write(PKG, 'BareDances_v1.0/BareDances_v1.0.unitypackage')
    zf.writestr('BareDances_v1.0/readme.txt', 'Action layer に入れてください\n')
LOOSE = os.path.join(W, 'loose')
os.makedirs(LOOSE)
for name in CLIPS:  # as saved out of a project: no .meta
    shutil.copy(os.path.join(PROJ, DANCE, name + '.anim'), LOOSE)


def emotes(want, db=None):
    db = db or u.DB(tempfile.mkdtemp(dir=W))
    return [(n, loop, speed, params) for n, _, loop, speed, params in u.find_emotes(db, want)]


MOVING = [('Bare Dance', True, 1.0, set()), ('Wave Once', False, 1.0, set())]
check('a package: its clips that move, named as the clips, looping as they loop', emotes(PKG), MOVING)
check('a zip holding the package', emotes(ZIP), MOVING)
check('the project folder', emotes(os.path.join(PROJ, DANCE)), MOVING)
check('a loose clip that loops', emotes(os.path.join(LOOSE, 'Bare_Dance.anim')), MOVING[:1])
check('a still pose named on its own is kept (held, not looping)', emotes(os.path.join(LOOSE, 'Proxy_Stand.anim')),
      [('Proxy Stand', False, 1.0, set())])
fails('a clip with no muscle or body curves', lambda: emotes(os.path.join(LOOSE, 'Face_Only.anim')),
      'no humanoid animation clip')
check('... and why, in a warning', any('Face_Only.anim is not a humanoid clip' in w for w in u.WARNINGS), True)
fails('a file that is not a clip', lambda: emotes(os.path.join(PROJ, DANCE + '.meta')), 'not an animation clip')
fails('a name no clip has', lambda: emotes('Toothless'), 'no animation clip called "Toothless"')

db = u.DB(tempfile.mkdtemp(dir=W))
db.add_input(PKG)
check('by name, once the package is in', emotes('Proxy_Stand', db), [('Proxy Stand', False, 1.0, set())])
check('by name, ignoring case and marks', emotes('proxy stand', db), [('Proxy Stand', False, 1.0, set())])
first = u.find_emotes(db, PKG)
check('the package again, as when it is an --outfit too', [e[0] for e in first], ['Bare Dance', 'Wave Once'])
check('where each clip was read from', [e[1].src for e in first],
      [(g.guid_of('emote/Bare_Dance'), 7400000), (g.guid_of('emote/Wave_Once'), 7400000)])
check('a loose clip is its file', u.find_emotes(db, os.path.join(LOOSE, 'Wave_Once.anim'))[0][1].src,
      (os.path.realpath(os.path.join(LOOSE, 'Wave_Once.anim')), 7400000))
dz = u.DB(tempfile.mkdtemp(dir=W))
a = sorted(x.guid for x in u.input_assets(dz, ZIP))
b = sorted(x.guid for x in u.input_assets(dz, ZIP))
check('a zip read twice holds the same assets', (len(a), a == b), (len(CLIPS), True))

clip = first[0][1]
check('the dance moves', clip.moves, True)
check('its face curve', sorted(clip.faces), ['あ'])
check('its length', round(clip.length, 3), 1.0)
check('the pose does not move', u.find_emotes(db, 'Proxy_Stand')[0][1].moves, False)

shutil.rmtree(W, ignore_errors=True)
print('%d failed: %s' % (len(FAILS), ', '.join(FAILS)) if FAILS else 'all passed')
sys.exit(1 if FAILS else 0)
