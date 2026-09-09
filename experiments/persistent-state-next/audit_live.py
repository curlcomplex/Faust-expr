#!/usr/bin/env python3
"""Independent live-audio, timeline and transient-cost audit; no native evaluator imports."""
from pathlib import Path
import argparse,csv,hashlib,json,math,statistics
import numpy as np

def read(p):return json.loads(p.read_text())
def tsv(p):
    with p.open() as f:return list(csv.DictReader(f,delimiter='\t'))
def audio(p):
    x=np.fromfile(p,dtype='<f4');assert x.size and np.isfinite(x).all(),str(p);return x

def stats(xs):
    xs=sorted(xs);assert xs and all(math.isfinite(x) for x in xs)
    return {'count':len(xs),'median':statistics.median(xs),'p95':xs[math.ceil(.95*len(xs))-1],'maximum':max(xs)}
def digest(p):
    h=hashlib.sha256()
    with p.open('rb') as f:
        for x in iter(lambda:f.read(1048576),b''):h.update(x)
    return h.hexdigest()

def main(root,out):
    cases=read(root/'live-cases.json')
    wanted={(m,f,n,128) for m in ('live','live-stress') for f,n in [('ben',32),('parallel',32),('feedback',8),('memory',8)]}
    assert len(cases)==8 and {(c['mode'],c['family'],c['size'],c['frames']) for c in cases}==wanted
    results=[];failed=[];hashes={};samples=0
    for c in cases:
        p=root/c['name']
        if c['returncode']:
            failed.append(c);continue
        try:
            r=read(p/'live-result.json');blocks=tsv(p/'callbacks.tsv');events=tsv(p/'events.tsv')
            assert [int(b['block']) for b in blocks]==list(range(len(blocks)))
            assert len(blocks)==r['blocks'] and r['frames']==c['frames'] and r['channels']==2*(c['size']+3)
            assert [(int(e['revision']),e['topology']) for e in events]==[(1,'B'),(2,'A'),(3,'B')]
            revisions=[int(b['revision']) for b in blocks];assert revisions==sorted(revisions) and set(revisions)<={0,1,2,3}
            for b in blocks:
                rev=int(b['revision']);assert b['topology']==('B' if rev in (1,3) else 'A')
                begin,end,scheduled=[float(b[k]) for k in ['begin_us','end_us','scheduled_us']]
                assert end>=begin and math.isfinite(end+begin+scheduled)
                if rev:
                    event=events[rev-1];assert begin>=float(event['request_us'])
                if rev==1 and int(b['optimized']):assert begin>=r['compile_end_us']
                if rev==2:assert not int(b['optimized']), 'superseded A must not be installed'
            edited=[b for b in blocks if b['revision']=='1' and b['optimized']=='0']
            optimized=[b for b in blocks if b['revision']=='1' and b['optimized']=='1'];assert edited and optimized
            assert float(edited[0]['end_us'])<r['compile_end_us']
            dt=(float(edited[0]['end_us'])-r['request_us'])/1000
            ot=(float(optimized[0]['end_us'])-r['request_us'])/1000
            assert abs(dt-r['request_to_editable_compute_ms'])<1e-8 and abs(ot-r['request_to_optimized_compute_ms'])<1e-8
            assert abs((r['compile_end_us']-r['compile_begin_us'])/1000-r['compile_ms'])<1e-8
            x=audio(p/'live-output.f32').reshape(-1,r['channels']);y=audio(p/'live-reference.f32').reshape(x.shape);neg=audio(p/'live-noedit-negative.f32').reshape(x.shape)
            assert len(x)==len(blocks)*r['frames']
            err=np.abs(x.astype('f8')-y.astype('f8'));assert np.all(err<=1e-5+1e-5*np.abs(y))
            slots=[i for i,b in enumerate(blocks) if b['revision']=='1' and float(b['end_us'])<r['compile_end_us']]
            assert slots
            master=2*(c['size']+1);delta=0.0
            for i in slots:
                lo=i*r['frames'];hi=lo+r['frames'];delta=max(delta,float(np.max(np.abs(x[lo:hi,master:master+2]-neg[lo:hi,master:master+2]))))
            assert delta>1e-4 and abs(delta-r['changed_master_before_compile'])<1e-6
            assert all(r[k] for k in ['stale_rejected','failed_rejected','schema_rejected'])
            assert r['state_instances_created_during_edits']==0 and r['state_bytes_copied_during_edits']==0
            source=(p/'online-B.cpp').read_text();assert 'for(int s=0;s<count;++s)' in source
            assert all(z not in source for z in ['instanceInit','instanceClear','memcpy','ps_create'])
            assert (p/'online-B.dylib').stat().st_size>0 and (p/'stale-A.dylib').stat().st_size>0
            assert 'INTENTIONAL_COMPILE_FAILURE' in (p/'invalid-negative-compile.log').read_text()
            phases={}
            period=r['frames']*1e6/48000
            for label,selector in [('initial-optimized',lambda b:b['revision']=='0'),('editable-during-compile',lambda b:b['revision']=='1' and b['optimized']=='0'),('new-optimized',lambda b:b['revision']=='1' and b['optimized']=='1'),('later-edit-burst',lambda b:int(b['revision'])>=2)]:
                sub=[b for b in blocks if selector(b)]
                if not sub:continue
                compute=[float(b['end_us'])-float(b['begin_us']) for b in sub]
                phases[label]={'compute_us':stats(compute),'median_budget_fraction':statistics.median(compute)/period,
                    'synthetic_schedule_misses':sum(float(b['end_us'])>float(b['scheduled_us'])+period for b in sub),
                    'compute_over_budget':sum(t>period for t in compute),'wake_lateness_us':stats([max(0,float(b['begin_us'])-float(b['scheduled_us'])) for b in sub])}
            memory={}
            if c['family']=='memory':
                positions=np.flatnonzero(np.abs(x[:,7])>.04).tolist();assert positions==[6000]
                memory={'pulse_absolute_frame':6000,'first_edit_block_frame':blocks.index(edited[0])*r['frames']}
            for file in p.iterdir():
                if file.suffix in ('.f32','.dylib','.cpp'):
                    hashes[str(file.relative_to(root))]=digest(file)
                    if file.suffix=='.f32':samples+=file.stat().st_size//4
            results.append({'name':c['name'],'stress':r['stress'],'copies':r['state_copies_for_load'],'calibration_us_per_copy':r['calibration_us_per_copy'],
                'edit_response_ms':dt,'optimized_response_ms':ot,'compile_ms':r['compile_ms'],'max_waveform_error':float(err.max()),'changed_master_before_compile':delta,
                'phases':phases,**memory})
        except (AssertionError,ValueError,KeyError,OSError) as e:failed.append({'name':c['name'],'audit_failure':str(e)})
    out.mkdir(parents=True,exist_ok=True)
    summary={'passed':not failed and len(results)==8,'cases':results,'failures':failed,'checked_float_samples':samples,'file_hashes':hashes,
        'scope':'normal-thread VM schedule; not Core Audio or proof of real-time safety; all code owners retained until join'}
    (out/'live-independent-audit.json').write_text(json.dumps(summary,indent=2)+'\n')
    for r in results:print('LIVE_AUDIT',json.dumps(r),flush=True)
    print('LIVE_AUDIT_PASSED',summary['passed'],'cases',len(results),'failures',json.dumps(failed),flush=True)
    if not summary['passed']:raise RuntimeError('live independent audit failed')
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('root',type=Path);p.add_argument('output',type=Path);a=p.parse_args();main(a.root,a.output)
