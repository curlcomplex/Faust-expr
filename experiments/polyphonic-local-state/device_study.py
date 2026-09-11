#!/usr/bin/env python3
"""Run the final CoreAudio/AudioWorkgroup A/B on the physical Mac.
All output sent to the actual device is silence; the DSP output is captured internally.
"""
from __future__ import annotations
import json, os, random, statistics, subprocess, sys
from pathlib import Path

def write(path, value): path.write_text(json.dumps(value, indent=2) + '\n')
def run(exe, kernels, out):
    out.mkdir(parents=True, exist_ok=True)
    planned=[dict(participants=p, workgroup=w, repeat=r) for r in range(2) for p in (4,8) for w in (0,1)]
    random.Random(261114).shuffle(planned);write(out/'planned.json',planned);records=[]
    for i,c in enumerate(planned):
        name=f"{i:02d}-p{c['participants']}-wg{c['workgroup']}-r{c['repeat']}";folder=out/name;folder.mkdir(parents=True,exist_ok=True)
        cmd=[str(exe),'parallel','16',str(c['participants']),str(c['workgroup']),str(kernels/'parallel-16'),str(folder)]
        env=os.environ.copy();env.update(PS_RT_CONTEXT='workers',PS_RT_BUDGET='geometry')
        with (folder/'native.log').open('w') as log:
            result=subprocess.run(cmd,env=env,stdout=log,stderr=subprocess.STDOUT,timeout=30)
        if result.returncode:
            print((folder/'native.log').read_text()[-12000:],flush=True);raise RuntimeError('device case failed '+name)
        data=json.loads((folder/'device-result.json').read_text());assert data['passed'] and data['participants']==c['participants'] and bool(data['workgroup_requested'])==bool(c['workgroup'])
        if c['workgroup']: assert data['workgroup_available'] and data['workgroup_max_parallel']>0
        records.append(dict(name=name,**c,**data));write(out/'cases.json',records);print('FINAL_DEVICE_CASE',name,json.dumps(data),flush=True)
    table=[]
    for p in (4,8):
      for w in (0,1):
        rows=[r for r in records if r['participants']==p and r['workgroup']==w]
        table.append(dict(participants=p,workgroup=w,cases=len(rows),callbacks=sum(r['callbacks'] for r in rows),over_budget=sum(r['over_budget'] for r in rows),over_budget_pct=100*sum(r['over_budget'] for r in rows)/sum(r['callbacks'] for r in rows),median_case_median_us=statistics.median(r['median_us'] for r in rows),median_case_p99_us=statistics.median(r['p99_us'] for r in rows),maximum_us=max(r['maximum_us'] for r in rows),maximum_host_interval_ms=max(r['max_host_interval_ms'] for r in rows),xrun_delta=sum(max(0,r['xrun_delta']) for r in rows if r['xrun_delta']>=0),max_adoption_us=max(r['adoption_callback_us'] for r in rows),workgroup_max=min(r['workgroup_max_parallel'] for r in rows) if w else 0))
    result={'passed':True,'cases':len(records),'table':table,'device_acceptance_scope':'default CoreAudio output, 48kHz/128, silent physical output, internally captured graph audio, prepared deferred compile/adoption'};write(out/'summary.json',result);print('FINAL_DEVICE_SUMMARY',json.dumps(result),flush=True)
if __name__=='__main__':run(Path(sys.argv[1]),Path(sys.argv[2]),Path(sys.argv[3]))
