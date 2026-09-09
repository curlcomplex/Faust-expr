#!/usr/bin/env python3
"""Re-read native evidence without optional Python dependencies or gain fitting."""
from __future__ import annotations
import array, csv, hashlib, json, math, statistics, sys
from pathlib import Path


def read_tsv(path: Path) -> list[dict]:
    with path.open() as f:
        return list(csv.DictReader(f, delimiter='\t'))


def samples(path: Path) -> list[float]:
    b=path.read_bytes()
    if len(b)%4: raise ValueError(f'partial float: {path.name}')
    a=array.array('f'); a.frombytes(b)
    if sys.byteorder!='little': a.byteswap()
    if not a or not all(math.isfinite(v) for v in a):
        raise ValueError(f'empty/nonfinite audio: {path.name}')
    return a.tolist()


def maximum_error(a, b):
    if len(a)!=len(b): raise ValueError('audio length mismatch')
    return max(abs(x-y) for x,y in zip(a,b))


def stats(xs):
    xs=sorted(xs)
    if not xs or not all(math.isfinite(x) for x in xs):
        raise ValueError('empty/nonfinite metric')
    return {'n':len(xs),'median':statistics.median(xs),
            'p95_nearest_rank':xs[math.ceil(.95*len(xs))-1], 'max':max(xs)}


def definitions():
    out=[]
    for frames in (64,128):
        for family,size in [('serial',4),('serial',16),('serial',32),('parallel',4),
                            ('parallel',16),('parallel',32),('feedback',8),('control',16)]:
            out.append(('matrix',family,size,frames))
        out.extend([('continuity','serial',4,frames),('parameter','serial',0,frames)])
        for family in ('serial','parallel'):
            out.append(('paced',family,32,frames))
    return out


def validate_cases(cases):
    expected={f'{m}-{f}-{n}-f{b}':(m,f,n,b) for m,f,n,b in definitions()}
    if len(cases)!=len(expected) or len({c['name'] for c in cases})!=len(expected):
        raise ValueError('missing or duplicate benchmark cases')
    for c in cases:
        actual=(c['mode'],c['family'],c['size'],c['frames'])
        if c['name'] not in expected or actual!=expected[c['name']]:
            raise ValueError('unexpected case identity')


def analyze(root: Path) -> dict:
    result={'schema':1, 'scope':'headless actual product renderer; not GUI or Core Audio',
            'matrix':[], 'paced':[], 'continuity':[], 'parameter':[], 'raw_hashes':{}}
    cases=json.loads((root/'cases.json').read_text())
    validate_cases(cases)
    failed=[c for c in cases if c.get('returncode')!=0]
    result['failed_cases']=failed
    for case in cases:
        folder=root/case['name']; frames=case['frames']; mode=case['mode']
        for p in folder.glob('*.f32'):
            samples(p)
            result['raw_hashes'][str(p.relative_to(root))]=hashlib.sha256(p.read_bytes()).hexdigest()
        if case.get('returncode')!=0: continue
        if mode=='matrix':
            rows=read_tsv(folder/'edits.tsv')
            if [int(r['trial']) for r in rows]!=list(range(-1,10)):
                raise ValueError('missing or reordered edit rows')
            a=samples(folder/'first--1.f32'); b=samples(folder/'first-0.f32')
            if len(a)!=2*frames or len(b)!=2*frames: raise ValueError('first-block length mismatch')
            expected=[x+(0.01 if i%2 else 0.02)*math.sqrt(.5) for i,x in enumerate(a)]
            if maximum_error(b,expected)>1e-5: raise ValueError('independent marker oracle failed')
            for k in range(-1,10):
                actual=samples(folder/f'first-{k}.f32')
                reference=b if k>=0 and k%2==0 else a
                if maximum_error(actual,reference)>1e-5: raise ValueError('revisit waveform mismatch')
            for phase in dict.fromkeys(r['phase'] for r in rows):
                group=[r for r in rows if r['phase']==phase]
                metrics={key:stats([float(r[key])/1000 for r in group])
                         for key in ('mutation_us','prepare_wall_us','graph_plan_us','factory_us',
                                     'instance_init_us','prepare_unattributed_us',
                                     'edit_to_first_render_complete_us','first_render_us')}
                result['matrix'].append({'name':case['name'],'family':case['family'],
                    'size':case['size'],'frames':frames,'phase':phase,'metrics_ms':metrics})
        elif mode=='continuity':
            fresh=samples(folder/'fresh-anchor.f32'); continued=samples(folder/'continued-source.f32')
            replacement=samples(folder/'replaced-source.f32')
            if len(fresh)!=8192*2 or len(continued)!=len(fresh) or len(replacement)!=len(fresh):
                raise ValueError('continuity capture length')
            def impulse(x):
                positions=[i for i,v in enumerate(x[1::2]) if abs(v)>.04]
                if len(positions)!=1: raise ValueError('expected one retained delay impulse')
                return positions[0]
            observed={'continued_impulse_frame':impulse(continued),
                      'replacement_impulse_frame':impulse(replacement),
                      'fresh_impulse_frame':impulse(fresh)}
            observed.update(replacement_vs_fresh_max_error=maximum_error(replacement,fresh),
                            replacement_vs_continued_max_error=maximum_error(replacement,continued))
            observed['live_state_preserved']=maximum_error(replacement,continued)<=1e-5
            observed['restart_demonstrated']=(observed['continued_impulse_frame']==1904 and
                observed['replacement_impulse_frame']==6000 and maximum_error(replacement,fresh)<=1e-5)
            result['continuity'].append({'name':case['name'],**observed})
        elif mode=='parameter':
            rows=read_tsv(folder/'parameter.tsv')
            if [int(r['trial']) for r in rows]!=list(range(20)):
                raise ValueError('missing or reordered parameter rows')
            for r in rows:
                x=samples(folder/f"parameter-{r['trial']}.f32")
                target=float(r['value'])*math.sqrt(.5)
                if len(x)!=2*frames or max(abs(v-target) for v in x)>1e-6:
                    raise ValueError('parameter audio does not match value')
            result['parameter'].append({'name':case['name'],'metrics_ms':stats([
                float(r['row_update_to_first_render_complete_us'])/1000 for r in rows])})
        elif mode=='paced':
            timing_rows=read_tsv(folder/'paced.tsv')
            if len(timing_rows)!=1: raise ValueError('paced timing count')
            r=timing_rows[0]; t={k:float(v) for k,v in r.items()}
            a=samples(folder/'initial-anchor.f32'); b=samples(folder/'first-published.f32')
            expected=[v+(0.01 if i%2 else .02)*math.sqrt(.5) for i,v in enumerate(a)]
            if len(a)!=2*frames or maximum_error(b,expected)>1e-5: raise ValueError('paced marker failure')
            callbacks=read_tsv(folder/'callbacks.tsv'); byphase={}
            period=frames*1e6/48000
            for phase in ('before_build','during_build','after_build'):
                entries=[]
                for item in callbacks:
                    start=float(item['compute_start_us']); end=float(item['compute_end_us'])
                    thisphase=('before_build' if start<t['edit_us'] else
                               'during_build' if start<t['prepared_us'] else 'after_build')
                    if thisphase==phase: entries.append(item)
                if not entries: continue
                byphase[phase]={'callback_count':len(entries),
                    'compute_us':stats([float(x['compute_end_us'])-float(x['compute_start_us']) for x in entries]),
                    'wake_lateness_us':stats([max(0,float(x['compute_start_us'])-float(x['scheduled_us'])) for x in entries]),
                    'missed_synthetic_schedule_deadlines':sum(float(x['compute_end_us'])>float(x['scheduled_us'])+period for x in entries)}
            result['paced'].append({'name':case['name'],'frames':frames,
                'build_ms':(t['prepared_us']-t['mutation_end_us'])/1000,
                'edit_to_changed_compute_complete_ms':(t['first_changed_compute_complete_us']-t['edit_us'])/1000,
                'publication_to_changed_compute_complete_ms':(t['first_changed_compute_complete_us']-t['publication_us'])/1000,
                'callbacks':byphase})
    result['executed_cases']=len(cases)
    result['numeric_evidence_valid']=not failed
    result['full_interactive_patching_acceptance']=False
    result['unmeasured']=['GUI dispatch/acknowledgement','EngineSlot publication and reclamation',
        'Core Audio delivery/device latency and real xruns','incremental compiler-region reuse',
        'polyphonic and full-song graphs','stale asynchronous-build suppression']
    (root/'summary.json').write_text(json.dumps(result,indent=2)+'\n')
    return result

if __name__=='__main__':
    summary=analyze(Path(sys.argv[1]))
    print(json.dumps({k:summary[k] for k in ('executed_cases','numeric_evidence_valid','full_interactive_patching_acceptance')},indent=2))
    sys.exit(0 if summary['numeric_evidence_valid'] else 1)
