"""Inventory accessible hardware sample references for the Analog Classics synth pass.

Reference WAVs are downloaded transiently and NEVER copied into the artifact.
Only provenance, archive hashes, filenames and derived descriptors are retained.
Commercial/purchase-gated corpora remain external listening references.
"""
from pathlib import Path
import argparse, hashlib, io, json, re, urllib.request, zipfile
import numpy as np
from scipy.io import wavfile
from scipy.signal import welch

SOURCES = {
  "juno60_hornet_1997": [
    "https://files.scene.org/get/mirrors/hornet/music/samples/aq-j60v1.zip",
    "https://files.scene.org/mirrors/hornet/music/samples/aq-j60v1.zip",
  ],
  "juno106_soundwave_1_1997": [
    "https://files.scene.org/get/mirrors/hornet/music/samples/swjuno1.zip",
    "https://files.scene.org/mirrors/hornet/music/samples/swjuno1.zip",
  ],
  "juno106_soundwave_2_1997": [
    "https://files.scene.org/get/mirrors/hornet/music/samples/swjuno2.zip",
    "https://files.scene.org/mirrors/hornet/music/samples/swjuno2.zip",
  ],
  "sh101_musicradar": [
    "https://cdn.mos.musicradar.com/audio/samples/musicradar-roland-sh101-samples.zip",
  ],
}

NOTES = {
 "juno60_hornet_1997": "Legacy Juno-60 hardware WAV archive; capture settings incomplete. Spectral/envelope cross-check only, not absolute gain or exact-knob calibration.",
 "juno106_soundwave_1_1997": "Legacy Juno-106 hardware WAV archive; default C3 unless filename says otherwise; settings metadata limited.",
 "juno106_soundwave_2_1997": "Second legacy Juno-106 hardware WAV archive; settings metadata limited.",
 "sh101_musicradar": "Royalty-free SH-101 pack from Computer Music/SampleRadar. Loops/FX rather than controlled calibration notes; qualitative timbre evidence only.",
}

def sha(b): return hashlib.sha256(b).hexdigest()

def fetch(urls):
    errors=[]
    for u in urls:
        try:
            req=urllib.request.Request(u, headers={"User-Agent":"Mozilla/5.0 Faust-expr reference study"})
            with urllib.request.urlopen(req, timeout=60) as r:
                data=r.read()
            if len(data)<1000: raise RuntimeError(f"short response {len(data)}")
            return u,data,errors
        except Exception as e: errors.append(f"{u}: {e}")
    raise RuntimeError(" | ".join(errors))

def mono_float(sr,x):
    x=np.asarray(x)
    if x.ndim>1: x=x.mean(axis=1)
    if np.issubdtype(x.dtype,np.integer):
        scale=float(max(abs(np.iinfo(x.dtype).min),np.iinfo(x.dtype).max)); x=x.astype(np.float64)/scale
    else: x=x.astype(np.float64)
    return int(sr),x

def desc(sr,x):
    if len(x)==0: return {"frames":0}
    a=np.abs(x); peak=float(a.max()); rms=float(np.sqrt(np.mean(x*x)))
    # Work on at most the first 4 seconds; descriptors are deliberately coarse.
    y=x[:min(len(x),sr*4)]
    if np.max(np.abs(y))>0:
        f,p=welch(y,fs=sr,nperseg=min(8192,len(y)))
        ps=float(p.sum())
        centroid=float((f*p).sum()/ps) if ps else 0.0
        roll=float(f[np.searchsorted(np.cumsum(p),.85*ps)]) if ps else 0.0
    else: centroid=roll=0.0
    e=x*x; total=float(e.sum()); e90=0.0
    if total>0: e90=float(np.searchsorted(np.cumsum(e),.90*total)/sr)
    return {"frames":int(len(x)),"seconds":float(len(x)/sr),"peak":peak,"rms":rms,"centroid_hz":centroid,"rolloff85_hz":roll,"energy90_s":e90}

def run(out):
    out.mkdir(parents=True,exist_ok=True)
    report={"policy":"No reference WAV or archive bytes retained; descriptors are diagnostic, not authenticity scores.","sources":{}}
    success=0
    for name,urls in SOURCES.items():
        item={"note":NOTES[name],"attempted_urls":urls}
        try:
            used,data,errors=fetch(urls); item.update(url=used,archive_sha256=sha(data),archive_bytes=len(data),fallback_errors=errors)
            z=zipfile.ZipFile(io.BytesIO(data)); rows=[]
            for fn in z.namelist():
                if not fn.lower().endswith((".wav",".wave")): continue
                raw=z.read(fn)
                try:
                    sr,x=wavfile.read(io.BytesIO(raw)); sr,x=mono_float(sr,x)
                    rows.append({"file":fn,"wav_sha256":sha(raw),"sample_rate":sr,"channels":1 if np.asarray(x).ndim==1 else int(np.asarray(x).shape[1]),**desc(sr,x)})
                except Exception as e: rows.append({"file":fn,"wav_sha256":sha(raw),"error":str(e)})
            item["wav_count"]=len(rows); item["wavs"]=rows; success+=bool(rows)
        except Exception as e: item["error"]=str(e)
        report["sources"][name]=item
    report["successful_sources"]=int(success)
    report["passed"]=success>=2 and bool(report["sources"].get("juno60_hornet_1997",{}).get("wav_count"))
    (out/"reference_inventory.json").write_text(json.dumps(report,indent=2))
    print(json.dumps({k:{"wav_count":v.get("wav_count",0),"error":v.get("error")} for k,v in report["sources"].items()},indent=2))
    return 0 if report["passed"] else 1

if __name__=="__main__":
    ap=argparse.ArgumentParser(); ap.add_argument("--out",type=Path,required=True); a=ap.parse_args(); raise SystemExit(run(a.out))
