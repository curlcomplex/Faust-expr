#!/usr/bin/env python3
from __future__ import annotations
import array,csv,hashlib,json,math,statistics,sys
from pathlib import Path

def floats(p):
    a=array.array('f');a.frombytes(p.read_bytes())
    if sys.byteorder!='little':a.byteswap()
    if not a or not all(math.isfinite(x) for x in a):raise ValueError('empty/nonfinite '+str(p))
    return a

def compare(a,b):
    if len(a)!=len(b):raise ValueError('length')
    maxerr=0
    for x,y in zip(a,b):
        e=abs(x-y);maxerr=max(maxerr,e)
        if e>1e-5+1e-5*abs(y):raise ValueError('waveform difference '+str(e))
    return maxerr

def tsv(p):
    with p.open() as f:return list(csv.DictReader(f,delimiter='\t'))
def stats(a):
    a=sorted(a)
    if not a or not all(math.isfinite(x) for x in a):raise ValueError('bad metric')
    return dict(n=len(a),median=statistics.median(a),p95=a[math.ceil(.95*len(a))-1],maximum=a[-1])

def gate(root,cases):
    rows=[];failures=[];hashes={}
    for case in cases:
        path=root/case['name']
        if case['returncode']:failures.append(case);continue
        try:
            r=json.loads((path/'result.json').read_text());ref=floats(path/'whole-llvm.f32')
            if len(ref)!=r['channels']*8192:raise ValueError('capture length')
            errors={}
            for backend in ['shared-optimized','whole-cpp','shared-modular']:
                errors[backend]=compare(floats(path/(backend+'.f32')),ref)
            perf=tsv(path/'throughput.tsv')
            expected={(str(i),a,m) for i in range(11) for a in ['shared-optimized','whole-llvm','whole-cpp','shared-modular'] for m in ['dsp','with-observers']}
            if len(perf)!=len(expected) or {(r['trial'],r['backend'],r['measurement']) for r in perf}!=expected:raise ValueError('timing inventory')
            metrics={}
            for measurement in ('dsp','with-observers'):
                metrics[measurement]={b:stats([float(r['ns_per_frame']) for r in perf if r['backend']==b and r['measurement']==measurement]) for b in ['shared-optimized','whole-llvm','whole-cpp','shared-modular']}
            ratio={m:metrics[m]['shared-optimized']['median']/metrics[m]['whole-llvm']['median'] for m in metrics}
            rows.append(dict(name=case['name'],family=case['family'],size=case['size'],frames=case['frames'],errors=errors,metrics=metrics,ratio=ratio,state_bytes=r['state_bytes']))
            for p in path.glob('*.f32'):hashes[str(p.relative_to(root))]=hashlib.sha256(p.read_bytes()).hexdigest()
        except (ValueError,KeyError,OSError) as e:failures.append(dict(name=case['name'],error=str(e)))
    geomean={m:math.exp(statistics.mean(math.log(r['ratio'][m]) for r in rows)) if rows else None for m in ('dsp','with-observers')}
    maximum={m:max((r['ratio'][m] for r in rows),default=None) for m in ('dsp','with-observers')}
    # Engineering screen, declared before native execution. NOT a production SLA.
    positive=not failures and len(rows)==len(cases) and geomean['dsp']<=1.25 and geomean['with-observers']<=1.20 and maximum['dsp']<=1.75
    out=dict(schema=1,scope='matched whole-Faust LLVM vs compiler-generated persistent-module-state unified native loop',cases=rows,failures=failures,
             ratio_geomean=geomean,ratio_max=maximum,gate_positive=positive,limits=dict(dsp_geomean=1.25,observer_geomean=1.20,dsp_worst=1.75),raw_hashes=hashes)
    (root/'gate-summary.json').write_text(json.dumps(out,indent=2)+'\n');return out

def transitions(root,cases):
    results=[];failures=[]
    for c in cases:
        path=root/c['name']
        if c['returncode']:failures.append(c);continue
        try:
            r=json.loads((path/'result.json').read_text());a=floats(path/'transition.f32');b=floats(path/'independent-llvm-reference.f32')
            if len(a)!=r['channels']*24576:raise ValueError('capture length')
            error=compare(a,b);blocks=tsv(path/'blocks.tsv');offset=0;stages=set()
            for row in blocks:
                if int(row['offset'])!=offset:raise ValueError('sample clock gap')
                offset+=int(row['frames']);stages.add((row['mode'],row['topology']))
            if offset!=24576 or stages!={('optimized','A'),('optimized','B'),('editable','A'),('editable','B')}:raise ValueError('missing transition stage')
            if c['family']=='memory':
                # Module 3 (delay) right output = channel 13. Marker enters final module8, not module3.
                ch=13;positions=[i for i in range(24576) if abs(a[i*r['channels']+ch])>.04]
                if positions!=[6000]:raise ValueError('persistent delay history lost '+str(positions[:5]))
                neg=floats(path/'reset-negative.f32');npositions=[i for i in range(8192) if abs(neg[i*r['channels']+ch])>.04]
                if npositions!=[6000]:raise ValueError('reset negative impulse missing')
            results.append(dict(name=c['name'],max_error=error,blocks=len(blocks),compute_us=stats([float(x['compute_us']) for x in blocks])))
        except (ValueError,KeyError,OSError) as e:failures.append(dict(name=c['name'],error=str(e)))
    summary=dict(passed=not failures and len(results)==len(cases),cases=results,failures=failures)
    (root/'transition-summary.json').write_text(json.dumps(summary,indent=2)+'\n');return summary
