"""Independently authored wavetable cycles and equivalent Fourier coefficients.
No sample/firmware extraction. Generation is deterministic for a pinned NumPy
build; the generated bank bytes/digest are part of each sound release.
"""
from __future__ import annotations
import argparse, hashlib, json
from pathlib import Path
import numpy as np
HARMONICS=64
CYCLE_SIZE=8192
NAMES=['Sine','Soft','Saw','Hollow','Vowel','Organ','Glass','Edge']

def generate():
 n=np.arange(1,HARMONICS+1,dtype=float);odd=(n.astype(int)%2)
 c=np.stack([(n==1).astype(float),odd/n**2,1/n,odd/n,
             .4*(n==1)+np.exp(-.18*(n-5)**2)+.5*np.exp(-.07*(n-13)**2),
             .9*(n==1)+.6*(n==2)+.4*(n==4)+.25*(n==8)+.2*(n==16),
             .25*(n==1)+odd*np.exp(-.07*(n-9)**2),
             .7*(n==1)+1/(n**.65)])
 phase=2*np.pi*np.arange(CYCLE_SIZE)/CYCLE_SIZE
 basis=np.sin(n[:,None]*phase)
 clean=[];driven=[];cycles=[]
 for row in c:
  row=row*(.75/np.max(np.abs(row@basis)))
  waveform=row@basis
  sat=np.tanh(4*waveform)
  coeff=-np.fft.rfft(sat).imag[1:HARMONICS+1]*2/CYCLE_SIZE
  coeff*=.75/np.max(np.abs(coeff@basis))
  clean.append(row);driven.append(coeff);cycles.extend([waveform,coeff@basis])
 return np.array(clean),np.array(driven),np.array(cycles)

def write(out:Path):
 out.mkdir(parents=True,exist_ok=True);clean,driven,cycles=generate()
 lines=['// GENERATED from generate_morph_bank.py. Do not hand edit.','import("stdfaust.lib");']
 for name,values in [('clean',clean),('driven',driven)]:
  for i,row in enumerate(values):
   lines.append(f'{name}{i}(n)=('+','.join(f'{v:.17e}' for v in row)+'):ba.selectn(64,n-1);')
  lines.append(name+'(k,n)=('+','.join(f'{name}{i}(n)' for i in range(8))+'):ba.selectn(8,k);')
 content='\n'.join(lines)+'\n';(out/'bank.lib').write_text(content)
 cycles.astype('<f4').tofile(out/'wavetable-cycles.f32')
 info={'schema':1,'names':NAMES,'harmonics':HARMONICS,'cycle_size':CYCLE_SIZE,
       'cycle_order':'interleaved clean/driven per named frame',
       'clean_coefficients':clean.tolist(),'driven_coefficients':driven.tolist(),
       'bank_sha256':hashlib.sha256(content.encode()).hexdigest(),
       'cycles_sha256':hashlib.sha256((out/'wavetable-cycles.f32').read_bytes()).hexdigest(),
       'normalization':'Each authored frame endpoint is peak calibrated to .75 offline. No output/per-hit normalization.',
       'drive':'FFT of tanh(4*clean) truncated to 64 sine harmonics and peak calibrated; runtime blends coefficients before pitch band limiting.',
       'rendering':'Analytic Fourier evaluation, not runtime table lookup; the same cycles are exported for audit/other consumers.'}
 (out/'bank.json').write_text(json.dumps(info,indent=2)+'\n')
 return info
if __name__=='__main__':
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--out',type=Path,required=True)
 a=p.parse_args();print(json.dumps({'bank_sha256':write(a.out)['bank_sha256']}))
