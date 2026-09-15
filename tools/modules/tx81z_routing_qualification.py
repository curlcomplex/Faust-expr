#!/usr/bin/env python3
"""#97: runtime OPZ routing; topology fixture + real compiled-Faust auditions.

The native reference executes the unchanged ymfm output_4op body with tagged
operator test doubles. It validates routes, NOT chip envelopes, waves or audio.
The static-graph differential reference is another Faust build, NOT hardware.
"""
from __future__ import annotations
import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tarfile
import time
import wave
import numpy as np
from tx81z_wave_qualification import Qualification, identity, sha, ROOT, PIN, RENDERER_SHA, ARCHIVE_SHA
from lab import wav

IPP_SHA = '4fb7fe59d4494a19e9f9c7888eb644a63d86e109fdefb27a8117bf6a5f6ab4b9'
STATIC_BLOB = '27d54f429e49ac6d449926d30a043f24aa860ad9'
AUDIO_TOLERANCE = 2e-4  # a numerical replay tolerance, not a sonic metric


def extract_routing_body(text: str) -> str:
    hits = re.findall(r'template<class RegisterType>\nvoid fm_channel<RegisterType>::output_4op\(.*?\n}\n', text, re.S)
    if len(hits) != 1 or 's_algorithm_ops[8+4]' not in hits[0]:
        raise ValueError('expected exactly one pinned output_4op definition')
    return hits[0]


def parse_routes(csv: str) -> np.ndarray:
    if not csv or csv.splitlines()[0] != 'algorithm,basis,in2,in3,in4,carrier':
        raise ValueError('unexpected route CSV header')
    rows = np.loadtxt(io.StringIO(csv), delimiter=',', skiprows=1)
    if rows.shape != (32,6) or not np.isfinite(rows).all():
        raise ValueError('expected 32 finite route rows')
    if not (np.array_equal(rows[:,0],np.repeat(np.arange(8),4)) and
            np.array_equal(rows[:,1],np.tile(np.arange(4),8)) and
            np.isin(rows[:,2:], [0,1]).all()):
        raise ValueError('wrong route ordering or nonbinary result')
    return rows[:,2:].reshape(8,4,4).transpose(0,2,1)


def same_matrix(a, b) -> bool:
    a,b=np.asarray(a),np.asarray(b)
    if a.shape!=(8,4,4) or b.shape!=a.shape or not np.isfinite(a).all() or not np.isfinite(b).all():
        raise ValueError('expected finite 8 x 4 x 4 routing matrices')
    return bool(np.array_equal(a,b))


class RoutingQualification(Qualification):
    def run(self, args, cwd=ROOT, timeout=180):
        args=[str(a) for a in args]; start=time.monotonic()
        timed_out=False
        try:
            p=subprocess.run(args,cwd=cwd,capture_output=True,text=True,timeout=timeout)
            stdout,stderr,code=p.stdout,p.stderr,p.returncode
        except subprocess.TimeoutExpired as e:
            def decoded(v): return v.decode(errors='replace') if isinstance(v,bytes) else v or ''
            stdout,stderr,code=decoded(e.stdout),decoded(e.stderr),None
            timed_out=True
        i=len(self.commands)
        for kind,text in [('stdout',stdout),('stderr',stderr)]:
            (self.out/f'command-{i:03d}.{kind}').write_text(text)
        self.commands.append({'argv':args,'cwd':str(cwd),'returncode':code,'timed_out':timed_out,
                              'wall_seconds':time.monotonic()-start,
                              'stdout':f'command-{i:03d}.stdout','stderr':f'command-{i:03d}.stderr'})
        if timed_out or code:
            raise RuntimeError(f'command failed (timeout={timed_out}, code={code}): {args}; {stderr[-3000:]}')
        return stdout

    def source(self, name, text):
        path=self.out/(name+'.dsp');path.write_text(text);return path

    def topology(self):
        ipp=self.a.ymfm/'src/ymfm_fm.ipp'
        self.check('ymfm:source-pin',sha(ipp)==IPP_SHA)
        body=extract_routing_body(ipp.read_text())
        wrapper=ROOT/'tools/modules/ymfm_opz_route_fixture.cpp'
        def fixture(name, content):
            d=self.out/name;d.mkdir()
            (d/'routing_body.inc').write_text(content)
            shutil.copy2(wrapper,d/'fixture.cpp')
            exe=d/'fixture'
            self.run([self.a.cxx,'-std=c++17','-O2','-I'+str(self.a.ymfm/'src'),'-I'+str(d),d/'fixture.cpp','-o',exe])
            result=self.run([exe]);(d/'routes.csv').write_text(result)
            return exe,result
        exe,csv=fixture('native-topology',body)
        self.check('native-topology:cold-repeat',self.run([exe])==csv)
        expected=parse_routes(csv)
        self.report['oracle']={'status':'completed','kind':'unchanged ymfm routing body with tagged operator test doubles; NOT chip audio',
                               'commit':PIN,'source_sha256':sha(ipp),'excerpt_sha256':sha(exe.parent/'routing_body.inc'),
                               'binary_sha256':sha(exe),'wrapper_sha256':sha(wrapper),
                               'matrix':expected.tolist(),
                               'tag_policy':'even operator output 2, production PM >>1; carrier sum /2, no overflow/clipping'}
        # Native mutants prove the readout notices one missing edge/carrier.
        for name,old,new in [
            ('missing-edge','ALGORITHM(1,2,3, 0,0,0)','ALGORITHM(0,2,3, 0,0,0)'),
            ('missing-carrier','ALGORITHM(0,0,0, 1,1,1)','ALGORITHM(0,0,0, 0,1,1)')]:
            altered=body.replace(old,new,1)
            self.check(name+':mutation-applied',altered!=body)
            _,text=fixture('mutant-'+name,altered)
            self.check(name+':detected',not same_matrix(expected,parse_routes(text)))
        core=self.module/'v4/opz_routes.lib'
        probe=self.source('topology-probe',f'''opz=library("{core}");
a=nentry("a",0,0,7,1); p=nentry("probe",0,0,3,1);
process(o4,o3,o2,o1)= select2(p>=2,
    select2(p>=1,opz.in3(a,o4),opz.in2(a,o4,o3)),
    select2(p>=3,opz.in1(a,o4,o3,o2),opz.carriers(a,o4,o3,o2,o1)));
''')
        exe=self.build('topology-probe-build',probe)
        actual=np.empty((8,4,4))
        for a in range(8):
            for p in range(4):
                actual[a,p]=self.render(f'route-{a}-{p}',exe,{'a':a,'probe':p},input_data=np.eye(4,dtype=np.float32))
                self.check(f'route:{a}:{p}:exact',np.array_equal(actual[a,p],expected[a,p]))
        self.check('routing:all-128-coefficients',same_matrix(actual,expected))
        self.check('routing:gain-mutant-detected',not same_matrix(actual*.5,expected))
        self.report['candidate_routing_matrix']=actual.tolist()

    def static_build(self, algorithm):
        # Preserve the archived v3 explicit equations as a same-Faust reference.
        # Those equations number operators in ymfm's evaluation order. Reverse
        # their control arguments to compare the v2/v4 carrier-first interface.
        core=self.module/'v3/opz_core.lib'
        data=core.read_bytes()
        git_blob=hashlib.sha1(f'blob {len(data)}\0'.encode()+data).hexdigest()
        self.check(f'static-{algorithm}:pinned-source',git_blob==STATIC_BLOB)
        text=(self.module/'v4/voice.dsp').read_text()
        text=text.replace('library("opz_routes.lib")',f'library("{core}")')
        original='opz.voice(a,freq,ratio1,ratio2,ratio3,ratio4,level1,level2,level3,level4,wave1,wave2,wave3,wave4)'
        reference=f'opz.alg{algorithm}(freq,ratio4,ratio3,ratio2,ratio1,level4,level3,level2,level1,wave4,wave3,wave2,wave1)'
        self.check(f'static-{algorithm}:one-replacement',text.count(original)==1)
        return self.build(f'static-{algorithm}',self.source(f'static-source-{algorithm}',text.replace(original,reference)))

    def audition(self, name, arrays, gap=.2):
        silence=np.zeros(round(48000*gap))
        raw=np.concatenate([np.concatenate([x,silence]) for x in arrays])*.5
        path=self.out/'listening'/name
        wav(path,raw,48000)
        with wave.open(str(path),'rb') as f:
            self.check(name+':PCM16-format',f.getsampwidth()==2 and f.getnchannels()==1 and f.getframerate()==48000)
            actual=np.frombuffer(f.readframes(f.getnframes()),dtype='<i2')
        self.check(name+':PCM16-samples',np.array_equal(actual,np.rint(raw*32767).astype('<i2')))

    def execute(self):
        self.report['schema']='faust-expr/tx81z-runtime-routing/v1'
        self.report['scope']='Eight selectable topologies in four running Faust operators; v2 waves and provisional ADSR, not complete TX81Z.'
        self.report['audio_comparison_tolerance']=AUDIO_TOLERANCE
        self.check('renderer:pinned-unchanged',sha(self.a.renderer)==RENDERER_SHA)
        v=self.run([self.a.faust,'--version'])
        self.check('compiler:2.88.0','FAUST Version 2.88.0\n' in v)
        self.report['compiler']={'version':v,'sha256':sha(self.a.faust),'cxx_version':self.run([self.a.cxx,'--version'])}
        if self.a.archive:
            self.check('archive:sha256',sha(self.a.archive)==ARCHIVE_SHA)
            with tarfile.open(self.a.archive) as t:
                members={m.name.split('/libraries/',1)[1]:hashlib.sha256(t.extractfile(m).read()).hexdigest()
                         for m in t.getmembers() if m.isfile() and '/libraries/' in m.name and m.name.endswith('.lib')}
            self.check('libraries:complete-release',members==self.library_identity)
            self.report['library_verification']='complete official release archive'
        else:
            self.report['library_verification']='retained verified artifact; complete archive not rechecked in this supplementary run'
        upstream_before=identity(self.a.ymfm/'src')
        self.topology()
        scalar=self.build('runtime',self.module/'v4/voice.dsp')
        vector=self.build('runtime-vector',self.module/'v4/voice.dsp',True)
        v2=self.build('v2-preserved',self.module/'v2/voice.dsp')
        generated=(scalar.parent/'generated.hpp').read_text()
        count=generated.count('std::sin(')
        self.check('one-voice:four-sine-evaluations',count==4,count=count)
        self.report['architecture']={'sine_evaluations_in_scalar_generated_cpp':count,
                                    'runtime_algorithm_control':'1..8; source routes 0..7; OP1 carrier-first',
                                    'generic_voice_allocation':'host-owned'}
        controls=self.run([scalar,'--controls']).splitlines()
        names={line.split('\t')[0] for line in controls[1:]}
        self.check('host:note-contract',controls[0]=='io\t0\t1' and {'gate','freq','velocity','algorithm'}<=names)
        (self.out/'listening').mkdir()
        self.report['comparisons']=[]
        gate=[(2400,'gate',1),(36003,'gate',0)]
        # Full eight-wave/routing cross-product with asymmetric ratios/levels.
        for a in range(8):
            static=self.static_build(a)
            for w in range(8):
                params={'freq':110,'op1Ratio':1,'op2Ratio':2.5,'op3Ratio':1.5,'op4Ratio':3,
                        'op1Level':.55,'op2Level':.21,'op3Level':.17,'op4Level':.13,
                        **{f'op{i+1}Wave':(w+i)%8 for i in range(4)}}
                x=self.render(f'runtime-{a}-wave{w}',scalar,params|{'algorithm':a+1},gate)
                y=self.render(f'static-{a}-wave{w}',static,params,gate)
                error=float(np.max(np.abs(x-y)))
                self.check(f'whole-voice:{a}:{w}:parity',error<=AUDIO_TOLERANCE,max_abs=error)
                self.report['comparisons'].append({'algorithm':a+1,'wave_seed':w,'max_abs':error})
            x=self.render(f'algorithm-{a+1}-audition',scalar,params|{'algorithm':a+1},gate)
            if a==0: algorithm_audio=[]
            algorithm_audio.append(x)
        self.audition('01-eight-algorithms-1-to-8.wav',algorithm_audio)
        self.report['static_reference']={'kind':'archived explicit v3 equations; same Faust waveform/phase engine, not independent chip audio',
                                         'blob':STATIC_BLOB,'operator_mapping':'v3 evaluation O1/O2/O3/O4 = v4 OP4/OP3/OP2/OP1'}
        # No silent revoicing: old controls have the same meaning in serial mode.
        for w in range(8):
            params={f'op{i+1}Wave':(w+i)%8 for i in range(4)}
            old=self.render(f'v2-identity-{w}',v2,params,gate)
            new=self.render(f'v4-identity-{w}',scalar,params,gate)
            self.check(f'v2:serial-wave{w}:exact',np.array_equal(old,new))
        self.audition('02-v2-then-v4-serial-preserved.wav',[old,new])
        # Exercise all eight algorithm changes on one continuing instance.
        cases=[]
        for i in range(8):
            frame=2401+i*6001
            cases += [(frame,'algorithm',i+1),(frame,'gate',1),(frame+3003,'gate',0)]
        params={'freq':143,'op1Wave':7,'op2Wave':6,'op3Wave':3,'op4Wave':1,'release':.025}
        base=self.render('switching',scalar,params,cases,seconds=1.5)
        repeat=self.render('switching-cold',scalar,params,cases,seconds=1.5)
        self.check('switching:cold-exact',np.array_equal(base,repeat))
        for block in [1,127,256,511]:
            x=self.render(f'switching-block-{block}',scalar,params,cases,block=block)
            self.check(f'switching:block-{block}:exact',np.array_equal(base,x))
        x=self.render('switching-vector',vector,params,cases)
        self.check('switching:vector-parity',np.max(np.abs(base-x))<=AUDIO_TOLERANCE,max_abs=float(np.max(np.abs(base-x))))
        self.check('startup:exact-silence',np.max(np.abs(base[:2401]))==0)
        self.check('release:finished',np.max(np.abs(base[-4800:]))<1e-5)
        self.audition('03-one-voice-algorithm-changes.wav',[base])
        for a in range(1,9):
            p={'algorithm':a,'op1Level':1,'op2Level':1,'op3Level':1,'op4Level':1,'freq':7500,
               'op1Ratio':16,'op2Ratio':.5,'op3Ratio':15.5,'op4Ratio':1.5}
            for rate in [44100,48000,96000]:
                events=[(17,'gate',1),(rate//8,'gate',0)]
                x=self.render(f'corner-{a}-{rate}',scalar,p,events,rate=rate,seconds=.3)
                self.check(f'corner-{a}-{rate}:bounded',np.max(np.abs(x))<2)
            x=self.render(f'zero-velocity-{a}',scalar,p|{'velocity':0},gate,seconds=1.5)
            self.check(f'velocity-zero-{a}:silent',np.max(np.abs(x))==0)
        # Gate, release-period modulation, and parameter-event segmentation.
        params={'algorithm':6,'op4Level':.05,'release':.7}
        events=[(2400,'gate',1),(16001,'gate',0)]
        dry=self.render('release-steady',scalar,params,events)
        live=self.render('release-modulated',scalar,params,events+[(19003,'op4Level',.2)])
        self.check('release:modulation-live',np.max(np.abs(dry[19500:25000]-live[19500:25000]))>.001)
        self.check('release:no-early-change',np.array_equal(dry[:19003],live[:19003]))
        self.audition('07-release-steady-then-modulated.wav',[dry,live])
        programs=[
            ('04-dual-pair-bass.wav',{'algorithm':5,'op1Level':.8,'op3Level':.28,'op3Ratio':.5,'op2Ratio':2,
              'op2Level':.16,'op4Level':.15,'op2Wave':6,'op4Wave':7,'attack':.002,'decay':.12,'sustain':.2,'release':.12},[36,36,43,39,36,46,43,34],.27),
            ('05-three-carrier-bells.wav',{'algorithm':6,'op1Level':.55,'op2Level':.2,'op3Level':.09,
              'op1Ratio':1,'op2Ratio':2.5,'op3Ratio':4,'op4Ratio':3.5,'op4Level':.19,'op4Wave':1,
              'attack':.002,'decay':.5,'sustain':.08,'release':.7},[60,67,63,72,70,67,63,58],.45),
            ('06-four-partial-keys.wav',{'algorithm':8,'op1Level':.55,'op2Level':.25,'op3Level':.14,'op4Level':.08,
              'op1Ratio':1,'op2Ratio':2,'op3Ratio':3,'op4Ratio':4,'op1Wave':0,'op2Wave':1,'op3Wave':2,'op4Wave':7,
              'attack':.012,'decay':.3,'sustain':.5,'release':.3},[48,55,60,63,58,55,51,46],.35)]
        for filename,p,notes,step in programs:
            events=[]
            for i,note in enumerate(notes):
                frame=2400+round(i*step*48000)
                events += [(frame,'freq',440*2**((note-69)/12)),(frame,'velocity',.65+.1*(i%4)),
                           (frame,'gate',1),(frame+round(step*.68*48000),'gate',0)]
            x=self.render(filename[:-4],scalar,p,events,seconds=len(notes)*step+1)
            self.audition(filename,[x])
        self.check('sources:unchanged',self.initial_source==identity(self.module))
        self.check('libraries:unchanged',self.library_identity==identity(self.a.libraries,'*.lib'))
        self.check('ymfm:unchanged',upstream_before==identity(self.a.ymfm/'src'))
        self.report['listening']=identity(self.out/'listening')
        self.report['status']='completed'


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ['out','faust','libraries','renderer','ymfm']:p.add_argument('--'+name,type=Path,required=True)
    p.add_argument('--archive',type=Path)
    p.add_argument('--cxx',default=os.getenv('CXX','c++'))
    a=p.parse_args()
    for name in ['faust','libraries','renderer','ymfm','archive']:
        if getattr(a,name) is not None:setattr(a,name,getattr(a,name).resolve())
    q=RoutingQualification(a)
    try:q.execute()
    except Exception as e:
        q.report['status']='failed';q.report['error']=str(e);raise
    finally:q.save()
    print(json.dumps({'status':q.report['status'],'checks':len(q.checks),'renders':len(q.renders),'oracle':q.report['oracle']['status']}))

if __name__=='__main__':main()
