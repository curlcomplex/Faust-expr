"""Reference diagnostics: stereo power, onset alignment, log-band decay; not a realism score."""
from pathlib import Path
import numpy as np, soundfile as sf, scipy.signal as sig, json, subprocess, hashlib
ROOT=Path("build/reference")
RATE=48000
BANDS=np.geomspace(80,16000,33)
TIMES=np.array([0,.04,.12,.35,1,2,4,6])
def audio(path):
 x,r=sf.read(path,always_2d=True)
 if r!=RATE:
  import math
  g=math.gcd(r,RATE);x=sig.resample_poly(x,RATE//g,r//g)
 return x

def aligned(x):
 peak=np.max(np.abs(x))
 if not np.isfinite(x).all() or peak<1e-12:raise ValueError('Silent/nonfinite audio')
 j=np.argmax(np.max(np.abs(x),axis=1)>peak*.005)
 return x[max(0,j-48):],int(j)

def features(x,seconds=4):
 x,j=aligned(x); x=x[:int(seconds*RATE)]
 if len(x)<int(seconds*RATE):x=np.pad(x,((0,int(seconds*RATE)-len(x)),(0,0)))
 f,t,z=sig.stft(x,fs=RATE,nperseg=2048,noverlap=1536,axis=0,boundary=None,padded=False)
 # z axes: frequency, channel, frame. Average channel powers, never sum waves.
 p=np.mean(abs(z)**2,axis=1)
 band=np.stack([p[(f>=a)&(f<b)].sum(axis=0) for a,b in zip(BANDS[:-1],BANDS[1:])])
 tm=TIMES[TIMES<=seconds]; intervals=[]
 for a,b in zip(tm[:-1],tm[1:]):
  keep=(t>=a)&(t<b)
  intervals.append(np.mean(band[:,keep],axis=1) if keep.any() else np.full(32,1e-30))
 v=np.stack(intervals,axis=0)
 spec=p.sum(axis=1)
 energy=np.mean(x*x,axis=1)
 cum=np.cumsum(energy)/(energy.sum()+1e-30)
 return {'onset_frame':j,'peak':float(abs(x).max()),'rms':float(np.sqrt(energy.mean())),
  'centroid_hz':float((f*spec).sum()/(spec.sum()+1e-30)),
  'energy_above_3500_fraction':float(spec[f>3500].sum()/(spec.sum()+1e-30)),
  'energy_duration_90_s':float(np.searchsorted(cum,.9)/RATE),
  'band_time_power':v.tolist()}

def distance(a,b):
 # Gain-invariant log-power band/time RMSE, 50 dB floor relative each peak cell.
 av=np.array(a['band_time_power']);bv=np.array(b['band_time_power']);n=min(len(av),len(bv));av=av[:n];bv=bv[:n]
 ad=10*np.log10(np.maximum(av,1e-30));bd=10*np.log10(np.maximum(bv,1e-30))
 ad=np.maximum(ad,ad.max()-50);bd=np.maximum(bd,bd.max()-50)
 delta=ad-bd; offset=np.mean(delta)
 return float(np.sqrt(np.mean((delta-offset)**2)))

def render(engine,params,seconds=4,label='tmp'):
 path=ROOT/'out'/f'{engine}-{label}.f32'
 p=subprocess.run([str(ROOT/engine/'render_compare'),str(path),str(seconds)]+[f'{k}={v}' for k,v in params.items()],capture_output=True,text=True,timeout=60)
 if p.returncode:raise RuntimeError(p.stderr)
 x=np.fromfile(path,dtype='<f4').reshape(-1,2)
 return x,json.loads(p.stderr)

FILES={
 'suspended_soft':'1-vcsl-suspended/susCymb1_hit_stick_pp1.wav',
 'suspended_hard':'1-vcsl-suspended/susCymb1_hit_stick_f1.wav',
 'suspended_bell':'1-vcsl-suspended/susCymb1_hit_bell_mf1.wav',
 'suspended_general':'1-vcsl-suspended/susCymb1_hit_fff1.wav',
 'crash_soft':'2-virtuosity-crash/oh_crash_crash_vl1_rr1.flac',
 'crash_medium':'2-virtuosity-crash/oh_crash_crash_vl2_rr1.flac',
 'crash_hard':'2-virtuosity-crash/oh_crash_crash_vl3_rr1.flac',
 'ride_soft':'3-virtuosity-ride/oh_ride_ride_vl1_rr1.flac',
 'ride_hard':'3-virtuosity-ride/oh_ride_ride_vl3_rr1.flac',
 'ride_bell':'3-virtuosity-ride/oh_ride_bell_vl2_rr1.flac'}

def controls(engine,name):
 pos=.82 if not name.endswith('bell') else .13
 if name.startswith('ride') and not name.endswith('bell'):pos=.55
 vel=.7
 if name.endswith('soft'):vel=.1
 if name.endswith('medium'):vel=.35
 if name.endswith('bell'):vel=.45
 if name.endswith('general'):vel=1
 return {'velocity':vel,('strike_radius' if engine=='A' else 'position'):pos}

def validate_sources():
    manifest=json.loads((ROOT/'refs/sources.json').read_text())
    for record in manifest['files']:
        data=(ROOT/'refs'/record['file']).read_bytes()
        if hashlib.sha256(data).hexdigest()!=record['sha256']:
            raise ValueError('Reference checksum mismatch: '+record['file'])
    return manifest

def main():
    global ROOT
    import argparse
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--work',type=Path,default=ROOT)
    parser.add_argument('--preset',type=Path,required=True)
    args=parser.parse_args(); ROOT=args.work.resolve(); (ROOT/'out').mkdir(parents=True,exist_ok=True)
    sources=validate_sources(); preset=json.loads(args.preset.read_text())
    reference={n:features(audio(ROOT/'refs'/f),seconds=6) for n,f in FILES.items()}
    report={'method':'See references/CALIBRATION.md; six-second gain-invariant log-band/time comparison, NOT realism percentage.',
            'source_commits':{'A':'43f39b60b7b239f8e460d1d8ee7b6f027a0532fb','B':'821736879bdea6c1c58ce9f0d932e0ebe5196840'},
            'preset':preset,'references':sources,'reference_metrics':reference,'baseline':{'A':{},'B':{}},'candidate':{},'checks':[]}
    def run_case(engine,name,params,label):
        x,d=render(engine,params,seconds=6,label=label+'-'+name)
        if not np.isfinite(x).all() or d['peak']>=1: raise ValueError('Invalid or clipping audio')
        m=features(x,seconds=6)
        sf.write(ROOT/'out'/f'{engine}-{label}-{name}.wav',x,RATE,subtype='PCM_24')
        return {'parameters':params,'metrics':m,'diagnostics':d,'raw_sha256':hashlib.sha256(x.astype('<f4').tobytes()).hexdigest(),
                'log_band_time_rmse_db':distance(m,reference[name])}
    for engine in ('A','B'):
        for name in FILES:
            report['baseline'][engine][name]=run_case(engine,name,controls(engine,name),'baseline')
            print(engine,name,report['baseline'][engine][name]['log_band_time_rmse_db'],flush=True)
    for name in preset['velocities']:
        params=preset['body_and_stick']|controls('A',name);params['velocity']=preset['velocities'][name]
        report['candidate'][name]=run_case('A',name,params,'candidate')
    for name in preset['optimizer_training']:
        report['checks'].append({'name':name+':lower_registered_error_than_default','passed':report['candidate'][name]['log_band_time_rmse_db']<report['baseline']['A'][name]['log_band_time_rmse_db']})
    for key,group in [('reference',reference),('baseline-A',{n:r['metrics'] for n,r in report['baseline']['A'].items()}),('candidate-A',{n:r['metrics'] for n,r in report['candidate'].items()})]:
        report.setdefault('soft_to_hard_rms_db',{})[key]=float(20*np.log10(group['suspended_soft']['rms']/group['suspended_hard']['rms']))
    report['render_count']=24
    report['evaluation_commit']=subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip()
    (ROOT/'out/results.json').write_text(json.dumps(report,indent=2)+'\n')
    if not all(c['passed'] for c in report['checks']):raise RuntimeError('Registered calibration comparison failed')

if __name__=='__main__':main()
