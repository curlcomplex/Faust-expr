"""Acquire fixed, licensed Syntakt SD Basic/Vintage public preview references.
Lossy previews with unknown settings: descriptor/coverage evidence only.
"""
from __future__ import annotations
import argparse, hashlib, json, subprocess
from datetime import datetime, timezone
from html.parser import HTMLParser
from pathlib import Path
from urllib.parse import urlparse, urljoin
from urllib.request import Request, build_opener, HTTPRedirectHandler
ALBUM='https://particlesintowaves.bandcamp.com/album/syntakt-designer-drums'
LICENSE='https://creativecommons.org/licenses/by/4.0/'
SELECTION=('SDB-01','SDB-04','SDB-07','SDB-10','SDV-01','SDV-05','SDV-09','SDV-13')
def approved(url):
    p=urlparse(url);host=(p.hostname or '').lower()
    if p.scheme!='https' or p.username or p.password or p.port not in (None,443) or (host!='particlesintowaves.bandcamp.com' and not host.endswith('.bcbits.com')):
        raise ValueError('unexpected URL '+url)
    return url
class Redirect(HTTPRedirectHandler):
    def redirect_request(self,req,fp,code,msg,headers,newurl): return super().redirect_request(req,fp,code,msg,headers,approved(newurl))
def get(url,limit):
    with build_opener(Redirect()).open(Request(approved(url),headers={'User-Agent':'Faust-expr-reference-research/1.0'}),timeout=30) as r:
        approved(r.url);data=r.read(limit+1)
        if len(data)>limit: raise ValueError('download bound')
        return data
class Parser(HTMLParser):
    def __init__(self): super().__init__(convert_charrefs=True);self.album=None
    def handle_starttag(self,tag,attrs):
        for k,v in attrs:
            if k=='data-tralbum': self.album=json.loads(v)
def sha(b): return hashlib.sha256(b).hexdigest()
def acquire(out):
    out.mkdir(parents=True,exist_ok=True);html=get(ALBUM,4_000_000);text=html.decode()
    if 'creativecommons.org/licenses/by/4.0' not in text: raise ValueError('CC BY declaration missing')
    p=Parser();p.feed(text)
    if not p.album: raise ValueError('track metadata missing')
    tracks={r['title']:r for r in p.album['trackinfo']};records=[]
    for name in SELECTION:
        tr=tracks[name];url=tr.get('file',{}).get('mp3-128')
        if not url: raise ValueError('preview missing '+name)
        if url.startswith('//'): url='https:'+url
        if url.startswith('http:'): url='https:'+url[5:]
        raw=get(url,2_000_000);mp3=out/(name+'.mp3');mp3.write_bytes(raw)
        wav=out/(name+'.wav');subprocess.run(['ffmpeg','-v','error','-y','-i',str(mp3),'-map','0:a:0','-ac','1','-ar','48000','-c:a','pcm_f32le',str(wav)],check=True,timeout=30)
        records.append({'id':name,'machine':'SD Basic' if name.startswith('SDB') else 'SD Vintage','source_page':urljoin(ALBUM,tr['title_link']),'media_url':url,'mp3_sha256':sha(raw),'wav_sha256':sha(wav.read_bytes()),'firmware':None,'parameters':None,'recording_gain':None,'drive_per_hit':None})
    report={'schema':1,'source':ALBUM,'creator':'Winston Edwards / Particles Into Waves','collection':'Syntakt Designer Drums','release_date':'2022-06-13','license':'CC BY 4.0','license_url':LICENSE,'retrieved_utc':datetime.now(timezone.utc).isoformat(),'selection_before_audio_analysis':list(SELECTION),'records':records,'limits':'Public MP3 previews decoded mono float32/48k. Creator states pack is dry/no post, with a small handful of kicks/snares using onboard drive. Exact settings, firmware and per-hit drive are unknown. Not valid for hidden algorithm or knob-curve claims.'}
    (out/'manifest.json').write_text(json.dumps(report,indent=2)+'\n')
    (out/'ATTRIBUTION.md').write_text('# Snare reference attribution\n\nSDB-01/04/07/10 and SDV-01/05/09/13 from **Syntakt Designer Drums**, Winston Edwards / Particles Into Waves, 13 June 2022.\n\nSource: '+ALBUM+'\nLicense: CC BY 4.0 ('+LICENSE+').\n\nPublic MP3 previews were decoded to mono float32/48 kHz with no gain, EQ, trim or normalization. No endorsement by the creator or Elektron is implied.\n')
    print(json.dumps({'references':len(records),'license':'CC BY 4.0','quality':'lossy preview / unknown settings'}))
if __name__=='__main__':
    a=argparse.ArgumentParser();a.add_argument('--out',type=Path,required=True);args=a.parse_args();acquire(args.out.resolve())
