"""TR-606 real-hardware reference acquisition and descriptor pass.
Primary neutral archive: Roland Clan TR-606 sample set.  Reference bytes are
fetched at CI runtime and are never committed or repackaged.
"""
from pathlib import Path
import argparse, io, json, urllib.request, zipfile

URL='https://www.rolandclan.com/media/39/Roland_TR-606.zip'

def fetch_zip():
    req=urllib.request.Request(URL, headers={'User-Agent':'Mozilla/5.0 Faust-expr hardware-reference lab','Referer':'https://www.rolandclan.com/library/tr-606/'})
    with urllib.request.urlopen(req, timeout=30) as r:
        data=r.read(8_000_001)
    if len(data)>8_000_000: raise RuntimeError('reference archive unexpectedly large')
    return data

def run(out):
    out=Path(out); out.mkdir(parents=True,exist_ok=True)
    data=fetch_zip()
    z=zipfile.ZipFile(io.BytesIO(data))
    names=[n for n in z.namelist() if not n.endswith('/')]
    report={'source_url':URL,'files':names,'bytes':len(data)}
    (out/'tr606-source-probe.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--out',required=True);a=p.parse_args();run(a.out)
