"""Inspect public SH-101 and Model-D archives without republishing audio.
Uses the ordinary public download request for the producer-linked WeTransfer
file. Authentication/expired-transfer/CAPTCHA errors are retained, never bypassed.
"""
from pathlib import Path
import argparse, http.cookiejar, io, json, re, urllib.request, zipfile
import numpy as np
import soundfile as sf
from synth_finish_materials import fetch, sha
from synth_sample_probe import candidate_windows
SH_URL='https://cdn.mos.musicradar.com/audio/samples/musicradar-roland-sh101-samples.zip'
SH_SHA='949bf42a6af79efdc40bb9d99113ae28ed10f38c2f3b2dc6e6fcbcc18a7cdd45'
MINI_PAGE='https://legowelt.wetransfer.com/downloads/f1471b9a8a28ab96a7a8b7b4ee28332020180329170955/1b03c9'
TRANSFER='f1471b9a8a28ab96a7a8b7b4ee28332020180329170955'

def mini_download():
    opener=urllib.request.build_opener(urllib.request.HTTPCookieProcessor(http.cookiejar.CookieJar()))
    opener.addheaders=[('User-Agent','Mozilla/5.0 (Faust-expr reference study)')]
    with opener.open(MINI_PAGE,timeout=40) as r:page=r.read(5_000_000).decode('utf-8','replace')
    m=re.search(r'<script[^>]+id="__NEXT_DATA__"[^>]*>(.*?)</script>',page,re.S)
    if not m:raise ValueError('public download page has no metadata')
    props=json.loads(m.group(1))['props']['pageProps']
    if props.get('metadata',{}).get('title')!='Legowelt MINIMOOG sample pack.zip':
        raise ValueError('unexpected producer-linked archive title')
    payload=json.dumps({'security_hash':props['securityHash'],'intent':'entire_transfer'}).encode()
    request=urllib.request.Request('https://legowelt.wetransfer.com/api/v4/transfers/'+TRANSFER+'/download',data=payload,
        headers={'Content-Type':'application/json','Accept':'application/json','Referer':MINI_PAGE},method='POST')
    with opener.open(request,timeout=40) as r:result=json.loads(r.read(2_000_000))
    link=result.get('direct_link')
    if not link:raise ValueError('ordinary download response has no direct link: '+str(sorted(result)))
    return fetch(link,limit=160_000_000)[0]

def inventory(raw):
    z=zipfile.ZipFile(io.BytesIO(raw)); rows=[];notes={}
    for n in z.namelist():
        if n.startswith('__MACOSX/'):continue
        if n.lower().endswith(('.txt','.md','.nfo')) and z.getinfo(n).file_size<100000:
            notes[n]=z.read(n).decode('utf-8','replace')
        if not n.lower().endswith('.wav'):continue
        data=z.read(n)
        try:
            x,sr=sf.read(io.BytesIO(data),always_2d=True,dtype='float64')
            row=dict(file=n,sr=sr,frames=len(x),seconds=len(x)/sr,sha256=sha(data),peak=float(abs(x).max()),channels=x.shape[1])
            wins=candidate_windows(data)
            row['periodic_windows']=[dict(start=a/sr,end=b/sr,frequency_hz=f,periodicity=q,harmonic_db=h.tolist()) for _,a,b,f,q,fl,fr,d,h in wins[:2]]
            rows.append(row)
        except Exception as e:rows.append(dict(file=n,error=str(e)))
    return dict(archive_sha256=sha(raw),bytes=len(raw),files=rows,notes=notes)

def run(out):
    out.mkdir(parents=True,exist_ok=True);report={}
    for name in ('sh101','minimoog'):
        try:
            raw=fetch(SH_URL,limit=350_000_000)[0] if name=='sh101' else mini_download()
            if name=='sh101' and sha(raw)!=SH_SHA:raise ValueError('SH101 archive hash mismatch')
            report[name]=inventory(raw)
            report[name]['url']=SH_URL if name=='sh101' else MINI_PAGE
        except Exception as e:report[name]=dict(error=str(e),url=SH_URL if name=='sh101' else MINI_PAGE)
    (out/'inventory.json').write_text(json.dumps(report,indent=2))
    print('EXTRA_REFERENCE_INVENTORY',json.dumps({k:{a:b for a,b in v.items() if a not in ('files','notes')}|{'wav_count':len(v.get('files',[]))} for k,v in report.items()}),flush=True)
    return report

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--out',required=True,type=Path)
    a=p.parse_args();run(a.out)
