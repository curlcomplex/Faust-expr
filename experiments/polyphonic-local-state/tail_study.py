#!/usr/bin/env python3
"""Extend the existing physical benchmark; no alternative engine or workflow."""
from __future__ import annotations
import argparse, csv, hashlib, json, math, os, random, statistics, subprocess, time
from pathlib import Path
import numpy as np
HERE=Path(__file__).resolve().parent

def read(p):return json.loads(p.read_text())
def digest(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def distribution(values):
    x=sorted(float(v) for v in values)
    if not x or not all(math.isfinite(v) for v in x):raise ValueError('invalid timing values')
    return dict(count=len(x),median=statistics.median(x),p99=x[math.ceil(.99*len(x))-1],maximum=x[-1])
def audio(p):
    x=np.fromfile(p,dtype='<f4')
    if not len(x) or len(x)%2 or not np.isfinite(x).all():raise ValueError('invalid raw audio '+str(p))
    return x.reshape(-1,2)
def compare(x,y):
    if x.shape!=y.shape:raise ValueError('audio length mismatch')
    e=np.abs(x.astype('f8')-y.astype('f8'))
    if not np.all(e<=1e-5+1e-5*np.abs(y)):raise ValueError('audio mismatch '+str(e.max()))
    return float(e.max())
def audit(folder,case):
    m=read(folder/'result.json');x=audio(folder/'live.f32');y=audio(folder/'reference.f32');a=audio(folder/'counterfactual-A.f32')
    error=compare(x,y)
    with (folder/'deadline.tsv').open() as f:rows=list(csv.DictReader(f,delimiter='\t'))
    block=case['block'];period=block*1e6/48000
    assert len(x)==m['sample_frames']==len(rows)*block
    assert m['state_objects_replaced']==0 and type(m['state_objects_replaced']) is int
    assert m['handoff']==case['handoff'] and m['policy']==case['policy'] and m['fault']==case['fault']
    assert m['participants']==case['participants'] and m['grain']==case['grain']
    previous=None
    for i,r in enumerate(rows):
        assert int(r['sample'])==i*block and int(r['frames'])==block
        for key in ('scheduled_us','entry_us','render_begin_us','render_end_us','end_us','owner_cpu_us','job_cpu_us','job_max_wall_us','job_max_off_cpu_us','job_last_start_us'):
            r[key]=float(r[key]);assert math.isfinite(r[key])
        assert r['scheduled_us']<=r['entry_us']+.5
        assert r['entry_us']<=r['render_begin_us']<=r['render_end_us']<=r['end_us']
        assert r['owner_cpu_us']>=0
        if previous:
            assert r['entry_us']>=previous['end_us']
            assert abs(r['scheduled_us']-previous['scheduled_us']-period)<.1
        previous=r
    fault=case['fault']!='none'
    changed=np.flatnonzero(np.max(np.abs(x.astype('f8')-a.astype('f8')),axis=1)>1e-5)
    adopted=[i for i,r in enumerate(rows) if r['phase']=='optimized-B']
    if fault:
        assert not m['adopted'] and not adopted and not len(changed)
        compare(x,a)
    else:
        assert m['adopted'] and len(adopted)==128 and len(changed)
        assert all(rows[i]['phase']=='optimized-B' for i in range(adopted[0],len(rows)))
        if case['policy']=='deferred':compare(x[:adopted[0]*block],a[:adopted[0]*block])
        first=rows[adopted[0]]
        if case['handoff']=='owner':
            assert m['loader_on_render_owner']
            assert first['entry_us']<=m['load_begin_us']<=m['load_end_us']<=first['render_begin_us']
        elif case['handoff']=='prepared':
            assert not m['loader_on_render_owner']
            assert m['load_end_us']<=m['published_us']<=m['ready_us']<=first['render_begin_us']
        else:assert m['precompiled_control'] and m['load_end_us']<rows[0]['entry_us']
        if case['policy']!='deferred' and case['handoff']!='prebuilt':
            assert rows[int(changed[0])//block]['end_us']<m['compile_end_us']
    assert m['context']==case['context']
    policy_counts={key:sum(int(r[key]) for r in rows) for key in ('worker_tc_jobs','worker_other_jobs','caller_tc_jobs','policy_errors')}
    if case['probe']:
        assert policy_counts['policy_errors']==0 and policy_counts['worker_tc_jobs']+policy_counts['worker_other_jobs']>0
        if case['context']=='workers':assert policy_counts['worker_tc_jobs']>0 and policy_counts['worker_other_jobs']==0,'worker realtime policy not installed'
    summary=dict(case=case,max_error=error,frames=len(x),adopted=m['adopted'],phases={},thread_policy_counts=policy_counts,caller_thread_policy=m['caller_thread_policy'],compiler_thread_policy=m['compiler_thread_policy'])
    summary['load_ms']=(m['load_end_us']-m['load_begin_us'])/1000 if m['load_end_us'] else None
    summary['compile_ms']=(m['compile_end_us']-m['compile_begin_us'])/1000
    summary['first_changed_block_ms']=(rows[int(changed[0])//block]['end_us']-m['request_us'])/1000 if len(changed) else None
    for phase in sorted({r['phase'] for r in rows}):
        rs=[r for r in rows if r['phase']==phase]
        elapsed=[r['end_us']-r['entry_us'] for r in rs]
        render=[r['render_end_us']-r['render_begin_us'] for r in rs]
        handoff=[r['render_begin_us']-r['entry_us'] for r in rs]
        lateness=[max(0,r['end_us']-r['scheduled_us']-period) for r in rs]
        summary['phases'][phase]=dict(callback_us=distribution(elapsed),render_us=distribution(render),handoff_us=distribution(handoff),deadline_lateness_us=distribution(lateness),callback_over_budget=sum(v>period for v in elapsed),missed_deadlines=sum(v>0 for v in lateness))
    summary['all_callback_us']=distribution([r['end_us']-r['entry_us'] for r in rows])
    summary['all_deadline_misses']=sum(r['end_us']>r['scheduled_us']+period for r in rows)
    summary['maximum_output_interval_ms']=max((rows[i]['end_us']-rows[i-1]['end_us'])/1000 for i in range(1,len(rows)))
    if adopted:summary['adoption_callback_us']=rows[adopted[0]]['end_us']-rows[adopted[0]]['entry_us']
    summary['raw_sha256']={p.name:digest(p) for p in folder.glob('*.f32')}
    return summary

def definitions(phase):
    cases=[]
    def add(family,n,p,grain,policy,handoff,repeat=0,fault='none',probe=False,context='legacy'):
        cases.append(dict(family=family,stages=n,participants=p,grain=grain,policy=policy,handoff=handoff,repeat=repeat,fault=fault,probe=probe,block=128,voices=16,context=context))
    if phase=='handoff':
        for fault in ('compile','stale','schema'):add('serial',16,1,4,'deferred','prepared',fault=fault)
        for repeat in range(2):
            for family,n in [('serial',16),('parallel',16)]:
                for policy in ('local','deferred'):
                    for handoff in ('owner','prepared','prebuilt'):add(family,n,8,4,policy,handoff,repeat)
        for family,n in [('feedback',8),('memory',8)]:
            for policy in ('local','deferred'):add(family,n,8,4,policy,'prepared')
    elif phase=='scheduler':
        for repeat in range(3):
            for family,n in [('serial',16),('parallel',16)]:
                for p in (4,8):
                    for grain in (1,4):
                        for policy in ('local','deferred'):
                            for handoff in ('prepared','prebuilt'):add(family,n,p,grain,policy,handoff,repeat)
        # Instrumented attribution controls, separate from the uninstrumented comparisons.
        for family,n in [('serial',16),('parallel',16)]:
            for p in (4,8):
                for handoff in ('prepared','prebuilt'):add(family,n,p,4,'local',handoff,probe=True)
    elif phase=='context':
        for repeat in range(3):
            for family,n in [('serial',16),('parallel',16)]:
                for p in (4,8):
                    for policy in ('local','deferred'):
                        for handoff in ('prepared','prebuilt'):
                            for context in ('legacy','workers'):add(family,n,p,4,policy,handoff,repeat,probe=True,context=context)
    else:raise ValueError('unknown phase')
    random.Random(261010 if phase=='handoff' else 261011).shuffle(cases)
    return cases

def run(exe,kernels,out,phase):
    out.mkdir(parents=True,exist_ok=True)
    planned=definitions(phase);completed=[]
    (out/'planned.json').write_text(json.dumps(planned,indent=2)+'\n')
    identity=dict(executable_sha256=digest(exe),source_sha256={p.name:digest(p) for p in HERE.iterdir() if p.is_file()},phase=phase)
    (out/'identity.json').write_text(json.dumps(identity,indent=2)+'\n')
    for index,c in enumerate(planned):
        name=f"{index:03d}-{c['policy']}-{c['family']}-p{c['participants']}-g{c['grain']}-{c['handoff']}-r{c['repeat']}-{c['fault']}-probe{int(c['probe'])}"
        folder=out/name;folder.mkdir(exist_ok=True)
        cmd=[str(exe),c['policy'],c['family'],str(c['stages']),str(c['block']),str(c['voices']),str(c['participants']),str(c['grain']),str(kernels/f"{c['family']}-{c['stages']}"),str(folder)]
        env=os.environ.copy();env.update(PS_HANDOFF=c['handoff'],PS_HANDOFF_FAULT=c['fault'],PS_JOB_PROBE=str(int(c['probe'])),PS_RT_CONTEXT=c['context'])
        record=dict(name=name,case=c,command=cmd,environment={k:env[k] for k in ('PS_HANDOFF','PS_HANDOFF_FAULT','PS_JOB_PROBE','PS_FAUST_PREFIX','PS_RT_CONTEXT')})
        start=time.monotonic()
        try:
            with (folder/'native.log').open('w') as log:r=subprocess.run(cmd,env=env,stdout=log,stderr=subprocess.STDOUT,timeout=100)
            record['returncode']=r.returncode
            if r.returncode==0:record['audit']=audit(folder,c)
        except (subprocess.TimeoutExpired,AssertionError,ValueError,KeyError,OSError) as e:
            record['returncode']=124 if isinstance(e,subprocess.TimeoutExpired) else 2;record['error']=str(e)
        record['seconds']=time.monotonic()-start;completed.append(record)
        (out/'cases.json').write_text(json.dumps(completed,indent=2)+'\n')
        print('TAIL_CASE',phase,name,record['returncode'],flush=True)
        if record['returncode']:
            print((folder/'native.log').read_text()[-10000:],flush=True)
            raise RuntimeError('tail case failed; all evidence retained: '+name)
    assert len(completed)==len(planned)
    assert digest(exe)==identity['executable_sha256']
    for name,sha in identity['source_sha256'].items():assert digest(HERE/name)==sha
    summary=dict(passed=True,phase=phase,case_count=len(completed),cases=[r['audit'] for r in completed],real_time_device_acceptance=False)
    (out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
    print('TAIL_AUDIT',phase,'PASS',len(completed),flush=True)
    return summary

if __name__=='__main__':
    ap=argparse.ArgumentParser();ap.add_argument('--prepared-root',type=Path,required=True);ap.add_argument('--output',type=Path,required=True);ap.add_argument('--phase',choices=['handoff','scheduler','context'],required=True)
    args=ap.parse_args();root=args.prepared_root.resolve();exe=HERE/'build/CombinedPolyBench'
    assert digest(exe)==read(root/'identity.json')['executable_sha256'],'prepared executable changed'
    run(exe,root/'kernels',args.output.resolve(),args.phase)
