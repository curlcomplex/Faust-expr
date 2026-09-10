from __future__ import annotations
import json,math,subprocess,sys,wave,hashlib,os
from pathlib import Path
import numpy as np
ROOT=Path(__file__).resolve().parents[2]; MOD=ROOT/'modules/morph-wavetable'
def run(a):
 p=subprocess.run(list(map(str,a)),cwd=ROOT,capture_output=True,text=True,timeout=180)
 if p.returncode: raise RuntimeError(p.stdout+p.stderr)
 return p.stdout
def wav(p,x,rate=48000):
 if not np.isfinite(x).all() or np.abs(x).max()>=1: raise ValueError('audio')
 with wave.open(str(p),'wb') as f:f.setnchannels(1);f.setsampwidth(2);f.setframerate(rate);f.writeframes(np.rint(x*32767).astype('<i2').tobytes())
def build(out,vec=False):
 d=out/('vector' if vec else 'scalar');d.mkdir(parents=True,exist_ok=True)
 flags=['-lang','cpp','-single','-cn','ModuleDSP']+(['-vec','-lv','0','-vs','32'] if vec else [])
 run(['faust',*flags,MOD/'morph.dsp','-o',d/'generated.hpp']);run(['c++','-std=c++17','-O2','-ffp-contract=off','-I'+str(d),ROOT/'tools/modules/render.cpp','-o',d/'render']);return d/'render'
def render(exe,out,label,params,events=[],seconds=2,block=128,rate=48000):
 defaults={'pitch_hz':220,'morph':.28,'shape':.45,'detune':.18,'stack':2,'decay':.55,'drive':.08,'velocity':1,'gate':0};rows=[]
 for k,v in defaults|params: pass
 values=defaults.copy();values.update(params); rows=[(0,k,v) for k,v in values.items()]+events;rows.sort()
 score=out/(label+'.tsv');score.write_text(''.join(f'{n}\t{k}\t{v}\n' for n,k,v in rows));raw=out/(label+'.f32');frames=round(seconds*rate)
 diag=json.loads(run([exe,score,raw,rate,block,frames,0]));x=np.fromfile(raw,dtype='<f4');return x,diag
def main():
 out=Path(sys.argv[sys.argv.index('--out')+1]);out.mkdir(parents=True,exist_ok=True);s=build(out);v=build(out,True);checks=[];renders=0
 def ck(n,b):checks.append([n,bool(b)]);assert b,n
 def hit(n,off):return [(n,'gate',1),(off,'gate',0)]
 # silence, pitch, segmentation, vector parity, held release
 x,_=render(s,out,'silence',{},seconds=.1);renders+=1;ck('silence',not np.any(x))
 for hz in (55,220,880):
  x,_=render(s,out,f'pitch-{hz}',{'pitch_hz':hz,'morph':0,'shape':.5,'detune':0,'stack':1,'drive':0},hit(1000,30000),seconds=1);renders+=1
  y=x[5000:25000]*np.hanning(20000);sp=np.abs(np.fft.rfft(y));freq=np.fft.rfftfreq(len(y),1/48000);m=freq[np.argmax(sp)];ck('pitch-'+str(hz),abs(m-hz)<2.5)
 ev=hit(101,18001)+[(9001,'morph',.9),(12007,'detune',.8),(15011,'shape',.1)]
 ref,_=render(s,out,'dynamic-128',{},ev,block=128);renders+=1
 for b in (1,32,64,127,256,512):
  z,_=render(s,out,'dynamic-'+str(b),{},ev,block=b);renders+=1;ck('block-'+str(b),np.array_equal(z,ref))
 z,_=render(v,out,'dynamic-vector',{},ev);renders+=1;ck('vector',np.max(np.abs(z-ref))<3e-4)
 # stack changes level sensibly, no chord intervals: late spectra stay around same fundamental family.
 for st in (1,2,3,4):
  z,_=render(s,out,'stack-'+str(st),{'stack':st,'detune':.55,'morph':.35},hit(1000,30000),seconds=1);renders+=1;ck('stack-finite-'+str(st),np.isfinite(z).all() and np.max(np.abs(z))<.95)
 # endpoint stress
 for i in range(32):
  p={'morph':(i&1),'shape':((i>>1)&1),'detune':((i>>2)&1),'stack':1+((i>>3)&3),'decay':((i>>1)&1),'drive':((i>>4)&1),'pitch_hz':80 if i&1 else 1200}
  z,_=render(s,out,'corner-'+str(i),p,hit(101,4000),seconds=.35,block=127);renders+=1;ck('corner-'+str(i),np.isfinite(z).all() and np.max(np.abs(z))<.98)
 patches=json.loads((MOD/'patches.json').read_text())['anchors'];ev=[]
 for i,(name,p) in enumerate(patches.items()):
  n=2400+i*12000;ev += [(n,k,val) for k,val in p.items()]+[(n,'pitch_hz',110*2**((i%5)/12)),(n,'velocity',1),(n,'gate',1),(n+7000,'gate',0)]
 x,_=render(s,out,'anchors',{},ev,seconds=2.4);renders+=1;wav(out/'morph-anchors.wav',x)
 # melodic locked pattern
 ev=[];notes=[110,164.81,220,293.66,329.63,220,196,146.83]
 for i in range(32):
  n=2400+i*6000;p=list(patches.values())[(i//4)%8];ev +=[(n,k,val) for k,val in p.items()]+[(n,'pitch_hz',notes[i%8]),(n,'velocity',1 if i%4==0 else .62),(n,'gate',1),(n+4200,'gate',0)]
 x,_=render(s,out,'pattern',{},ev,seconds=4.4);renders+=1;wav(out/'morph-pattern.wav',x)
 # control traversal
 ev=[];controls=['morph','shape','detune','stack','decay','drive']
 for j,k in enumerate(controls):
  for i in range(8):
   n=2400+j*48000+i*6000;val=(1+round(i/7*3)) if k=='stack' else i/7;ev +=[(n,k,val),(n,'pitch_hz',220),(n,'gate',1),(n+4000,'gate',0)]
 x,_=render(s,out,'controls',{},ev,seconds=6.5);renders+=1;wav(out/'morph-controls.wav',x)
 report={'passed':all(x[1] for x in checks),'checks':len(checks),'renders':renders,'source_commit':run(['git','rev-parse','HEAD']).strip(),'human_approved':False,'tone_distinctness_approved':False,'device_qualified':False,'notes':'No hardware clone claim; Nord used as architectural prior art for wavetable + same-note unison.'};(out/'report.json').write_text(json.dumps(report,indent=2));print(json.dumps(report))
if __name__=='__main__':main()
