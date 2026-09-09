#!/usr/bin/env python3
"""Independent regression evidence reader. No imports from any benchmark evaluator.
A skipped paced revision remains a failure, even if all produced audio is valid.
"""
from pathlib import Path
import argparse,csv,hashlib,json,math
import numpy as np

def rows(p):
    with p.open() as f:return list(csv.DictReader(f,delimiter='\t'))
def read(p):return json.loads(p.read_text())
def audio(p):
    x=np.fromfile(p,dtype='<f4');assert x.size and np.isfinite(x).all(),str(p);return x

def compare(x,y):
    assert x.shape==y.shape and np.isfinite(x).all() and np.isfinite(y).all()
    d=np.abs(x.astype('f8')-y.astype('f8'));assert np.all(d<=1e-5+1e-5*np.abs(y))
    return float(d.max())
def expected():
    result=set()
    for frames in (64,128):
        for family,n in [('serial',4),('serial',16),('serial',32),('parallel',4),('parallel',16),('parallel',32),('parallel',64),('feedback',8),('control',16)]:
            for mode in ('matrix','novel'):result.add((f'{mode}-{family}-{n}-f{frames}',mode,family,n,frames))
        for mode,family,n in [('nonlinear','serial',16),('continuity','serial',4),('source','serial',4),('paced','serial',32),('paced','parallel',32)]:
            result.add((f'{mode}-{family}-{n}-f{frames}',mode,family,n,frames))
    return result

def main(root,out):
    cases=read(root/'retained-cases.json');assert len(cases)==46
    assert {(c['name'],c['mode'],c['family'],c['size'],c['frames']) for c in cases}==expected()
    native=read(root/'retained-summary.json');results=[];failures=[];hashes={};samples=0
    marker=np.array([.02,.01])*math.sqrt(.5)
    for c in cases:
        p=root/c['name'];frames=c['frames'];mode=c['mode']
        try:
            assert c['returncode']==0,c
            item={'name':c['name']}
            if mode=='matrix':
                b=rows(p/'blocks.tsv');x=audio(p/'edited.f32').reshape(-1,2);y=audio(p/'continued-fused.f32').reshape(x.shape)
                assert [int(r['block']) for r in b]==list(range(len(b))) and len(x)==len(b)*frames
                connected=np.repeat([int(r['marker_connected']) for r in b],frames)
                item['max_error']=compare(x,y+connected[:,None]*marker)
                events=rows(p/'edits.tsv');assert [int(e['trial']) for e in events]==list(range(32))
                for e in events:
                    assert int(e['created'])==0 and int(e['acquired'])==0 and int(e['reused'])==c['size']+3
                perf=rows(p/'throughput.tsv')
                assert len(perf)==14 and {(int(r['trial']),r['backend']) for r in perf}=={(i,b) for i in range(7) for b in ('retained','fused')}
                assert all(math.isfinite(float(r['ns_per_frame'])) and float(r['ns_per_frame'])>0 for r in perf)
            elif mode in ('novel','nonlinear'):
                x=audio(p/'novel-edited.f32');y=audio(p/'scripted-fused-oracle.f32');assert x.size==16384
                item['max_error']=compare(x,y);ev=rows(p/'novel.tsv');assert len(ev)==1 and int(ev[0]['created'])==0 and int(ev[0]['acquired'])==0
            elif mode=='continuity':
                x=audio(p/'preserved-source.f32').reshape(8192,2);y=audio(p/'continued-source.f32').reshape(x.shape);z=audio(p/'reset-negative.f32').reshape(x.shape)
                item['max_error']=compare(x,y)
                assert np.flatnonzero(np.abs(x[:,1])>.04).tolist()==[1904]
                assert np.flatnonzero(np.abs(z[:,1])>.04).tolist()==[6000]
                assert np.abs(x-z).max()>.01
            elif mode=='source':
                x=audio(p/'unaffected-source.f32');y=audio(p/'continued-source.f32');assert x.size==16384
                item['max_error']=compare(x,y);ev=rows(p/'source-edit.tsv');assert len(ev)==1 and int(ev[0]['created'])==1 and int(ev[0]['acquired'])==1 and int(ev[0]['reused'])==6
                assert (p/'negative-controls.txt').read_text().count('rejected')==2
            elif mode=='paced':
                b=rows(p/'callbacks.tsv');events=rows(p/'events.tsv');x=audio(p/'paced-edited.f32').reshape(-1,2);y=audio(p/'paced-continued-fused.f32').reshape(x.shape)
                assert [int(r['block']) for r in b]==list(range(len(b))) and len(x)==len(b)*frames
                rev=[int(r['revision']) for r in b];assert rev==sorted(rev)
                connected=np.repeat([r>=2 and r%2==0 for r in rev],frames)
                item['max_error']=compare(x,y+connected[:,None]*marker)
                assert [int(e['revision']) for e in events]==list(range(2,18))
                for e in events:
                    assert int(e['created'])==0 and int(e['acquired'])==0 and int(e['stale_rejected'])==1
                    observed=[r for r in b if int(r['revision'])==int(e['revision'])]
                    assert observed, 'unobserved revision '+e['revision']
                    assert float(observed[0]['end_us'])>=float(e['request_us'])
                item['observed_revisions']=sorted(set(rev))
            else:raise AssertionError('unexpected mode')
            results.append(item)
        except (AssertionError,ValueError,KeyError,OSError) as e:failures.append({'name':c['name'],'error':str(e)})
        for f in p.glob('*.f32'):
            x=audio(f);samples+=x.size;key=str(f.relative_to(root));digest=hashlib.sha256(f.read_bytes()).hexdigest();assert native['raw_hashes'][key]==digest;hashes[key]=digest
    assert set(hashes)==set(native['raw_hashes'])
    report={'passed':not failures and len(results)==46,'cases':results,'failures':failures,'raw_captures':len(hashes),'float_samples':samples,'raw_hashes':hashes}
    out.mkdir(parents=True,exist_ok=True);(out/'original-regression-independent.json').write_text(json.dumps(report,indent=2)+'\n')
    print('ORIGINAL_REGRESSION_INDEPENDENT',report['passed'],len(results),'cases',len(hashes),'captures',samples,'samples','max_error',max((r['max_error'] for r in results),default=None),'failures',json.dumps(failures),flush=True)
    if not report['passed']:raise RuntimeError('original regression evidence failed')
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('root',type=Path);p.add_argument('out',type=Path);a=p.parse_args();main(a.root,a.out)
