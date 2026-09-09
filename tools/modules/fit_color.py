"""Bounded in-sample color/drive study; candidates rendered only by generated Faust.
No hardware knob-map, held-out validation or perceptual quality claim.
"""
from __future__ import annotations
import argparse, ctypes, hashlib, json, math, os, time
from pathlib import Path
import numpy as np
from scipy.io import wavfile
from scipy.signal import stft
from scipy.optimize import differential_evolution, minimize


def sha(p): return hashlib.sha256(Path(p).read_bytes()).hexdigest()

class Kernel:
    def __init__(self, directory):
        self.directory=Path(directory).resolve()
        rows=(self.directory/'controls.tsv').read_text().splitlines()
        if rows[0]!='io\t0\t1': raise ValueError('kernel I/O')
        self.names=[r.split('\t')[0] for r in rows[1:]]
        self.defaults={r.split('\t')[0]:float(r.split('\t')[3]) for r in rows[1:]}
        self.handle=ctypes.CDLL(str(self.directory/'kernel.so'))
        self.fn=self.handle.color_render
        self.fn.argtypes=[ctypes.POINTER(ctypes.c_double),ctypes.c_int,ctypes.c_int,ctypes.c_int,ctypes.c_int,ctypes.c_int,ctypes.POINTER(ctypes.c_float)]
        self.fn.restype=ctypes.c_int
        self.evaluations=0
    def render(self, p, rate, frames, off, block=128):
        if set(p)-set(self.names): raise ValueError('unknown control')
        vals=np.array([(self.defaults|p)[k] for k in self.names],np.float64)
        y=np.empty(frames,np.float32)
        code=self.fn(vals.ctypes.data_as(ctypes.POINTER(ctypes.c_double)),len(vals),rate,frames,int(off),block,y.ctypes.data_as(ctypes.POINTER(ctypes.c_float)))
        self.evaluations+=1
        if code: raise ValueError('native render error '+str(code))
        return y.astype(np.float64)


def dominant(x, rate, window):
    y=x[round(window[0]*rate):round(window[1]*rate)].astype(float)
    if len(y)<512 or np.max(np.abs(y))<1e-8: return float('nan')
    y=y-y.mean();n=1<<max(18,(len(y)-1).bit_length())
    m=np.abs(np.fft.rfft(y*np.hanning(len(y)),n));m[0]=0;k=int(m.argmax())
    if k<1 or k==len(m)-1: return float('nan')
    z=np.log(np.maximum(m[k-1:k+2],1e-30));den=z[0]-2*z[1]+z[2]
    return float((k+(.5*(z[0]-z[2])/den if den else 0))*rate/n)

class Objective:
    def __init__(self, x, rate, window):
        self.x=x;self.rate=rate;self.window=window;self.norm=np.sum(x*x)
        self.n=(256,1024,4096)
        self.ref=[self.spec(x,n) for n in self.n]
        self.floors=[max(float(s.max())*1e-3,1e-12) for s in self.ref]
        self.db=[20*np.log10(np.maximum(s,f)) for s,f in zip(self.ref,self.floors)]
        self.hop=round(.01*rate)
        self.env=self.envelope(x)
        self.mask=self.env>self.env.max()*10**(-45/20)
        self.ref_pitch=dominant(x,rate,window)
    def spec(self,x,n): return abs(stft(x,fs=self.rate,nperseg=n,noverlap=n*3//4,boundary='zeros',padded=True)[2])
    def envelope(self,x):
        return np.sqrt(np.mean(np.pad(x,(0,(-len(x))%self.hop)).reshape(-1,self.hop)**2,axis=1))
    def metrics(self,y):
        energy=np.sum(y*y)
        if not np.isfinite(y).all() or energy<1e-20: raise ValueError('invalid/silent candidate')
        gain=math.sqrt(self.norm/energy)
        errs=[float(np.sqrt(np.mean((20*np.log10(np.maximum(self.spec(y*gain,n),f))-d)**2))) for n,f,d in zip(self.n,self.floors,self.db)]
        env=self.envelope(y*gain)
        er=20*np.log10(np.maximum(env[self.mask],1e-12)/np.maximum(self.env[self.mask],1e-12))
        p=dominant(y,self.rate,self.window);cents=1200*np.log2(p/self.ref_pitch) if np.isfinite(p) else 1e6
        def quant(x,q): return np.searchsorted(np.cumsum(x*x),q*np.sum(x*x))/self.rate
        quants={str(q):dict(reference_s=quant(self.x,q),candidate_s=quant(y,q),error_s=quant(y,q)-quant(self.x,q)) for q in (.5,.9,.99)}
        return dict(spectral_rmse_db=float(np.mean(errs)),per_resolution_db=errs,
                    envelope_rmse_db=float(np.sqrt(np.mean(er*er))),rms_gain=gain,
                    reference_late_peak_hz=self.ref_pitch,candidate_late_peak_hz=p,pitch_error_cents=float(cents),
                    pitch_acceptable=bool(abs(cents)<=15),raw_peak=float(abs(y).max()),raw_rms=math.sqrt(energy/len(y)),
                    energy_quantiles=quants)
    def loss(self,y):
        m=self.metrics(y)
        return m['spectral_rmse_db']+.15*m['envelope_rmse_db']+max(0,abs(m['pitch_error_cents'])-5)/20

# Bounds in physical coordinates are recorded before each search. Positive times
# and pitch sweep span logarithmically; frequency is fixed, not traded for timbre.
CORE=('pitch_amount_hz','pitch_tau_s','body_tau_s','gate_off_s','release_tau_s','attack_tau_s','phase_cycles','drive')
LOG={'pitch_amount_hz','pitch_tau_s','body_tau_s','release_tau_s','attack_tau_s','mod_tau_s'}
COLOR=('square','triangle','mod_envelope','mod_tau_s')

def transformed_bounds(frames,rate,pm):
    secs=frames/rate
    bounds=[(1.,1600.),(.002,.12),(.025,1.5),(.04,min(2.5,secs)),(.008,.7),(.000001,.003),(0,1),(0,1)]
    if pm: bounds += [(0,1),(0,1),(0,1),(.002,.6)]
    names=CORE+(COLOR if pm else ())
    return names,[(math.log(a),math.log(b)) if k in LOG else (a,b) for k,(a,b) in zip(names,bounds)]

def decode(values,names,stage,frequency,feedback=1):
    p=dict(frequency_hz=frequency,level=.65,velocity=1.,gate=0.,square=0.,triangle=0.,mod_envelope=1.,mod_tau_s=.06,
           drive_after_body=float(stage),feedback_mode=float(feedback))
    p.update({k:(float(np.exp(v)) if k in LOG else float(v)) for k,v in zip(names,values)})
    off=p.pop('gate_off_s')
    return p,off

def fit(kernel,x,rate,window,out,label,stage,pm,seed_params,seed_off,iterations,seed):
    names,bounds=transformed_bounds(len(x),rate,pm)
    obj=Objective(x,rate,window)
    initial=seed_params|{'gate_off_s':seed_off}
    v=np.array([math.log(max(initial[k],math.exp(bounds[i][0]))) if k in LOG else initial[k] for i,k in enumerate(names)])
    v=np.clip(v,np.array(bounds)[:,0],np.array(bounds)[:,1])
    params,off=decode(v,names,stage,obj.ref_pitch)
    best=dict(loss=obj.loss(kernel.render(params,rate,len(x),min(len(x),round(off*rate)))),vector=v.copy())
    history=[]
    protocol=dict(names=names,transformed_bounds=bounds,log_parameters=sorted(LOG),initial_parameters=params,
                  initial_gate_off_s=off,mode=dict(stage=stage,pm=pm),seed=seed,iterations=iterations,popsize=5,
                  local_maxfev=400,objective='spectral RMSE + 0.15 envelope RMSE + penalty beyond 5 late-pitch cents',
                  reference_sha256=hashlib.sha256(x.astype('<f8').tobytes()).hexdigest(),
                  library_sha256=sha(kernel.directory/'kernel.so'),script_sha256=sha(__file__))
    (out/(label+'-protocol.json')).write_text(json.dumps(protocol,indent=2)+'\n')
    count=0
    def fun(v):
        nonlocal count
        p,off=decode(v,names,stage,obj.ref_pitch)
        y=kernel.render(p,rate,len(x),min(len(x),max(1,round(off*rate))))
        value=obj.loss(y);count+=1
        if value<best['loss']:
            best.update(loss=value,vector=v.copy());history.append(dict(evaluation=count,loss=float(value),values=v.tolist()))
        return value
    start=time.monotonic()
    de=differential_evolution(fun,bounds,seed=seed,popsize=5,maxiter=iterations,polish=False,workers=1,
                              updating='immediate',x0=v,atol=0,tol=0)
    local=minimize(fun,best['vector'],method='Powell',bounds=bounds,options={'maxfev':400,'ftol':1e-5,'xtol':1e-5})
    p,off=decode(best['vector'],names,stage,obj.ref_pitch)
    off_sample=min(len(x),max(1,round(off*rate)))
    y=kernel.render(p,rate,len(x),off_sample)
    replay=kernel.render(p,rate,len(x),off_sample,127)
    if np.max(abs(y-replay))>1e-6: raise AssertionError('winner segmentation changed sound')
    y.astype('<f4').tofile(out/(label+'.f32'))
    m=obj.metrics(y)
    result=dict(label=label,parameters=p,gate_off_frame=off_sample,rate=rate,frames=len(x),metrics=m,
                search_evaluations=count,search_seconds=time.monotonic()-start,history=history,protocol=protocol,
                raw_sha256=sha(out/(label+'.f32')),replay_max_error=float(np.max(abs(y-replay))),
                note='in-sample selected preset; not controlled hardware parameters or validation')
    (out/(label+'.json')).write_text(json.dumps(result,indent=2)+'\n')
    print(label,'spec',round(m['spectral_rmse_db'],4),'env',round(m['envelope_rmse_db'],4),
          'cents',round(m['pitch_error_cents'],2),'n',count,flush=True)
    return result


def main():
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--kernel',type=Path,required=True)
    ap.add_argument('--references',type=Path,required=True);ap.add_argument('--out',type=Path,required=True)
    ap.add_argument('--id',choices=['BDM-05','BDM-09','BDM-14'],required=True);ap.add_argument('--iterations',type=int,default=12)
    args=ap.parse_args()
    if not 1<=args.iterations<=30:raise ValueError('unbounded search')
    out=args.out.resolve();out.mkdir(parents=True,exist_ok=True);refs=args.references.resolve()
    mf=json.loads((refs/'manifest.json').read_text());rec=next(r for r in mf['records'] if r['id']==args.id)
    path=(refs/rec['decoded_file']).resolve()
    if path.parent!=refs or sha(path)!=rec['decoded_sha256']:raise ValueError('reference path/hash')
    rate,x=wavfile.read(path);x=x.astype(np.float64)
    if rate!=44100 or x.ndim!=1:raise ValueError('reference format')
    kernel=Kernel(args.kernel);window=(.8,1.5) if args.id=='BDM-14' else (.22,.36)
    seed=kernel.defaults|dict(pitch_amount_hz=350 if args.id=='BDM-05' else (80 if args.id=='BDM-09' else 600),
        pitch_tau_s=.025,body_tau_s=.18 if args.id!='BDM-14' else .4,release_tau_s=.04 if args.id!='BDM-14' else .2,
        attack_tau_s=.0001,phase_cycles=.0,drive=.4)
    off=.23 if args.id=='BDM-05' else (.8 if args.id=='BDM-09' else 1.2)
    results=[]
    for stage in (0,1):
        label=args.id+f'-noPM-stage{stage}'
        results.append(fit(kernel,x,rate,window,out,label,stage,False,seed,off,args.iterations,20260909))
    # Equal-budget PM extension for both drive placements, each seeded by its own
    # no-PM result so the nested baseline cannot be discarded by a random search.
    for stage in (0,1):
        r=results[stage];p=r['parameters'];p=p|{'mod_envelope':1.,'mod_tau_s':.06}
        results.append(fit(kernel,x,rate,window,out,args.id+f'-PM-stage{stage}',stage,True,p,r['gate_off_frame']/rate,args.iterations,20260909))
    report=dict(reference=rec,reference_manifest_sha256=sha(refs/'manifest.json'),results=results,
                scope='same-preview patch fit; unknown settings/firmware, no blind validation, source of drive unknown',
                human_approved=False,target_realtime_qualified=False)
    (out/'results.json').write_text(json.dumps(report,indent=2)+'\n')
if __name__=='__main__':main()
