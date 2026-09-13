"""Read one immutable completed-run artifact, verify it, make a smaller review copy.
Full raw artifact is NOT modified/deleted. No downloaded code is executed.
"""
import hashlib,json,os,shutil,urllib.request,urllib.error,zipfile
from pathlib import Path
ARTIFACT=10324622090
EXPECTED='fd5dcc1251fd1fa499ffaf4fb7a89d0d5e69b3f8245b4331888ded0313d7148d'
class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self,*args,**kwargs):return None
url=f'https://api.github.com/repos/curlcomplex/Faust-expr/actions/artifacts/{ARTIFACT}/zip'
req=urllib.request.Request(url,headers={'Authorization':'Bearer '+os.environ['GH_TOKEN'],'Accept':'application/vnd.github+json'})
op=urllib.request.build_opener(NoRedirect)
try:
    response=op.open(req,timeout=30)
except urllib.error.HTTPError as e:
    if e.code not in (301,302,303,307,308):raise
    target=e.headers['Location']
    if not target.startswith('https://'):raise RuntimeError('non-HTTPS artifact redirect')
    # Never forward the repository token to blob storage.
    response=urllib.request.urlopen(target,timeout=120)
archive=Path('build/full-airwindows.zip');archive.parent.mkdir(exist_ok=True)
with response,archive.open('wb') as f:shutil.copyfileobj(response,f)
h=hashlib.sha256()
with archive.open('rb') as f:
    for chunk in iter(lambda:f.read(1048576),b''):h.update(chunk)
assert h.hexdigest()==EXPECTED
out=Path('build/airwindows-review');out.mkdir(exist_ok=True)
verified=[]
with zipfile.ZipFile(archive) as z:
    assert z.testzip() is None
    report=json.loads(z.read('results.json'))
    for rec in report['renders']:
        for suffix,key in [('.f32','raw_sha256'),('.tsv','score_sha256'),('-input.f32','input_sha256')]:
            fn=rec['name']+suffix
            assert hashlib.sha256(z.read(fn)).hexdigest()==rec[key],fn
            verified.append(fn)
    for name,b in report['builds'].items():
        fn=name+'/generated.hpp';assert hashlib.sha256(z.read(fn)).hexdigest()==b['generated_sha256'];verified.append(fn)
    for info in z.infolist():
        n=info.filename
        keep=n in ('source.tar','commit.txt','diff-check.txt','results.json') or n.startswith('upstream/') or n.startswith('audition/') or n.endswith('generated.hpp') or n.endswith('controls.tsv') or n.endswith('.tsv') or n=='legacy-regression/results.json'
        if not keep or info.is_dir():continue
        p=out/n
        if not p.resolve().is_relative_to(out.resolve()):raise RuntimeError('unsafe zip member')
        p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes(z.read(info))
(out/'verification.json').write_text(json.dumps({'original_artifact':ARTIFACT,'original_sha256':EXPECTED,'original_unchanged':True,'verified_files':verified,'note':'Full raw audio stays in original GitHub artifact; this review bundle retains sources, scores, generated code, reports and auditions.'},indent=2))
for module in ('tape','ensemble'):
    for implementation in ('previous','revised','double'):
        rows=[r for r in report['comparisons'] if r['module']==module and r['implementation']==implementation and r['kind']=='mixed']
        print(module,implementation,'worst_rel_rms',max(r['relative_rms'] for r in rows),'worst_max_abs',max(r['max_abs'] for r in rows),flush=True)
print(json.dumps({'checks':len(report['checks']),'renders':len(report['renders']),'builds':len(report['builds']),'verified':len(verified),'review_bytes':sum(p.stat().st_size for p in out.rglob('*') if p.is_file())}),flush=True)
