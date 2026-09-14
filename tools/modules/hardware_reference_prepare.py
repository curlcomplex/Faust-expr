"""Acquire identified hardware references and export analysis, not restricted audio.

Publisher-hosted MP3s are analyzed transiently; deliver their original links.
The Soundwave archive explicitly permits free distribution and travels with its
original notes. Public Freesound previews are only exported after verifying CC0.
No login, checkout or download gate is bypassed. No synth audio is made in Python.
"""
from pathlib import Path
import hashlib, html, io, json, os, re, subprocess, tempfile, urllib.request, zipfile
import numpy as np
import soundfile as sf
from scipy.signal import stft, resample_poly
from synth_batch import SynthLab, ROOT

OUT=ROOT/'build/hardware-prepare'
SOURCES={
 'juno60-bass1':'https://www.synthmania.com/Roland%20Juno-60/Audio/Factory%20presets%20group%201/31%20Bass%201.mp3',
 'juno60-bass2':'https://www.synthmania.com/Roland%20Juno-60/Audio/Factory%20presets%20group%201/32%20Bass%202.mp3',
 'sh101-electric-bass':'https://www.synthmania.com/Roland%20SH-101/Audio/Factory%20Patches/08%20ELECTRIC%20BASS%20GUITAR.mp3',
 'sh101-funky-bass':'https://www.synthmania.com/Roland%20SH-101/Audio/Factory%20Patches/27%20FUNKY%20BASS.mp3',
}
FREESOUND={
 'juno60-pulse': 'https://freesound.org/people/jessepash/sounds/167141/',
 'sh101-siren': 'https://freesound.org/people/leonseptavaux/sounds/716142/',
}
ARCHIVE='https://files.scene.org/get/mirrors/hornet/music/samples/swjuno2.zip'
EXPECTED='b671f055d388d739a65bb4ec0279d3eabd064e729212dda19d93d25fbc69f5f9'

def fetch(url,limit=60000000):
    req=urllib.request.Request(url,headers={'User-Agent':'Mozilla/5.0 (CURLOP reference research)'})
    with urllib.request.urlopen(req,timeout=45) as r: data=r.read(limit+1)
    if len(data)>limit: raise ValueError('size limit exceeded')
    return data

def sha(data): return hashlib.sha256(data).hexdigest()

def decode(data):
    with tempfile.TemporaryDirectory() as d:
        inp=Path(d)/'input'; output=Path(d)/'audio.wav'; inp.write_bytes(data)
        subprocess.run(['ffmpeg','-hide_banner','-loglevel','error','-y','-i',str(inp),'-acodec','pcm_f32le',str(output)],check=True,timeout=30)
        x,sr=sf.read(output,always_2d=True)
    if not np.isfinite(x).all(): raise ValueError('bad audio')
    return x,int(sr)

def features(tag,data,source,extra=None):
    x,sr=decode(data); mono=x.mean(axis=1)
    target_rate=24000
    import math
    g=math.gcd(sr,target_rate)
    y=resample_poly(mono,target_rate//g,sr//g).astype(np.float32)
    n=len(y); hop=240
    starts=np.arange(0,n,hop)
    rms=np.array([np.sqrt(np.mean(y[a:min(a+hop,n)].astype(float)**2)) for a in starts])
    # Harmonic/pitch descriptors preserve no phase and are not redistributed sound.
    specs={}
    for size in (256,1024,4096):
        f,t,z=stft(y,target_rate,nperseg=size,noverlap=size-hop if size>hop else 16,boundary='zeros',padded=True)
        specs[f'mag{size}']=abs(z).astype(np.float32)
        specs[f'time{size}']=t.astype(np.float32)
    pitch=[]; quality=[]
    from synth_sample_probe import estimate_pitch
    for center in starts:
        a=max(0,center-1920); b=min(n,center+1920)
        try: p,q=estimate_pitch(y[a:b],target_rate)
        except Exception:p,q=0,0
        pitch.append(p);quality.append(q)
    dest=OUT/'descriptors'/f'{tag}.npz'
    np.savez_compressed(dest,sr=target_rate,frames=n,time=starts/target_rate,rms=rms,
                        pitch=pitch,quality=quality,**specs)
    info=dict(id=tag,url=source,source_sha256=sha(data),source_rate=sr,
        source_channels=x.shape[1],duration_seconds=len(x)/sr,
        peak=float(abs(x).max()),rms=float(np.sqrt(np.mean(x*x))),
        stereo_difference_ratio=float(np.linalg.norm(x[:,0]-x[:,-1])/(np.linalg.norm(x)+1e-20)),
        descriptors=str(dest.relative_to(OUT)),reference_kind='real hardware recording; panel/capture levels not fully known',
        **(extra or {}))
    print('REFERENCE',json.dumps(info),flush=True)
    return info

def main():
    (OUT/'descriptors').mkdir(parents=True,exist_ok=True)
    (OUT/'redistributable-references').mkdir(exist_ok=True)
    records=[];errors=[]
    for tag,url in SOURCES.items():
        try: records.append(features(tag,fetch(url),url,dict(audio_exported=False,rights='Publisher-hosted demonstration: original audio not repackaged; use direct URL.')))
        except Exception as e: errors.append(dict(id=tag,error=str(e))); print('ACQUISITION_ERROR',tag,str(e),flush=True)
    try:
        raw=fetch(ARCHIVE)
        if sha(raw)!=EXPECTED: raise ValueError('Soundwave archive identity changed')
        z=zipfile.ZipFile(io.BytesIO(raw)); notes=z.read('SWJUNO2.TXT')
        if b'Distribute these samples freely' not in notes: raise ValueError('expected distribution permission missing')
        (OUT/'redistributable-references'/'SWJUNO2.TXT').write_bytes(notes)
        for index in (3,4,5,6,19,20):
            fn=f'JUNO2-{index}.WAV'; data=z.read(fn)
            tag=f'juno106-sw{index}'
            (OUT/'redistributable-references'/f'{tag}.wav').write_bytes(data)
            records.append(features(tag,data,ARCHIVE,dict(archive_sha256=EXPECTED,archive_member=fn,
                audio_exported=True,rights='Chris Gould / Soundwave: Distribute these samples freely; original notes included.',
                chorus_status='explicitly NO CHORUS' if index in (3,4) else 'not documented for this take',
                patch='A47' if index in (3,4) else 'see SWJUNO2.TXT')))
    except Exception as e: errors.append(dict(id='soundwave',error=str(e)));print('ACQUISITION_ERROR','soundwave',str(e),flush=True)
    for tag,page in FREESOUND.items():
        try:
            text=fetch(page,4000000).decode('utf-8')
            if not ('creativecommons.org/publicdomain/zero/' in text or 'Creative Commons 0' in text): raise ValueError('CC0 not verified')
            urls=re.findall(r'(?:https?:)?//(?:cdn\.)?freesound\.org/(?:data/)?previews/[^\s\"<>]+',html.unescape(text).replace('\\/','/'))
            urls=[u for u in urls if '-hq.' in u and ('.mp3' in u or '.ogg' in u)]
            if not urls: raise ValueError('no public high-quality preview found')
            url=urls[0].split("'")[0]
            if url.startswith('//'):url='https:'+url
            data=fetch(url); ext='.ogg' if '.ogg' in url else '.mp3'
            records.append(features(tag,data,url,dict(page=page,audio_exported=True,
                rights='CC0 verified on source page; public lossy preview, not gated original WAV.',
                source_format='public preview, lossy')))
            (OUT/'redistributable-references'/(tag+ext)).write_bytes(data)
        except Exception as e: errors.append(dict(id=tag,error=str(e)));print('ACQUISITION_ERROR',tag,str(e),flush=True)
    lab=SynthLab(OUT/'compiled')
    paths={'juno60-v1':'juno-60/v1','juno60-v2':'juno-60/v2',
        'juno106-v1':'juno-106/v1','juno106-v2':'juno-106/v2',
        'sh101-v1':'mono-101/v1','sh101-v3':'mono-101/v3'}
    for tag,path in paths.items():
        print('BUILD',tag,flush=True);lab.build(tag,ROOT/'modules'/path/'voice.dsp')
    for f in (OUT/'compiled').rglob('render'): f.unlink()
    result=dict(commit=os.getenv('GITHUB_SHA','local'),references=records,errors=errors,builds=lab.report['builds'],
        status='Acquisition/descriptors only. No fit or sonic approval.',hardware_approved=False)
    (OUT/'inventory.json').write_text(json.dumps(result,indent=2))
    print('PREPARED',len(records),'references',len(paths),'builds',flush=True)
    if not any(r['id']=='juno106-sw3' for r in records):return 1
    return 0
if __name__=='__main__':raise SystemExit(main())
