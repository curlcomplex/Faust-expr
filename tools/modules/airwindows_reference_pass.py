"""Whole-algorithm Airwindows comparison; original bodies, existing Faust runner.
Network reads are pinned public source files only. No private host/model services.
Artifacts retain originals, generated adapters, raw stimuli/scores/results and fails.
"""
from pathlib import Path
import argparse, hashlib, json, os, platform, re, time, urllib.request
import numpy as np
from scipy.io import wavfile
from airwindows_pair import PairLab, DEFAULTS, PRESETS, SOURCES, controls, program
from hats_v2_delivery import command, digest
ROOT=Path(__file__).resolve().parents[2]
PIN='03c9931839881bae6dfd4e36bfd3cced79f54b4a'
NAMES={'tape':'IronOxideClassic2','ensemble':'Ensemble'}
NEW={'tape':ROOT/'modules/vintage-tape/v2/tape.dsp','ensemble':ROOT/'modules/retro-ensemble/v2/ensemble.dsp'}

def obtain(out):
    """Cache only at the immutable pin; archive content hashes, no retry loop."""
    dest=out/'upstream';dest.mkdir(exist_ok=True)
    index={}
    for module,name in NAMES.items():
        for suffix in ('.h','.cpp','Proc.cpp'):
            fn=name+suffix;p=dest/fn
            url=f'https://raw.githubusercontent.com/airwindows/airwindows/{PIN}/plugins/LinuxVST/src/{name}/{fn}'
            if not p.exists():
                with urllib.request.urlopen(url,timeout=25) as r:p.write_bytes(r.read())
            index[fn]={'url':url,'sha256':digest(p),'bytes':p.stat().st_size}
    (dest/'sources.json').write_text(json.dumps({'commit':PIN,'files':index},indent=2))
    return dest,index

def oracle_header(module,src,seed=0):
    """Extract (not transcribe) the complete original double processing method.
    Stateful members and reset body also originate in the pinned source. VST glue
    is omitted. Seed=0 disables the original near-silence noise injection; the
    original double entry point already comments out its output-dither additions.
    """
    name=NAMES[module];h=(src/(name+'.h')).read_text();cpp=(src/(name+'.cpp')).read_text();proc=(src/(name+'Proc.cpp')).read_text()
    fields=h.split('private:',1)[1].rsplit('};',1)[0]
    fields='\n'.join(s for s in fields.splitlines() if '_programName' not in s and '_canDo' not in s)
    init=cpp.split('AudioEffectX(audioMaster, kNumPrograms, kNumParameters)',1)[1].split('{',1)[1].split('_canDo.insert',1)[0]
    init='\n'.join(s for s in init.splitlines() if not ('fpdL =' in s or 'fpdR =' in s))
    method=proc[proc.index('void '+name+'::processDoubleReplacing'):].replace(name+'::','',1)
    if module=='tape':
        ui='ui->addHorizontalSlider("input",&inDb,0,-18,18,.01);ui->addHorizontalSlider("speed",&ipsControl,16.35,1.5,150,.01);ui->addHorizontalSlider("output",&outDb,0,-18,18,.01);'
        params='A=float((double(inDb)+18)/36);B=float(pow((double(ipsControl)-1.5)/148.5,.25));C=float((double(outDb)+18)/36);'
    else:
        ui='ui->addHorizontalSlider("voices",&voiceControl,25,2,48,1);ui->addHorizontalSlider("fullness",&fullControl,0,0,1,.001);ui->addHorizontalSlider("brighten",&brightControl,1,0,1,.001);ui->addHorizontalSlider("mix",&mixControl,1,0,1,.001);'
        params='A=float((double(voiceControl)-2)/46);B=fullControl;C=brightControl;D=mixControl;'
    return '''// Generated only in test artifacts from pinned MIT Airwindows source.
using VstInt32=int32_t;
class ModuleDSP : public dsp {
public:
'''+fields+'''
int rate=48000;
float inDb=0,ipsControl=16.35f,outDb=0,voiceControl=25,fullControl=0,brightControl=1,mixControl=1;
double inL[8192],inR[8192],outL[8192],outR[8192];
float getSampleRate() {return float(rate);}
int getNumInputs(){return 2;} int getNumOutputs(){return 2;}
void init(int sr){rate=sr;
'''+init+f'\nfpdL={seed}u;fpdR={seed}u;\n'+'}\nvoid buildUserInterface(UI* ui){'+ui+'}\n'+method+'''
void compute(int n,float** ins,float** outs){
'''+params+'''
for(int i=0;i<n;i++){inL[i]=ins[0][i];inR[i]=ins[1][i];}
double* ip[2]={inL,inR};double* op[2]={outL,outR};
processDoubleReplacing(ip,op,n);
for(int i=0;i<n;i++){outs[0][i]=float(outL[i]);outs[1][i]=float(outR[i]);}
}
};
'''

class ReferenceLab(PairLab):
    def native(self,name,header):
        d=self.out/name;d.mkdir(exist_ok=True);(d/'generated.hpp').write_text(header)
        t=time.perf_counter();command([os.getenv('CXX','c++'),'-std=c++17','-O2','-ffp-contract=off','-I'+str(d),ROOT/'tools/modules/render.cpp','-o',d/'render'])
        self.report['builds'][name]={'kind':'original-C++-adapter','generated_sha256':digest(d/'generated.hpp'),'cxx_seconds':time.perf_counter()-t}
        return d/'render'
    def faust(self,name,path,double=False,vector=False):
        d=self.out/name;d.mkdir(exist_ok=True)
        args=[os.getenv('FAUST','faust'),'-I',path.parent,'-lang','cpp','-double' if double else '-single','-cn','ModuleDSP']
        if vector:args+=['-vec','-lv','0','-vs','32']
        t=time.perf_counter();command(args+[path,'-o',d/'generated.hpp']);ft=time.perf_counter()-t
        t=time.perf_counter();command([os.getenv('CXX','c++'),'-std=c++17','-O2','-ffp-contract=off','-I'+str(d),ROOT/'tools/modules/render.cpp','-o',d/'render']);ct=time.perf_counter()-t
        self.report['builds'][name]={'kind':'actual-Faust','source':str(path.relative_to(ROOT)),'source_sha256':digest(path),'generated_sha256':digest(d/'generated.hpp'),'faust_args':list(map(str,args)),'faust_seconds':ft,'cxx_seconds':ct,'controls':command([d/'render','--controls'])}
        return d/'render'

def errors(y,ref):
    y=y.astype(float);ref=ref.astype(float);d=y-ref
    rms=lambda x:float(np.sqrt(np.mean(x*x)))
    rr=rms(ref);err=rms(d)
    return {'max_abs':float(abs(d).max()),'rms_error':err,'reference_rms':rr,'relative_rms':err/max(rr,1e-30),'residual_db':20*np.log10(max(err,1e-30)/max(rr,1e-30)),'gain_db':20*np.log10(max(rms(y),1e-30)/max(rr,1e-30))}

def stimulus(sr,seconds=2,kind='mixed'):
    n=round(sr*seconds);t=np.arange(n)/sr
    if kind=='impulse':
        x=np.zeros((n,2),np.float32);x[17,0]=.35;x[93,1]=-.24;return x
    rng=np.random.default_rng(1871)
    x=program(sr,seconds).astype(float)+rng.uniform(-.025,.025,(n,2))
    # Broad-band transients and a high-frequency probe distinguish interpolation.
    x[:,0]+=.017*np.sin(2*np.pi*7000*t);x[:,1]+=.014*np.sin(2*np.pi*9300*t)
    x[:17]=0
    return x.astype(np.float32)

def run(out):
    L=ReferenceLab(out);c=L.check;r=L.render_audio;start=time.perf_counter();(L.out/'audition').mkdir(exist_ok=True)
    L.report.update(version='airwindows-reference-pass-0.2',commit=os.environ.get('GITHUB_SHA','local'),oracle_approved=False,comparisons=[],benchmarks={},source_pin=PIN)
    L.report['environment']={'platform':platform.platform(),'machine':platform.machine(),'cpu':next((x.split(':',1)[1].strip() for x in Path('/proc/cpuinfo').read_text().splitlines() if x.startswith('model name')),'unknown'),'faust':command([os.getenv('FAUST','faust'),'--version']),'cxx':command([os.getenv('CXX','c++'),'--version'])}
    try:
        src,index=obtain(L.out);L.report['upstream']=index
        tapeproc=(src/'IronOxideClassic2Proc.cpp').read_text()
        code=re.sub(r'//[^\n]*|/\*.*?\*/','',tapeproc,flags=re.S)
        writes=re.findall(r'[^\n]*(?:gcount\s*(?:\+\+|--|[+\-*/]?=)|(?:\+\+|--)\s*gcount)[^\n]*',code)
        L.report['tape_history_counter_writes']=writes
        c('pinned-tape-no-history-counter-advance',len(writes)==2 and all('gcount = 131' in s for s in writes),writes=writes)
        exes={}
        for name in NAMES:
            print('BUILD',name,flush=True)
            exes[name]={'original':L.native(name+'-original',oracle_header(name,src)),'previous':L.faust(name+'-v1',SOURCES[name]),'revised':L.faust(name+'-v2',NEW[name]),'double':L.faust(name+'-v2-double',NEW[name],True)}
            for label,exe in exes[name].items():
                io,p=controls(exe);c(name+'-'+label+':controls',io==(2,2) and set(p)==set(DEFAULTS[name]))
            vals=[('Default',DEFAULTS[name])]+[(k,DEFAULTS[name]|v) for k,v in PRESETS[name].items()]
            for sr in (44100,48000,96000):
                for preset,p in vals:
                    x=stimulus(sr)
                    ys={label:r(f'{name}-{preset}-{sr}-{label}',exe,p,x,sr=sr) for label,exe in exes[name].items()}
                    for label in ('previous','revised','double'):
                        e=errors(ys[label],ys['original']);L.report['comparisons'].append(dict(module=name,preset=preset,rate=sr,implementation=label,kind='mixed',**e))
                    c(f'{name}-{preset}-{sr}:improved',errors(ys['revised'],ys['original'])['relative_rms']<errors(ys['previous'],ys['original'])['relative_rms'])
            for kind in ('impulse','silence'):
                x=stimulus(48000,2,kind) if kind=='impulse' else np.zeros((48000,2),np.float32)
                ys={label:r(f'{name}-{kind}-{label}',exe,DEFAULTS[name],x) for label,exe in exes[name].items()}
                for label in ('revised','double'):
                    L.report['comparisons'].append(dict(module=name,preset='Default',rate=48000,implementation=label,kind=kind,**errors(ys[label],ys['original'])))
                if kind=='silence':c(name+':zero-input-exact-silence',all(not np.any(y) for y in ys.values()))
            # Same automation schedule reaches every implementation at identical frames.
            live=([(12007,'input',6),(36011,'speed',7.5),(60013,'output',-4)] if name=='tape' else [(12007,'voices',6),(36011,'voices',48),(60013,'fullness',.7),(72017,'brighten',.3),(84019,'mix',.5)])
            x=stimulus(48000,3)
            ys={label:r(f'{name}-automation-{label}',exe,DEFAULTS[name],x,events=live) for label,exe in exes[name].items()}
            for label in ('previous','revised','double'):
                L.report['comparisons'].append(dict(module=name,preset='live',rate=48000,implementation=label,kind='automation',**errors(ys[label],ys['original'])))
            # Partition invariance and scalar/vector comparisons do not use oracle fit.
            for block in (1,127,512):
                yy=r(f'{name}-partition-{block}',exes[name]['revised'],DEFAULTS[name],x,block=block,events=live)
                c(f'{name}:partition-{block}',np.array_equal(yy,ys['revised']),max_abs=float(abs(yy-ys['revised']).max()))
            vec=L.faust(name+'-v2-vector',NEW[name],vector=True)
            yy=r(name+'-vector',vec,DEFAULTS[name],x,events=live)
            c(name+':vector-parity',float(abs(yy-ys['revised']).max())<1e-4,max_abs=float(abs(yy-ys['revised']).max()))
            # Audition order is original, previous, revised. One common fixed gain.
            p=DEFAULTS[name]|PRESETS[name]['Slam' if name=='tape' else 'Classic'];x=stimulus(48000,8)
            aud=[]
            wavfile.write(L.out/'audition'/f'{name}_dry.wav',48000,x)
            for label in ('original','previous','revised'):
                yy=r(f'{name}-audition-{label}',exes[name][label],p,x)
                c(name+'-'+label+':audition-no-over',float(abs(yy).max())<1,peak=float(abs(yy).max()))
                wavfile.write(L.out/'audition'/f'{name}_{label}.wav',48000,yy);aud.append(yy)
            wavfile.write(L.out/'audition'/f'{name}_original_previous_revised.wav',48000,np.concatenate(aud))
        # Verify native noise exclusion separately against a nonzero-seeded original.
        for name in NAMES:
            seeded=L.native(name+'-original-seeded',oracle_header(name,src,123456789))
            x=stimulus(48000,2);a=r(name+'-seed0',exes[name]['original'],DEFAULTS[name],x);b=r(name+'-seeded',seeded,DEFAULTS[name],x)
            c(name+':noise-exclusion-small',float(abs(a-b).max())<2e-6,max_abs=float(abs(a-b).max()))
        for name,taps in [('tape',0),('ensemble',6),('ensemble',48)]:
            p=DEFAULTS[name]|({'voices':taps} if taps else {});x=stimulus(48000,5);bench={}
            for label in ('original','previous','revised'):
                vals=[]
                for repeat in range(5):
                    r(f'bench-{name}-{taps}-{label}-{repeat}',exes[name][label],p,x)
                    vals.append(L.report['renders'][-1]['diagnostics'])
                bench[label]=vals
            L.report['benchmarks'][f'{name}-{taps}']=bench
        # A genuine wrong-gain mutation must fail a numerical equality comparison.
        h=(L.out/'tape-v2'/'generated.hpp').read_text();mut=h.replace('output0[i0] =','output0[i0] = 0.99 *',1)
        if mut!=h:
            exe=L.native('tape-negative-gain',mut);x=stimulus(48000,.25)
            good=r('negative-control-good',exes['tape']['revised'],DEFAULTS['tape'],x);bad=r('negative-control-bad',exe,DEFAULTS['tape'],x)
            c('wrong-gain-mutation-detected',errors(bad,good)['relative_rms']>.005)
        else:c('negative-control-mutation-created',False)
        L.report['complete']=True
    except Exception as e:
        L.report['complete']=False;L.report['error']=repr(e)
        L.report['compiler_output']=getattr(e,'output',None)
        raise
    finally:
        L.report['suite_wall_seconds']=time.perf_counter()-start
        (L.out/'results.json').write_text(json.dumps(L.report,indent=2))
        print(json.dumps({'complete':L.report.get('complete',False),'checks':len(L.report['checks']),'renders':len(L.report['renders']),'builds':len(L.report['builds']),'error':L.report.get('error'),'compiler_output':L.report.get('compiler_output')}),flush=True)
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--out',required=True);run(p.parse_args().out)
