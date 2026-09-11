"""Acquire the twelve licensed CP Vintage public MP3 previews, not lossless captures.
No checkout, account, credentials or payment; no implied hardware settings.
"""
from pathlib import Path
import argparse,json,subprocess
from datetime import datetime,timezone
from urllib.parse import urljoin
import fetch_snare_references as source
SELECTION=tuple(f'CPV-{i:02d}' for i in range(1,13))
def acquire(out):
 out.mkdir(parents=True,exist_ok=True);html=source.get(source.ALBUM,4_000_000);text=html.decode()
 if 'creativecommons.org/licenses/by/4.0' not in text:raise ValueError('creator license declaration missing')
 (out/'source-page.html').write_bytes(html);parser=source.Parser();parser.feed(text)
 if not parser.album:raise ValueError('metadata missing')
 tracks={r['title']:r for r in parser.album['trackinfo']};records=[]
 for name in SELECTION:
  tr=tracks[name];url=tr.get('file',{}).get('mp3-128')
  if not url:raise ValueError('public preview missing '+name)
  if url.startswith('//'):url='https:'+url
  if url.startswith('http:'):url='https:'+url[5:]
  raw=source.get(url,2_000_000);mp3=out/(name+'.mp3');mp3.write_bytes(raw)
  wav=out/(name+'.wav');subprocess.run(['ffmpeg','-v','error','-y','-i',str(mp3),'-map','0:a:0','-ac','1','-ar','48000','-c:a','pcm_f32le',str(wav)],check=True,timeout=30)
  records.append({'id':name,'machine':'CP Vintage','source_page':urljoin(source.ALBUM,tr['title_link']),'media_url':url,'mp3_sha256':source.sha(raw),'wav_sha256':source.sha(wav.read_bytes()),'firmware':None,'parameters':None,'recording_gain':None,'drive_per_hit':None})
 report={'schema':1,'source':source.ALBUM,'source_page_sha256':source.sha(html),'creator':'Winston Edwards / Particles Into Waves','collection':'Syntakt Designer Drums','release_date':'2022-06-13','license':'CC BY 4.0','license_url':source.LICENSE,'retrieved_utc':datetime.now(timezone.utc).isoformat(),'selection_before_audio_analysis':list(SELECTION),'records':records,'limits':'Public MP3 previews decoded mono float32/48k; no gain, EQ, trim or normalization. Creator states dry/no post, some other pack hits use onboard drive. Firmware, controls, per-hit drive and capture gain unknown; contextual comparisons not waveform/knob fitting.'}
 (out/'manifest.json').write_text(json.dumps(report,indent=2)+'\n')
 (out/'ATTRIBUTION.md').write_text('# Clap reference attribution\n\nCPV-01 through CPV-12 from Syntakt Designer Drums, Winston Edwards / Particles Into Waves, 13 June 2022.\n\nSource: '+source.ALBUM+'\nLicense: CC BY 4.0 ('+source.LICENSE+').\n\nPublic MP3 previews decoded to mono float32/48 kHz. No gain, EQ, trim or normalization on acquired files. Later audition gains are recorded separately. No endorsement is implied.\n')
 print(json.dumps({'references':len(records),'license':'CC BY 4.0','quality':'lossy / unknown settings'}))
if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('--out',type=Path,required=True);a=p.parse_args();acquire(a.out.resolve())
