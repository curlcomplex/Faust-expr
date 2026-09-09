#!/usr/bin/env python3
"""Full-capture checks, explicit failures, stage timings; no gain/time fitting."""
from pathlib import Path
import json, math, statistics, sys, hashlib
from analyze import read_tsv, samples, stats

def definitions():
    result=[]
    for frames in (64,128):
        for family,n in [('serial',4),('serial',16),('serial',32),('parallel',4),('parallel',16),
                         ('parallel',32),('parallel',64),('feedback',8),('control',16)]:
            result.extend([('matrix',family,n,frames),('novel',family,n,frames)])
        result.extend([('nonlinear','serial',16,frames),('continuity','serial',4,frames),
                       ('source','serial',4,frames),('paced','serial',32,frames),('paced','parallel',32,frames)])
    return result

def compare(a,b):
    if len(a)!=len(b) or not a:raise ValueError('comparison length')
    error=max(abs(x-y) for x,y in zip(a,b))
    if any(not math.isfinite(x) or not math.isfinite(y) or abs(x-y)>1e-5+1e-5*abs(y) for x,y in zip(a,b)):
        raise ValueError(f'full capture mismatch {error}')
    return error

def analyze(root):
    cases=json.loads((root/'retained-cases.json').read_text()); case_names={f'{m}-{f}-{n}-f{b}' for m,f,n,b in definitions()}
    if len(cases)!=len(case_names) or {c['name'] for c in cases}!=case_names:raise ValueError('incomplete/duplicate cases')
    result={'schema':1,'scope':'retained authoring candidate in existing native harness; not deployed UI or Core Audio',
            'cases':[],'failures':[],'raw_hashes':{}}
    for c in cases:
        folder=root/c['name']; frames=c['frames']; item={'name':c['name'],'mode':c['mode'],'frames':frames}
        for file in folder.glob('*.f32'):
            samples(file);result['raw_hashes'][str(file.relative_to(root))]=hashlib.sha256(file.read_bytes()).hexdigest()
        if c['returncode']!=0:result['failures'].append(c);continue
        try:
            if c['mode']=='matrix':
                a=samples(folder/'edited.f32');b=samples(folder/'continued-fused.f32');blocks=read_tsv(folder/'blocks.tsv')
                if len(a)!=len(blocks)*frames*2 or [int(r['block']) for r in blocks]!=list(range(len(blocks))):raise ValueError('block identity/length')
                expected=[v+(0.01 if i%2 else .02)*math.sqrt(.5)*int(blocks[i//(2*frames)]['marker_connected']) for i,v in enumerate(b)]
                item['max_error']=compare(a,expected)
                edits=read_tsv(folder/'edits.tsv')
                if [int(r['trial']) for r in edits]!=list(range(32)) or any(int(r['created']) or int(r['acquired']) for r in edits):raise ValueError('recompiled/missing edit')
                item['prepare_us']=stats([float(r['prepare_us']) for r in edits]);item['edit_to_computed_us']=stats([float(r['edit_to_changed_compute_us']) for r in edits])
                perf=read_tsv(folder/'throughput.tsv')
                if len(perf)!=14 or len({(r['trial'],r['backend']) for r in perf})!=14:raise ValueError('timing cells')
                item['ns_per_frame']={backend:stats([float(r['ns_per_frame']) for r in perf if r['backend']==backend]) for backend in ('retained','fused')}
                item['retained_time_over_fused']=item['ns_per_frame']['retained']['median']/item['ns_per_frame']['fused']['median']
                item['initial']=read_tsv(folder/'initial.tsv')[0]
            elif c['mode'] in ('novel','nonlinear'):
                a=samples(folder/'novel-edited.f32');b=samples(folder/'scripted-fused-oracle.f32')
                if len(a)!=16384:raise ValueError('novel capture length')
                item['max_error']=compare(a,b);item['event']=read_tsv(folder/'novel.tsv')[0]
                if int(item['event']['created']) or int(item['event']['acquired']):raise ValueError('novel edit compiled')
            elif c['mode']=='continuity':
                a=samples(folder/'preserved-source.f32');b=samples(folder/'continued-source.f32');negative=samples(folder/'reset-negative.f32')
                if len(a)!=16384:raise ValueError('continuity length')
                item['max_error']=compare(a,b)
                def impulse(x):
                    found=[i for i,v in enumerate(x[1::2]) if abs(v)>.04]
                    if len(found)!=1:raise ValueError('impulse count')
                    return found[0]
                item['preserved_impulse_frame']=impulse(a);item['reset_impulse_frame']=impulse(negative)
                if impulse(a)!=1904 or impulse(negative)!=6000 or max(abs(x-y) for x,y in zip(a,negative))<=.01:raise ValueError('continuity/reset control')
            elif c['mode']=='source':
                item['max_error']=compare(samples(folder/'unaffected-source.f32'),samples(folder/'continued-source.f32'))
                item['event']=read_tsv(folder/'source-edit.tsv')[0]
                if int(item['event']['created'])!=1 or int(item['event']['acquired'])!=1:raise ValueError('source invalidation count')
                if (folder/'negative-controls.txt').read_text().count('rejected')!=2:raise ValueError('negative checks')
            elif c['mode']=='paced':
                a=samples(folder/'paced-edited.f32');b=samples(folder/'paced-continued-fused.f32');blocks=read_tsv(folder/'callbacks.tsv');events=read_tsv(folder/'events.tsv')
                if len(a)!=len(blocks)*frames*2 or [int(r['block']) for r in blocks]!=list(range(len(blocks))):raise ValueError('paced length/identity')
                revisions=[int(r['revision']) for r in blocks]
                if revisions!=sorted(revisions) or [int(r['revision']) for r in events]!=list(range(2,18)):raise ValueError('stale/missing revisions')
                expected=[v+(0.01 if i%2 else .02)*math.sqrt(.5)*(revisions[i//(2*frames)]>=2 and revisions[i//(2*frames)]%2==0) for i,v in enumerate(b)]
                item['max_error']=compare(a,expected)
                latency=[]
                for event in events:
                    rev=int(event['revision']);first=next((r for r in blocks if int(r['revision'])==rev),None)
                    if first is None or int(event['created']) or int(event['acquired']) or int(event['stale_rejected'])!=1:raise ValueError('unobserved event/compile/stale')
                    dt=float(first['end_us'])-float(event['request_us'])
                    if dt<0:raise ValueError('timestamp ordering')
                    latency.append(dt)
                item['request_to_changed_compute_us']=stats(latency)
                item['synthetic_deadline_misses']=sum(float(r['end_us'])>float(r['scheduled_us'])+frames*1e6/48000 for r in blocks)
                item['callback_count']=len(blocks)
                item['compute_us']=stats([float(r['end_us'])-float(r['start_us']) for r in blocks])
            result['cases'].append(item)
        except (ValueError,KeyError,StopIteration,OSError) as e:
            result['failures'].append({'name':c['name'],'verification_error':str(e)})
    result['passed']=not result['failures'];result['expected_case_count']=len(case_names)
    (root/'retained-summary.json').write_text(json.dumps(result,indent=2)+'\n')
    return result

if __name__=='__main__':sys.exit(0 if analyze(Path(sys.argv[1]))['passed'] else 1)
