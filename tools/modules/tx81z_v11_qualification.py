#!/usr/bin/env python3
import argparse,json,pathlib,subprocess,struct
P=pathlib.Path
def run(c): return subprocess.run(c,check=True,text=True,capture_output=True)
def compile_dsp(root,faust,renderer,src,out):
 cpp=out.with_suffix('.cpp'); exe=out.with_suffix('.bin')
 run([faust,'-lang','cpp','-single','-I',str(root/'modules/tx81z/v11'),'-I',str(root/'modules/tx81z/v10'),'-I',str(root/'modules/tx81z/v7'),'-I',str(root/'modules/tx81z/v6'),'-I',str(root/'modules/tx81z/v5'),str(src),'-o',str(cpp)])
 run(['c++','-std=c++17','-O2',str(renderer),str(cpp),'-o',str(exe)]); return exe
def render(exe,out,frames=96000):
 score=out.with_suffix('.score'); score.write_text('0 freq 110\n0 gate 1\n60000 gate 0\n')
 return run([str(exe),str(score),str(out),'48000','64',str(frames),'0']).stdout
def floats(path):
 d=path.read_bytes(); return struct.unpack('<%df'%(len(d)//4),d)
def main():
 ap=argparse.ArgumentParser(); ap.add_argument('--out',required=True); ap.add_argument('--faust',required=True); ap.add_argument('--renderer',required=True); a=ap.parse_args(); root=P(__file__).resolve().parents[2]; out=P(a.out); out.mkdir(parents=True,exist_ok=True)
 e10=compile_dsp(root,a.faust,P(a.renderer),root/'modules/tx81z/v10/voice.dsp',out/'v10'); e11=compile_dsp(root,a.faust,P(a.renderer),root/'modules/tx81z/v11/recomposed_voice.dsp',out/'v11'); ea=compile_dsp(root,a.faust,P(a.renderer),root/'modules/tx81z/v11/alternate.dsp',out/'alternate')
 p10=out/'v10.f32'; p11=out/'v11.f32'; pa=out/'alternate.f32'; render(e10,p10); render(e11,p11); render(ea,pa)
 x=floats(p10); y=floats(p11); z=floats(pa); assert len(x)==len(y)==96000; assert p10.read_bytes()==p11.read_bytes(); assert max(map(abs,z))>1e-6
 report={'status':'completed','checks':{'v10_v11_float_bytes_exact':True,'alternate_non_silent':True},'frames':len(x),'alternate_peak':max(map(abs,z))}; (out/'results.json').write_text(json.dumps(report,indent=2)); print(json.dumps(report,indent=2))
if __name__=='__main__': main()
