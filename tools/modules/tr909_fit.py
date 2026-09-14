"""Bounded preset search using actual previously Faust-generated C++ executables.
Reference descriptors come from hash-pinned acquisition. All candidate DSP is native.
Fit records are exploratory and are not the frozen qualification results.
"""
from pathlib import Path
import sys,json,subprocess,hashlib,argparse,time,os
import numpy as np
from scipy.optimize import differential_evolution,minimize
from tr909_reference import features,loss,components,ANCHORS,TONAL
from drums909_batch import DEFAULTS
RANGES={
 'kick':{'freq':(40,58),'decay':(.06,.7),'pitchAmount':(0,.7),'pitchDecay':(.002,.04),'attack':(0,.3),'tone':(0,.7),'drive':(0,.2)},
 'snare':{'freq':(120,200),'decay':(.035,.4),'snappy':(0,.7),'noiseDecay':(.025,.4),'bend':(0,.4),'tone':(0,1),'drive':(0,.15)},
 'low-tom':{'freq':(65,130),'decay':(.3,1),'bend':(0,1),'tone':(0,1),'noise':(0,.6),'drive':(0,.15)},
 'mid-tom':{'freq':(80,150),'decay':(.25,.8),'bend':(0,1),'tone':(0,1),'noise':(0,.6),'drive':(0,.15)},
 'high-tom':{'freq':(100,190),'decay':(.25,.8),'bend':(0,1),'tone':(0,1),'noise':(0,.6),'drive':(0,.15)},
 'rim':{'freq':(180,340),'decay':(.04,.22),'tone':(0,1),'snap':(0,1),'drive':(0,.2)},
 'clap':{'freq':(600,1600),'decay':(.12,.6),'spacing':(.007,.02),'tail':(.3,1),'tone':(0,1),'drive':(0,.2)}
}
def run(baseline,out):
 p=Path(baseline);out=Path(out);out.mkdir(exist_ok=True)
 r=json.loads((p/'baseline.json').read_text());fit={'baseline_commit':r['commit'],'archive_sha256':r['references']['archive_sha256'],'algorithm':'scipy DE seed909, popsize5, maxiter12, no polish; then bounded Powell maxfev120','selected':{},'search':{}}
 for name in ANCHORS:
  ex=p/('baseline-tom' if name.endswith('-tom') else 'baseline-'+name)/'render'
  target=r['references']['recordings'][ANCHORS[name]]['features'];keys=list(RANGES[name]);bounds=list(RANGES[name].values());hist=[]
  score=out/(name+'-fit.tsv');raw=out/(name+'-fit.f32')
  best=[float('inf'),None,None]
  def evaluate(v):
   pars=DEFAULTS[name]|dict(zip(keys,map(float,v)))
   score.write_text(''.join(f'0\t{k}\t{x:.9g}\n' for k,x in sorted(pars.items()))+'4800\tgate\t1\n4801\tgate\t0\n')
   d=json.loads(subprocess.check_output([str(ex),str(score),str(raw),'48000','128','96000','0'],text=True,stderr=subprocess.STDOUT,timeout=15))
   y=np.fromfile(raw,'<f4');f=features(y,48000);l=loss(target,f,name in TONAL)
   h=hashlib.sha256(raw.read_bytes()).hexdigest();hist.append({'loss':l,'values':list(map(float,v)),'raw_sha256':h})
   if l<best[0]:best[:]=[l,pars,f];(out/(name+'-best.f32')).write_bytes(raw.read_bytes())
   return l
  v0=np.array([np.clip(DEFAULTS[name][k],*RANGES[name][k]) for k in keys]);evaluate(v0)
  z=differential_evolution(evaluate,bounds,seed=909,popsize=5,maxiter=12,polish=False,x0=v0,workers=1)
  minimize(evaluate,z.x,method='Powell',bounds=bounds,options={'maxfev':120,'xtol':.003,'ftol':.002})
  fit['selected'][name]={'loss':best[0],'baseline_loss':r['baseline'][name]['loss'],'settings':best[1],'features':best[2],'source':r['baseline'][name]['source'],'source_sha256':r['baseline'][name]['source_sha256'],'reference':ANCHORS[name],'component_errors':components(target,best[2],name in TONAL)}
  fit['search'][name]={'keys':keys,'bounds':bounds,'evaluations':hist}
  (out/'fit.json').write_text(json.dumps(fit,indent=2));print(name,best[0],len(hist),best[1],flush=True)
  raw.unlink(missing_ok=True)
if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('--baseline',required=True);p.add_argument('--out',required=True);a=p.parse_args();run(a.baseline,a.out)
