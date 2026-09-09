#!/usr/bin/env python3
"""Correct a JUCE var overload in generated report code, not DSP or acceptance.
The exact preceding 19 output-only cost cases, 19 state cases and46 original
regressions are prerequisites. Reuse their verified kernel bytes; recompile the
host with only report-type corrections, then repeat all8 actual online cases.
"""
from pathlib import Path
import argparse,hashlib,importlib.util,json,os,shutil,subprocess,sys,tarfile
HERE=Path(__file__).resolve().parent;REPO=HERE.parents[1];NEXT=HERE.parent/'persistent-state-next'
sys.path.insert(0,str(HERE.parent/'retained-groups'))
spec=importlib.util.spec_from_file_location('prior_groups',HERE.parent/'retained-groups/run.py');prior=importlib.util.module_from_spec(spec);spec.loader.exec_module(prior)
HEADER_SHA='ea2edfff3a4cd3ff977074b13c3ca817e4fa2d5667f39a1b2d5352628747463f'
def digest(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def read(p):return json.loads(p.read_text())
def main():
    ap=argparse.ArgumentParser();ap.add_argument('--previous',type=Path,required=True);ap.add_argument('--output',type=Path,required=True);a=ap.parse_args();previous=a.previous.resolve();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True);records=[]
    def run(cmd,name,timeout=180):
        r=prior.command(cmd,out/(name+'.log'),timeout=timeout,cwd=REPO);records.append(r);(out/'commands.json').write_text(json.dumps(records,indent=2)+'\n')
        if r['returncode']:raise RuntimeError(name+' failed; log retained')
    prior.verify_snapshot();old=read(previous/'identity.json');assert old['head']=='008be1e5e9b8572ac3f9ef937e5cb391c9566b3c'
    gate=read(previous/'gate/gate-summary.json');state=read(previous/'transitions/transition-summary.json');reg=read(previous/'original-regression/retained-summary.json')
    assert gate['gate_positive'] and len(gate['cases'])==19 and not gate['failures']
    assert state['passed'] and len(state['cases'])==19 and not state['failures']
    assert reg['passed'] and len(reg['cases'])==46 and not reg['failures']
    for name,want in old['source_sha256'].items():
        f=REPO/name
        if f.exists():assert digest(f)==want, 'prior implementation changed: '+name
    run([sys.executable,NEXT/'prepare.py'],'pinned-source-adapter',30)
    original=(NEXT/'live.h').read_text();assert digest(NEXT/'live.h')==HEADER_SHA
    corrected=original
    for name in ['state_instances_created_during_edits','state_bytes_copied_during_edits']:
        before=f'prop(result,"{name}",0)';after=f'prop(result,"{name}",V(0))';assert corrected.count(before)==1;corrected=corrected.replace(before,after)
    generated_header=NEXT/'live-typed.generated.h';generated_header.write_text(corrected)
    mainfile=NEXT/'main.generated.cpp';maincode=mainfile.read_text();assert maincode.count('#include "live.h"')==1
    maincode=maincode.replace('#include "live.h"','#include "live-typed.generated.h"');mainfile.write_text(maincode)
    fix={'original_live_header_sha256':HEADER_SHA,'generated_live_header_sha256':digest(generated_header),
         'change':'Two literal-zero report values now explicitly construct juce::var(int); generated native DSP and compute/control code are unchanged.',
         'strict_evaluator_modified':False,'prior_native_head':old['head'],'prior_gate_summary_sha256':digest(previous/'gate/gate-summary.json'),
         'prior_state_summary_sha256':digest(previous/'transitions/transition-summary.json'),'prior_regression_summary_sha256':digest(previous/'original-regression/retained-summary.json')}
    (out/'metadata-correction.json').write_text(json.dumps(fix,indent=2)+'\n')
    compiler=subprocess.check_output(['clang++','--version'],text=True);faust=subprocess.check_output(['faust','--version'],text=True)
    kroot=out/'kernels';kroot.mkdir(exist_ok=True);kernelpins={}
    for shape in ['ben-4','ben-32','parallel-32','feedback-8','memory-8']:
        src=previous/'kernels'/shape;m=read(src/'manifest.json');assert m['abi']['compiler']==compiler and m['abi']['faust']==faust
        for name,want in {**m['source_hashes'],**m['binary_hashes']}.items():assert digest(src/name)==want
        shutil.copytree(src,kroot/shape,dirs_exist_ok=True);kernelpins[shape]={'schema':m['schema'],'binary_hashes':m['binary_hashes'],'source_hashes':m['source_hashes']}
    prefix=Path(subprocess.check_output(['brew','--prefix','faust'],text=True).strip());os.environ['PS_FAUST_PREFIX']=str(prefix)
    build=HERE/'build';exe=build/'PersistentStateNextBench'
    run(['cmake','-S',NEXT,'-B',build,'-DCMAKE_BUILD_TYPE=RelWithDebInfo','-DFAUST_ROOT='+str(prefix)],'configure',400)
    run(['cmake','--build',build,'--target','PersistentStateNextBench','--parallel','3'],'build',1500)
    identity={'head':subprocess.check_output(['git','rev-parse','HEAD'],cwd=REPO,text=True).strip(),'executable_sha256':digest(exe),'prior_native_head':old['head'],
              'metadata_correction':fix,'kernel_pins':kernelpins,'source_sha256':{}}
    sources=[p for d in [HERE,NEXT,HERE.parent/'persistent-state'] for p in d.iterdir() if p.is_file()]
    with tarfile.open(out/'executed-source.tar.gz','w:gz') as tf:
        for f in sources:identity['source_sha256'][str(f.relative_to(REPO))]=digest(f);tf.add(f,arcname=str(f.relative_to(REPO)))
    (out/'identity.json').write_text(json.dumps(identity,indent=2)+'\n');liveout=out/'live';liveout.mkdir(exist_ok=True);cases=[]
    for mode in ('live','live-stress'):
      for family,n in [('ben',32),('parallel',32),('feedback',8),('memory',8)]:
        name=f'{mode}-{family}-{n}-b128';folder=liveout/name;folder.mkdir(exist_ok=True)
        r=prior.command([exe,mode,family,n,128,kroot/f'{family}-{n}',folder],liveout/(name+'.log'),timeout=90,cwd=REPO)
        r.update(name=name,mode=mode,family=family,size=n,frames=128);cases.append(r);(liveout/'live-cases.json').write_text(json.dumps(cases,indent=2)+'\n')
        print('ONLINE_REPEAT',name,r['returncode'],flush=True)
        if (folder/'live-result.json').exists():print('ONLINE_RESULT',name,(folder/'live-result.json').read_text(),flush=True)
    run([sys.executable,NEXT/'audit_live.py',liveout,out/'independent'],'strict-independent-online-audit',300)
    print((out/'strict-independent-online-audit.log').read_text(),flush=True)
    identity['online_passed']=read(out/'independent/live-independent-audit.json')['passed'];prior.verify_snapshot()
    for name,want in identity['source_sha256'].items():assert digest(REPO/name)==want
    (out/'identity.json').write_text(json.dumps(identity,indent=2)+'\n')
    print('ONLINE_REPEAT_STATUS',identity['online_passed'],flush=True)
if __name__=='__main__':main()
