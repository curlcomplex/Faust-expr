"""Reference acquisition/inspection only; no hardware-approval claim.

Third-party audio remains transient except the Juno-106 archive whose author
explicitly permits redistribution. Other sources are linked, not repackaged.
"""
from pathlib import Path
import argparse, hashlib, html, io, json, re, urllib.parse, urllib.request, zipfile
import numpy as np
import soundfile as sf
from synth_sample_probe import candidate_windows, estimate_pitch, spectrum

J60='https://raw.githubusercontent.com/pendragon-andyh/Juno60/beab8655781db7dadf2bf9fa6e3d4b6514de3132/'
J60_FILES={
 'j60-saw':('Chorus/Juno 60 SawChorus0.wav','c0cea49399dff209a9e254af5b50f970236b80a7'),
 'j60-pulse':('Chorus/Juno 60 PulseChorus0.wav','58e59d5959f6c74aae031224dde9e6f1fe4ba0c5'),
 'j60-mix':('DCO/Mixdown/Juno 60 Pse+Saw.wav',None),
}
J106_URL='https://files.scene.org/get/mirrors/hornet/music/samples/swjuno2.zip'
J106_SHA='b671f055d388d739a65bb4ec0279d3eabd064e729212dda19d93d25fbc69f5f9'
PAGES={
 'dms':'https://www.dancemidisamples.com/download-free-analogue-synthesizer-wavetables-from-dancemidisamples/',
 'preve':'https://www.symplesound.com/shop/oneshots1',
 'legowelt':'https://legowelt.org/samples/',
}

def fetch(url,limit=150_000_000):
    req=urllib.request.Request(url,headers={'User-Agent':'Mozilla/5.0 (Faust-expr reference study)'})
    with urllib.request.urlopen(req,timeout=40) as r:
        data=r.read(limit+1); final=r.url
    if len(data)>limit: raise ValueError('download byte limit')
    return data,final

def sha(data):return hashlib.sha256(data).hexdigest()
def gitblob(data):return hashlib.sha1(b'blob '+str(len(data)).encode()+b'\0'+data).hexdigest()

def describe(raw):
    x,sr=sf.read(io.BytesIO(raw),always_2d=True,dtype='float64'); x=x.mean(axis=1)
    if len(x)<64 or not np.isfinite(x).all():raise ValueError('invalid audio')
    win=candidate_windows(raw)
    result=dict(sr=sr,frames=len(x),seconds=len(x)/sr,sha256=sha(raw),git_blob=gitblob(raw),
                peak=float(abs(x).max()),rms=float(np.sqrt(np.mean(x*x))),windows=[])
    for score,a,b,f,q,fl,fr,drift,features in win:
        result['windows'].append(dict(start=a/sr,end=b/sr,frequency=f,periodicity=q,
            pitch_drift_octaves=drift,harmonic_db=features.tolist()))
    # A compact energy envelope allows note-boundary inspection without copying audio.
    stride=max(1,round(.01*sr)); n=len(x)//stride
    energy=np.sqrt(np.mean(x[:n*stride].reshape(n,stride)**2,axis=1))
    result['envelope_10ms_rms']=energy.tolist()
    return result

def run(out):
    out.mkdir(parents=True,exist_ok=True)
    report=dict(acquisition={},page_links={},hardware_approved=False,qualification_complete=False)
    for name,(path,expected) in J60_FILES.items():
        url=J60+urllib.parse.quote(path,safe='/')
        try:
            raw,final=fetch(url); actual=gitblob(raw)
            if expected and actual!=expected:raise ValueError('Juno60 Git blob mismatch')
            report['acquisition'][name]=dict(url=url,final_url=final,path=path,**describe(raw))
        except Exception as e:report['acquisition'][name]=dict(url=url,error=str(e))
    try:
        raw,final=fetch(J106_URL)
        if sha(raw)!=J106_SHA:raise ValueError('Juno106 archive hash mismatch')
        z=zipfile.ZipFile(io.BytesIO(raw)); permitted=out/'j106-original';permitted.mkdir(exist_ok=True)
        for file in z.namelist():
            basename=Path(file).name.upper()
            if basename in ('JUNO2-3.WAV','JUNO2-4.WAV'):
                b=z.read(file); (permitted/basename).write_bytes(b)
                report['acquisition'][basename]=dict(url=J106_URL,archive_sha256=sha(raw),file=file,
                    eligibility='Capture notes explicitly say no chorus; panel state incomplete',**describe(b))
            elif basename=='SWJUNO2.TXT':
                (permitted/basename).write_bytes(z.read(file))
    except Exception as e:report['acquisition']['j106']=dict(url=J106_URL,error=str(e))
    for name,url in PAGES.items():
        try:
            raw,final=fetch(url,limit=5_000_000); text=raw.decode('utf-8','replace')
            (out/(name+'-page.html')).write_text(text)
            links=[urllib.parse.urljoin(final,html.unescape(x)) for x in re.findall(r'href=[\"\']([^\"\']+)',text)]
            report['page_links'][name]=dict(url=final,links=sorted(set(x for x in links if any(k in x.lower() for k in ('.zip','we.tl','bit.ly','download','static1.squarespace','dropbox')))))
            # Context around the specific hardware pack prevents using neighboring synths.
            text=re.sub(r'<[^>]+>',' ',text)
            key='MINIMOOG' if name=='legowelt' else 'SH-101'
            report['page_links'][name]['context']=text[max(0,text.upper().find(key)-250):text.upper().find(key)+2000]
        except Exception as e:report['page_links'][name]=dict(url=url,error=str(e))
    lib=Path('/usr/share/faust/oscillators.lib')
    if lib.exists():(out/'compiler-oscillators.lib').write_bytes(lib.read_bytes())
    report['acquired_audio_count']=sum('sha256' in x for x in report['acquisition'].values())
    (out/'inventory.json').write_text(json.dumps(report,indent=2))
    print(json.dumps({k:v for k,v in report.items() if k!='acquisition'},indent=2),flush=True)
    print('ACQUISITION',json.dumps({k:{kk:vv for kk,vv in v.items() if kk not in ('windows','envelope_10ms_rms')} for k,v in report['acquisition'].items()}),flush=True)
    return 0 if all('sha256' in report['acquisition'].get(k,{}) for k in J60_FILES) else 1

if __name__=='__main__':
    ap=argparse.ArgumentParser();ap.add_argument('--out',type=Path,required=True)
    a=ap.parse_args();raise SystemExit(run(a.out))
