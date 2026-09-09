"""Fixed licensed CY Alloy previews; unknown settings, not calibration captures.
No login, payment, private endpoint or access bypass. Network acquisition is
separate from DSP playback and ordinary offline qualification.
"""
from __future__ import annotations
import argparse, hashlib, json, subprocess
from pathlib import Path
from datetime import datetime, timezone
from urllib.parse import urljoin
from fetch_snare_references import get, Parser, ALBUM, LICENSE
# Registered before hearing/fitting. Test references do not fit descriptor scale.
TRAIN=('CYA-01','CYA-04','CYA-07','CYA-10','CYA-13','CYA-16')
TEST=('CYA-03','CYA-09')
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def acquire(out):
    out.mkdir(parents=True,exist_ok=True)
    page=get(ALBUM,4_000_000);text=page.decode()
    if 'creativecommons.org/licenses/by/4.0' not in text:raise ValueError('Creator CC BY declaration missing')
    (out/'source-page.html').write_bytes(page)
    parser=Parser();parser.feed(text)
    if not parser.album:raise ValueError('No public track metadata')
    tracks={r['title']:r for r in parser.album['trackinfo']};rows=[]
    for name in TRAIN+TEST:
        tr=tracks[name];url=tr.get('file',{}).get('mp3-128')
        if not url:raise ValueError('Missing public preview '+name)
        if url.startswith('//'):url='https:'+url
        if url.startswith('http:'):url='https:'+url[5:]
        original=out/(name+'.mp3');original.write_bytes(get(url,2_000_000))
        info=json.loads(subprocess.check_output(['ffprobe','-v','error','-show_streams','-show_format','-of','json',str(original)],text=True))
        if float(info['format']['duration'])>20:raise ValueError('Duration bound')
        target=out/(name+'.wav')
        subprocess.run(['ffmpeg','-v','error','-y','-i',str(original),'-map','0:a:0','-ac','1','-ar','48000','-c:a','pcm_f32le',str(target)],check=True,timeout=30)
        rows.append(dict(id=name,split='development' if name in TRAIN else 'reserved-comparator',source_page=urljoin(ALBUM,tr['title_link']),media_url=url,mp3_sha256=sha(original),wav_sha256=sha(target),original_format=info,firmware=None,parameters=None,recording_gain=None))
    result=dict(schema=1,source=ALBUM,creator='Winston Edwards / Particles Into Waves',collection='Syntakt Designer Drums',release_date='2022-06-13',license='CC BY 4.0',license_url=LICENSE,source_page_sha256=sha(out/'source-page.html'),retrieved_utc=datetime.now(timezone.utc).isoformat(),development=list(TRAIN),reserved_comparators=list(TEST),records=rows,conversion='MP3 decoded to mono float32 48 kHz; no gain, trim, EQ or normalization',limits='Lossy previews, unknown firmware/patch/gain. Creator describes dry/no-post recordings, some onboard drive. Broad spectral/decay coverage only; not waveform or knob calibration. Reserved comparator split is not known-control generalization.')
    (out/'manifest.json').write_text(json.dumps(result,indent=2)+'\n')
    (out/'ATTRIBUTION.md').write_text('# Metal reference attribution\n\n'+', '.join(TRAIN+TEST)+' from **Syntakt Designer Drums**, Winston Edwards / Particles Into Waves, 13 June 2022.\n\nSource: '+ALBUM+'\nLicense: Creative Commons Attribution 4.0 International ('+LICENSE+').\n\nPublic MP3 previews decoded to mono float32/48 kHz; no acquisition gain/EQ/normalization. Any audition gains are recorded separately. No creator or Elektron endorsement implied.\n')
    print(json.dumps(dict(references=len(rows),license='CC BY 4.0')))
if __name__=='__main__':
    a=argparse.ArgumentParser();a.add_argument('--out',type=Path,required=True);acquire(a.parse_args().out.resolve())
