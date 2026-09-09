#!/usr/bin/env python3
"""Read-only postflight over a completed native artifact; no benchmark evaluator imports."""
from pathlib import Path
import argparse,hashlib,json,re,tarfile
import numpy as np

def digest(path):
    h=hashlib.sha256()
    with path.open('rb') as f:
        for x in iter(lambda:f.read(1048576),b''):h.update(x)
    return h.hexdigest()
def read(p):return json.loads(p.read_text())
def compare(x,y):
    assert x.shape==y.shape and np.isfinite(x).all() and np.isfinite(y).all()
    d=np.abs(x.astype('f8')-y.astype('f8'));assert np.all(d<=1e-5+1e-5*np.abs(y))
    return float(d.max())
def main(root,out):
    out.mkdir(parents=True,exist_ok=True);identity=read(root/'identity.json');archive=root/'executed-experiment-source.tar.gz'
    checked=[]
    with tarfile.open(archive) as tf:
        for name,want in identity['source_sha256'].items():
            member=tf.getmember(name);assert member.isfile()
            data=tf.extractfile(member).read();assert hashlib.sha256(data).hexdigest()==want,name;checked.append(name)
    host=[]
    for case in read(root/'gate/cases.json'):
        assert case['returncode']==0
        p=root/'gate'/case['name'];meta=read(p/'result.json');assert meta['channels']==2*(case['size']+3)
        outputs=np.fromfile(p/'shared-optimized.f32',dtype='<f4').reshape(8192,meta['channels'])
        actual=np.fromfile(p/'actual-product-host.f32',dtype='<f4').reshape(8192,2)
        first=2*(case['size']+1);error=compare(outputs[:,first:first+2],actual)
        host.append({'name':case['name'],'host_max_error':error,'host_peak':float(np.abs(actual).max())})
    code=[]
    for folder in sorted((root/'kernels').iterdir()):
        if not folder.is_dir():continue
        manifest=read(folder/'manifest.json')
        for variant in ('A','B'):
            llvm=(folder/f'kernel-{variant}.ll').read_text()
            match=re.search(r'^define[^\n]*@ps_compute\([^\n]*\)[^\n]*\{\n(.*?)^\}',llvm,re.M|re.S)
            assert match,'ps_compute definition missing in '+str(folder)
            calls=re.findall(r'\b(?:call|invoke)\b[^\n]*',match.group(1))
            frames=[s for s in calls if 'frame' in s and 'PSM' in s]
            indirect=[s for s in calls if re.search(r'\b(?:call|invoke)\b[^@\n]*%[\w.]+\(',s)]
            source=(folder/f'kernel-{variant}.cpp').read_text()
            assert 'for(int s=0;s<count;++s)' in source
            assert all(s not in source for s in ['instanceInit','instanceClear','ps_create','memcpy'])
            code.append({'shape':folder.name,'variant':variant,'remaining_module_frame_calls':len(frames),'indirect_calls':len(indirect),
                         'direct_call_lines':calls,'kernel_sha256':digest(folder/f'kernel-{variant}.dylib'),'schema':manifest['schema']})
    summary={'native_head':identity['head'],'checked_executed_sources':checked,'source_archive_sha256':digest(archive),
             'product_host_output_comparisons':host,'compiled_loop_inspection':code,
             'all_module_frame_calls_inlined':all(c['remaining_module_frame_calls']==0 for c in code),
             'scope':'IR inspection and source/hash validation do not prove arbitrary state equivalence or race freedom'}
    (out/'postflight-contract.json').write_text(json.dumps(summary,indent=2)+'\n')
    print('POSTFLIGHT_SOURCE_AND_OUTPUT_PASS',len(checked),'sources',len(host),'actual host output captures',len(code),'optimized kernels',flush=True)
    print('POSTFLIGHT_HOST_MAX_ERROR',max(h['host_max_error'] for h in host),flush=True)
    for c in code:print('INLINE_INSPECTION',c['shape'],c['variant'],'frame_calls',c['remaining_module_frame_calls'],'indirect_calls',c['indirect_calls'],flush=True)
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('root',type=Path);p.add_argument('out',type=Path);a=p.parse_args();main(a.root,a.out)
