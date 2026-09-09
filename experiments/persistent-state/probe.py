#!/usr/bin/env python3
"""Compiler capability evidence, not performance or state-migration acceptance."""
import argparse, hashlib, json, pathlib, subprocess, time

def main():
    p=argparse.ArgumentParser();p.add_argument('--output',type=pathlib.Path,required=True);a=p.parse_args()
    out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
    source='''import("stdfaust.lib");
gain=hslider("gain",0.2,0.0,1.0,0.001);
process = (os.osc(173)*gain : fi.lowpass(2,4200)), ((1-1') : de.delay(8192,6000));
'''
    f=out/'probe.dsp';f.write_text(source)
    records=[]
    for backend in ('cpp','c','llvm'):
      for mem in (0,1,2,3):
        for one in (False,True):
          name=f'{backend}-mem{mem}-os{int(one)}';suffix='.ll' if backend=='llvm' else '.h'
          cmd=['faust','-lang',backend,'-cn','MemoryProbe']+(['-mem'+str(mem)] if mem else [])+(['-os'] if one else [])+[str(f),'-o',str(out/(name+suffix))]
          start=time.monotonic()
          try:
            r=subprocess.run(cmd,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,timeout=45)
            code=r.returncode;text=r.stdout
          except subprocess.TimeoutExpired as e:code=124;text=str(e)
          (out/(name+'.log')).write_text(text)
          records.append(dict(name=name,command=cmd,returncode=code,elapsed_s=time.monotonic()-start))
    (out/'capabilities.json').write_text(json.dumps(records,indent=2)+'\n')
    subprocess.run(['faust','-v'],stdout=(out/'faust-version.txt').open('w'),stderr=subprocess.STDOUT,check=True)
    print(json.dumps(records,indent=2))
if __name__=='__main__':main()
