#!/usr/bin/env python3
"""Validate full-lifetime cached worker policy evidence, not the final assignment.
This does not claim continuous policy polling or distinct worker identification.
"""
from __future__ import annotations
import csv,json,math
from pathlib import Path

def require(condition,message):
    if not condition:raise ValueError(message)

def validate(metadata,case,observations,deadlines):
    b=case['budget'];period=case['block']/48.0
    require(b in ('startup','geometry','periodic'),'unknown budget')
    require(metadata['context']=='workers' and metadata['job_probe'],'worker probe disabled')
    require(metadata['worker_budget']==b and metadata['configured_player_rate']==48000,'wrong configuration')
    require(metadata['configured_player_frames']==(512 if b=='startup' else case['block']),'wrong frame configuration')
    require(metadata['voices']==4*case['voices'] and metadata['grain']==case['grain'],'wrong voice/batch shape')
    convert=float(metadata['mach_tick_ns'])/1e6
    require(math.isfinite(convert) and convert>0,'invalid Mach timebase')
    batches=(int(metadata['voices'])+case['grain']-1)//case['grain']
    require(bool(deadlines),'no callbacks')
    require(len(observations)==len(deadlines)*batches==metadata['worker_policy_rows'],'incomplete policy history')
    totals={'worker_tc_jobs':0,'worker_other_jobs':0,'caller_tc_jobs':0,'policy_errors':0}
    worker_constraint=512/44.1 if b=='startup' else period
    signatures=set()
    def check(p,caller):
        require(int(p['result'])==0 and int(p['default'])==0,'missing realtime policy or read error')
        expected=(0,512/44.1,512/44.1) if caller else ((period,period*.5,period) if b=='periodic' else (0,worker_constraint,worker_constraint))
        for key,want in zip(('period','computation','constraint'),expected):
            value=float(p[key])*convert
            require(math.isfinite(value) and abs(value-want)<.002,'incorrect '+key+' for '+('caller' if caller else 'worker'))
    check(metadata['caller_thread_policy'],True)
    for index,deadline in enumerate(deadlines):
        require(int(deadline['sample'])==index*case['block'] and int(deadline['frames'])==case['block'],'callback sequence differs')
        counts={key:0 for key in totals}
        for batch in range(batches):
            p=observations[index*batches+batch]
            require(int(p['sample'])==index*case['block'] and int(p['batch'])==batch,'duplicate or reordered policy record')
            role=int(p['caller']);require(role in (0,1),'invalid caller flag')
            check(p,bool(role))
            key='caller_tc_jobs' if role else 'worker_tc_jobs';counts[key]+=1
            if not role:signatures.add(tuple(int(p[k]) for k in ('period','computation','constraint')))
        for key in totals:
            require(counts[key]==int(float(deadline[key])),'policy/callback counter mismatch: '+key)
            totals[key]+=counts[key]
    require(totals['worker_tc_jobs']>0,'no worker observed over the complete run')
    require(int(metadata['sample_frames'])==len(deadlines)*case['block'],'sample count mismatch')
    return dict(totals,worker_constraint_ms=worker_constraint,worker_period_ms=period if b=='periodic' else 0,
                worker_policy_signatures=[list(s) for s in sorted(signatures)],callback_count=len(deadlines),
                policy_rows=len(observations),final_worker_observations=sum(1-int(p['caller']) for p in observations[-batches:]))

def policy_check(folder,case):
    folder=Path(folder)
    m=json.loads((folder/'result.json').read_text())
    with (folder/'worker-policy.tsv').open() as f:policies=list(csv.DictReader(f,delimiter='\t'))
    with (folder/'deadline.tsv').open() as f:deadlines=list(csv.DictReader(f,delimiter='\t'))
    return validate(m,case,policies,deadlines)
