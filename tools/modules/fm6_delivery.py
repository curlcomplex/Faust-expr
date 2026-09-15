"""Full FM6 candidate qualification and dry MSFA/candidate musical comparisons.

No hardware claim. Original DX7 H1/H1.1 evidence is never edited. The reference
core has explicit exclusions; unsupported comparisons are not scored as passes.
"""
from __future__ import annotations
import argparse, hashlib, json, math, os, shlex, subprocess, sys, time, traceback
from pathlib import Path
import numpy as np
import lab
import dx7_slice1 as h1
ROOT=Path(__file__).resolve().parents[2]
MODULE=ROOT/'modules/fm6-classic'
RATE=44100
MSFA_SOURCES=h1.SOURCES+['lfo.cc']


def run(cmd,timeout=180):
    p=subprocess.run(list(map(str,cmd)),capture_output=True,text=True,timeout=timeout)
    if p.returncode: raise RuntimeError(f'{cmd}\n{p.stdout}\n{p.stderr}')
    return p.stdout


def manifest(): return json.loads((MODULE/'manifest.json').read_text())
def defaults(): return {k:c['default'] for k,c in manifest()['controls'].items() if k!='gate'}
def validate(params):
    controls=manifest()['controls']
    for k,v in params.items():
        if k not in controls or type(v) not in (int,float) or not math.isfinite(v): raise ValueError('unknown/nonfinite control '+k)
        c=controls[k]
        if not c['min']<=v<=c['max'] or (c['step']==1 and v!=int(v)): raise ValueError('out-of-range/discrete control '+k)


def encode(params,name='FM6 TEST'):
    """Independent packed/unpacked encodings; native UnpackPatch must agree."""
    validate(params);p=defaults()|params;packed=[];expected=[]
    for n in range(6,0,-1):
        def v(k):return int(p[f'op{n}_{k}'])
        r=[v(f'rate{i}') for i in range(1,5)];l=[v(f'env{i}') for i in range(1,5)]
        point=[v('breakpoint'),v('left_depth'),v('right_depth')]
        packed+=r+l+point+[v('left_curve')|(v('right_curve')<<2),v('rate_scale')|((v('detune')+7)<<3),v('ampmod')|(v('velocity')<<2),v('level'),v('mode')|(v('coarse')<<1),v('fine')]
        expected+=r+l+point+[v('left_curve'),v('right_curve'),v('rate_scale'),v('ampmod'),v('velocity'),v('level'),v('mode'),v('coarse'),v('fine'),v('detune')+7]
    pr=[int(p[f'pitch_rate{i}']) for i in range(1,5)];pl=[int(p[f'pitch_level{i}']) for i in range(1,5)]
    name=name.encode('ascii','replace')[:10].ljust(10,b' ')
    packed+=pr+pl+[int(p['algorithm'])-1,int(p['feedback'])|(int(p['osc_sync'])<<3),int(p['lfo_speed']),int(p['lfo_delay']),int(p['lfo_pmd']),int(p['lfo_amd']),int(p['lfo_sync'])|(int(p['lfo_wave'])<<1)|(int(p['lfo_pms'])<<4),int(p['transpose'])+24]+list(name)
    expected+=pr+pl+[int(p['algorithm'])-1,int(p['feedback']),int(p['osc_sync']),int(p['lfo_speed']),int(p['lfo_delay']),int(p['lfo_pmd']),int(p['lfo_amd']),int(p['lfo_sync']),int(p['lfo_wave']),int(p['lfo_pms']),int(p['transpose'])+24]+list(name)+[63]
    if len(packed)!=128 or len(expected)!=156 or min(packed)<0 or max(packed)>127:raise ValueError('patch encoding')
    sx=bytes([240,67,0,0,1,27]+expected[:155]+[(-sum(expected[:155]))&127,247])
    return bytes(packed),expected,sx


def shape(x,start,end):
    y=np.asarray(x[start:end],dtype=float);s=np.abs(np.fft.rfft(y*np.hanning(len(y))));return s/max(np.linalg.norm(s),1e-30)
def distance(a,b,start,end):return float(np.linalg.norm(shape(a,start,end)-shape(b,start,end)))
def aligned64(seconds,rate=RATE):return max(64,round(seconds*rate/64)*64)


def musical_score(notes,step=.32,hold=.21,velocity=100):
    events=[];on=2048
    for i,n in enumerate(notes):
        k=on+i*aligned64(step)
        events += [(k,'note',n),(k,'velocity',velocity if i%3 else max(30,velocity-20)),(k,'gate',1),(k+aligned64(hold),'gate',0)]
    return events


class Qualification:
    def __init__(self,out,faust,libraries,msfa):
        self.out=out.resolve();self.out.mkdir(parents=True,exist_ok=True)
        if (self.out/'results.json').exists():raise ValueError('fresh output directory required')
        self.faust=faust.resolve();self.libs=libraries.resolve();self.msfa=msfa.resolve()
        self.r={'schema':1,'status':'running','checks':[],'comparisons':{},'renders':[], 'limitations':['Related Faust/MSFA software lineage, not physical DX7 validation.','Original MSFA does not implement AM or algorithms4/6 multi-operator feedback.','Detune, detailed envelope/LFO fidelity and hardware output path are not certified.','Single sample-rate oracle is 44100; candidate cross-rate tests do not establish cross-rate reference equality.']}
        self.m=manifest();self.init=defaults()
    def check(self,name,ok,**details):
        self.r['checks'].append(dict(name=name,passed=bool(ok),**details))
        if not ok:raise AssertionError(f'{name}: {details}')
    def build(self):
        shim=self.out/'faust-pinned';shim.write_text('#!/bin/sh\nexec '+shlex.quote(str(self.faust))+' -I '+shlex.quote(str(self.libs))+' -I '+shlex.quote(str(MODULE/'v1/engine'))+' "$@"\n');shim.chmod(0o755)
        self.worker=lab.Lab(self.out/'build');self.worker.faust=str(shim)
        self.exe=self.worker.build('scalar',MODULE/'v1/voice.dsp')
        self.vec=self.worker.build('vector',MODULE/'v1/voice.dsp',True)
        native=self.msfa/'app/src/main/jni';self.oracle=self.out/'msfa-render'
        cmd=[os.environ.get('CXX','c++'),*h1.CPP_FLAGS,'-I'+str(native),ROOT/'tools/modules/fm6_msfa_oracle.cpp',*[native/s for s in MSFA_SOURCES],'-o',self.oracle]
        run(cmd);self.r['builds']=self.worker.builds
        self.r['oracle_build']={'command':list(map(str,cmd)),'binary_sha256':lab.digest(self.oracle),'sources':{s:lab.digest(native/s) for s in MSFA_SOURCES}}
    def render(self,name,params,events,frames,rate=RATE,block=128,exe=None):
        validate(params);d=self.out/'cases'/name;d.mkdir(parents=True,exist_ok=True)
        p=self.init|params;score=[(0,k,v) for k,v in p.items()]
        for n,k,v in events:
            if k=='note':score.append((n,'freq',440*2**((v-69)/12)))
            elif k=='velocity':score.append((n,k,v/127))
            else:score.append((n,k,v))
        score.sort(key=lambda e:e[0]);sp=d/f'candidate-{rate}-{block}.tsv';raw=sp.with_suffix('.f32')
        sp.write_text(''.join(f'{n}\t{k}\t{v:.12g}\n' for n,k,v in score))
        diag=json.loads(run([exe or self.exe,sp,raw,rate,block,frames,0]));x=np.fromfile(raw,dtype='<f4')
        self.check(name+':valid',len(x)==frames and np.isfinite(x).all() and diag['channels']==1)
        self.r['renders'].append({'case':name,'engine':'candidate','rate':rate,'block':block,'file':str(raw.relative_to(self.out)),'sha256':lab.digest(raw),'diagnostics':diag})
        return x
    def reference(self,name,params,events,frames,block=128):
        d=self.out/'cases'/name;d.mkdir(parents=True,exist_ok=True)
        packed,expected,sx=encode(params,name);(d/'patch128.bin').write_bytes(packed);(d/'patch.syx').write_bytes(sx)
        sp=d/'msfa.tsv';sp.write_text(''.join(f'{n}\t{k}\t{v}\n' for n,k,v in events));raw=d/f'msfa-{block}.f32'
        diag=json.loads(run([self.oracle,d/'patch128.bin',sp,raw,frames,block]));x=np.fromfile(raw,dtype='<f4')
        self.check(name+':reference-valid',len(x)==frames and np.isfinite(x).all() and diag['unpacked']==expected)
        self.r['renders'].append({'case':name,'engine':'original-MSFA','rate':RATE,'block':block,'sha256':lab.digest(raw),'file':str(raw.relative_to(self.out))})
        return x
    def compare(self,name,a,b,start=6144,end=30720):
        rms=lambda x:float(np.sqrt(np.mean(np.asarray(x,dtype=float)**2)))
        self.r['comparisons'][name]={'normalized_steady_spectrum_l2':distance(a,b,start,end),'candidate_over_core_db':20*math.log10(max(rms(a[start:end]),1e-30)/max(rms(b[start:end]),1e-30)), 'candidate_pre_gate_peak':float(np.max(abs(a[:2048]))), 'candidate_peak':float(np.max(abs(a)))}
    def execute(self):
        self.r['compiler_version']=run([self.faust,'--version'])
        self.check('compiler-version',self.r['compiler_version'].splitlines()[0]=='FAUST Version 2.88.0')
        controls=run([self.exe,'--controls']);actual={l.split('\t')[0] for l in controls.splitlines()[1:]}
        self.check('compiled-control-contract',actual==set(self.m['controls']),missing=sorted(set(self.m['controls'])-actual),extra=sorted(actual-set(self.m['controls'])))
        self.r['controls_count']=len(actual)
        ev=[(2048,'note',57),(2048,'velocity',100),(2048,'gate',1),(32768,'gate',0)];frames=65536
        # Frozen baseline carrier/pair/articulation translated to runtime controls.
        for family in ('C01','C02','C03'):
            p={'algorithm':1,'op1_level':80,'op2_level':70 if family=='C02' else 80 if family=='C03' else 0,'op2_coarse':2}
            if family=='C03':
                for n,r,l in [(1,[85,55,45,65],[99,80,70,0]),(2,[90,60,50,65],[99,50,30,0])]:
                    p.update({f'op{n}_rate{i+1}':r[i] for i in range(4)});p.update({f'op{n}_env{i+1}':l[i] for i in range(4)})
            a=self.render(family,p,ev,frames);b=self.reference(family,p,ev,frames);self.compare(family,a,b)
            self.check(family+':pre-note-silent',not np.any(a[:2048]))
            self.check(family+':releases',np.max(abs(a[-4096:]))<1e-8)
            self.check(family+':block127',np.array_equal(a,self.render(family+'-block',p,ev,frames,block=127)))
            self.check(family+':cold-repeat',np.array_equal(a,self.render(family+'-repeat',p,ev,frames)))
            if getattr(self,'vec',None):
                v=self.render(family+'-vector',p,ev,frames,exe=self.vec);self.check(family+':vector-parity',np.max(abs(a-v))<2e-4,max_abs=float(np.max(abs(a-v))))
        # Make every operator observable, at distinguishable ratios. No feedback:
        # all 32 routings can be compared without MSFA's feedback4/6 limitation.
        routing={}
        for n in range(1,7):routing.update({f'op{n}_level':65 if n>1 else 80,f'op{n}_coarse':n})
        for alg in range(1,33):
            p=routing|{'algorithm':alg};name=f'algorithm-{alg:02d}'
            a=self.render(name,p,ev,frames);b=self.reference(name,p,ev,frames);self.compare(name,a,b)
            self.check(name+':routing-spectrum',self.r['comparisons'][name]['normalized_steady_spectrum_l2']<.08,distance=self.r['comparisons'][name]['normalized_steady_spectrum_l2'])
        # Single-operator feedback reference, and unsupported multi-op feedback as
        # candidate-only stability/repeat tests, never false reference matches.
        for fb in (1,3,5,7):
            p={'algorithm':32,'op1_level':0,'op6_level':85,'feedback':fb};name=f'feedback-{fb}'
            a=self.render(name,p,ev,frames);b=self.reference(name,p,ev,frames);self.compare(name,a,b)
            self.check(name+':feedback-spectrum',self.r['comparisons'][name]['normalized_steady_spectrum_l2']<.08,distance=self.r['comparisons'][name]['normalized_steady_spectrum_l2'])
        for alg in (4,6):
            p=routing|{'algorithm':alg,'feedback':7};name=f'multiloop-{alg}'
            a=self.render(name,p,ev,frames);b=self.render(name+'-repeat',p,ev,frames,block=127)
            self.check(name+':stable-repeat',np.array_equal(a,b) and np.max(abs(a))<6)
        # Broader data-law probes report differences; they are not automatically
        # tuning targets because the older native oracle differs from modern DX.
        for name,p,note,vel in [
            ('velocity-35',{'op1_velocity':7},57,35),('velocity-100',{'op1_velocity':7},57,100),
            ('fixed-frequency',{'op1_mode':1,'op1_coarse':2,'op1_fine':25},57,100),
            ('left-key-scaling',{'op1_breakpoint':55,'op1_left_depth':45,'op1_left_curve':1},33,100),
            ('right-key-scaling',{'op1_breakpoint':20,'op1_right_depth':55,'op1_right_curve':2},81,100),
            ('key-rate-scaling',{'op1_rate1':70,'op1_rate_scale':5},81,100),
            ('pitch-envelope',{'pitch_level1':60,'pitch_rate1':60},57,100),
            ('pitch-lfo',{'lfo_pmd':15,'lfo_pms':3,'lfo_wave':4},57,100)]:
            e=[(2048,'note',note),(2048,'velocity',vel),(2048,'gate',1),(32768,'gate',0)]
            a=self.render(name,p,e,frames);b=self.reference(name,p,e,frames);self.compare(name,a,b)
        # Candidate-only lifecycle and full-rate cases.
        for rate in (44100,48000,96000):
            n=aligned64(1.5,rate);on=aligned64(.05,rate);off=aligned64(.7,rate)
            e=[(on,'note',60),(on,'velocity',90),(on,'gate',1),(off,'gate',0)]
            a=self.render(f'rate-{rate}',{},e,n,rate=rate);self.check(f'rate-{rate}:silent-pre-note',not np.any(a[:on]))
            z=self.render(f'never-started-{rate}',{},[],n,rate=rate);self.check(f'rate-{rate}:never-started',not np.any(z))
            e0=[(on,'note',60),(on,'velocity',0),(on,'gate',1),(off,'gate',0)]
            z=self.render(f'zero-velocity-{rate}',{},e0,n,rate=rate);self.check(f'rate-{rate}:zero-velocity',not np.any(z))
        p={'algorithm':1,'op2_level':88,'op2_coarse':2,'op1_rate4':40,'op2_rate4':40}
        base=self.render('release-base',p,ev,131072)
        changed=self.render('release-brightness',p,ev+[(40003,'brightness',.25)],131072)
        self.check('release-control-live',np.max(abs(base[40500:]-changed[40500:]))>.005)
        self.check('release-not-hard-gated',np.max(abs(base[32768:40000]))>.005)
        e=[(2048,'note',57),(2048,'velocity',100),(2048,'gate',1),(32768,'gate',0),(32769,'note',64),(32769,'gate',1),(50001,'gate',0)]
        a=self.render('one-sample-retrigger',p,e,131072,block=127)
        self.check('retrigger-audible',np.max(abs(a[33000:49000]))>.01)
        # Original musical programs; no factory ROM/sample assets.
        listen=self.out/'listening';listen.mkdir(exist_ok=True)
        self.r['auditions']=[]
        for patch in json.loads((MODULE/'presets.json').read_text())['presets']:
            name=patch['id'];p=patch['controls'];score=musical_score(patch['notes'],patch['step'],patch['hold'])
            frames=aligned64(score[-1][0]/RATE+patch['tail'])
            a=self.render(name,p,score,frames);b=self.reference(name,p,score,frames)
            h1.float_wav(self.out/'cases'/name/'candidate-raw.wav',a,RATE);h1.float_wav(self.out/'cases'/name/'reference-core-raw.wav',b,RATE)
            # Fixed conversion for all examples, not fitted by patch: core *.1
            # equals the candidate's known half-scale operator output * level .2.
            ref=b*.1;pair=np.concatenate([ref,np.zeros(11025),a]);self.check(name+':listening-headroom',np.max(abs(pair))<.99)
            lab.wav(listen/(name+'-reference-then-candidate.wav'),pair,RATE)
            lab.wav(listen/(name+'-candidate.wav'),a,RATE)
            self.r['auditions'].append({'id':name,'title':patch['title'],'order':['original MSFA core * 0.1','FM6 candidate (level 0.2)'],'gap_s':.25,'per_patch_normalization':False,'file':name+'-reference-then-candidate.wav','seconds_each':frames/RATE})
        self.r['status']='passed-playable-candidate; listening-and-hardware-approval-pending'


def main():
    ap=argparse.ArgumentParser();ap.add_argument('--out',type=Path,required=True);ap.add_argument('--faust',type=Path,required=True);ap.add_argument('--libraries',type=Path,required=True);ap.add_argument('--msfa',type=Path,required=True);ap.add_argument('--use-render',type=Path);ap.add_argument('--use-oracle',type=Path)
    a=ap.parse_args();q=Qualification(a.out,a.faust,a.libraries,a.msfa);started=time.time()
    try:
        if a.use_render:
            if not a.use_oracle:raise ValueError('--use-oracle required with --use-render')
            q.exe=a.use_render.resolve();q.oracle=a.use_oracle.resolve();q.vec=None
            q.r['build_mode']='provided binaries; see recorded identities; vector not run in this invocation'
        else:q.build()
        q.r['provenance']={'faust_sha256':lab.digest(q.faust),'candidate_binary_sha256':lab.digest(q.exe),'oracle_binary_sha256':lab.digest(q.oracle),'msfa_commit':h1.MSFA,'module_sources':{str(p.relative_to(MODULE)):lab.digest(p) for p in MODULE.rglob('*') if p.is_file()},'libraries':{str(p.relative_to(q.libs)):lab.digest(p) for p in q.libs.rglob('*.lib')},'source_commit':os.getenv('SOURCE_COMMIT','uncommitted-sandbox-source; see hashes'),'execution_lane':os.getenv('EXECUTION_LANE','sandbox'),'platform':run(['uname','-a']).strip()}
        q.execute()
        q.check('qualified-source-unchanged',q.r['provenance']['module_sources']=={str(p.relative_to(MODULE)):lab.digest(p) for p in MODULE.rglob('*') if p.is_file()})
        q.check('upstream-libraries-unchanged',q.r['provenance']['libraries']=={str(p.relative_to(q.libs)):lab.digest(p) for p in q.libs.rglob('*.lib')})
    except Exception as e:q.r['status']='failed';q.r['error']=str(e);(q.out/'failure.txt').write_text(traceback.format_exc());raise
    finally:
        q.r['wall_seconds']=time.time()-started;q.r['counts']={'checks':len(q.r['checks']),'passed':sum(x['passed'] for x in q.r['checks']),'renders':len(q.r['renders'])}
        (q.out/'results.json').write_text(json.dumps(q.r,indent=2,allow_nan=False)+'\n')
        print(q.r['status'],q.r['counts'],q.r.get('error',''),flush=True)
if __name__=='__main__':main()
