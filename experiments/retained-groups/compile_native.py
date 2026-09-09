"""Compile retained units with the exact earlier upstream ocpp fusion pass.

Ordinary ocpp and -ls-fuse use the same source, Faust commit, headers and C++
flags. Both become dsp objects in the same retained routing implementation.
"""
from pathlib import Path
import hashlib,json,os,subprocess,time
COMMIT='3d4baa164c0dd31b7d147617ed84a626495148dd'

def compile_kernels(exe,root,evidence,command):
    root.mkdir(parents=True,exist_ok=True);evidence.mkdir(parents=True,exist_ok=True);records=[]
    repo=Path(__file__).resolve().parents[2]
    def checked(cmd,name,timeout,cwd=repo):
        r=command(cmd,evidence/(name+'.log'),timeout=timeout,cwd=cwd);records.append(r)
        (evidence/'commands.json').write_text(json.dumps(records,indent=2)+'\n')
        if r['returncode']:raise RuntimeError('native preparation failed: '+name)
        return r
    checked([exe,'export',root],'source-export',30)
    upstream=root/'upstream'
    if not upstream.exists():
        checked(['git','clone','--filter=blob:none','--no-checkout','https://github.com/grame-cncm/faust.git',upstream],'upstream-clone',240)
    checked(['git','-C',upstream,'fetch','--depth','1','origin',COMMIT],'upstream-fetch',240)
    checked(['git','-C',upstream,'checkout','--detach',COMMIT],'upstream-checkout',30)
    actual=subprocess.check_output(['git','-C',upstream,'rev-parse','HEAD'],text=True).strip()
    if actual!=COMMIT:raise RuntimeError('wrong upstream source')
    checked(['cmake','-S',upstream/'build','-B',upstream/'build/retained-groups',
             '-C',upstream/'build/backends/light.cmake','-C',upstream/'build/targets/regular.cmake',
             '-DCMAKE_BUILD_TYPE=Release','-DCMAKE_CXX_FLAGS_RELEASE=-O0 -DNDEBUG',
             '-DINCLUDE_OSC=OFF','-DINCLUDE_HTTP=OFF','-DINCLUDE_EMCC=OFF','-DINCLUDE_WASM_GLUE=OFF'],'upstream-configure',180)
    checked(['cmake','--build',upstream/'build/retained-groups','--target','faust','--parallel','3'],'upstream-build',1200)
    compiler=upstream/'build/bin/faust';checked([compiler,'-v'],'upstream-version',15)
    prefix=Path(subprocess.check_output(['brew','--prefix','faust'],text=True).strip());include=prefix/'include';libs=prefix/'share/faust'
    kernels=[]
    for source in sorted((root/'sources').glob('*.dsp')):
        key=source.stem;data=source.read_bytes();hash_=hashlib.sha256(data).hexdigest();entry={'key':key,'source_sha256':hash_,'variants':{}}
        for variant,opts in [('ocpp',[]),('ls-fuse',['-ls-fuse','-ls-sched','cs2'])]:
            folder=root/variant/key;folder.mkdir(parents=True,exist_ok=True);local=folder/'unit.dsp';local.write_bytes(data)
            klass='Unit_'+key;generated=folder/'unit.hpp'
            r=checked([compiler,'-lang','ocpp','-cn',klass,'-I',libs,*opts,'-sng',local,'-o',generated],key+'-'+variant+'-faust',150,cwd=folder)
            text=generated.read_text()
            if variant=='ls-fuse' and '-ls-fuse' not in text:raise RuntimeError('fusion option missing from generated identity')
            cpp=folder/'unit.cpp'
            cpp.write_text('#include <algorithm>\n#include <cmath>\n#include <cstdint>\n#include <faust/dsp/dsp.h>\n#include <faust/gui/UI.h>\n#include <faust/gui/meta.h>\n#include "unit.hpp"\nextern "C" __attribute__((visibility("default"))) dsp* stable_group_create(){return new '+klass+'();}\n')
            library=root/variant/(key+'.dylib')
            c=checked(['clang++','-std=c++17','-O3','-DNDEBUG','-ffp-contract=off','-fvisibility=hidden','-I'+str(include),'-dynamiclib',cpp,'-o',library],key+'-'+variant+'-cpp',120,cwd=folder)
            entry['variants'][variant]={'faust_seconds':r['elapsed_seconds_not_edit_latency'],'cpp_seconds':c['elapsed_seconds_not_edit_latency'],
                 'generated_sha256':hashlib.sha256(generated.read_bytes()).hexdigest(),'library_sha256':hashlib.sha256(library.read_bytes()).hexdigest()}
            for dot in folder.glob('*-sn.dot'):
                (evidence/(key+'-'+variant+'-sn.dot')).write_bytes(dot.read_bytes())
                entry['variants'][variant]['compiler_loop_clusters']=dot.read_text().count('subgraph cluster_')
        kernels.append(entry);(evidence/'kernels.json').write_text(json.dumps({'upstream_commit':COMMIT,'compiler_executable_sha256':hashlib.sha256(compiler.read_bytes()).hexdigest(),
            'cpp_flags':['-O3','-DNDEBUG','-ffp-contract=off','-fvisibility=hidden'],'kernels':kernels},indent=2)+'\n')
        print('compiled native kernel',key,flush=True)
