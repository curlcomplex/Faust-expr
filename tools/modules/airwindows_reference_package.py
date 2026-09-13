"""Make a verified bounded review artifact without discarding full raw evidence."""
from pathlib import Path
import argparse,hashlib,json,shutil

def digest(p):
    h=hashlib.sha256()
    with p.open('rb') as f:
        for b in iter(lambda:f.read(1048576),b''):h.update(b)
    return h.hexdigest()
def main(root,out):
    root=Path(root);out=Path(out);out.mkdir(parents=True,exist_ok=True);verified=[]
    for rp in [root/'results.json',root/'precision/results.json',root/'legacy-regression/results.json']:
        if not rp.exists():continue
        report=json.loads(rp.read_text());base=rp.parent
        for rec in report['renders']:
            for suffix,key in [('.f32','raw_sha256'),('.tsv','score_sha256'),('-input.f32','input_sha256')]:
                f=base/(rec['name']+suffix)
                if key not in rec:continue
                assert digest(f)==rec[key],str(f)
                verified.append(str(f.relative_to(root)))
        for name,b in report['builds'].items():
            f=base/name/'generated.hpp';assert digest(f)==b['generated_sha256'];verified.append(str(f.relative_to(root)))
    for f in root.rglob('*'):
        if not f.is_file():continue
        rel=f.relative_to(root);n=str(rel)
        keep=f.name in ('source.tar','commit.txt','diff-check.txt','results.json','generated.hpp','controls.tsv') or f.suffix=='.tsv' or n.startswith('upstream/') or n.startswith('audition/')
        if not keep:continue
        dest=out/rel;dest.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(f,dest)
    (out/'verification.json').write_text(json.dumps({'verified_original_records':verified,'source_commit':(root/'commit.txt').read_text().strip(),'full_raw_evidence_retained':True,'review_file_sha256':{str(f.relative_to(out)):digest(f) for f in out.rglob('*') if f.is_file()}},indent=2))
    size=sum(f.stat().st_size for f in out.rglob('*') if f.is_file())
    assert size<450_000_000,size
    print(json.dumps({'verified_records':len(verified),'review_bytes':size}),flush=True)
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--source',required=True);p.add_argument('--out',required=True);a=p.parse_args();main(a.source,a.out)
