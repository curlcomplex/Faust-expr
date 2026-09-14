"""Ed's TR-606 -> Akai S3000XL, 1998 archive. No reference WAV redistribution."""
from pathlib import Path
import argparse,hashlib,json,os,subprocess,tempfile
import numpy as np
from scipy.io import wavfile
from tr606_reference_discovery import features,fetch,sha
from synth_batch import SynthLab
from drums606_batch import DEFAULTS
ROOT=Path(__file__).resolve().parents[2]
URL='https://mirrors.xmission.com/aminet/mods/smpl/tr-606.lha'
NOTES='https://mirrors.xmission.com/aminet/mods/smpl/tr-606.readme'
NOTES_SHA='9bd04f9d0212ce6ebd36f260fe51646cd953523a1058a1947ebf2b40825890b2'

def run(out):
 out=Path(out);out.mkdir(parents=True,exist_ok=True);L=SynthLab(out);(out/'audition').mkdir(exist_ok=True)
 report={'commit':os.getenv('GITHUB_SHA'),'references':{},'baseline':{},'limitations':['One identified archive, recording unit serial and panel levels/tempo unknown. Akai sampler capture is not transparent laboratory measurement. Audio remains transient; no redistribution permission inferred.'],'human_approved':False}
 try:
  notes,_=fetch(NOTES,10000)
  if sha(notes)!=NOTES_SHA:raise ValueError('Reference notes changed')
  data,final=fetch(URL,2_000_000)
  report['archive']={'url':URL,'final_url':final,'bytes':len(data),'sha256':sha(data),'notes':notes.decode(),'notes_sha256':sha(notes)}
  with tempfile.TemporaryDirectory() as tmp:
   tmp=Path(tmp);pack=tmp/'archive.lha';pack.write_bytes(data)
   listing=subprocess.check_output(['lha','l',str(pack)],text=True,timeout=20)
   if '..' in listing:raise ValueError('unsafe archive listing')
   report['archive']['listing']=listing
   subprocess.run(['lha','xq',str(pack)],cwd=tmp,check=True,capture_output=True,timeout=20)
   for p in sorted(tmp.rglob('*')):
    if p.is_symlink():raise ValueError('symlink in reference archive')
    if p.suffix.lower()=='.wav':
     sr,x=wavfile.read(p);report['references'][str(p.relative_to(tmp))]={'sha256':sha(p.read_bytes()),'dtype':str(x.dtype),'frames':len(x),'features':features(x,sr)}
    elif p.suffix.lower() in ('.txt','.readme'):
     report.setdefault('documents',{})[str(p.relative_to(tmp))]=p.read_text(errors='replace')
  ex={}
  for name,p in DEFAULTS.items():
   stem='tom' if name.endswith('-tom') else name
   if stem not in ex:ex[stem]=L.build('baseline-'+stem,ROOT/'modules/drums-606/v1'/f'{stem}.dsp')
   for accent in (0,1):
    pars=p|{'accent':accent};y=L.render('baseline-'+name+'-'+str(accent),ex[stem],pars,[(4800,'gate',1),(4801,'gate',0)],frames=192000)[:,0]
    L.wav(name+'_'+str(accent)+'.wav',y)
    report['baseline'][name+'-'+str(accent)]={'source':f'modules/drums-606/v1/{stem}.dsp','settings':pars,'features':features(y,48000)}
  report['execution_ok']=True
 except Exception as e:
  report.update(execution_ok=False,error=repr(e),compiler_output=getattr(e,'output',None));raise
 finally:
  report['lab']=L.report;(out/'analysis.json').write_text(json.dumps(report,indent=2))
  print(json.dumps({'execution_ok':report.get('execution_ok'),'error':report.get('error'),'archive':report.get('archive'),'documents':report.get('documents'),'files':list(report['references'])},indent=2),flush=True)
if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('--out',required=True);run(p.parse_args().out)
