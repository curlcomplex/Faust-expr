"""Regenerate the frozen hardware-led studies with actual Faust.

Restricted publisher audio is never rehosted. The Soundwave take and its notes
are exported only after validating their pinned identity and distribution text.
Presets and inferred note scores are frozen; this delivery command does no fit.
"""
from pathlib import Path
import argparse,hashlib,io,json,os,urllib.request,zipfile
import numpy as np
import soundfile as sf
from scipy.signal import resample_poly
from synth_batch import SynthLab,ROOT
from acid_batch import controls

MANIFEST=ROOT/'modules/analog-classics/hardware-listening/presets.json'

def digest(data): return hashlib.sha256(data).hexdigest()

def deliver(out):
 m=json.loads(MANIFEST.read_text());L=SynthLab(out)
 aud=out/'audition';aud.mkdir(exist_ok=True)
 L.report.update(commit=os.getenv('GITHUB_SHA','local'),hardware_approved=False,human_approved=False,
  manifest_sha256=digest(MANIFEST.read_bytes()),reference_links={},scope='Frozen render validation, not new fitting or independent hardware acquisition evidence')
 for name,e in m['instruments'].items():
  src=ROOT/e['source'];L.check(name+':source-identity',digest(src.read_bytes())==e['source_sha256'])
  oldsrc=ROOT/'modules/juno-106/v1/voice.dsp' if name=='juno106' else src
  old=L.build(name+'-before',oldsrc);new=L.build(name+'-revised',src)
  default={k:v[2] for k,v in controls(old)[1].items()}
  events=[]
  for on,off,f in e['notes_frames']:events += [(on,'freq',f),(on,'gate',1),(off,'gate',0)]
  frames=round(e['duration']*e['score_rate']);rendered={}
  for label,exe,values,gainkey in [('before',old,default,'before_gain'),('revised',new,e['controls'],'after_gain')]:
   raw=L.render(name+'-'+label,exe,values,events,frames=frames)[:,0]
   measured=float(np.sqrt(np.mean(raw.astype(float)**2)))
   expected=e['measurement']['before' if label=='before' else 'after']['raw_rms']
   L.check(name+':'+label+':frozen-rms',abs(measured-expected)<2e-6,measured=measured,expected=expected)
   y=raw*e['playback'][gainkey]
   L.check(name+':'+label+':headroom',np.max(abs(y))<=.901)
   sf.write(aud/f'{name}-{label}.wav',y,48000,subtype='PCM_24');rendered[label]=y
  L.report['reference_links'][name]=e['reference']
  sf.write(aud/f'{name}-before-then-revised.wav',np.concatenate([rendered['before'],np.zeros(24000),rendered['revised']]),48000,subtype='PCM_24')
  if name=='juno106':
   ref=e['reference'];req=urllib.request.Request(ref['url'],headers={'User-Agent':'CURLOP reference study'})
   with urllib.request.urlopen(req,timeout=45) as response: data=response.read(20000000)
   if digest(data)!=ref['archive_sha256']:raise ValueError('reference archive changed')
   archive=zipfile.ZipFile(io.BytesIO(data));notes=archive.read('SWJUNO2.TXT')
   if b'Distribute these samples freely' not in notes:raise ValueError('reference permission not verified')
   wav=archive.read(ref['archive_member'])
   if digest(wav)!=ref['source_sha256']:raise ValueError('reference take changed')
   x,sr=sf.read(io.BytesIO(wav));x=x[:round(e['duration']*sr)]
   x=resample_poly(x,160,147)[:frames]*e['playback']['reference_gain']
   (aud/'SWJUNO2.TXT').write_bytes(notes)
   sf.write(aud/'juno106-hardware.wav',x,48000,subtype='PCM_24')
   sf.write(aud/'juno106-hardware-before-revised.wav',np.concatenate([x,np.zeros(24000),rendered['before'],np.zeros(24000),rendered['revised']]),48000,subtype='PCM_24')
 L.report['passed']=bool(L.report['checks']) and all(q.get('passed') is True for q in L.report['checks'])
 (out/'results.json').write_text(json.dumps(L.report,indent=2));(out/'presets.json').write_text(MANIFEST.read_text())
 print('HARDWARE_DELIVERY',json.dumps({'passed':L.report['passed'],'checks':len(L.report['checks']),'renders':len(L.report['renders'])}),flush=True)
 return 0 if L.report['passed'] else 1

if __name__=='__main__':
 ap=argparse.ArgumentParser();ap.add_argument('--out',type=Path,required=True)
 a=ap.parse_args();raise SystemExit(deliver(a.out))
