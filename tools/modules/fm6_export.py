"""Compact single-file FM6 export; preserve definitions, not a huge -e expression.

Only machine-local imports are inlined. Standard Faust libraries remain normal
compiler dependencies. No synthesis equations or parameter values are changed.
"""
from pathlib import Path
import argparse,re,json,os
import numpy as np
import fm6_delivery as fm


def export(module: Path, destination: Path) -> None:
    root=Path(module)/'v1'; engine=root/'engine'
    operator=(engine/'operator.lib').read_text()
    for filename in ('env.lib','lfo.lib','pitchenv.lib'):
        needle=f'import("{filename}");'
        if operator.count(needle)!=1:
            raise ValueError('Unexpected local import: '+filename)
        dependency=(engine/filename).read_text()
        dependency=re.sub(r'^(?:ba|ma|si) = library\("[^"\n]+"\);\n','',dependency,flags=re.M)
        operator=operator.replace(needle,dependency)
    operator=re.sub(r'^declare version [^\n]+\n','',operator,flags=re.M)
    voice=(root/'voice.lib').read_text()
    needle='dx=library("engine/operator.lib");'
    if voice.count(needle)!=1:raise ValueError('Unexpected voice import')
    voice=voice.replace(needle,'dx=environment {\n'+operator+'\n};')
    destination=Path(destination);destination.parent.mkdir(parents=True,exist_ok=True)
    destination.write_text('declare name "FM6 Classic";\ndeclare version "1.0.0-candidate";\n'+voice+'\nprocess=voice;\n')

def verify(module, destination, faust, libraries, baseline, out):
    """Fresh direct compilation and exact parity with the qualified multi-file voice."""
    module=Path(module);destination=Path(destination);baseline=Path(baseline)
    out=Path(out);out.mkdir(parents=True,exist_ok=True)
    if (out/'verification.json').exists():raise ValueError('Fresh verification directory required')
    faust=Path(faust).resolve();libraries=Path(libraries).resolve()
    header=out/'generated.hpp';exe=out/'render'
    fm.run([faust,'-I',libraries,'-lang','cpp','-single','-cn','ModuleDSP',destination,'-o',header],timeout=300)
    fm.run([os.getenv('CXX','c++'),'-std=c++17','-O2','-ffp-contract=off','-I'+str(out.resolve()),fm.ROOT/'tools/modules/render.cpp','-o',exe],timeout=180)
    checks=[];renders=0
    def check(name,ok,**detail):
        checks.append(dict(name=name,passed=bool(ok),**detail))
        if not ok:raise AssertionError(name)
    for name in ('C01','C02','C03'):
        d=baseline/'cases'/name;score=d/'candidate-44100-128.tsv';raw=out/(name+'.f32')
        original=np.fromfile(d/'candidate-44100-128.f32',dtype='<f4')
        fm.run([exe,score,raw,44100,128,len(original),0]);renders+=1
        x=np.fromfile(raw,dtype='<f4')
        check(name+':single-file-identical',np.array_equal(x,original))
    # The old MSFA oracle lacks AM. Exercise all six candidate LFO waves at
    # maximum AM depth against the qualified unmodulated carrier, not hardware.
    original=np.fromfile(baseline/'cases/C01/candidate-44100-128.f32',dtype='<f4')
    m=json.loads((module/'manifest.json').read_text());defaults={k:c['default'] for k,c in m['controls'].items() if k!='gate'}
    for wave in range(6):
        params=defaults|{'algorithm':1,'op1_level':80,'op1_ampmod':3,'lfo_amd':99,'lfo_speed':80,'lfo_wave':wave,'freq':220,'velocity':100/127}
        score=out/f'am-{wave}.tsv';raw=score.with_suffix('.f32')
        score.write_text(''.join(f'0\t{k}\t{v:.12g}\n' for k,v in params.items())+'2048\tgate\t1\n32768\tgate\t0\n')
        fm.run([exe,score,raw,44100,127,65536,0]);renders+=1
        x=np.fromfile(raw,dtype='<f4')
        check(f'am-{wave}:finite',len(x)==65536 and np.isfinite(x).all() and np.max(abs(x))<1)
        check(f'am-{wave}:observable',np.max(abs(x[6144:30720]-original[6144:30720]))>1e-5)
        check(f'am-{wave}:pre-note-silent',not np.any(x[:2048]))
    report={'status':'passed-export-and-candidate-AM-checks','checks':checks,'counts':{'checks':len(checks),'passed':sum(c['passed'] for c in checks),'renders':renders},'source_sha256':fm.lab.digest(destination),'binary_sha256':fm.lab.digest(exe),'qualified_commit':json.loads((baseline/'results.json').read_text())['provenance']['source_commit'],'AM_scope':'candidate-only behavior, no independent/hardware oracle','standard_libraries_required':True}
    (out/'verification.json').write_text(json.dumps(report,indent=2)+'\n')
    print(report['status'],report['counts'])

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--module',type=Path,required=True);p.add_argument('--out',type=Path,required=True)
    p.add_argument('--faust',type=Path);p.add_argument('--libraries',type=Path);p.add_argument('--baseline',type=Path);p.add_argument('--verify-out',type=Path)
    a=p.parse_args();export(a.module,a.out)
    if any((a.faust,a.libraries,a.baseline,a.verify_out)):
        if not all((a.faust,a.libraries,a.baseline,a.verify_out)):p.error('All verification paths are required together')
        verify(a.module,a.out,a.faust,a.libraries,a.baseline,a.verify_out)
