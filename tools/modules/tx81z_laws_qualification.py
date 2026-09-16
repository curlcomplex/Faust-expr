#!/usr/bin/env python3
"""#97: actual native ymfm core against actual Faust OPZ laws and musical voice.
55930-Hz comparisons are native core tests; 44.1/48/96k are candidate adaptation.
"""
from __future__ import annotations
import argparse,hashlib,json,os,re,shutil,tarfile,wave
from pathlib import Path
import numpy as np
from scipy.signal import resample_poly
from tx81z_routing_qualification import RoutingQualification,ROOT,identity,sha,IPP_SHA
from tx81z_wave_qualification import RENDERER_SHA,ARCHIVE_SHA
from tx81z_prepare import prepare, standalone_source
from tx81z_programs import BASE,PROGRAMS
from lab import wav
NATIVE_RATE=55930
FLOAT_TOL=1e-7 # Float output multiplication only; integer diagnostic tolerance is zero.

def residual(a,b):
    a,b=np.asarray(a),np.asarray(b)
    if a.shape!=b.shape or not a.size or not np.isfinite(a).all() or not np.isfinite(b).all():
        raise ValueError('matched finite nonempty recordings required')
    d=a.astype('float64')-b
    return float(np.max(abs(d))),float(np.sqrt(np.mean(d*d)))

class LawsQualification(RoutingQualification):
    def build(self,name,source,vector=False):
        # The prior analytic checkpoints still use their unchanged -e route.
        # Native-law integer DSP compiles directly: -e needlessly duplicates
        # nested state expressions into >500 MiB before reducing to 56 KiB C++.
        d=self.out/name;d.mkdir()
        flags=['-lang','cpp','-single','-cn','ModuleDSP']
        if vector:flags+=['-vec','-lv','0','-vs','32']
        self.run([self.a.faust,'-I',self.a.libraries,'-I',source.parent,*flags,source,'-o',d/'generated.hpp'])
        shutil.copy2(self.a.renderer,d/'render.cpp')
        self.run([self.a.cxx,'-std=c++17','-O2','-ffp-contract=off','-I'+str(d),d/'render.cpp','-o',d/'render'])
        (d/'controls.tsv').write_text(self.run([d/'render','--controls']))
        self.builds[name]={'files':identity(d),'source':str(source),'source_sha256':sha(source),
            'faust_flags':flags,'source_route':'direct, not -e text round-trip'}
        return d/'render'

    def render(self,name,exe,params=None,events=(),rate=NATIVE_RATE,seconds=.7,block=128,input_data=None):
        frames=len(input_data) if input_data is not None else round(rate*seconds)
        score=self.out/(name+'.tsv');raw=self.out/(name+'.f32')
        ev=[(0,k,v) for k,v in (params or {}).items()]+list(events);ev.sort(key=lambda e:e[0])
        if len({(i,k) for i,k,v in ev})!=len(ev):raise ValueError('duplicate score event')
        score.write_text(''.join(f'{i}\t{k}\t{v:.12g}\n' for i,k,v in ev))
        cmd=[exe,score,raw,rate,block,frames,0]
        if input_data is not None:
            ip=self.out/(name+'-input.f32');np.asarray(input_data,dtype='<f4').tofile(ip);cmd.append(ip)
        diag=json.loads(self.run(cmd));ch=diag['channels'];x=np.fromfile(raw,dtype='<f4')
        self.check(name+':length',x.size==frames*ch);self.check(name+':finite',np.isfinite(x).all())
        self.renders.append({'name':name,'raw':raw.name,'raw_sha256':sha(raw),'score':score.name,'score_sha256':sha(score),
            'rate':rate,'frames':frames,'channels':ch,'diagnostics':diag,'kind':'audio' if ch==1 else 'operator-state, NOT audio'})
        return x if ch==1 else x.reshape(frames,ch)

    def native_build(self,name,probe=False):
        d=self.out/name;d.mkdir()
        shutil.copy2(ROOT/'tools/modules/ymfm_opz_voice_reference.hpp',d/'generated.hpp');shutil.copy2(self.a.renderer,d/'render.cpp')
        self.run([self.a.cxx,'-std=c++17','-O2','-ffp-contract=off','-DTX81Z_NATIVE_PROBE='+str(int(probe)),
            '-I'+str(self.a.ymfm/'src'),d/'render.cpp',self.a.ymfm/'src/ymfm_opz.cpp','-o',d/'render'])
        (d/'controls.tsv').write_text(self.run([d/'render','--controls']))
        self.builds[name]={'files':identity(d),'kind':'unchanged native ymfm engine with transport adapter'}
        return d/'render'

    def pair(self,name,params,events,seconds=.7,probe=False):
        a=self.render(name+'-faust',self.probe if probe else self.scalar,params,events,seconds=seconds)
        b=self.render(name+'-ymfm',self.native_probe if probe else self.native,params,events,seconds=seconds)
        maximum,rms=residual(a,b);tol=0 if probe else FLOAT_TOL
        self.comparisons.append({'name':name,'kind':'operator-state' if probe else 'whole core audio',
            'max_abs':maximum,'rms_error':rms,'tolerance':tol,'samples':a.size})
        self.check(name+':native-match',maximum<=tol,max_abs=maximum,rms_error=rms,tolerance=tol)
        return a,b

    def listening(self,name,arrays):
        pieces=[]
        for a in arrays:pieces += [resample_poly(a.astype('float64'),4800,5593)*.8,np.zeros(12000)]
        x=np.concatenate(pieces);p=self.out/'listening'/name;wav(p,x,48000)
        with wave.open(str(p),'rb') as w:
            self.check(name+':format',w.getnchannels()==1 and w.getsampwidth()==2 and w.getframerate()==48000)
            pcm=np.frombuffer(w.readframes(w.getnframes()),dtype='<i2')
        self.check(name+':PCM',np.array_equal(pcm,np.round(x*32767).astype('<i2')))
        self.listening_index.append({'file':name,'sha256':sha(p),'gain':.8,'order':'native then Faust',
            'source_rate':55930,'output_rate':48000,'resampling':'same scipy.resample_poly 4800/5593 for both halves'})

    def execute(self):
        self.check('renderer:unchanged',sha(self.a.renderer)==RENDERER_SHA)
        self.check('ymfm:pin',sha(self.a.ymfm/'src/ymfm_fm.ipp')==IPP_SHA)
        before=identity(self.a.ymfm/'src')
        self.report['compiler']={'version':self.run([self.a.faust,'--version']),'sha256':sha(self.a.faust),'cxx':self.run([self.a.cxx,'--version'])}
        self.check('faust:2.88.0','FAUST Version 2.88.0\n' in self.report['compiler']['version'])
        if self.a.archive:
            self.check('archive:pin',sha(self.a.archive)==ARCHIVE_SHA)
            with tarfile.open(self.a.archive) as t:
                m={m.name.split('/libraries/',1)[1]:hashlib.sha256(t.extractfile(m).read()).hexdigest() for m in t.getmembers()
                    if m.isfile() and '/libraries/' in m.name and m.name.endswith('.lib')}
            self.check('libraries:complete-official',m==self.library_identity)
        else:self.report['library_verification']='retained verified prior hosted artifact, not newly downloaded full archive'
        self.report['native_rate']={'rate':55930,'input_clock':3579545,'meaning':'upstream integer sample_rate(clock), not measured TX81Z clock'}
        self.report['oracle']={'status':'running','commit':'81aec25ccbb98f4873a255f7551ac4dadac59b4a',
            'kind':'actual unchanged fm_engine_base<opz_registers> channel 0, pre-DAC','source_sha256':before,
            'limits':['no firmware or SysEx','no hardware','LFO/noise/EG-shift off','55930-Hz native comparisons only']}
        self.report['schema']='faust-expr/tx81z-native-laws/v1'
        self.report['playback_gain']=.8
        self.report['scope']='OPZ frequency, operator envelopes, logarithmic levels and feedback, not complete TX81Z or device acceptance'
        self.comparisons=[];self.listening_index=[];self.report['comparisons']=self.comparisons;self.report['listening']=self.listening_index
        src=self.module/'v5/voice.dsp'
        self.scalar=self.build('v5-scalar',src);self.vector=self.build('v5-vector',src,True)
        self.probe=self.build('v5-probe',self.module/'v5/operator_probe.dsp')
        self.native=self.native_build('native');self.native_probe=self.native_build('native-probe',True)
        self.check('controls:match',(self.out/'v5-scalar/controls.tsv').read_text()==(self.out/'native/controls.tsv').read_text())
        (self.out/'listening').mkdir();gate=[(2400,'gate',1),(22000,'gate',0)]
        for alg in range(1,9):
            for w in range(8):
                p=BASE|{'algorithm':alg,'blockFreq':4625,'level':1,'velocity':1,**{f'op{i}Wave':(w+i-1)%8 for i in range(1,5)}}
                a,b=self.pair(f'route-{alg}-waves-{w}',p,gate)
                self.check(f'route-{alg}-waves-{w}:audible',np.max(abs(a))>.001)
        for i in range(32):
            p=BASE|{'algorithm':1+i%8,'blockFreq':((i%8)<<10)+(((i*3)%16)<<6)+(i*17)%64,'level':1,'velocity':1}
            for op in range(1,5):
                p.update({f'op{op}Mode':int(i%3==0),f'op{op}Coarse':[0,1,7,15][(i+op)%4],f'op{op}Fine':(i+3*op)%16,
                    f'op{op}DT1':(i+op)%8,f'op{op}DT2':(i+op)%4,f'op{op}Range':(i+op)%8})
            self.pair('frequency-'+str(i),p,gate)
        for i in range(32):
            p={'blockFreq':((i%8)<<10)+((i%3)<<6)+(i*7)%64,'op1AR':[0,1,10,20,30,31][i%6],
                'op1D1R':[0,12,31][i%3],'op1D2R':[0,9,31][(i//3)%3],'op1SL':[0,7,14,15][i%4],
                'op1RR':[0,1,7,15][(i//4)%4],'op1KS':i%4,'op1Reverb':[0,1,3,7][(i//3)%4],
                'op1Mode':i%2,'op1Coarse':i%16,'op1Fine':(i*7)%16,'op1DT1':i%8,'op1DT2':i%4,'op1Range':i%8,'op1Wave':i%8,'op1TL':i*3%128}
            self.pair('operator-state-'+str(i),p,[(31,'gate',1),(23003,'gate',0),(29001,'gate',1),(29002,'gate',0)],seconds=.8,probe=True)
        for ar in [0,20,31]:
            self.pair('live-attack-'+str(ar),{'blockFreq':4096,'op1AR':ar,'op1SL':0,'op1D2R':0},
                [(37,'gate',1),(407,'op1AR',31),(8003,'op1AR',20),(17009,'gate',0)],probe=True)
        for fb in range(8):self.pair('feedback-'+str(fb),BASE|{'feedback':fb,'algorithm':1+fb,'op4TL':8,'blockFreq':3840},gate)
        ev=[(2400,'gate',1),(5901,'algorithm',8),(8107,'algorithm',3),(12003,'feedback',7),
            (17001,'op4Mode',1),(17001,'op4Range',3),(19503,'op4Mode',0),(22003,'gate',0),
            (23011,'op2TL',12),(23011,'op2Wave',7),(26509,'op2TL',50),(29013,'gate',1),(31001,'freq',330),(35011,'gate',0)]
        p=BASE|{'feedback':3,'blockFreq':-1};a,b=self.pair('persistent',p,ev,seconds=1)
        for exe,label in [(self.scalar,'cold'),(self.vector,'vector'),(self.native,'native-cold')]:
            x=self.render('persistent-'+label,exe,p,ev,seconds=1);self.check('persistent:'+label,residual(x,a)[0]<=FLOAT_TOL,max_abs=residual(x,a)[0])
        for block in [1,127,256,511]:
            for exe,label,truth in [(self.scalar,'faust',a),(self.native,'ymfm',b)]:
                x=self.render(f'partition-{label}-{block}',exe,p,ev,seconds=1,block=block)
                self.check(f'partition-{label}-{block}:identity',np.array_equal(x,truth))
        for alg in range(1,9):
            x=self.render('zero-velocity-'+str(alg),self.scalar,BASE|{'algorithm':alg,'velocity':0},gate)
            self.check('zero-velocity-'+str(alg)+':silent',np.max(abs(x))==0)
        x=self.render('never-triggered',self.scalar,BASE,(),seconds=2);self.check('never-triggered:silent',np.max(abs(x))==0)
        a,b=self.pair('release-and-settle',BASE|{f'op{i}RR':15 for i in range(1,5)},[(2400,'gate',1),(22001,'gate',0)],seconds=2)
        self.check('startup:silent',np.max(abs(a[:2400]))==0);self.check('release:not-cut',np.max(abs(a[22001:22300]))>.0001)
        self.check('release:settled',np.max(abs(a[-5000:]))==0)
        rates=[]
        for rate in [44100,48000,96000]:
            p={'algorithm':8,'op1TL':0,'op1AR':31,'op1D1R':0,'op1D2R':0,'op1SL':0,**{f'op{i}AR':0 for i in [2,3,4]},'level':1}
            for freq in [55,220,1760]:
                ev=[(100,'gate',1),(rate+100,'gate',0)]
                x=self.render(f'rate-{rate}-{freq}',self.scalar,p|{'freq':freq},ev,rate=rate,seconds=1.2)
                seg=x[round(rate*.2):round(rate*.8)];spec=abs(np.fft.rfft(seg*np.hanning(len(seg)),n=rate*8));hz=(np.argmax(spec[1:])+1)/8
                self.check(f'rate-{rate}-{freq}:pitch',abs(hz-freq)<max(.3,freq*.003),measured_hz=float(hz),target_hz=freq)
                v=self.render(f'rate-vector-{rate}-{freq}',self.vector,p|{'freq':freq},ev,rate=rate,seconds=1.2)
                self.check(f'rate-{rate}-{freq}:vector',residual(x,v)[0]<=FLOAT_TOL,max_abs=residual(x,v)[0])
                rates.append({'rate':rate,'requested_hz':freq,'measured_hz':float(hz),'native_parity':False})
        self.report['host_rate_checks']=rates
        # Retain a counterexample to the upstream comment that labels the
        # fixed register number as Hz: actual native phase is 10.10, not 10.0.
        fixed={'algorithm':8,'op1Mode':1,'op1Coarse':8,'op1Fine':0,'op1Range':6,
            'op1TL':0,'op1AR':31,'op1D1R':0,'op1D2R':0,'op1SL':0,'level':1,
            **{f'op{i}AR':0 for i in [2,3,4]}}
        a,b=self.pair('fixed-unit-counterexample',fixed,[(100,'gate',1)],seconds=2)
        peak_bin=int(np.argmax(abs(np.fft.rfft(b[100:]))[1:])+1)
        measured=peak_bin*NATIVE_RATE/len(b[100:]);predicted=NATIVE_RATE*75*8192/(4096*1048576)
        self.check('fixed:predicted-native-frequency',abs(measured-predicted)<.3,measured_hz=measured,predicted_hz=predicted)
        self.check('fixed:reject-register-number-as-hz',abs(measured-8192)>8000)
        self.report['fixed_frequency_counterexample']={'register_number':8192,'measured_hz':measured,
            'native_phase_prediction_hz':predicted,'source':'ymfm_opz.cpp compute_phase_step and ymfm_fm.h phase()',
            'verdict':'Preserved native behavior conflicts with Hz interpretation in upstream comment; hardware fixed-mode acceptance remains OPEN.'}

        text=src.read_text().replace('library("opz_core.lib")',f'library("{src.parent/"opz_core.lib"}")')
        mutant=text.replace('int(opz.r.in1(a,o4,o3,o2))>>1','int(opz.r.in1(a,o4,o3,o2))>>2')
        self.check('mutant:source-diff',mutant!=text);mp=self.source('wrong-modulation-units',mutant);me=self.build('wrong-modulation-build',mp)
        a,b=self.pair('modulation-unit-control',BASE|{'algorithm':1,'level':1},gate)
        x=self.render('wrong-modulation-render',me,BASE|{'algorithm':1,'level':1},gate);error=residual(x,b)[0]
        self.check('mutant:rejected',error>FLOAT_TOL,max_abs=error);self.report['wrong_modulation_units_max_abs']=error
        for name,p,notes,spacing in PROGRAMS:
            ev=[]
            for i,note in enumerate(notes):
                frame=2400+round(i*spacing*NATIVE_RATE)
                ev += [(frame,'freq',440*2**((note-69)/12)),(frame,'velocity',.65+.1*(i%4)),(frame,'gate',1),(frame+round(spacing*.68*NATIVE_RATE),'gate',0)]
            a,b=self.pair(name,p,ev,seconds=spacing*len(notes)+1.3);self.listening(name+'.wav',[b,a])
        a,b=self.pair('06-live-release',BASE|{'algorithm':5,'op1RR':4,'op3RR':4},[(100,'gate',1),(12000,'gate',0),(15001,'op2TL',7),(19003,'op2TL',38)],seconds=1.3)
        self.listening('06-live-release.wav',[b,a])
        text,dependencies=standalone_source(src)
        export=self.out/'TX81Z_OPZ.dsp';export.write_text(text);ex=self.build('single-file-export',export)
        self.check('export:controls',(self.out/'single-file-export/controls.tsv').read_text()==(self.out/'v5-scalar/controls.tsv').read_text())
        ready=self.out/'ready-source';ready.mkdir()
        for path in ['v5/voice.dsp','v5/opz_core.lib','v5/opz_tables.lib','v4/opz_routes.lib','v2/opz_core.lib','YMFM-LICENSE.txt']:
            dest=ready/path;dest.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(self.module/path,dest)
        (ready/'dependency-hashes.json').write_text(json.dumps(dependencies,indent=2)+'\n')
        (self.out/'programs.json').write_text(json.dumps(PROGRAMS,indent=2)+'\n')
        p=BASE|{'feedback':3};a=self.render('export-baseline',self.scalar,p,gate);b=self.render('export-replay',ex,p,gate)
        self.check('export:audio',np.array_equal(a,b),max_abs=residual(a,b)[0]);self.report['export']={'file':export.name,'sha256':sha(export)}
        size=self.out/'v5-scalar/size.cpp';size.write_text('#define main existing_renderer_main\n#include "render.cpp"\n#undef main\nint main(){std::cout<<sizeof(ModuleDSP)<<"\\n";}\n')
        self.run([self.a.cxx,'-std=c++17','-O2',size,'-o',size.parent/'size']);self.report['object_bytes']=int(self.run([size.parent/'size']).strip())
        header=(self.out/'v5-scalar/generated.hpp').read_text()
        arrays=re.findall(r'(?:const )?static int (\w+)\[(\d+)\]',header)
        self.report['static_integer_arrays']={name:int(count) for name,count in arrays}
        self.report['declared_static_table_bytes']=sum(int(n) for _,n in arrays)*4
        self.report['memory_boundary']='Object and declared initializer/runtime lookup arrays only; compiler may eliminate constants; stack, buffers and host excluded. classInit once before concurrent instance rendering.'
        timings={}
        for repeat in range(3):
            for label,exe in ([('faust',self.scalar),('native',self.native)] if repeat%2==0 else [('native',self.native),('faust',self.scalar)]):
                self.render(f'cost-{label}-{repeat}',exe,BASE,[(100,'gate',1)],seconds=3,rate=48000 if label=='faust' else NATIVE_RATE)
                timings.setdefault(label,[]).append(self.renders[-1]['diagnostics']['instrumented_compute_ns'])
        self.report['cost']={'compute_ns':timings,'seconds_per_render':3,'requested_block':128,
            'rates':{'faust':48000,'native':NATIVE_RATE},'note':'Offline instrumented compute including native transport overhead. Rates differ: NOT a speedup comparison or owner-device deadline test.'}

        self.check('sources:unchanged',self.initial_source==identity(self.module));self.check('libraries:unchanged',self.library_identity==identity(self.a.libraries,'*.lib'))
        self.check('ymfm:unchanged',before==identity(self.a.ymfm/'src'));self.report['oracle']['status']='completed';self.report['status']='completed'

def main():
    p=argparse.ArgumentParser(description=__doc__)
    for n in ['out','faust','libraries','renderer','ymfm']:p.add_argument('--'+n,type=Path,required=True)
    p.add_argument('--archive',type=Path);p.add_argument('--cxx',default=os.getenv('CXX','c++'));a=p.parse_args()
    for n in ['faust','libraries','renderer','ymfm','archive']:
        if getattr(a,n) is not None:setattr(a,n,getattr(a,n).resolve())
    prepared=prepare(a.ymfm,ROOT/'modules/tx81z/v5/opz_tables.lib');q=LawsQualification(a);q.report['prepared_tables']=prepared
    try:q.execute()
    except Exception as e:q.report['status']='failed';q.report['error']=str(e);raise
    finally:q.save()
    print(json.dumps({'status':q.report['status'],'checks':len(q.checks),'renders':len(q.renders),'comparisons':len(q.comparisons)}))
if __name__=='__main__':main()
