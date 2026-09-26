# Japanese vowel recordings: sources and licences

28 recordings of the five Japanese vowels by 11 speakers, gathered on 2026-09-25/26 for testing the hypr3d lip sync. Every file came from Wikimedia Commons (Lingua Libre or individual contributors) or from the Tofugu/WaniKani pronunciation repository on GitHub, and every one is public domain, CC0, CC BY or CC BY-SA. The licence of each was read from the Commons API (extmetadata) or from the repository's LICENSE file, and each download was checked against the SHA-1 that Commons or GitHub gives for it.

Two kinds of recording:

- **isolated**: the whole utterance is the vowel: a kana read aloud (あ, い, ...), a one-vowel word (胃, 絵, 尾) or one long vowel (いい, おう).
- **word**: the vowel cut out of a short word, next to a voiceless stop or affricate so the cut falls in the silent closure (the あ of あか, both う of うつ, ...). These add speakers where isolated vowels are scarce, above all for あ and う.

## Counts

| Vowel | Recordings | Isolated | From words | Male | Female | Speakers |
|---|---|---|---|---|---|---|
| a (あ) | 4 | 1 | 3 | 3 | 1 | hakatanoshio, marsian, tofugu-f, tofugu-m |
| i (い) | 7 | 5 | 2 | 6 | 1 | firipinjin, hakatanoshio, marsian, tofugu-f, zsrtrgh, tofugu-m, wadakuramon |
| u (う) | 7 | 2 | 5 | 5 | 2 | hakatanoshio, spacecat2, aoishin, doughaque, marsian, tofugu-f, tofugu-m |
| e (え) | 5 | 3 | 2 | 3 | 2 | aoishin, hakatanoshio, tofugu-m, quatrogatos, tofugu-f |
| o (お) | 5 | 5 | 0 | 3 | 2 | aoishin, hakatanoshio, spacecat2, tofugu-f, tofugu-m |

Male and female include the sexes guessed from the voice's pitch where the source does not state one (marked "by F0" below). All speakers are adults as far as anyone can tell; no free recording of a child was found. One speaker (firipinjin) is not a native speaker of Japanese.

## Recordings

Files are named `<vowel>_<speaker>[-<word>].wav`; `wav/` has the whole recording, `in/` the looped vowel.

| Loop (in/) | Vowel | Recorded as | Speaker | Sex | Vowel ms | Licence | Attribution | Source |
|---|---|---|---|---|---|---|---|---|
| `a_hakatanoshio.wav` | あ | あ, isolated | Hakatanoshio117117 (Wikimedia user) | not stated (male by F0) | 240 | Public domain | Hakatanoshio117117, Public domain, via Wikimedia Commons | [Commons](https://commons.wikimedia.org/wiki/File:Ja-A.oga) |
| `a_marsian-aka.wav` | あ | 赤 (あか), word | marsian (Wikimedia user) | not stated (male by F0) | 150 | Public domain | marsian, Public domain, via Wikimedia Commons | [Commons](https://commons.wikimedia.org/wiki/File:Ja-aka-red.ogg) |
| `a_tofugu-f-atsui.wav` | あ | 厚い (あつい), word | Tofugu/WaniKani female voice | female | 120 | CC BY-SA 4.0 | Tofugu and WaniKani (https://www.tofugu.com, https://www.wanikani.com), CC BY-SA 4.0, via github.com/tofugu/japanese-vocabulary-pronunciation-audio | [GitHub](https://github.com/tofugu/japanese-vocabulary-pronunciation-audio/blob/master/lib/ogg/%E5%8E%9A%E3%81%84%E3%80%90%E3%81%82%E3%81%A4%E3%81%84%E3%80%91.ogg) |
| `a_tofugu-m-aka.wav` | あ | 赤 (あか), word | Tofugu/WaniKani male voice | male | 115 | CC BY-SA 4.0 | Tofugu and WaniKani (https://www.tofugu.com, https://www.wanikani.com), CC BY-SA 4.0, via github.com/tofugu/japanese-vocabulary-pronunciation-audio | [GitHub](https://github.com/tofugu/japanese-vocabulary-pronunciation-audio/blob/master/lib/ogg/%E8%B5%A4%E3%80%90%E3%81%82%E3%81%8B%E3%80%91.ogg) |
| `i_firipinjin.wav` | い | い, isolated | フィリピン人 (Lingua Libre speaker Q1575602) | male | 165 | CC0 | フィリピン人 (Lingua Libre speaker and recorder), CC0, via Wikimedia Commons | [Commons](https://commons.wikimedia.org/wiki/File:LL-Q5287_%28jpn%29-%E3%83%95%E3%82%A3%E3%83%AA%E3%83%94%E3%83%B3%E4%BA%BA-%E3%81%84.wav) |
| `i_hakatanoshio.wav` | い | い, isolated | Hakatanoshio117117 (Wikimedia user) | not stated (male by F0) | 250 | Public domain | Hakatanoshio117117, Public domain, via Wikimedia Commons | [Commons](https://commons.wikimedia.org/wiki/File:Japanese_I.ogg) |
| `i_marsian.wav` | い | 良い (いい), isolated | marsian (Wikimedia user) | not stated (male by F0) | 200 | Public domain | marsian, Public domain, via Wikimedia Commons | [Commons](https://commons.wikimedia.org/wiki/File:Ja-ii-good.ogg) |
| `i_tofugu-f.wav` | い | 胃 (い), isolated | Tofugu/WaniKani female voice | female | 180 | CC BY-SA 4.0 | Tofugu and WaniKani (https://www.tofugu.com, https://www.wanikani.com), CC BY-SA 4.0, via Wikimedia Commons (uploader VioletMother) | [Commons](https://commons.wikimedia.org/wiki/File:Ja-WaniKaniTofuguFemale-i.oga) |
| `i_zsrtrgh.wav` | い | いい, isolated | Zsrtrgh (Lingua Libre speaker Q1564396) | not stated (male by F0) | 165 | CC BY-SA 4.0 | Zsrtrgh (Lingua Libre speaker and recorder), CC BY-SA 4.0, via Wikimedia Commons | [Commons](https://commons.wikimedia.org/wiki/File:LL-Q5287_%28jpn%29-Zsrtrgh-%E3%81%84%E3%81%84.wav) |
| `i_tofugu-m-ichi.wav` | い | 一 (いち), word | Tofugu/WaniKani male voice | male | 130 | CC BY-SA 4.0 | Tofugu and WaniKani (https://www.tofugu.com, https://www.wanikani.com), CC BY-SA 4.0, via github.com/tofugu/japanese-vocabulary-pronunciation-audio | [GitHub](https://github.com/tofugu/japanese-vocabulary-pronunciation-audio/blob/master/lib/ogg/%E4%B8%80%E3%80%90%E3%81%84%E3%81%A1%E3%80%91.ogg) |
| `i_wadakuramon-ishizuchi.wav` | い | 石鎚山 (いしづちさん), word | Wadakuramon (Wikimedia user) | not stated (male by F0) | 105 | CC BY-SA 4.0 | Wadakuramon, CC BY-SA 4.0, via Wikimedia Commons | [Commons](https://commons.wikimedia.org/wiki/File:Ishizuchi-san.ogg) |
| `u_hakatanoshio.wav` | う | う, isolated | Hakatanoshio117117 (Wikimedia user) | not stated (male by F0) | 250 | Public domain | Hakatanoshio117117, Public domain, via Wikimedia Commons | [Commons](https://commons.wikimedia.org/wiki/File:Japanese_U.ogg) |
| `u_spacecat2.wav` | う | う, isolated | Spacecat2 (Wikimedia user) | not stated (female by F0) | 290 | Public domain | Spacecat2~commonswiki, Public domain, via Wikimedia Commons | [Commons](https://commons.wikimedia.org/wiki/File:Ja-U.oga) |
| `u_aoishin-uchuu.wav` | う | 宇宙 (うちゅう), word | 葵心 (Lingua Libre speaker Q1392056) | male | 160 | CC0 | 葵心 (Lingua Libre speaker and recorder), CC0, via Wikimedia Commons | [Commons](https://commons.wikimedia.org/wiki/File:LL-Q5287_%28jpn%29-%E8%91%B5%E5%BF%83-%E5%AE%87%E5%AE%99.wav) |
| `u_doughaque-kuuki.wav` | う | 空気 (くうき), word | Doughaque (Wikimedia user) | not stated (male by F0) | 110 | Public domain | Doughaque, Public domain, via Wikimedia Commons | [Commons](https://commons.wikimedia.org/wiki/File:Ja-kuuki.ogg) |
| `u_marsian-utsu.wav` | う | 打つ (うつ), word | marsian (Wikimedia user) | not stated (male by F0) | 125 | Public domain | marsian, Public domain, via Wikimedia Commons | [Commons](https://commons.wikimedia.org/wiki/File:Ja-utsu.ogg) |
| `u_tofugu-f-utsu.wav` | う | 撃つ (うつ), word | Tofugu/WaniKani female voice | female | 230 | CC BY-SA 4.0 | Tofugu and WaniKani (https://www.tofugu.com, https://www.wanikani.com), CC BY-SA 4.0, via github.com/tofugu/japanese-vocabulary-pronunciation-audio | [GitHub](https://github.com/tofugu/japanese-vocabulary-pronunciation-audio/blob/master/lib/ogg/%E6%92%83%E3%81%A4%E3%80%90%E3%81%86%E3%81%A4%E3%80%91.ogg) |
| `u_tofugu-m-utsu.wav` | う | 打つ (うつ), word | Tofugu/WaniKani male voice | male | 145 | CC BY-SA 4.0 | Tofugu and WaniKani (https://www.tofugu.com, https://www.wanikani.com), CC BY-SA 4.0, via github.com/tofugu/japanese-vocabulary-pronunciation-audio | [GitHub](https://github.com/tofugu/japanese-vocabulary-pronunciation-audio/blob/master/lib/ogg/%E6%89%93%E3%81%A4%E3%80%90%E3%81%86%E3%81%A4%E3%80%91.ogg) |
| `e_aoishin.wav` | え | 絵 (え), isolated | 葵心 (Lingua Libre speaker Q1392056) | male | 155 | CC0 | 葵心 (Lingua Libre speaker and recorder), CC0, via Wikimedia Commons | [Commons](https://commons.wikimedia.org/wiki/File:LL-Q5287_%28jpn%29-%E8%91%B5%E5%BF%83-%E7%B5%B5.wav) |
| `e_hakatanoshio.wav` | え | え, isolated | Hakatanoshio117117 (Wikimedia user) | not stated (male by F0) | 250 | Public domain | Hakatanoshio117117, Public domain, via Wikimedia Commons | [Commons](https://commons.wikimedia.org/wiki/File:Ja-E.oga) |
| `e_tofugu-m.wav` | え | 絵 (え), isolated | Tofugu/WaniKani male voice | male | 170 | CC BY-SA 4.0 | Tofugu and WaniKani (https://www.tofugu.com, https://www.wanikani.com), CC BY-SA 4.0, via Wikimedia Commons (uploader VioletMother) | [Commons](https://commons.wikimedia.org/wiki/File:Ja-WaniKaniTofuguMale-e.ogg) |
| `e_quatrogatos-eiko.wav` | え | 英子 (えいこ), word | Quatrogatos (Wikimedia user) | not stated (female by F0) | 150 | CC BY 4.0 | Quatrogatos, CC BY 4.0, via Wikimedia Commons | [Commons](https://commons.wikimedia.org/wiki/File:Eiko_Matsuda.ogg) |
| `e_tofugu-f-ekken.wav` | え | 越権 (えっけん), word | Tofugu/WaniKani female voice | female | 95 | CC BY-SA 4.0 | Tofugu and WaniKani (https://www.tofugu.com, https://www.wanikani.com), CC BY-SA 4.0, via github.com/tofugu/japanese-vocabulary-pronunciation-audio | [GitHub](https://github.com/tofugu/japanese-vocabulary-pronunciation-audio/blob/master/lib/ogg/%E8%B6%8A%E6%A8%A9%E3%80%90%E3%81%88%E3%81%A3%E3%81%91%E3%82%93%E3%80%91.ogg) |
| `o_aoishin.wav` | お | 黄 (おう), isolated | 葵心 (Lingua Libre speaker Q1392056) | male | 175 | CC0 | 葵心 (Lingua Libre speaker and recorder), CC0, via Wikimedia Commons | [Commons](https://commons.wikimedia.org/wiki/File:LL-Q5287_%28jpn%29-%E8%91%B5%E5%BF%83-%E9%BB%84_%28%E3%81%8A%E3%81%86%29.wav) |
| `o_hakatanoshio.wav` | お | お, isolated | Hakatanoshio117117 (Wikimedia user) | not stated (male by F0) | 290 | Public domain | Hakatanoshio117117, Public domain, via Wikimedia Commons | [Commons](https://commons.wikimedia.org/wiki/File:Japanese_O.ogg) |
| `o_spacecat2.wav` | お | お, isolated | Spacecat2 (Wikimedia user) | not stated (female by F0) | 255 | Public domain | Spacecat2~commonswiki, Public domain, via Wikimedia Commons | [Commons](https://commons.wikimedia.org/wiki/File:Ja-O.oga) |
| `o_tofugu-f.wav` | お | 尾 (お), isolated | Tofugu/WaniKani female voice | female | 235 | CC BY-SA 4.0 | Tofugu and WaniKani (https://www.tofugu.com, https://www.wanikani.com), CC BY-SA 4.0, via Wikimedia Commons (uploader VioletMother) | [Commons](https://commons.wikimedia.org/wiki/File:Ja-WaniKaniTofuguFemale-o.ogg) |
| `o_tofugu-m.wav` | お | 王 (おう), isolated | Tofugu/WaniKani male voice | male | 255 | CC BY-SA 4.0 | Tofugu and WaniKani (https://www.tofugu.com, https://www.wanikani.com), CC BY-SA 4.0, via github.com/tofugu/japanese-vocabulary-pronunciation-audio | [GitHub](https://github.com/tofugu/japanese-vocabulary-pronunciation-audio/blob/master/lib/ogg/%E7%8E%8B%E3%80%90%E3%81%8A%E3%81%86%E3%80%91.ogg) |

## Speakers

- **hakatanoshio**: Hakatanoshio117117 (Wikimedia user). Sex: not stated; its pitch suggests male. Commons/ja.wikipedia user with babel "ja" (native); set "Audio files of hiragana (set by Hakatanoshio117117)".
- **marsian**: marsian (Wikimedia user). Sex: not stated; its pitch suggests male. File description: "recorded by: a native speaker of Japanese"; "I, marsian recorded my own voice"; babel ja.
- **tofugu-f**: Tofugu/WaniKani female voice. Sex: female (Tofugu README and the Commons file names/descriptions ("by a native Japanese woman")). Native Japanese, Kansai accent, amateur voice actor, amateur recording (Tofugu README).
- **tofugu-m**: Tofugu/WaniKani male voice. Sex: male (Tofugu README and the Commons file names/descriptions ("by a male native Japanese voice actor")). Native Japanese, Tokyo accent, professional voice actor, professional recording (Tofugu README).
- **firipinjin**: フィリピン人 (Lingua Libre speaker Q1575602). Sex: male (Lingua Libre profile (P8 male) and file page speakerGender=male). NOT native: Lingua Libre profile gives Japanese = beginner; native Tagalog and English.
- **zsrtrgh**: Zsrtrgh (Lingua Libre speaker Q1564396). Sex: not stated; its pitch suggests male. Lingua Libre profile: Japanese, native.
- **wadakuramon**: Wadakuramon (Wikimedia user). Sex: not stated; its pitch suggests male. File description: ネイティブスピーカーによる (by a native speaker).
- **spacecat2**: Spacecat2 (Wikimedia user). Sex: not stated; its pitch suggests female. File description: "pronounced by a native speaker of Japanese".
- **aoishin**: 葵心 (Lingua Libre speaker Q1392056). Sex: male (Lingua Libre profile (P8 male) and file page speakerGender=male). Lingua Libre profile: Japanese, native.
- **doughaque**: Doughaque (Wikimedia user). Sex: not stated; its pitch suggests male. File description: "recorded by Doughaque, a native Japanese speaker"; babel ja-N.
- **quatrogatos**: Quatrogatos (Wikimedia user). Sex: not stated; its pitch suggests female. File descriptions: "by a native Japanese speaker" / ネイティブスピーカーによる.

## Problems and caveats

- `a_hakatanoshio.wav`: three short takes (about 80 ms of steady vowel each after trimming) with about 0.6 s of digital silence between them in the original; the loop joins the three takes.
- `a_marsian-aka.wav`: the uploader ran Audacity noise removal and normalisation on it; lossy Vorbis at 60 kbps nominal.
- `a_tofugu-m-aka.wav`: rough check only: my LPC estimate F1/F2 913/1435 Hz is +2.8/+1.1 SD off the Tokyo male /a/ mean 687/1283 Hz (Yazawa & Kondo 2019): an unusual token or an estimation slip, not a wrong vowel.
- `i_firipinjin.wav`: non-native speaker (Japanese level: beginner).
- `i_hakatanoshio.wav`: three short takes joined in the loop (see a_hakatanoshio).
- `i_marsian.wav`: the uploader ran Audacity noise removal and normalisation on it; lossy Vorbis at 60 kbps nominal.
- `i_tofugu-f.wav`: amateur recording (Tofugu README); Kansai accent.
- `i_wadakuramon-ishizuchi.wav`: this uploader's other files name a female and a male native speaker, so the speaker of this one is not certain; lossy Vorbis at 80 kbps nominal; rough check only: my LPC estimate F1/F2 285/2773 Hz is -0.4/+3.0 SD off the Tokyo male /i/ mean 301/2154 Hz (Yazawa & Kondo 2019): an unusual token or an estimation slip, not a wrong vowel.
- `u_hakatanoshio.wav`: three short takes joined in the loop (see a_hakatanoshio); rough check only: my LPC estimate F1/F2 278/1854 Hz is -1.8/+2.6 SD off the Tokyo male /u/ mean 348/1435 Hz (Yazawa & Kondo 2019): an unusual token or an estimation slip, not a wrong vowel.
- `u_spacecat2.wav`: lossy Vorbis at 96 kbps nominal.
- `u_doughaque-kuuki.wav`: between two /k/ closures (velar context), not word-initial; lossy Vorbis at 80 kbps nominal.
- `u_marsian-utsu.wav`: the uploader ran Audacity noise removal and normalisation on it; lossy Vorbis at 60 kbps nominal.
- `e_hakatanoshio.wav`: three short takes joined in the loop (see a_hakatanoshio).
- `e_quatrogatos-eiko.wav`: cut from a recording of the name 'Eiko Matsuda' (given name first); lossy Vorbis at 80 kbps nominal.
- `e_tofugu-f-ekken.wav`: short: 95 ms of vowel (the loop repeats it 53 times).
- `o_hakatanoshio.wav`: three short takes joined in the loop (see a_hakatanoshio).
- `o_spacecat2.wav`: the file is only 0.35 s and the vowel starts and ends at its edges (little silence to estimate noise from); lossy Vorbis at 96 kbps nominal.
- `o_tofugu-f.wav`: amateur recording (Tofugu README); Kansai accent.

## How the files were made

- `raw/`: the originals as downloaded (Ogg Vorbis from Commons and GitHub, 16-bit PCM WAV from Lingua Libre). `raw/unused/` holds originals that were looked at and left out (below).
- `wav/`: each original converted with ffmpeg to mono (channels averaged), 16-bit PCM, 48000 Hz, nothing else changed (level, silence and all). The hypr3d harness (`--audio`) reads 16/24/32-bit PCM or 32-bit float WAV at any rate and mixes channels; the lip sync decimates to about 12 kHz.
- `in/`: the vowel only, looped to at least 5 s. The vowel was found from the frame level (25 ms frames, 5 ms hop), periodicity and the share of energy below 1 kHz: for isolated recordings every voiced stretch within 25 dB of the loudest frame (Hakatanoshio117117 reads each kana three times, so those loops join three takes), trimmed by 10 ms at both ends; for words the chosen vowel nuclei (split at level dips of 8 dB or more, each spanning the frames within 12 dB of its peak), trimmed by 5 ms at a word-initial start, 15 ms after a consonant and 15 ms at the end, to keep the consonant transitions out. The pieces get 5 ms raised-cosine fades and are joined back to back with no gap, and the whole is repeated to reach 5 s. The level is left as recorded. `segments_s` in manifest.json gives the cut points in the `wav/` file.
- The previous session's five files (Ja-A.oga, Ja-E.oga, Ja-O.oga, Ja-U.oga, Ja-WaniKaniTofuguFemale-i.oga) are included under their Commons names and processed the same way. Its `in/real_*.wav` loops repeated the whole file, silences included; these loops hold only the vowel.
- manifest.json also gives each file's original codec, bit rate and sample rate, the durations, levels (peak, the vowel's RMS, the noise floor; plain RMS in dBFS, so a full-scale sine is -3 dB where the lip sync's level() says 0), the median F0 and a rough F1/F2 from my own quick LPC, as a sanity check only.

## Licence terms for these files

- Public domain and CC0 (Hakatanoshio117117, Spacecat2, marsian, Doughaque, 葵心, フィリピン人): no conditions.
- CC BY 4.0 (Quatrogatos): credit the author and say what was changed.
- CC BY-SA 4.0 (Tofugu and WaniKani; Zsrtrgh; Wadakuramon): credit, say what was changed, and share the `wav/` and `in/` files made from them under CC BY-SA 4.0 too. The changes: converted to 48 kHz mono PCM; for `in/`, the vowel cut out, faded and looped.
- Tofugu asks for the credit "Tofugu (https://www.tofugu.com) and WaniKani (https://www.wanikani.com)" (README of github.com/tofugu/japanese-vocabulary-pronunciation-audio).

## Looked at and left out

- Ja-atatakai.ogg (TAKASUGI Shinji, CC BY-SA 3.0): 60 Hz hum at about -33 dBFS and a first あ of only 40 ms.
- LL-Q5287 (jpn)-葵心-アタック.wav, -葵心-一月.wav, -Zsrtrgh-あか.wav, -Higa4-お寺.wav (CC0 / CC BY-SA 4.0): their vowels are only 50-75 ms long after trimming.
- Akiko Wakabayashi.ogg (Quatrogatos, CC0): the first あ of Akiko is a 30 ms blip.
- Ja-WaniKaniTofuguMale-oto.oga and the Tofugu words 〜位, 一気, 内, 得体, 暑い, 歌, 秋, 跡 (CC BY-SA 4.0): the same two Tofugu voices already cover these vowels.
- Not free, so not used: JVPD (NII-SRC "Japanese vowel database with physical information of male, female and child speakers": /haa hii huu hee hoo/ by 385 speakers aged 6-56, the only source with children; research use only), JVS and JSUT (research only), Forvo. Freesound disallows automated access (robots.txt), so it was not searched. Lingua Libre's other Japanese speakers are beginners (CKali, 530 words) or have no suitable words; Tatoeba has no audio for vowel-only sentences such as ああ or ええ.

## Reference: Tokyo Japanese vowel formants

Mean F1/F2 (Hz, SD) at the vowel midpoint from Yazawa & Kondo (2019), 16 native Tokyo speakers (8 male, 8 female, aged 21-30), /CVCV/ nonce words in isolation and in a carrier sentence, measured with Praat; data: Kakeru Yazawa, "Japanese Vowel Length Acoustic Data", Zenodo record 15227304, CC BY 4.0 (ref/JPLongShortVowels.csv, summary in ref/yazawa_kondo_2019_f1f2_summary.json).

| Vowel | Male short F1 / F2 | Male long F1 / F2 | Female short F1 / F2 | Female long F1 / F2 |
|---|---|---|---|---|
| a | 687 (80) / 1283 (137) | 744 (80) / 1237 (93) | 801 (80) / 1530 (162) | 889 (77) / 1474 (130) |
| i | 301 (41) / 2154 (203) | 306 (49) / 2293 (219) | 346 (58) / 2639 (244) | 355 (55) / 2794 (211) |
| u | 348 (39) / 1435 (162) | 352 (46) / 1442 (154) | 434 (66) / 1645 (234) | 459 (76) / 1653 (183) |
| e | 443 (53) / 1947 (168) | 460 (41) / 2043 (163) | 516 (76) / 2302 (227) | 555 (89) / 2378 (220) |
| o | 462 (52) / 949 (141) | 455 (45) / 813 (95) | 526 (73) / 1127 (194) | 535 (79) / 996 (128) |
