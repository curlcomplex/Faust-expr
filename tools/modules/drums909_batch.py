"""Seven real Faust 909-oriented voices plus an explicitly non-musical asset spike.
Whole-instrument hardware/oracle matching and four sampled articulations remain pending.
"""
from pathlib import Path
import argparse, itertools, json, os, platform, time
import numpy as np
from synth_batch import SynthLab
from acid_batch import controls
from hats_v2_delivery import command, digest
ROOT=Path(__file__).resolve().parents[2]
SRC=ROOT/'modules/drums-909/v1'
DEFAULTS=json.loads((SRC/'defaults.json').read_text())
MANIFEST=json.loads((SRC/'manifest.json').read_text())
PRESETS={
 'kick':{'Classic':{},'Short':dict(decay=.20,attack=.35),'Deep':dict(freq=39,decay=1.5,pitchAmount=.35),'Driven':dict(drive=.8,tone=.8,pitchDecay=.04)},
 'snare':{'Classic':{},'Tight':dict(decay=.12,noiseDecay=.07,snappy=.8),'Body':dict(snappy=.12,freq=160,decay=.48),'Sizzle':dict(noiseDecay=.65,snappy=.9,tone=.85)},
 'low-tom':{'Classic':{},'Dry':dict(noise=0,decay=.25),'Deep':dict(freq=63,decay=1.1),'Sweep':dict(bend=.9,drive=.3,tone=.8)},
 'mid-tom':{'Classic':{},'Dry':dict(noise=0,decay=.22),'Deep':dict(freq=106,decay=.7),'Sweep':dict(bend=.9,drive=.3,tone=.8)},
 'high-tom':{'Classic':{},'Dry':dict(noise=0,decay=.17),'Tuned':dict(freq=320,decay=.45),'Sweep':dict(bend=.9,drive=.3,tone=.8)},
 'rim':{'Classic':{},'Short':dict(decay=.025,snap=.1),'Wood':dict(freq=320,tone=.15),'Metal':dict(freq=820,tone=.9,drive=.5)},
 'clap':{'Classic':{},'Fast':dict(spacing=.004,decay=.10,tail=.25),'Wide':dict(spacing=.020,tail=.6),'Tail':dict(decay=.65,tail=.9,tone=.25)},
}

def run(out):
 L=SynthLab(out); c=L.check; r=L.render; L.out.joinpath('audition').mkdir(exist_ok=True)
 L.report.update(version='909-core-0.1.0-experiment',commit=os.environ.get('GITHUB_SHA','local-snapshot'),defaults=DEFAULTS,presets=PRESETS,
  human_approved=False,hardware_approved=False,host_integrated=False,device_qualified=False,
  asset_pending=['closed-hat','open-hat','crash','ride'],reference_comparison='Only two extracted Plaits component curves are compared; whole instruments deferred.')
 L.report['environment']=dict(platform=platform.platform(),machine=platform.machine(),faust=command([os.getenv('FAUST','faust'),'--version']),cxx=command([os.getenv('CXX','c++'),'--version']))
 start=time.perf_counter()
 try:
  exes={};uis={};notes={};isolation={}
  for name,p in DEFAULTS.items():
   print('QUALIFY',name,flush=True)
   src=SRC/(name+'.dsp');exe=L.build(name,src);exes[name]=exe
   vec=L.build(name+'-vector',src,True)
   io,ui=controls(exe);uis[name]=ui
   c(name+':single-note-io',io==(0,1));c(name+':exact-controls',set(ui)==set(p))
   c(name+':defaults',all(abs(ui[k][2]-v)<1e-5 for k,v in p.items()))
   c(name+':canonical-roles',{'gate','freq','velocity'}<=set(ui) and ui['gate'][:2]==[0.,1.] and ui['freq'][0]>0)
   ev=[(480,'gate',1),(481,'gate',0)]
   x=r(name+'-default',exe,p,ev,frames=144000)[:,0];notes[name]=x
   peak=float(abs(x).max());c(name+':audible',.005<peak<1,peak=peak)
   c(name+':pre-onset-silent',not np.any(x[:480]))
   c(name+':never-triggered',not np.any(r(name+'-never',exe,p,frames=24000)))
   c(name+':zero-velocity',not np.any(r(name+'-zero',exe,p|{'velocity':0},ev,frames=24000)))
   half=r(name+'-half',exe,p|{'velocity':.5},ev,frames=48000)[:,0]
   c(name+':linear-velocity',abs(half-.5*x[:48000]).max()<2e-6)
   held=r(name+'-held',exe,p,[(480,'gate',1)],frames=48000)[:,0]
   c(name+':held-is-one-hit',np.array_equal(held,x[:48000]))
   latched=['freq','velocity','accent']+[k for k,v in MANIFEST['implemented_synthesis'][name]['controls'].items() if v['policy']=='onset-latched']
   y=r(name+'-latch',exe,p,ev+[(4000,k,ui[k][1]) for k in latched],frames=48000)[:,0]
   c(name+':onset-latching',np.array_equal(y,x[:48000]))
   ac=r(name+'-accent',exe,p|{'accent':1},ev,frames=48000)[:,0]
   c(name+':accent-energy',np.linalg.norm(ac)>np.linalg.norm(x[:48000])*1.05)
   for block in (1,32,127,512):
    z=r(name+'-block-'+str(block),exe,p,ev,block=block,frames=24000)[:,0]
    c(name+':block-'+str(block),np.array_equal(z,x[:24000]))
   z=r(name+'-vector',vec,p,ev,frames=48000)[:,0]
   c(name+':vector-parity',abs(z-x[:48000]).max()<3e-5,max_absolute=float(abs(z-x[:48000]).max()))
   for sr in (44100,96000):
    n=round(.01*sr);z=r(name+'-rate-'+str(sr),exe,p,[(n,'gate',1),(n+1,'gate',0)],sr=sr,frames=sr*2)
    c(name+':rate-finite-'+str(sr),np.isfinite(z).all() and abs(z).max()<1)
   rapid=[(480+i*311+j,'gate',1-j) for i in range(96) for j in (0,1)]
   z=r(name+'-rapid',exe,p,rapid,frames=72000)
   c(name+':persistent-retriggers',abs(z).max()<1.5 and np.linalg.norm(z[32000:])>0)
   longs=p|{k:ui[k][1] for k in ('decay','noiseDecay') if k in ui}
   z=r(name+'-long-tail',exe,longs,ev,frames=576000)
   c(name+':tail-terminates',abs(z[-24000:]).max()<1e-7,tail_peak=float(abs(z[-24000:]).max()))
   live=['tone','drive','level']+[k for k in ('snappy','noise','tail','snap','attack') if k in p]
   # Explicit full-range checks for each musically exposed control, not only four macros.
   for key in ['freq']+list(MANIFEST['implemented_synthesis'][name]['controls']):
    if key=='level':continue
    z0=r(name+'-'+key+'-min',exe,p|{key:ui[key][0]},ev,frames=48000)
    z1=r(name+'-'+key+'-max',exe,p|{key:ui[key][1]},ev,frames=48000)
    diff=float(np.linalg.norm(z0-z1)/(np.linalg.norm(z0)+1e-20))
    c(name+':effective-'+key,diff>.002,relative_l2=diff)
   ref=r(name+'-live-reference',exe,longs,ev,frames=48000)
   for key in live:
    z=r(name+'-live-'+key,exe,longs,ev+[(6000,key,0. if p[key]>=.5 else ui[key][1])],frames=48000)
    delta=float(np.linalg.norm(z[6500:]-ref[6500:])/(np.linalg.norm(ref[6500:])+1e-20))
    if key in ('attack','snap'):c(name+':transient-no-resurrection-'+key,delta<.002,relative_l2=delta)
    else:c(name+':live-tail-'+key,delta>.002,relative_l2=delta)
   corner_keys=['freq','decay','tone','drive']
   edge=[]
   for i,bits in enumerate(itertools.product((0,1),repeat=4)):
    n=480+i*2500;edge += [(n,k,ui[k][bit]) for k,bit in zip(corner_keys,bits)]
    edge += [(n,'accent',i%2),(n,'gate',1),(n+1,'gate',0)]
   z=r(name+'-corners',exe,p,edge,frames=96000)
   c(name+':corners-bounded',abs(z).max()<1.5,peak=float(abs(z).max()))
   bank=[]
   for title,pre in PRESETS[name].items():
    pe=[(4800,'gate',1),(4801,'gate',0),(67200,'velocity',.65),(67200,'gate',1),(67201,'gate',0)]
    z=r(name+'-preset-'+title,exe,p|pre,pe,frames=144000)[:,0]
    L.wav(name+'_'+title+'.wav',z);bank.append(z)
    if title=='Classic':isolation[name]=z
   L.wav(name+'_four_presets.wav',np.concatenate(bank))
  # Actual native C++ extracted component equations, no waveform fitting or whole-voice claim.
  oracle=L.out/'component-oracle';command([os.getenv('CXX','c++'),'-std=c++17','-O2','-ffp-contract=off',ROOT/'tools/modules/drums909_oracle.cpp','-o',oracle])
  probe=L.out/'components.dsp';probe.write_text(f'u=library("{SRC / "drums909.lib"}");process(p,x,g)=u.snareSine(p),u.transistorVCA(x,g);\n')
  exe=L.build('components',probe)
  rng=np.random.default_rng(909);stim=np.column_stack([np.linspace(0,1,16384,endpoint=False),rng.uniform(-3,3,16384),rng.uniform(0,1,16384)]).astype(np.float32)
  ip=L.out/'oracle-input.f32';op=L.out/'oracle-output.f32';stim.tofile(ip);command([oracle,ip,op])
  reference=np.fromfile(op,np.float32).reshape(-1,2)
  result=r('plaits-components',exe,{},inputs=stim,frames=len(stim))
  for i,title in enumerate(('snare-distorted-sine','kick-transistor-vca')):
   err=float(abs(result[:,i]-reference[:,i]).max());c('plaits-component-'+title,err<1e-6,max_absolute=err)
  L.report['component_oracle']=dict(source='tools/modules/drums909_oracle.cpp',source_sha256=digest(ROOT/'tools/modules/drums909_oracle.cpp'),input_sha256=digest(ip),output_sha256=digest(op),provenance='Two functions extracted from MIT Plaits headers; does NOT compare full kick/snare or hardware.')
  # Read-only waveform playback proof, explicitly a synthetic fixture, NOT a 909 hat/cymbal.
  print('QUALIFY diagnostic sample player',flush=True)
  fixture=L.build('sample-fixture',SRC/'sample-fixture.dsp');fixture_vec=L.build('sample-fixture-vector',SRC/'sample-fixture.dsp',True)
  p=dict(gate=0,freq=440,velocity=1,chokeGate=0)
  table=np.linspace(-.6,.6,64,dtype=np.float32)
  for sr in (44100,48000,96000):
   for hz in (220,440,880):
    on=48;z=r(f'fixture-{sr}-{hz}',fixture,p|{'freq':hz},[(on,'gate',1),(on+1,'gate',0)],sr=sr,frames=4096)[:,0]
    pos=np.maximum(0,np.arange(4096)-on)*(hz/440)*(8000/sr)
    expected=np.interp(pos,np.arange(64),table)*((np.arange(4096)>=on)&(pos<64))
    err=float(abs(z-expected).max());c(f'fixture-rate-pitch-{sr}-{hz}',err<3e-5,max_absolute=err)
    c(f'fixture-eof-silent-{sr}-{hz}',not np.any(z[2000:]))
  ev=[(48,'gate',1),(49,'gate',0)]
  base=r('fixture-default',fixture,p,ev,frames=2048)
  vec=r('fixture-vector',fixture_vec,p,ev,frames=2048)
  c('fixture-vector-parity',abs(base-vec).max()<3e-5)
  c('fixture-never-silent',not np.any(r('fixture-never',fixture,p,frames=2048)))
  for block in (1,127,512):
   z=r('fixture-block-'+str(block),fixture,p,ev,block=block,frames=2048);c('fixture-block-'+str(block),np.array_equal(z,base))
  z=r('fixture-choke',fixture,p,ev+[(120,'chokeGate',1),(121,'chokeGate',0)],frames=2048)
  c('fixture-choke-no-early-change',np.array_equal(z[:120],base[:120]))
  c('fixture-choke-reduces-tail',np.linalg.norm(z[250:400])<np.linalg.norm(base[250:400])*.02)
  z=r('fixture-collision',fixture,p,ev+[(48,'chokeGate',1),(49,'chokeGate',0)],frames=2048)
  c('fixture-choke-collision-priority',not np.any(z))
  z=r('fixture-retrigger',fixture,p,ev+[(800,'gate',1),(801,'gate',0)],frames=2048)
  c('fixture-retrigger-restarts-table',np.array_equal(z[800:1184],base[48:432]))
  # Negative control: disabling EOF mask must be caught even if last sample is nonzero.
  sample_source=(SRC/'sample-player.lib').read_text();anchor='valid=u.seen(h)*float(pos<n);'
  c('sample-mutant-anchor',sample_source.count(anchor)==1)
  badlib=L.out/'bad-sample.lib';badlib.write_text(sample_source.replace('library("drums909.lib")',f'library("{SRC / "drums909.lib"}")').replace(anchor,'valid=u.seen(h);'))
  bad=L.out/'bad-sample.dsp';bad.write_text((SRC/'sample-fixture.dsp').read_text().replace('library("sample-player.lib")',f'library("{badlib.resolve()}")'))
  bexe=L.build('bad-sample',bad);z=r('bad-sample',bexe,p,ev,frames=2048)
  c('eof-test-rejects-stuck-last-sample',abs(z[-128:]).max()>.5)
  L.report['sample_spike']=dict(actual_drum=False,source_rate=8000,table_length=64,content='Original linear diagnostic table, deliberately nonzero at EOF',asset_pending=['closed-hat','open-hat','crash','ride'],host_binding_proven=False)
  # Mutation proves onset assertion, not an assumption about every output starting nonzero.
  anchor="h=gate>gate';";orig=(SRC/'kick.dsp').read_text();c('onset-mutant-anchor',orig.count(anchor)==1)
  bad=L.out/'late-kick.dsp';bad.write_text(orig.replace('library("drums909.lib")',f'library("{SRC / "drums909.lib"}")').replace(anchor,"h=(gate>gate')';"))
  traces=[]
  for label,path in [('on-time',SRC/'kick.dsp'),('late',bad)]:
   pth=L.out/(label+'-trace.dsp');pth.write_text(f'm=library("{path}");process=float(m.h);\n');exe=L.build(label+'-trace',pth)
   traces.append(r(label+'-trace',exe,{'gate':0},[(480,'gate',1),(481,'gate',0)],frames=1024)[:,0])
  expected=np.zeros(1024,np.float32);expected[480]=1
  c('onset-sample-exact',np.array_equal(traces[0],expected));c('onset-rejects-late-mutation',not np.array_equal(traces[1],expected))
  seq=L.build('existing-trigger-seq',ROOT/'modules/trigger-seq/v1/trigger.dsp')
  seq_defaults=dict(run=1,length=16,clock=0,reset=0)|{f'step{i+1:02d}':0 for i in range(32)}
  patterns={'kick':[0,4,8,12],'snare':[4,12],'low-tom':[7,15],'mid-tom':[6,14],'high-tom':[3,10],'rim':[2,6,10,14],'clap':[4,12]}
  clocks=[(4800+i*6000+j,'clock',1-j) for i in range(64) for j in (0,1)]
  stems={}
  for name,pattern in patterns.items():
   sd=seq_defaults|{f'step{i+1:02d}':1 for i in pattern}
   z=r(name+'-sequence',seq,sd,clocks,frames=576000)
   hits=np.flatnonzero(z[:,0]>.5).tolist();expected_hits=[4800+i*6000 for i in range(64) if i%16 in pattern]
   c(name+':seq-onsets',hits==expected_hits)
   ev=[]
   for n in hits:
    i=(n-4800)//6000;ev += [(n,'gate',1),(n+1,'gate',0),(n,'velocity',.72 if i%4 else 1),(n,'accent',float(i%4==0))]
   z=r(name+'-persistent-groove',exes[name],DEFAULTS[name],sorted(ev),frames=576000)[:,0]
   stems[name]=z;L.wav(name+'_stem.wav',z)
  L.wav('00_909_synthesis_core_groove.wav',sum(stems.values())*.4)
  L.wav('01_909_seven_synthesized_voices.wav',np.concatenate(list(isolation.values())))
  L.report['audition']=dict(bpm=120,bars=4,seconds=12,fixed_groove_gain=.4,preset_gain=1,slots=3,order=list(DEFAULTS),processing='Dry seven-voice core only. No hats/crash/ride placeholders. No external FX/limiting/normalization; offline adapter, not CURLOP.')
  L.report['sources']={str(p.relative_to(ROOT)):digest(p) for p in sorted(SRC.iterdir()) if p.is_file()}
  L.report['driver_sha256']=digest(__file__);L.report['renderer_sha256']=digest(ROOT/'tools/modules/render.cpp')
  L.report['passed']=all(t['passed'] for t in L.report['checks'])
  if not L.report['passed']:raise AssertionError([t['name'] for t in L.report['checks'] if not t['passed']])
 except Exception as e:
  L.report.update(passed=False,error=repr(e),compiler_output=getattr(e,'output',None));raise
 finally:
  L.report['suite_wall_seconds']=time.perf_counter()-start
  L.report['all_renders_compute_seconds']=sum(a['diagnostics']['instrumented_compute_ns'] for a in L.report['renders'])/1e9
  (L.out/'results.json').write_text(json.dumps(L.report,indent=2)+'\n')
  print(json.dumps(dict(passed=L.report.get('passed',False),checks=len(L.report['checks']),renders=len(L.report['renders']),builds=len(L.report['builds']),error=L.report.get('error'),compiler_output=L.report.get('compiler_output'))),flush=True)
if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('--out',required=True);run(p.parse_args().out)
