"""Strengthen Tone qualification after tone_batch.py: controlled PM ablation, rate diagnostic and four-voice benchmark."""
from __future__ import annotations
import argparse,json,math,subprocess,wave
from pathlib import Path
import numpy as np
from scipy.signal import resample_poly
from scipy.io import wavfile
ROOT=Path(__file__).resolve().parents[2]
def cmd(a,timeout=180):
 p=subprocess.run(list(map(str,a)),cwd=ROOT,capture_output=True,text=True,timeout=timeout)
 if p.returncode:raise RuntimeError(f'{a}\n{p.stdout}\n{p.stderr}')
 return p.stdout
def render(exe,out,label,p,rate=48000,seconds=2.5,block=128):
 score=out/(label+'.tsv');raw=out/(label+'.f32');rows={**p,'velocity':1,'gate':0};ev=[(0,k,v) for k,v in rows.items()]+[(101 if rate==48000 else round(101*rate/48000),'gate',1),(165 if rate==48000 else round(165*rate/48000),'gate',0)];ev.sort();score.write_text(''.join(f'{n}\t{k}\t{v}\n' for n,k,v in ev));frames=round(rate*seconds);diag=json.loads(cmd([exe,score,raw,rate,block,frames,0]));x=np.fromfile(raw,dtype='<f4');return x,diag
def desc(x,rate=48000):
 x=np.asarray(x,dtype=float);e=x*x;tot=max(float(e.sum()),1e-30);cum=np.cumsum(e)/tot;spec=np.abs(np.fft.rfft(x*np.hanning(len(x))))**2;freq=np.fft.rfftfreq(len(x),1/rate);st=max(float(spec.sum()),1e-30);cent=float((spec*freq).sum()/st);flat=float(np.exp(np.mean(np.log(spec+1e-20)))/(np.mean(spec)+1e-20));return np.array([np.searchsorted(cum,.5)/rate,np.searchsorted(cum,.9)/rate,math.log1p(cent)/10,flat,np.sqrt(np.mean(x*x))])
def dist(a,b):return float(np.sqrt(np.mean(((a-b)/np.array([.5,1,.25,.2,.1]))**2)))
def main():
 a=argparse.ArgumentParser();a.add_argument('--build',type=Path,required=True);a.add_argument('--references',type=Path,required=True);x=a.parse_args();out=x.build.resolve();refs=x.references.resolve();exe=out/'scalar/render';generated=out/'scalar/generated.hpp';bench=out/'scalar/tone-benchmark';cmd([__import__('os').environ.get('CXX','c++'),'-std=c++17','-O2','-ffp-contract=off','-I'+str(out/'scalar'),ROOT/'tools/modules/tone_benchmark.cpp','-o',bench])
 patches=json.loads((ROOT/'modules/tone-pm/patches.json').read_text())['anchors'];manifest=json.loads((refs/'manifest.json').read_text());rd={r['id']:desc(wavfile.read(refs/(r['id']+'.wav'))[1],48000) for r in manifest['records']}
 full=[];carrier=[]
 # Fixed 32-setting grid chosen by deterministic index; no reference-specific fitting.
 names=list(patches);grid=[]
 for i in range(32):
  base=dict(patches[names[i%len(names)]])
  base['ratio']=(i%8)/7;base['modulation']=((i*3)%8)/7;base['feedback']=((i*5)%8)/7;base['mod_env']=((i*7)%8)/7;base['pitch_hz']=[55,110,220,440][i%4];grid.append(base)
 for i,p in enumerate(grid):
  y,_=render(exe,out,f'delivery-full-{i}',p);full.append(desc(y))
  q=dict(p);q['modulation']=0;q['feedback']=0;y,_=render(exe,out,f'delivery-carrier-{i}',q);carrier.append(desc(y))
 nearest_full={k:min(dist(v,d) for d in full) for k,v in rd.items()};nearest_carrier={k:min(dist(v,d) for d in carrier) for k,v in rd.items()}
 # 48/96 rate comparison for clean and extreme patches; high-rate is polyphase-downsampled.
 rate=[]
 for name,p in [('clean',dict(patches['Pure'])),('bright',dict(patches['Acid']))]:
  lo,_=render(exe,out,'rate-'+name+'-48',p,48000,2.0);hi,_=render(exe,out,'rate-'+name+'-96',p,96000,2.0);down=resample_poly(hi.astype(float),1,2,window=('kaiser',10.))[:len(lo)];sl=slice(4800,min(len(lo),48000));rel=20*np.log10(max(1e-15,np.sqrt(np.mean((lo[sl]-down[sl])**2)))/max(1e-15,np.sqrt(np.mean(down[sl]**2))));rate.append({'patch':name,'relative_rms_db':float(rel)})
 perf=[]
 for block in (32,64,128,512):
  vals=[json.loads(cmd([bench,block])) for _ in range(4)];perf.append({'block':block,'runs':vals,'median_p50_us':float(np.median([v['p50_us'] for v in vals])),'max_new_allocations':max(v['ordinary_new_allocations_in_compute'] for v in vals)})
 result={'controlled_reference_coverage':{'full_pm_mean':float(np.mean(list(nearest_full.values()))),'carrier_only_mean':float(np.mean(list(nearest_carrier.values()))),'full_by_reference':nearest_full,'carrier_by_reference':nearest_carrier,'warning':'same fixed 32-setting grid; broad unknown-settings descriptor comparison, not clone score'},'rate_consistency_not_alias_proof':rate,'four_voice_benchmark':perf,'generated_header':str(generated),'passed':all(p['max_new_allocations']==0 for p in perf)}
 (out/'delivery.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps({'passed':result['passed'],'full_pm_mean':result['controlled_reference_coverage']['full_pm_mean'],'carrier_only_mean':result['controlled_reference_coverage']['carrier_only_mean'],'rate':rate,'p50':[x['median_p50_us'] for x in perf]}))
if __name__=='__main__':main()
