#!/usr/bin/env python3
import argparse, json, pathlib, subprocess, tempfile, wave, struct, math
P=pathlib.Path

def run(c,**kw): return subprocess.run(c,check=True,text=True,capture_output=True,**kw)
def compile_dsp(root,faust,renderer,src,out):
 cpp=out.with_suffix('.cpp'); exe=out.with_suffix('.bin')
 run([faust,'-lang','cpp','-single','-I',str(root/'modules/tx81z/v11'),'-I',str(root/'modules/tx81z/v10'),'-I',str(root/'modules/tx81z/v7'),'-I',str(root/'modules/tx81z/v6'),'-I',str(root/'modules/tx81z/v5'),str(src),'-o',str(cpp)])
 run(['c++','-std=c++17','-O2',str(renderer),str(cpp),'-o',str(exe)])
 return exe

def render(exe,out,frames=96000):
 # Established renderer accepts output path, sample rate, frames and control events.
 cmd=[str(exe),'--output',str(out),'--sample-rate','48000','--frames',str(frames),'--event','0:gate=1','--event','0:freq=110','--event','60000:gate=0']
 r=run(cmd); return r.stdout+r.stderr

def samples(path):
 with wave.open(str(path),'rb') as w:
  assert w.getnchannels()==1 and w.getsampwidth()==2
  return struct.unpack('<%dh'%w.getnframes(),w.readframes(w.getnframes()))
def main():
 ap=argparse.ArgumentParser(); ap.add_argument('--out',required=True); ap.add_argument('--faust',required=True); ap.add_argument('--renderer',required=True); a=ap.parse_args(); root=P(__file__).resolve().parents[2]; out=P(a.out); out.mkdir(parents=True,exist_ok=True)
 # Compile v10, block recomposition and alternate independently.
 e10=compile_dsp(root,a.faust,P(a.renderer),root/'modules/tx81z/v10/voice.dsp',out/'v10'); e11=compile_dsp(root,a.faust,P(a.renderer),root/'modules/tx81z/v11/recomposed_voice.dsp',out/'v11'); ea=compile_dsp(root,a.faust,P(a.renderer),root/'modules/tx81z/v11/alternate.dsp',out/'alternate')
 w10=out/'v10.wav'; w11=out/'v11.wav'; wa=out/'alternate.wav'; render(e10,w10); render(e11,w11); render(ea,wa)
 x=samples(w10); y=samples(w11); z=samples(wa); assert len(x)==len(y) and x==y; assert max(map(abs,z))>0
 report={'status':'completed','checks':{'v10_v11_pcm16_exact':True,'alternate_non_silent':True},'frames':len(x),'alternate_peak':max(map(abs,z))}
 (out/'results.json').write_text(json.dumps(report,indent=2)); print(json.dumps(report,indent=2))
if __name__=='__main__': main()
