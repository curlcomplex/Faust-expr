#!/usr/bin/env python3
"""Full frozen-data native benchmark; offline VM timings, not a DAW/device pass."""
from __future__ import annotations
import argparse, hashlib, importlib.util, json, os, platform, shutil, struct, subprocess, sys, tarfile
from pathlib import Path
import numpy as np
HERE=Path(__file__).resolve().parent
NATIVE_SHA='ce05ec79603e11fd8a85902a89b4b5920141352fc662f7413d42de7c3d49ed83'
EXPECTED_FILES={'native/scene_api.cpp','native/scene_api.h','native/stream_population.hpp','native/parallel_pool.hpp','native/engine_stream.cpp','baseline/execution-candidate/pole_batch.hpp','baseline/cymbal-interactions-18/generated/coefficients.hpp','host/native_harness.cpp','host/prepared_player.hpp','host/test_worker_hooks.cpp','host/build_host.py','host/run_host.py'}
def digest(b): return hashlib.sha256(b).hexdigest()
def prepared_file(path, model, name):
    low,high,knots=(model[k] for k in ('low','high','knots')); et,ew=(model[k] for k in ('envelope_time','envelope_weight'))
    base=[0,0,0,1,1,1.2,1,1,1,20,1,0,998,.001002,0,0,8,5,1,0,1,1,1.]; controls=[base]
    if name=='hard': hits=[.05];seconds=2.4;events=[]
    elif name=='overlap': hits=[.05,.35,.70];seconds=2.4;events=[]
    elif name=='controls':
        hits=[.05,.65];seconds=2.4
        felt=base.copy();felt[14]=1;felt[15]=.25;felt[11]=.15
        gold=base.copy();gold[0]=gold[1]=1
        controls.extend([felt,gold,base]);events=[(int(.40*48000),0,1),(int(1.05*48000),0,2),(int(1.70*48000),0,3)]
    else: raise ValueError('Unknown case')
    events=[(0,0,0)]+events+[(round(t*48000),1,0) for t in hits];events.sort(key=lambda e:e[0])
    with path.open('wb') as f:
        f.write(b'CYM21PK\0');f.write(struct.pack('<12I',1,48000,len(low),len(high),knots.shape[1],1296,len(et),len(controls),1,len(events),round(seconds*48000),8))
        def put(a): f.write(np.ascontiguousarray(a,dtype='<f8').tobytes())
        for a in (low,high,knots,et,ew): put(a)
        for a in controls: put(a)
        put([1.]);put(low[:,15:19]);put(high[:,15:19])
        for e in events:f.write(struct.pack('<3I',*e))
    return {'name':name,'seconds':seconds,'hard_strikes_s':hits,'snapshots':len(controls),'fixture_sha256':digest(path.read_bytes())}
def main():
    p=argparse.ArgumentParser();p.add_argument('--output',type=Path,default=Path('build/mac-full-audit'));p.add_argument('--allow-linux',action='store_true');p.add_argument('--quick',action='store_true');args=p.parse_args();out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
    mac=sys.platform=='darwin' and platform.machine()=='arm64'
    if not mac and not args.allow_linux: raise RuntimeError('This CI task requires an Apple Silicon runner')
    spec=importlib.util.spec_from_file_location('frozen_decoder',HERE/'checkpoint21-model-decode.py');decoder=importlib.util.module_from_spec(spec);spec.loader.exec_module(decoder)
    model,identity=decoder.decode(HERE/'checkpoint21-model-capsule.tar.xz');(out/'model-identity.json').write_text(json.dumps(identity,indent=2)+'\n')
    packet=HERE/'checkpoint21-native-core.tar.xz'
    if digest(packet.read_bytes())!=NATIVE_SHA: raise RuntimeError('Frozen native source changed')
    root=out/'source';root.mkdir(exist_ok=True)
    with tarfile.open(packet,'r:xz') as tar:
        entries=tar.getmembers()
        if {e.name for e in entries}!=EXPECTED_FILES or len(entries)!=12: raise RuntimeError('Unexpected native source archive')
        for e in entries:
            if not e.isfile() or not (root/e.name).resolve().is_relative_to(root): raise RuntimeError('Unsafe source archive')
            dest=root/e.name;dest.parent.mkdir(parents=True,exist_ok=True);dest.write_bytes(tar.extractfile(e).read())
    source_hashes={str(f.relative_to(root)):digest(f.read_bytes()) for f in root.rglob('*') if f.is_file()};(out/'source-hashes.json').write_text(json.dumps(source_hashes,indent=2)+'\n')
    commands=[]
    def run(cmd,label,timeout=170):
        commands.append(cmd);(out/'commands.json').write_text(json.dumps(commands,indent=2)+'\n');env=os.environ.copy();env['OPENBLAS_NUM_THREADS']='1';env['OMP_NUM_THREADS']='1';r=subprocess.run(cmd,capture_output=True,text=True,timeout=timeout,env=env);(out/(label+'.stdout.txt')).write_text(r.stdout);(out/(label+'.stderr.txt')).write_text(r.stderr)
        if r.returncode: raise RuntimeError(label+': '+r.stderr)
        return r
    cxx=['xcrun','clang++'] if mac else [shutil.which('g++') or 'g++'];flags=['-std=c++20','-O3','-ffp-contract=off','-pthread']+(['-mmacosx-version-min=12.0'] if mac else []);inc=[]
    for rel in ('native','baseline/execution-candidate','baseline/cymbal-interactions-18/generated'): inc+=['-I',str(root/rel)]
    lib=out/('scene.dylib' if mac else 'scene.so');run(cxx+flags+inc+['-fPIC','-dynamiclib' if mac else '-shared',str(root/'native/scene_api.cpp'),'-o',str(lib)],'compile-native')
    host=out/'native_harness';frameworks=['-framework','AudioToolbox','-framework','CoreAudio'] if mac else [];run(cxx+flags+inc+[str(root/'host/native_harness.cpp'),str(lib)]+frameworks+['-o',str(host)],'compile-host')
    hooks=out/'test_worker_hooks';run(cxx+flags+inc+[str(root/'host/test_worker_hooks.cpp'),str(lib)]+frameworks+['-o',str(hooks)],'compile-hooks')
    results=[];comparisons=[];cases=['hard'] if args.quick else ['hard','overlap','controls']
    for name in cases:
        case=out/(name+'.bin');metadata=prepared_file(case,model,name);sequence=[(1,128),(3,128)] if args.quick else [(1,128),(3,128),(3,128),(1,128),(3,257)];reference=None
        for i,(workers,block) in enumerate(sequence):
            label=f'{name}-r{i}-w{workers}-b{block}';prefix=out/label;run([str(host),str(case),str(prefix),'offline',str(workers),str(block),'2'],label);raw=prefix.with_suffix('.f64').read_bytes();x=np.frombuffer(raw,dtype='<f8')
            if len(x)!=round(metadata['seconds']*48000)*2 or not np.isfinite(x).all() or not np.any(x): raise RuntimeError('Invalid full audio result')
            if reference is None: reference=raw
            else:
                exact=raw==reference;comparisons.append({'case':name,'workers':workers,'block':block,'byte_exact':exact,'sha256':digest(raw)})
                if not exact: raise RuntimeError('Full-model worker/block preservation failed')
            results.append({'case':metadata,'workers':workers,'block':block,'repeat':i,'measurements':json.loads(prefix.with_suffix('.json').read_text()),'audio_sha256':digest(raw)})
    run([str(hooks),str(out/'hard.bin')],'worker-hooks')
    report={'platform':platform.platform(),'architecture':platform.machine(),'numpy':np.__version__,'actual_apple_silicon_run':mac,'frozen_native_source_sha256':NATIVE_SHA,'full_frozen_model_loaded':True,'model_identity':identity,'cases':results,'exact_comparisons':comparisons,'worker_hooks_passed':True,'audio_device_opened':False,'realtime_release_claim':False,'scope':'Full frozen 7,260-component workload using unchanged checkpoint21 native core. Hosted-VM offline callbacks, not an audio-device/DAW test.'};(out/'result.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
if __name__=='__main__': main()
