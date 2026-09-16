#!/usr/bin/env python3
"""Resume the existing completion suite without rebuilding unchanged Faust DSP.
This is a cache adapter, not a different suite: every completion check executes.
Reused results retain their original commit; source/compiler/renderer/binary
identities are checked before use. Run only on the established physical Mac queue.
"""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil

from tx81z_completion import Completion, source_closure
from tx81z_patch_qualification import ROOT, RENDERER_SHA, sha
from tx81z_patch import decode_sysex, to_controls

class CachedCompletion(Completion):
    def verify_manifest_file(self, root, relative):
        manifest=json.loads((root/'SHA256SUMS.json').read_text())
        self.check('cache:file:'+str(relative),relative in manifest and sha(root/relative)==manifest[relative])

    def reuse_baseline(self):
        base=self.a.baseline
        self.verify_manifest_file(base,'results.json')
        self.base_report=json.loads((base/'results.json').read_text()); r=self.base_report
        self.check('baseline:completed',r['status']=='completed' and bool(r['checks']) and all(c['passed'] for c in r['checks']))
        self.check('baseline:identified-old-source',r['commit']==self.a.reuse_source)
        self.check('baseline:same-compiler',r['compiler']['faust_sha256']==sha(self.a.faust))
        actual=source_closure(ROOT/'modules/tx81z/v10/voice.dsp')
        self.check('baseline:exact-source-closure',actual==r['source_closure'])
        # A changed test/decoder is not relabelled a previously executed suite.
        self.run(['git','diff','--exit-code',self.a.reuse_source,'HEAD','--',
                  'tools/modules/tx81z_patch_qualification.py','tools/modules/tx81z_patch.py',
                  'tools/modules/tx81z_prepare.py','tests/test_tx81z_patch.py'])
        self.baseline=base/'scalar/render'; self.vector=base/'vector/render'
        for kind in ('scalar','vector'):
            for filename in ('generated.hpp','render.cpp','render','controls.tsv'):
                self.verify_manifest_file(base,kind+'/'+filename)
            self.check('baseline:generated:'+kind,sha(base/kind/'generated.hpp')==r['builds'][kind]['generated_sha256'])
            self.check('baseline:renderer:'+kind,sha(base/kind/'render.cpp')==RENDERER_SHA)
        self.report['baseline']={'commit':r['commit'],'results_sha256':sha(base/'results.json'),
            'checks':r['check_count'],'source_closure':actual,
            'execution':'reused earlier qualified builds after exact source/compiler/renderer/binary checks; baseline99 not falsely relabelled rerun'}
        self.report['resume']={'source_commit':self.a.reuse_source,
            'prior_completion_results_sha256':sha(self.a.reuse_completion/'results.json'),
            'new_completion_checks':'all execute again; no check bypass or tolerance change'}
        shutil.copy2(base/'results.json',self.out/'baseline-result.json')
        self.patches={}
        for name in ('Filter1','Filter2','FilterBass'):
            raw=base/'references'/(name+'.syx')
            self.check('patch:identity:'+name,sha(raw)==r['references'][name]['sha256'])
            patch=decode_sysex(raw.read_bytes()); controls,limits=to_controls(patch)
            self.patches[name]=controls
        self.report['references']=r['references']

    def build(self,name,source=None,vector=False,native=False):
        if name!='recomposed':
            return super().build(name,source,vector,native)
        old=self.a.reuse_completion
        self.verify_manifest_file(old,'results.json')
        record=json.loads((old/'results.json').read_text())
        self.check('cache:source-identity',record['commit']==self.a.reuse_source)
        self.check('cache:compiler-identity',record['compiler']['faust_sha256']==sha(self.a.faust))
        self.check('cache:build-mode',not vector and not native and record['builds'][name]['vector'] is False)
        closure=source_closure(source)
        for relative,digest in closure.items():
            if relative=='modules/tx81z/v5/opz_tables.lib':
                expected=self.base_report['source_closure'][relative]
            else:
                text=self.run(['git','show',self.a.reuse_source+':'+relative])
                expected=hashlib.sha256(text.encode('utf-8')).hexdigest()
            self.check('cache:source:'+relative,digest==expected)
        dest=self.out/name; dest.mkdir()
        for filename in ('generated.hpp','render.cpp','render','controls.tsv'):
            self.verify_manifest_file(old,name+'/'+filename)
            shutil.copy2(old/name/filename,dest/filename)
        self.check('cache:generated-header',sha(dest/'generated.hpp')==record['builds'][name]['generated_sha256'])
        self.check('cache:reviewed-renderer',sha(dest/'render.cpp')==RENDERER_SHA)
        self.report['builds'][name]={**record['builds'][name],
            'reused':True,'reused_source_commit':self.a.reuse_source,
            'source_closure':closure,'seconds_in_this_run':0,
            'timing_note':'seconds is the original build measurement, not new compilation'}
        return dest/'render'

    def alternate_and_package(self):
        super().alternate_and_package()
        target=self.out/'delivery/tools/modules/tx81z_resume_completion.py'
        shutil.copy2(Path(__file__),target)
        shutil.copy2(self.out/'baseline-result.json',self.out/'delivery/baseline-result.json')

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    for key in ('out','faust','renderer','ymfm','baseline','reuse-completion'):
        ap.add_argument('--'+key,type=Path,required=True)
    ap.add_argument('--reuse-source',required=True)
    a=ap.parse_args()
    if not re.fullmatch('[0-9a-f]{40}',a.reuse_source):ap.error('exact reuse source SHA required')
    for key in ('out','faust','renderer','ymfm','baseline','reuse_completion'):
        setattr(a,key,getattr(a,key).resolve())
    q=CachedCompletion(a)
    try:q.execute()
    except Exception as e:q.report['status']='failed'; q.report['error']=str(e); raise
    finally:q.save()
    print(json.dumps({'status':q.report['status'],'checks':q.report['check_count'],
                      'renders':q.report['render_count'],'pairs':len(q.report['recomposition'])}))
if __name__=='__main__':main()
