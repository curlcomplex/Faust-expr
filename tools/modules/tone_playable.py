"""Actual-Faust Tone 0.2 qualification, musical rendering and offline replay.
Read report categories: arithmetic tests are not reference fidelity or device acceptance.
"""
from __future__ import annotations
import argparse, hashlib, itertools, json, math, os, platform, shutil, statistics, subprocess, wave
from pathlib import Path
import numpy as np
from scipy.io import wavfile
from scipy.signal import stft, resample_poly
ROOT=Path(__file__).resolve().parents[2]
MOD=ROOT/'modules/tone-pm/playable'

def sha(path): return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def command(args,timeout=180):
    p=subprocess.run(list(map(str,args)),cwd=ROOT,capture_output=True,text=True,timeout=timeout)
    if p.returncode: raise RuntimeError(str(args)+'\n'+p.stdout+'\n'+p.stderr)
    return p.stdout

def validate(values,manifest):
    for k,v in values.items():
        if k not in manifest['controls'] or isinstance(v,bool) or not isinstance(v,(int,float)) or not math.isfinite(v): raise ValueError('invalid control '+k)
        c=manifest['controls'][k]
        if not c['min']<=v<=c['max'] or (k in ('gate','gate_mode') and v not in (0,1)): raise ValueError('invalid value '+k)

def score_rows(values,events,manifest,frames):
    validate(values,manifest);rows={(0,k):v for k,v in values.items()};seen=set()
    for n,k,v in events:
        if isinstance(n,bool) or not isinstance(n,int) or not 0<=n<frames: raise ValueError('frame')
        validate({k:v},manifest)
        if (n,k) in seen: raise ValueError('duplicate event')
        seen.add((n,k));rows[n,k]=v
    return ''.join(f'{n}\t{k}\t{v:.9g}\n' for (n,k),v in sorted(rows.items()))

def write_wav(path,x,rate=48000):
    x=np.asarray(x)
    if not x.size or not np.isfinite(x).all() or np.max(abs(x))>=1: raise ValueError('invalid/clipped audio')
    with wave.open(str(path),'wb') as f:
        f.setnchannels(1);f.setsampwidth(2);f.setframerate(rate);f.writeframes(np.rint(x*32767).astype('<i2').tobytes())

def descriptor(x,rate):
    x=np.asarray(x,dtype=float)
    if x.ndim>1: x=x.mean(axis=1)
    x=x[:10*rate]
    if not x.size or not np.isfinite(x).all() or np.sum(x*x)<1e-15: raise ValueError('silent/nonfinite reference')
    energy=x*x;cum=np.cumsum(energy)/energy.sum()
    f,_,z=stft(x,rate,nperseg=min(2048,len(x)),noverlap=min(1024,max(0,len(x)//2-1)),boundary='zeros')
    power=np.sum(abs(z)**2,axis=1);power/=max(power.sum(),1e-30)
    return np.array([np.searchsorted(cum,.5)/rate,np.searchsorted(cum,.9)/rate,
        math.log2(max(1,float(np.sum(f*power)))),
        *[float(power[(f>=a)&(f<b)].sum()) for a,b in ((0,200),(200,1000),(1000,5000),(5000,20001))]])

def sine_pitch(x,rate):
    y=np.asarray(x,dtype=float);y=y-y.mean();n=1<<max(19,(len(y)-1).bit_length())
    p=abs(np.fft.rfft(y*np.hanning(len(y)),n=n));p[0]=0;k=int(p.argmax());z=np.log(np.maximum(p[k-1:k+2],1e-30))
    d=z[0]-2*z[1]+z[2];off=.5*(z[0]-z[2])/d if d else 0
    return (k+off)*rate/n

class Study:
    def __init__(self,out,refs=None,replay=None):
        self.out=out;out.mkdir(parents=True,exist_ok=True);self.refs=refs;self.replay=replay
        self.man=json.loads((MOD/'manifest.json').read_text());self.defaults={k:v['default'] for k,v in self.man['controls'].items()}
        self.patches=json.loads((MOD/'patches.json').read_text())['anchors'];self.zones={}
        self.report={'schema':1,'module':'tone-pm/0.2.0-experiment','passed':False,'checks':[],'renders':[],'builds':{},'auditions':{},'platform':platform.platform(),
                     'hardware_matched':False,'human_approved':False,'device_qualified':False}
    def check(self,name,ok,**details):
        self.report['checks'].append(dict(name=name,passed=bool(ok),**details))
        if not ok: raise AssertionError(name+' '+str(details))
    def build(self,name,file,vector=False,full=False):
        d=self.out/name;d.mkdir(exist_ok=True);flags=['-lang','cpp','-single','-cn','ModuleDSP']+(['-vec','-lv','0','-vs','32'] if vector else [])
        if self.replay:
            gold=json.loads((self.replay/'report.json').read_text())['builds'][name]
            if sha(self.replay/name/'generated.hpp')!=gold['generated_sha256']: raise ValueError('generated checksum')
            shutil.copyfile(self.replay/name/'generated.hpp',d/'generated.hpp')
        else:
            command([os.getenv('FAUST','faust'),'-I',MOD,'-e',MOD/file,'-o',d/'expanded.dsp'])
            command([os.getenv('FAUST','faust'),'-I',MOD,*flags,MOD/file,'-o',d/'generated.hpp'])
        cflags=['-std=c++17','-O2','-ffp-contract=off','-fstack-usage','-I'+str(d)]
        command([os.getenv('CXX','c++'),*cflags,ROOT/'tools/modules/render.cpp','-o',d/'render'])
        rows=command([d/'render','--controls']);(d/'controls.tsv').write_text(rows)
        zones={r.split('\t')[0]:list(map(float,r.split('\t')[1:])) for r in rows.splitlines()[1:]};self.zones[name]=zones
        self.check(name+':known-controls',set(zones)<=set(self.defaults))
        if full:self.check(name+':all-controls',set(zones)==set(self.defaults))
        for k,z in zones.items():
            c=self.man['controls'][k];self.check(name+':'+k,np.allclose(z[:3],[c['min'],c['max'],c['default']],atol=1e-5,rtol=0))
        self.report['builds'][name]={'file':file,'generated_sha256':sha(d/'generated.hpp'),'flags':flags,'cxx_flags':cflags,'binary_sha256':sha(d/'render')}
        return d/'render'
    def render(self,name,exe,params=None,events=(),seconds=1,rate=48000,block=128,audio=True):
        vals=self.defaults|(params or {});validate(vals,self.man);zones=self.zones[exe.parent.name]
        vals={k:v for k,v in vals.items() if k in zones};events=list(events)
        if any(k not in zones for _,k,_ in events): raise ValueError('event on omitted diagnostic control')
        frames=round(seconds*rate);sc=self.out/(name+'.tsv');sc.write_text(score_rows(vals,events,self.man,frames));raw=self.out/(name+'.f32')
        diag=json.loads(command([exe,sc,raw,rate,block,frames,0]));x=np.fromfile(raw,dtype='<f4').reshape(frames,diag['channels'])
        if x.shape[1]==1:x=x[:,0]
        self.check(name+':valid',np.isfinite(x).all() and (not audio or np.max(abs(x))<.95),peak=float(np.max(abs(x))))
        self.report['renders'].append({'name':name,'build':exe.parent.name,'audio':audio,'rate':rate,'block':block,'frames':frames,'channels':diag['channels'],
             'score_sha256':sha(sc),'raw_sha256':sha(raw),'peak':float(np.max(abs(x))),'rms':float(np.sqrt(np.mean(x.astype(float)**2))),
             'max_jump':float(np.max(abs(np.diff(x,axis=0)))),'diag':diag})
        return x
    @staticmethod
    def hit(n,length=64):return [(n,'gate',1),(n+length,'gate',0)]
    def run(self):
        scalar=self.build('scalar','tone.dsp',full=True);vector=self.build('vector','tone.dsp',True,True)
        env=self.build('envelope','envelope.dsp');controls=self.build('control-diagnostic','controls.dsp');raw=self.build('raw-diagnostic','raw.dsp')
        self.report['compilers']={'cxx':command([os.getenv('CXX','c++'),'--version']).strip(),'faust': 'replay only' if self.replay else command([os.getenv('FAUST','faust'),'-v']).strip()}
        self.check('silence',not np.any(self.render('silence',scalar,seconds=.2)))
        clean={'punch':0,'drive':0,'modulation':0,'feedback':0,'gate_mode':1,'decay':0.5}
        for rate in (44100,48000,96000):
            for hz in (20,55,220,880,4000,8000):
                x=self.render(f'pitch-{rate}-{hz}',scalar,clean|{'pitch_hz':hz},[(101,'gate',1)],rate=rate,seconds=1.3)
                m=sine_pitch(x[round(.2*rate):round(1.2*rate)],rate);self.check(f'pitch:{rate}:{hz}',abs(m-hz)<.3,measured_hz=m)
            for mode,dec,punch,length in ((0,0,0,.2),(0,1,1,.1),(1,.45,.2,.5),(1,1,0,1.0),(1,.2,1,1/rate)):
                on=101;off=on+max(1,round(length*rate));secs=50 if dec==1 else 3
                x=self.render(f'env-{rate}-{mode}-{dec}-{length}',env,{'decay':dec,'punch':punch,'gate_mode':mode},self.hit(on,off-on),seconds=secs,rate=rate,audio=False)
                n=np.arange(len(x));t=np.maximum(n-on,0)/rate;atk=.0004-.00025*punch;tau=.03*200**dec
                attack=1-np.exp(-t/atk);expected=attack*np.exp(-t/tau)
                if mode:expected=np.where(n<off,attack,(1-math.exp(-(off-on)/rate/atk))*np.exp(-np.maximum(n-off,0)/rate/tau))
                expected[n<on]=0;mask=expected>1e-3
                err=float(np.max(abs(x[mask]-expected[mask])/expected[mask]));self.check('closed-form:'+str((rate,mode,dec,length)),err<1e-4,relative_error=err)
            # Ratio physical landmarks, not guessed display detents.
            for pos,ratio in ((0,.25),(.2,.5),(.4,1),(.6,2),(.8,4),(1,8)):
                x=self.render(f'ratio-{rate}-{pos}',controls,{'ratio':pos},[(101,'gate',1)],seconds=.02,rate=rate,audio=False)
                self.check('ratio:'+str((rate,pos)),abs(float(x[101,1])-ratio)<2e-5,ratio=float(x[101,1]))
            target=self.patches['Acid'];a=self.render(f'lock-pre-{rate}',scalar,target,self.hit(1001,500),rate=rate)
            b=self.render(f'lock-same-{rate}',scalar,events=[(1001,k,v) for k,v in target.items()]+self.hit(1001,500),rate=rate)
            self.check('same-sample:'+str(rate),np.array_equal(a,b))
        events=self.hit(101,9000)+[(3011,'ratio',.67),(5111,'feedback',.75),(8111,'drive',.6),(12001,'pitch_hz',440),(12001,'modulation',.8)]+self.hit(12001,16000)
        base=self.render('dynamic-128',scalar,{'gate_mode':1},events)
        for block in (1,32,64,127,256,512):
            x=self.render('dynamic-'+str(block),scalar,{'gate_mode':1},events,block=block);self.check('segmentation:'+str(block),np.array_equal(x,base))
        x=self.render('dynamic-vector',vector,{'gate_mode':1},events);self.check('vector-parity',np.max(abs(x-base))<3e-4,max_abs=float(np.max(abs(x-base))))
        # Actual control-signal oracle for a live change without retrigger.
        x=self.render('live-control-oracle',controls,{'pitch_hz':110,'ratio':.4},[(101,'gate',1),(1001,'pitch_hz',440),(1001,'ratio',.8)],seconds=.08,audio=False)
        a=math.exp(-1/(.003*48000));n=np.arange(len(x)-1001)+1
        self.check('pitch-smoothing',np.max(abs(x[1001:,0]-(440-330*a**n)))<.02)
        expected=.25*32**(.8-.4*a**n);self.check('ratio-smoothing',np.max(abs(x[1001:,1]-expected))<.001)
        shot=self.render('one-shot-pulse',scalar,clean|{'gate_mode':0},self.hit(101,1),seconds=2)
        held=self.render('one-shot-held',scalar,clean|{'gate_mode':0},[(101,'gate',1)],seconds=2);self.check('one-shot-noteoff',np.array_equal(shot,held))
        half=self.render('half-velocity',scalar,clean|{'gate_mode':0,'velocity':.5},self.hit(101,1),seconds=2);self.check('velocity-linear',np.max(abs(half-.5*shot))<2e-6)
        self.check('zero-velocity',not np.any(self.render('zero-velocity',scalar,{'velocity':0},self.hit(101))))
        # Pitch live without new gate edge is explicitly legato, not retrigger.
        x=self.render('legato',scalar,clean|{'pitch_hz':110},[(101,'gate',1),(24000,'pitch_hz',220)],seconds=1.6)
        self.check('legato-new-pitch',abs(sine_pitch(x[35000:70000],48000)-220)<.3)
        lat=self.render('latched-envelope',env,{'gate_mode':1,'decay':.3},[(101,'gate',1),(4801,'decay',1),(4801,'punch',1),(4801,'gate_mode',0),(10001,'gate',0)],seconds=1,audio=False)
        orig=self.render('latched-envelope-reference',env,{'gate_mode':1,'decay':.3},self.hit(101,9900),seconds=1,audio=False)
        self.check('envelope-settings-latched',np.array_equal(lat,orig))
        # Neutral path must be a real sine, not tanh(sine)/tanh(1).
        x=self.render('neutral-raw',raw,clean|{'pitch_hz':240},[(101,'gate',1)],seconds=1.2,audio=False)
        y=x[4800:52800];p=abs(np.fft.rfft(y));fund=p[240];harm=float(np.sqrt(sum(p[k*240]**2 for k in range(2,20)))/fund)
        self.check('neutral-harmonics',harm<1e-4,harmonic_ratio=harm)
        # Every boolean corner at both real pitch limits and both articulations.
        ev=[];keys=['ratio','punch','decay','feedback','modulation','mod_env','drive'];i=0
        for hz,mode,bits in itertools.product((20,8000),(0,1),itertools.product((0.,1.),repeat=7)):
            n=101+i*1024;i+=1;ev += [(n,k,v) for k,v in zip(keys,bits)]+[(n,'pitch_hz',hz),(n,'gate_mode',mode)]+self.hit(n,512)
        self.render('endpoints',scalar,events=ev,seconds=(101+i*1024+48000)/48000,block=127);self.report['endpoint_settings']=i
        # Authored patches; do not claim matched hardware presets.
        ev=[];timeline=[]
        for i,(name,p) in enumerate(self.patches.items()):
            n=2400+i*144000;ev += [(n,k,v) for k,v in p.items()]+[(n,'velocity',1)]+self.hit(n,24000)
            ev += [(n+60000,'velocity',.55)]+self.hit(n+60000,12000);timeline.append({'seconds':n/48000,'patch':name})
        self.audio('tone-anchors.wav',self.render('anchors',scalar,events=ev,seconds=25));self.report['auditions']['tone-anchors.wav']=timeline
        ev=[];notes=[55,65.4064,82.4069,110,130.813,164.814,220,261.626]
        for i in range(48):
            n=2400+i*6000;p=list(self.patches.values())[(i//8)%8]|{'pitch_hz':notes[i%8],'velocity':.9 if i%4==0 else .55,'gate_mode':1}
            ev += [(n,k,v) for k,v in p.items()]+self.hit(n,3000 if i%8!=6 else 5500)
        self.audio('tone-pattern.wav',self.render('pattern',scalar,events=ev,seconds=8))
        ev=[];order=['pitch_hz','ratio','feedback','modulation','drive']
        for j,key in enumerate(order):
            n=2400+j*192000;p=self.patches['Reed']|{'pitch_hz':110,'decay':.15};ev += [(n,k,v) for k,v in p.items()]+self.hit(n,156000)
            for i in range(65):
                q=n+2400+i*2000;v=55*8**(i/64) if key=='pitch_hz' else i/64;ev.append((q,key,v))
        self.audio('tone-live-controls.wav',self.render('live-controls',scalar,events=ev,seconds=21));self.report['auditions']['tone-live-controls.wav']=order
        # Four independently generated voices overlap; this is a mix, not a chord machine.
        voices=[]
        for j,hz in enumerate((110,164.814,220,261.626)):
            ev=[(4800+j*1800,'gate',1),(148800+j*1200,'gate',0),(52800,'modulation',.6),(100800,'ratio',.6)]
            voices.append(self.render('poly-voice-'+str(j),scalar,self.patches['Keys']|{'pitch_hz':hz,'decay':.22},ev,seconds=6))
        self.audio('tone-polyphony.wav',sum(voices)*.25);self.report['polyphony_mix']='Four actual independent instances, same mono kernel, fixed 0.25 per voice. Not built-in chords or host integration.'
        rate_results=[]
        for name,p in [('clean',clean|{'pitch_hz':440}),('keys',self.patches['Keys']),('extreme',self.patches['Acid']|{'pitch_hz':4000,'ratio':1,'feedback':1,'modulation':1,'drive':1})]:
            lo=self.render('rate-'+name+'-48',scalar,p,self.hit(4800,24000),seconds=1.5)
            hi=self.render('rate-'+name+'-96',scalar,p,self.hit(9600,48000),seconds=1.5,rate=96000)
            down=resample_poly(hi.astype(float),1,2,window=('kaiser',10));sl=slice(6000,60000);diff=lo[sl]-down[sl]
            rate_results.append({'name':name,'relative_rms_db':float(20*np.log10(max(1e-15,np.sqrt(np.mean(diff**2)))/max(1e-15,np.sqrt(np.mean(down[sl]**2)))))})
        self.report['rate_consistency_not_alias_energy']=rate_results
        if self.refs:self.coverage(scalar)
        self.benchmark()
        for f in self.out.glob('*.wav'):
            with wave.open(str(f),'rb') as w:self.check('container:'+f.name,len(w.readframes(w.getnframes()))==w.getnframes()*w.getsampwidth()*w.getnchannels())
        self.report['passed']=True
    def audio(self,name,x):write_wav(self.out/name,x);self.report['auditions'].setdefault(name,{'processing':'fixed kernel gain; PCM16 only'})
    def coverage(self,exe):
        manifest=json.loads((self.refs/'manifest.json').read_text());refs={}
        for row in manifest['records']:
            rate,x=wavfile.read(self.refs/(row['id']+'.wav'));refs[row['id']]=descriptor(x,rate)
        if len(refs)!=8:raise ValueError('eight fixed references required')
        rng=np.random.default_rng(19102026);params=[]
        for _ in range(32):
            p={k:float(rng.random()) for k in ('ratio','punch','feedback','modulation','mod_env','drive')};p.update(pitch_hz=float(55*8**rng.random()),decay=float(.1+.55*rng.random()),gate_mode=0);params.append(p)
        pools={}
        for mode in ('full','carrier'):
            d=[]
            for i,p in enumerate(params):
                v=p if mode=='full' else p|{'feedback':0,'modulation':0};x=self.render(f'coverage-{mode}-{i}',exe,v,self.hit(101),seconds=10);d.append(descriptor(x[101:],48000))
            pools[mode]=np.array(d)
        r=np.array(list(refs.values()));all_d=np.vstack([r,*pools.values()]);scale=np.maximum(all_d.std(axis=0),1e-7);result={}
        for mode,d in pools.items():
            dist=np.sqrt(np.mean(((r[:,None,:]-d[None,:,:])/scale)**2,axis=2));result[mode]={'mean_nearest':float(dist.min(axis=1).mean()),'nearest':{key:{'distance':float(dist[i].min()),'pool_index':int(dist[i].argmin())} for i,key in enumerate(refs)}}
        self.report['reference_coverage']={'results':result,'parameters':params,'scale':scale.tolist(),'limits':'10s horizon, pooled energy spectra, gain-independent descriptors; unknown-settings lossy previews, all development data, no clone/holdout claim.'}
    def benchmark(self):
        perf=[]
        for name in ('scalar','vector'):
            d=self.out/name;command([os.getenv('CXX','c++'),'-std=c++17','-O2','-ffp-contract=off','-fstack-usage','-I'+str(d),ROOT/'tools/modules/tone_benchmark.cpp','-o',d/'benchmark'])
        for block in (32,64,128,512):
            pairs=[]
            for rep in range(4):
                row={}
                for name in (('scalar','vector') if rep%2==0 else ('vector','scalar')):row[name]=json.loads(command([self.out/name/'benchmark',block]))
                self.check('ordinary-new:'+str((block,rep)),all(x['ordinary_new_allocations_in_compute']==0 for x in row.values()));pairs.append(row)
            perf.append({'block':block,'pairs':pairs,'median_p50_ratio':statistics.median([p['scalar']['p50_us']/p['vector']['p50_us'] for p in pairs])})
        self.report['performance']=perf;self.report['performance_scope']='Warmed four-voice offline same-source scalar/vector, one-shot benchmark. New/new[] only, not full allocator coverage or device deadline/thermal evidence.'
    def save(self,error=None):
        self.report['failure']=error
        if error:self.report['passed']=False
        try:self.report['source_commit']=command(['git','rev-parse','HEAD']).strip()
        except Exception:self.report['source_commit']=None
        paths=list(MOD.glob('*'))+[ROOT/'tools/modules/tone_playable.py',ROOT/'tools/modules/render.cpp',ROOT/'tools/modules/tone_benchmark.cpp',ROOT/'tests/test_tone_playable.py']
        self.report['source_files']={str(p.relative_to(ROOT)):sha(p) for p in paths if p.is_file()}
        (self.out/'report.json').write_text(json.dumps(self.report,indent=2)+'\n')
        print(json.dumps({'passed':self.report['passed'],'renders':len(self.report['renders']),'checks':len(self.report['checks']),'failure':error}))
def main():
    a=argparse.ArgumentParser(description=__doc__);a.add_argument('--out',type=Path,required=True);a.add_argument('--references',type=Path);a.add_argument('--replay',type=Path);args=a.parse_args()
    s=Study(args.out.resolve(),args.references.resolve() if args.references else None,args.replay.resolve() if args.replay else None);err=None
    try:s.run()
    except Exception as e:err=str(e)
    finally:s.save(err)
    if err:raise SystemExit(err)
if __name__=='__main__':main()
