# mat_unit.py: tools/unity2hyprwalk.py's material reader on hand-made UnlitWF, lilToon, Poiyomi and MToon materials: alpha
# sources, faces drawn, emission, stencils and render queues, outlines, back faces, light clamp, toon shading and
# matcaps; then a GLB exported and read back (masks and inverted alpha baked into the base texture's alpha, the
# material extras and their textures).
#   blender -b --factory-startup --python-exit-code 1 -P mat_unit.py
import sys, os, tempfile
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))  # tools/
import unity2hyprwalk as u

FAILS = []


def check(what, got, want):
    ok = got == want
    print('%s %s: %r%s' % ('ok  ' if ok else 'FAIL', what, got, '' if ok else ' (want %r)' % (want,)))
    if not ok:
        FAILS.append(what)


class Asset:
    def __init__(self, guid, path, file):
        self.guid, self.path, self.file = guid, path, file
        self.name, self.ext = os.path.splitext(os.path.basename(path))


class DB:
    def __init__(self):
        self.assets = {}

    def get(self, g):
        return self.assets.get(g)

    def importer(self, g):
        return {}


TMP = tempfile.mkdtemp(prefix='mat_unit')
db = DB()


def asset(path, text=None):
    guid = '%032x' % (len(db.assets) + 1)
    f = os.path.join(TMP, guid + os.path.splitext(path)[1])
    if text is not None:
        with open(f, 'w', encoding='utf-8') as fh:
            fh.write(text)
    db.assets[guid] = Asset(guid, path, f)
    return {'fileID': '4800000' if path.endswith('.shader') else '2800000', 'guid': guid}


def shader(name, props, passes):
    return asset('Assets/Unlit_WF_ShaderSuite/Shaders/%s.shader' % name.split('/')[-1], (
        'Shader "%s" {\n    Properties {\n%s    }\n    SubShader {\n%s    }\n}\n' % (
            name, ''.join('        %s\n' % p for p in props),
            ''.join('        Pass {\n            Name "%s"\n            Cull %s\n        }\n' % p for p in passes))))


MASK = shader('UnlitWF/WF_UnToon_Transparent_Mask', ['_AL_Source ("[AL] Alpha Source", Float) = 0'],
              [('MAIN_BACK', 'FRONT'), ('MAIN_FRONT', 'BACK')])
TRANS = shader('UnlitWF/WF_UnToon_Transparent', [], [('MAIN_BACK', 'FRONT'), ('MAIN_FRONT', 'BACK')])
CUT = shader('UnlitWF/WF_UnToon_TransCutout', ['_CullMode ("Cull Mode", int) = 0'], [('MAIN', '[_CullMode]')])
OPAQUE = shader('UnlitWF/UnToon_Outline/WF_UnToon_Outline_Opaque', ['_CullMode ("Cull Mode", int) = 2'],
                [('OUTLINE', 'FRONT'), ('MAIN', '[_CullMode]')])
NOFILE = asset('Assets/Unlit_WF_ShaderSuite/Shaders/Unlit_WF_UnToon_Transparent.shader')  # not readable
TEX, MASKTEX = asset('Assets/Tex/body.png'), asset('Assets/Tex/mask.png')
mats = u.Materials(db)


def mat(sh, fl=None, col=None, tex=None, rq=-1, tags=None):
    t = {'_MainTex': (TEX, (1.0, 1.0), (0.0, 0.0))}
    t.update(tex or {})
    c = {'_Color': (1.0, 1.0, 1.0, 1.0)}
    c.update(col or {})
    return mats.build('m', sh, set(), rq, tags or {}, t, dict(fl or {}), c)


def alpha(m):
    return (m.mode, m.alpha if not m.alpha else (os.path.basename(db.get(MASKTEX['guid']).path) if m.alpha[0] ==
                                                  db.get(MASKTEX['guid']).file else m.alpha[0], m.alpha[1]),
            round(m.color[3], 4))


print('== alpha: an empty mask is white, so opaque; a mask\'s channel; the main texture\'s alpha times the power')
m = mat(MASK, {'_AL_Source': 1, '_AL_Power': 1}, {'_Color': (1, 1, 1, 0.5)})
check('an eye on the stencil mask shader, its mask empty: opaque (and both faces)', (alpha(m), m.double),
      (('OPAQUE', None, 1.0), True))
m = mat(MASK, {'_AL_Source': 2}, {'_Color': (1, 1, 1, 0.5)}, {'_AL_MaskTex': (MASKTEX, (1, 1), (0, 0))})
check('a mask\'s alpha, the colour\'s playing no part', alpha(m), ('BLEND', ('mask.png', 'A'), 1.0))
m = mat(MASK, {'_AL_Source': 1, '_AL_Power': 0.5})
check('an empty mask at half power: half see-through, not by the texture', alpha(m), ('BLEND', '', 0.5))
m = mat(TRANS, {'_AL_Source': 0, '_AL_Power': 0.5}, {'_Color': (1, 1, 1, 0.8)})
check('the main texture\'s alpha: the colour\'s times the power', alpha(m), ('BLEND', None, 0.4))
m = mat(CUT, {'_AL_Source': 1, '_Cutoff': 0.3})
check('cutout with an empty mask: nothing cut', alpha(m), ('OPAQUE', None, 1.0))
m = mat(CUT, {'_AL_Source': 0, '_Cutoff': 0.3, '_AL_Power': 0.5})
check('cutout by the texture: the power plays no part', (alpha(m), m.cutoff), (('MASK', None, 1.0), 0.3))

print('== faces: the transparent shaders draw both, the others cull by _CullMode or its default')
check('transparent: both faces', mat(TRANS).double, True)
check('cutout, no _CullMode: its default, both', mat(CUT).double, True)
check('cutout, _CullMode 2: the front', mat(CUT, {'_CullMode': 2}).double, False)
check('opaque, no _CullMode: its default, the front', mat(OPAQUE).double, False)
check('opaque, _CullMode 0: both', mat(OPAQUE, {'_CullMode': 0}).double, True)
check('a shader file not read: by its file name', (mat(NOFILE).mode, mat(NOFILE).double), ('BLEND', True))
m = mat({'fileID': '4800000', 'guid': 'f' * 32}, {'_GL_LevelMin': 0.1, '_AL_Source': 1}, tags={'RenderType': 'Transparent'})
check('no shader: UnlitWF by its properties, see-through taken for its transparent shaders', (m.mode, m.double),
      ('OPAQUE', True))
m = mat({'fileID': '4800000', 'guid': 'f' * 32}, {'_GL_LevelMin': 0.1, '_AL_Source': 0}, tags={'RenderType': 'Transparent'})
check('... and with the texture\'s alpha', (m.mode, m.double), ('BLEND', True))

print('== emission: constant at 1 + offset, a wave at its middle; off unless enabled')
col = {'_EmissionColor': (1.0, 0.5, 0.0, 1.0)}
lin = u.to_linear(0.5)
m = mat(OPAQUE, {'_ES_Enable': 1, '_ES_Shape': 3, '_ES_LevelOffset': -0.25}, col)
check('constant, offset -0.25', tuple(round(x, 4) for x in m.emit), (0.75, round(lin * 0.75, 4), 0.0))
m = mat(OPAQUE, {'_ES_Enable': 1, '_ES_Shape': 0, '_ES_LevelOffset': -0.25}, col)
check('a wave: its middle, offset', tuple(round(x, 4) for x in m.emit), (0.25, round(lin * 0.25, 4), 0.0))
check('off', mat(OPAQUE, {'_ES_Enable': 0}, col).emit, (0.0, 0.0, 0.0))

print('== stencil masks: from the shaders\' Stencil blocks (outline passes aside), or UnlitWF\'s by GUID')


def stencil_shader(name, blocks, extra=''):
    """a shader whose passes have these Stencil blocks: [(pass name, Stencil body lines)]"""
    return asset('Assets/Shaders/%s.shader' % name.split('/')[-1], (
        'Shader "%s" {\n    Properties {\n        _StencilMaskID ("ID", int) = 8\n'
        '        _AL_StencilPower ("Alpha Power", Range(0, 1)) = 0.5\n    }\n    SubShader {\n'
        '        Tags { "Queue" = "Transparent+1" }\n%s    }\n}\n%s' % (name, ''.join(
            '        Pass {\n            Name "%s"\n            Stencil {\n%s            }\n        }\n' % (
                n, ''.join('                %s\n' % x for x in body)) for n, body in blocks), extra)))


WRITE = ['Ref [_StencilMaskID]', 'WriteMask [_StencilMaskID]', 'Comp ALWAYS', 'Pass replace']
NOTEQ = ['Ref [_StencilMaskID]', 'ReadMask 15', 'Comp notEqual']
EQ = ['Ref [_StencilMaskID]', 'ReadMask 15', 'Comp equal']
SMASK = stencil_shader('UnlitWF/WF_UnToon_Transparent_Mask', [('MAIN_BACK', WRITE), ('MAIN_FRONT', WRITE)])
SOUT = stencil_shader('UnlitWF/UnToon_Outline/WF_UnToon_Outline_Transparent_MaskOut',
                      [('OUTLINE', NOTEQ), ('MAIN_BACK', NOTEQ), ('MAIN_FRONT', NOTEQ)])
SBLEND = stencil_shader('UnlitWF/WF_UnToon_Transparent_MaskOut_Blend',
                        [('MAIN_BACK', NOTEQ), ('MAIN_FRONT', NOTEQ), ('MAIN_BACK', EQ), ('MAIN_FRONT', EQ)],
                        '// #define _AL_CustomValue _AL_StencilPower\n')
m = mat(SMASK, {'_StencilMaskID': 10, '_AL_Source': 1})
check('Mask: writes its ID (the write mask its ID too) where it draws', m.stencil,
      {'ref': 10, 'read': 255, 'write': 10, 'comp': 'always', 'pass': 'replace', 'fail': 'keep', 'zfail': 'keep'})
check('... and its queue, the shader\'s with none of its own', mat(SMASK).queue, 3001)
m = mat(SOUT, {'_StencilMaskID': 10}, rq=2449)
check('MaskOut: draws where the low four bits are not its ID', (m.stencil, m.queue), (
    {'ref': 10, 'read': 15, 'write': 255, 'comp': 'notequal', 'pass': 'keep', 'fail': 'keep', 'zfail': 'keep'},
    2449))
m = mat(SBLEND, {'_StencilMaskID': 3})
check('MaskOut_Blend: and again where it is, at _AL_StencilPower (its default)', m.stencil.get('again'),
      {'comp': 'equal', 'alpha': 0.5})
check('... at the material\'s', mat(SBLEND, {'_AL_StencilPower': 0.3}).stencil['again'], {'comp': 'equal', 'alpha': 0.3})
WF_GUID = {'fileID': '4800000', 'guid': '9350854c6e88f3f4eb873d2f94ff3328'}  # Outline_Transparent_MaskOut
m = mat(WF_GUID, {'_StencilMaskID': 10, '_GL_LevelMin': 0.1})
check('UnlitWF\'s MaskOut by its GUID, not in the input', (m.stencil['comp'], m.stencil['ref'], m.stencil['read'],
                                                           m.queue), ('notequal', 10, 15, 3001))
m = mat({'fileID': '4800000', 'guid': '2efe527cfcbf0e1408b67463225f552f'}, {'_GL_LevelMin': 0.1})
check('... Mask by its GUID, the ID the shaders\' default', (m.stencil['comp'], m.stencil['ref'], m.stencil['write']),
      ('always', 8, 8))
LIL = {'fileID': '4800000', 'guid': 'df12117ecd77c31469c224178886498e'}
m = mat(LIL, {'_StencilRef': 1, '_StencilComp': 8, '_StencilPass': 2, '_TransparentMode': 0})
check('lilToon\'s stencil properties: a writer', (m.stencil['comp'], m.stencil['pass'], m.stencil['ref']),
      ('always', 'replace', 1))
m = mat(LIL, {'_StencilRef': 1, '_StencilComp': 6, '_StencilPass': 0, '_TransparentMode': 0})
check('... a reader', (m.stencil['comp'], m.stencil['pass']), ('notequal', 'keep'))
check('... one that does nothing', mat(LIL, {'_StencilRef': 0, '_StencilComp': 8, '_StencilPass': 0}).stencil, None)
check('Poiyomi\'s', mat({'fileID': '4800000', 'guid': 'e' * 32}, {'_StencilRef': 2, '_StencilCompareFunction': 3,
                                                                 '_StencilPassOp': 0}).stencil['comp'], 'equal')

print('== outlines: UnlitWF\'s, lilToon\'s, Poiyomi\'s, MToon\'s')
OUTMASK = asset('Assets/Tex/outline mask.png')
col = {'_TL_LineColor': (0.5, 0.5, 0.5, 1.0)}
m = mats.build('m', OPAQUE, {'_TL_ENABLE'}, -1, {}, {'_MainTex': (TEX, (1, 1), (0, 0)),
                                                      '_TL_MaskTex': (OUTMASK, (1, 1), (0, 0))},
               {'_TL_LineWidth': 0.4, '_TL_BlendBase': 0.1, '_TL_LineType': 1, '_TL_InvMaskVal': 1}, dict(col))
lin5 = round(u.to_linear(0.5), 5)
check('UnlitWF: world metres, EDGE lines pushed back ten widths, the mask inverted', (
    m.outline['width'], m.outline['space'], m.outline['shift'], m.outline['color'], m.outline['base'],
    os.path.basename(db.get(OUTMASK['guid']).path) if m.outline['mask'][0] == db.get(OUTMASK['guid']).file else '?',
    m.outline['mask'][1:]), (0.004, 'world', -0.04, (lin5, lin5, lin5, 1.0), 0.1, 'outline mask.png', ('R', True)))
check('... no colour texture unless _TL_BlendCustom', m.outline['tex'], None)
OUTCOL = asset('Assets/Tex/outline colour.png')
m = mats.build('m', OPAQUE, {'_TL_ENABLE'}, -1, {}, {'_MainTex': (TEX, (2, 1), (0.5, 0)),
                                                      '_TL_CustomColorTex': (OUTCOL, (3, 3), (0, 0))},
               {'_TL_LineWidth': 0.4, '_TL_BlendCustom': 0.25}, dict(col))
check('... its custom colour texture, mixed in a quarter, on the main texture\'s uv', (
    m.outline['tex'][0] == db.get(OUTCOL['guid']).file, m.outline['tex'][1:]), (True, (((2, 1), (0.5, 0)), 0.25)))
m = mats.build('m', OPAQUE, {'_TL_ENABLE'}, -1, {}, {'_MainTex': (TEX, (1, 1), (0, 0))},
               {'_TL_LineWidth': 0.4, '_TL_BlendCustom': 0.5}, dict(col))
check('... without one, towards its white', (m.outline['tex'], m.outline['color']),
      (None, tuple(round(lin5 + (1 - lin5) * 0.5, 5) for _ in range(3)) + (1.0,)))
check('... not without _TL_ENABLE', mats.build('m', OPAQUE, {'_ES_ENABLE'}, -1, {}, {}, {'_TL_LineWidth': 0.4},
                                              {}).outline, None)
check('... not on a shader without an outline pass', mats.build('m', TRANS, {'_TL_ENABLE'}, -1, {}, {},
                                                                {'_TL_LineWidth': 0.4}, {}).outline, None)
LILO = {'fileID': '4800000', 'guid': 'efa77a80ca0344749b4f19fdd5891cbe'}  # Hidden/lilToonOutline
m = mat(LILO, {'_OutlineWidth': 0.1, '_OutlineFixWidth': 0.25, '_OutlineZBias': 0.002, '_TransparentMode': 0},
        {'_OutlineColor': (0, 0, 0, 1)})
check('lilToon\'s outline shader: object space, thinner up close', (m.outline['width'], m.outline['space'],
                                                                     m.outline['fix'], m.outline['shift'],
                                                                     m.outline['lit']),
      (0.001, 'object', [0.25, 1.0], -0.002, 1.0))
m = mat(LILO, {'_OutlineWidth': 0.1, '_TransparentMode': 0}, {'_OutlineColor': (0.5, 0.5, 0.5, 1)},
        {'_OutlineTex': (TEX, (2, 2), (0, 0))})
check('... its _OutlineTex, which the colour multiplies, on its own tiling', (
    m.outline['tex'][0] == db.get(TEX['guid']).file, m.outline['tex'][1:]), (True, (((2, 2), (0, 0)), None)))
check('... not lilToon without one', mat(LIL, {'_OutlineWidth': 0.1, '_TransparentMode': 0}).outline, None)
POI = shader('.poiyomi/Poiyomi Toon', [], [('Base', 'Back')])
m = mat(POI, {'_EnableOutlines': 1, '_LineWidth': 0.5, '_OutlineSpace': 1, '_OutlineMaskChannel': 3,
              '_OutlineFixedSize': 0}, {'_LineColor': (1, 0, 0, 1)}, {'_OutlineMask': (OUTMASK, (1, 1), (0, 0))})
check('Poiyomi: its size in cm, world space, the mask\'s alpha', (m.outline['width'], m.outline['space'],
                                                                   m.outline['mask'][1], m.outline['fix']),
      (0.005, 'world', 'A', None))
m = mat(POI, {'_EnableOutlines': 1, '_LineWidth': 0.5}, {}, {'_OutlineTexture': (OUTCOL, (1, 1), (0, 0.5))})
check('... its _OutlineTexture, times the colour', (m.outline['tex'][0] == db.get(OUTCOL['guid']).file,
                                                   m.outline['tex'][1:]), (True, (((1, 1), (0, 0.5)), None)))
m = mat(POI, {'_EnableOutlines': 1, '_LineWidth': 0.5, '_OutlineTextureUV': 1}, {}, {'_OutlineTexture': (OUTCOL, (1, 1), (0, 0))})
check('... not on another uv set', m.outline['tex'], None)
check('... off', mat(POI, {'_EnableOutlines': 0, '_LineWidth': 0.5}).outline, None)
MTOON = shader('VRM/MToon', [], [('FORWARD', 'Back')])
m = mat(MTOON, {'_OutlineWidthMode': 1, '_OutlineWidth': 0.4, '_OutlineColorMode': 0, '_BlendMode': 0})
check('MToon: world width in cm, its colour fixed (not lit)', (m.outline['width'], m.outline['space'],
                                                                m.outline['lit']), (0.004, 'world', 0.0))
m = mat(MTOON, {'_OutlineWidthMode': 2, '_OutlineWidth': 0.4, '_OutlineColorMode': 1, '_OutlineLightingMix': 0.5,
                '_OutlineScaledMaxDistance': 2, '_BlendMode': 0})
check('... screen width, lit halfway', (m.outline['space'], m.outline['lit'], m.outline['max']), ('screen', 0.5, 2.0))
check('... none', mat(MTOON, {'_OutlineWidthMode': 0, '_OutlineWidth': 0.4, '_BlendMode': 0}).outline, None)

print('== UnlitWF\'s back faces and light clamp')
BACK = asset('Assets/Tex/back.png')
m = mats.build('m', OPAQUE, {'_BK_ENABLE'}, -1, {}, {'_MainTex': (TEX, (1, 1), (0, 0)),
                                                      '_BK_BackTex': (TEX, (2, 1), (0, 0))},
               {'_GL_LevelMin': 0.1, '_GL_LevelMax': 0.9, '_GL_BlendPower': 0.6},
               {'_BK_BackColor': (0.5, 0.5, 0.5, 1)})
check('the main texture on the back, tinted', (m.back['tex'] == m.tex, m.back['color'], m.back['xf']),
      (True, (lin5, lin5, lin5, 1.0), ((2, 1), (0, 0))))
check('... its light between 0.1 (gamma: 0.01 linear) and 0.9, the colour 60% saturated', m.light,
      (round(u.to_linear(0.1), 4), 0.9, 0.6))
m = mats.build('m', OPAQUE, set(), -1, {}, {'_BK_BackTex': (BACK, (1, 1), (0, 0))},
               {'_BK_Enable': 1, '_GL_LevelMin': 0.2, '_GL_LevelTweak': 0.5}, {})
check('a texture of its own (a legacy material: no keywords, the toggle); the minimum tweaked halfway to 1',
      (m.back['tex'] == db.get(BACK['guid']).file, m.light[0]), (True, round(u.to_linear(0.6), 4)))
check('none without the keyword', mats.build('m', OPAQUE, {'_TL_ENABLE'}, -1, {}, {}, {'_BK_Enable': 1}, {}).back,
      None)
check('the light clamp: UnlitWF only', mat(MTOON, {'_BlendMode': 0}).light, None)

print('== toon shading and matcaps')
CAP = asset('Assets/Tex/matcap.png')
lin = lambda *c: [round(u.to_linear(x), 5) for x in c]
gamma = lambda c: max(1.055 * c ** (1 / 2.4) - 0.055, 0.0)


def capfile(m):
    return m.matcap and (m.matcap['tex'] == db.get(CAP['guid']).file, m.matcap['color'], m.matcap['mode'], m.matcap['lit'])


m = mats.build('m', OPAQUE, {'_TS_ENABLE'}, -1, {}, {'_MainTex': (TEX, (1, 1), (0, 0))}, {}, {})
w = [round(1 + 0.75 * (f - 1), 4) for f in lin(0.81, 0.81, 0.9)]
check('UnlitWF\'s toon shade: 1st / base, 3/4 of the way (its contrast adjusted), half-Lambert 0.4..0.45',
      (m.toon['shade'], m.toon['base'], m.toon['lo'], m.toon['hi'], m.toon['strength']), (w, True, -0.2, -0.1, 1.0))
m = mats.build('m', OPAQUE, {'_TS_ENABLE', '_TS_FIXC_ENABLE'}, -1, {}, {}, {'_TS_Power': 2, '_TS_1stFeather': 0},
               {'_TS_1stColor': (0.5, 0.5, 0.5, 1), '_TS_BaseColor': (1, 1, 1, 1)})
check('... its contrast fixed and its power 2: black; no feather: a step', (m.toon['shade'], m.toon['hi'] - m.toon['lo'] < 0.003),
      ([0.0, 0.0, 0.0], True))
m = mats.build('m', OPAQUE, {'_TL_ENABLE'}, -1, {}, {}, {'_TS_Enable': 1}, {})
check('... without the keyword, lit the same all round (UnlitWF\'s light has no N·L)', (m.toon['lo'], m.toon['hi'], m.toon['base']),
      (-1.0, -1.0, True))
m = mats.build('m', OPAQUE, {'_HL_ENABLE'}, -1, {}, {'_HL_MatcapTex': (CAP, (1, 1), (0, 0))}, {'_HL_CapType': 1}, {})
check('UnlitWF\'s light cap: added, its grey tint doubled and gamma encoded', capfile(m),
      (True, [round(gamma(2 * lin(0.5)[0]), 4)] * 3 + [1.0], 'add', 1.0))
m = mats.build('m', OPAQUE, {'_HL_ENABLE'}, -1, {}, {'_HL_MatcapTex': (CAP, (1, 1), (0, 0))}, {'_HL_Power': 0.5}, {})
check('... its median cap: the tint as it is', capfile(m), (True, [round(2 * lin(0.5)[0], 4)] * 3 + [0.5], 'median', 1.0))
m = mat(LIL, {'_TransparentMode': 0, '_UseShadow': 1, '_ShadowStrength': 0.5, '_UseMatCap': 1, '_MatCapBlend': 0.8,
              '_MatCapBlendMode': 3, '_MatCapEnableLighting': 0.25}, {'_MatCapColor': (1, 1, 1, 0.5)},
        {'_MatCapTex': (CAP, (1, 1), (0, 0))})
check('lilToon\'s shadow: the base times _ShadowColor, border 0.5 blur 0.1 (N·L -0.1..0.1), half strength',
      (m.toon['shade'], m.toon['base'], m.toon['tex'], m.toon['lo'], m.toon['hi'], m.toon['strength']),
      (lin(0.82, 0.76, 0.85), True, None, -0.1, 0.1, 0.5))
check('... its matcap multiplied, 0.8 of its 0.5, a quarter lit', capfile(m), (True, [1.0, 1.0, 1.0, 0.4], 'multiply', 0.25))
m = mat(LIL, {'_TransparentMode': 0, '_UseShadow': 1, '_ShadowBorder': 0.1, '_ShadowBlur': 0.4},
        tex={'_ShadowColorTex': (MASKTEX, (1, 1), (0, 0))})
check('... its shadow colour texture in place of the base; the step clamped at half-Lambert 0',
      (m.toon['base'], m.toon['tex'] == db.get(MASKTEX['guid']).file, m.toon['lo'], m.toon['hi']), (False, True, -1.0, -0.4))
check('... no shadow, no matcap: none', (mat(LIL, {'_TransparentMode': 0}).toon, mat(LIL, {'_TransparentMode': 0}).matcap),
      (None, None))
m = mat(POI, {})
check('Poiyomi\'s Flat (its default): lit all round', (m.toon['lo'], m.toon['hi'], m.toon['shade'], m.toon['base']),
      (-1.0, -1.0, [1.0, 1.0, 1.0], True))
m = mat(POI, {'_LightingMode': 1, '_ShadowBorder': 0.6, '_ShadowBlur': 0.2, '_MatcapEnable': 1, '_MatcapIntensity': 2},
        {'_ShadowColor': (0.5, 0.5, 0.5, 1)}, {'_Matcap': (CAP, (1, 1), (0, 0))})
check('... Multilayer Math: lilToon\'s (N·L 0..0.4)', (m.toon['shade'], m.toon['lo'], m.toon['hi']), (lin(0.5, 0.5, 0.5), 0.0, 0.4))
check('... its matcap in place of the colour (Replace), twice', capfile(m), (True, [1.0, 1.0, 1.0, 2.0], 'mix', 1.0))
check('... another lighting type: as it is', mat(POI, {'_LightingMode': 6}).toon, None)
m = mat(POI, {'_MatcapEnable': 1, '_MatcapReplace': 0, '_MatcapAdd': 0.5}, tex={'_Matcap': (CAP, (1, 1), (0, 0))})
check('... its matcap added, half', capfile(m), (True, [1.0, 1.0, 1.0, 0.5], 'add', 1.0))
m = mat(MTOON, {'_BlendMode': 0}, tex={'_ShadeTexture': (TEX, (1, 1), (0, 0)), '_SphereAdd': (CAP, (1, 1), (0, 0))})
check('MToon: its shade colour (default) times _ShadeTexture, N·L from _ShadeShift 0 to 0.1 (toony 0.9)',
      (m.toon['shade'], m.toon['base'], m.toon['tex'] == m.tex, m.toon['lo'], m.toon['hi']),
      (lin(0.97, 0.81, 0.86), False, True, 0.0, 0.1))
check('... _SphereAdd added as it is', capfile(m), (True, [1.0, 1.0, 1.0, 1.0], 'add', 0.0))
m = mat(MTOON, {'_BlendMode': 0, '_ShadingToonyFactor': 0.8, '_ShadingShiftFactor': -0.1, '_RimLightingMix': 0.5},
        {'_ShadeColor': (0.5, 0.25, 0.25, 1)}, {'_MatcapTex': (CAP, (1, 1), (0, 0))})
check('MToon10: N·L -1 + toony - shift .. 1 - toony - shift', (m.toon['shade'], m.toon['lo'], m.toon['hi']),
      (lin(0.5, 0.25, 0.25), -0.1, 0.3))
check('... its matcap, lit halfway', capfile(m), (True, [1.0, 1.0, 1.0, 1.0], 'add', 0.5))
check('none on the Standard shader', mat(shader('Standard', [], [('FORWARD', 'Back')]), {}).toon, None)

print('== inverted alpha: baked into the texture\'s (1 - the alpha, times the colour\'s for the main texture\'s)')
m = mat(TRANS, {'_AL_Source': 0, '_AL_InvMaskVal': 1, '_AL_Power': 0.5}, {'_Color': (1, 1, 1, 0.8)})
check('the main texture\'s', (m.mode, m.invert, round(m.color[3], 4)), ('BLEND', 0.8, 0.5))
m = mat(MASK, {'_AL_Source': 1, '_AL_InvMaskVal': 1}, tex={'_AL_MaskTex': (MASKTEX, (1, 1), (0, 0))})
check('a mask\'s red', (alpha(m), m.invert), (('BLEND', ('mask.png', 'R'), 1.0), 1.0))
m = mat(MASK, {'_AL_Source': 1, '_AL_InvMaskVal': 1})
check('an empty mask inverted: nothing shows', alpha(m), ('BLEND', '', 0.0))

print('== the GLB: the alpha baked into the base texture, and the extras\' textures added, read back')
import bpy
import numpy as np
from types import SimpleNamespace


def image_file(path, w, h, fill):
    """a PNG made in Blender: fill(x, y) -> (r, g, b, a) in 0..1"""
    im = bpy.data.images.new(os.path.basename(path), w, h, alpha=True)
    px = np.array([fill(x, y) for y in range(h) for x in range(w)], np.float32)
    im.pixels.foreach_set(px.ravel())
    im.filepath_raw = path
    im.file_format = 'PNG'
    im.save()
    bpy.data.images.remove(im)


def put(path, w, h, fill):
    """an image asset of the test's database, as a texture reference"""
    r = asset(path)
    image_file(db.get(r['guid']).file, w, h, fill)
    return r


MAIN = put('Assets/Tex/hair.png', 8, 8, lambda x, y: (x / 7, y / 7, 0.5, 0.25 + 0.5 * (x / 7)))
AMASK = put('Assets/Tex/alpha mask.png', 8, 8, lambda x, y: (1.0 if x < 4 else 0.0, 0.0, 0.0, 1.0))
SMALL = put('Assets/Tex/small mask.png', 4, 4, lambda x, y: (0.0, 0.0, 0.0, 0.6))
LMASK = put('Assets/Tex/line mask.png', 4, 4, lambda x, y: (1.0 if y < 2 else 0.0, 0.0, 0.0, 1.0))
for r in (MAIN, AMASK, SMALL, LMASK):
    db.importer = lambda g: {}
bd = object.__new__(u.Build)
bd.mats, bd.opts, bd._images, bd._made, bd.post = mats, SimpleNamespace(max_texture=0), {}, {}, {}
T = lambda r, s=(1, 1): (r, s, (0, 0))
cases = {
    'red mask': mats.build('red mask', MASK, set(), -1, {}, {'_MainTex': T(MAIN), '_AL_MaskTex': T(AMASK)},
                           {'_AL_Source': 1}, {'_Color': (1, 1, 1, 1)}),
    'small alpha mask': mats.build('small alpha mask', TRANS, set(), -1, {}, {'_MainTex': T(MAIN),
                                                                               '_AL_MaskTex': T(SMALL)},
                                   {'_AL_Source': 2}, {'_Color': (1, 1, 1, 1)}),
    'inverted': mats.build('inverted', TRANS, set(), -1, {}, {'_MainTex': T(MAIN)},
                           {'_AL_Source': 0, '_AL_InvMaskVal': 1}, {'_Color': (1, 1, 1, 0.5)}),
    'toon': mats.build('toon', LIL, set(), -1, {}, {'_MainTex': T(MAIN), '_ShadowColorTex': T(MAIN), '_MatCapTex': T(LMASK)},
                       {'_TransparentMode': 0, '_UseShadow': 1, '_UseMatCap': 1}, {'_Color': (1, 1, 1, 1)}),
    'outlined': mats.build('outlined', OPAQUE, {'_TL_ENABLE', '_BK_ENABLE'}, 2449, {},
                           {'_MainTex': T(MAIN), '_TL_MaskTex': T(LMASK), '_BK_BackTex': T(MAIN, (2, 1)),
                            '_TL_CustomColorTex': T(MAIN)},
                           {'_TL_LineWidth': 0.2, '_GL_LevelMin': 0.1, '_TL_BlendCustom': 0.3}, {'_BK_BackColor': (1, 0, 0, 1)}),
}
for bpy_coll in (bpy.data.objects, bpy.data.meshes):
    for x in list(bpy_coll):
        bpy_coll.remove(x)
for k, (name, mi) in enumerate(cases.items()):
    me = bpy.data.meshes.new(name)
    me.from_pydata([(k * 2, 0, 0), (k * 2 + 1, 0, 0), (k * 2 + 1, 1, 0), (k * 2, 1, 0)], [], [(0, 1, 2, 3)])
    uv = me.uv_layers.new()
    for i, c in enumerate(((0, 0), (1, 0), (1, 1), (0, 1))):
        uv.data[i].uv = c
    ob = bpy.data.objects.new(name, me)
    bpy.context.scene.collection.objects.link(ob)
    ob.data.materials.append(bd.blender_for(mi))
glb = os.path.join(TMP, 'mat_unit.glb')
u.Build.export(bd, glb)
js, binc = u.read_glb(glb)
u.patch_materials(js, bd.post)
binc = u.material_extras(js, binc, bd)
u.write_glb(glb, js, binc)
js, binc = u.read_glb(glb)
mats_js = {x['name']: x for x in js['materials']}


def texels(ti):
    """the RGBA of a GLB texture, rows from the top"""
    im = js['images'][js['textures'][ti]['source']]
    bv = js['bufferViews'][im['bufferView']]
    f = os.path.join(TMP, 'tex%d.png' % ti)
    with open(f, 'wb') as fh:
        fh.write(binc[bv.get('byteOffset', 0):bv.get('byteOffset', 0) + bv['byteLength']])
    b = bpy.data.images.load(f)
    px = np.empty(b.size[0] * b.size[1] * b.channels, np.float32)
    b.pixels.foreach_get(px)
    out = px.reshape(b.size[1], b.size[0], b.channels)[::-1]
    bpy.data.images.remove(b)
    return out


def base_alpha(name):
    return texels(mats_js[name]['pbrMetallicRoughness']['baseColorTexture']['index'])[..., 3]


a = base_alpha('red mask')
check('the mask\'s red in the alpha (left half 1, right 0)', (round(float(a[3, 1]), 2), round(float(a[3, 6]), 2),
                                                             mats_js['red mask'].get('alphaMode')), (1.0, 0.0, 'BLEND'))
rgb = texels(mats_js['red mask']['pbrMetallicRoughness']['baseColorTexture']['index'])
check('... the main texture\'s colour kept (rows from the top: its y up)', (
    round(float(rgb[0, 7, 0]), 2), round(float(rgb[0, 0, 1]), 2), round(float(rgb[7, 0, 1]), 2)), (1.0, 1.0, 0.0))
a = base_alpha('small alpha mask')
check('a smaller mask\'s alpha, at the texture\'s size', (a.shape, round(float(a.mean()), 2)), ((8, 8), 0.6))
a = base_alpha('inverted')
want = [1 - 0.5 * (0.25 + 0.5 * x / 7) for x in (0, 7)]
check('inverted: 1 - the colour\'s alpha times the texture\'s (to 8 bits)',
      [abs(float(a[0, x]) - w) < 2.5 / 255 for x, w in zip((0, 7), want)], [True, True])
check('... the colour\'s alpha taken in', mats_js['inverted']['pbrMetallicRoughness'].get('baseColorFactor', [1] * 4)[3],
      1.0)
ex = mats_js['outlined'].get('extras', {})
check('extras: queue, outline, back, light, toon (flat)', sorted(ex), ['hyprwalk_back', 'hyprwalk_light', 'hyprwalk_outline',
                                                                      'hyprwalk_queue', 'hyprwalk_toon'])
lm = texels(ex['hyprwalk_outline']['mask']['index'])
check('the outline mask added to the GLB, as it is (its lower rows set)', (
    lm.shape[:2], round(float(lm[0, 0, 0]), 2), round(float(lm[3, 0, 0]), 2)), ((4, 4), 0.0, 1.0))
check('the outline\'s colour texture: the main one\'s, a third of the way', (
    ex['hyprwalk_outline']['texture']['index'] == mats_js['outlined']['pbrMetallicRoughness']['baseColorTexture']['index'],
    ex['hyprwalk_outline']['texture'].get('blend'), ex['hyprwalk_outline']['texture'].get('transform')), (True, 0.3, None))
check('the back texture: the main one\'s, its tiling', (
    ex['hyprwalk_back']['texture']['index'] == mats_js['outlined']['pbrMetallicRoughness']['baseColorTexture']['index'],
    ex['hyprwalk_back']['texture'].get('transform'), ex['hyprwalk_back']['color']),
      (True, {'offset': [0, 0.0], 'scale': [2, 1]}, [1.0, 0.0, 0.0, 1.0]))
tn = mats_js['toon'].get('extras', {})
check('lilToon\'s toon extras: its shadow colour texture the base\'s (the same file), its matcap added',
      (sorted(tn), tn['hyprwalk_toon'].get('texture', {}).get('index') ==
       mats_js['toon']['pbrMetallicRoughness']['baseColorTexture']['index'], tn['hyprwalk_toon']['base'],
       tn['hyprwalk_matcap']['mode'], texels(tn['hyprwalk_matcap']['index']).shape[:2]),
      (['hyprwalk_matcap', 'hyprwalk_toon'], True, False, 'add', (4, 4)))
check('the others: UnlitWF\'s light clamp and flat light only', sorted((n, tuple(x.get('extras', {}))) for n, x in mats_js.items()
                                                                       if n not in ('outlined', 'toon')),
      [('inverted', ('hyprwalk_light', 'hyprwalk_toon')), ('red mask', ('hyprwalk_light', 'hyprwalk_toon')),
       ('small alpha mask', ('hyprwalk_light', 'hyprwalk_toon'))])

print('all passed' if not FAILS else '%d FAILED: %s' % (len(FAILS), ', '.join(FAILS)))
sys.exit(1 if FAILS else 0)
