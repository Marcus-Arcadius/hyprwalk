# builds tools/cands.json: what to process, with source/licence metadata (Commons metadata from meta/commons_info.json)
import json, re, html, os, urllib.parse
SP = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
info = {e['title']: e for e in json.load(open(f'{SP}/meta/commons_info.json'))}

def plain(s):
    s = re.sub(r'<[^>]+>', '', s or '')
    return re.sub(r'\s+', ' ', html.unescape(s)).strip()

TOFUGU_REPO = 'https://github.com/tofugu/japanese-vocabulary-pronunciation-audio'
SPK = {
    'hakatanoshio': dict(speaker='Hakatanoshio117117 (Wikimedia user)', sex='unknown', sex_source='not stated', age=None, native=True,
                         native_note='Commons/ja.wikipedia user with babel "ja" (native); set "Audio files of hiragana (set by Hakatanoshio117117)"'),
    'spacecat2': dict(speaker='Spacecat2 (Wikimedia user)', sex='unknown', sex_source='not stated', age=None, native=True,
                      native_note='file description: "pronounced by a native speaker of Japanese"'),
    'tofugu-f': dict(speaker='Tofugu/WaniKani female voice', sex='f', sex_source='Tofugu README and the Commons file names/descriptions ("by a native Japanese woman")', age='adult', native=True,
                     native_note='native Japanese, Kansai accent, amateur voice actor, amateur recording (Tofugu README)'),
    'tofugu-m': dict(speaker='Tofugu/WaniKani male voice', sex='m', sex_source='Tofugu README and the Commons file names/descriptions ("by a male native Japanese voice actor")', age='adult', native=True,
                     native_note='native Japanese, Tokyo accent, professional voice actor, professional recording (Tofugu README)'),
    'marsian': dict(speaker='marsian (Wikimedia user)', sex='unknown', sex_source='not stated', age=None, native=True,
                    native_note='file description: "recorded by: a native speaker of Japanese"; "I, marsian recorded my own voice"; babel ja'),
    'zsrtrgh': dict(speaker='Zsrtrgh (Lingua Libre speaker Q1564396)', sex='unknown', sex_source='Lingua Libre profile has no gender', age=None, native=True,
                    native_note='Lingua Libre profile: Japanese, native'),
    'aoishin': dict(speaker='葵心 (Lingua Libre speaker Q1392056)', sex='m', sex_source='Lingua Libre profile (P8 male) and file page speakerGender=male', age=None, native=True,
                    native_note='Lingua Libre profile: Japanese, native'),
    'firipinjin': dict(speaker='フィリピン人 (Lingua Libre speaker Q1575602)', sex='m', sex_source='Lingua Libre profile (P8 male) and file page speakerGender=male', age=None, native=False,
                       native_note='NOT native: Lingua Libre profile gives Japanese = beginner; native Tagalog and English'),
    'higa4': dict(speaker='Higa4 (Lingua Libre speaker Q273320)', sex='m', sex_source='Lingua Libre profile (P8 male)', age=None, native=True,
                  native_note='Lingua Libre profile: Japanese, native; Commons babel ja; Open Knowledge Japan member'),
    'takasugi': dict(speaker='TAKASUGI Shinji (Wikimedia user)', sex='m', sex_source='inferred from the given name Shinji (not stated)', age=None, native=True,
                     native_note='Commons babel: ja (native), ko-4, en-3, fr-3'),
    'doughaque': dict(speaker='Doughaque (Wikimedia user)', sex='unknown', sex_source='not stated', age=None, native=True,
                      native_note='file description: "recorded by Doughaque, a native Japanese speaker"; babel ja-N'),
    'quatrogatos': dict(speaker='Quatrogatos (Wikimedia user)', sex='unknown', sex_source='not stated', age=None, native=True,
                        native_note='file descriptions: "by a native Japanese speaker" / ネイティブスピーカーによる'),
    'wadakuramon': dict(speaker='Wadakuramon (Wikimedia user)', sex='unknown', sex_source='not stated (this uploader\'s files name both a female and a male native speaker)', age=None, native=True,
                        native_note='file description: ネイティブスピーカーによる (by a native speaker)'),
}

def commons(title, raw, **kw):
    e = info[title]
    x = e['ext']
    lic = plain(x.get('LicenseShortName'))
    d = dict(source=e['descriptionurl'], original_url=e['url'], raw=raw, license=lic,
             license_url=x.get('LicenseUrl') or ('https://creativecommons.org/publicdomain/zero/1.0/' if lic == 'CC0' else None),
             author=plain(x.get('Artist')), credit=plain(x.get('Credit')), description=plain(x.get('ImageDescription'))[:300],
             commons_title=title, commons_sha1=e.get('sha1'), commons_size=e.get('size'), uploader=e.get('user'), uploaded=e.get('timestamp'))
    d.update(kw)
    return d

def github(name, **kw):
    path = f'lib/ogg/{name}'
    d = dict(source=f'{TOFUGU_REPO}/blob/master/{urllib.parse.quote(path)}',
             original_url=f'https://raw.githubusercontent.com/tofugu/japanese-vocabulary-pronunciation-audio/master/{urllib.parse.quote(path)}',
             raw=f'raw/tofugu-github/{name}', license='CC BY-SA 4.0', license_url='https://creativecommons.org/licenses/by-sa/4.0/',
             author='Tofugu and WaniKani', credit=f'{TOFUGU_REPO} (repository licence: CC BY-SA 4.0; README: "Please attribute to Tofugu and WaniKani")',
             description='WaniKani vocabulary audio (old recordings), GitHub repository tofugu/japanese-vocabulary-pronunciation-audio')
    d.update(kw)
    return d

LL = 'LL-Q5287 (jpn)-'
def ll(speaker, word):
    return f'File:{LL}{speaker}-{word}.wav', f'raw/{LL.replace(" ", "_")}{speaker}-{word.replace(" ", "_")}.wav'

C = []
def add(id, vowel, kana, word, reading, gloss, set_, context, slug, src, take=None):
    d = dict(id=id, vowel=vowel, kana=kana, word=word, reading=reading, gloss=gloss, set=set_, context=context, speaker_slug=slug, take=take)
    d.update(SPK[slug]); d.update(src)
    C.append(d)

# --- isolated: the whole utterance is the vowel (kana reading, a one-vowel word, or one long vowel) ---
add('a_hakatanoshio', 'a', 'あ', 'あ', 'あ', 'kana reading', 'isolated', 'kana あ read aloud, 3 short takes', 'hakatanoshio', commons('File:Ja-A.oga', 'raw/Ja-A.oga'))
add('i_hakatanoshio', 'i', 'い', 'い', 'い', 'kana reading', 'isolated', 'kana い read aloud', 'hakatanoshio', commons('File:Japanese I.ogg', 'raw/Japanese_I.ogg'))
add('u_hakatanoshio', 'u', 'う', 'う', 'う', 'kana reading', 'isolated', 'kana う read aloud', 'hakatanoshio', commons('File:Japanese U.ogg', 'raw/Japanese_U.ogg'))
add('e_hakatanoshio', 'e', 'え', 'え', 'え', 'kana reading', 'isolated', 'kana え read aloud, 3 short takes', 'hakatanoshio', commons('File:Ja-E.oga', 'raw/Ja-E.oga'))
add('o_hakatanoshio', 'o', 'お', 'お', 'お', 'kana reading', 'isolated', 'kana お read aloud', 'hakatanoshio', commons('File:Japanese O.ogg', 'raw/Japanese_O.ogg'))
add('u_spacecat2', 'u', 'う', 'う', 'う', 'kana reading', 'isolated', 'kana う read aloud', 'spacecat2', commons('File:Ja-U.oga', 'raw/Ja-U.oga'))
add('o_spacecat2', 'o', 'お', 'お', 'お', 'kana reading', 'isolated', 'kana お read aloud', 'spacecat2', commons('File:Ja-O.oga', 'raw/Ja-O.oga'))
add('i_tofugu-f', 'i', 'い', '胃', 'い', 'stomach', 'isolated', 'one-vowel word 胃 (い)', 'tofugu-f', commons('File:Ja-WaniKaniTofuguFemale-i.oga', 'raw/Ja-WaniKaniTofuguFemale-i.oga'))
add('o_tofugu-f', 'o', 'お', '尾', 'お', 'tail', 'isolated', 'one-vowel word 尾 (お)', 'tofugu-f', commons('File:Ja-WaniKaniTofuguFemale-o.ogg', 'raw/Ja-WaniKaniTofuguFemale-o.ogg'))
add('e_tofugu-m', 'e', 'え', '絵', 'え', 'picture', 'isolated', 'one-vowel word 絵 (え)', 'tofugu-m', commons('File:Ja-WaniKaniTofuguMale-e.ogg', 'raw/Ja-WaniKaniTofuguMale-e.ogg'))
add('o_tofugu-m', 'o', 'お', '王', 'おう', 'king', 'isolated', 'word 王 (おう), said as one long vowel [oː]', 'tofugu-m', github('王【おう】.ogg'))
add('i_marsian', 'i', 'い', '良い', 'いい', 'good', 'isolated', 'word いい, one long vowel [iː]', 'marsian', commons('File:Ja-ii-good.ogg', 'raw/Ja-ii-good.ogg'))
t, r = ll('Zsrtrgh', 'いい')
add('i_zsrtrgh', 'i', 'い', 'いい', 'いい', 'good', 'isolated', 'word いい, one long vowel [iː]', 'zsrtrgh', commons(t, r))
t, r = ll('葵心', '絵')
add('e_aoishin', 'e', 'え', '絵', 'え', 'picture', 'isolated', 'one-vowel word 絵 (え)', 'aoishin', commons(t, r))
t, r = ll('葵心', '黄 (おう)')
add('o_aoishin', 'o', 'お', '黄', 'おう', 'yellow (on reading)', 'isolated', 'reading おう of 黄, one long vowel [oː]', 'aoishin', commons(t, r))
t, r = ll('フィリピン人', 'い')
add('i_firipinjin', 'i', 'い', 'い', 'い', 'kana reading', 'isolated', 'kana い read aloud (non-native beginner)', 'firipinjin', commons(t, r))

# --- word: the first vowel of a word, cut before a voiceless stop/affricate closure (more speakers for あ and う) ---
t, r = ll('葵心', 'アタック')
# dropped a_aoishin-atakku: too short, 65 ms of vowel after trimming (two short ア)
t, r = ll('葵心', '一月')
# dropped i_aoishin-ichigatsu: too short, 75 ms of vowel after trimming
t, r = ll('葵心', '宇宙')
add('u_aoishin-uchuu', 'u', 'う', '宇宙', 'うちゅう', 'space', 'word', 'the う of うちゅう: word-initial (before /tɕ/) and the long ゅう after /tɕ/', 'aoishin', commons(t, r), take=[0, 1])
t, r = ll('Zsrtrgh', 'あか')
# dropped a_zsrtrgh-aka: too short, 55 ms of vowel after trimming
add('a_marsian-aka', 'a', 'あ', '赤', 'あか', 'red', 'word', 'both あ of あか: word-initial (before /k/) and after /k/', 'marsian', commons('File:Ja-aka-red.ogg', 'raw/Ja-aka-red.ogg'), take=[0, 1])
add('u_marsian-utsu', 'u', 'う', '打つ', 'うつ', 'to hit', 'word', 'both う of うつ: word-initial (before /ts/) and after /ts/', 'marsian', commons('File:Ja-utsu.ogg', 'raw/Ja-utsu.ogg'), take=[0, 1])
# dropped a_takasugi-atatakai: 60 Hz hum at -33 dBFS (SNR ~18 dB) and a 40 ms first vowel
add('u_doughaque-kuuki', 'u', 'う', '空気', 'くうき', 'air', 'word', 'the long う [ɯː] of くうき, between /k/ and /k/', 'doughaque', commons('File:Ja-kuuki.ogg', 'raw/Ja-kuuki.ogg'), take=[0])
t, r = ll('Higa4', 'お寺')
# dropped o_higa4-otera: too short, 50 ms of vowel after trimming
add('a_tofugu-m-aka', 'a', 'あ', '赤', 'あか', 'red', 'word', 'both あ of あか: word-initial (before /k/) and after /k/', 'tofugu-m', github('赤【あか】.ogg'), take=[0, 1])
add('a_tofugu-f-atsui', 'a', 'あ', '厚い', 'あつい', 'thick', 'word', 'word-initial あ before /ts/ (あつい)', 'tofugu-f', github('厚い【あつい】.ogg'), take=[0])
add('u_tofugu-m-utsu', 'u', 'う', '打つ', 'うつ', 'to hit', 'word', 'both う of うつ: word-initial (before /ts/) and after /ts/', 'tofugu-m', github('打つ【うつ】.ogg'), take=[0, 1])
add('u_tofugu-f-utsu', 'u', 'う', '撃つ', 'うつ', 'to shoot', 'word', 'both う of うつ: word-initial (before /ts/) and after /ts/', 'tofugu-f', github('撃つ【うつ】.ogg'), take=[0, 1])
add('e_tofugu-f-ekken', 'e', 'え', '越権', 'えっけん', 'overstepping authority', 'word', 'word-initial え before /kk/ (えっけん)', 'tofugu-f', github('越権【えっけん】.ogg'), take=[0])
add('i_tofugu-m-ichi', 'i', 'い', '一', 'いち', 'one', 'word', 'both い of いち: word-initial (before /tɕ/) and after /tɕ/', 'tofugu-m', github('一【いち】.ogg'), take=[0, 1])

# dropped a_quatrogatos-akiko: the first あ of Akiko is a 30 ms blip
add('e_quatrogatos-eiko', 'e', 'え', '英子', 'えいこ', 'given name Eiko (松田英子, given name first)', 'word', 'word-initial long え [eː] before /k/ (えいこ)', 'quatrogatos', commons('File:Eiko Matsuda.ogg', 'raw/Eiko_Matsuda.ogg'), take=[0])
add('i_wadakuramon-ishizuchi', 'i', 'い', '石鎚山', 'いしづちさん', 'Mount Ishizuchi', 'word', 'word-initial い before /ɕ/ (いしづち)', 'wadakuramon', commons('File:Ishizuchi-san.ogg', 'raw/Ishizuchi-san.ogg'), take=[0])

json.dump(C, open(f'{SP}/tools/cands.json', 'w'), ensure_ascii=False, indent=1)
print(len(C), 'candidates')
missing = [c['raw'] for c in C if not os.path.exists(f"{SP}/{c['raw']}")]
print('missing raw:', missing)
