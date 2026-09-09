#!/usr/bin/env python3
"""Independent audit of a supplied executed artifact. Imports no benchmark evaluator."""
from pathlib import Path
import argparse,hashlib,json,csv,math,statistics
import numpy as np
SHAPES=[('ben',4),('ben',32),('ben',64),('serial',32),('parallel',32),('control',16),('nonlinear',16),('feedback',8),('memory',8)]
def read(p):return json.loads(p.read_text())
def digest(p):
    h=hashlib.sha256()
    with p.open('rb') as f:
        for b in iter(lambda:f.read(1048576),b''):h.update(b)
    return h.hexdigest()
def rows(p):
    with p.open() as f:return list(csv.DictReader(f,delimiter='\t'))
def audio(p):
    x=np.fromfile(p,dtype='<f4');assert x.size and np.isfinite(x).all(),str(p);return x

def compare(x,y):
    assert x.shape==y.shape
    d=np.abs(x.astype(np.float64)-y.astype(np.float64))
    assert np.all(d<=1e-5+1e-5*np.abs(y)),(float(d.max()),int(d.argmax()))
    return float(d.max())
def main(root,out):
    expected={(f'{f}-{n}-b{b}',f,n,b) for f,n in SHAPES for b in ((64,128,512) if (f,n)==('ben',64) else (64,128))}
    manifests=[]
    for family,n in SHAPES:
        k=root/'kernels'/f'{family}-{n}';m=read(k/'manifest.json');assert (m['family'],m['size'])==(family,n)
        for name,want in {**m['source_hashes'],**m['binary_hashes']}.items():assert digest(k/name)==want
        for module in m['modules']:
            abi=next(v for v in m['abi']['modules'] if v['index']==module['index'])
            assert digest(k/(module['class']+'.h'))==abi['header']
            assert hashlib.sha256(module['compiled_source'].encode()).hexdigest()==abi['source']
        assert hashlib.sha256(json.dumps(m['abi'],sort_keys=True).encode()).hexdigest()==m['schema']
        assert all(b['returncode']==0 for b in m['builds'])
        for variant in ['A','B']:
            code=(k/f'kernel-{variant}.cpp').read_text();assert '#include "bank.h"' in code
            assert all(x not in code for x in ['instanceInit','instanceClear','ps_create','memcpy'])
        a={(e['source'],e['target']) for e in m['graphs']['A']['edges']};b={(e['source'],e['target']) for e in m['graphs']['B']['edges']}
        assert a<b and len(b-a)==1
        manifests.append(dict(family=family,size=n,schema=m['schema'],new_route=sorted(b-a)))
    cases=read(root/'gate/cases.json');assert len(cases)==19 and {(c['name'],c['family'],c['size'],c['frames']) for c in cases}==expected
    result=[]
    for c in cases:
        assert c['returncode']==0,c;p=root/'gate'/c['name'];m=read(p/'result.json');channels=m['channels'];assert channels in (2*(c['size']+3),4*(c['size']+3))
        ref=audio(p/'whole-llvm.f32').reshape(8192,channels)
        errors={arm:compare(audio(p/f'{arm}.f32').reshape(ref.shape),ref) for arm in ['shared-optimized','whole-cpp','shared-modular']}
        if (p/'actual-product-taps.f32').exists():errors['product']=compare(audio(p/'actual-product-taps.f32').reshape(ref.shape),ref)
        timing=rows(p/'throughput.tsv');cells={(int(r['trial']),r['backend'],r['measurement']):float(r['ns_per_frame']) for r in timing}
        wanted={(i,a,m) for i in range(11) for a in ['shared-optimized','whole-llvm','whole-cpp','shared-modular'] for m in ['dsp','with-observers']}
        assert len(timing)==len(cells)==88 and set(cells)==wanted and all(math.isfinite(x) and x>0 for x in cells.values())
        medians={measure:{arm:statistics.median(cells[i,arm,measure] for i in range(11)) for arm in ['shared-optimized','whole-llvm','whole-cpp','shared-modular']} for measure in ['dsp','with-observers']}
        ratios={m:v['shared-optimized']/v['whole-llvm'] for m,v in medians.items()}
        result.append(dict(name=c['name'],channels=channels,errors=errors,median_ns_per_frame=medians,ratios=ratios))
    ratios={m:math.exp(statistics.mean(math.log(r['ratios'][m]) for r in result)) for m in ['dsp','with-observers']}
    maxima={m:max(r['ratios'][m] for r in result) for m in ratios}
    positive=ratios['dsp']<=1.25 and ratios['with-observers']<=1.2 and maxima['dsp']<=1.75
    native=read(root/'gate/gate-summary.json');assert native['gate_positive']==positive
    for m in ratios:assert abs(native['ratio_geomean'][m]-ratios[m])<1e-10
    transitioned=[];partitions=[]
    if (root/'transitions/cases.json').exists():
        tcases=read(root/'transitions/cases.json');assert {(c['name'],c['family'],c['size'],c['frames']) for c in tcases}==expected and len(tcases)==19 and positive
        for c in tcases:
            assert c['returncode']==0,c;p=root/'transitions'/c['name'];meta=read(p/'result.json');channels=meta['channels']
            x=audio(p/'transition.f32').reshape(24576,channels);y=audio(p/'independent-llvm-reference.f32').reshape(x.shape);error=compare(x,y)
            blocks=rows(p/'blocks.tsv');offset=0
            for row in blocks:
                assert int(row['offset'])==offset
                stage=0 if offset<4096 else 1 if offset<5120 else 2 if offset<12288 else 3 if offset<14336 else 4
                assert row['mode']==('editable' if stage in [1,3] else 'optimized') and row['topology']==('B' if stage in [1,2] else 'A')
                g=1 if offset<3001 else .73 if offset<10003 else .91;assert abs(float(row['gain'])-g)<1e-6
                offset+=int(row['frames']);assert float(row['compute_us'])>=0
            assert offset==24576
            memory={}
            if c['family']=='memory':
                channel=13 if channels==4*(c['size']+3) else 7
                pos=np.flatnonzero(np.abs(x[:,channel])>.04).tolist();assert pos==[6000]
                neg=audio(p/'reset-negative.f32').reshape(8192,channels);npos=np.flatnonzero(np.abs(neg[:,channel])>.04).tolist();assert npos==[6000]
                memory=dict(impulse_at_absolute_frame=6000,remaining_at_edit=1904,reset_negative_remaining=6000)
            if (p/'no-edit-negative.f32').exists():
                noedit=audio(p/'no-edit-negative.f32').reshape(x.shape);master=(c['size']+1)*(channels//(c['size']+3))
                delta=np.abs(x[4096:12288,master:master+2]-noedit[4096:12288,master:master+2]);assert delta.max()>1e-4
                memory['noedit_master_difference']=float(delta.max())
            transitioned.append(dict(name=c['name'],max_error=error,blocks=len(blocks),**memory))
        for family,n in SHAPES:
            ps=[root/'transitions'/f'{family}-{n}-b{b}'/'transition.f32' for b in ((64,128,512) if (family,n)==('ben',64) else (64,128))]
            for p in ps[1:]:
                x=audio(ps[0]);y=audio(p);e=compare(x,y);partitions.append(dict(pair=[ps[0].parent.name,p.parent.name],bit_identical=np.array_equal(x.view('u4'),y.view('u4')),error=e))
    hashes={};sample_count=0
    for p in root.rglob('*.f32'):
        x=audio(p);sample_count+=x.size;hashes[str(p.relative_to(root))]=digest(p)
    out.mkdir(parents=True,exist_ok=True)
    report=dict(independent=True,source_manifests=manifests,gate_positive=positive,ratios=ratios,ratio_maxima=maxima,gate_cases=result,transitions=transitioned,partition_pairs=partitions,raw_captures=len(hashes),float_samples=sample_count,raw_hashes=hashes)
    (out/'independent-audit.json').write_text(json.dumps(report,indent=2)+'\n')
    print('INDEPENDENT_AUDIT_PASSED',len(result),'static cases',len(transitioned),'transitions',len(hashes),'captures',sample_count,'floats')
    print('GATE',positive,'geomean',ratios,'max',maxima)
    for r in result:print('RESULT',r['name'],json.dumps(r['ratios']),json.dumps(r['median_ns_per_frame']))
    for r in transitioned:print('STATE',json.dumps(r))
    print('PARTITION',json.dumps(partitions))
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('root',type=Path);p.add_argument('output',type=Path);a=p.parse_args();main(a.root,a.output)
