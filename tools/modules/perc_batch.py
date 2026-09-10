"""Actual-Faust qualification and audition for perc-pm. Not a hardware-clone score."""
from __future__ import annotations
import argparse, hashlib, itertools, json, math, os, shutil, subprocess, wave
from pathlib import Path
import numpy as np
ROOT=Path(__file__).resolve().parents[2];MOD=ROOT/'modules/perc-pm'
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def cmd(a,timeout=180):
    p=subprocess.run(list(map(str,a)),cwd=ROOT,capture_output=True,text=True,timeout=timeout)
    if p.returncode:raise RuntimeError(str(a)+'\n'+p.stdout+'\n'+p.stderr)
    return p.stdout
def wav(path,x,rate=48000):
    if not np.isfinite(x).all() or np.max(np.abs(x))>=1:raise ValueError('refuse clipped/nonfinite WAV')
    with wave.open(str(path),'wb') as f:f.setnchannels(1);f.setsampwidth(2);f.setframerate(rate);f.writeframes(np.rint(x*32767).astype('<i2').tobytes())
def describe(x,rate=48000):
    y=np.asarray(x,float);e=y*y;peak=float(np.max(np.abs(y)));rms=float(np.sqrt(np.mean(e)));
    n=min(len(y),rate);s=np.abs(np.fft.rfft(y[:n]*np.hanning(n)))**2;freq=np.fft.rfftfreq(n,1/rate);tot=max(float(s.sum()),1e-30)
    centroid=float((freq*s).sum()/tot);flat=float(np.exp(np.mean(np.log(np.maximum(s,1e-20))))/max(np.mean(s),1e-20))
    cum=np.cumsum(e)/max(float(e.sum()),1e-30);d90=float(np.searchsorted(cum,.9)/rate)
    return dict(peak=peak,rms=rms,centroid_hz=centroid,flatness=flat,energy90_s=d90,max_jump=float(np.max(np.abs(np.diff(y)))),dc=float(np.mean(y)))
def distance(a,b):
    # Broad descriptor coverage only. Log ratios stop absolute scales dominating.
    da,db=describe(a),describe(b);keys=('centroid_hz','flatness','energy90_s')
    return float(np.sqrt(np.mean([(math.log(max(da[k],1e-8)/max(db[k],1e-8)))**2 for k in keys])))
class Study:
    def __init__(self,out,refs):
        self.out=out;out.mkdir(parents=True,exist_ok=True);self.refs=refs
        self.man=json.loads((MOD/'manifest.json').read_text());self.patches=json.loads((MOD/'patches.json').read_text())['anchors'];self.defaults={k:v['default'] for k,v in self.man['controls'].items()}
        self.r={'checks':[],'renders':[],'passed':False,'hardware_reference_fit':False,'human_approved':False,'device_qualified':False}
    def check(self,n,ok,**d):self.r['checks'].append(dict(name=n,passed=bool(ok),**d));(_ for _ in ()).throw(AssertionError(n+str(d))) if not ok else None
    def build(self,label,vec=False):
        d=self.out/label;d.mkdir(exist_ok=True);flags=['-lang','cpp','-single','-cn','ModuleDSP']+(['-vec','-lv','0','-vs','32'] if vec else [])
        cmd([os.getenv('FAUST','faust'),*flags,MOD/'perc.dsp','-o',d/'generated.hpp']);cmd([os.getenv('CXX','c++'),'-std=c++17','-O2','-ffp-contract=off','-I'+str(d),ROOT/'tools/modules/render.cpp','-o',d/'render'])
        self.r.setdefault('builds',{})[label]=dict(flags=flags,generated_sha256=sha(d/'generated.hpp'),binary_sha256=sha(d/'render'))
        return d/'render'
    def render(self,label,exe,p=None,events=None,rate=48000,block=128,seconds=1.2):
        vals=self.defaults|(p or {});rows={(0,k):v for k,v in vals.items()};
        for n,k,v in events or []:rows[n,k]=v
        score=self.out/(label+'.tsv');raw=self.out/(label+'.f32');score.write_text(''.join(f'{n}\t{k}\t{v:.9g}\n' for (n,k),v in sorted(rows.items())))
        frames=round(seconds*rate);diag=json.loads(cmd([exe,score,raw,rate,block,frames,0]));x=np.fromfile(raw,dtype='<f4')
        self.check(label+':finite-headroom',len(x)==frames and np.isfinite(x).all() and np.max(np.abs(x))<.9,peak=float(np.max(np.abs(x))))
        self.r['renders'].append(dict(label=label,rate=rate,block=block,raw_sha256=sha(raw),score_sha256=sha(score),metrics=describe(x,rate),diagnostics=diag));return x
    @staticmethod
    def hit(n,l=64):return [(n,'gate',1),(n+l,'gate',0)]
    def execute(self):
        scalar=self.build('scalar');vec=self.build('vector',True)
        ctl=cmd([scalar,'--controls']).splitlines();self.check('control-contract',ctl[0]=='io\t0\t1' and {x.split('\t')[0] for x in ctl[1:]}==set(self.defaults))
        z=self.render('silence',scalar,seconds=.1);self.check('initial-silence',not np.any(z))
        for rate in (44100,48000,96000):
            on=round(.04*rate)
            for hz in (55,220,880):
                x=self.render(f'clean-{rate}-{hz}',scalar,{'pitch_hz':hz,'sweep':0,'punch':0,'inharmonicity':0,'modulation':0,'drive':0,'decay':.82},self.hit(on),rate=rate,seconds=1.1)
                # Dominant line should remain near body pitch after sweep is removed.
                y=x[round(.25*rate):round(.85*rate)];n=1<<18;s=np.abs(np.fft.rfft(y*np.hanning(len(y)),n=n));f=np.fft.rfftfreq(n,1/rate);meas=float(f[np.argmax(s)])
                self.check(f'pitch:{rate}:{hz}',abs(meas-hz)<1.0,measured=meas)
        ev=self.hit(101)+[(8001,'inharmonicity',.9),(8001,'modulation',.85),(8001,'drive',.6)]+self.hit(8001)+[(17003,'sweep',.85),(17003,'punch',.8)]+self.hit(17003)
        base=self.render('dynamic-128',scalar,events=ev,seconds=.7)
        for b in (1,32,64,127,256,512):
            x=self.render('dynamic-'+str(b),scalar,events=ev,block=b,seconds=.7);self.check('segmentation:'+str(b),np.array_equal(x,base))
        v=self.render('dynamic-vector',vec,events=ev,seconds=.7);self.check('vector-close',np.max(np.abs(v-base))<3e-4,max_abs=float(np.max(np.abs(v-base))))
        short=self.render('short-gate',scalar,events=self.hit(101,1));held=self.render('held-gate',scalar,events=[(101,'gate',1)]);self.check('noteoff-independent',np.array_equal(short,held))
        half=self.render('half-velocity',scalar,{'velocity':.5},events=self.hit(101));self.check('velocity-linear',np.max(np.abs(half-short*.5))<2e-6)
        changed=self.render('latched-tail',scalar,events=self.hit(101,1)+[(4001,'decay',1),(4001,'inharmonicity',1),(4001,'modulation',1),(4001,'drive',1)]);self.check('tail-controls-latched',np.array_equal(short,changed))
        # 512 corners in one persistent trajectory.
        keys=['sweep','punch','decay','inharmonicity','modulation','mod_envelope','drive'];ev=[];i=0
        for bits in itertools.product((0.,1.),repeat=7):
            n=101+i*512;i+=1;ev += [(n,k,v) for k,v in zip(keys,bits)]+[(n,'pitch_hz',55 if i%2 else 1500)]+self.hit(n,32)
        x=self.render('endpoints',scalar,events=ev,seconds=(101+i*512+24000)/48000,block=127);self.check('endpoint-dc',abs(float(np.mean(x)))<.02,settings=i)
        # Audition anchors and pattern.
        ev=[];timeline=[]
        for i,(name,p) in enumerate(self.patches.items()):
            n=round((.05+i*2.2)*48000);ev +=[(n,k,v) for k,v in p.items()]+[(n,'velocity',1)]+self.hit(n);n2=n+36000;ev +=[(n2,'velocity',.55)]+self.hit(n2);timeline.append(dict(name=name,start_s=n/48000))
        a=self.render('anchors',scalar,events=ev,seconds=18);wav(self.out/'perc-anchors.wav',a);self.r['anchors_timeline']=timeline
        ev=[]
        for i in range(48):
            if i%3==0 or i in (7,14,22,23,31,38,46,47):
                n=2400+i*6000;p=list(self.patches.values())[(i//8)%len(self.patches)];ev +=[(n,k,v) for k,v in p.items()]+[(n,'velocity',1 if i%3==0 else .5)]+self.hit(n,64)
        ptn=self.render('pattern',scalar,events=ev,seconds=6.5);wav(self.out/'perc-pattern.wav',ptn)
        # All controls, eight discrete points each.
        ev=[]
        for j,key in enumerate(self.man['musical_control_order']):
            c=self.man['controls'][key]
            for i in range(8):
                n=2400+j*120000+i*15000;value=c['min']+(c['max']-c['min'])*i/7;vals=self.defaults.copy();vals[key]=value;vals.pop('gate');ev +=[(n,k,v) for k,v in vals.items()]+self.hit(n)
        ctr=self.render('controls',scalar,events=ev,seconds=21);wav(self.out/'perc-controls.wav',ctr)
        # Reference broad-coverage comparison, no per-sample fitting.
        if self.refs:
            refs=[]
            for p in sorted(self.refs.glob('PCC-*.wav')):
                import scipy.io.wavfile as wf
                rate,data=wf.read(p);data=np.asarray(data,dtype=float);data=data[:,0] if data.ndim>1 else data
                if np.issubdtype(data.dtype,np.integer):data=data/np.iinfo(data.dtype).max
                refs.append((p.stem,data.astype(float)))
            candidates=[]
            grid=[dict(pitch_hz=p,decay=d,inharmonicity=i,modulation=m,mod_envelope=.55,sweep=.25,punch=.35,drive=.08) for p in (90,180,360,720) for d in (.25,.55,.82) for i in (.1,.5,.9) for m in (.15,.55,.9)]
            for j,g in enumerate(grid):candidates.append((g,self.render('coverage-'+str(j),scalar,g,self.hit(101),seconds=.9)))
            nearest=[]
            for name,r in refs:
                ds=[distance(r[:min(len(r),len(x))],x[:min(len(r),len(x))]) for _,x in candidates];k=int(np.argmin(ds));nearest.append(dict(reference=name,distance=float(ds[k]),candidate=candidates[k][0]))
            self.r['reference_coverage']=nearest
        self.r['passed']=True
    def save(self,error=None):
        self.r['failure']=error;self.r['passed']=self.r['passed'] and error is None
        try:self.r['source_commit']=cmd(['git','rev-parse','HEAD']).strip()
        except: self.r['source_commit']=None
        (self.out/'report.json').write_text(json.dumps(self.r,indent=2)+'\n');print(json.dumps(dict(passed=self.r['passed'],checks=len(self.r['checks']),renders=len(self.r['renders']),failure=error)))
def main():
    p=argparse.ArgumentParser();p.add_argument('--out',type=Path,required=True);p.add_argument('--refs',type=Path);a=p.parse_args();s=Study(a.out.resolve(),a.refs.resolve() if a.refs else None);e=None
    try:s.execute()
    except Exception as x:e=str(x)
    finally:s.save(e)
    if e:raise SystemExit(e)
if __name__=='__main__':main()
