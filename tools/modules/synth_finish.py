"""Actual-Faust oscillator assessment and bounded hardware comparisons.
No implicit promotion. Reference measurements, regressions, preset fitting and
owner acceptance remain separate. Only freely redistributable reference audio
is included; the Juno60 recordings are linked, not repackaged.
"""
from pathlib import Path
import argparse, io, json, os, re, tempfile, traceback, urllib.parse, zipfile
import numpy as np
import soundfile as sf
from scipy.optimize import differential_evolution
from synth_batch import SynthLab, DEFAULTS, ROOT, phrase
from synth_four_voice_checkpoint import DEFAULT as J60_DEFAULT
from acid_batch import controls
from hats_v2_delivery import command, digest
from synth_sample_probe import estimate_pitch, spectrum
from synth_finish_materials import fetch, sha, gitblob, J60, J60_FILES, J106_URL, J106_SHA

PAIRS={
 'juno60':('juno-60/v1','juno-60/v3',J60_DEFAULT),
 'juno106':('juno-106/v2','juno-106/v3',DEFAULTS['juno106']),
 'sh101':('mono-101/v3','mono-101/v4',DEFAULTS['mono101']),
}

def fundamental(x,sr,f=220):
    x=np.asarray(x,float);w=np.hanning(len(x))
    return float(2*abs(np.sum(x*w*np.exp(-2j*np.pi*f*np.arange(len(x))/sr)))/w.sum())

def patch(name,base,shape='mix',f=220):
    p=dict(base);p.update(gate=0,freq=f,velocity=1,saw=1 if shape!='pulse' else 0,
      pulse=1 if shape!='saw' else 0,sub=0,noise=0,pwm=.5,pwmDepth=0,cutoff=16000,
      resonance=0,filterEnv=0,attack=.003,sustain=1,release=.1)
    if 'hpf' in p:p['hpf']=20
    if 'lfoPitch' in p:p.update(lfoPitch=0,lfoFilter=0)
    return p

def loss(got,target):
    active=np.asarray(target)>-35
    return float(np.sqrt(np.mean((np.asarray(got)[active]-np.asarray(target)[active])**2)))

def trial(exe,values,sr,frames,score,audio,onset=.1):
    rows={(0,k):v for k,v in values.items()};rows[round(onset*sr),'gate']=1
    score.write_text(''.join(f'{n}\t{k}\t{v:.9g}\n' for (n,k),v in sorted(rows.items())))
    command([exe,score,audio,sr,128,frames,0])
    y=np.fromfile(audio,'<f4')
    if len(y)!=frames or not np.isfinite(y).all():raise ValueError('bad native trial')
    return y

def decode(raw):
    x,sr=sf.read(io.BytesIO(raw),always_2d=True,dtype='float64')
    return x.mean(axis=1),int(sr)

def expected_delta(original,version):
    s=original.replace('import("stdfaust.lib");','import("stdfaust.lib");\ncdco=library("../../analog-classics/synth-finish/coherent_dco.lib");')
    s=re.sub(r'declare version "[^"]+";',f'declare version "0.{version}.0-coherent-candidate";',s)
    return s.replace('os.polyblep_saw(f)','(cdco.waves(f,width):(_,!,!))').replace('os.pulsetrain(f,width)','(cdco.waves(f,width):(!,_,!))').replace('os.polyblep_square(f*.5)','(cdco.waves(f,width):(!,!,_))')

def run(out):
    L=SynthLab(out);c=L.check;(out/'audition').mkdir(exist_ok=True)
    L.report.update(commit=os.getenv('GITHUB_SHA','local'),version='finish-assessment-1',
      hardware_approved=False,human_approved=False,selected_for_promotion=False,
      oscillator_results={},hardware_comparisons=[],extra_sources={},freeze_ready=False,
      limitations=['Only declared component/note comparisons are complete.',
      'A steady-window score is not panel/envelope or whole-instrument calibration.'])
    try:
      exes={};bases={}
      for name,(old,new,base) in PAIRS.items():
        bases[name]=base
        oldsrc=ROOT/'modules'/old/'voice.dsp';newsrc=ROOT/'modules'/new/'voice.dsp'
        c(name+':only-oscillator-delta',newsrc.read_text()==expected_delta(oldsrc.read_text(),new[-1]))
        for version,path in [('baseline',old),('coherent',new)]:
          exes[name,version]=L.build(name+'-'+version,ROOT/'modules'/path/'voice.dsp')
        oi,ou=controls(exes[name,'baseline']);ni,nu=controls(exes[name,'coherent'])
        c(name+':contract',oi==ni==(0,1) and ou==nu)
      with tempfile.TemporaryDirectory(prefix='finish-trials-') as temp:
        temp=Path(temp);score=temp/'score.tsv';audio=temp/'audio.f32'
        for name,base in bases.items():
          result={}
          for version in ('baseline','coherent'):
            shapes={}
            for shape in ('saw','pulse','mix'):
              amps=[]
              for idle in (.025,.05,.1,.2,.5,1.):
                frames=round((idle+1.5)*48000)
                y=trial(exes[name,version],patch(name,base,shape),48000,frames,score,audio,idle)
                amps.append(fundamental(y[round((idle+.5)*48000):],48000))
              spread=float(20*np.log10(max(amps)/min(amps)))
              shapes[shape]=dict(amplitudes=amps,fundamental_spread_db=spread)
              if version=='coherent':c(name+':coherent-'+shape,spread<.1,spread_db=spread)
            result[version]=shapes
          L.report['oscillator_results'][name]=result
          c(name+':old-idle-defect-reproduced',result['baseline']['mix']['fundamental_spread_db']>1)
          exe=exes[name,'coherent'];src=ROOT/'modules'/PAIRS[name][1]/'voice.dsp'
          vec=L.build(name+'-vec',src,True);events=[(4800,'gate',1),(72000,'gate',0)];p=patch(name,base)
          original=L.render(name+'-lifecycle',exe,p,events,frames=96000)[:,0]
          c(name+':initial-silence',not np.any(original[:4800]))
          c(name+':tail-settles',np.max(abs(original[-4800:]))<1e-6)
          for block in (1,127,512):
            b=L.render(name+f'-block-{block}',exe,p,events,block=block,frames=96000)[:,0]
            c(name+f':block-{block}',np.array_equal(b,original))
          b=L.render(name+'-vec-check',vec,p,events,frames=96000)[:,0]
          c(name+':vector',np.max(abs(b-original))<1e-4,max_error=float(np.max(abs(b-original))))
          for sr in (44100,48000,96000):
            y=L.render(name+f'-low-pulse-{sr}',exe,patch(name,base,'pulse',20),[(round(.1*sr),'gate',1)],sr=sr,frames=2*sr)[:,0]
            freq=float(np.argmax(abs(np.fft.rfft((y[sr:]-y[sr:].mean())*np.hanning(sr)))[1:])+1)
            c(name+f':pulse20Hz-{sr}',abs(freq-20)<1.01,fft_frequency=freq)
          for version in ('baseline','coherent'):
            music=L.render(name+'-'+version+'-phrase',exes[name,version],base,phrase(root=220 if name.startswith('juno') else 110),frames=384000)
            L.wav(name+'-'+version+'-phrase.wav',music)
          print('OSCILLATOR',name,json.dumps(result),flush=True)
        # Fixed known waveform selections: no optimizer, no factory mapping change.
        for key,(path,blob) in J60_FILES.items():
          raw,final=fetch(J60+urllib.parse.quote(path,safe='/'))
          if blob and gitblob(raw)!=blob:raise ValueError('hardware Git blob mismatch')
          x,sr=decode(raw);region={'j60-saw':(2.0,3.0),'j60-pulse':(2.6,3.6),'j60-mix':(1.5,2.5)}[key]
          ref=x[round(region[0]*sr):round(region[1]*sr)]
          f,q=estimate_pitch(ref,sr);target=spectrum(ref,sr,f);shape=key.split('-')[-1]
          row=dict(reference=key,url=final,sha256=sha(raw),git_blob=gitblob(raw),window_seconds=region,
            frequency_hz=f,periodicity=q,kind='Fixed waveform selection; no preset fitting',versions={})
          for version in ('baseline','coherent'):
            p=patch('juno60',bases['juno60'],shape,f)
            y=trial(exes['juno60',version],p,sr,2*sr,score,audio);got=spectrum(y[sr:],sr,f)
            row['versions'][version]=dict(loss_db=loss(got,target),harmonic_db=got.tolist(),controls=p,
              output_sha256=sha(y.astype('<f4').tobytes()),raw_rms=float(np.sqrt(np.mean(y[sr:]**2))))
            note=y[sr:].copy();n=round(.005*sr);note[:n]*=np.linspace(0,1,n);note[-n:]*=np.linspace(1,0,n)
            gain=float(np.sqrt(np.mean(ref*ref))/max(np.sqrt(np.mean(note*note)),1e-20))
            if np.max(abs(note*gain))>.98:gain=.98/np.max(abs(note))
            L.wav(key+'-'+version+'-raw.wav',note,sr);L.wav(key+'-'+version+'-level-matched.wav',note*gain,sr)
            row['versions'][version]['listening_gain_db']=20*np.log10(gain)
          row['target_harmonic_db']=target.tolist()
          row['coherent_minus_baseline_db']=row['versions']['coherent']['loss_db']-row['versions']['baseline']['loss_db']
          L.report['hardware_comparisons'].append(row);print('HARDWARE',json.dumps(row),flush=True)
        # Use the last held note, not a window spanning the sequenced opening.
        raw,_=fetch(J106_URL)
        if sha(raw)!=J106_SHA:raise ValueError('Juno106 archive mismatch')
        z=zipfile.ZipFile(io.BytesIO(raw));file='JUNO2-4.WAV';raw=z.read(file);x,sr=decode(raw)
        ref=x[round(8.0*sr):round(9.0*sr)];hold=x[round(10.0*sr):round(11.0*sr)]
        f,q=estimate_pitch(ref,sr);target=spectrum(ref,sr,f);target_hold=spectrum(hold,sr,f)
        row=dict(reference='j106-dry-held',url=J106_URL,archive_sha256=J106_SHA,file=file,
            sha256=sha(raw),window_seconds=[8,9],holdout_seconds=[10,11],frequency_hz=f,
            kind='Diagnostic preset fit; known no chorus, unknown original panel controls',versions={})
        for version in ('baseline','coherent'):
          p=patch('juno106',bases['juno106'],'mix',f);trials=[]
          def objective(theta):
            q=p|dict(cutoff=2**theta[0],resonance=theta[1],saw=1-theta[2],pulse=theta[2],pwm=theta[3])
            y=trial(exes['juno106',version],q,sr,2*sr,score,audio);value=loss(spectrum(y[sr:],sr,f),target)
            trials.append(dict(controls=q,loss_db=value,sha256=sha(y.astype('<f4').tobytes())))
            return value
          differential_evolution(objective,[(np.log2(300),np.log2(16000)),(0,.9),(0,1),(.15,.85)],seed=106,popsize=5,maxiter=10,polish=False,workers=1)
          best=min(trials,key=lambda a:a['loss_db']);q=best['controls']
          y=trial(exes['juno106',version],q,sr,2*sr,score,audio)[sr:]
          gain=float(np.sqrt(np.mean(ref*ref))/max(np.sqrt(np.mean(y*y)),1e-20));gain=min(gain,.98/max(np.max(abs(y)),1e-20))
          L.wav('j106-dry-'+version+'-raw.wav',y,sr);L.wav('j106-dry-'+version+'-level-matched.wav',y*gain,sr)
          row['versions'][version]=best|dict(holdout_loss_db=loss(spectrum(y,sr,f),target_hold),trials=len(trials),listening_gain_db=20*np.log10(gain))
          (out/('j106-'+version+'-fit.json')).write_text(json.dumps(trials,indent=2))
        L.wav('j106-dry-hardware-excerpt.wav',ref,sr)
        L.report['hardware_comparisons'].append(row);print('J106_FIT',json.dumps(row),flush=True)
    except Exception as exc:
      L.report['exception']=traceback.format_exc();c('assessment-completed',False);print(L.report['exception'],flush=True)
      if getattr(exc,'output',None):
        L.report['subprocess_output']=str(exc.output);print(L.report['subprocess_output'],flush=True)
    L.report['passed']=bool(L.report['checks']) and all(t.get('passed') is True for t in L.report['checks'])
    L.report['failures']=[t['name'] for t in L.report['checks'] if t.get('passed') is not True]
    (out/'results.json').write_text(json.dumps(L.report,indent=2))
    print('FINISH_ASSESSMENT',json.dumps(dict(passed=L.report['passed'],failures=L.report['failures'],hardware_comparisons=len(L.report['hardware_comparisons']))),flush=True)
    return 0 if L.report['passed'] else 1

if __name__=='__main__':
    ap=argparse.ArgumentParser();ap.add_argument('--out',type=Path,required=True)
    args=ap.parse_args();raise SystemExit(run(args.out))
