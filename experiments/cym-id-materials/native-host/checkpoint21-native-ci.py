#!/usr/bin/env python3
"""Compile unmodified checkpoint21 native source; exercise a synthetic fixture.
NOT the complete 7,260-component instrument and NOT a real-time benchmark.
"""
from pathlib import Path
import array, hashlib, json, math, platform, struct, subprocess, sys, tarfile
HERE=Path(__file__).resolve().parent
PACKET=HERE/'checkpoint21-native-core.tar.xz'
EXPECTED='ce05ec79603e11fd8a85902a89b4b5920141352fc662f7413d42de7c3d49ed83'
OUT=Path('build/mac-native-audit').resolve();OUT.mkdir(parents=True,exist_ok=True)
ROOT=OUT/'source';ROOT.mkdir(exist_ok=True)
if hashlib.sha256(PACKET.read_bytes()).hexdigest()!=EXPECTED:raise RuntimeError('Native source packet hash mismatch')
with tarfile.open(PACKET,'r:xz') as a:
    for m in a.getmembers():
        if not (ROOT/m.name).resolve().is_relative_to(ROOT) or not m.isfile():raise RuntimeError('Unsafe archive entry')
    a.extractall(ROOT,filter='data')
manifest={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in ROOT.rglob('*') if p.is_file()}
(OUT/'source-hashes.json').write_text(json.dumps(manifest,indent=2))
commands=[]
def run(cmd,label,timeout=90):
    commands.append(cmd)
    p=subprocess.run(cmd,capture_output=True,text=True,timeout=timeout)
    (OUT/(label+'.stdout.txt')).write_text(p.stdout);(OUT/(label+'.stderr.txt')).write_text(p.stderr)
    (OUT/'commands.json').write_text(json.dumps(commands,indent=2))
    if p.returncode:raise RuntimeError(label+': '+p.stderr)
    return p
if sys.platform!='darwin' or platform.machine()!='arm64':raise RuntimeError('Apple Silicon job required')
cxx=['xcrun','clang++']
inc=['-I',str(ROOT/'native'),'-I',str(ROOT/'baseline/execution-candidate'),'-I',str(ROOT/'baseline/cymbal-interactions-18/generated')]
flags=['-std=c++20','-O3','-ffp-contract=off','-pthread','-mmacosx-version-min=12.0']
lib=OUT/'scene.dylib'
run(cxx+flags+inc+['-fPIC','-dynamiclib',str(ROOT/'native/scene_api.cpp'),'-o',str(lib)],'compile-native')
host=OUT/'native_harness'
run(cxx+flags+inc+[str(ROOT/'host/native_harness.cpp'),str(lib),'-framework','AudioToolbox','-framework','CoreAudio','-o',str(host)],'compile-host')
hooks=OUT/'test_worker_hooks'
run(cxx+flags+inc+[str(ROOT/'host/test_worker_hooks.cpp'),str(lib),'-o',str(hooks)],'compile-worker-hooks')
# Explicitly synthetic coefficients: test scheduling, delayed activation and
# shared controls. This fixture is not a reduced or replacement cymbal.
nl,nh,nk,ks=8,8,12,1296
low=[];high=[]
for i in range(8):
    shape=[.25,.15,.15,.15,.15,.15,.7];wet=[0,.2,.5,.8,1,.3]
    force=[.001*(i+1),-.0003*(i+1),.0008*(i+1),.0001*(i+1)]
    low.append([210.+i*371.,2.+i*.25]+shape+wet+force+[0.,0.])
    high.append([1700.+i*837.,4.+i*.7]+shape+wet+force+[.015+i*.005,1. if i<4 else 2.])
knots=[[.004*math.sin(i*.7+j*.4) if i<4 else 0. for j in range(nk)] for i in range(nh)]
ctrl=[0,0,0,1,1,1.2,1,1,1,20,1,0,998,.001002,0,0,8,5,1,0,1,1,1.]
touch=ctrl.copy();touch[14]=1;touch[15]=.35;touch[11]=.2
snapshots=[ctrl,touch,ctrl]
events=[(0,0,0),(1200,1,0),(3600,1,1),(6000,0,1),(10000,0,2)]
case=OUT/'synthetic-fixture.bin'
with case.open('wb') as f:
    f.write(b'CYM21PK\0');f.write(struct.pack('<12I',1,48000,nl,nh,nk,ks,3,len(snapshots),2,len(events),19200,8))
    def put(x):
        vals=array.array('d',x)
        if sys.byteorder!='little':vals.byteswap()
        f.write(vals.tobytes())
    for a in [low,high,knots]:put(v for row in a for v in row)
    put([0.,.1,1.]);put([.3,1.,.4])
    for s in snapshots:put(s)
    for scale in [1.,.7]:
        put([.85]);put(v*scale for row in low for v in row[15:19]);put(v*scale for row in high for v in row[15:19])
    for e in events:f.write(struct.pack('<3I',*e))
runs=[];reference=None
for workers,block in [(1,128),(3,128),(3,257)]:
    prefix=OUT/f'fixture-w{workers}-b{block}'
    run([str(host),str(case),str(prefix),'offline',str(workers),str(block),'2'],prefix.name)
    raw=prefix.with_suffix('.f64').read_bytes();samples=array.array('d');samples.frombytes(raw)
    if not samples or not all(math.isfinite(v) for v in samples) or max(map(abs,samples))==0:raise RuntimeError('Invalid/silent fixture')
    if reference is None:reference=raw
    elif raw!=reference:raise RuntimeError('Block/worker exactness failure')
    runs.append({'workers':workers,'block':block,'byte_exact':True,'sha256':hashlib.sha256(raw).hexdigest()})
run([str(hooks),str(case)],'worker-hooks')
(OUT/'result.json').write_text(json.dumps({
 'native_core_compiled':True,'core_audio_adapter_compiled':True,'unmodified_checkpoint21_source_files':len(manifest),
 'synthetic_fixture':{'ordinary_components':nl,'high_components':nh,'cases':runs},'worker_lifecycle_test_passed':True,
 'full_model_data_loaded':False,'full_cymbal_rendered':False,'audio_device_opened':False,'real_time_performance_claim':False,
 'scope':'Actual preserved native source and a 16-component synthetic correctness fixture; NOT the full cymbal, not a reduced candidate instrument, not a throughput benchmark.'},indent=2))
print((OUT/'result.json').read_text())
