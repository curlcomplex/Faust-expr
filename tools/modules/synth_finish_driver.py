"""Finishing-suite entry point: preserve subprocess errors and source availability."""
from pathlib import Path
import argparse, json, re, subprocess, traceback
from synth_finish_materials import fetch
from hats_v2_delivery import command as original_command
import synth_batch
import synth_finish


def main(out):
    out.mkdir(parents=True,exist_ok=True)
    pages={}
    for name,url in [('dms','https://www.mediafire.com/?b0v9zou2dvd4qpv'),
        ('minimoog','https://legowelt.wetransfer.com/downloads/f1471b9a8a28ab96a7a8b7b4ee28332020180329170955/1b03c9')]:
        try:
            raw,final=fetch(url,limit=5_000_000)
            text=raw.decode('utf-8','replace');(out/(name+'-public-page.html')).write_text(text)
            links=re.findall(r'https?://[^\s\"<>]+',text)
            pages[name]={'url':final,'links':sorted(set(t for t in links if '.zip' in t or 'download' in t))[:30]}
        except Exception as e:pages[name]={'url':url,'error':str(e)}
    (out/'download-pages.json').write_text(json.dumps(pages,indent=2))
    print('PUBLIC_REFERENCE_PAGES',json.dumps(pages),flush=True)
    def command(args):
        try:return original_command(args)
        except subprocess.CalledProcessError as e:
            text=str(e.output)
            (out/'failed-subprocess-output.txt').write_text(text)
            print('SUBPROCESS_DIAGNOSTIC',text,flush=True)
            raise
    synth_batch.command=command
    synth_finish.command=command
    return synth_finish.run(out)

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--out',required=True,type=Path)
    a=p.parse_args();raise SystemExit(main(a.out))
