"""Compile the new Juno106 controls; sonic fit remains a separate evidence layer."""
import json,os,traceback
import numpy as np
from synth_batch import SynthLab,ROOT,DEFAULTS
from acid_batch import controls
L=SynthLab(ROOT/'build/juno106-reference');c=L.check;r=L.render
try:
 old=L.build('juno106-v1',ROOT/'modules/juno-106/v1/voice.dsp')
 new=L.build('juno106-v3',ROOT/'modules/juno-106/v3/voice.dsp')
 vec=L.build('juno106-v3-vector',ROOT/'modules/juno-106/v3/voice.dsp',True)
 p=DEFAULTS['juno106'];q=p|dict(vcaGate=0,filterLfo=0,filterLfoPhase=0)
 c('new-controls-additive',set(controls(new)[1])==set(q))
 ev=[(480,'gate',1),(24480,'gate',0)]
 a=r('old-neutral',old,p,ev,frames=96000);b=r('new-neutral',new,q,ev,frames=96000)
 c('neutral-matches-v1',np.max(abs(a-b))<2e-6,max_error=float(np.max(abs(a-b))))
 for sr in (44100,48000,96000):
  for mode in (0,1):
   z=q|dict(vcaGate=mode,filterLfo=1.5,lfoRate=.38,cutoff=3000,resonance=.65,noise=0)
   ev2=[(round(sr*.1),'gate',1),(round(sr*1.5),'gate',0)]
   y=r(f'mode-{mode}-{sr}',new,z,ev2,sr=sr,frames=sr*3)
   c(f'bounded-{mode}-{sr}',np.max(abs(y))<1 and np.std(y[sr//2:sr])>1e-3)
   c(f'release-{mode}-{sr}',np.max(abs(y[-sr//10:]))<1e-6)
   v=r(f'vector-{mode}-{sr}',vec,z,ev2,sr=sr,frames=sr*3)
   c(f'vector-{mode}-{sr}',np.max(abs(y-v))<1e-4,max_error=float(np.max(abs(y-v))))
except Exception:
 c('completed',False);L.report['exception']=traceback.format_exc();print(L.report['exception'])
L.report.update(commit=os.getenv('GITHUB_SHA','local'),hardware_approved=False,
 passed=bool(L.report['checks']) and all(q.get('passed') is True for q in L.report['checks']))
(L.out/'results.json').write_text(json.dumps(L.report,indent=2))
print('JUNO106_CHECK',json.dumps(dict(passed=L.report['passed'],checks=len(L.report['checks']),renders=len(L.report['renders']))))
raise SystemExit(0 if L.report['passed'] else 1)
