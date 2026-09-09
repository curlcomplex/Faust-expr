"""Complete offline sharp-family prototype qualification and audible evidence.
No hardware recordings are used; no reference-fidelity or device claim follows.
FAUST generation runs on CI. --replay verifies and recompiles saved generated C++.
"""
from __future__ import annotations
import argparse, hashlib, itertools, json, math, os, platform, shutil, statistics, subprocess, sys, wave
from pathlib import Path
import numpy as np
from scipy.signal import resample_poly
ROOT=Path(__file__).resolve().parents[2]
MOD=ROOT/'modules/kick-analog/sharp-02'

def sha(p): return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def cmd(args, timeout=180):
    p=subprocess.run(list(map(str,args)),capture_output=True,text=True,cwd=ROOT,timeout=timeout)
    if p.returncode: raise RuntimeError(f'{args}\n{p.stdout}\n{p.stderr}')
    return p.stdout

def validate(parameters, manifest):
    for key,value in parameters.items():
        if key not in manifest['controls'] or not isinstance(value,(int,float)) or isinstance(value,bool) or not math.isfinite(value):
            raise ValueError('invalid control '+str(key))
        c=manifest['controls'][key]
        if not c['min']<=value<=c['max']: raise ValueError('out of range '+key)
        if key in ('gate','wave') and int(value)!=value: raise ValueError('categorical '+key)

def write_wav(path,x,rate=48000):
    x=np.asarray(x)
    if not np.isfinite(x).all() or np.abs(x).max()>=1: raise ValueError('no hidden clipping/normalization')
    with wave.open(str(path),'wb') as f:
        f.setnchannels(1);f.setsampwidth(2);f.setframerate(rate)
        f.writeframes(np.rint(x*32767).astype('<i2').tobytes())

def clean_pitch(x,rate):
    y=np.asarray(x,dtype=float); y-=y.mean()
    n=1<<max(19,(len(y)-1).bit_length())
    s=np.abs(np.fft.rfft(y*np.hanning(len(y)),n=n));s[0]=0
    k=int(s.argmax());z=np.log(np.maximum(s[max(0,k-1):k+2],1e-30))
    off=.5*(z[0]-z[2])/(z[0]-2*z[1]+z[2]) if len(z)==3 else 0
    return float((k+off)*rate/n)

class Study:
    def __init__(self,out,replay=None):
        self.out=out;out.mkdir(parents=True,exist_ok=True)
        self.replay=replay
        self.man=json.loads((MOD/'manifest.json').read_text())
        self.defaults={k:v['default'] for k,v in self.man['controls'].items()}
        self.patches=json.loads((MOD/'patches.json').read_text())['anchors']
        self.report={'schema':1,'checks':[],'renders':[],'builds':{},'auditions':{},'passed':False,
                     'hardware_reference_fit':False,'human_approved':False,'device_qualified':False,
                     'platform':platform.platform(),'precision':'float32; no fast-math; ffp-contract=off'}
    def check(self,name,ok,**d):
        self.report['checks'].append(dict(name=name,passed=bool(ok),**d))
        if not ok: raise AssertionError(name+': '+str(d))
    def build(self,label,source,vector=False):
        d=self.out/label;d.mkdir(exist_ok=True)
        flags=['-lang','cpp','-single','-cn','ModuleDSP']+(['-vec','-lv','0','-vs','32'] if vector else [])
        if self.replay:
            old=self.replay/label
            expected=json.loads((self.replay/'report.json').read_text())['builds'][label]
            if sha(old/'generated.hpp')!=expected['generated_sha256']: raise ValueError('generated header checksum')
            shutil.copyfile(old/'generated.hpp',d/'generated.hpp')
        else:
            cmd([os.getenv('FAUST','faust'),'-I',MOD,*flags,MOD/source,'-o',d/'generated.hpp'])
        cflags=['-std=c++17','-O2','-ffp-contract=off','-fstack-usage','-I'+str(d)]
        cmd([os.getenv('CXX','c++'),*cflags,ROOT/'tools/modules/render.cpp','-o',d/'render'])
        cmd([os.getenv('CXX','c++'),*cflags,ROOT/'tools/modules/sharp_benchmark.cpp','-o',d/'benchmark'])
        controls=cmd([d/'render','--controls']);(d/'controls.tsv').write_text(controls)
        self.check(label+':control-contract',{r.split('\t')[0] for r in controls.splitlines()[1:]}==set(self.defaults))
        self.check(label+':mono',controls.splitlines()[0]=='io\t0\t1')
        self.report['builds'][label]=dict(source=source,source_sha256=sha(MOD/source),engine_sha256=sha(MOD/'engine.lib'),
            generated_sha256=sha(d/'generated.hpp'),binary_sha256=sha(d/'render'),faust_flags=flags,cxx_flags=cflags)
        return d/'render'
    def render(self,label,exe,params=None,events=None,rate=48000,block=128,seconds=1.2):
        values=self.defaults|(params or {});validate(values,self.man)
        rows={(0,k):v for k,v in values.items()};used=set();ev=list(events or [])
        for n,k,v in ev:
            validate({k:v},self.man)
            if (n,k) in used: raise ValueError('duplicate score event')
            used.add((n,k));rows[n,k]=v
        score=self.out/(label+'.tsv');raw=self.out/(label+'.f32')
        score.write_text(''.join(f'{n}\t{k}\t{v:.9g}\n' for (n,k),v in sorted(rows.items())))
        frames=round(seconds*rate)
        diag=json.loads(cmd([exe,score,raw,rate,block,frames,0]))
        x=np.fromfile(raw,dtype='<f4')
        self.check(label+':finite-headroom',len(x)==frames and np.isfinite(x).all() and np.max(np.abs(x))<.85,
            peak=float(np.max(np.abs(x))))
        self.report['renders'].append(dict(label=label,build=exe.parent.name,rate=rate,block=block,frames=frames,
            score_sha256=sha(score),raw_sha256=sha(raw),peak=float(np.max(np.abs(x))),rms=float(np.sqrt(np.mean(x.astype(float)**2))),
            mean=float(np.mean(x)),max_jump=float(np.max(np.abs(np.diff(x)))),diagnostics=diag))
        return x
    @staticmethod
    def hit(n,length=64): return [(n,'gate',1),(n+length,'gate',0)]
    def execute(self):
        ref=self.build('reference','reference.dsp');fast=self.build('optimized','optimized.dsp')
        vec=self.build('vector','optimized.dsp',True)
        self.report['compilers']={'cxx':cmd([os.getenv('CXX','c++'),'--version']).strip(),
            'faust': 'replayed generated C++; no Faust invocation' if self.replay else cmd([os.getenv('FAUST','faust'),'-v']).strip()}
        silence=self.render('initial-silence',fast,seconds=.1)
        self.check('exact-initial-silence',not np.any(silence))
        # All twelve waveform/reset categories and sample rates, same score.
        for rate in (44100,48000,96000):
            on=round(.05*rate)
            for w in range(12):
                p={'wave':w,'pitch_hz':52,'drive':.5,'click':.2}
                a=self.render(f'r{rate}-w{w}-reference',ref,p,self.hit(on),rate=rate,seconds=.65)
                b=self.render(f'r{rate}-w{w}-optimized',fast,p,self.hit(on),rate=rate,seconds=.65)
                self.check(f'fourier-equivalence:{rate}:{w}',np.max(abs(a-b))<.0003,
                    max_abs=float(np.max(abs(a-b))),rms_error=float(np.sqrt(np.mean((a.astype(float)-b)**2))))
            for hz in (20,52,160):
                x=self.render(f'pitch-{rate}-{hz}',fast,{'wave':0,'pitch_hz':hz,'sweep':0,'click':0,'drive':0,'hold':.8},self.hit(on),rate=rate,seconds=1.4)
                measured=clean_pitch(x[round(.3*rate):round(1.3*rate)],rate)
                self.check(f'clean-pitch:{rate}:{hz}',abs(measured-hz)<.25,measured_hz=measured)
        ev=self.hit(101)+[(13011,'wave',8),(13011,'drive',.9),(13011,'decay',.8)]+self.hit(13011)+[(24001,'pitch_hz',130),(25007,'drive',0)]+self.hit(38001)
        a=self.render('dynamic-reference',ref,events=ev)
        b=self.render('dynamic-optimized',fast,events=ev)
        self.check('dynamic-fourier-equivalence',np.max(abs(a-b))<.0003,max_abs=float(np.max(abs(a-b))))
        for block in (1,32,64,127,512):
            x=self.render('dynamic-block-'+str(block),fast,events=ev,block=block)
            self.check('segmentation:'+str(block),np.array_equal(x,b),max_abs=float(np.max(abs(x-b))))
        v=self.render('dynamic-vector',vec,events=ev)
        self.check('vector-equivalence',np.max(abs(v-b))<.0003,max_abs=float(np.max(abs(v-b))))
        a=self.render('gate-pulse',fast,events=self.hit(101,1));b=self.render('gate-held',fast,events=[(101,'gate',1)])
        self.check('gateoff-does-not-choke',np.array_equal(a,b))
        h=self.render('half-velocity',fast,{'velocity':.5},self.hit(101,1))
        self.check('half-velocity-law',np.max(abs(h-a*.5))<2e-6)
        z=self.render('zero-velocity',fast,{'velocity':0},self.hit(101))
        self.check('zero-velocity-silence',not np.any(z))
        changed=self.render('latched-parameters',fast,events=self.hit(101,1)+[(3001,'decay',1),(3001,'hold',1),(3001,'sweep',1),(3001,'sweep_time',1),(3001,'wave',10),(3001,'velocity',.2),(3001,'click',1)])
        self.check('onset-latched-controls-do-not-rewrite-tail',np.array_equal(a,changed))
        # Reset vs free phase tested with late onsets, no sample/phase alignment.
        for w in (0,1):
            p={'wave':w,'click':0,'drive':0,'decay':0,'sweep':0}
            x=self.render(f'phase-early-{w}',fast,p,self.hit(4001))
            y=self.render(f'phase-later-{w}',fast,p,self.hit(14019))
            err=float(np.max(abs(x[4001:10001]-y[14019:20019])))
            self.check('phase-mode:'+str(w),err<.0001 if w==0 else err>.01,max_abs=err)
        # 1536 endpoint settings in one persistent trajectory, no per-hit reset.
        keys=['sweep_time','sweep','decay','hold','click','drive'];ev=[];idx=0
        for w,bits,hz in itertools.product(range(12),itertools.product((0.,1.),repeat=6),(20.,160.)):
            n=101+idx*1024;idx+=1
            ev += [(n,k,x) for k,x in zip(keys,bits)]+[(n,'pitch_hz',hz),(n,'wave',w)]+self.hit(n,128)
        x=self.render('all-endpoints',fast,events=ev,seconds=(101+idx*1024+24000)/48000,block=127)
        self.check('endpoint-DC',abs(float(np.mean(x)))<.02,settings=idx,mean=float(np.mean(x)))
        # Same continuous-control score tests ringing-note movement and onset locks.
        ev=[]
        for i in range(160):
            n=101+i*256;ev+= [(n,'drive',i%2),(n,'pitch_hz',20+140*((i%17)/16))]
            if i%4==0:ev+=self.hit(n,64)
        self.render('rapid-gestures',fast,events=ev,seconds=1.5,block=32)
        # Proper, time-aligned 2x comparison. Never use hi[::2] as an alias test.
        alias=[]
        for w in (0,4,8,10):
            for driven in (0.,1.):
                p={'pitch_hz':150,'sweep':0,'hold':.85,'wave':w,'drive':driven,'click':0}
                lo=self.render(f'hf-w{w}-d{int(driven)}-48',fast,p,self.hit(2400),rate=48000,seconds=1.)
                hi=self.render(f'hf-w{w}-d{int(driven)}-96',fast,p,self.hit(4800,128),rate=96000,seconds=1.)
                down=resample_poly(hi.astype(float),1,2,window=('kaiser',10.))
                sl=slice(9600,38400);err=lo[sl]-down[sl]
                alias.append({'wave':w,'drive':driven,'relative_rms_db':float(20*np.log10(max(1e-15,np.sqrt(np.mean(err**2)))/max(1e-15,np.sqrt(np.mean(down[sl]**2))))),
                    'max_abs':float(np.max(abs(err)))})
        self.report['rate_consistency_not_alias_proof']=alias
        # Auditions with fixed kernel gain; no normalization or clipping.
        ev=[];timeline=[]
        for i,(name,p) in enumerate(self.patches.items()):
            n=round((i*2.5+.05)*48000);ev += [(n,k,v) for k,v in p.items()]+[(n,'velocity',1)]+self.hit(n)
            n2=n+48000;ev += [(n2,'velocity',.55)]+self.hit(n2)
            timeline.append({'start_s':n/48000,'patch':name,'second_hit_s':n2/48000})
        x=self.render('anchors',fast,events=ev,seconds=16.)
        write_wav(self.out/'analog-sharp-anchors.wav',x)
        self.report['auditions']['analog-sharp-anchors.wav']=timeline
        ev=[]
        for i in range(64):
            if i%4==0 or i in (7,15,22,23,31,39,47,54,55,62,63):
                n=2400+i*6000;p=list(self.patches.values())[(i//12)%len(self.patches)]
                ev += [(n,k,v) for k,v in p.items()]+[(n,'velocity',1 if i%4==0 else .45)]+self.hit(n,100)
        x=self.render('pattern',fast,events=ev,seconds=9.)
        write_wav(self.out/'analog-sharp-pattern.wav',x)
        self.report['auditions']['analog-sharp-pattern.wav']='One persistent voice, 64-step phrase at 120 BPM with locks and accents; fixed gain.'
        ev=[];order=self.man['musical_control_order']
        for j,key in enumerate(order):
            for i in range(8):
                n=2400+j*144000+i*18000
                p=self.defaults.copy();p.update({'wave':4,'sweep':.5,'hold':.1})
                p[key]=20*8**(i/7) if key=='pitch_hz' else (int(round(i/7*11)) if key=='wave' else i/7)
                p.pop('gate');ev +=[(n,k,v) for k,v in p.items()]+self.hit(n,64)
        x=self.render('control-sweeps',fast,events=ev,seconds=25.)
        write_wav(self.out/'analog-sharp-controls.wav',x)
        self.report['auditions']['analog-sharp-controls.wav']=dict(seconds_per_control=3,order=order,discrete_steps=8)
        # Paired repetitions compare exactly the same full synth, not a clean sine.
        perf=[]
        for block in (64,128,512):
            pairs=[]
            for rep in range(3):
                order=['reference','optimized'] if rep%2==0 else ['optimized','reference'];r={}
                for label in order:r[label]=json.loads(cmd([self.out/label/'benchmark',block]))
                self.check(f'callback-new:{block}:{rep}',not any(x['ordinary_new_allocations_in_compute'] for x in r.values()))
                pairs.append(r)
            speedup=statistics.median([x['reference']['p50_us']/x['optimized']['p50_us'] for x in pairs])
            perf.append(dict(block=block,pairs=pairs,median_p50_speedup=speedup))
        self.report['performance']=perf
        self.report['performance_scope']='Hosted/container offline DSP-only, four voices, warm run, ordinary new hook only; no device/UI contention or malloc/aligned-allocation proof.'
        self.report['passed']=True
    def save(self,error=None):
        self.report['failure']=error
        if error:self.report['passed']=False
        try:self.report['source_commit']=cmd(['git','rev-parse','HEAD']).strip()
        except Exception:self.report['source_commit']=None
        paths=list(MOD.glob('*'))+[ROOT/'tools/modules/render.cpp',ROOT/'tools/modules/sharp_batch.py',ROOT/'tools/modules/sharp_benchmark.cpp']
        self.report['source_files']={}
        for p in paths:
            if p.is_file():
                rel=p.relative_to(ROOT);dest=self.out/'source'/rel;dest.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(p,dest)
                self.report['source_files'][str(rel)]=sha(p)
        (self.out/'report.json').write_text(json.dumps(self.report,indent=2)+'\n')
        print(json.dumps(dict(passed=self.report['passed'],checks=len(self.report['checks']),renders=len(self.report['renders']),failure=error)))

def main():
    a=argparse.ArgumentParser(description=__doc__);a.add_argument('--out',type=Path,required=True);a.add_argument('--replay',type=Path)
    args=a.parse_args();s=Study(args.out.resolve(),args.replay.resolve() if args.replay else None);error=None
    try:s.execute()
    except Exception as e:error=str(e)
    finally:s.save(error)
    if error:raise SystemExit(error)
if __name__=='__main__':main()
