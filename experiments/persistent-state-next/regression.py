#!/usr/bin/env python3
"""Run the original unmodified 46 native tests in separate child processes."""
import argparse,json,sys
from pathlib import Path
REPO=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(REPO/'vendor/curlop-latency/scripts/bench/patching_latency'))
from run import command
import analyze_retained

def main():
    p=argparse.ArgumentParser();p.add_argument('executable',type=Path);p.add_argument('output',type=Path);a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True);cases=[]
    for mode,family,n,frames in analyze_retained.definitions():
        name=f'{mode}-{family}-{n}-f{frames}';folder=out/name;folder.mkdir(exist_ok=True)
        r=command([a.executable.resolve(),mode,family,n,frames,folder],out/(name+'.log'),timeout=90,cwd=REPO)
        r.update(name=name,mode=mode,family=family,size=n,frames=frames);cases.append(r)
        (out/'retained-cases.json').write_text(json.dumps(cases,indent=2)+'\n');print('ORIGINAL_REGRESSION',name,r['returncode'],flush=True)
    result=analyze_retained.analyze(out)
    if len(cases)!=46 or not result['passed']:raise RuntimeError('original native regression failed')
if __name__=='__main__':main()
