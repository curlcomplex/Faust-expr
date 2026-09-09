#!/usr/bin/env python3
"""Existing native build graph, bounded isolated children, retained failures."""
from __future__ import annotations
import argparse,hashlib,importlib.util,json,os,platform,subprocess,sys
from pathlib import Path
import analyze_policy
HERE=Path(__file__).resolve().parent;REPO=HERE.parents[1]
sys.path.insert(0,str(HERE.parent/'evolving-groups'))
spec=importlib.util.spec_from_file_location('evolving_runner',HERE.parent/'evolving-groups/run.py');old=importlib.util.module_from_spec(spec);spec.loader.exec_module(old)
command=old.command

def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
    ap=argparse.ArgumentParser();ap.add_argument('--output',required=True,type=Path);ap.add_argument('--shard',required=True,type=int,choices=(0,1));a=ap.parse_args()
    out=a.output.resolve();out.mkdir(parents=True,exist_ok=True);old.previous.verify_snapshot();commands=[]
    identity={'head':subprocess.check_output(['git','rev-parse','HEAD'],cwd=REPO,text=True).strip(),'platform':platform.platform(),'machine':platform.machine(),'shard':a.shard,'source_sha256':{}}
    for folder in (HERE,HERE.parent/'evolving-groups',HERE.parent/'retained-groups'):
        for p in folder.glob('*'):
            if p.is_file():identity['source_sha256'][str(p.relative_to(REPO))]=sha(p)
    def run(cmd,name,timeout):
        r=command(cmd,out/(name+'.log'),timeout=timeout,cwd=REPO);commands.append(r);(out/'commands.json').write_text(json.dumps(commands,indent=2))
        if r['returncode']:raise RuntimeError(name+' failed; evidence retained')
    (out/'identity.json').write_text(json.dumps(identity,indent=2));run([sys.executable,HERE/'test_policy.py'],'evaluator-tests',30)
    prefix=subprocess.check_output(['brew','--prefix','faust'],text=True).strip();build=HERE/'build'
    run(['cmake','-S',HERE,'-B',build,'-DCMAKE_BUILD_TYPE=RelWithDebInfo','-DFAUST_ROOT='+prefix],'configure',400)
    run(['cmake','--build',build,'--target','AuthoringPolicyBench','--parallel','3'],'build',1500)
    exe=build/'AuthoringPolicyBench';identity['executable_sha256']=sha(exe);os.environ['CURLOP_FAUST_BACKEND']='jit';cases=[]
    for c in analyze_policy.definitions(a.shard):
        folder=out/c['name'];folder.mkdir(exist_ok=True)
        r=command([exe,c['mode'],c['family'],c['size'],c['policy_value'],c['frames'],c['variant'],folder],out/(c['name']+'.log'),timeout=180,cwd=REPO)
        r.update(c);r['load_average']=os.getloadavg();cases.append(r);(out/'cases.json').write_text(json.dumps(cases,indent=2)+'\n');print(c['name'],'exit',r['returncode'],flush=True)
    result=analyze_policy.analyze(out,a.shard);old.previous.verify_snapshot()
    for name,want in identity['source_sha256'].items():
        if sha(REPO/name)!=want:raise RuntimeError('source changed: '+name)
    identity['passed']=result['passed'];(out/'identity.json').write_text(json.dumps(identity,indent=2)+'\n')
    if not result['passed']:raise RuntimeError(json.dumps(result['failures']))
    print('PASS',len(cases),'native policy cases, shard',a.shard,flush=True)
if __name__=='__main__':main()
