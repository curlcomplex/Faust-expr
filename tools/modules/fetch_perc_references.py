"""Fixed licensed PC Carbon previews; unknown settings, not calibration captures."""
from __future__ import annotations
import argparse, hashlib, json, subprocess
from pathlib import Path
from datetime import datetime, timezone
from urllib.parse import urljoin
from fetch_snare_references import get, Parser, ALBUM, LICENSE
TRAIN=('PCC-01','PCC-02','PCC-04','PCC-05','PCC-07','PCC-08')
TEST=('PCC-03','PCC-09')
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def acquire(out):
    out.mkdir(parents=True,exist_ok=True);page=get(ALBUM,4_000_000);text=page.decode()
    if 'creativecommons.org/licenses/by/4.0' not in text:raise ValueError('Creator CC BY declaration missing')
    (out/'source-page.html').write_bytes(page);parser=Parser();parser.feed(text)
    if not parser.album:raise ValueError('No public track metadata')
    tracks={r['title']:r for r in parser.album['trackinfo']};rows=[]
    for name in TRAIN+TEST:
        tr=tracks[name];url=tr.get('file',{}).get('mp3-128')
        if not url:raise ValueError('Missing public preview '+name)
        if url.startswith('//'):url='https:'+url
        if url.startswith('http:'):url='https:'+url[5:]
        mp3=out/(name+'.mp3');mp3.write_bytes(get(url,2_000_000))
        info=json.loads(subprocess.check_output(['ffprobe','-v','error','-show_streams','-show_format','-of','json',str(mp3)],text=True))
        if float(info['format']['duration'])>15:raise ValueError('Duration bound')
        wav=out/(name+'.wav');subprocess.run(['ffmpeg','-v','error','-y','-i',str(mp3),'-map','0:a:0','-ac','1','-ar','48000','-c:a','pcm_f32le',str(wav)],check=True,timeout=30)
        rows.append(dict(id=name,split='development' if name in TRAIN else 'reserved-comparator',source_page=urljoin(ALBUM,tr['title_link']),media_url=url,mp3_sha256=sha(mp3),wav_sha256=sha(wav),original_format=info,firmware=None,parameters=None,recording_gain=None))
    result=dict(schema=1,source=ALBUM,creator='Winston Edwards / Particles Into Waves',collection='Syntakt Designer Drums',release_date='2022-06-13',license='CC BY 4.0',license_url=LICENSE,retrieved_utc=datetime.now(timezone.utc).isoformat(),development=list(TRAIN),reserved_comparators=list(TEST),records=rows,conversion='MP3 decoded to mono float32 48 kHz; no gain, trim, EQ or normalization',limits='Lossy previews, unknown firmware/patch/gain; broad spectral/decay coverage only.')
    (out/'manifest.json').write_text(json.dumps(result,indent=2)+'\n')
    (out/'ATTRIBUTION.md').write_text('# Perc reference attribution\n\n'+', '.join(TRAIN+TEST)+' from **Syntakt Designer Drums**, Winston Edwards / Particles Into Waves, 13 June 2022.\n\nSource: '+ALBUM+'\nLicense: Creative Commons Attribution 4.0 International ('+LICENSE+').\n\nPublic MP3 previews decoded to mono float32/48 kHz. No creator or Elektron endorsement implied.\n')
    print(json.dumps(dict(references=len(rows),license='CC BY 4.0')))
if __name__=='__main__':
    a=argparse.ArgumentParser();a.add_argument('--out',type=Path,required=True);acquire(a.parse_args().out.resolve())
