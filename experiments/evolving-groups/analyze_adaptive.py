#!/usr/bin/env python3
"""Read every executed capture and event; no fitting and no silent exclusions."""
from pathlib import Path
import array,csv,hashlib,json,math,statistics,sys

CONFIGS=[('serial',32,4),('serial',32,8),('parallel',32,4),('parallel',32,8),('feedback',8,8),('control',16,4),('nonlinear',16,4),('serialshuffled',32,4)]
EVENTS=['external-connect','external-disconnect','expose-hidden-input','connect-exposed-input','disconnect-exposed-input','expose-hidden-output','connect-exposed-output','disconnect-exposed-output','internal-endpoint-rewire','undo-endpoint-rewire','insert-node','remove-inserted-node','member-source-change','undo-member-source','delete-member','restore-member','explicit-compact','repeat-after-compact']

def definitions():
    result=[]
    for frames in (64,128):
        result += [('trace',family,n,w,frames) for family,n,w in CONFIGS]
        result += [('state','split',8,4,frames),('state','identity',8,4,frames),('negative','serial',8,4,frames),('feedback','serial',16,4,frames),('planner','serial',32,4,frames)]
    return result

def stats(values):
    x=sorted(values)
    if not x or any(not math.isfinite(v) or v<0 for v in x):raise ValueError('bad timing')
    return dict(n=len(x),median=statistics.median(x),p95=x[math.ceil(.95*len(x))-1],maximum=x[-1])
def tsv(path):
    with path.open() as f:return list(csv.DictReader(f,delimiter='\t'))
def audio(path):
    raw=path.read_bytes()
    if not raw or len(raw)%4:raise ValueError('empty/truncated capture')
    a=array.array('f');a.frombytes(raw)
    if sys.byteorder!='little':a.byteswap()
    if any(not math.isfinite(v) for v in a):raise ValueError('nonfinite capture')
    return a

def compare(a,b):
    if not a or len(a)!=len(b):raise ValueError('unequal/empty captures')
    delta=max(abs(x-y) for x,y in zip(a,b))
    if any(not math.isfinite(x) or not math.isfinite(y) or abs(x-y)>1e-5+1e-5*abs(y) for x,y in zip(a,b)):raise ValueError('sample comparison failed')
    return delta

def verify_layout(g):
    ids={m['id'] for m in g['modules']}
    if len(ids)!=len(g['modules']):raise ValueError('duplicate id')
    members=[];owners={}
    for unit in g['groups']:
        if len(unit['members'])<2 or unit['owner']!=unit['members'][0]:raise ValueError('bad group')
        for member in unit['members']:
            if member not in ids or member in owners:raise ValueError('invalid group coverage')
            owners[member]=unit
    for e in g['edges']:
        if e['source'] not in ids or e['target'] not in ids:raise ValueError('dangling wire')
        a=owners.get(e['source']);b=owners.get(e['target'])
        if a is b:continue
        if a and not a['parallel'] and e['source']!=a['members'][-1]:raise ValueError('hidden output not split')
        if b and not b['parallel'] and e['target']!=b['members'][0]:raise ValueError('hidden input not split')

def check_manifest(cases):
    expected={f'{m}-{f}-{n}-g{w}-b{b}':(m,f,n,w,b) for m,f,n,w,b in definitions()}
    if len(cases)!=len(expected) or len({c['name'] for c in cases})!=len(expected):raise ValueError('incomplete/duplicate manifest')
    for c in cases:
        if expected.get(c['name'])!=(c['mode'],c['family'],c['size'],c['width'],c['frames']):raise ValueError('case identity mismatch')

def analyze(root):
    cases=json.loads((root/'cases.json').read_text());check_manifest(cases)
    result=dict(scope='offline real topology edits, selective state resets explicit; not GUI/Core Audio',cases=[],failures=[],hashes={})
    for case in cases:
        out=root/case['name'];item=dict(name=case['name'],mode=case['mode']);frames=case['frames']
        if case['returncode']!=0:result['failures'].append(case);continue
        try:
            for p in out.glob('*.f32'):
                audio(p);result['hashes'][str(p.relative_to(root))]=hashlib.sha256(p.read_bytes()).hexdigest()
            mode=case['mode']
            if mode=='trace':
                edits=tsv(out/'edits.tsv');blocks=tsv(out/'blocks.tsv')
                if [r['name'] for r in edits]!=EVENTS or [int(r['event']) for r in edits]!=list(range(18)):raise ValueError('event inventory')
                if [int(r['block']) for r in blocks]!=list(range(len(blocks))) or len(blocks)!=4096//frames+72:raise ValueError('block inventory')
                x=audio(out/'live-adaptive.f32');y=audio(out/'live-selective-reference.f32')
                if len(x)!=len(blocks)*frames*2:raise ValueError('live capture length')
                item['live_max_error']=compare(x,y)
                for label in ('initial','final'):
                    a=audio(out/(label+'-adaptive.f32'));b=audio(out/(label+'-product.f32'))
                    if len(a)!=4096:raise ValueError('cold oracle length')
                    item[label+'_product_error']=compare(a,b)
                initial=json.loads((out/'graph-initial.json').read_text());verify_layout(initial)
                snapshots=[json.loads((out/f'graph-{i}.json').read_text()) for i in range(18)]
                for g in snapshots:verify_layout(g)
                # Independently recompute logical program invalidation from
                # complete emitted source/identity, not native pointer counters.
                old=initial
                for r,new in zip(edits,snapshots):
                    old_units={x['index']:x for x in old['compiled_units']};expected=[]
                    for unit in new['compiled_units']:
                        if unit!=old_units.get(unit['index']):expected.append(unit['index'])
                    if len(expected)!=int(r['created']):raise ValueError('invalidation count disagrees with snapshots')
                    reset=set()
                    groups={g['owner']:g['members'] for g in new['groups']}
                    for i in expected:reset.update(groups.get(i,[i]))
                    if sorted(reset)!=json.loads(r['reset_members']):raise ValueError('reset member accounting')
                    old=new
                for i in (0,1,3,4,6,7,17):
                    if int(edits[i]['created']) or int(edits[i]['acquired']):raise ValueError('routing edit compiled')
                if int(edits[2]['created'])==0 or int(edits[17]['source_misses']):raise ValueError('split/cache check')
                if snapshots[2]['groups']!=snapshots[4]['groups']:raise ValueError('automatic merge churn after undo')
                before={(e['source'],e['target']) for e in snapshots[7]['edges']};after={(e['source'],e['target']) for e in snapshots[8]['edges']}
                if not(before-after and after-before):raise ValueError('internal endpoint edit did not change endpoints')
                item['routing_only_ms']=stats([float(edits[i]['edit_compute_us'])/1000 for i in (0,1,3,4,6,7,17)])
                item['split_ms']=stats([float(edits[i]['edit_compute_us'])/1000 for i in (2,5)])
                item['edits']=[{**r,'reset_members':json.loads(r['reset_members'])} for r in edits]
                perf=tsv(out/'throughput.tsv')
                if len(perf)!=14 or len({(r['trial'],r['backend']) for r in perf})!=14:raise ValueError('timing inventory')
                item['ns_per_frame']={k:stats([float(r['ns_per_frame']) for r in perf if r['backend']==k]) for k in ('adaptive','individual')}
            elif mode=='state':
                a=audio(out/'untouched.f32');b=audio(out/'untouched-product.f32');c=audio(out/'affected-reset.f32');d=audio(out/'affected-uninterrupted.f32')
                if any(len(x)!=16384 for x in (a,b,c,d)):raise ValueError('state lengths')
                item['untouched_error']=compare(a,b)
                def impulse(x):return max(range(8192),key=lambda i:abs(x[2*i+1]))
                item['untouched_impulse']=impulse(a);item['affected_impulse']=impulse(c);item['uninterrupted_impulse']=impulse(d)
                if (impulse(a),impulse(c),impulse(d))!=(1904,6000,1904):raise ValueError('state/reset timing')
                if max(abs(x-y) for x,y in zip(c,d))<.001:raise ValueError('reset not detectable')
            elif mode=='negative':
                rows=tsv(out/'rejections.tsv')
                if len(rows)!=6 or any(r['rejected']!='1' or r['plan_unchanged']!='1' for r in rows):raise ValueError('transaction failure')
                item['max_error']=compare(audio(out/'after-reject-adaptive.f32'),audio(out/'after-reject-reference.f32'))
            elif mode=='feedback':
                rows=tsv(out/'feedback.tsv')
                if [int(r['sample_mode']) for r in rows]!=[0,1,0,0] or any(int(r['created']) or int(r['acquired']) for r in rows):raise ValueError('feedback topology event')
                item['max_error']=compare(audio(out/'feedback-adaptive.f32'),audio(out/'feedback-reference.f32'))
            elif mode=='planner':
                rows=tsv(out/'planner.tsv')
                if len(rows)!=128 or int(rows[-1]['groups'])!=0:raise ValueError('planner refinement')
            result['cases'].append(item)
        except (ValueError,KeyError,OSError) as error:result['failures'].append(dict(name=case['name'],verification_error=str(error)))
    result['passed']=not result['failures'];result['case_count']=len(cases)
    (root/'summary.json').write_text(json.dumps(result,indent=2)+'\n');return result
if __name__=='__main__':sys.exit(0 if analyze(Path(sys.argv[1]))['passed'] else 1)
