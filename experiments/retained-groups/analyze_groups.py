#!/usr/bin/env python3
"""Re-read all group audio/timing evidence; no fitting or post-hoc tolerances."""
from pathlib import Path
import array,csv,hashlib,json,math,statistics,sys
CONFIGS=[('serial',32,4),('serial',32,8),('parallel',32,4),('parallel',32,8),('feedback',8,8),('control',16,4),('nonlinear',16,4)]
def definitions():
    out=[]
    for frames in (64,128):
        out += [('matrix',f,n,w,frames) for f,n,w in CONFIGS]
        out += [('state','serial',8,4,frames),('partition','serial',32,8,frames),('partition','feedback',8,8,frames)]
    return out

def tsv(path):
    with path.open() as f:return list(csv.DictReader(f,delimiter='\t'))
def audio(path):
    data=path.read_bytes()
    if not data or len(data)%4:raise ValueError('invalid float capture length')
    a=array.array('f');a.frombytes(data)
    if sys.byteorder!='little':a.byteswap()
    if not all(math.isfinite(x) for x in a):raise ValueError('nonfinite capture')
    return a

def compare(a,b):
    if len(a)!=len(b) or not a:raise ValueError('comparison length')
    if any(not math.isfinite(x) or not math.isfinite(y) or abs(x-y)>1e-5+1e-5*abs(y) for x,y in zip(a,b)):
        raise ValueError('audio comparison failed')
    return max(abs(x-y) for x,y in zip(a,b))
def stats(xs):
    xs=sorted(xs)
    if not xs or any(not math.isfinite(x) or x<0 for x in xs):raise ValueError('invalid timing')
    return dict(n=len(xs),median=statistics.median(xs),p95_nearest_rank=xs[math.ceil(.95*len(xs))-1],max=max(xs))
def impulse(x):
    p=[i for i,v in enumerate(x[1::2]) if abs(v)>1e-6]
    if len(p)!=1:raise ValueError('expected one group-local pending impulse')
    return p[0]
def validate_inventory(cases):
    expected={f'{m}-{f}-{n}-g{w}-b{b}':(m,f,n,w,b) for m,f,n,w,b in definitions()}
    if len(cases)!=len(expected) or len({r['name'] for r in cases})!=len(expected):raise ValueError('case count or duplicates')
    for r in cases:
        if expected.get(r['name'])!=(r['mode'],r['family'],r['size'],r['width'],r['frames']):raise ValueError('case identity')

def analyze(root,phase):
    cases=json.loads((root/'cases.json').read_text());validate_inventory(cases)
    backends=['modules','group-jit']+(['ocpp','ls-fuse'] if phase=='aot' else [])
    result={'phase':phase,'expected_native_processes':len(cases),'cases':[],'failures':[],'raw_hashes':{},
            'scope':'fixed retained groups, offline native compute; no product/device/multicore acceptance'}
    for c in cases:
        folder=root/c['name'];frames=c['frames'];r={k:c[k] for k in ('name','mode','family','size','width','frames')}
        try:
            for path in folder.glob('*.f32'):
                audio(path);result['raw_hashes'][str(path.relative_to(root))]=hashlib.sha256(path.read_bytes()).hexdigest()
            if c['returncode']!=0:raise ValueError('native case failed; log retained')
            if c['mode']=='matrix':
                expected_length=(4096+32*4*frames)*2
                ref=audio(folder/'fused-oracle.f32')
                if len(ref)!=expected_length:raise ValueError('matrix length')
                dry=audio(folder/'unpatched-negative.f32')
                if len(dry)!=len(ref):raise ValueError('negative control length')
                r['edit_effect_peak']=max(abs(x-y) for x,y in zip(ref,dry))
                if r['edit_effect_peak']<=1e-4:raise ValueError('insufficient connection effect; false-positive risk')
                blocks=tsv(folder/'blocks.tsv');edits=tsv(folder/'edits.tsv');perf=tsv(folder/'throughput.tsv');initial=tsv(folder/'initial.tsv')
                expected_blocks=4096//frames+128
                if [int(b['block']) for b in blocks]!=list(range(expected_blocks)):raise ValueError('block inventory')
                for b in blocks:
                    k=int(b['block'])-4096//frames;on=k>=0 and (k//4)%2==0
                    if int(b['connected'])!=int(on):raise ValueError('wrong wire schedule')
                if len(edits)!=32*len(backends) or {(int(e['trial']),e['backend']) for e in edits}!={(i,b) for i in range(32) for b in backends}:raise ValueError('edit inventory')
                if len(perf)!=9*len(backends) or {(int(e['trial']),e['backend']) for e in perf}!={(i,b) for i in range(9) for b in backends}:raise ValueError('throughput inventory')
                r['backends']={}
                for b in backends:
                    err=compare(audio(folder/(b+'.f32')),ref);rows=[e for e in edits if e['backend']==b]
                    count=c['size']+3 if b=='modules' else c['size']//c['width']+3
                    if any(int(e['created']) or int(e['acquired']) or int(e['reused'])!=count or int(e['connected'])!=int(int(e['trial'])%2==0) for e in rows):raise ValueError('instance/cache retention failure')
                    i=next(e for e in initial if e['backend']==b)
                    if int(i['instances'])!=count:raise ValueError('cold instance count')
                    sample_mode=int(i['external_feedback_sample_mode'])
                    if sample_mode!=int(c['family']=='feedback' and b=='modules'):raise ValueError('feedback execution boundary')
                    r['backends'][b]={'max_audio_error':err,'prepare_us':stats([float(e['group_and_routing_prepare_us']) for e in rows]),
                        'prepare_and_compute_us':stats([float(e['prepare_and_first_compute_us']) for e in rows]),
                        'ns_per_frame':stats([float(e['ns_per_frame']) for e in perf if e['backend']==b]),'initial_prepare_us':float(i['prepare_us'])}
                r['group_jit_time_over_modules']=r['backends']['group-jit']['ns_per_frame']['median']/r['backends']['modules']['ns_per_frame']['median']
                if phase=='aot':r['fusion_time_over_plain_ocpp']=r['backends']['ls-fuse']['ns_per_frame']['median']/r['backends']['ocpp']['ns_per_frame']['median']
                if c['family']!='parallel' and (folder/'boundary-negative.txt').read_text()!='hidden-input: rejected\n':raise ValueError('missing hidden-port rejection')
            elif c['mode']=='state':
                rows=tsv(folder/'state.tsv');bs=backends[1:];events=['external','internal-wire','source-code']
                if len(rows)!=3*len(bs) or {(e['event'],e['backend']) for e in rows}!={(e,b) for e in events for b in bs}:raise ValueError('state event inventory')
                r['events']=[]
                for event in events:
                    reference=audio(folder/(event+'-oracle.f32'))
                    if len(reference)!=16384 or impulse(reference)!=1904:raise ValueError('independent impulse reference')
                    for b in bs:
                        x=audio(folder/(event+'-'+b+'.f32'));negative=audio(folder/(event+'-reset-'+b+'.f32'))
                        err=compare(x,reference)
                        if impulse(x)!=1904 or impulse(negative)!=6000 or max(abs(a-v) for a,v in zip(x,negative))<=.001:raise ValueError('group memory not preserved/reset not detected')
                        row=next(e for e in rows if e['event']==event and e['backend']==b)
                        created=int(event!='external')
                        if int(row['created'])!=created or int(row['acquired'])!=created or int(row['reused'])!=5-created or int(row['unchanged_group_same_object'])!=1:raise ValueError('wrong group invalidation')
                        r['events'].append({'event':event,'backend':b,'prepare_us':float(row['prepare_us']),'max_audio_error':err,'preserved_impulse':1904,'reset_impulse':6000})
            elif c['mode']=='partition':
                r['partition_bit_exact']={}
                for b in backends[1:]:
                    x=audio(folder/(b+'-fixed.f32'));y=audio(folder/(b+'-irregular.f32'))
                    if len(x)!=16384 or x!=y:raise ValueError('block-partition invariance failed: '+b)
                    r['partition_bit_exact'][b]=True
            result['cases'].append(r)
        except (ValueError,KeyError,OSError,StopIteration) as err:result['failures'].append({'name':c['name'],'error':str(err),'native_returncode':c['returncode']})
    result['passed']=not result['failures']
    (root/'summary.json').write_text(json.dumps(result,indent=2)+'\n')
    return result
if __name__=='__main__':sys.exit(0 if analyze(Path(sys.argv[1]),sys.argv[2])['passed'] else 1)
