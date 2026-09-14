"""Bounded fitting against seven unaccented recordings; accented files are validation only.
All generated sound is the existing render.cpp consuming actual Faust-generated C++.
The objective is a diagnostic shape distance, NOT a hardware authenticity score.
"""
from pathlib import Path
import argparse,hashlib,json,math,os,subprocess,tempfile,time
import numpy as np
from scipy.io import wavfile
from scipy.optimize import differential_evolution
from tr606_reference_discovery import features,fetch,sha
from tr606_archive_analysis import URL,NOTES,NOTES_SHA
from drums606_batch import DEFAULTS
from synth_batch import SynthLab
from hats_v2_delivery import command,digest
ROOT=Path(__file__).resolve().parents[2]
ARCHIVE_SHA='8467ac4c3d3e3d1d298e8b5d3bb567a10b4ea3d99e6f37c5010833d6bc8da59a'
MAPPING={'kick':'606bass','snare':'606snare','low-tom':'606ltom','high-tom':'606htom','closed-hat':'606chat','open-hat':'606ohat','cymbal':'606cymbal'}
TONAL={'kick','snare','low-tom','high-tom'}
REFDIR='modules/drums-606-reference/v1/'
BASIS={
 'kick':dict(freq=61.22312,decay=.264360,tone=.390777,click=.106453),
 'snare':dict(freq=195,decay=.15,tone=.5,snappy=.35),
 'low-tom':dict(freq=137.55581,decay=.299480,tone=.386120,noise=.021917),
 'high-tom':dict(freq=207.57604,decay=.205787,tone=.162234,noise=.102346),
 'closed-hat':dict(freq=428.58367,decay=.115452,tone=.976658,metalSpread=.395506),
 'open-hat':dict(freq=459.55743,decay=2.6,tone=.950861,metalSpread=.578413),
 'cymbal':dict(freq=435.09093,decay=1.8,tone=.978252,metalSpread=.447787)}
BOUNDS={
 'snare':{'freq':(180,210),'decay':(.09,.22),'tone':(0,1),'snappy':(.04,.9)},
 'open-hat':{'freq':(405,485),'decay':(2.1,3.6),'tone':(.4,1),'metalSpread':(.35,.65)},
 'cymbal':{'freq':(405,485),'decay':(.8,3),'tone':(0,1),'metalSpread':(.35,.65)}}
def components(a,b,tonal=False):
 m={k+'_log_error':abs(math.log(max(1e-5,b[k])/max(1e-5,a[k]))) for k in ('t50','t90','t99')}
 for s in ('attack','body'):
  m[s+'_spectral_distance']=float(np.linalg.norm(np.sqrt(a[s]['bands'])-np.sqrt(b[s]['bands']))/math.sqrt(2))
  m[s+'_centroid_log_error']=abs(math.log(max(1,b[s]['centroid'])/max(1,a[s]['centroid'])))
 if tonal:m['body_peak_log_error']=abs(math.log(b['body']['peaks'][0][0]/a['body']['peaks'][0][0]))
 return m
def loss(a,b,tonal=False):
 if b.get('silent',False):return 1e6
 m=components(a,b,tonal)
 return sum((.4 if k.startswith(('t50','t90','t99')) else .2 if 'centroid' in k else 1 if 'spectral' in k else .7)*v for k,v in m.items())/(3*.4+2*1+2*.2+(.7 if tonal else 0))
def reference_features():
 notes,_=fetch(NOTES,10000)
 if sha(notes)!=NOTES_SHA:raise ValueError('Reference notes changed')
 data,_=fetch(URL,2_000_000)
 if sha(data)!=ARCHIVE_SHA:raise ValueError('Reference archive changed')
 refs={}
 with tempfile.TemporaryDirectory() as tmp:
  tmp=Path(tmp);pack=tmp/'archive.lha';pack.write_bytes(data)
  subprocess.run(['lha','xq',str(pack)],cwd=tmp,check=True,capture_output=True,timeout=20)
  for name in MAPPING.values():
   for suff in ('','acc'):
    p=tmp/(name+suff+'.wav')
    if p.is_symlink() or not p.is_file():raise ValueError('Missing exact reference '+p.name)
    sr,x=wavfile.read(p)
    if x.dtype!=np.int16 or x.ndim!=1 or sr!=44100:raise ValueError('Unexpected PCM format')
    refs[p.name]={'sha256':sha(p.read_bytes()),'frames':len(x),'features':features(x,sr)}
 return refs,notes.decode()
def source(name,new=True):
 stem='tom' if name.endswith('-tom') else name
 return ROOT/((REFDIR if new and name in BOUNDS else 'modules/drums-606/v1/')+stem+'.dsp')
def run(out):
 out=Path(out);out.mkdir(parents=True,exist_ok=True);(out/'audition').mkdir(exist_ok=True)
 L=SynthLab(out);c=L.check;r=L.render
 report={'commit':os.getenv('GITHUB_SHA'),'archive_sha256':ARCHIVE_SHA,'source_url':URL,'notes_url':NOTES,'notes_sha256':NOTES_SHA,'reference_audio_redistributed':False,'selection':{},'comparisons':{},'search':{},'human_approved':False,'host_integrated':False}
 try:
  refs,notes=reference_features();report['references']=refs;report['source_notes']=notes
  c('all-seven-unaccented-plus-accented-references',len(refs)==14)
  ex={};oldex={};bank=[]
  for name in MAPPING:
   stem='tom' if name.endswith('-tom') else name
   if stem not in oldex:oldex[stem]=L.build('old-'+stem,source(name,False))
   ex[name]=L.build('candidate-'+name,source(name)) if name in BOUNDS else oldex[stem]
   pars=DEFAULTS[name]|BASIS[name];target=refs[MAPPING[name]+'.wav']['features']
   if name in BOUNDS:
    keys=list(BOUNDS[name]);evaluations=[];score=out/'search.tsv';raw=out/'search.f32';exe=ex[name]
    def objective(v):
     p=pars|dict(zip(keys,map(float,v)));rows={(0,k):value for k,value in p.items()};rows[4800,'gate']=1;rows[4801,'gate']=0
     score.write_text(''.join(f'{n}\t{k}\t{v:.9g}\n' for (n,k),v in sorted(rows.items())))
     diag=json.loads(command([exe,score,raw,48000,128,144000,0]));x=np.fromfile(raw,'<f4')
     if diag['channels']!=1 or len(x)!=144000 or not np.isfinite(x).all():raise ValueError('Invalid search render')
     f=features(x,48000);err=loss(target,f,name in TONAL)
     evaluations.append({'settings':p,'loss':err,'raw_sha256':digest(raw),'compute_seconds':diag.get('compute_seconds')})
     return err
    t=time.perf_counter();opt=differential_evolution(objective,list(BOUNDS[name].values()),seed=606,popsize=6,maxiter=14,tol=.005,polish=False)
    pars|=dict(zip(keys,map(float,opt.x)));report['search'][name]={'seed':606,'bounds':BOUNDS[name],'evaluations':evaluations,'wall_seconds':time.perf_counter()-t,'raw_policy':'Search scratch audio replaced per trial; hashes/logs retained, selected final raw evidence retained separately.'}
    score.unlink(missing_ok=True);raw.unlink(missing_ok=True)
   report['selection'][name]={'source':str(source(name).relative_to(ROOT)),'settings':pars}
   report['comparisons'][name]={}
   for accent in (0,1):
    p0=DEFAULTS[name]|{'accent':accent};p1=pars|{'accent':accent}
    ev=[(4800,'gate',1),(4801,'gate',0)]
    old=r(name+'-old-'+str(accent),oldex[stem],p0,ev,frames=192000)[:,0]
    new=r(name+'-new-'+str(accent),ex[name],p1,ev,frames=192000)[:,0]
    a=refs[MAPPING[name]+('acc' if accent else '')+'.wav']['features'];b=features(old,48000);d=features(new,48000)
    metrics={'reference':a,'previous':b,'candidate':d,'previous_loss':loss(a,b,name in TONAL),'candidate_loss':loss(a,d,name in TONAL),'candidate_components':components(a,d,name in TONAL),'role':'validation-only' if accent else 'calibration'}
    report['comparisons'][name][str(accent)]=metrics
    c(name+'-'+str(accent)+'-shape-improves',metrics['candidate_loss']<metrics['previous_loss'],previous=metrics['previous_loss'],candidate=metrics['candidate_loss'])
    if accent==0:
     L.wav(name+'_current_then_candidate.wav',np.concatenate((old,new)));bank.extend((old,new))
     L.wav(name+'_candidate.wav',new)
   print(name,json.dumps({'settings':pars,'calibration_loss':report['comparisons'][name]['0']['candidate_loss'],'validation_loss':report['comparisons'][name]['1']['candidate_loss']}),flush=True)
  L.wav('all_seven_current_then_candidate.wav',np.concatenate(bank))
  report['execution_ok']=True
 except Exception as e:
  report.update(execution_ok=False,error=repr(e),compiler_output=getattr(e,'output',None));raise
 finally:
  report['lab']=L.report;(out/'fit.json').write_text(json.dumps(report,indent=2));(out/'selected-presets.json').write_text(json.dumps(report['selection'],indent=2))
if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('--out',required=True);run(p.parse_args().out)
