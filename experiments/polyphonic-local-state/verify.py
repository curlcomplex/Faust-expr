#!/usr/bin/env python3
"""Read native files independently; no imports from generator or benchmark evaluators."""
import argparse,csv,hashlib,json,math,statistics
from pathlib import Path
import numpy as np

def read(p):return json.loads(p.read_text())
def rows(p):
    with p.open() as f:return list(csv.DictReader(f,delimiter='\t'))
def audio(p):
    x=np.fromfile(p,dtype='<f4');assert len(x)>0 and len(x)%2==0 and np.isfinite(x).all(),str(p);return x.reshape(-1,2)
def comparison(x,y):
    assert x.shape==y.shape
    error=np.abs(x.astype('f8')-y.astype('f8'));assert np.all(error<=1e-5+1e-5*np.abs(y)),float(error.max())
    return float(error.max())
def stats(x):
    x=sorted(x);assert x and all(math.isfinite(v) and v>=0 for v in x)
    return dict(count=len(x),median=statistics.median(x),p95=x[math.ceil(.95*len(x))-1],maximum=max(x))
def main(root):
    planned=read(root/'planned-cases.json');cases=read(root/'cases.json');failures=[];result=[];raw={}
    expected={tuple(x) for x in planned};observed={(c['mode'],c['family'],c['stages'],c['block'],c['voices'],c['participants'],c['grain']) for c in cases}
    if len(cases)!=len(expected) or observed!=expected:failures.append({'inventory':'missing or duplicated planned native cases','planned':len(expected),'executed':len(cases)})
    for c in cases:
        folder=root/'cases'/c['name']
        if c['returncode']:failures.append(c);continue
        try:
            meta=read(folder/'result.json');item={k:c[k] for k in ['name','mode','family','stages','block','voices','participants','grain']};item['worker_threads_observed']=meta['workers_observed']
            if c['mode']=='conformance':
                x=audio(folder/'candidate.f32');y=audio(folder/'independent-llvm.f32');assert len(x)==48000
                item['max_error']=comparison(x,y);assert meta['allocations_unchanged'] and meta['all_notes_retired']
                trace=rows(folder/'voice-states.tsv');offset=0;silent=0
                for row in trace:
                    assert int(row['sample'])==offset;offset+=int(row['frames']);assert 0<=int(row['active'])<=16
                    silent+=int(row['active'])==0
                assert offset==48000 and silent>0
                assert {0,13,1249,2049,3001,4096,5120,8192,9216,10003,24000,30007}<={int(r['sample']) for r in trace}
                item['silent_chunks']=silent
            elif c['mode']=='benchmark':
                timing=rows(folder/'timing.tsv');wanted={(str(i),arm,mode) for i in range(9) for mode in ['optimized-A','whole-fallback-B','local-fallback-B','optimized-B'] for arm in (['stock-poly-whole-LLVM','stock-poly-shared-state','tracktion-pool-shared-state'] if mode=='optimized-A' else ['stock-poly-shared-state','tracktion-pool-shared-state'])}
                assert len(timing)==len(wanted)==81 and {(r['trial'],r['arm'],r['mode']) for r in timing}==wanted
                assert all(int(r['voices'])==4*c['voices'] for r in timing)
                summaries={}
                for mode in ['optimized-A','whole-fallback-B','local-fallback-B','optimized-B']:
                    arms={r['arm'] for r in timing if r['mode']==mode};summaries[mode]={a:stats([float(r['ns_per_frame']) for r in timing if r['mode']==mode and r['arm']==a]) for a in arms}
                item['processing_ns_per_frame']=summaries
                stock=summaries['optimized-A']['stock-poly-whole-LLVM']['median'];shared=summaries['optimized-A']['stock-poly-shared-state']['median'];pool=summaries['optimized-A']['tracktion-pool-shared-state']['median']
                item['optimized_pool_speedup_vs_stock_llvm']=stock/pool;item['pool_speedup_vs_same_shared_serial']=shared/pool
                item['local_over_whole_fallback']=summaries['local-fallback-B']['tracktion-pool-shared-state']['median']/summaries['whole-fallback-B']['tracktion-pool-shared-state']['median']
            else:
                x=audio(folder/'live.f32');y=audio(folder/'reference.f32');assert len(x)==meta['sample_frames'];item['max_error']=comparison(x,y)
                assert meta['compiled_instruments']==1 and meta['state_objects_replaced']==0
                assert meta['unaffected_optimized_instruments']==(0 if c['mode']=='whole' else 3)
                trace=rows(folder/'callbacks.tsv');offset=0;phases={}
                for r in trace:
                    assert int(r['sample'])==offset;offset+=int(r['frames']);phase=r['phase'];phases.setdefault(phase,[]).append(float(r['end_us'])-float(r['begin_us']))
                    assert float(r['end_us'])>=float(r['begin_us'])
                assert offset==len(x) and 'optimized-B' in phases and 'before' in phases
                waiting='pending-A' if c['mode']=='deferred' else 'editable-B';assert waiting in phases
                item['phase_compute_us']={p:stats(t) for p,t in phases.items()}
                first=next(r for r in trace if r['phase']==('optimized-B' if c['mode']=='deferred' else 'editable-B'))
                item['request_to_selected_B_block_ms']=(float(first['end_us'])-meta['request_us'])/1000
                item['compile_ms']=(meta['compile_end_us']-meta['compile_begin_us'])/1000
                item['budget_us']=c['block']*1e6/48000
                item['compute_over_budget']={p:sum(v>item['budget_us'] for v in t) for p,t in phases.items()}
                assert (folder/'new-instrument-B.cpp').exists() and (folder/'new-instrument-B.dylib').exists()
                if c['mode']!='deferred':assert float(first['end_us'])<meta['compile_end_us']
            result.append(item)
        except (AssertionError,ValueError,KeyError,OSError) as e:failures.append({'name':c['name'],'audit_failure':str(e)})
        for p in folder.glob('*.f32'):audio(p);raw[str(p.relative_to(root))]=hashlib.sha256(p.read_bytes()).hexdigest()
    summary={'passed':not failures,'cases':result,'failures':failures,'planned_case_count':len(planned),'raw_hashes':raw,
             'real_time_acceptance':False,'scope':'one shared Tracktion pool; no device AudioWorkgroup; prototype voice-steal/release compatibility; not full CURLOP polyphony/CV/VM/MPE integration'}
    (root/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
    print('COMBINED_AUDIT',summary['passed'],'accepted',len(result),'planned',len(planned),'failures',json.dumps(failures),flush=True)
    for r in result:
        if r['mode']=='benchmark':print('COST',r['name'],'pool/stock speedup',r['optimized_pool_speedup_vs_stock_llvm'],'pool/shared speedup',r['pool_speedup_vs_same_shared_serial'],'local/whole time',r['local_over_whole_fallback'],'threads',r['worker_threads_observed'],flush=True)
        elif r['mode'] not in ('conformance',):print('ONLINE',r['name'],json.dumps(r),flush=True)
    if failures:raise RuntimeError('combined evidence not accepted')
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('root',type=Path);a=p.parse_args();main(a.root)
