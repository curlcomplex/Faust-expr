"""Independent percussion diagnostics, NOT a perceptual similarity verdict.
Inputs are onset-aligned stereo at 48 kHz. No gain matching is performed here.
Centered zero padding retains the initial sample; channel powers never cancel.
"""
from __future__ import annotations
import numpy as np
from scipy import signal
RATE=48000
WINDOWS=(256,1024,4096,16384)
TIMES=(0.,.03,.2,1.,3.,6.)
def check_audio(x):
 x=np.asarray(x,dtype=np.float64)
 if x.ndim==1:x=x[:,None]
 if x.ndim!=2 or not 1<=x.shape[1]<=2 or len(x)<32 or not np.isfinite(x).all():
  raise ValueError('Expected finite mono/stereo audio with >=32 frames')
 return x

def spec(x,n):
 x=check_audio(x)
 if len(x)<n:x=np.pad(x,((0,n-len(x)),(0,0)))
 f,t,z=signal.stft(x,fs=RATE,nperseg=n,noverlap=n-n//4,axis=0,boundary='zeros',padded=True)
 return f,t,np.mean(np.abs(z)**2,axis=1)

def describe(x):
 x=check_audio(x); e=np.mean(x*x,axis=1);total=e.sum()
 env=np.sqrt(np.mean(np.pad(e,(0,(-len(e))%48)).reshape(-1,48),axis=1))
 cum=np.cumsum(e)/max(total,1e-30)
 out={'duration_s':len(x)/RATE,'peak':float(abs(x).max()),'rms':float(np.sqrt(e.mean())),
      'energy_times_s':{str(q):float(np.searchsorted(cum,q)/RATE) for q in [.2,.5,.9,.99]},
      'first_10ms_energy_fraction':float(e[:480].sum()/max(total,1e-30)),
      'peak_time_ms':float(np.argmax(env)), 'intervals':[]}
 f,t,p=spec(x,4096); keep=(f>=100)&(f<=16000)
 for a,b in zip(TIMES,TIMES[1:]):
  mask=(t>=a)&(t<min(b,len(x)/RATE))
  P=p[:,mask].mean(axis=1) if mask.any() else np.zeros(len(f))
  out['intervals'].append({'seconds':[a,b],'available':bool(mask.any()),
   'centroid_hz':float(np.sum(f*P)/max(P.sum(),1e-30)),
   'flatness_100_16k':float(np.exp(np.mean(np.log(np.maximum(P[keep],1e-25))))/max(P[keep].mean(),1e-25)) if P.sum()>0 else None,
   'high_band_fraction':float(P[f>=6000].sum()/max(P.sum(),1e-30))})
 return out

def compare(ref,candidate):
 r=check_audio(ref);c=check_audio(candidate)
 lengths=[len(r),len(c)];n=max(lengths);r=np.pad(r,((0,n-len(r)),(0,0)));c=np.pad(c,((0,n-len(c)),(0,0)))
 R=describe(r);C=describe(c);out={'input_frames':lengths,'reference':R,'candidate':C,'resolution_metrics':[]}
 for w in WINDOWS:
  f,t,rp=spec(r,w);_,_,cp=spec(c,w);band=(f>=30)&(f<=20000)
  rp=rp[band];cp=cp[band];eps=max(rp.max()*1e-8,1e-25)
  active=rp>max(rp.max()*1e-6,1e-25)
  delta=10*np.log10(np.maximum(cp,eps)/np.maximum(rp,eps))
  out['resolution_metrics'].append({'window':w,
   'spectral_convergence':float(np.linalg.norm(np.sqrt(cp)-np.sqrt(rp))/max(np.linalg.norm(np.sqrt(rp)),1e-25)),
   'active_log_magnitude_mae_db':float(np.mean(abs(delta[active]))) if active.any() else float(abs(delta).mean()),
   'attack_convergence':float(np.linalg.norm(np.sqrt(cp[:,t<=.03])-np.sqrt(rp[:,t<=.03]))/max(np.linalg.norm(np.sqrt(rp[:,t<=.03])),1e-25))})
 def envelope(x):
  e=np.mean(x*x,axis=1);return np.sqrt(np.pad(e,(0,(-len(e))%48)).reshape(-1,48).mean(axis=1))
 re,ce=envelope(r),envelope(c);eps=max(re.max()*1e-4,1e-15);mask=re>eps*3
 out['envelope_mae_db']=float(np.mean(abs(20*np.log10(np.maximum(ce[mask],eps)/np.maximum(re[mask],eps))))) if mask.any() else 0.
 out['rms_ratio_db']=float(20*np.log10(max(C['rms'],1e-25)/max(R['rms'],1e-25)))
 out['energy_time_relative_errors']={q:float((C['energy_times_s'][q]-v)/max(v,.01)) for q,v in R['energy_times_s'].items()}
 out['attack_energy_fraction_error']=float(C['first_10ms_energy_fraction']-R['first_10ms_energy_fraction'])
 out['max_interval_centroid_fractional_error']=float(max([abs(b['centroid_hz']-a['centroid_hz'])/max(a['centroid_hz'],100) for a,b in zip(R['intervals'],C['intervals']) if a['centroid_hz']]+[0.]))
 out['engineering_gates']={
  'no_clipping':C['peak']<.98,
  'envelope_within_3dB':out['envelope_mae_db']<=3.,
  't50_within_15pct':abs(out['energy_time_relative_errors']['0.5'])<=.15,
  't90_within_15pct':abs(out['energy_time_relative_errors']['0.9'])<=.15,
  'attack_fraction_within_5pp':abs(out['attack_energy_fraction_error'])<=.05,
  'multires_active_error_under_6dB':all(v['active_log_magnitude_mae_db']<=6 for v in out['resolution_metrics']),
  'centroid_trajectory_within_15pct':out['max_interval_centroid_fractional_error']<=.15}
 out['threshold_status']='Provisional engineering thresholds, not validated perceptual equivalence thresholds. Compare independent real-hit variability.'
 out['listening_equivalence']='NOT ESTABLISHED; listening test required even if all engineering gates pass'
 return out
