"""Conventional Morph table playback: inherited musical regressions, accurate
reference comparisons, full Stack/polyphony benchmarks and offline replay.
The old Fourier code is an oracle, never silently rewritten to match tables.
"""
from __future__ import annotations
import argparse, hashlib, json, math, os, re, shutil, statistics
from pathlib import Path
import numpy as np
import morph_v2_batch as base
from generate_morph_tables import write as generate_tables
ROOT=base.ROOT; MOD=ROOT/'modules/morph-wavetable/v3'
MAX_ABS_ERROR=3e-4
MAX_RELATIVE_RMS_DB=-65.0
class Study(base.Study):
 def build(self,label,source,vector=False,diagnostic=False):
  if vector:raise RuntimeError('vector deliberately not attempted: scalar lookup architecture is this experiment; no vector timing claim')
  if label!='morph':return super().build(label,source,vector,diagnostic)
  source=MOD/'morph.dsp';d=self.out/label;d.mkdir(exist_ok=True);flags=['-lang','cpp','-single','-cn','ModuleDSP']
  if self.replay:
   old=self.replay/label;expected=json.loads((self.replay/'report.json').read_text())['builds'][label]
   self.check('wavetable:header-hash',base.sha(old/'generated.hpp')==expected['generated_sha256']);shutil.copyfile(old/'generated.hpp',d/'generated.hpp')
  else:
   info=generate_tables(self.out/'bank');self.report['tables']=info
   base.cmd([os.getenv('FAUST','faust'),'-t','240','-I',source.parent,'-I',self.out/'bank',*flags,source,'-o',d/'generated.hpp'],timeout=270)
  cppflags=['-std=c++17','-O2','-ffp-contract=off','-fstack-usage','-I'+str(d)]
  base.cmd([os.getenv('CXX','c++'),*cppflags,ROOT/'tools/modules/render.cpp','-o',d/'render'])
  base.cmd([os.getenv('CXX','c++'),*cppflags,ROOT/'tools/modules/morph_wavetable_benchmark.cpp','-o',d/'benchmark'])
  text=base.cmd([d/'render','--controls']);(d/'controls.tsv').write_text(text);rows=text.splitlines()
  self.controls[label]={x.split('\t')[0]:dict(zip(('min','max','default'),map(float,x.split('\t')[1:4]))) for x in rows[1:]}
  self.check('wavetable:stereo',rows[0]=='io\t0\t2');self.check('wavetable:ids',set(self.controls[label])==set(self.defaults))
  for key,c in self.man['controls'].items():self.check('wavetable:control-'+key,all(abs(self.controls[label][key][f]-c[f])<1e-5 for f in ('min','max','default')))
  self.report['builds'][label]={'source':str(source.relative_to(ROOT)),'source_sha256':base.sha(source),'generated_sha256':base.sha(d/'generated.hpp'),'binary_sha256':base.sha(d/'render'),'faust_flags':flags,'cpp_flags':cppflags}
  return d/'render'
 def check(self,n,ok,**details):
  if n.startswith('clenshaw:'):n=n.replace('clenshaw:','lookup-versus-direct:',1)
  return super().check(n,ok,**details)
 def compare(self,name,params,events=None,seconds=.6,rate=48000):
  events=events or self.note(101,round(seconds*rate*.6))
  a=self.render(name+'-oracle',self.out/'clenshaw/render',params,events,seconds=seconds,rate=rate)
  b=self.render(name+'-lookup',self.out/'morph/render',params,events,seconds=seconds,rate=rate)
  err=a.astype(float)-b.astype(float);rms=float(np.sqrt(np.mean(err*err)));level=float(np.sqrt(np.mean(a.astype(float)**2)))
  relative=20*math.log10(max(1e-15,rms)/max(1e-15,level));mx=float(abs(err).max())
  self.report.setdefault('fidelity',[]).append({'case':name,'max_abs':mx,'relative_rms_db':relative,'rate':rate})
  self.check(name+':fidelity',mx<MAX_ABS_ERROR and relative<MAX_RELATIVE_RMS_DB,max_abs=mx,relative_rms_db=relative)
  return a,b
 def benchmark(self):
  self.build('clenshaw',base.MOD/'morph.dsp')
  for label in ('reference','clenshaw','morph'):
   d=self.out/label;base.cmd([os.getenv('CXX','c++'),'-std=c++17','-O2','-ffp-contract=off','-I'+str(d),ROOT/'tools/modules/morph_wavetable_benchmark.cpp','-o',d/'benchmark'])
  for rate in (44100,48000,96000):
   for hz in (20.,249.,501.,1760.,4000.,8000.):self.compare(f'accuracy-{rate}-{hz}',{'pitch_hz':hz,'morph':.62,'shape':1,'drive':1,'stack':4,'detune':.7},rate=rate)
  rng=np.random.default_rng(902106)
  for i in range(16):
   p={k:float(rng.random()) for k in ('morph','shape','drive','detune','decay')};p.update(pitch_hz=float(np.exp(rng.uniform(np.log(20),np.log(8000)))),stack=1+i%4)
   self.compare('reserved-random-'+str(i),p)
  ev=self.note(101,170000)
  for i in range(160):
   n=101+1000*i;ev +=[(n,'pitch_hz',float(20*400**(i/159))),(n,'morph',i/159),(n,'shape',1-i/159),(n,'drive',i/159)]
  self.compare('all-band-live-glide',{'stack':4,'detune':.7},ev,seconds=4.5)
  row=next(x for x in self.report['renders'] if x['label']=='pattern');score=self.out/'pattern.tsv';data={}
  for label in ('reference','clenshaw','morph'):
   out=self.out/('comparison-pattern-'+label+'.f32');base.cmd([self.out/label/'render',score,out,48000,128,row['frames'],0])
   data[label]=np.fromfile(out,dtype='<f4').reshape(-1,2)
   self.report['renders'].append(dict(label='comparison-pattern-'+label,build=label,rate=48000,block=128,frames=row['frames'],channels=2,score_file='pattern.tsv',score_sha256=base.sha(score),raw_sha256=base.sha(out),diagnostic=False))
  er=data['morph'].astype(float)-data['clenshaw'].astype(float);self.check('pattern:max-error',abs(er).max()<MAX_ABS_ERROR,max_abs=float(abs(er).max()))
  base.wav(self.out/'morph-lookup-vs-fourier.wav',np.concatenate([data['clenshaw'],np.zeros((24000,2)),data['morph']]))
  self.report['auditions']['morph-lookup-vs-fourier.wav']='Old Clenshaw9s -> 0.5s silence -> new lookup9s; SAME score and fixed gain, no level matching.'
  perf=[]
  for hosts,stack,block in [(h,s,b) for h in (1,4,8) for s in (1,2,3,4) for b in (64,128)]+[(4,s,512) for s in (1,2,3,4)]:
   pairs=[]
   for rep in range(3):
    labels=['reference','clenshaw','morph'];labels=labels[rep:]+labels[:rep];row={}
    for label in labels:row[label]=json.loads(base.cmd([self.out/label/'benchmark',block,stack,hosts]))
    self.check(f'no-new:{hosts}:{stack}:{block}:{rep}',all(v['ordinary_new_in_compute']==0 for v in row.values()));pairs.append(row)
   perf.append({'host_voices':hosts,'stack':stack,'frames':block,'pairs':pairs,'speedup_over_clenshaw':statistics.median(r['clenshaw']['p50_us']/r['morph']['p50_us'] for r in pairs),'speedup_over_direct':statistics.median(r['reference']['p50_us']/r['morph']['p50_us'] for r in pairs)})
  self.report['three_way_benchmarks']=perf
  self.report['benchmark_scope']='Same source/score/control settings; warm DSP only; rotating3 repetitions;1/4/8 hosts x allStack counts; no iPhone claim or guaranteed speedup threshold.'
  header=(self.out/'morph/generated.hpp').read_text();decls=re.findall(r'((?:const\s+static|static|static\s+const)\s+(?:float|int))\s+(\w+)\[(\d+)\]',header)
  self.report['generated_shared_arrays']=[{'type':t,'symbol':n,'elements':int(c),'bytes':4*int(c)} for t,n,c in decls]
  self.report['generated_shared_total_bytes']=sum(4*int(c) for t,n,c in decls)
  self.report['tables']=json.loads((self.out/'bank/tablebank.json').read_text())
  self.report['bank_lifecycle']='Call generated classInit once OFF AUDIO THREAD, before voices; thereafter instanceInit per voice/rate. rdtable storage is shared. Calling init/classInit concurrently with rendering can write shared tables: prohibited.'
 def save(self,error=None):
  super().save(error)
  for p in [*MOD.glob('*'),ROOT/'tools/modules/generate_morph_tables.py',ROOT/'tools/modules/morph_wavetable_batch.py',ROOT/'tools/modules/morph_wavetable_benchmark.cpp',ROOT/'tests/test_morph_wavetable.py']:
   if p.is_file():self.report['source_files'][str(p.relative_to(ROOT))]=base.sha(p)
  self.report['tolerances']={'max_absolute_sample_error':MAX_ABS_ERROR,'relative_rms_db_max':MAX_RELATIVE_RMS_DB,'kind':'implementation fidelity, not perceptual identity'}
  self.report['build_aliases']={'reference':'direct Fourier sum','clenshaw':'previous accepted v2','morph':'new conventional cubic wavetable'}
  (self.out/'report.json').write_text(json.dumps(self.report,indent=2)+'\n')
if __name__=='__main__':
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--out',type=Path,required=True);p.add_argument('--replay',type=Path)
 a=p.parse_args();s=Study(a.out.resolve(),a.replay.resolve() if a.replay else None);failure=None
 try:s.run()
 except Exception as e:failure=str(e)
 finally:s.save(failure)
 if failure:raise SystemExit(failure)
