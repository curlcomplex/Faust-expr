"""BDM-14 same-preview tail study through actual Faust kernels only.
Four diagnostic fits refine two drive placements with/without amplitude hold.
Requires the earlier four-config exploration as recorded seed evidence.
"""
from __future__ import annotations
import argparse, json, math, time
from pathlib import Path
import numpy as np
from scipy.io import wavfile
from scipy.optimize import minimize, differential_evolution
from fit_color import Kernel, Objective, CORE, COLOR, LOG, sha


def fit_case(k, x, rate, seed, allow_hold, out, label, maxfev=600):
    obj=Objective(x,rate,(.8,1.5))
    names=CORE+COLOR+(('body_hold_s',) if allow_hold else ())
    physical=[(1,1600),(.002,.12),(.025,2.),(.04,len(x)/rate),(.008,.7),(.000001,.003),(0,1),(0,1),
              (0,1),(0,1),(0,1),(.002,.6)]+([(0,1.2)] if allow_hold else [])
    bounds=[(math.log(a),math.log(b)) if name in LOG else (a,b) for name,(a,b) in zip(names,physical)]
    initial=seed['parameters']|dict(gate_off_s=seed['gate_off_frame']/rate,body_hold_s=0.)
    v0=np.array([math.log(initial[n]) if n in LOG else initial[n] for n in names])
    v0=np.clip(v0,np.array(bounds)[:,0],np.array(bounds)[:,1])
    def decode(v):
        p=seed['parameters'].copy()
        p.update({n:float(np.exp(vv)) if n in LOG else float(vv) for n,vv in zip(names,v)})
        off=max(1,min(len(x),round(p.pop('gate_off_s')*rate)))
        if not allow_hold:p.pop('body_hold_s',None)
        return p,off
    best={'loss':float('inf'),'v':v0.copy()};history=[];count=0
    def fun(v):
        nonlocal count
        p,off=decode(v);y=k.render(p,rate,len(x),off); loss=obj.loss(y); count+=1
        if loss<best['loss']:
            best.update(loss=loss,v=v.copy());history.append(dict(evaluation=count,loss=loss,vector=v.tolist()))
        return loss
    protocol=dict(label=label,initial=initial,names=names,physical_bounds=physical,log_parameters=sorted(LOG),
        seed=20260909,differential_evolution_iterations=4,popsize=5,powell_maxfev=maxfev,
        objective='unchanged color-04: mean 3-resolution log spectrum RMSE + .15 envelope RMSE + late pitch penalty',
        library_sha256=sha(k.directory/'kernel.so'),script_sha256=sha(__file__),
        note='Same-recording optimization, not a controlled architecture benchmark or blind validation')
    (out/(label+'-protocol.json')).write_text(json.dumps(protocol,indent=2)+'\n')
    start=time.monotonic();initial_loss=fun(v0)
    differential_evolution(fun,bounds,x0=v0,seed=20260909,popsize=5,maxiter=4,polish=False,tol=0,atol=0,workers=1)
    minimize(fun,best['v'],method='Powell',bounds=bounds,options={'maxfev':maxfev,'ftol':1e-5,'xtol':1e-5})
    p,off=decode(best['v']);y=k.render(p,rate,len(x),off);b=k.render(p,rate,len(x),off,127)
    assert np.max(abs(y-b))<1e-6
    y.astype('<f4').tofile(out/(label+'.f32'));m=obj.metrics(y)
    r=dict(label=label,parameters=p,gate_off_frame=off,rate=rate,frames=len(x),metrics=m,initial_loss=initial_loss,
        loss=best['loss'],search_evaluations=count,search_seconds=time.monotonic()-start,history=history,protocol=protocol,
        raw_sha256=sha(out/(label+'.f32')),block_replay_max_error=float(np.max(abs(y-b))))
    (out/(label+'.json')).write_text(json.dumps(r,indent=2)+'\n')
    print(label,'spectral',m['spectral_rmse_db'],'envelope',m['envelope_rmse_db'],'hold',p.get('body_hold_s',0),flush=True)
    return r


def main():
    a=argparse.ArgumentParser(description=__doc__)
    for n in ['baseline','candidate','references','seeds','out']:a.add_argument('--'+n,type=Path,required=True)
    args=a.parse_args();out=args.out.resolve();out.mkdir(parents=True,exist_ok=True)
    refs=args.references.resolve();mf=json.loads((refs/'manifest.json').read_text())
    rec=next(r for r in mf['records'] if r['id']=='BDM-14');p=(refs/rec['decoded_file']).resolve()
    if p.parent!=refs or sha(p)!=rec['decoded_sha256']:raise ValueError('reference hash/path')
    rate,x=wavfile.read(p);x=x.astype(np.float64)
    if rate!=44100 or x.ndim!=1:raise ValueError('reference format')
    seeds=json.loads(args.seeds.read_text())['results'];base=Kernel(args.baseline);candidate=Kernel(args.candidate)
    results=[]
    for stage in (0,1):
        seed=next(r for r in seeds if r['label']==f'BDM-14-PM-stage{stage}')
        r=fit_case(base,x,rate,seed,False,out,f'color04-refined-stage{stage}');results.append(r)
        # Demonstrate zero-hold starting equivalence, then retain it as incumbent.
        a=base.render(r['parameters'],rate,len(x),r['gate_off_frame'])
        b=candidate.render(r['parameters']|dict(body_hold_s=0),rate,len(x),r['gate_off_frame'])
        if np.max(abs(a-b))>3e-6:raise AssertionError('zero hold changed baseline')
        results.append(fit_case(candidate,x,rate,r,True,out,f'tail05-stage{stage}'))
    report=dict(reference=rec,reference_manifest_sha256=sha(refs/'manifest.json'),seed_report_sha256=sha(args.seeds),
        results=results,scope='BDM-14 in-sample model comparison; no hardware controls, human approval or realtime claims')
    (out/'results.json').write_text(json.dumps(report,indent=2)+'\n')
if __name__=='__main__':main()
