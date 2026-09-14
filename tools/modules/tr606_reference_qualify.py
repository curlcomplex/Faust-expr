"""Frozen 606 reference selections: exact sources, diagnostic matching, lifecycle and audio.
No refitting happens in this acceptance run. Reference WAVs never enter the evidence folder.
"""
from pathlib import Path
import argparse,hashlib,json,os,time
import numpy as np
from synth_batch import SynthLab
from acid_batch import controls
from drums606_batch import DEFAULTS
from tr606_reference_fit import reference_features,loss,components,MAPPING,TONAL
from tr606_reference_discovery import features
from hats_v2_delivery import digest
ROOT=Path(__file__).resolve().parents[2]
MANIFEST=ROOT/'modules/drums-606-reference/v1/selection.json'
def run(out):
 L=SynthLab(out);c=L.check;r=L.render;(L.out/'audition').mkdir(exist_ok=True)
 start=time.perf_counter();manifest=json.loads(MANIFEST.read_text());selections=manifest['selections']
 report={'commit':os.getenv('GITHUB_SHA'),'manifest_sha256':digest(MANIFEST),'human_approved':False,'host_integrated':False,'reference_comparisons':{},'phase_checks':{},'timings':{},'reference_audio_redistributed':False}
 try:
  refs,notes=reference_features();report['references']=refs;report['notes']=notes
  c('all-14-reference-hashes',len(refs)==14 and set(refs)==set(manifest['reference_hashes']) and all(refs[k]['sha256']==v for k,v in manifest['reference_hashes'].items()))
  c('legacy-files-unchanged',all(digest(ROOT/p)==h for p,h in manifest['baseline_hashes'].items()))
  c('selected-files-exact',all(digest(ROOT/s['source'])==s['source_sha256'] for s in selections.values()))
  c('six-engines-seven-articulations',len(set(s['source'] for s in selections.values()))==6 and len(selections)==7)
  ex={};vx={};oldex={};uimap={};banks=[];comparison_bank=[]
  for name,s in selections.items():
   src=ROOT/s['source'];oldstem='tom' if name.endswith('-tom') else name;oldsrc=ROOT/f'modules/drums-606/v1/{oldstem}.dsp'
   if s['source'] not in ex:
    ex[s['source']]=L.build('selected-'+src.stem,src)
    vx[s['source']]=L.build('vector-'+src.stem,src,True)
   exe=ex[s['source']];vec=vx[s['source']]
   if oldstem not in oldex:oldex[oldstem]=L.build('previous-'+oldstem,oldsrc)
   p=s['settings'];io,ui=controls(exe);uimap[name]=ui
   c(name+':note-controls',io==(0,1) and set(ui)==set(p) and {'gate','freq','velocity'}<=set(ui),actual=sorted(ui))
   c(name+':canonical-Hz-range',ui['freq'][0]>0 and ui['freq'][0]<=p['freq']<=ui['freq'][1])
   ev=[(4800,'gate',1),(4801,'gate',0)]
   report['reference_comparisons'][name]={}
   for accent in (0,1):
    old=r(name+'-previous-'+str(accent),oldex[oldstem],DEFAULTS[name]|{'accent':accent},ev,frames=192000)[:,0]
    y=r(name+'-selected-'+str(accent),exe,p|{'accent':accent},ev,frames=192000)[:,0]
    a=refs[MAPPING[name]+('acc' if accent else '')+'.wav']['features'];b=features(old,48000);d=features(y,48000)
    before=loss(a,b,name in TONAL);after=loss(a,d,name in TONAL)
    report['reference_comparisons'][name][str(accent)]={'reference':a,'previous':b,'selected':d,'before_loss':before,'after_loss':after,'component_errors':components(a,d,name in TONAL),'role':'reserved-accent validation' if accent else 'calibration'}
    c(name+':reference-improvement-'+str(accent),after<before,before=before,after=after)
    if accent==0:
     x=y;L.wav(name+'_current_then_revised.wav',np.concatenate((old,y)));L.wav(name+'_revised.wav',y)
     banks.append(y);comparison_bank.extend((old,y))
   c(name+':initial-silence',not np.any(x[:4800]))
   c(name+':audible',.003<float(abs(x).max())<1,peak=float(abs(x).max()))
   c(name+':never-triggered-silence',not np.any(r(name+'-never',exe,p,frames=48000)))
   c(name+':zero-velocity',not np.any(r(name+'-zero',exe,p|{'velocity':0},ev,frames=96000)))
   half=r(name+'-half',exe,p|{'velocity':.5},ev,frames=96000)[:,0]
   c(name+':velocity-linear',np.max(abs(half-x[:96000]*.5))<2e-6)
   held=r(name+'-held',exe,p,[(4800,'gate',1)],frames=96000)[:,0]
   c(name+':one-shot-gate-off-independent',np.array_equal(held,x[:96000]))
   for block in (1,32,127,512):
    y=r(name+'-block-'+str(block),exe,p,ev,block=block,frames=96000)[:,0]
    c(name+':block-parity-'+str(block),np.array_equal(y,x[:96000]))
   y=r(name+'-vector',vec,p,ev,frames=96000)[:,0]
   c(name+':vector-parity',np.max(abs(y-x[:96000]))<3e-5,max_absolute=float(np.max(abs(y-x[:96000]))))
   for sr in (44100,96000):
    on=round(.1*sr);r(name+'-rate-'+str(sr),exe,p,[(on,'gate',1),(on+1,'gate',0)],sr=sr,frames=sr*3)
   latches=ev+[(6000,'freq',ui['freq'][0]),(6000,'decay',ui['decay'][0]),(6000,'velocity',.2),(6000,'accent',1)]
   y=r(name+'-onset-locks',exe,p,latches,frames=96000)[:,0]
   c(name+':onset-locks',np.array_equal(y,x[:96000]))
   # Selected controls retain useful live-tail action. No retroactive transient regen.
   lp=p|{'decay':ui['decay'][1]}
   ref=r(name+'-live-reference',exe,lp,ev,frames=96000)
   colour=next(k for k in ('click','snappy','noise','metalSpread') if k in p)
   for key in ('tone',colour):
    value=0 if p[key]>=.5 else ui[key][1]
    y=r(name+'-live-'+key,exe,lp,ev+[(9000,key,value)],frames=96000)
    residual=float(np.linalg.norm(y[10000:]-ref[10000:])/(np.linalg.norm(ref[10000:])+1e-20))
    c(name+':live-'+key,residual<.001 if key=='click' else residual>.002,relative_l2=residual)
   rapid=[(4800+i*401+j,'gate',1-j) for i in range(64) for j in (0,1)]
   r(name+'-persistent-retrigger',exe,p,rapid,frames=96000)
   y=r(name+'-long-tail',exe,lp,ev,frames=576000)
   c(name+':maximum-tail-settles',abs(y[-48000:]).max()<1e-7,late_peak=float(abs(y[-48000:]).max()))
   corner=[]
   for i in range(16):
    at=4800+i*4000
    corner.extend((at,k,ui[k][(i>>j)&1]) for j,k in enumerate(('freq','decay','tone',colour)))
    corner.extend(((at,'gate',1),(at+1,'gate',0),(at,'accent',i%2)))
   y=r(name+'-corners',exe,p,corner,frames=144000)
   c(name+':bounded-corners',abs(y).max()<1.6,peak=float(abs(y).max()))
   # Onset/warmup variations include startup smoothing, not only oscillator phase.
   losses=[]
   a=refs[MAPPING[name]+'.wav']['features']
   for on in (17,10007,22931):
    y=r(name+'-phase-'+str(on),exe,p,[(on,'gate',1),(on+1,'gate',0)],frames=192000)[:,0]
    losses.append(loss(a,features(y,48000),name in TONAL))
   report['phase_checks'][name]={'onsets':[17,10007,22931],'losses':losses,'role':'post-selection onset/warmup robustness report, including near-startup smoothing; not a pure oscillator-phase test and not used to refit'}
  # OH range-only change preserves the old engine when all parameters match.
  name='open-hat';s=selections[name];exe=ex[s['source']]
  oldp=DEFAULTS[name]
  a=r('oh-old-range-original',oldex[name],oldp,[(4800,'gate',1),(4801,'gate',0)],frames=96000)
  b=r('oh-old-range-revised',exe,oldp,[(4800,'gate',1),(4801,'gate',0)],frames=96000)
  c('oh-matched-controls-sound-preservation',np.array_equal(a,b),max_abs=float(abs(a-b).max()))
  p=s['settings'];ev=[(4800,'gate',1),(4801,'gate',0)];kill=[(14400,'chokeGate',1),(14401,'chokeGate',0)]
  free=r('oh-unchoked',exe,p,ev,frames=96000);cut=r('oh-choked',exe,p,ev+kill,frames=96000)
  c('oh-choke-preserves-early-audio',np.array_equal(free[:14400],cut[:14400]))
  c('oh-choke-kills-tail',np.linalg.norm(cut[18000:])/(np.linalg.norm(free[18000:])+1e-20)<.001)
  c('oh-repeat-choke-no-resurrection',np.array_equal(cut,r('oh-repeat-choke',exe,p,ev+kill+[(22000,'chokeGate',1),(22001,'chokeGate',0)],frames=96000)))
  c('oh-simultaneous-choke-wins',not np.any(r('oh-collision',exe,p,ev+[(4800,'chokeGate',1),(4801,'chokeGate',0)],frames=48000)))
  restart=r('oh-rearmed',exe,p,ev+kill+[(28800,'gate',1),(28801,'gate',0)],frames=96000)
  c('oh-rearms-on-next-hit',abs(restart[29000:40000]).max()>.003)
  # Negative controls: silence and the wrong actual drum do not satisfy the same metric gate.
  kr=report['reference_comparisons']['kick']['0'];sn=report['reference_comparisons']['snare']['0']
  c('shape-gate-rejects-silent-candidate',loss(kr['reference'],{'silent':True},True)>kr['before_loss'])
  c('shape-gate-rejects-wrong-real-drum',loss(kr['reference'],sn['selected'],True)>kr['before_loss'])
  # The 6-source selection is sequenced as seven independent parts, never an internal kit.
  seq=L.build('trigger-seq',ROOT/'modules/trigger-seq/v1/trigger.dsp')
  sd=dict(run=1,length=16,clock=0,reset=0)|{f'step{i+1:02d}':0 for i in range(32)}
  patterns={'kick':[0,6,8,14],'snare':[4,12],'low-tom':[7,15],'high-tom':[3,10,14],'closed-hat':[0,2,4,6,8,10,12,14,15],'open-hat':[3,11],'cymbal':[0]}
  clocks=[(4800+i*6000+j,'clock',1-j) for i in range(64) for j in (0,1)]
  gates={};mixes={}
  for name,pattern in patterns.items():
   pp=sd|{f'step{i+1:02d}':1 for i in pattern}
   if name=='cymbal':pp['length']=32
   y=r(name+'-sequence',seq,pp,clocks,frames=576000)
   gates[name]=np.flatnonzero(y[:,0]>.5).tolist()
   c(name+':sequence-exact',gates[name]==[4800+i*6000 for i in range(64) if i%pp['length'] in pattern])
  for version in ('previous','revised'):
   stems={}
   for name,s in selections.items():
    p=DEFAULTS[name] if version=='previous' else s['settings']
    stem='tom' if name.endswith('-tom') else name
    exe=oldex[stem] if version=='previous' else ex[s['source']]
    ev=[]
    for n in gates[name]:
     i=(n-4800)//6000;ev.extend(((n,'gate',1),(n+1,'gate',0),(n,'velocity',.72 if i%4 else 1),(n,'accent',float(i%4==0))))
    if name=='open-hat':ev.extend((n+j,'chokeGate',1-j) for n in gates['closed-hat'] for j in (0,1))
    y=r(name+'-'+version+'-groove',exe,p,sorted(ev),frames=576000)[:,0]
    stems[name]=y
    if version=='revised':L.wav(name+'_stem.wav',y)
   mixes[version]=sum(stems.values())*.45
   L.wav(version+'_606_groove.wav',mixes[version])
  L.wav('606_groove_current_then_revised.wav',np.concatenate(list(mixes.values())))
  L.wav('all_seven_current_then_revised.wav',np.concatenate(comparison_bank))
  # Same build, repeated instrumented compute time; not process-start/file I/O or CI duration.
  for name in ('snare','open-hat','cymbal'):
   s=selections[name];exe=ex[s['source']];times=[]
   for i in range(5):
    r(name+'-timing-'+str(i),exe,s['settings'],[(4800,'gate',1),(4801,'gate',0)],frames=240000)
    times.append(L.report['renders'][-1]['diagnostics']['instrumented_compute_ns']/1e9)
   report['timings'][name]={'audio_seconds':5,'repeats':times,'median':float(np.median(times)),'sample_rate':48000,'block':128,'channels':1}
  report['audition']={'sample_rate':48000,'single_hit_pair_order':'4s previous /4s revised','groove_pair_order':'12s previous /12s revised','groove_gain':.45,'other_gain':1,'reference_audio_included':False,'processing':'None added: no normalization, EQ, compressor, limiter, reverb. Intrinsic DSP coloration retained.'}
  report['passed']=all(x['passed'] for x in L.report['checks'])
  if not report['passed']:raise AssertionError([x['name'] for x in L.report['checks'] if not x['passed']])
 except Exception as e:
  report.update(passed=False,error=repr(e),compiler_output=getattr(e,'output',None));raise
 finally:
  report['lab']=L.report;report['wall_seconds']=time.perf_counter()-start
  (L.out/'qualification.json').write_text(json.dumps(report,indent=2))
  print(json.dumps({'passed':report.get('passed',False),'checks':len(L.report['checks']),'renders':len(L.report['renders']),'builds':len(L.report['builds']),'failed':[x['name'] for x in L.report['checks'] if not x['passed']],'error':report.get('error')}),flush=True)
if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('--out',required=True);run(p.parse_args().out)
