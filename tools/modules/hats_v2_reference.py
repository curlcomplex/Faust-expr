"""Pinned original-808 comparison. The reference WAVs never enter the DSP.
The recording sweeps were inspected during design. Listed repeated shots
are stability examples, NOT untouched or controlled held-out validation.
"""
from pathlib import Path
import argparse,hashlib,json,urllib.request
import numpy as np
from scipy.io import wavfile
from scipy.signal import welch,resample_poly
from scipy.spatial.distance import jensenshannon
from hats_v2_delivery import Lab,DEFAULT,describe,digest
HASHES={'closed':'115bc46c9dc65dd7341576665fae8d81712696cb5f3c296c9fd878a2c83ec359','open':'9fc7c8847d508c61c244fa20f83bc30324e19022efb4b551b68077c60235dee9'}
URLROOT='https://www.synthmania.com/Roland%20TR-808/Audio/Instrument%20samples/TR-808%20'
# Exact source sample indices: thresholded onsets minus 1ms at 44100Hz.
SHOTS={
 'CH_development':('closed',48840,.16,.46),
 'CH_repeat':('closed',192940,.16,.46),
 'OH_short_development':('open',836,.45,0.),
 'OH_short_repeat':('open',115852,.45,0.),
 'OH_medium_development':('open',449372,1.2,.46),
 'OH_long_development':('open',1324840,1.5,.675),
 'OH_long_repeat':('open',1434224,1.5,.675),
}

def fetch_refs(out):
 out.mkdir(parents=True,exist_ok=True)
 for kind in HASHES:
  p=out/('808_'+kind+'.wav')
  if p.exists() and digest(p)==HASHES[kind]:continue
  url=URLROOT+kind.title()+'%20High%20Hat.wav'
  req=urllib.request.Request(url,headers={'User-Agent':'Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36','Accept':'*/*','Accept-Language':'en-US,en;q=0.9'})
  with urllib.request.urlopen(req,timeout=45) as response:data=response.read(40_000_001)
  if hashlib.sha256(data).hexdigest()!=HASHES[kind]:raise ValueError('Reference changed: '+kind)
  p.write_bytes(data)

def load_reference(path):
 rate,x=wavfile.read(path)
 if x.dtype!=np.int16 or rate!=44100:raise ValueError('Unexpected source WAV format')
 x=x.astype(float)/32768
 if x.ndim==2:x=x.mean(axis=1)
 return rate,x

def bands(x):
 f,p=welch(np.pad(np.asarray(x,float),(512,512)),48000,nperseg=1024,noverlap=768);p/=p.sum()+1e-30
 edges=[0,3000,5000,7000,9000,12000,16000,24001]
 return np.array([p[(f>=a)&(f<b)].sum() for a,b in zip(edges,edges[1:])])

def run(refs,render,out):
 out.mkdir(parents=True,exist_ok=True)
 for kind,h in HASHES.items():
  if digest(refs/('808_'+kind+'.wav'))!=h:raise ValueError('Unpinned reference')
 source={k:load_reference(refs/('808_'+k+'.wav')) for k in HASHES}
 L=Lab(render);exe=render/'scalar/render';legacy=render/'legacy/render'
 results={};clips={};synths={};oldclips={}
 for name,(kind,index,seconds,dec) in SHOTS.items():
  rate,x=source[kind];length=round(seconds*48000)
  clip=resample_poly(x[index:index+round(seconds*rate)],160,147)[:length]
  if len(clip)!=length:raise ValueError('Incomplete source excerpt')
  y=L.render('reference-patch-'+name,exe,dict(articulation=int(kind=='open'),decay=dec),seconds=seconds+.02)[480:480+length]
  # Legacy defaults are a fixed baseline, not retuned per reference.
  old=np.fromfile(render/('old-'+kind+'.f32'),'<f4')[480:480+length]
  features={n:describe(v) for n,v in [('reference',clip),('new',y),('old',old)]}
  distance={n:float(jensenshannon(bands(clip),bands(v))) for n,v in [('new',y),('old',old)]}
  if name in ('CH_development','OH_medium_development'):
   for label,prefix in [('noise_only','ablation-noise-'),('one_oscillator','ablation-one-')]:
    ab=np.fromfile(render/(prefix+str(int(kind=='open'))+'.f32'),'<f4')[480:480+length]
    distance[label]=float(jensenshannon(bands(clip),bands(ab)))
  results[name]={'source':kind,'source_start_sample':index,'source_rate':rate,'duration_seconds':seconds,'synth_decay':dec,'features':features,'band_JS_distance':distance}
  clips[name]=clip;synths[name]=y;oldclips[name]=old
 # Audition: whole active excerpt RMS-matched per reference/synth/old, one global
 # headroom scalar. All gains logged. No EQ, compression, limiter, or reverb.
 order=['CH_development','OH_short_development','OH_medium_development','OH_long_development']
 gains={};matched={}
 for name in order:
  matched[name]={};gains[name]={}
  for label,x in [('Reference',clips[name]),('New',synths[name]),('Old',oldclips[name])]:
   active=.15 if name.startswith('CH') else SHOTS[name][2]
   gain=.08/np.sqrt(np.mean(x[:round(active*48000)]**2));gains[name][label]=float(gain)
   matched[name][label]=x*gain
 peak=max(abs(x).max() for d in matched.values() for x in d.values());common=float(min(1,.88/peak))
 def sequence(filename,labels,slot):
  n=len(order)*len(labels);audio=np.zeros(round((n*slot+.2)*48000));cues=[]
  for i,name in enumerate(order):
   for j,label in enumerate(labels):
    start=(i*len(labels)+j)*slot+.05;y=matched[name][label].copy()*common
    y[-240:]*=np.linspace(1,0,240);s=round(start*48000);audio[s:s+len(y)]+=y
    cues.append({'at_seconds':start,'case':name,'sound':label})
  wavfile.write(out/filename,48000,np.round(audio*32767).astype('<i2'));return cues
 cues=sequence('01_808_reference_then_new.wav',['Reference','New'],1.7)
 oldcues=sequence('04_808_old_new.wav',['Reference','Old','New'],1.7)
 bank_rate,bank=wavfile.read(render/'preset_bank_raw.wav')
 bank_peak=max(float(np.max(np.abs(wavfile.read(f)[1]))) for f in [render/'preset_bank_raw.wav',render/'groove_raw.wav',*render.glob('0[1-8]_*_raw.wav')])
 gain=.88/bank_peak;wavfile.write(out/'02_eight_analog_hats.wav',bank_rate,np.round(bank*gain*32767).astype('<i2'))
 for f in render.glob('0[1-8]_*_raw.wav'):
  rate,x=wavfile.read(f);wavfile.write(out/f.name.replace('_raw',''),rate,np.round(x*gain*32767).astype('<i2'))
 rate,groove=wavfile.read(render/'groove_raw.wav');wavfile.write(out/'03_hats_only_groove.wav',rate,np.round(groove*gain*32767).astype('<i2'))
 report={'reference':{'instrument':'Roland TR-808, serial 209265','recordist':'Synthmania','page':'https://www.synthmania.com/tr-808.htm','files':{k:{'url':URLROOT+k.title()+'%20High%20Hat.wav','sha256':h} for k,h in HASHES.items()},'settings':'Direct output according to creator; controls varied during recording. Knob values, gain and capture-chain calibration unknown.','rights':'Creator explicitly permits downloading/editing/sampling. No runtime sample playback. Only short critique excerpts redistributed here.'},'method':'Mono, resampled 44.1 to48k with polyphase160/147. Onset-aligned excerpts. Welch1024/Hann/256-hop with512-sample zero padding each boundary. Seven broad bands. Lower JS means closer band distribution, not more musical or more authentic. Times are computed over each stated excerpt. Development excerpts informed design, repeated-hit checks are not controlled knob holdouts.','comparisons':results,'audition':{'cues':cues,'old_new_cues':oldcues,'per_excerpt_gains':gains,'common_headroom_gain':common,'bank_uniform_gain':float(gain),'processing':'Comparison RMS matching plus one headroom scalar and5ms end fades only. Bank/groove/individuals share one global gain; no per-hit normalization or mastering.'},'additional_actual_renders':L.report['renders']}
 report['output_sha256']={p.name:digest(p) for p in sorted(out.glob('*.wav'))}
 (out/'reference_report.json').write_text(json.dumps(report,indent=2)+'\n')
 (out/'LISTEN.md').write_text('# Analog Hats v2\n\n01: 808 reference then new synth. Closed, short open, medium open, long open; one sound every1.7s.\n02: Eight patches: Classic, Tight, Soft, Dust, Crunch, Low Metal, Glass, Long. Three seconds each: CH, softer CH, OH, then OH closed after150ms.\n03: Hats-only groove,120BPM, no kick/music hiding the hats.\n04: Reference, old v1, new v2; same four cases.\n\nNo samples in the runtime synth. Full source provenance, timings and audition gains are in reference_report.json. Numerical checks are not musical approval.\n')
 print(json.dumps({k:v['band_JS_distance'] for k,v in results.items()},indent=2))
if __name__=='__main__':
 a=argparse.ArgumentParser();a.add_argument('--references',type=Path,required=True);a.add_argument('--render',type=Path);a.add_argument('--out',type=Path);a.add_argument('--fetch-only',action='store_true');args=a.parse_args()
 if args.fetch_only:fetch_refs(args.references)
 else:run(args.references,args.render,args.out)
