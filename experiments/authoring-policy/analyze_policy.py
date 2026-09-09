#!/usr/bin/env python3
"""Verify complete policy traces and independently derive invalidated members."""
from __future__ import annotations
import array,csv,hashlib,json,math,random,statistics,sys
from pathlib import Path
POLICIES={'individual':1,'feedback-only':0,'cap2':2,'cap4':4,'cap8':8}
SHAPES=[('serial',32),('parallel',32),('feedback',8),('control',16),('nonlinear',16),('serialshuffled',32)]
PHASES=['initial','after-exposure','before-compact','after-compact']
NAMES=['external-connect','external-disconnect','expose-member-input','connect-member-input','disconnect-member-input','expose-member-output','connect-member-output','disconnect-member-output','endpoint-rewire','undo-endpoint-rewire','insert-node','remove-inserted-node','member-source-change','undo-member-source','delete-member','restore-member','explicit-compact','repeat-after-compact']
def definitions(shard=None):
    blocks=[]
    for f,(family,size) in enumerate(SHAPES):
        for b,frames in enumerate((64,128)):
            for variant in (0,1):blocks.append(('trace',family,size,frames,variant,(f+b+variant)%2))
    for b,frames in enumerate((64,128)):
        for variant in (0,1):blocks.append(('state','serial',8,frames,variant,(b+variant)%2))
    random.Random(20260909).shuffle(blocks);rng=random.Random(3941);out=[]
    for mode,family,size,frames,variant,s in blocks:
        policies=list(POLICIES.items());rng.shuffle(policies)
        for name,value in policies:
            if shard is not None and shard!=s:continue
            out.append(dict(name=f'{mode}-{family}-{size}-{name}-b{frames}-v{variant}',mode=mode,family=family,size=size,policy=name,policy_value=value,frames=frames,variant=variant,shard=s))
    return out

def rows(p):
    with p.open() as f:return list(csv.DictReader(f,delimiter='\t'))
def load(p):return json.loads(p.read_text())
def samples(p):
    b=p.read_bytes();a=array.array('f')
    if not b or len(b)%4:raise ValueError('empty/partial raw capture')
    a.frombytes(b)
    if sys.byteorder!='little':a.byteswap()
    if not all(math.isfinite(x) for x in a):raise ValueError('nonfinite raw capture')
    return a

def compare(a,b):
    if len(a)!=len(b) or not a:raise ValueError('capture length')
    error=0.
    for x,y in zip(a,b):
        d=abs(x-y)
        if not math.isfinite(d) or d>1e-5+1e-5*abs(y):raise ValueError('waveform mismatch')
        error=max(error,d)
    return error

def stats(v):
    v=sorted(v)
    if not v or not all(math.isfinite(x) and x>=0 for x in v):raise ValueError('invalid timing')
    return dict(n=len(v),median=statistics.median(v),p95=v[math.ceil(.95*len(v))-1],max=max(v))

def membership(g):
    groups={}
    for b in g['groups']:
        if len(b['members'])<2 or b['owner']!=b['members'][0]:raise ValueError('malformed group')
        for i in b['members']:
            if i in groups:raise ValueError('duplicate group member')
            groups[i]=b['members']
    return groups

def invalidation(before,after):
    old={m['index']:m for m in before['compiled_units']};groups=membership(after);reset=set();created=0
    for m in after['compiled_units']:
        if old.get(m['index'])!=m:created+=1;reset.update(groups.get(m['index'],[m['index']]))
    old_authored={m['id']:m['identity'] for m in before['modules']}
    survivors={m['id'] for m in after['modules'] if old_authored.get(m['id'])==m['identity']}
    return reset,reset&survivors,created

def topology(g):
    return {'modules':sorted(g['modules'],key=lambda m:m['id']), 'edges':sorted(g['edges'],key=lambda e:json.dumps(e,sort_keys=True))}

def verify_case(root,c):
    folder=root/c['name'];frames=c['frames'];item={**c};hashes={}
    paths=list(folder.glob('*.f32'))
    if len(paths)!=(6 if c['mode']=='trace' else 5):raise ValueError('unexpected capture inventory')
    for path in paths:samples(path);hashes[str(path.relative_to(root))]=hashlib.sha256(path.read_bytes()).hexdigest()
    if c['mode']=='trace':
        a=samples(folder/'live-policy.f32');b=samples(folder/'live-selective-reference.f32');br=rows(folder/'blocks.tsv')
        count=4096//frames+72
        if len(a)!=count*2*frames or len(br)!=count:raise ValueError('trace block count')
        if [int(r['block']) for r in br]!=list(range(count)):raise ValueError('trace block identity')
        events=[-1]*(4096//frames)+[e for e in range(18) for _ in range(4)]
        if [int(r['event']) for r in br]!=events:raise ValueError('missing event audio')
        expected_frames=[];at=0
        for i,e in enumerate(events):
            if i==4096//frames:at+=9*256*frames
            if i>4096//frames and i%4==0 and events[i-1] in (7,15,16):at+=9*256*frames
            expected_frames.append(at);at+=frames
        if [int(r['frame']) for r in br]!=expected_frames:raise ValueError('audio timeline mismatch')
        item['max_error']=compare(a,b);item['event_reference_peaks']=[]
        for e in range(18):
            first=(4096//frames+e*4)*frames*2;peak=max(abs(x) for x in b[first:first+4*frames*2])
            if peak<=1e-4:raise ValueError('silent event reference')
            item['event_reference_peaks'].append(peak)
        for label in ('initial','final'):
            aa=samples(folder/(label+'-adaptive.f32'));bb=samples(folder/(label+'-product.f32'))
            if len(aa)!=4096:raise ValueError('cold product oracle length')
            item[label+'_oracle_error']=compare(aa,bb)
        er=rows(folder/'edits.tsv')
        if [(int(r['event']),r['name']) for r in er]!=list(enumerate(NAMES)):raise ValueError('edit inventory')
        graphs=[load(folder/'graph-initial.json')]+[load(folder/f'graph-{k}.json') for k in range(18)]
        item['events']=[]
        for k,r in enumerate(er):
            reset,survivors,created=invalidation(graphs[k],graphs[k+1])
            if reset!=set(json.loads(r['reset_members'])) or created!=int(r['created']):raise ValueError('reset report contradicts program diff')
            expected_acquired=sum(m['index']!=c['size']+1 and m not in graphs[k]['compiled_units'] for m in graphs[k+1]['compiled_units'])
            if int(r['acquired'])!=expected_acquired:raise ValueError('factory accounting')
            if k in (0,1,3,4,6,7,17) and reset:raise ValueError('exposed edit reset')
            if c['policy']=='individual' and k<=9 and reset:raise ValueError('individual cable edit reset')
            target=(1 if c['family']=='serialshuffled' else c['size'])
            collateral=survivors-({target} if k in (12,13) else set())
            times={n:float(r[n])/1000 for n in ('mutation_us','plan_us','prepare_us','edit_compute_us')}
            stats(list(times.values()))
            if times['edit_compute_us']+1e-5<times['mutation_us']+times['plan_us']+times['prepare_us']:raise ValueError('stage sum exceeds edit interval')
            item['events'].append(dict(event=k,name=r['name'],latency_ms=times['edit_compute_us'],created=created,acquired=expected_acquired,
                reset_members=sorted(reset),reset_survivors=sorted(survivors),collateral_resets=sorted(collateral),groups=int(r['groups']),sample_mode=int(r['sample_mode'])))
        if topology(graphs[10])!=topology(graphs[8]):raise ValueError('endpoint undo does not restore graph')
        if topology(graphs[16])!=topology(graphs[14]):raise ValueError('member restore differs')
        perf=rows(folder/'throughput.tsv');expected={(p,str(t),be) for p in PHASES for t in range(9) for be in ('policy','individual-reference')}
        if len(perf)!=72 or {(r['phase'],r['trial'],r['backend']) for r in perf}!=expected:raise ValueError('CPU cell inventory')
        item['cpu']={}
        for p in PHASES:
            arms={be:stats([float(r['ns_per_frame']) for r in perf if r['phase']==p and r['backend']==be]) for be in ('policy','individual-reference')}
            pairs=[]
            for t in range(9):
                d={r['backend']:float(r['ns_per_frame']) for r in perf if r['phase']==p and int(r['trial'])==t}
                pairs.append(d['policy']/d['individual-reference'])
            item['cpu'][p]={'ns_per_frame':arms,'paired_ratio':stats(pairs),'ratio_of_medians':arms['policy']['median']/arms['individual-reference']['median']}
        item['initial']=rows(folder/'initial.tsv')[0]
        # Cross-policy equality checked by the combined audit; hash excludes groups.
        item['authored_trace_sha256']=hashlib.sha256(json.dumps([topology(g) for g in graphs],sort_keys=True).encode()).hexdigest()
    else:
        x=samples(folder/'untouched.f32');y=samples(folder/'untouched-continuous.f32');a=samples(folder/'affected.f32');b=samples(folder/'affected-continuous.f32');neg=samples(folder/'reset-negative.f32')
        if any(len(v)!=16384 for v in (x,y,a,b,neg)):raise ValueError('memory capture length')
        item['max_error']=compare(x,y)
        def impulse(v):
            inds=[i for i,x in enumerate(v[1::2]) if abs(x)>.01]
            if len(inds)!=1:raise ValueError('impulse count')
            return inds[0]
        state=load(folder/'state.json');reset,survivors,created=invalidation(load(folder/'graph-initial.json'),load(folder/'graph-0.json'))
        if set(state['reset_members'])!=reset or state['created']!=created:raise ValueError('memory reset accounting')
        expected=6000 if 5 in reset else 1904
        if impulse(x)!=1904 or impulse(y)!=1904 or impulse(b)!=1904 or impulse(neg)!=6000 or impulse(a)!=expected:raise ValueError('memory history incorrect')
        if not 5 in reset:compare(a,b)
        elif max(abs(x-y) for x,y in zip(a,b))<=1e-3:raise ValueError('unobservable reset')
        item['state']={**state,'verified_affected_impulse':expected,'reset_survivors':sorted(survivors)}
    return item,hashes

def analyze(root,shard=None):
    wanted={c['name']:c for c in definitions(shard)};cases=load(root/'cases.json');errors=[];verified=[];hashes={}
    if len(cases)!=len(wanted) or {c['name'] for c in cases}!=set(wanted):raise ValueError('case manifest missing/duplicate')
    for c in cases:
        if any(c.get(k)!=v for k,v in wanted[c['name']].items()):raise ValueError('case identity mismatch')
        if c.get('returncode')!=0:errors.append({'name':c['name'],'returncode':c.get('returncode')});continue
        try:item,h=verify_case(root,c);verified.append(item);hashes.update(h)
        except (OSError,ValueError,KeyError,ZeroDivisionError) as e:errors.append({'name':c['name'],'error':str(e)})
    groups={}
    for c in verified:
        if c['mode']=='trace':groups.setdefault((c['family'],c['frames'],c['variant']),[]).append(c)
    for key,group in groups.items():
        if len(group)!=5 or len({c['authored_trace_sha256'] for c in group})!=1:errors.append({'trace':key,'error':'policies did not receive identical authored traces'})
    result={'passed':not errors,'failures':errors,'cases':verified,'raw_hashes':hashes,'expected_count':len(wanted),'shard':shard,
        'scope':'offline warm evolving traces; separate processes; no UI/Core Audio deadline or state migration claim'}
    (root/'policy-summary.json').write_text(json.dumps(result,indent=2)+'\n');return result
if __name__=='__main__':
    result=analyze(Path(sys.argv[1]),int(sys.argv[2]) if len(sys.argv)>2 else None)
    print(json.dumps({'passed':result['passed'],'verified':len(result['cases']),'failures':result['failures']},indent=2));sys.exit(0 if result['passed'] else 1)
