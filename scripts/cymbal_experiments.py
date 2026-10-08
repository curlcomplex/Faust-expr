"""Render and falsify the actual compiled Faust instrument. Never normalise tests."""
from __future__ import annotations
import argparse,hashlib,json,pathlib,subprocess,sys,wave
import numpy as np

def metrics(x,rate):
 y=x[int(.35*rate):].mean(axis=1)
 spec=abs(np.fft.rfft(y*np.hanning(len(y))))**2
 hz=np.fft.rfftfreq(len(y),1/rate)
 return {'rms':float(np.sqrt(np.mean(y*y))),'centroid_hz':float(np.sum(hz*spec)/max(1e-30,spec.sum())),'high_band_fraction':float(spec[hz>3500].sum()/max(1e-30,spec.sum())),'sha256':hashlib.sha256(x.tobytes()).hexdigest()}

def main():
 p=argparse.ArgumentParser();p.add_argument('--renderer',type=pathlib.Path,required=True);p.add_argument('--out',type=pathlib.Path,required=True);args=p.parse_args()
 out=args.out;out.mkdir(parents=True,exist_ok=True);renderer=args.renderer.resolve()
 cases=[
 ('silence',{'velocity':0},1,48000,None,False),
 ('bronze_edge',{'position':.9},4,48000,None,False),
 ('bronze_bell',{'position':.13},4,48000,None,False),
 ('bronze_bow',{'position':.55},4,48000,None,False),
 ('felt',{'beater':2,'position':.8},4,48000,None,False),
 ('rubber',{'beater':3,'position':.8},3,48000,None,False),
 ('nylon',{'beater':1},3,48000,None,False),
 ('steel_beater',{'beater':4},3,48000,None,False),
 ('soft_felt',{'beater':5,'position':.8},4,48000,None,False),
 ('tiny',{'diameter_m':.08,'position':.85},3,48000,None,False),
 ('huge',{'diameter_m':3,'position':.85},6,48000,None,False),
 ('glass_like',{'material':3,'position':.65},4,48000,None,False),
 ('wood_like',{'material':4,'position':.65},3,48000,None,False),
 ('small_bell',{'bell_size':.08,'bell_height':.03,'position':.18},3,48000,None,False),
 ('large_bell',{'bell_size':.50,'bell_height':.14,'position':.18},3,48000,None,False),
 ('unhammered',{'hammering':0},3,48000,None,False),
 ('hammered',{'hammering':1},3,48000,None,False),
 ('linear_soft',{'velocity':.25,'nonlinearity':0},3,48000,None,False),
 ('linear_hard',{'velocity':.9,'nonlinearity':0},3,48000,None,False),
 ('nonlinear_soft',{'velocity':.25,'nonlinearity':1},3,48000,None,False),
 ('nonlinear_hard',{'velocity':.9,'nonlinearity':1},3,48000,None,False),
 ('grow_while_ringing',{'position':.85},6,48000,'diameter_m,0.12,3',False),
 ('position_roll',{},4,48000,'position,0.04,1',True),
 ('stress_tiny_thick',{'diameter_m':.02,'thickness_mm':6,'velocity':1,'nonlinearity':1,'beater_mass_g':120,'beater':4},1.5,44100,None,True),
 ('stress_giant_thin',{'diameter_m':10,'thickness_mm':.05,'velocity':1,'nonlinearity':1,'material':4,'bell_size':.5,'bell_height':.14,'damping':.25},2,96000,None,True),
 ('stress_tiny_thin',{'diameter_m':.02,'thickness_mm':.05,'velocity':1,'nonlinearity':1,'material':3,'damping':.25},1.5,48000,None,True),
 ('repeatability',{'position':.9},4,48000,None,False)]
 results={};audio={};checks=[]
 def check(name,ok,detail=None): checks.append({'test':name,'passed':bool(ok),'detail':detail})
 for name,params,seconds,rate,sweep,roll in cases:
  command=[str(renderer),str(out/(name+'.f32')),'--seconds',str(seconds),'--rate',str(rate)]
  for key,value in params.items():command+=['--param',f'{key}={value}']
  if sweep:command+=['--sweep',sweep]
  if roll:command+=['--roll']
  try:
   run=subprocess.run(command,text=True,capture_output=True,timeout=100,check=True)
   record=json.loads(run.stdout.strip().splitlines()[-1]);record.update({'parameters':params,'sweep':sweep,'roll':roll})
   x=np.fromfile(out/(name+'.f32'),dtype='<f4').reshape(-1,2)
   record.update(metrics(x,rate));results[name]=record;audio[name]=x
   check(name+':finite',np.isfinite(x).all())
   check(name+':solver',record['solver_residual_peak']<1e-5,record['solver_residual_peak'])
   if name=='silence':check('zero_velocity_is_silent',np.max(abs(x))<1e-12)
   elif not name.startswith('stress_'):check(name+':audible',record['rms']>1e-7)
   check(name+':no_output_clipping',record['raw_peak']<.98,record['raw_peak'])
   check(name+':initial_silence',np.max(abs(x[:int(.34*rate)]))<1e-10)
   if not roll and not sweep:
    injected=.5*(params.get('beater_mass_g',18)*.001)*(3*params.get('velocity',.7))**2
    check(name+':energy_bound',record['energy_peak']<=injected*1.025+1e-8,{'peak':record['energy_peak'],'injected':injected})
    check(name+':decay_passivity',record['max_post_contact_energy_increase']<max(1e-8,injected*1e-5),record['max_post_contact_energy_increase'])
   if record['raw_peak']<.98:
    with wave.open(str(out/(name+'.wav')),'wb') as wav:
     wav.setnchannels(2);wav.setsampwidth(2);wav.setframerate(rate);wav.writeframes(np.round(x*32767).astype('<i2').tobytes())
   (out/(name+'.json')).write_text(json.dumps(record,indent=2)+'\n')
   print(name,json.dumps(record),flush=True)
  except (subprocess.SubprocessError,ValueError,OSError) as e:
   detail=str(e)+'\n'+getattr(e,'stderr','');results[name]={'error':detail};check(name+':render',False,detail);print(name,'FAILED',detail,flush=True)
 def diff(a,b):
  if a not in audio or b not in audio:return 0
  n=min(len(audio[a]),len(audio[b]));return float(np.linalg.norm(audio[a][:n]-audio[b][:n]))
 for a,b in [('bronze_bell','bronze_edge'),('felt','steel_beater'),('unhammered','hammered'),('small_bell','large_bell'),('glass_like','wood_like')]:check('response:'+a+'!='+b,diff(a,b)>1e-5)
 if all(k in audio for k in ('linear_soft','linear_hard','nonlinear_soft','nonlinear_hard')):
  lin=np.linalg.norm(audio['linear_soft']/.25-audio['linear_hard']/.9)/max(1e-30,np.linalg.norm(audio['linear_hard']/.9))
  nonlin=np.linalg.norm(audio['nonlinear_soft']/.25-audio['nonlinear_hard']/.9)/max(1e-30,np.linalg.norm(audio['nonlinear_hard']/.9))
  check('linear_reference_scales_with_velocity',lin<1e-4,float(lin))
  check('nonlinear_hard_hit_is_not_just_louder',nonlin>1e-3,float(nonlin))
 if 'contact_samples' in results.get('soft_felt',{}) and 'contact_samples' in results.get('steel_beater',{}):check('soft_beater_has_longer_contact',results['soft_felt']['contact_samples']>results['steel_beater']['contact_samples'])
 if 'sha256' in results.get('bronze_edge',{}) and 'sha256' in results.get('repeatability',{}):check('bitwise_repeatability',results['bronze_edge']['sha256']==results['repeatability']['sha256'])
 summary={'cases':results,'checks':checks,'passed':sum(c['passed'] for c in checks),'failed':sum(not c['passed'] for c in checks),'source':'actual compiled Faust DSP; no per-file level normalisation','limitations':'Engineering checks are not perceptual or experimental validation of a real cymbal.'}
 (out/'results.json').write_text(json.dumps(summary,indent=2)+'\n')
 print('CHECKS',summary['passed'],'passed',summary['failed'],'failed',flush=True)
 return int(summary['failed']>0)

if __name__=='__main__':sys.exit(main())
