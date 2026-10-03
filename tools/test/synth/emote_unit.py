# emote_unit.py: tools/unity2hypr3d.py's --emote on small hand-made clips: a motion sold as bare humanoid clips, with
# no prefab, controller or menu, for the buyer to put in their Action layer. The same package is read as a
# .unitypackage (twice, as when it is an --outfit too), inside a Booth-style .zip (twice), as a folder, clip by clip
# as loose .anim files, and by a clip's name. A package's still poses are left out when it has clips that move, but
# named on their own they are kept; a clip with no muscle or body curves is not an emote; each clip knows where it was
# read from, so convert() adds none twice. The emotes are named as their clips with the words spaced out, a looping
# clip without its "Loop" unless it is one of a set. A clip's song is the sound file of its name beside it, or its
# folder's one when it is the folder's one clip, and it is copied for the settings file only when it is Ogg Vorbis.
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

# names as a menu shows them: a clip's words spaced out, and a looping clip's "Loop" left off unless that name is
# taken or the clip is one of a set
NAMED = os.path.join(W, 'named')
os.makedirs(os.path.join(NAMED, 'ProjectSettings'))
NAMING = {  # name: (loops, the emote's name)
    'FreddyFazbearPumpItUp_Loop': (True, 'Freddy Fazbear Pump It Up'),
    'FallBackward2': (False, 'Fall Backward 2'),
    'Thumbs up Entry': (False, 'Thumbs up Entry'),
    'Thumbs up Loop': (True, 'Thumbs up Loop'),
    'Spin': (True, 'Spin'),
    'Spin_Loop': (True, 'Spin Loop'),
    'Hold_Loop': (False, 'Hold Loop'),
    'INTERNET_YAMERO': (True, 'INTERNET YAMERO'),
    'pHM_Dance': (True, 'pHM Dance'),
}
for name, (loops, _) in NAMING.items():
    text = g.clip_yaml(name, CLIPS['Bare_Dance'][0])
    if loops:
        text = text.replace('m_LoopTime: 0', 'm_LoopTime: 1')
    g.write_asset(os.path.join(NAMED, DANCE, name + '.anim'), text, g.guid_of('named/' + name), 'NativeFormatImporter',
                  g.native(7400000))
g.folder_metas(NAMED, 'named')
got = {n: loop for n, loop, *_ in emotes(os.path.join(NAMED, DANCE))}
for name, (loops, want) in NAMING.items():
    check('%s is called "%s"%s' % (name, want, ', looping' if loops else ''), (want in got, got.get(want)), (True, loops))
check('one emote a clip', len(got), len(NAMING))
check('a loose clip\'s Loop is left off too',
      emotes(os.path.join(NAMED, DANCE, 'FreddyFazbearPumpItUp_Loop.anim'))[0][0], 'Freddy Fazbear Pump It Up')
check('words spaced out', [u.spaced_words(n) for n in ('AloneRamp', 'SlowMoFlylBack', 'Dance2Loop', 'VRSuya Dance')],
      ['Alone Ramp', 'Slow Mo Flyl Back', 'Dance 2 Loop', 'VRSuya Dance'])

# songs: a bare clip's sound file beside it goes with it (the one of its name, else its folder's one when it is the
# folder's one clip), copied for the settings file's "sound" when it is Ogg Vorbis
check('clips without sound files beside them have no song', [e[1].sound for e in first], [None, None])
SONGS = os.path.join(W, 'songs')
os.makedirs(os.path.join(SONGS, 'ProjectSettings'))
VORBIS = b'OggS\x00\x02' + bytes(22) + b'\x01vorbis' + bytes(40)  # (an Ogg Vorbis file's start: all write_sound looks at)
WAV = b'RIFF\x24\x00\x00\x00WAVEfmt ' + bytes(40)
SONG_FILES = {  # under Assets/pHM: a clip that loops, or a sound file's bytes
    'Song Dance/SongDance_Loop.anim': True, 'Song Dance/SongDance_Loop.ogg': VORBIS,
    'Lone Dance/Lone.anim': True, 'Lone Dance/music.ogg': VORBIS,
    'Two Dances/A.anim': True, 'Two Dances/B.anim': True, 'Two Dances/music.ogg': VORBIS,
    'Wav Dance/WavDance.anim': True, 'Wav Dance/WavDance.wav': WAV,
}
for rel, data in SONG_FILES.items():
    p, key = os.path.join(SONGS, 'Assets/pHM', rel), 'songs/' + rel
    if data is True:
        text = g.clip_yaml(os.path.splitext(os.path.basename(rel))[0], CLIPS['Bare_Dance'][0])
        g.write_asset(p, text.replace('m_LoopTime: 0', 'm_LoopTime: 1'), g.guid_of(key), 'NativeFormatImporter',
                      g.native(7400000))
    else:
        g.write_asset(p, data, g.guid_of(key), 'AudioImporter')
g.folder_metas(SONGS, 'songs')
SPKG = os.path.join(W, 'SongDances.unitypackage')
g.unitypackage(SONGS, ['Assets/pHM'], SPKG)
SZIP = os.path.join(W, 'SongDances.zip')
with zipfile.ZipFile(SZIP, 'w') as zf:
    zf.write(SPKG, 'SongDances/SongDances.unitypackage')


def songs(want):
    return {n: c.sound and c.sound[0] for n, c, *_ in u.find_emotes(u.DB(tempfile.mkdtemp(dir=W)), want)}


SONGS_OF = {'Song Dance': 'Assets/pHM/Song Dance/SongDance_Loop.ogg', 'Lone': 'Assets/pHM/Lone Dance/music.ogg',
            'A': None, 'B': None, 'Wav Dance': 'Assets/pHM/Wav Dance/WavDance.wav'}
got = songs(SPKG)
check('a clip with a sound file of its name beside it: that is its song', got.get('Song Dance'), SONGS_OF['Song Dance'])
check('a folder\'s one clip: the folder\'s one sound file', got.get('Lone'), SONGS_OF['Lone'])
check('two clips, a sound file of neither\'s name: no song', (got.get('A'), got.get('B')), (None, None))
check('a WAV of its name is its song too (write_sound leaves it out)', got.get('Wav Dance'), SONGS_OF['Wav Dance'])
check('the same in a zip', songs(SZIP), SONGS_OF)
LOOSE_SONG = os.path.join(W, 'loose_song')
os.makedirs(LOOSE_SONG)
for f in ('SongDance_Loop.anim', 'SongDance_Loop.ogg'):
    shutil.copy(os.path.join(SONGS, 'Assets/pHM/Song Dance', f), LOOSE_SONG)
check('a loose clip: the sound file of its name beside it', songs(os.path.join(LOOSE_SONG, 'SongDance_Loop.anim')),
      {'Song Dance': 'SongDance_Loop.ogg'})
found = {n: c for n, c, *_ in u.find_emotes(u.DB(tempfile.mkdtemp(dir=W)), SPKG)}
OUT = os.path.join(W, 'out')
os.makedirs(OUT)
check('an Ogg Vorbis song is copied for the settings file\'s "sound"',
      u.write_sound('Song Dance', found['Song Dance'].sound, os.path.join(OUT, 'Av.Song Dance.ogg')), 'Av.Song Dance.ogg')
check('... byte for byte', open(os.path.join(OUT, 'Av.Song Dance.ogg'), 'rb').read() == VORBIS, True)
check('a WAV is not (hypr3d plays Ogg Vorbis)',
      (u.write_sound('Wav Dance', found['Wav Dance'].sound, os.path.join(OUT, 'Av.Wav Dance.ogg')),
       os.path.exists(os.path.join(OUT, 'Av.Wav Dance.ogg'))), (None, False))
check('... and why, in a warning', any('WavDance.wav is not Ogg Vorbis' in w for w in u.WARNINGS), True)

shutil.rmtree(W, ignore_errors=True)
print('%d failed: %s' % (len(FAILS), ', '.join(FAILS)) if FAILS else 'all passed')
sys.exit(1 if FAILS else 0)
