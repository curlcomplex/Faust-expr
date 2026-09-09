"""Full offline synthetic-snare qualification and broad licensed-reference coverage.
Reference settings are unknown: descriptor fitting is architectural evidence, not a clone score.
"""
from __future__ import annotations
import argparse, hashlib, itertools, json, math, os, platform, random, shutil, statistics, subprocess, wave
from pathlib import Path
import numpy as np
from scipy.io import wavfile
ROOT=Path(__file__).resolve().parents[2];MOD=ROOT/'modules/snare-pm'
def sha(p): return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def cmd(a,timeout=180):
    p=subprocess.run(list(map(str,a)),cwd=ROOT,capture_output=True,text=True,timeout=timeout)
    if p.returncode: raise RuntimeError(f'{a}\n{p.stdout}\n{p.stderr}')
    return p.stdout
def write_wav(path,x,rate=48000):
    x=np.asarray(x)
    if not np.isfinite(x).all() or np.max(np.abs(x))>=1: raise ValueError('clip/nonfinite')
    with wave.open(str(path),'wb') as f:f.setnchannels(1);f.setsampwidth(2);f.setframerate(rate);f.writeframes(np.rint(x*32767).astype('<i2').tobytes())
def descriptor(x,rate=48000):
    x=np.asarray(x,dtype=float).reshape(-1);x=x[:min(len(x),int(rate*1.5))]
    if not len(x) or not np.isfinite(x).all(): raise ValueError('audio')
    peak=max(float(np.max(np.abs(x))),1e-12);energy=x*x;cum=np.cumsum(energy)/max(float(energy.sum()),1e-30)
    n=max(4096,1<<(len(x)-1).bit_length());spec=np.abs(np.fft.rfft(x*np.hanning(len(x)),n=n))**2+1e-18;freq=np.fft.rfftfreq(n,1/rate);p=spec/spec.sum()
    centroid=float((freq*p).sum());flat=float(np.exp(np.mean(np.log(spec)))/np.mean(spec));zcr=float(np.mean(np.signbit(x[1:])!=np.signbit(x[:-1]))) if len(x)>1 else 0
    bands=[]
    for lo,hi in ((40,400),(400,2000),(2000,8000),(8000,20000)): bands.append(float(p[(freq>=lo)&(freq<hi)].sum()))
    return np.array([np.searchsorted(cum,.5)/rate,np.searchsorted(cum,.9)/rate,math.log10(max(centroid,1)),math.log10(max(flat,1e-12)),zcr,*bands],float)
def validate(values,manifest):
    for k,v in values.items():
        if k not in manifest['controls'] or isinstance(v,bool) or not isinstance(v,(int,float)) or not math.isfinite(v): raise ValueError(k)
        c=manifest['controls'][k]
        if not c['min']<=v<=c['max'] or (k=='gate' and v not in (0,1)): raise ValueError(k)
class Study:
    def __init__(self,out,refs):
        self.out=out;out.mkdir(parents=True,exist_ok=True);self.refs=refs
        self.man=json.loads((MOD/'manifest.json').read_text());self.defaults={k:v['default'] for k,v in self.man['controls'].items()}
        self.patches=json.loads((MOD/'patches.json').read_text())['anchors'];self.report={'schema':1,'checks':[],'renders':[],'builds':{},'passed':False,'hardware_clone_claim':False,'human_approved':False,'device_qualified':False,'platform':platform.platform()}
    def check(self,name,ok,**d):
        self.report['checks'].append(dict(name=name,passed=bool(ok),**d))
        if not ok: raise AssertionError(name+str(d))
    def build(self,label,source,vector=False):
        d=self.out/label;d.mkdir(exist_ok=True);flags=['-lang','cpp','-single','-cn','ModuleDSP']+(['-vec','-lv','0','-vs','32'] if vector else [])
        cmd([os.getenv('FAUST','faust'),'-I',MOD,*flags,MOD/source,'-o',d/'generated.hpp']);cflags=['-std=c++17','-O2','-ffp-contract=off','-I'+str(d)]
        cmd([os.getenv('CXX','c++'),*cflags,ROOT/'tools/modules/render.cpp','-o',d/'render']);controls=cmd([d/'render','--controls'])
        self.check(label+':controls',{r.split('\t')[0] for r in controls.splitlines()[1:]}==set(self.defaults));self.check(label+':mono',controls.splitlines()[0]=='io\t0\t1')
        self.report['builds'][label]={'source':source,'source_sha256':sha(MOD/source),'engine_sha256':sha(MOD/'engine.lib'),'generated_sha256':sha(d/'generated.hpp'),'flags':flags};return d/'render'
    def render(self,label,exe,params=None,events=None,rate=48000,block=128,seconds=1.2):
        values=self.defaults|(params or {});validate(values,self.man);rows={(0,k):v for k,v in values.items()}
        for n,k,v in events or []: validate({k:v},self.man);rows[n,k]=v
        score=self.out/(label+'.tsv');raw=self.out/(label+'.f32');score.write_text(''.join(f'{n}\t{k}\t{v:.9g}\n' for (n,k),v in sorted(rows.items())))
        frames=round(seconds*rate);diag=json.loads(cmd([exe,score,raw,rate,block,frames,0]));x=np.fromfile(raw,dtype='<f4')
        self.check(label+':finite-headroom',len(x)==frames and np.isfinite(x).all() and np.max(np.abs(x))<.92,peak=float(np.max(np.abs(x))))
        self.report['renders'].append({'label':label,'build':exe.parent.name,'rate':rate,'block':block,'score_sha256':sha(score),'raw_sha256':sha(raw),'peak':float(np.max(np.abs(x))),'rms':float(np.sqrt(np.mean(x.astype(float)**2))),'mean':float(np.mean(x)),'max_jump':float(np.max(np.abs(np.diff(x)))),'diag':diag});return x
    @staticmethod
    def hit(n,length=64): return [(n,'gate',1),(n+length,'gate',0)]
    def refs_desc(self):
        out={}
        for p in sorted(self.refs.glob('SD*.wav')):
            rate,x=wavfile.read(p);x=np.asarray(x,dtype=float);x=x.mean(axis=1) if x.ndim>1 else x
            out[p.stem]=descriptor(x,rate)
        if len(out)!=8: raise ValueError('expected eight licensed refs')
        return out
    def execute(self):
        det=self.build('deterministic','deterministic.dsp');hyb=self.build('hybrid','hybrid.dsp');vec=self.build('hybrid-vector','hybrid.dsp',True)
        self.report['compilers']={'faust':cmd([os.getenv('FAUST','faust'),'-v']).strip(),'cxx':cmd([os.getenv('CXX','c++'),'--version']).strip()}
        self.check('initial-silence',not np.any(self.render('silence',hyb,seconds=.1)))
        for rate in (44100,48000,96000):
            on=round(.05*rate)
            for hz in (80,185,420):
                x=self.render(f'pitch-{rate}-{hz}',hyb,{'pitch_hz':hz,'sweep':0,'shape':0,'inharm':0,'punch':0,'drive':0,'decay':.9},self.hit(on),rate=rate,seconds=1.1)
                # dominant late body frequency; wide search prevents expected-pitch tunnel vision
                y=x[round(.25*rate):round(.85*rate)].astype(float);n=1<<19;s=np.abs(np.fft.rfft((y-y.mean())*np.hanning(len(y)),n=n));freq=np.fft.rfftfreq(n,1/rate);mask=(freq>40)&(freq<1000);m=float(freq[mask][np.argmax(s[mask])])
                self.check(f'pitch:{rate}:{hz}',abs(m-hz)<1.2,measured=m)
        events=self.hit(101)+[(8011,'inharm',.95),(8011,'shape',.9),(8011,'contour',.9),(8011,'drive',.8)]+self.hit(8011)+[(17003,'pitch_hz',410),(17003,'sweep',.8)]+self.hit(17003)
        base=self.render('dynamic-128',hyb,events=events)
        for block in (1,32,64,127,512): self.check('segmentation:'+str(block),np.array_equal(base,self.render('dynamic-'+str(block),hyb,events=events,block=block)))
        v=self.render('dynamic-vector',vec,events=events);self.check('vector-parity',np.max(np.abs(base-v))<3e-4,max_abs=float(np.max(np.abs(base-v))))
        a=self.render('gate-pulse',hyb,events=self.hit(101,1));b=self.render('gate-held',hyb,events=[(101,'gate',1)]);self.check('gateoff-independent',np.array_equal(a,b))
        h=self.render('velocity-half',hyb,{'velocity':.5},self.hit(101,1));self.check('velocity-linear',np.max(np.abs(h-a*.5))<2e-6)
        changed=self.render('latched-tail',hyb,events=self.hit(101,1)+[(3001,'decay',1),(3001,'inharm',1),(3001,'shape',0),(3001,'contour',1),(3001,'drive',1),(3001,'pitch_hz',500)])
        # All controls are latched in this version, including drive/pitch; the tail must be identical.
        self.check('tail-controls-latched',np.array_equal(a,changed))
        keys=['sweep','punch','decay','inharm','shape','contour','drive'];ev=[];idx=0
        for bits in itertools.product((0.,1.),repeat=7):
            n=101+idx*1536;idx+=1;ev += [(n,k,x) for k,x in zip(keys,bits)]+[(n,'pitch_hz',80 if idx%2 else 420)]+self.hit(n,96)
        x=self.render('endpoints',hyb,events=ev,seconds=(101+idx*1536+24000)/48000,block=127);self.check('endpoint-dc',abs(float(np.mean(x)))<.02,settings=idx,mean=float(np.mean(x)))
        ev=[]
        for i in range(96):
            n=101+i*512;ev += [(n,'inharm',(i%13)/12),(n,'shape',(i%17)/16),(n,'contour',(i%11)/10),(n,'drive',(i%7)/6)]+self.hit(n,48)
        self.render('rapid-locks',hyb,events=ev,seconds=1.2,block=32)
        # Architecture coverage: fixed pseudo-random parameter set, both candidates, no fit to individual refs.
        refs=self.refs_desc();rng=random.Random(16092026);params=[]
        for i in range(48):
            params.append({'pitch_hz':60+440*rng.random(),'sweep':rng.random(),'punch':rng.random(),'decay':rng.random(),'inharm':rng.random(),'shape':rng.random(),'contour':rng.random(),'drive':.55*rng.random()})
        pools={}
        for label,exe in [('deterministic',det),('hybrid',hyb)]:
            desc=[]
            for i,p in enumerate(params): desc.append(descriptor(self.render(f'coverage-{label}-{i}',exe,p,self.hit(101),seconds=1.05)))
            pools[label]=np.vstack(desc)
        R=np.vstack(list(refs.values()));allD=np.vstack([R,pools['deterministic'],pools['hybrid']]);mu=allD.mean(0);sd=np.maximum(allD.std(0),1e-8);Rz=(R-mu)/sd
        coverage={}
        for label,P in pools.items():
            Pz=(P-mu)/sd;dist=np.sqrt(((Rz[:,None,:]-Pz[None,:,:])**2).mean(axis=2));coverage[label]={'per_reference':{name:float(dist[i].min()) for i,name in enumerate(refs)},'median_nearest':float(np.median(dist.min(axis=1))),'mean_nearest':float(np.mean(dist.min(axis=1)))}
        self.report['licensed_reference_descriptor_coverage']=coverage
        self.report['reference_limit']='Unknown settings/gain/lossy previews. Distances are z-scored broad descriptors over a fixed authored search pool; not fidelity percentages or macro identification.'
        # Auditions: selected hybrid candidate, fixed gain.
        ev=[];timeline=[]
        for i,(name,p) in enumerate(self.patches.items()):
            n=round((i*2+.05)*48000);ev += [(n,k,v) for k,v in p.items()]+[(n,'velocity',1)]+self.hit(n);n2=n+36000;ev += [(n2,'velocity',.55)]+self.hit(n2);timeline.append({'start_s':n/48000,'patch':name,'soft_s':n2/48000})
        x=self.render('anchors',hyb,events=ev,seconds=16.2);write_wav(self.out/'snare-anchors.wav',x);self.report['anchors']=timeline
        ev=[]
        for i in range(64):
            if i%4==0 or i in (7,15,22,23,30,31,39,46,47,54,55,62,63):
                n=2400+i*6000;p=list(self.patches.values())[(i//10)%len(self.patches)];ev += [(n,k,v) for k,v in p.items()]+[(n,'velocity',1 if i%4==0 else .45)]+self.hit(n)
        x=self.render('pattern',hyb,events=ev,seconds=9);write_wav(self.out/'snare-pattern.wav',x)
        ev=[];order=self.man['musical_control_order']
        for j,key in enumerate(order):
            for i in range(8):
                n=2400+j*144000+i*18000;p=self.defaults.copy();p.update({'pitch_hz':185,'inharm':.5,'shape':.55,'contour':.5});p[key]=60*8.333333**(i/7) if key=='pitch_hz' else i/7;p.pop('gate');ev += [(n,k,v) for k,v in p.items()]+self.hit(n)
        x=self.render('controls',hyb,events=ev,seconds=25);write_wav(self.out/'snare-controls.wav',x);self.report['control_order']=order
        # Same-source scalar/vector timing; don't assume vector wins.
        perf=[]
        for block in (32,64,128,512):
            # use render diagnostic compute times across a 4-hit score instead of a second handwritten benchmark
            ev=self.hit(101)+self.hit(3001)+self.hit(6103)+self.hit(9107)
            s=[];q=[]
            for rep in range(5):
                xs=self.render(f'perf-s-{block}-{rep}',hyb,events=ev,block=block,seconds=.3);s.append(self.report['renders'][-1]['diag']['instrumented_compute_ns'])
                xv=self.render(f'perf-v-{block}-{rep}',vec,events=ev,block=block,seconds=.3);q.append(self.report['renders'][-1]['diag']['instrumented_compute_ns']);self.check(f'perf-audio:{block}:{rep}',np.max(np.abs(xs-xv))<3e-4)
            perf.append({'block':block,'scalar_median_ns':statistics.median(s),'vector_median_ns':statistics.median(q),'scalar_over_vector':statistics.median(s)/statistics.median(q)})
        self.report['performance']=perf;self.report['performance_scope']='Offline single-instance renderer instrumentation on hosted runner; not device callback/thermal acceptance.'
        self.report['passed']=True
    def save(self,error=None):
        if error:self.report['passed']=False
        self.report['failure']=error;self.report['source_files']={}
        for p in list(MOD.glob('*'))+[ROOT/'tools/modules/snare_batch.py',ROOT/'tools/modules/fetch_snare_references.py']:
            if p.is_file(): self.report['source_files'][str(p.relative_to(ROOT))]=sha(p)
        try:self.report['source_commit']=cmd(['git','rev-parse','HEAD']).strip()
        except Exception:self.report['source_commit']=None
        (self.out/'report.json').write_text(json.dumps(self.report,indent=2)+'\n');print(json.dumps({'passed':self.report['passed'],'checks':len(self.report['checks']),'renders':len(self.report['renders']),'failure':error}))
def main():
    a=argparse.ArgumentParser();a.add_argument('--out',type=Path,required=True);a.add_argument('--references',type=Path,required=True);args=a.parse_args();s=Study(args.out.resolve(),args.references.resolve());err=None
    try:s.execute()
    except Exception as e:err=str(e)
    finally:s.save(err)
    if err:raise SystemExit(err)
if __name__=='__main__':main()
