"""Four single-note drums; actual Faust/C++ using the existing renderer, no Python synthesis."""
from pathlib import Path
import argparse, hashlib, itertools, json, os, subprocess, urllib.request
import numpy as np
from scipy.io import wavfile
from scipy.signal import resample_poly, welch
from scipy.spatial.distance import jensenshannon
from hats_v2_delivery import Lab, command, digest
ROOT=Path(__file__).resolve().parents[2]
SRC=ROOT/'modules/analog-classics/drums-v2'
DEFAULTS={
 'kick808':dict(tone=.32,decay=.62,punch=.58,click=.16,drive=.08,freq=52.,velocity=1.,gate=0.),
 'snare808':dict(tone=.48,snappy=.62,decay=.42,noise_color=.56,drive=.05,freq=180.,velocity=1.,gate=0.),
 'clap808':dict(spacing=.40,tone=.48,snap=.62,decay=.32,tail=.35,drive=.08,freq=1500.,velocity=1.,gate=0.),
 'cymbal808':dict(metal=.96,tone=.52,decay=.68,shape=.28,drive=.05,freq=440.,velocity=1.,gate=0.),
}
PRESETS={
 'kick808':{'Classic':{},'Tight':{'decay':.18,'punch':.35},'Deep':{'freq':36,'decay':.82,'punch':.20},'Driven':{'freq':65,'punch':.9,'drive':.65}},
 'snare808':{'Classic':{},'Tight':{'decay':.15,'snappy':.72},'Body':{'snappy':.25,'freq':155},'Bright':{'noise_color':.82,'snappy':.85,'decay':.6}},
 'clap808':{'Classic':{},'Tight':{'spacing':.22,'decay':.08,'tail':.15},'Wide':{'spacing':.72,'tail':.55},'Dark':{'freq':900,'tone':.35,'decay':.6}},
 'cymbal808':{'Classic':{},'Short':{'decay':.15},'Dark':{'tone':.12,'freq':320,'decay':.58},'Long':{'decay':.9,'shape':.48}},
}
REFNAMES={'kick808':'Bass Drum','snare808':'Snare Drum','clap808':'Hand Clap','cymbal808':'Cymbal'}
REFROOT='https://www.synthmania.com/Roland%20TR-808/Audio/Instrument%20samples/TR-808%20'
PINNED={}
def features(x,sr=48000):
    x=np.asarray(x,float);f,p=welch(x,sr,nperseg=min(2048,len(x)))
    edges=[0,80,160,350,700,1500,3000,6000,12000,24001]
    band=np.array([p[(f>=a)&(f<b)].sum() for a,b in zip(edges,edges[1:])]);band/=band.sum()+1e-30
    e=np.cumsum(x*x);e/=e[-1]+1e-30
    return dict(peak=float(abs(x).max()),rms=float(np.sqrt(np.mean(x*x))),mean=float(x.mean()),centroid_hz=float(np.sum(f*p)/(p.sum()+1e-30)),t90_ms=float(np.searchsorted(e,.9)*1000/sr),bands=band.tolist())
class DrumLab(Lab):
    # Replace only hat-specific score defaults; preserve the existing compiler and renderer.
    def render(self,name,exe,values,events=None,sr=48000,block=128,seconds=2.):
        events=[(round(.01*sr),'gate',1),(round(.01*sr)+1,'gate',0)] if events is None else events
        rows={(0,k):float(v) for k,v in values.items()};seen=set()
        for n,k,v in events:
            if (n,k) in seen:raise ValueError('duplicate event')
            seen.add((n,k));rows[n,k]=float(v)
        score=self.out/(name+'.tsv');raw=self.out/(name+'.f32')
        score.write_text(''.join(f'{n}\t{k}\t{v:.9g}\n' for (n,k),v in sorted(rows.items())))
        diag=json.loads(command([exe,score,raw,sr,block,round(seconds*sr),0]))
        x=np.fromfile(raw,'<f4').reshape(-1,diag['channels'])
        if x.shape[1]==1:x=x[:,0]
        self.check(name+':finite',len(x)==round(seconds*sr) and np.isfinite(x).all())
        self.report['renders'].append(dict(name=name,rate=sr,block=block,raw_sha256=digest(raw),score_sha256=digest(score),diagnostics=diag))
        return x
def ui(exe):
    lines=command([exe,'--controls']).splitlines()
    return tuple(map(int,lines[0].split('\t')[1:])),{s[0]:tuple(map(float,s[1:])) for s in (l.split('\t') for l in lines[1:])}
def references(L,rendered):
    dest=L.out/'references';dest.mkdir(exist_ok=True)
    report={'recordist':'Synthmania','page':'https://www.synthmania.com/tr-808.htm','instrument':'Roland TR-808 serial 209265','rights':'Creator permits download/edit/sample; only short comparison excerpts retained; no runtime samples.','limitations':'Knobs varied, settings/gain unknown. First usable detected hit, no best-match selection or held-out calibration.','comparisons':{},'errors':{}}
    audition=[];cues=[]
    for name,stem in REFNAMES.items():
        url=REFROOT+stem.replace(' ','%20')+'.wav'
        try:
            req=urllib.request.Request(url,headers={'User-Agent':'Mozilla/5.0','Accept':'*/*'})
            with urllib.request.urlopen(req,timeout=20) as resp:data=resp.read(40000001)
            if len(data)>40000000 or data[:4]!=b'RIFF':raise ValueError('invalid/oversize reference')
            h=hashlib.sha256(data).hexdigest()
            if name in PINNED and h!=PINNED[name]:raise ValueError('reference hash changed')
            import io
            sr,x=wavfile.read(io.BytesIO(data))
            if x.dtype!=np.int16:raise ValueError('expected PCM16')
            x=x.astype(float)/32768.;x=x.mean(axis=1) if x.ndim==2 else x
            hop=round(sr*.001);n=len(x)//hop
            energy=np.sqrt(np.mean(x[:n*hop].reshape(n,hop)**2,axis=1));mask=energy>max(.002,float(energy.max())*.055)
            starts=np.flatnonzero(mask & ~np.r_[False,mask[:-1]])
            if not len(starts):raise ValueError('no onset')
            start=max(0,int(starts[0]*hop)-hop);nexts=[int(i*hop) for i in starts if i*hop>start+.22*sr]
            end=min(len(x),start+round(1.5*sr))
            for n0 in nexts:
                lo=max(start,n0-round(.025*sr))
                if np.sqrt(np.mean(x[lo:n0]**2))<max(.002,energy.max()*.025):end=min(end,n0-hop);break
            clip=resample_poly(x[start:end],48000,sr)
            if len(clip)<4800:raise ValueError('short excerpt')
            synth=rendered[name][480:480+len(clip)];clip=clip[:len(synth)]
            rf,sf=features(clip),features(synth);gains=[.07/(rf['rms']+1e-20),.07/(sf['rms']+1e-20)]
            pair=[clip*gains[0],synth*gains[1]];headroom=min(1.,.85/max(abs(a).max() for a in pair))
            report['comparisons'][name]={'url':url,'original_sha256':h,'source_rate':sr,'start_sample':start,'end_sample':end,'reference':rf,'candidate':sf,'band_JS_distance':float(jensenshannon(rf['bands'],sf['bands'])),'audition_gains':gains,'pair_headroom_gain':headroom}
            wavfile.write(dest/(name+'-excerpt.wav'),48000,clip.astype(np.float32))
            for label,a in zip(('reference','candidate'),pair):
                at=sum(len(k) for k in audition)/48000;slot=np.zeros(96000,np.float32);a=a*headroom;a[-240:]*=np.linspace(1,0,240)
                slot[:len(a)]=a;cues.append(dict(at_seconds=at,instrument=name,sound=label));audition.append(slot)
        except Exception as e:report['errors'][name]=repr(e)
    if audition:wavfile.write(L.out/'audition/02_reference_then_candidate.wav',48000,np.concatenate(audition))
    report['cues']=cues;report['complete']=len(report['comparisons'])==4
    (L.out/'reference_report.json').write_text(json.dumps(report,indent=2)+'\n');L.report['reference_comparison_complete']=report['complete']
def run(out):
    L=DrumLab(out);c=L.check;r=L.render
    L.report.update(version='analog-classics-drums-0.2.0-experiment',presets=PRESETS,human_approved=False,host_integrated=False,hardware_approved=False,commit=os.environ.get('GITHUB_SHA','local-snapshot'),note='standalone qualification; no clone or real-time performance claim')
    (L.out/'audition').mkdir(exist_ok=True)
    try:
        exes={};rendered={}
        for name,d in DEFAULTS.items():
            exe=L.build(name+'-scalar',SRC/(name+'.dsp'));exes[name]=exe;vec=L.build(name+'-vector',SRC/(name+'.dsp'),True)
            io,params=ui(exe)
            c(name+':io',io==(0,1));c(name+':controls',set(params)==set(d));c(name+':defaults',all(abs(params[k][2]-v)<1e-5 for k,v in d.items()))
            x=r(name+'-default',exe,d,seconds=4);rendered[name]=x
            c(name+':audible',.005<float(abs(x).max())<1,peak=float(abs(x).max()));c(name+':before-onset-silent',not np.any(x[:480]))
            c(name+':never-silent',not np.any(r(name+'-never',exe,d,events=[])));c(name+':zero-velocity',not np.any(r(name+'-zero',exe,d|{'velocity':0})))
            c(name+':half-velocity',abs(r(name+'-half',exe,d|{'velocity':.5},seconds=4)-x*.5).max()<2e-6)
            c(name+':held-gate',np.array_equal(r(name+'-held',exe,d,events=[(480,'gate',1)],seconds=4),x))
            locks=[(480,'gate',1),(481,'gate',0)]+[(5000,k,params[k][1]) for k in d if k not in ('gate',)]
            c(name+':onset-latched',np.array_equal(r(name+'-locks',exe,d,events=locks,seconds=4),x))
            for b in (1,32,64,127,256,512):c(name+':block-'+str(b),np.array_equal(r(name+'-block-'+str(b),exe,d,block=b),x[:96000]))
            c(name+':vector',abs(r(name+'-vec',vec,d)-x[:96000]).max()<3e-5)
            for sr in (44100,96000):r(name+'-rate-'+str(sr),exe,d,sr=sr)
            tail=r(name+'-long-tail',exe,d|{'decay':1},seconds=24);c(name+':tail-ends',abs(tail[-48000:]).max()<1e-8)
            steps=[(480+i*311+j,'gate',1-j) for i in range(128) for j in (0,1)];r(name+'-128-retriggers',exe,d,events=steps)
            corners=[];keys=[k for k in d if k not in ('gate','freq','velocity')]
            for i,bits in enumerate(itertools.product((0.,1.),repeat=len(keys))):
                n=480+i*2400;corners += [(n,k,v) for k,v in zip(keys,bits)]+[(n,'freq',params['freq'][i%2]),(n,'velocity',.25+.75*(i%2)),(n,'gate',1),(n+1,'gate',0)]
            edge=r(name+'-corners',exe,d,events=corners,seconds=5);c(name+':bounded-corners',float(abs(edge).max())<4,peak=float(abs(edge).max()))
            for key in keys+['freq']:
                a=r(name+'-'+key+'-min',exe,d|{key:params[key][0]});b=r(name+'-'+key+'-max',exe,d|{key:params[key][1]})
                c(name+':effective-'+key,float(np.linalg.norm(a-b)/(np.linalg.norm(a)+1e-20))>.005)
            bank=[]
            for preset,p in PRESETS[name].items():
                y=r(name+'-preset-'+preset,exe,d|p,seconds=3);wavfile.write(L.out/'audition'/f'{name}_{preset}.wav',48000,y.astype(np.float32));bank.append(y)
            wavfile.write(L.out/'audition'/(name+'_four_presets.wav'),48000,np.concatenate(bank))
        seq=L.build('trigger-seq',ROOT/'modules/trigger-seq/v1/trigger.dsp');hats=ROOT/'modules/hats-analog/single-note-v1'
        from analog_classics_slice import HAT,OPEN
        exes['closed']=L.build('closed-hat',hats/'closed.dsp');exes['open']=L.build('open-hat',hats/'open.dsp')
        patterns={'kick808':[0,6,8,14],'snare808':[4,12],'clap808':[12],'cymbal808':[0],'closed':[0,2,4,6,8,10,12,14],'open':[3,11]}
        clocks=[(480+i*6000+j,'clock',1 if j==0 else 0) for i in range(64) for j in (0,1)]
        sd=dict(run=1,length=16,clock=0,reset=0)|{f'step{i+1:02d}':0 for i in range(32)};gates={};lanes={}
        for name,pattern in patterns.items():
            p=sd|{f'step{i+1:02d}':1 for i in pattern}
            if name=='cymbal808':p=sd|{'length':32,'step01':1}
            g=r(name+'-sequence',seq,p,events=clocks,seconds=12)[:,0];gates[name]=np.flatnonzero(g>.5).tolist()
        for name in patterns:
            d=HAT if name=='closed' else OPEN if name=='open' else DEFAULTS[name]
            events=[(n+j,'gate',1-j) for n in gates[name] for j in (0,1)]
            for i,n in enumerate(gates[name]):events.append((n,'velocity',.6 if i%3==1 else .9))
            if name=='open':events += [(n+j,'chokeGate',1-j) for n in gates['closed'] for j in (0,1)]
            lanes[name]=r(name+'-persistent-pattern',exes[name],d,events=events,seconds=12);wavfile.write(L.out/'audition'/(name+'_stem.wav'),48000,lanes[name].astype(np.float32))
        mix=sum(lanes.values());gain=min(1.,.9/float(abs(mix).max()))
        wavfile.write(L.out/'audition/03_six_voice_pattern.wav',48000,(mix*gain).astype(np.float32))
        wavfile.write(L.out/'audition/01_four_new_instruments.wav',48000,np.concatenate([rendered[n] for n in DEFAULTS]))
        L.report['audition']={'groove_gain':gain,'raw_mix_peak':float(abs(mix).max()),'bpm':120,'routing':'Actual Faust sequencer outputs -> persistent independent Faust voices; offline, not CURLOP. No limiter/EQ/FX.','preset_gains':1}
        references(L,rendered)
        L.report['source_sha256']={str(p.relative_to(ROOT)):digest(p) for p in sorted(SRC.glob('*.dsp'))};L.report['driver_sha256']=digest(__file__);L.report['renderer_sha256']=digest(ROOT/'tools/modules/render.cpp')
        L.report['features']={n:features(x[480:]) for n,x in rendered.items()};L.report['passed']=all(t['passed'] for t in L.report['checks'])
    except Exception as e:L.report.update(passed=False,error=repr(e),compiler_output=getattr(e,'output',None));raise
    finally:
        (L.out/'results.json').write_text(json.dumps(L.report,indent=2)+'\n')
        print(json.dumps({'passed':L.report.get('passed',False),'checks':len(L.report['checks']),'renders':len(L.report['renders']),'error':L.report.get('error'),'compiler_output':L.report.get('compiler_output')}))
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--out',required=True);run(p.parse_args().out)
