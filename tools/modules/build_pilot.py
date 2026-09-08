"""Compile unchanged baseline and candidate 02 using actual Faust; no fitting."""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

ROOT=Path(__file__).resolve().parents[2]
def sha(p): return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out',type=Path,default=ROOT/'build/kick-pm-02')
    args=p.parse_args(); out=args.out.resolve(); out.mkdir(parents=True,exist_ok=True)
    faust=os.environ.get('FAUST','faust'); cxx=os.environ.get('CXX','c++')
    report={'source_commit':None,'builds':{},'passed':False,'scope':'actual Faust generation/native compile, not reference fit'}
    def run(cmd):
        r=subprocess.run(cmd,cwd=ROOT,capture_output=True,text=True,timeout=120)
        with (out/'build.log').open('a') as f: f.write(json.dumps(cmd)+'\n'+r.stdout+r.stderr+'\n')
        if r.returncode: raise RuntimeError('command failed: '+json.dumps(cmd)+'\n'+r.stderr)
        return r.stdout
    try:
        report['source_commit']=run(['git','rev-parse','HEAD']).strip()
        report['faust']=run([faust,'-v']).strip(); report['cxx']=run([cxx,'--version']).strip()
        for name,source,vector in [('baseline',ROOT/'modules/kick-pm/kick.dsp',False),
                                  ('body-02',ROOT/'modules/kick-pm/candidates/body-02.dsp',False),
                                  ('body-02-vector',ROOT/'modules/kick-pm/candidates/body-02.dsp',True)]:
            d=out/name; d.mkdir(exist_ok=True)
            run([faust,'-e',str(source),'-o',str(d/'expanded.dsp')])
            flags=['-lang','cpp','-single','-cn','ModuleDSP']+(['-vec','-lv','0','-vs','32'] if vector else [])
            run([faust,*flags,str(d/'expanded.dsp'),'-o',str(d/'generated.hpp')])
            native=['-std=c++17','-O2','-ffp-contract=off','-I'+str(d)]
            run([cxx,*native,str(ROOT/'tools/modules/render.cpp'),'-o',str(d/'render')])
            run([cxx,*native,'-shared','-fPIC',str(ROOT/'tools/modules/fit_api.cpp'),'-o',str(d/'kernel.so')])
            controls=run([str(d/'render'),'--controls']); (d/'controls.tsv').write_text(controls)
            report['builds'][name]={'source':str(source.relative_to(ROOT)),'source_sha256':sha(source),
                'expanded_sha256':sha(d/'expanded.dsp'),'generated_sha256':sha(d/'generated.hpp'),
                'native_sha256':sha(d/'render'),'library_sha256':sha(d/'kernel.so'),
                'controls_sha256':sha(d/'controls.tsv'),'faust_flags':flags,'native_flags':native}
        report['passed']=True
    except Exception as e:
        report['failure']=str(e)
    finally:
        for rel in ['tools/modules/render.cpp','tools/modules/fit_api.cpp','tools/modules/build_pilot.py',
                    'modules/kick-pm/kick.dsp','modules/kick-pm/candidates/body-02.dsp']:
            dest=out/'source'/rel; dest.parent.mkdir(parents=True,exist_ok=True); shutil.copyfile(ROOT/rel,dest)
        (out/'build.json').write_text(json.dumps(report,indent=2)+'\n'); print(json.dumps(report))
    if not report['passed']: raise SystemExit(1)
if __name__=='__main__': main()
