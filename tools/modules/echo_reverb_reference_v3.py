"""MV2 full-output reference; three-head echo transport/routing qualification.
Uses the established renderer and pinned upstream originals. No host or GUI code.
"""
from pathlib import Path
import argparse, hashlib, json, os, platform, re, time, urllib.request
import numpy as np
from scipy.io import wavfile
from airwindows_reference_pass import ReferenceLab, errors, stimulus
from airwindows_pair import controls, program
from hats_v2_delivery import command, digest
ROOT=Path(__file__).resolve().parents[2]
PIN='03c9931839881bae6dfd4e36bfd3cced79f54b4a'
RV=dict(decay=.58,size=.52,tone=.46,character=.64,mix=1.)
EC=dict(time=.36,feedback=.48,tone=.58,age=.32,drive=.18,head1=1.,head2=.65,head3=.8,mix=.38)
TD=dict(dry=.8,wet=.55,delay=.36,feedback=.35,tone=.7,fatness=.5)

def obtain(out):
    dest=out/'upstream';dest.mkdir(exist_ok=True);index={}
    for name in ('MV2','TapeDelay'):
        for suffix in ('.h','.cpp','Proc.cpp'):
            fn=name+suffix;p=dest/fn
            url=f'https://raw.githubusercontent.com/airwindows/airwindows/{PIN}/plugins/LinuxVST/src/{name}/{fn}'
            with urllib.request.urlopen(url,timeout=25) as r:p.write_bytes(r.read())
            index[fn]=dict(url=url,sha256=digest(p),bytes=p.stat().st_size)
    (dest/'sources.json').write_text(json.dumps(dict(commit=PIN,files=index),indent=2))
    return dest,index

def oracle(name,src,seed=0,probe=False):
    """Complete original method and reset/state extraction, not a transcription."""
    h=(src/(name+'.h')).read_text();cpp=(src/(name+'.cpp')).read_text();p=(src/(name+'Proc.cpp')).read_text()
    fields=h.split('private:',1)[1].rsplit('};',1)[0]
    fields='\n'.join(s for s in fields.splitlines() if '_programName' not in s and '_canDo' not in s)
    init=cpp.split('AudioEffectX(audioMaster, kNumPrograms, kNumParameters)',1)[1].split('{',1)[1].split('_canDo.insert',1)[0]
    init='\n'.join(s for s in init.splitlines() if not ('fpdL =' in s or 'fpdR =' in s))
    method=p[p.index('void '+name+'::processDoubleReplacing'):].replace(name+'::','',1)
    if probe:
        assert name=='TapeDelay'
        assert method.count('*out1 = inputSampleL;')==1 and method.count('*out2 = inputSampleR;')==1
        method=method.replace('*out1 = inputSampleL;','*out1 = maxdelay;').replace('*out2 = inputSampleR;','*out2 = maxdelay;')
    vals=RV if name=='MV2' else TD
    ui=''.join(f'ui->addHorizontalSlider("{k}",&v{i},{v},0,1,.001);' for i,(k,v) in enumerate(vals.items()))
    params='A=v1;B=v2;C=v0;D=float(.55+.45*double(v3));E=v4;' if name=='MV2' else 'A=v0;B=v1;C=v2;D=v3;E=v4;F=v5;'
    return ('// MIT Airwindows original DSP extracted at '+PIN+'\nusing VstInt32=int32_t;\nclass ModuleDSP: public dsp {public:\n'+fields+
       '\nint rate=48000;float v0=0,v1=0,v2=0,v3=0,v4=0,v5=0;\ndouble inL[8192],inR[8192],outL[8192],outR[8192];\nfloat getSampleRate(){return float(rate);}\nint getNumInputs(){return 2;} int getNumOutputs(){return 2;}\nvoid init(int sr){rate=sr;'+init+f'\nfpdL={seed}u;fpdR={seed}u;\n'+'}\nvoid buildUserInterface(UI* ui){'+ui+'}\n'+method+
       '\nvoid compute(int n,float** ins,float** outs){'+params+'\nfor(int i=0;i<n;i++){inL[i]=ins[0][i];inR[i]=ins[1][i];}\ndouble* ip[2]={inL,inR};double* op[2]={outL,outR};processDoubleReplacing(ip,op,n);\nfor(int i=0;i<n;i++){outs[0][i]=float(outL[i]);outs[1][i]=float(outR[i]);}}\n};\n')

def run(out):
    L=ReferenceLab(out);c=L.check;r=L.render_audio;start=time.perf_counter();aud=L.out/'audition';aud.mkdir(exist_ok=True)
    L.report.update(version='echo-reverb-reference-0.3',commit=os.getenv('GITHUB_SHA','local'),comparisons=[],benchmarks={},source_pin=PIN,presets={},human_approved=False,host_integrated=False)
    L.report['environment']=dict(platform=platform.platform(),cpu=next((x.split(':',1)[1].strip() for x in Path('/proc/cpuinfo').read_text().splitlines() if x.startswith('model name')),'unknown'),faust=command(['faust','--version']),cxx=command([os.getenv('CXX','c++'),'--version']))
    def cmp(tag,y,ref,limit):
        e=errors(y,ref);L.report['comparisons'].append(dict(name=tag,**e));c(tag,e['relative_rms']<limit,limit=limit,**e)
    def wav(name,y):wavfile.write(aud/(name+'.wav'),48000,y.astype(np.float32))
    try:
        src,index=obtain(L.out);L.report['upstream']=index
        delays=list(map(int,re.findall(r'delay[A-Z] = (\d+);',(src/'MV2.cpp').read_text())))
        c('MV2-pinned-delay-inventory',delays==[7573,7307,7177,6907,6779,6521,5981,5563,5297,4903,4759,4489,4391,4229,4153,3989,3659,3407,3251,2999,2917,2749,2503,2423,2146,2088],actual=delays)
        paths={'echo':ROOT/'modules/tape-echo/v3/echo.dsp','reverb':ROOT/'modules/vintage-rack-reverb/v3/reverb.dsp'}
        ex={}
        for name,path in paths.items():
            print('BUILD',name,flush=True)
            ex[name]={k:L.faust(name+'-'+k,p, double=(k=='double'), vector=(k=='vector')) for k,p in [('v1',path.parents[1]/'v1'/path.name),('v2',path.parents[1]/'v2'/path.name),('v3',path),('double',path),('vector',path)]}
            for label,exe in ex[name].items():
                io,ctrl=controls(exe); expected=RV if name=='reverb' else EC
                c(name+'-'+label+':control-contract',io==(2,2) and set(ctrl)==set(expected),controls=ctrl)
        orig=L.native('MV2-original',oracle('MV2',src));td=L.native('TapeDelay-original',oracle('TapeDelay',src))
        cases={'Default':RV,'Small':RV|dict(size=.25,tone=.8,decay=.3),'Dark':RV|dict(size=.8,tone=.1,decay=.7),'Bloom':RV|dict(size=1.,tone=1.,decay=.75),'NoStages':RV|dict(size=0.,decay=0.),'NoRegen':RV|dict(decay=0.,tone=0.)}
        L.report['presets']['reverb']=cases
        for sr in (44100,48000,96000):
            for preset,p in cases.items():
                x=stimulus(sr,4);ys={label:r(f'rv-{sr}-{preset}-{label}',exe,p,x,sr=sr) for label,exe in {'original':orig,**{k:ex['reverb'][k] for k in ('v2','v3','double')}}.items()}
                for k in ('v3','double'):cmp(f'MV2-{sr}-{preset}-{k}',ys[k],ys['original'],2e-4 if k=='v3' else 2e-6)
                L.report['comparisons'].append(dict(name=f'MV2-{sr}-{preset}-previous',**errors(ys['v2'],ys['original'])))
        x=np.zeros((48000*8,2),np.float32);x[17,0]=.35;x[190,1]=-.21
        vals={k:r('rv-impulse-'+k,exe,RV,x) for k,exe in [('original',orig),('v1',ex['reverb']['v1']),('v2',ex['reverb']['v2']),('v3',ex['reverb']['v3'])]}
        cmp('MV2-wet-impulse',vals['v3'],vals['original'],2e-4)
        for k,y in vals.items():wav('reverb_impulse_'+k,y)
        wav('reverb_impulse_original_previous_revised',np.concatenate([vals[k] for k in ('original','v2','v3')]))
        ev=[(12007,'size',.25),(36011,'tone',.0),(60013,'size',1.),(96017,'tone',1.),(130001,'size',.52),(180019,'decay',.2),(220001,'character',.1),(240001,'mix',.4)]
        x=stimulus(48000,6);ref=r('rv-live-original',orig,RV,x,events=ev)
        ys={k:r('rv-live-'+k,ex['reverb'][k],RV,x,events=ev) for k in ('v3','double','vector')}
        for k in ys:cmp('MV2-live-'+k,ys[k],ref,2e-4 if k!='double' else 2e-6)
        for b in (1,32,127,512):
            y=r('rv-live-block'+str(b),ex['reverb']['v3'],RV,x,block=b,events=ev);c('rv-partition-'+str(b),np.array_equal(y,ys['v3']))
        for name,pars in [('reverb',RV),('echo',EC)]:
            y=r(name+'-zero',ex[name]['v3'],pars,np.zeros((48000,2),np.float32));c(name+'-exact-silence',not np.any(y))
            x=stimulus(48000,3);evs=[(30007,k,0.) for k in pars if k not in ('time',)]+[(70011,k,1.) for k in pars if k not in ('time','feedback',)]
            if name=='echo':evs += [(12007,'time',.12),(95011,'time',.6),(80007,'feedback',.96)]
            y=r(name+'-extreme-live',ex[name]['v3'],pars,x,events=evs);c(name+'-extreme-finite',np.isfinite(y).all() and float(abs(y).max())<8,peak=float(abs(y).max()))
        for sr in (44100,96000):
            x=np.zeros((sr*20,2),np.float32);x[64]=.3
            y=r('rv-tail-'+str(sr),ex['reverb']['v3'],RV|dict(decay=.7),x,sr=sr)
            c('rv-tail-decays-'+str(sr),np.linalg.norm(y[-sr:])<np.linalg.norm(y[:sr*4]),tail_rms=float(np.sqrt(np.mean(y[-sr:]**2))))
        probe=L.faust('echo-transport',ROOT/'modules/tape-echo/v3/transport-probe.dsp')
        nativeProbe=L.native('TapeDelay-transport',oracle('TapeDelay',src,probe=True))
        evtd=[(12007,'delay',.2),(40011,'delay',.05),(80013,'delay',.8),(160019,'delay',.36)]
        effective=lambda v: int(np.float32(44000)*np.float32(v))
        evfa=[(n,'target',effective(v)) for n,k,v in evtd]
        z=np.zeros((48000*6,2),np.float32)
        a=r('transport-faust',probe,{'target':effective(.36)},z,events=evfa)
        b=r('transport-native',nativeProbe,TD|dict(feedback=0.,tone=.5),z,events=evtd)
        c('TapeDelay-complete-original-transport-trace',np.array_equal(a,b),max_abs=float(abs(a-b).max()))
        x=np.zeros((48000*6,2),np.float32);x[3*48000]=.2
        for head in (1,2,3):
            p=EC|dict(time=.18,mix=1,age=0,drive=0,head1=0,head2=0,head3=0);p['head'+str(head)]=1
            on=r(f'echo-head{head}-feedback',ex['echo']['v3'],p|dict(feedback=.65),x)
            off=r(f'echo-head{head}-no-feedback',ex['echo']['v3'],p|dict(feedback=0.),x)
            old=r(f'echo-old-head{head}-feedback',ex['echo']['v2'],p|dict(feedback=.65),x)
            late=slice(int(3.26*48000),int(4.3*48000))
            c(f'head{head}-receives-repeats',float(np.linalg.norm(on[late]))>1e-5 and float(np.linalg.norm(off[late]))<1e-8,late_energy=float(np.linalg.norm(on[late])))
            if head<3:c(f'old-head{head}-missing-repeat-reproduced',float(np.linalg.norm(old[late]))<1e-8)
        ecases={'Slap':EC|dict(time=.10,feedback=.2,age=.05,mix=.35),'Classic':EC,'Dub':EC|dict(time=.45,feedback=.75,age=.5,mix=.5),'Worn':EC|dict(time=.28,feedback=.55,age=.9,drive=.35,mix=.5)}
        L.report['presets']['echo']=ecases
        for sr in (44100,48000,96000):
            for nm,p in ecases.items():
                y=r(f'echo-{nm}-{sr}',ex['echo']['v3'],p,stimulus(sr,3),sr=sr)
                c(f'echo-{nm}-{sr}-bounded',float(abs(y).max())<3,peak=float(abs(y).max()))
        x=stimulus(48000,4);ev=[(36011,'time',.14),(84019,'time',.55),(130001,'age',.8),(170007,'head2',0.)]
        base=r('echo-live-base',ex['echo']['v3'],EC,x,events=ev)
        for k in (1,32,127,512):
            y=r('echo-block-'+str(k),ex['echo']['v3'],EC,x,events=ev,block=k);c('echo-partition-'+str(k),np.array_equal(y,base))
        y=r('echo-live-vector',ex['echo']['vector'],EC,x,events=ev);cmp('echo-vector-parity',y,base,2e-4)
        x=program(48000,8);x[48000*4:]=0
        for name,pars in [('reverb',RV|dict(mix=.4)),('echo',EC)]:
            buf=[];wav(name+'_dry',x)
            if name=='reverb':
                yy=r('music-reverb-original',orig,pars,x);wav('reverb_original',yy);buf.append(yy)
            else:
                yy=r('music-TapeDelay-context',td,TD,x);wav('tapedelay_original_context',yy)
            for k in ('v2','v3'):
                yy=r('music-'+name+'-'+k,ex[name][k],pars,x);c(name+k+'-audition-safe',float(abs(yy).max())<1,peak=float(abs(yy).max()));wav(name+'_'+k,yy);buf.append(yy)
            wav(name+'_comparison',np.concatenate(buf))
        h=(L.out/'reverb-v3'/'generated.hpp').read_text()
        marker='output0[i0] =';bad=h.replace(marker,marker+' 0.99 *',1)
        c('gain-mutation-created',bad!=h)
        if bad!=h:
            exe=L.native('reverb-wrong-gain',bad);x=stimulus(48000,1)
            good=r('mutant-good',ex['reverb']['v3'],RV,x);broken=r('mutant-bad',exe,RV,x)
            c('gain-mutation-rejected',errors(broken,good)['relative_rms']>2e-4)
        seeded=L.native('MV2-seeded',oracle('MV2',src,123456789));x=stimulus(48000,3)
        a=r('MV2-seed-zero',orig,RV,x);b=r('MV2-seed-nonzero',seeded,RV,x)
        c('near-zero-noise-exclusion-small',float(abs(a-b).max())<2e-6,max_abs=float(abs(a-b).max()))
        for name,pars in [('reverb',RV),('echo',EC)]:
            bench={};x=stimulus(48000,5)
            for label,exe in [('v2',ex[name]['v2']),('v3',ex[name]['v3'])]+([('original',orig)] if name=='reverb' else []):
                ds=[]
                for i in range(5):
                    r(f'bench-{name}-{label}-{i}',exe,pars,x);ds.append(L.report['renders'][-1]['diagnostics'])
                bench[label]=ds
            L.report['benchmarks'][name]=bench
        L.report['complete']=True
    except Exception as e:
        L.report.update(complete=False,error=repr(e),compiler_output=getattr(e,'output',None));raise
    finally:
        L.report['suite_wall_seconds']=time.perf_counter()-start
        L.report['passed']=L.report.get('complete',False) and all(k['passed'] for k in L.report['checks'])
        (L.out/'results.json').write_text(json.dumps(L.report,indent=2))
        print(json.dumps(dict(passed=L.report['passed'],checks=len(L.report['checks']),renders=len(L.report['renders']),error=L.report.get('error'),failed=[k for k in L.report['checks'] if not k['passed']]),indent=2),flush=True)
    if not L.report['passed']:raise SystemExit(1)
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--out',required=True);run(p.parse_args().out)
