"""Frozen 909 selection qualification. Does not optimise or change fit settings.
Native Faust renders are compared with dry hardware shape descriptors, not waveform nulls.
"""
from pathlib import Path
import argparse,hashlib,json,os,time,subprocess,itertools
import numpy as np
from scipy.io import wavfile
from synth_batch import SynthLab
from acid_batch import controls
from drums909_batch import DEFAULTS,PRESETS
from hats_v2_delivery import command,digest
from tr909_reference import acquire,features,loss,components,ANCHORS,TONAL,source
ROOT=Path(__file__).resolve().parents[2]
SELECTION=ROOT/'modules/drums-909-reference/v1/selection.json'
LATCHED={'freq','velocity','accent','decay','pitchAmount','pitchDecay','noiseDecay','bend','spacing'}

def run(out):
 L=SynthLab(out);r=L.render;c=L.check;out=L.out;(out/'audition').mkdir(exist_ok=True);start=time.perf_counter()
 sel=json.loads(SELECTION.read_text());L.report.update(commit=os.getenv('GITHUB_SHA'),selection=sel,hardware_approved=False,human_approved=False,host_integrated=False,comparisons={},heldout={},limitations=['Single original unit; sampler capture and per-file normalization','Seven fitted anchors, not a calibrated full panel map','Restricted reference recordings not redistributed','No runtime 909 metal assets provided'])
 try:
  refs=acquire(sel['reference_archive_sha256']);(out/'reference-index.json').write_text(json.dumps(refs,indent=2))
  c('reference-files',len(refs['recordings'])==160 and refs['unique_recordings']==159)
  for n,h in sel['reference_hashes'].items():c('reference-hash-'+n,refs['recordings'][n]['sha256']==h)
  ex={};vec={};uis={};baselines={};revised={};old={}
  for name,entry in sel['selections'].items():
   src=ROOT/entry['source'];c(name+'-source-hash',digest(src)==entry['source_sha256'])
   if src not in ex:
    k=src.stem;ex[src]=L.build('selected-'+k,src);vec[src]=L.build('selected-'+k+'-vector',src,True);uis[src]=controls(ex[src])
   io,ui=uis[src];p=entry['settings'];exe=ex[src];vexe=vec[src];oldsrc=source(name)
   if oldsrc not in old:old[oldsrc]=exe if oldsrc==src else L.build('previous-'+oldsrc.stem,oldsrc)
   c(name+'-single-note-io',io==(0,1));c(name+'-canonical-controls',{'gate','freq','velocity'}<=set(ui))
   c(name+'-complete-settings',set(p)==set(ui),actual=sorted(ui),selected=sorted(p))
   c(name+'-in-ranges',all(ui[k][0]<=v<=ui[k][1] for k,v in p.items()))
   ev=[(4800,'gate',1),(4801,'gate',0)]
   a=r(name+'-previous',old[oldsrc],DEFAULTS[name],ev,frames=192000)[:,0];b=r(name+'-selected',exe,p,ev,frames=192000)[:,0]
   baselines[name]=a;revised[name]=b
   af=features(a,48000);bf=features(b,48000);rf=refs['recordings'][entry['reference']]['features']
   la=loss(rf,af,name in TONAL);lb=loss(rf,bf,name in TONAL)
   L.report['comparisons'][name]={'reference':entry['reference'],'previous':af,'revised':bf,'reference_features':rf,'old_loss':la,'new_loss':lb,'components_previous':components(rf,af,name in TONAL),'components_revised':components(rf,bf,name in TONAL)}
   c(name+'-improved-anchor',lb<la*.95,previous=la,revised=lb)
   c(name+'-audible',.003<float(abs(b).max())<1,peak=float(abs(b).max()))
   c(name+'-pre-onset-silence',not np.any(b[:4800]))
   c(name+'-never-triggered',not np.any(r(name+'-never',exe,p,frames=24000)))
   c(name+'-velocity-zero',not np.any(r(name+'-zero',exe,p|{'velocity':0},ev,frames=96000)))
   half=r(name+'-half',exe,p|{'velocity':.5},ev,frames=96000)[:,0];c(name+'-velocity-linear',float(abs(half-.5*b[:96000]).max())<2e-6)
   held=r(name+'-held',exe,p,[(4800,'gate',1)],frames=96000)[:,0];c(name+'-held-same-one-shot',np.array_equal(held,b[:96000]))
   for block in (1,32,127,512):
    y=r(name+'-block'+str(block),exe,p,ev,block=block,frames=96000)[:,0];c(name+'-block-equality'+str(block),np.array_equal(y,b[:96000]),maximum=float(abs(y-b[:96000]).max()))
   y=r(name+'-vector',vexe,p,ev,frames=96000)[:,0];c(name+'-vector-parity',float(abs(y-b[:96000]).max())<1e-4,maximum=float(abs(y-b[:96000]).max()))
   for sr in (44100,96000):
    y=r(name+'-sr'+str(sr),exe,p,[(round(.1*sr),'gate',1),(round(.1*sr)+1,'gate',0)],sr=sr,frames=sr*2)[:,0]
    yf=features(y,sr);c(name+'-rate-bounds'+str(sr),float(abs(y).max())<1);L.report.setdefault('sample_rate_shapes',{})[name+str(sr)]=yf
   tailpars=p|{k:ui[k][1] for k in ('decay','noiseDecay') if k in ui}
   y=r(name+'-max-tail',exe,tailpars,ev,frames=48000*16)[:,0];c(name+'-tail-ends',float(abs(y[-48000:]).max())<1e-7,tail_peak=float(abs(y[-48000:]).max()))
   later=ev+[(6000,k,ui[k][1]) for k in p if k in LATCHED]
   y=r(name+'-latched',exe,p,later,frames=96000)[:,0];c(name+'-note-controls-latched',np.array_equal(y,b[:96000]))
   live=ev+[(8000,k,ui[k][0]) for k in p if k not in LATCHED|{'gate'}]
   y=r(name+'-live',exe,p,live,frames=96000)[:,0];c(name+'-live-parameters-act',np.linalg.norm(y[9000:]-b[9000:96000])>1e-5)
   rapid=[(4800+i*377+j,'gate',1-j) for i in range(64) for j in (0,1)]
   y=r(name+'-rapid',exe,p,rapid,frames=96000)[:,0];c(name+'-rapid-bounds',float(abs(y).max())<2)
   for high in (False,True):
    pars={k:(ui[k][1 if high else 0] if k!='gate' else 0) for k in p};pars['velocity']=1;pars['level']=p['level']
    y=r(name+'-corner'+str(int(high)),exe,pars,ev,frames=96000)[:,0];c(name+'-corner-bounds'+str(int(high)),float(abs(y).max())<4)
   # Cold-onset/phase controls are diagnostics; smoothing start state is not refitted away.
   for offset in (17,13711):
    y=r(name+'-onset'+str(offset),exe,p,[(offset,'gate',1),(offset+1,'gate',0)],frames=96000)[:,0]
    L.report.setdefault('onset_sensitivity',{})[name+str(offset)]={'onset_frame':offset,'features':features(y,48000),'loss':loss(rf,features(y,48000),name in TONAL)}
   L.wav(name+'_previous_then_revised.wav',np.r_[a,b]);L.wav(name+'_revised.wav',b)
   # A real mismatched voice and silence must fail the same candidate-vs-old improvement predicate.
   c(name+'-rejects-silence',not (loss(rf,features(np.zeros(48000),48000),name in TONAL)<la*.95))
  c('five-engines-seven-articulations',len(ex)==5 and len(sel['selections'])==7)
  for name,other in [('kick','clap'),('clap','kick'),('low-tom','high-tom')]:
   target=refs['recordings'][sel['selections'][name]['reference']]['features'];wrong=loss(target,features(revised[other],48000),name in TONAL)
   c(name+'-wrong-voice-rejected',not (wrong<L.report['comparisons'][name]['old_loss']*.95),wrong=wrong)
  # Preselected second captures checked only after frozen fits, not separate-unit validation.
  for name,ref in [('rim','RIM63.WAV'),('clap','HANDCLP2.WAV')]:
   rf=refs['recordings'][ref]['features'];a=features(baselines[name],48000);b=features(revised[name],48000);la=loss(rf,a,name in TONAL);lb=loss(rf,b,name in TONAL)
   L.report['heldout'][name]={'reference':ref,'old_loss':la,'new_loss':lb,'reference_features':rf,'note':'same unit, different captured hit/velocity; normalized, not gain law'}
   c(name+'-other-hit-not-worse',lb<=la,previous=la,revised=lb)
  # Every legacy Tom preset is recalled through the one consolidated kernel.
  for n in ('low-tom','mid-tom','high-tom'):
   e=L.build('legacy-'+n,ROOT/'modules/drums-909/v1'/f'{n}.dsp')
   for preset,override in PRESETS[n].items():
    p=DEFAULTS[n]|override;ev=[(4800,'gate',1),(4801,'gate',0),(16117,'gate',1),(16118,'gate',0)]
    a=r(n+'-'+preset+'-slot',e,p,ev,frames=48000)[:,0];b=r(n+'-'+preset+'-shared',ex[source(n)],p,ev,frames=48000)[:,0]
    c(n+'-'+preset+'-exact-recall',np.array_equal(a,b),maximum=float(abs(a-b).max()))
  seq=L.build('trigger-seq',ROOT/'modules/trigger-seq/v1/trigger.dsp');sd=dict(run=1,length=16,clock=0,reset=0)|{f'step{i+1:02d}':0 for i in range(32)}
  patterns={'kick':[0,4,8,12],'snare':[4,12],'low-tom':[7,15],'mid-tom':[6,14],'high-tom':[3,10],'rim':[2,6,10,14],'clap':[4,12]}
  clocks=[(4800+i*6000+j,'clock',1-j) for i in range(64) for j in (0,1)];stems={'previous':{},'revised':{}}
  for n,pattern in patterns.items():
   z=r(n+'-seq',seq,sd|{f'step{i+1:02d}':1 for i in pattern},clocks,frames=576000);hits=np.flatnonzero(z[:,0]>.5).tolist()
   c(n+'-seq-exact',hits==[4800+i*6000 for i in range(64) if i%16 in pattern]);ev=[]
   for at in hits:
    i=(at-4800)//6000;ev.extend([(at,'gate',1),(at+1,'gate',0),(at,'velocity',.72 if i%4 else 1),(at,'accent',float(i%4==0))])
   for label,p,e in [('previous',DEFAULTS[n],old[source(n)]),('revised',sel['selections'][n]['settings'],ex[ROOT/sel['selections'][n]['source']])]:
    z=r(n+'-'+label+'-groove',e,p,ev,frames=576000)[:,0];stems[label][n]=z;L.wav(n+'_'+label+'_stem.wav',z)
  a=.4*sum(stems['previous'].values());b=.4*sum(stems['revised'].values());L.wav('groove_previous_then_revised.wav',np.r_[a,b]);L.wav('groove_revised.wav',b)
  L.wav('all_previous_then_revised.wav',np.concatenate([np.r_[baselines[n],revised[n]] for n in ANCHORS]))
  L.report['audition']={'order':list(ANCHORS),'pair_seconds':[4,4],'groove_seconds':[12,12],'gain':.4,'normalization':False,'hardware_wavs_included':False,'sampler_metal_voices_included':False}
  L.report['passed']=all(t['passed'] for t in L.report['checks'])
  if not L.report['passed']:raise AssertionError([t['name'] for t in L.report['checks'] if not t['passed']])
 except Exception as e:L.report.update(passed=False,error=repr(e),output=getattr(e,'output',None));raise
 finally:
  L.report['suite_wall_seconds']=time.perf_counter()-start;L.report['compute_seconds']=sum(t['diagnostics']['instrumented_compute_ns'] for t in L.report['renders'])/1e9
  L.report['sources']={str(p.relative_to(ROOT)):digest(p) for p in (ROOT/'modules/drums-909/v1').iterdir() if p.is_file()};L.report['renderer_sha256']=digest(ROOT/'tools/modules/render.cpp');L.report['driver_sha256']=digest(__file__)
  (out/'results.json').write_text(json.dumps(L.report,indent=2));print(json.dumps({'passed':L.report.get('passed'),'checks':len(L.report['checks']),'renders':len(L.report['renders']),'builds':len(L.report['builds']),'error':L.report.get('error')}),flush=True)
if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('--out',required=True);run(p.parse_args().out)
