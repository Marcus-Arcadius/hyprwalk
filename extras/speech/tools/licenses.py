# writes LICENSES.md from manifest.json
import json, os, collections
SP = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
M = json.load(open(f'{SP}/manifest.json'))
REF = json.load(open(f'{SP}/ref/yazawa_kondo_2019_f1f2_summary.json'))

def sexcol(m):
    if m['sex'] in ('m', 'f'):
        return {'m': 'male', 'f': 'female'}[m['sex']]
    g = m['sex_guess_from_f0']
    return f"not stated ({'male' if g == 'm' else 'female' if g == 'f' else '?'} by F0)"

def link(m):
    s = m['source']
    label = 'Commons' if 'commons.wikimedia.org' in s else 'GitHub'
    return f'[{label}]({s.replace(" ", "%20").replace("(", "%28").replace(")", "%29")})'

L = []
w = L.append
spk = collections.OrderedDict()
for m in M:
    spk.setdefault(m['speaker_slug'], m)
w('# Japanese vowel recordings: sources and licences\n')
w(f'{len(M)} recordings of the five Japanese vowels by {len(spk)} speakers, gathered on 2026-09-25/26 for testing the hyprwalk lip sync. '
  'Every file came from Wikimedia Commons (Lingua Libre or individual contributors) or from the Tofugu/WaniKani pronunciation repository on GitHub, '
  'and every one is public domain, CC0, CC BY or CC BY-SA. The licence of each was read from the Commons API (extmetadata) or from the repository\'s LICENSE file, '
  'and each download was checked against the SHA-1 that Commons or GitHub gives for it.\n')
w('Two kinds of recording:\n')
w('- **isolated**: the whole utterance is the vowel: a kana read aloud (あ, い, ...), a one-vowel word (胃, 絵, 尾) or one long vowel (いい, おう).')
w('- **word**: the vowel cut out of a short word, next to a voiceless stop or affricate so the cut falls in the silent closure (the あ of あか, both う of うつ, ...). '
  'These add speakers where isolated vowels are scarce, above all for あ and う.\n')

w('## Counts\n')
w('| Vowel | Recordings | Isolated | From words | Male | Female | Speakers |')
w('|---|---|---|---|---|---|---|')
for v in 'aiueo':
    ms = [m for m in M if m['vowel'] == v]
    sx = lambda m: m['sex'] if m['sex'] in ('m', 'f') else m['sex_guess_from_f0']
    w(f"| {v} ({ms[0]['kana']}) | {len(ms)} | {sum(m['set'] == 'isolated' for m in ms)} | {sum(m['set'] == 'word' for m in ms)} | "
      f"{sum(sx(m) == 'm' for m in ms)} | {sum(sx(m) == 'f' for m in ms)} | {', '.join(m['speaker_slug'] for m in ms)} |")
w('\nMale and female include the sexes guessed from the voice\'s pitch where the source does not state one (marked "by F0" below). '
  'All speakers are adults as far as anyone can tell; no free recording of a child was found. One speaker (firipinjin) is not a native speaker of Japanese.\n')

w('## Recordings\n')
w('Files are named `<vowel>_<speaker>[-<word>].wav`; `wav/` has the whole recording, `in/` the looped vowel.\n')
w('| Loop (in/) | Vowel | Recorded as | Speaker | Sex | Vowel ms | Licence | Attribution | Source |')
w('|---|---|---|---|---|---|---|---|---|')
for m in M:
    rec = m['word'] if m['word'] == m['reading'] else f"{m['word']} ({m['reading']})"
    w(f"| `{os.path.basename(m['looped'])}` | {m['kana']} | {rec}, {m['set']} | {m['speaker']} | {sexcol(m)} | {round(m['voiced_s'] * 1000)} | "
      f"{m['license']} | {m['attribution'].replace('|', '/')} | {link(m)} |")

w('\n## Speakers\n')
for slug, m in spk.items():
    if m['sex'] in ('m', 'f'):
        sx = f"{'male' if m['sex'] == 'm' else 'female'} ({m['sex_source']})"
    else:
        g = {'m': 'male', 'f': 'female'}.get(m['sex_guess_from_f0'], 'unclear')
        sx = f"not stated; its pitch suggests {g}"
    w(f"- **{slug}**: {m['speaker']}. Sex: {sx}. {m['native_note'][0].upper() + m['native_note'][1:]}.")

w('\n## Problems and caveats\n')
for m in M:
    p = [x for x in m['problems'] if not x.startswith('sex not documented')]
    if p:
        w(f"- `{os.path.basename(m['file'])}`: " + '; '.join(p) + '.')

w('\n## How the files were made\n')
w('- `raw/`: the originals as downloaded (Ogg Vorbis from Commons and GitHub, 16-bit PCM WAV from Lingua Libre). `raw/unused/` holds originals that were looked at and left out (below).')
w('- `wav/`: each original converted with ffmpeg to mono (channels averaged), 16-bit PCM, 48000 Hz, nothing else changed (level, silence and all). '
  'The hyprwalk harness (`--audio`) reads 16/24/32-bit PCM or 32-bit float WAV at any rate and mixes channels; the lip sync decimates to about 12 kHz.')
w('- `in/`: the vowel only, looped to at least 5 s. The vowel was found from the frame level (25 ms frames, 5 ms hop), periodicity and the share of energy below 1 kHz: '
  'for isolated recordings every voiced stretch within 25 dB of the loudest frame (Hakatanoshio117117 reads each kana three times, so those loops join three takes), '
  'trimmed by 10 ms at both ends; for words the chosen vowel nuclei (split at level dips of 8 dB or more, each spanning the frames within 12 dB of its peak), '
  'trimmed by 5 ms at a word-initial start, 15 ms after a consonant and 15 ms at the end, to keep the consonant transitions out. '
  'The pieces get 5 ms raised-cosine fades and are joined back to back with no gap, and the whole is repeated to reach 5 s. '
  'The level is left as recorded. `segments_s` in manifest.json gives the cut points in the `wav/` file.')
w('- The previous session\'s five files (Ja-A.oga, Ja-E.oga, Ja-O.oga, Ja-U.oga, Ja-WaniKaniTofuguFemale-i.oga) are included under their Commons names and processed the same way. '
  'Its `in/real_*.wav` loops repeated the whole file, silences included; these loops hold only the vowel.')
w('- manifest.json also gives each file\'s original codec, bit rate and sample rate, the durations, levels (peak, the vowel\'s RMS, the noise floor; plain RMS in dBFS, '
  'so a full-scale sine is -3 dB where the lip sync\'s level() says 0), the median F0 and a rough F1/F2 from my own quick LPC, as a sanity check only.\n')

w('## Licence terms for these files\n')
w('- Public domain and CC0 (Hakatanoshio117117, Spacecat2, marsian, Doughaque, 葵心, フィリピン人): no conditions.')
w('- CC BY 4.0 (Quatrogatos): credit the author and say what was changed.')
w('- CC BY-SA 4.0 (Tofugu and WaniKani; Zsrtrgh; Wadakuramon): credit, say what was changed, and share the `wav/` and `in/` files made from them under CC BY-SA 4.0 too. '
  'The changes: converted to 48 kHz mono PCM; for `in/`, the vowel cut out, faded and looped.')
w('- Tofugu asks for the credit "Tofugu (https://www.tofugu.com) and WaniKani (https://www.wanikani.com)" (README of github.com/tofugu/japanese-vocabulary-pronunciation-audio).\n')

w('## Looked at and left out\n')
w('- Ja-atatakai.ogg (TAKASUGI Shinji, CC BY-SA 3.0): 60 Hz hum at about -33 dBFS and a first あ of only 40 ms.')
w('- LL-Q5287 (jpn)-葵心-アタック.wav, -葵心-一月.wav, -Zsrtrgh-あか.wav, -Higa4-お寺.wav (CC0 / CC BY-SA 4.0): their vowels are only 50-75 ms long after trimming.')
w('- Akiko Wakabayashi.ogg (Quatrogatos, CC0): the first あ of Akiko is a 30 ms blip.')
w('- Ja-WaniKaniTofuguMale-oto.oga and the Tofugu words 〜位, 一気, 内, 得体, 暑い, 歌, 秋, 跡 (CC BY-SA 4.0): the same two Tofugu voices already cover these vowels.')
w('- Not free, so not used: JVPD (NII-SRC "Japanese vowel database with physical information of male, female and child speakers": /haa hii huu hee hoo/ by 385 speakers aged 6-56, the only source with children; research use only), JVS and JSUT (research only), Forvo. '
  'Freesound disallows automated access (robots.txt), so it was not searched. Lingua Libre\'s other Japanese speakers are beginners (CKali, 530 words) or have no suitable words; '
  'Tatoeba has no audio for vowel-only sentences such as ああ or ええ.\n')

w('## Reference: Tokyo Japanese vowel formants\n')
w('Mean F1/F2 (Hz, SD) at the vowel midpoint from Yazawa & Kondo (2019), 16 native Tokyo speakers (8 male, 8 female, aged 21-30), /CVCV/ nonce words in isolation and in a carrier sentence, '
  'measured with Praat; data: Kakeru Yazawa, "Japanese Vowel Length Acoustic Data", Zenodo record 15227304, CC BY 4.0 (ref/JPLongShortVowels.csv, summary in ref/yazawa_kondo_2019_f1f2_summary.json).\n')
w('| Vowel | Male short F1 / F2 | Male long F1 / F2 | Female short F1 / F2 | Female long F1 / F2 |')
w('|---|---|---|---|---|')
for v in 'aiueo':
    c = lambda L, S: f"{REF[f'{v}_{L}_{S}']['f1_mean']} ({REF[f'{v}_{L}_{S}']['f1_sd']}) / {REF[f'{v}_{L}_{S}']['f2_mean']} ({REF[f'{v}_{L}_{S}']['f2_sd']})"
    w(f"| {v} | {c('short', 'M')} | {c('long', 'M')} | {c('short', 'F')} | {c('long', 'F')} |")
open(f'{SP}/LICENSES.md', 'w').write('\n'.join(L) + '\n')
print('wrote', len(L), 'lines')
