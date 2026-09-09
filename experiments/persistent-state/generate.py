#!/usr/bin/env python3
"""Generate persistent-state code from real exported GraphState modules.
No parsing, guessing or copying of generated fields. Whole optimized graphs
statically inline Faust's sample functions while retaining exact module objects.
The comparison retains a stock whole-Faust LLVM baseline: performance is a gate.
"""
from __future__ import annotations
import hashlib,json,os,re,subprocess,time
from pathlib import Path

FLAGS=['-std=c++17','-O3','-DNDEBUG','-ffast-math','-ffp-contract=off','-fvisibility=hidden']

def sha(b):return hashlib.sha256(b if isinstance(b,bytes) else b.encode()).hexdigest()
def number(x):return format(float(x),'.9e')+'f'
def command(cmd,log,timeout=120):
    start=time.monotonic()
    try:
        r=subprocess.run([str(x) for x in cmd],stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,timeout=timeout)
        log.write_text(r.stdout);code=r.returncode
    except subprocess.TimeoutExpired as e:log.write_text(str(e));code=124
    row=dict(command=[str(x) for x in cmd],seconds=time.monotonic()-start,returncode=code)
    if code:raise RuntimeError(str(log)+': '+str(code))
    return row

PREAMBLE='''#include <faust/dsp/dsp.h>
#include <faust/gui/MapUI.h>
#include <faust/gui/meta.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <new>
#define API extern "C" __attribute__((visibility("default")))
'''

def module_sources(manifest,root,records):
    modules=manifest['modules']
    for m in modules:
        code=m['source']
        for p in m['params']:
            pattern='"'+p['label']
            if code.count(pattern)!=1:raise ValueError('ambiguous UI label '+pattern)
            p['compiled_label']='m'+str(m['index'])+'_'+p['label']
            code=code.replace(pattern,'"'+p['compiled_label'])
        m['compiled_source']=code;m['class']='PSM'+str(m['index'])
        path=root/(m['class']+'.dsp');path.write_text(code)
        records.append(command(['faust','-lang','cpp','-os','-ec','-cn',m['class'],path,'-o',root/(m['class']+'.h')],root/(m['class']+'.log')))
    return modules

def full_program(modules,graph):
    edges=graph['edges'];delays=[e for e in edges if e['delay']]
    by={m['index']:m for m in modules}
    declarations='\n'.join('e%d=environment{\n%s\n};'%(m['index'],m['compiled_source']) for m in modules)
    lines=[]
    for i in graph['order']:
        sums=['0.0','0.0']
        for e in edges:
            if e['target']!=i:continue
            for c in range(2):
                x=('fb%d'% (delays.index(e)*2+c)) if e['delay'] else '(v%d : %s)'%(e['source'],'_,!' if c==0 else '! ,_')
                sums[c]+='+('+x+'*'+number(e['gains'][c])[:-1]+')' if not e['delay'] else '+'+x
        lines.append('in%d=(%s),(%s);'%(i,*sums))
        lines.append(('v%d=(in%d : e%d.process);'%(i,i,i)) if by[i]['inputs'] else ('v%d=e%d.process;'%(i,i)))
    outputs=[]
    for m in modules:
        outputs+=['(v%d : _,!)'%m['index'],'(v%d : !,_)'%m['index'], '(in%d : _,!)'%m['index'],'(in%d : !,_)'%m['index']]
    if delays:
        fb=[]
        for e in delays:
            for c in range(2):fb.append('((v%d : %s)*%s)'%(e['source'],'_,!' if c==0 else '! ,_',number(e['gains'][c])[:-1]))
        args=','.join('fb%d'%i for i in range(len(fb)))
        expr='unit('+args+')='+','.join(fb+outputs)+' with {\n'+'\n'.join(lines)+'\n};\n'
        expr+='process=(unit ~ ('+','.join('_' for _ in fb)+')) : ('+','.join(['!']*len(fb)+['_']*len(outputs))+');\n'
    else:expr='process='+','.join(outputs)+' with {\n'+'\n'.join(lines)+'\n};\n'
    return declarations+'\n'+expr

def emit(root:Path,manifest_path:Path,prefix:Path):
    root.mkdir(parents=True,exist_ok=True);manifest=json.loads(manifest_path.read_text());records=[]
    modules=module_sources(manifest,root,records)
    abi={'modules':[{'id':m['id'],'index':m['index'],'source':sha(m['compiled_source']),'header':sha((root/(m['class']+'.h')).read_bytes())} for m in modules], 'flags':FLAGS,
         'compiler':subprocess.check_output(['clang++','--version'],text=True),'faust':subprocess.check_output(['faust','--version'],text=True)}
    schema=sha(json.dumps(abi,sort_keys=True));manifest['schema']=schema;manifest['abi']=abi
    # The same object types/Bank declaration are included in all code versions.
    header=PREAMBLE+'\n'.join('#include "%s.h"'%m['class'] for m in modules)+'\nstruct Bank {\n'
    header+='\n'.join('  %s m%d;'%(m['class'],m['index']) for m in modules)
    header+='\n  float previous[%d][2] = {};\n};\n'%(len(modules)*len(modules))
    header+='API const char* ps_schema(){return "'+schema+'";}\nAPI size_t ps_bytes(){return sizeof(Bank);}\n'
    header+='API void ps_tables(int rate){'+''.join(m['class']+'::classInit(rate);' for m in modules)+'}\n'
    # Create/init only belongs to the initial owner; these are never called on swaps.
    header+='API void* ps_create(int rate){auto* b=new Bank;'+''.join('b->m%d.instanceInit(rate);'%m['index'] for m in modules)+'return b;}\n'
    header+='API void ps_free(void* p){delete static_cast<Bank*>(p);}\n'
    header+='API float* ps_zone(void* p,int index,const char* name){auto* b=static_cast<Bank*>(p);MapUI ui;switch(index){'
    header+=''.join('case %d:b->m%d.buildUserInterface(&ui);break;'%(m['index'],m['index']) for m in modules)+'default:return nullptr;}return ui.getParamZone(name);}\n'
    header+='API void ps_control(void* p){auto* b=static_cast<Bank*>(p);'+''.join('b->m%d.control();'%m['index'] for m in modules)+'}\n'
    header+='API void ps_tick(void* p,int node,float* in,float* out){auto* b=static_cast<Bank*>(p);switch(node){'
    header+=''.join('case %d:b->m%d.%s::frame(in,out);break;'%(m['index'],m['index'],m['class']) for m in modules)+'}}\n'
    header+='API float* ps_feedback(void* p){return &static_cast<Bank*>(p)->previous[0][0];}\n'
    header+='API void ps_block(void* p,int node,int count,float** in,float** out){auto* b=static_cast<Bank*>(p);switch(node){'
    for m in modules:
        header+='case %d:for(int s=0;s<count;++s){float x[2]={in[0][s],in[1][s]},y[2];b->m%d.%s::frame(x,y);out[0][s]=y[0];out[1][s]=y[1];}break;'%(m['index'],m['index'],m['class'])
    header+='}}\n'
    (root/'bank.h').write_text(header)
    for variant,graph in manifest['graphs'].items():
        full=full_program(modules,graph);(root/('whole-'+variant+'.dsp')).write_text(full)
        records.append(command(['faust','-lang','cpp','-cn','Full'+variant,root/('whole-'+variant+'.dsp'),'-o',root/('whole-'+variant+'.h')],root/('whole-'+variant+'.log')))
        # One compiler-optimised sample loop, all frame functions visible/inlinable.
        code='#include "bank.h"\nAPI void ps_compute(void* __restrict p,int count,float** __restrict planes){auto* b=static_cast<Bank*>(p);\n'
        code+=''.join('b->m%d.control();'%m['index'] for m in modules)+'\nfor(int s=0;s<count;++s){\n'
        delays=[e for e in graph['edges'] if e['delay']]
        slots={m['index']:slot for slot,m in enumerate(modules)}
        for i in graph['order']:
            sums=['0.0f','0.0f']
            for e in graph['edges']:
                if e['target']!=i:continue
                for c in range(2):
                    x='b->previous[%d][%d]'%(e['key'],c) if e['delay'] else 'v%d[%d]'%(e['source'],c)
                    sums[c]+='+('+x+'*'+number(e['gains'][c])+')'
            cl=next(m['class'] for m in modules if m['index']==i)
            code+='float x%d[2]={%s,%s},v%d[2];b->m%d.%s::frame(x%d,v%d);\n'%(i,*sums,i,i,cl,i,i)
            j=slots[i]*4
            code+='planes[%d][s]=v%d[0];planes[%d][s]=v%d[1];planes[%d][s]=x%d[0];planes[%d][s]=x%d[1];\n'%(j,i,j+1,i,j+2,i,j+3,i)
        for e in delays:code+='b->previous[%d][0]=v%d[0];b->previous[%d][1]=v%d[1];\n'%(e['key'],e['source'],e['key'],e['source'])
        code+='}}\n';(root/('kernel-'+variant+'.cpp')).write_text(code)
        for kind in ('kernel','whole'):
            if kind=='whole':
                code=PREAMBLE+'#include "whole-'+variant+'.h"\n'
                code+='API void* ps_create(int sr){auto* d=new Full'+variant+';d->init(sr);return d;}\n'
                code+='API void ps_free(void* p){delete static_cast<Full'+variant+'*>(p);}\n'
                code+='API void ps_compute(void* p,int n,float** out){static_cast<Full'+variant+'*>(p)->compute(n,nullptr,out);}\n'
                code+='API float* ps_zone(void* p,int,const char* name){MapUI ui;static_cast<Full'+variant+'*>(p)->buildUserInterface(&ui);return ui.getParamZone(name);}\n'
                (root/('whole-'+variant+'.cpp')).write_text(code)
            cpp=root/(kind+'-'+variant+'.cpp');lib=root/(kind+'-'+variant+'.dylib')
            records.append(command(['clang++',*FLAGS,'-dynamiclib','-I'+str(prefix/'include'),cpp,'-o',lib],root/(kind+'-'+variant+'-build.log'),180))
            # Native bytes and generated LLVM retained, not only hash claims.
            records.append(command(['clang++',*FLAGS,'-S','-emit-llvm','-I'+str(prefix/'include'),cpp,'-o',root/(kind+'-'+variant+'.ll')],root/(kind+'-'+variant+'-ir.log'),180))
            (root/(kind+'-'+variant+'-symbols.txt')).write_text(subprocess.check_output(['nm','-g',str(lib)],text=True))
    manifest['builds']=records
    manifest['source_hashes']={p.name:sha(p.read_bytes()) for p in root.iterdir() if p.suffix in ('.cpp','.h','.dsp')}
    manifest['binary_hashes']={p.name:sha(p.read_bytes()) for p in root.glob('*.dylib')}
    (root/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    return manifest
