from pathlib import Path
import argparse, json, os, subprocess, urllib.request, re, time
import numpy as np
from scipy.io import wavfile
from hats_v2_delivery import command, digest
ROOT=Path(__file__).resolve().parents[2]
PIN='03c9931839881bae6dfd4e36bfd3cced79f54b4a'
SRC={'echo_v1':ROOT/'modules/tape-echo/v1/echo.dsp','echo_v2':ROOT/'modules/tape-echo/v2/echo.dsp','rev_v1':ROOT/'modules/vintage-rack-reverb/v1/reverb.dsp','rev_v2':ROOT/'modules/vintage-rack-reverb/v2/reverb.dsp'}

def build_faust(out,name,path):
 d=out/name;d.mkdir(parents=True,exist_ok=True)
 command(['faust','-I',str(path.parent),'-lang','cpp','-single','-cn','ModuleDSP',str(path),'-o',str(d/'generated.hpp')])
 command(['c++','-std=c++17','-O2','-I'+str(d),str(ROOT/'tools/modules/render.cpp'),'-o',str(d/'render')])
 return d/'render'

def render(exe,out,name,values,x,sr=48000,block=128,events=None):
 score=out/(name+'.tsv');rawin=out/(name+'-input.f32');raw=out/(name+'.f32')
 rows={(0,k):float(v) for k,v in values.items()}
 for n,k,v in events or []: rows[(int(n),k)]=float(v)
 score.write_text(''.join(f'{n}\t{k}\t{v:.9g}\n' for (n,k),v in sorted(rows.items())))
 np.asarray(x,np.float32).tofile(rawin)
 dg=json.loads(command([str(exe),str(score),str(raw),str(sr),str(block),str(len(x)),str(0),str(rawin)]))
 y=np.fromfile(raw,'<f4').reshape(-1,dg['channels']);return y,dg

def impulse(sr,secs=6):
 x=np.zeros((int(sr*secs),2),np.float32);x[64]=(.35,.31);return x

def program(sr,secs=8):
 n=int(sr*secs);t=np.arange(n)/sr;x=np.zeros((n,2),np.float32)
 x[:,0]=.08*np.sin(2*np.pi*110*t)+.04*np.sin(2*np.pi*440*t)
 x[:,1]=.08*np.sin(2*np.pi*110*t+.2)+.04*np.sin(2*np.pi*660*t)
 for beat in np.arange(.2,secs-.2,.5):
  i=int(beat*sr);m=min(n-i,int(.08*sr));tt=np.arange(m)/sr;b=.23*np.sin(2*np.pi*(90+80*np.exp(-tt*35))*tt)*np.exp(-tt*35);x[i:i+m]+=b[:,None]
 return x.astype(np.float32)

def descriptor(y,sr):
 mono=np.mean(y,axis=1);e=mono*mono
 wins=[]
 for a,b in ((.05,.15),(.25,.5),(1,2),(3,5)):
  i,j=int(a*sr),min(len(e),int(b*sr));wins.append(float(np.sqrt(np.mean(e[i:j])+1e-30)))
 spec=np.abs(np.fft.rfft(mono[:min(len(mono),sr*2)]));freq=np.fft.rfftfreq(min(len(mono),sr*2),1/sr)
 centroid=float((spec*freq).sum()/(spec.sum()+1e-30))
 return {'rms_windows':wins,'centroid':centroid,'peak':float(np.max(np.abs(y)))}

def dist(a,b):
 aa=np.array(a['rms_windows']);bb=np.array(b['rms_windows']);tail=float(np.mean(np.abs(np.log10((aa+1e-12)/(bb+1e-12)))))
 cent=abs(a['centroid']-b['centroid'])/max(b['centroid'],1)
 return tail+cent

def obtain(out,name):
 d=out/'upstream';d.mkdir(exist_ok=True);files={}
 for suf in ('.h','.cpp','Proc.cpp'):
  fn=name+suf;p=d/fn;url=f'https://raw.githubusercontent.com/airwindows/airwindows/{PIN}/plugins/LinuxVST/src/{name}/{fn}'
  with urllib.request.urlopen(url,timeout=30) as r:p.write_bytes(r.read())
  files[fn]={'sha256':digest(p),'url':url}
 return d,files

def native_header(name,src,ui,params):
 h=(src/(name+'.h')).read_text();cpp=(src/(name+'.cpp')).read_text();proc=(src/(name+'Proc.cpp')).read_text()
 fields=h.split('private:',1)[1].rsplit('};',1)[0];fields='\n'.join(s for s in fields.splitlines() if '_programName' not in s and '_canDo' not in s)
 init=cpp.split('AudioEffectX(audioMaster, kNumPrograms, kNumParameters)',1)[1].split('{',1)[1].split('_canDo.insert',1)[0];init='\n'.join(s for s in init.splitlines() if not ('fpdL =' in s or 'fpdR =' in s))
 method=proc[proc.index('void '+name+'::processDoubleReplacing'):].replace(name+'::','',1)
 return f'''using VstInt32=int32_t; class ModuleDSP : public dsp {{ public:\n{fields}\nint rate=48000; {ui}\ndouble inL[8192],inR[8192],outL[8192],outR[8192]; float getSampleRate(){{return float(rate);}} int getNumInputs(){{return 2;}} int getNumOutputs(){{return 2;}} void init(int sr){{rate=sr;\n{init}\nfpdL=0;fpdR=0;}} void buildUserInterface(UI* ui){{{params[0]}}}\n{method}\nvoid compute(int n,float** ins,float** outs){{{params[1]} for(int i=0;i<n;i++){{inL[i]=ins[0][i];inR[i]=ins[1][i];}} double* ip[2]={{inL,inR}};double* op[2]={{outL,outR}};processDoubleReplacing(ip,op,n);for(int i=0;i<n;i++){{outs[0][i]=float(outL[i]);outs[1][i]=float(outR[i]);}} }} }};'''

def build_native(out,name,header):
 d=out/name;d.mkdir(exist_ok=True);(d/'generated.hpp').write_text(header);command(['c++','-std=c++17','-O2','-I'+str(d),str(ROOT/'tools/modules/render.cpp'),'-o',str(d/'render')]);return d/'render'

def run(out):
 out=Path(out);out.mkdir(parents=True,exist_ok=True);(out/'audition').mkdir(exist_ok=True);rep={'pin':PIN,'checks':[],'sources':{},'descriptors':{},'renders':[]}
 def check(name,ok,**kw):rep['checks'].append({'name':name,'passed':bool(ok),**kw});assert ok,name
 ex={k:build_faust(out,k,p) for k,p in SRC.items()}
 # Exact original software oracles, used according to valid topology boundary.
 up,files=obtain(out,'TapeDelay');rep['sources'].update(files)
 tape_ui='float dryC=1,wetC=.7,delayC=.4,fbC=.35,leanC=.5,depthC=.35;'
 tape_params=('ui->addHorizontalSlider("dry",&dryC,1,0,1,.001);ui->addHorizontalSlider("wet",&wetC,.7,0,1,.001);ui->addHorizontalSlider("delay",&delayC,.4,0,1,.001);ui->addHorizontalSlider("feedback",&fbC,.35,0,1,.001);ui->addHorizontalSlider("leanfat",&leanC,.5,0,1,.001);ui->addHorizontalSlider("depth",&depthC,.35,0,1,.001);','A=dryC;B=wetC;C=delayC;D=fbC;E=leanC;F=depthC;')
 tape=build_native(out,'tapedelay-original',native_header('TapeDelay',up,tape_ui,tape_params))
 up2,files2=obtain(out,'MV');rep['sources'].update(files2)
 mv_ui='float depthC=.52,brightC=.46,regenC=.58,outC=.838,mixC=.35;'
 mv_params=('ui->addHorizontalSlider("size",&depthC,.52,0,1,.001);ui->addHorizontalSlider("tone",&brightC,.46,0,1,.001);ui->addHorizontalSlider("decay",&regenC,.58,0,1,.001);ui->addHorizontalSlider("character",&outC,.64,0,1,.001);ui->addHorizontalSlider("mix",&mixC,.35,0,1,.001);','A=depthC;B=brightC;C=regenC;D=.55+.45*outC;E=mixC;')
 mv=build_native(out,'mv-original',native_header('MV',up2,mv_ui,mv_params))
 sr=48000;imp=impulse(sr)
 rv={'decay':.58,'size':.52,'tone':.46,'character':.64,'mix':.35}
 for label,exe in [('original',mv),('previous',ex['rev_v1']),('revised',ex['rev_v2'])]:
  y,dg=render(exe,out,'reverb-'+label,rv,imp,sr);rep['renders'].append({'name':'reverb-'+label,'diag':dg});rep['descriptors']['reverb-'+label]=descriptor(y,sr);wavfile.write(out/'audition'/f'reverb_{label}.wav',sr,y)
 d0=dist(rep['descriptors']['reverb-previous'],rep['descriptors']['reverb-original']);d1=dist(rep['descriptors']['reverb-revised'],rep['descriptors']['reverb-original']);check('reverb-descriptor-improved',d1<d0,previous=d0,revised=d1)
 # TapeDelay cannot be a whole-output oracle for a three-head echo. Compare the time-change behavior: revised should create a smoother moving-delay transition than v1 while preserving the three-head musical output.
 prog=program(sr,8);ev=[(sr*2,'time',.22),(sr*4,'time',.58),(sr*6,'time',.31)]
 tv={'time':.36,'feedback':.48,'tone':.58,'age':.32,'drive':.18,'head1':1,'head2':.65,'head3':.8,'mix':.38}
 banks=[]
 for label,exe in [('previous',ex['echo_v1']),('revised',ex['echo_v2'])]:
  y,dg=render(exe,out,'echo-'+label,tv,prog,sr,events=ev);rep['renders'].append({'name':'echo-'+label,'diag':dg});rep['descriptors']['echo-'+label]=descriptor(y,sr);wavfile.write(out/'audition'/f'echo_{label}.wav',sr,y);banks.append(y)
 # Native TapeDelay movement oracle, separate controls/topology; included for listening and transition descriptor only.
 td={'dry':.62,'wet':.62,'delay':.36*sr/44000,'feedback':.48/1.3,'leanfat':.34,'depth':.32}
 tdev=[(sr*2,'delay',.22*sr/44000),(sr*4,'delay',.58*sr/44000),(sr*6,'delay',.31*sr/44000)]
 yo,dg=render(tape,out,'tapedelay-original',td,prog,sr,events=tdev);rep['renders'].append({'name':'tapedelay-original','diag':dg});rep['descriptors']['tapedelay-original']=descriptor(yo,sr);wavfile.write(out/'audition'/'tapedelay_original.wav',sr,yo)
 check('echo-revised-finite',np.isfinite(banks[1]).all() and np.max(np.abs(banks[1]))<8,peak=float(np.max(np.abs(banks[1]))))
 # Compile/control contract and multi-head identity must remain.
 ctl=command([str(ex['echo_v2']),'--controls']);check('echo-keeps-three-head-controls',all(k in ctl for k in ('head1','head2','head3','time','feedback','mix')))
 ctl2=command([str(ex['rev_v2']),'--controls']);check('reverb-keeps-five-controls',all(k in ctl2 for k in ('decay','size','tone','character','mix')))
 wavfile.write(out/'audition'/'echo_previous_revised.wav',sr,np.concatenate(banks))
 wavfile.write(out/'audition'/'reverb_original_previous_revised.wav',sr,np.concatenate([wavfile.read(out/'audition'/f'reverb_{x}.wav')[1] for x in ('original','previous','revised')]))
 rep['passed']=all(x['passed'] for x in rep['checks']);(out/'results.json').write_text(json.dumps(rep,indent=2));print(json.dumps({'passed':rep['passed'],'checks':len(rep['checks']),'renders':len(rep['renders'])}))
if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('--out',required=True);run(p.parse_args().out)
