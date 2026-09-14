"""Bounded physical SH-101 and Model-D reference-note comparisons.
Patch/timbre probes, not exact panel/envelope calibration. References without
redistribution permission stay transient and are linked, not repackaged.
"""
from pathlib import Path
import argparse, json, os, tempfile, traceback, urllib.parse, zipfile, io
import numpy as np
from scipy.optimize import differential_evolution
from synth_batch import SynthLab, ROOT, DEFAULTS
from synth_finish import patch, trial, decode
from synth_finish_materials import fetch, sha
from synth_sample_probe import candidate_windows, spectrum
from synth_extra_materials import mini_download, MINI_PAGE

MINI_SHA='3d84afabb6a23743b795a4c5d48d38138c3924e958af762960953cdf417a4920'
MINI_FILES={
 'mini-saw-low':('SYNTH-SimpleWave2.wav','91444b47b76e49febd06ef7fc761387637e5e228753ed1dd8b88b77e6fe5f209','saw'),
 'mini-saw-mid':('SYNTH-SimpleWave1.wav','cb517e772704c8fe55db95fa6498f690c3911a3af3e6b0c011c83775214ab7a6','saw'),
 'mini-square-bass':('BASS-MicroFriebel3.wav',None,'square')}
SH_BASE='https://synthmania.com/Roland%20SH-101/Audio/Factory%20Patches/'
SH_FILES={'sh101-clarinet':'02 CLARINET.mp3','sh101-trumpet':'04 TRUMPET.mp3','sh101-bass':'08 ELECTRIC BASS GUITAR.mp3'}

def shape_loss(actual,target):
    a=np.asarray(actual,float);b=np.asarray(target,float)
    if a.shape!=(24,) or b.shape!=(24,) or not np.isfinite(a).all() or not np.isfinite(b).all():
        raise ValueError('invalid harmonic descriptor')
    # Include absent/weak target harmonics so excess partials are not ignored.
    return float(np.sqrt(np.mean((np.maximum(a,-45)-np.maximum(b,-45))**2)))

def run(out):
    L=SynthLab(out);(out/'audition').mkdir(exist_ok=True);c=L.check
    L.report.update(commit=os.getenv('GITHUB_SHA','local'),reference_results=[],acquisition={},
        hardware_approved=False,owner_approved=False,instrument_freeze_complete=False,
        metric='Normalized 24-harmonic log-spectrum RMS difference, -45dB floor; not authenticity',
        scope='Real hardware stationary-note timbre probes, not full synth or panel calibration')
    try:
        references=[]
        raw=mini_download()
        if sha(raw)!=MINI_SHA:raise ValueError('Model D archive changed')
        z=zipfile.ZipFile(io.BytesIO(raw))
        L.report['acquisition']['mini']=dict(url=MINI_PAGE,sha256=sha(raw),wave_count=sum(n.lower().endswith('.wav') for n in z.namelist()),
            capture_notes=z.read('LegoweltMiniMoogSamplePackInfo.txt').decode('utf-8','replace'))
        for tag,(file,expected,wave) in MINI_FILES.items():
            b=z.read(file)
            if expected and sha(b)!=expected:raise ValueError('Model D sample changed')
            references.append((tag,'mini',wave,b,dict(url=MINI_PAGE,archive_sha256=MINI_SHA,file=file)))
        for tag,file in SH_FILES.items():
            url=SH_BASE+urllib.parse.quote(file)
            try:
                b,final=fetch(url,limit=30_000_000)
                references.append((tag,'sh101','fit',b,dict(url=final,file=file,
                    note='Producer-recorded physical SH-101 factory patch, MP3; exact recorded knob settings/recording chain unknown')))
                L.report['acquisition'][tag]=dict(url=final,sha256=sha(b))
            except Exception as e:L.report['acquisition'][tag]=dict(url=url,error=str(e))
        exes={}
        for instrument,versions in [('mini',('minimoog/v1','minimoog/v3')),('sh101',('mono-101/v3','mono-101/v4'))]:
            for label,path in zip(('baseline','candidate'),versions):exes[instrument,label]=L.build(instrument+'-'+label,ROOT/'modules'/path/'voice.dsp')
        with tempfile.TemporaryDirectory(prefix='hardware-note-trials-') as temp:
            temp=Path(temp);score=temp/'score.tsv';audio=temp/'audio.f32'
            for tag,instrument,wave,raw,info in references:
                x,sr=decode(raw);wins=candidate_windows(raw)
                if not wins:
                    L.report['acquisition'][tag]=info|dict(error='No eligible stable note window');continue
                _,a,b,f,q,*_=wins[0];ref=x[a:b];target=spectrum(ref,sr,f)
                second=next((w for w in wins[1:] if abs(np.log2(w[3]/f))<.01),None)
                hold=None if second is None else spectrum(x[second[1]:second[2]],sr,f)
                row=dict(id=tag,instrument=instrument,reference=info|dict(sha256=sha(raw),sample_rate=sr,
                    window_seconds=[a/sr,b/sr],frequency_hz=f,periodicity=q),versions={},target_harmonic_db=target.tolist())
                if second:row['reference']['holdout_seconds']=[second[1]/sr,second[2]/sr]
                if hold is not None:row['reference']['temporal_shape_change_db']=shape_loss(hold,target)
                for label in ('baseline','candidate'):
                    if instrument=='mini':
                        p=DEFAULTS['mini']|dict(gate=0,freq=f,velocity=1,osc1=int(wave=='saw'),osc2=0,osc3=int(wave=='square'),detune3=0,
                            contour=0,noise=0,attack=.003,sustain=1,release=.1,drive=0,emphasis=.707)
                        bounds=[(np.log2(max(40,f*.5)),np.log2(16000)),(.707,8),(0,.5)]
                        def map_params(t):return p|dict(cutoff=2**t[0],emphasis=t[1],drive=t[2])
                    else:
                        p=patch('sh101',DEFAULTS['mono101'],'mix',f)
                        bounds=[(np.log2(max(40,f*.5)),np.log2(16000)),(0,.9),(0,1),(.1,.9)]
                        def map_params(t):return p|dict(cutoff=2**t[0],resonance=t[1],saw=1-t[2],pulse=t[2],pwm=t[3])
                    trials=[]
                    def objective(theta):
                        control=map_params(theta)
                        y=trial(exes[instrument,label],control,sr,round(1.4*sr),score,audio)
                        features=spectrum(y[-len(ref):],sr,f);value=shape_loss(features,target)
                        trials.append(dict(controls=control,loss_db=value,output_sha256=sha(y.astype('<f4').tobytes())))
                        return value
                    differential_evolution(objective,bounds,seed=10601,popsize=5,maxiter=8,polish=False,workers=1)
                    best=min(trials,key=lambda r:r['loss_db'])
                    y=trial(exes[instrument,label],best['controls'],sr,3*sr,score,audio)
                    note=y[sr:];gain=float(np.sqrt(np.mean(ref*ref))/max(np.sqrt(np.mean(note*note)),1e-20));gain=min(gain,.98/max(np.max(abs(note)),1e-20))
                    # Two-second audible sustain; diagnostic window is explicitly shorter.
                    L.wav(tag+'-'+label+'-raw.wav',note,sr)
                    L.wav(tag+'-'+label+'-level-matched.wav',note*gain,sr)
                    features=spectrum(y[-len(ref):],sr,f)
                    row['versions'][label]=best|dict(trial_count=len(trials),holdout_loss_db=None if hold is None else shape_loss(features,hold),
                        listening_gain_db=20*np.log10(gain),listening_duration_seconds=2,raw_rms=float(np.sqrt(np.mean(note*note))))
                    (out/(tag+'-'+label+'-trials.json')).write_text(json.dumps(trials,indent=2))
                row['candidate_minus_baseline_db']=row['versions']['candidate']['loss_db']-row['versions']['baseline']['loss_db']
                L.report['reference_results'].append(row);print('HARDWARE_NOTE',json.dumps(row),flush=True)
        represented={r['id'] for r in L.report['reference_results']}
        c('all-six-hardware-note-targets',represented==set(MINI_FILES)|set(SH_FILES),actual=sorted(represented))
    except Exception as e:
        L.report['exception']=traceback.format_exc();c('hardware-cases-completed',False)
        print(L.report['exception'],flush=True)
        if getattr(e,'output',None):print(e.output,flush=True)
    L.report['passed']=bool(L.report['checks']) and all(r.get('passed') is True for r in L.report['checks'])
    (out/'results.json').write_text(json.dumps(L.report,indent=2))
    print('HARDWARE_CASES_SUMMARY',json.dumps(dict(passed=L.report['passed'],targets=len(L.report['reference_results']),acquisition=L.report['acquisition'])),flush=True)
    return 0 if L.report['passed'] else 1

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--out',type=Path,required=True)
    a=p.parse_args();raise SystemExit(run(a.out))
