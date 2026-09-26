# joins tools/cands.json (sources, licences) and tools/results.json (analysis) into manifest.json and LICENSES.md
import json, os, math
SP = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
C = json.load(open(f'{SP}/tools/cands.json'))
R = json.load(open(f'{SP}/tools/results.json'))
EXTRA = json.load(open(f'{SP}/tools/notes.json')) if os.path.exists(f'{SP}/tools/notes.json') else {}
REF = json.load(open(f'{SP}/ref/yazawa_kondo_2019_f1f2_summary.json'))
LONG = {'i_marsian', 'i_zsrtrgh', 'o_tofugu-m', 'o_aoishin', 'u_doughaque-kuuki', 'e_quatrogatos-eiko'}

def attribution(c):
    lic = c['license']
    if c['speaker_slug'].startswith('tofugu'):
        via = 'Wikimedia Commons (uploader ' + c['uploader'] + ')' if c.get('commons_title') else 'github.com/tofugu/japanese-vocabulary-pronunciation-audio'
        return f'Tofugu and WaniKani (https://www.tofugu.com, https://www.wanikani.com), {lic}, via {via}'
    if c['commons_title'].startswith('File:LL-'):
        name = c['speaker'].split(' (')[0]
        return f'{name} (Lingua Libre speaker and recorder), {lic}, via Wikimedia Commons'
    who = c['author']
    if 'No machine-readable author' in who:
        who = who.split('. ')[-1].replace(' assumed (based on copyright claims).', '').replace(' assumed (based on copyright claims)', '')
    return f'{who}, {lic}, via Wikimedia Commons'

M = []
for c in C:
    r = R.get(c['id'])
    if not r:
        print('no result for', c['id']); continue
    p = r['probe']
    f0 = r['f0_median_hz']
    ff = r.get('f0_file_median_hz') or f0
    lo = r.get('f0_file_p10_hz')
    # the low end of the voice's range separates the sexes better than a median that pitch accent pulls up
    guess = None if lo is None else ('m' if lo < 140 else 'f' if lo > 160 else '?')
    probs = list(EXTRA.get(c['id'], {}).get('problems', []))
    br = p['stream_bit_rate'] or p['avg_bit_rate']
    if p['codec'] == 'vorbis' and br and br < 100000:
        probs.append(f'lossy Vorbis at {round(br / 1000)} kbps nominal')
    if r['voiced_s'] < 0.1:
        probs.append(f'short: {round(r["voiced_s"] * 1000)} ms of vowel (the loop repeats it {r["loop_repeats"]} times)')
    if r['clip']['n_full_scale']:
        probs.append(f'clipped: {r["clip"]["n_full_scale"]} samples at full scale in the original')
    if r['snr_db'] < 30:
        probs.append(f'noisy: vowel only {r["snr_db"]} dB above the noise floor')
    sx = c['sex'] if c['sex'] in ('m', 'f') else guess
    if r['rough_f1_f2_hz'] and sx in ('m', 'f'):
        ref = REF[f"{c['vowel']}_{'long' if c['id'] in LONG else 'short'}_{sx.upper()}"]
        f1, f2 = r['rough_f1_f2_hz']
        z1, z2 = (f1 - ref['f1_mean']) / ref['f1_sd'], (f2 - ref['f2_mean']) / ref['f2_sd']
        if abs(z1) > 2.5 or abs(z2) > 2.5:
            probs.append(f"rough check only: my LPC estimate F1/F2 {f1}/{f2} Hz is {z1:+.1f}/{z2:+.1f} SD off the Tokyo {'male' if sx == 'm' else 'female'} /{c['vowel']}/ mean "
                         f"{ref['f1_mean']}/{ref['f2_mean']} Hz (Yazawa & Kondo 2019): an unusual token or an estimation slip, not a wrong vowel")
    if not c['native']:
        probs.append('non-native speaker (Japanese level: beginner)')
    if c['sex'] == 'unknown' and guess in ('m', 'f'):
        probs.append(f'sex not documented; the voice\'s F0 (whole file: median {ff} Hz, 10th percentile {lo} Hz) suggests {"male" if guess == "m" else "female"}')
    m = dict(file=r['wav'], looped=r['looped'], vowel=c['vowel'], kana=c['kana'], set=c['set'], word=c['word'], reading=c['reading'],
             gloss=c['gloss'], context=c['context'], speaker=c['speaker'], speaker_slug=c['speaker_slug'], sex=c['sex'], sex_source=c['sex_source'],
             f0_median_hz=f0, f0_p10_p90_hz=r['f0_p10_p90_hz'], f0_file_median_hz=r.get('f0_file_median_hz'), f0_file_p10_hz=r.get('f0_file_p10_hz'), sex_guess_from_f0=guess, age=c['age'], native=c['native'], native_note=c['native_note'],
             source=c['source'], original_url=c['original_url'], raw=c['raw'], license=c['license'], license_url=c['license_url'],
             attribution=attribution(c), author=c['author'], credit=c['credit'], description=c['description'],
             original=dict(container=p['container'], codec=p['codec'], bit_rate=br, sample_rate=p['sample_rate'], channels=p['channels'],
                           bits_per_sample=p['bits_per_sample'], duration_s=p['duration_s'], sha1_commons=c.get('commons_sha1')),
             duration_s=r['duration_s'], voiced_s=r['voiced_s'], segments_s=r['segments_s'], loop_s=r['loop_s'], loop_repeats=r['loop_repeats'],
             peak_dbfs=r['peak_dbfs'], voiced_rms_dbfs=r['voiced_rms_dbfs'], noise_floor_dbfs=r['noise_floor_dbfs'], snr_db=r['snr_db'],
             clipped_samples=r['clip']['n_full_scale'], rough_f1_f2_hz=r['rough_f1_f2_hz'], problems=probs)
    M.append(m)
M.sort(key=lambda m: ('aiueo'.index(m['vowel']), m['set'] != 'isolated', m['speaker_slug']))
json.dump(M, open(f'{SP}/manifest.json', 'w'), ensure_ascii=False, indent=1)
print(len(M), 'entries')
