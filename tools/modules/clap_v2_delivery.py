"""Clap v2 qualification and offline replay. No device or hardware-clone claim.

Extends the preserved v1 test scores, not the v1 DSP. Diagnostic wrappers expose
only meaningful controls; no tiny fake audio dependencies retain removed zones.
"""
from __future__ import annotations
import argparse, hashlib, json, math, os, platform, shutil, statistics, subprocess, sys, wave
from pathlib import Path
import numpy as np
from scipy.io import wavfile
from scipy.signal import resample_poly
import clap_delivery as base
ROOT=Path(__file__).resolve().parents[2]
MOD=ROOT/'modules/clap/v2'
base.MOD=MOD
ORIGINAL_CMD=base.cmd

def validate(values, manifest):
    for k,v in values.items():
        if k not in manifest['controls'] or isinstance(v,bool) or not isinstance(v,(int,float)) or not math.isfinite(v):
            raise ValueError('invalid control '+str(k))
        c=manifest['controls'][k]
        if not c['min']<=v<=c['max'] or (k=='gate' and v not in (0,1)):
            raise ValueError('range '+k)

def envelope_oracle(t, p):
    sp,pn,de,be=(p[k] for k in ('spacing','punch','decay','body_env'))
    gap=.0015+.020*sp*sp; tau=.0007+.0048*(1-sp)+.002*(1-pn); attack=.0001+.00035*(1-pn)
    def burst(x):
        z=np.maximum(0,x)
        return (x>=0)*(1-np.exp(-z/attack))*np.exp(-z/tau)*(x<16*tau)
    cluster=sum(w*burst(t-n*gap) for n,w in ((0,1),(1,.88),(2.07,.70),(3.24,.52)))
    tt=.022*48**de; bt=.010*75**be
    tail=(1-np.exp(-t/.0035))*np.exp(-t/tt)*(t<16*tt)
    body=(1-np.exp(-t/.0008))*np.exp(-t/bt)*(t<16*bt)
    return cluster,tail,body

def features(audio, rate):
    """Energy-weighted short-window spectrum; no full-file Hann tail bias."""
    x=np.asarray(audio,dtype=np.float64)
    if x.ndim==2:x=x.mean(axis=1)
    if len(x)==0 or not np.isfinite(x).all() or np.max(np.abs(x))<1e-12:raise ValueError('invalid/silent audio')
    on=int(np.flatnonzero(abs(x)>np.max(abs(x))*.005)[0]);x=x[on:]
    energy=x*x; cumulative=np.cumsum(energy)/energy.sum()
    n=1024; hop=256; y=np.pad(x,(0,max(0,n-len(x))))
    frames=np.lib.stride_tricks.sliding_window_view(y,n)[::hop]
    power=(abs(np.fft.rfft(frames*np.hanning(n),axis=1))**2).sum(axis=0)
    f=np.fft.rfftfreq(n,1/rate);q=power/max(power.sum(),1e-30)
    return np.array([np.searchsorted(cumulative,.5)/rate,np.searchsorted(cumulative,.9)/rate,
                     np.sum(energy[:round(.1*rate)])/energy.sum(),np.sum(q[f<1000]),
                     np.sum(q[(f>=1000)&(f<5000)]),np.sum(q[f>=5000])])

def safe_gain(x):
    x=np.asarray(x,dtype=np.float64)
    return float(min(.12/max(float(np.sqrt(np.mean(x*x))),1e-12),.8/max(float(np.max(abs(x))),1e-12)))

class Study(base.Study):
    def __init__(self,out,replay=None,refs=None):
        super().__init__(out)
        self.replay=replay;self.refs=refs;self.uis={};self.commands=[]
        self.r.update(schema=2,sound_version='0.2.0-experiment',verification_mode='generated-C++ replay' if replay else 'actual-Faust',auditions={})
        self.old_report=json.loads((replay/'report.json').read_text()) if replay else None
    def ck(self,name,passed,**details):
        self.r['checks'].append(dict(name=name,passed=bool(passed),**details))
        if not passed:raise AssertionError(name+': '+str(details))
    def command(self,args,t=1200):
        self.commands.append([str(a) for a in args]);return ORIGINAL_CMD(args,t=t)
    def build(self,label,src='clap.dsp',vec=False):
        folder=self.o/label;folder.mkdir(exist_ok=True)
        source=MOD/src
        if label=='old-v1':source=ROOT/'modules/clap/v1/clap.dsp'
        flags=['-lang','cpp','-single','-cn','ModuleDSP']+(['-vec','-lv','0','-vs','32'] if vec else [])
        if self.replay:
            original=self.replay/label/'generated.hpp'
            expected=self.old_report['builds'][label]['generated_sha256']
            if base.sha(original)!=expected:raise ValueError('header hash '+label)
            shutil.copyfile(original,folder/'generated.hpp')
        else:
            self.command([os.getenv('FAUST','faust'),'-I',source.parent,'-e',source,'-o',folder/'expanded.dsp'])
            self.command([os.getenv('FAUST','faust'),'-I',source.parent,*flags,source,'-o',folder/'generated.hpp'])
        options=['-std=c++17','-O2','-ffp-contract=off','-I'+str(folder)]
        self.command([os.getenv('CXX','c++'),*options,ROOT/'tools/modules/render.cpp','-o',folder/'render'])
        text=self.command([folder/'render','--controls']);(folder/'controls.tsv').write_text(text)
        lines=text.splitlines();ch=int(lines[0].split('\t')[2]);ui={r.split('\t')[0]:list(map(float,r.split('\t')[1:])) for r in lines[1:]}
        self.uis[label]=dict(channels=ch,controls=ui)
        self.ck(label+':declared-ids',set(ui)<=set(self.d))
        if label in ('scalar','vector','direct','old-v1'):
            self.ck(label+':full-ABI',ch==1 and set(ui)==set(self.d))
            self.command([os.getenv('CXX','c++'),*options,ROOT/'tools/modules/clap_v2_benchmark.cpp','-o',folder/'benchmark'])
        for key,actual in ui.items():
            c=self.m['controls'][key]
            self.ck(label+':range:'+key,np.allclose(actual[:3],[c['min'],c['max'],c['default']],rtol=0,atol=1e-6))
        self.r['builds'][label]=dict(source=str(source.relative_to(ROOT)),source_sha256=base.sha(source),engine_sha256=base.sha(source.parent/'engine.lib'),generated_sha256=base.sha(folder/'generated.hpp'),faust_flags=flags,cxx_flags=options[:3],channels=ch,controls=list(ui))
        return folder/'render'
    def render(self,n,e,p=None,ev=None,r=48000,b=128,sec=1.3):
        if n.startswith('abl-'):sec=5.0
        values=self.d|(p or {});validate(values,self.m)
        rows={(0,k):v for k,v in values.items()};seen=set();frames=round(sec*r)
        for frame,k,v in ev or []:
            validate({k:v},self.m)
            if not isinstance(frame,int) or not 0<=frame<frames or (frame,k) in seen:raise ValueError('invalid/duplicate event')
            seen.add((frame,k));rows[frame,k]=v
        ui=self.uis[e.parent.name];rows={key:v for key,v in rows.items() if key[1] in ui['controls']}
        score=self.o/(n+'.tsv');raw=self.o/(n+'.f32')
        score.write_text(''.join(f'{frame}\t{k}\t{v:.9g}\n' for (frame,k),v in sorted(rows.items())))
        diag=json.loads(self.command([e,score,raw,r,b,frames,0]));x=np.fromfile(raw,dtype='<f4').reshape(frames,ui['channels'])
        if ui['channels']==1:x=x[:,0]
        diagnostic=e.parent.name in ('envelopes','body-envelope','phase-probe')
        peak=float(np.max(abs(x)));self.ck(n+':finite',np.isfinite(x).all() and (peak<.95 or diagnostic),peak=peak)
        self.r['renders'].append(dict(label=n,build=e.parent.name,rate=r,block=b,frames=frames,channels=ui['channels'],raw_sha256=base.sha(raw),score_sha256=base.sha(score),peak=peak,diagnostic=diagnostic,max_jump=float(np.max(abs(np.diff(x,axis=0)))),mean=float(np.mean(x)),diag=diag))
        return x
    def inherited(self):
        def scoped(args,t=300):
            if args==['git','rev-parse','HEAD'] and self.replay:return self.old_report['source_commit']
            if args==['faust','-v'] and self.replay:return 'not executed: verified generated-C++ replay'
            return self.command(args,t)
        before=base.cmd;base.cmd=scoped
        try:super().go()
        finally:base.cmd=before
        self.r['passed']=False
        self.r['legacy_metrics_note']='Inherited whole-file spectral descriptors retained only as history; new energy-weighted descriptors below govern reference discussion.'
    def extra(self):
        scalar=self.o/'scalar/render';vector=self.o/'vector/render';direct=self.build('direct','reference.dsp')
        env=self.build('envelopes','envelopes.dsp');benv=self.build('body-envelope','body-envelope.dsp');phase=self.build('phase-probe','phase-probe.dsp');old=self.build('old-v1')
        for rate in (44100,48000,96000):
            for level in (0.,1.):
                p={'spacing':level,'punch':1-level,'decay':level,'body_env':level};on=101
                x=self.render(f'envelopes-{rate}-{int(level)}',env,p,self.hit(on,1),r=rate,sec=20.)
                y=self.render(f'body-envelope-{rate}-{int(level)}',benv,p,self.hit(on,1),r=rate,sec=20.)
                t=np.arange(len(x)-on,dtype=float)/rate;a,b,c=envelope_oracle(t,self.d|p)
                self.ck(f'cluster-oracle:{rate}:{level}',np.max(abs(x[on:,0]-a))<4e-5,max_abs=float(np.max(abs(x[on:,0]-a))))
                for label,actual,expected in [('tail',x[on:,1],b),('body',y[on:],c)]:
                    mask=expected>1e-4;error=float(np.max(abs(actual[mask]-expected[mask])/expected[mask]))
                    self.ck(f'{label}-oracle:{rate}:{level}',error<2e-5,max_relative=error)
                self.ck(f'env-silence:{rate}:{level}',not np.any(x[:on]) and not np.any(y[:on]))
        pp={'pitch_hz':210.,'body':.5};on=101
        p=self.render('independent-phase',phase,pp,self.hit(on),sec=.2)
        increments=(210*(1.20+5.30*.25)/48000,105/48000)
        for j,inc in enumerate(increments):
            measured=np.mod(np.diff(p[on:,j]),1.)
            err=float(np.max(abs(measured-inc)))
            self.ck('independent-phase:'+str(j),err<2e-6,max_step_error=err)
        n=np.arange(9600);bad=np.mod((n*210/48000)%1*(1.20+5.30*.25),1)
        baderr=float(np.max(abs(np.mod(np.diff(bad),1)-increments[0])))
        self.ck('phase-oracle-rejects-v1-law',baderr>.1,old_law_max_step_error=baderr)
        errors=[]
        for rate in (44100,48000,96000):
            for name,patch in self.p.items():
                a=self.render(f'parity-{rate}-{name}-direct',direct,patch,self.hit(101),r=rate,sec=3.)
                b=self.render(f'parity-{rate}-{name}-lookup',scalar,patch,self.hit(101),r=rate,sec=3.)
                err=float(np.max(abs(a-b)));errors.append(err);self.ck('lookup:'+str(rate)+':'+name,err<3e-5,max_abs=err)
        self.r['max_direct_lookup_error']=max(errors)
        for name,patch in self.p.items():
            before=self.render('prepared-'+name,scalar,patch,self.hit(501),sec=.3)
            on=self.render('onset-'+name,scalar,ev=[(501,k,v) for k,v in patch.items()]+self.hit(501),sec=.3)
            self.ck('same-sample-lock:'+name,np.array_equal(before,on))
        zero=self.render('zero-velocity',scalar,{'velocity':0},self.hit(101),sec=.3);self.ck('zero-silent',not np.any(zero))
        self.render('late-retrigger',scalar,{'decay':1,'body_env':1},self.hit(101)+self.hit(1200001),sec=27.)
        bad=['0 unknown 1\n','0 drive nan\n','0 drive 2\n','0 gate .5\n','-1 gate 1\n','1 gate 1\n0 gate 0\n','0 gate 0\n0 gate 1\n','100 gate 1\n']
        for i,txt in enumerate(bad):
            path=self.o/f'invalid-{i}.tsv';path.write_text(txt)
            run=subprocess.run([str(scalar),str(path),str(self.o/f'invalid-{i}.f32'),'48000','128','100','0'],capture_output=True)
            self.ck('native-reject:'+str(i),run.returncode!=0)
        rate_report=[]
        for body in (0.,.5,1.):
            for drive in (0.,1.):
                patch={'balance':1,'body':body,'body_env':1,'pitch_hz':700,'drive':drive,'punch':0}
                a=self.render(f'rate48-{body}-{drive}',scalar,patch,self.hit(480),sec=1.)
                b=self.render(f'rate96-{body}-{drive}',scalar,patch,self.hit(960,128),r=96000,sec=1.)
                down=resample_poly(b.astype(float),1,2,window=('kaiser',10));sl=slice(9600,36000)
                db=float(20*np.log10(max(1e-15,np.sqrt(np.mean((a[sl]-down[sl])**2)))/max(1e-15,np.sqrt(np.mean(down[sl]**2)))))
                rate_report.append(dict(body=body,drive=drive,relative_rms_db=db))
        self.r['rate_consistency_not_alias_proof']=rate_report
        pieces=[];timeline=[]
        for name in ('Classic','Body','FM','Diffuse'):
            patch=self.p[name];a=self.render('old-'+name,old,patch,self.hit(480),sec=2.2);b=self.render('new-'+name,scalar,patch,self.hit(480),sec=2.2)
            timeline.append(dict(patch=name,v1_gain=1.,v2_gain=1.));pieces.extend([a,np.zeros(12000),b,np.zeros(12000)])
        base.writewav(self.o/'clap-v1-v2.wav',np.concatenate(pieces))
        self.r['auditions']['clap-v1-v2.wav']=timeline
        full=[];single=[];tailoff=[];bodyoff=[];attack=[]
        for name in self.p:
            rows=[]
            for tag in ('full','single','notail','nobody'):
                x=np.fromfile(self.o/f'abl-{tag}-{name}.f32',dtype='<f4');rows.append(x)
            full.append(features(rows[0],48000));single.append(features(rows[1],48000));tailoff.append(features(rows[2],48000));bodyoff.append(features(rows[3],48000))
            sl=slice(101,4901);den=max(np.sqrt(np.mean(rows[0][sl].astype(float)**2)),1e-12)
            attack.append(dict(patch=name,secondary_bursts_relative_rms=float(np.sqrt(np.mean((rows[0][sl]-rows[1][sl]).astype(float)**2))/den)))
        self.r['first_100ms_ablation']=attack
        if self.refs:
            refs=[];ref_names=[]
            manifest=json.loads((self.refs/'manifest.json').read_text())
            for row in manifest['records']:
                p=self.refs/(row['id']+'.wav')
                if base.sha(p)!=row['wav_sha256']:raise ValueError('reference hash')
                rate,x=wavfile.read(p);refs.append(features(x,rate));ref_names.append(row['id'])
            scale=np.array([.1,.35,.35,.35,.35,.35]);R=np.vstack(refs)
            coverage={}
            for label,pool in [('full',full),('single',single),('no_tail',tailoff),('no_body',bodyoff)]:
                dist=np.sqrt(np.mean(((R[:,None,:]-np.vstack(pool)[None,:,:])/scale)**2,axis=2));coverage[label]=dict(mean_nearest=float(dist.min(1).mean()),per_reference={n:dict(distance=float(dist[j].min()),patch=list(self.p)[int(dist[j].argmin())]) for j,n in enumerate(ref_names)})
            self.r['reference_context']=dict(coverage=coverage,scale=scale.tolist(),manifest_sha256=base.sha(self.refs/'manifest.json'),limits='12 public lossy CPV previews; unknown firmware/settings/gain. Eight authored anchors, no parameter search or held-out validation; broad context only.')
            seq=[];gains=[]
            for name in ref_names[::3]:
                rr=manifest['records'][ref_names.index(name)];rate,x=wavfile.read(self.refs/(name+'.wav'));x=np.asarray(x,float)
                if x.ndim>1:x=x.mean(1)
                if rate!=48000:x=resample_poly(x,48000,rate)
                selected=coverage['full']['per_reference'][name]['patch'];y=np.fromfile(self.o/f'abl-full-{selected}.f32','<f4')
                gx=safe_gain(x);gy=safe_gain(y);gains.append(dict(reference=name,anchor=selected,reference_gain=gx,candidate_gain=gy));seq +=[x*gx,np.zeros(12000),y*gy,np.zeros(12000)]
            base.writewav(self.o/'clap-reference-context.wav',np.concatenate(seq));self.r['auditions']['clap-reference-context.wav']=gains
        perf=[]
        for block in (32,64,128,512):
            reps=[]
            for rep in range(3):
                labels=['direct','scalar','vector'];labels=labels[rep:]+labels[:rep];row={}
                for label in labels:row[label]=json.loads(self.command([self.o/label/'benchmark',block]))
                self.ck(f'ordinary-new:{block}:{rep}',all(x['ordinary_new_allocations']==0 for x in row.values()));reps.append(row)
            perf.append(dict(block=block,repetitions=reps,lookup_vs_direct=statistics.median([r['direct']['p50_us']/r['scalar']['p50_us'] for r in reps]),vector_vs_lookup=statistics.median([r['scalar']['p50_us']/r['vector']['p50_us'] for r in reps])))
        self.r['performance']=perf;self.r['performance_scope']='Paired rotating warmed four-voice Linux DSP-only. Ordinary new/new[] hook, not malloc/aligned allocations, audio-device or thermal qualification.'
    def save(self,error):
        self.r['passed']=error is None;self.r['failure']=error;self.r['commands']=self.commands
        self.r['source_commit']=self.old_report['source_commit'] if self.replay else self.command(['git','rev-parse','HEAD']).strip()
        self.r['compilers']={'cxx':self.command([os.getenv('CXX','c++'),'--version']).splitlines()[0],'faust':'not run: generated-code replay' if self.replay else self.command([os.getenv('FAUST','faust'),'-v']).strip()}
        (self.o/'report.json').write_text(json.dumps(self.r,indent=2,allow_nan=False)+'\n')
        print(json.dumps(dict(passed=self.r['passed'],renders=len(self.r['renders']),checks=len(self.r['checks']),failure=error)))

def main():
    if not __debug__:raise SystemExit('Run without -O: inherited checks require assertions')
    p=argparse.ArgumentParser();p.add_argument('--out',required=True,type=Path);p.add_argument('--replay',type=Path);p.add_argument('--references',type=Path);a=p.parse_args()
    s=Study(a.out.resolve(),a.replay.resolve() if a.replay else None,a.references.resolve() if a.references else None);error=None
    try:s.inherited();s.extra()
    except Exception as e:error=str(e)
    finally:s.save(error)
    if error:raise SystemExit(error)
if __name__=='__main__':main()
