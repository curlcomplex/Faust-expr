"""Exact spectral-band knots, compact variable-length cycles, cubic playback.
Shares the established v2 coefficient generator; no new sound design. All 64
harmonics retained, including Shape/Drive endpoints. No files read at playback.
Only half of each cycle is stored: sine-series odd symmetry reconstructs the rest.
"""
from __future__ import annotations
import argparse, hashlib, json
from pathlib import Path
from fractions import Fraction
import numpy as np
from generate_morph_bank import generate
OVERSAMPLE=16
MIN_SIZE=128
HARMONICS=64
BAND_HZ=[float(v) for v in sorted({Fraction(20),Fraction(9000)}|{Fraction(c,n) for n in range(1,65) for c in (16000,19000) if 20<Fraction(c,n)<9000})]
def wave_definition(name,values):
 return name+'=waveform{'+','.join(format(float(x),'.9g') for x in values)+'};\n'
def build_arrays(clean,driven):
 n=np.arange(1,65,dtype=float);dark=1/(1+.08*(n-1)**2)
 frames=np.stack([clean,driven],axis=1);frames=np.stack([frames*dark,frames],axis=1)
 offsets=[];lengths=[];cycles=[];cache={};total=0
 for hz in BAND_HZ:
  rows=(frames*np.clip((19000-n*hz)/3000,0,1)).reshape(-1,64)
  for row in rows:
   nz=np.flatnonzero(np.abs(row)>1e-15);highest=int(nz[-1]+1) if len(nz) else 1
   size=max(MIN_SIZE,1<<(OVERSAMPLE*highest-1).bit_length())
   row=np.where(abs(row)<1e-15,0,row);key=(size,row.tobytes())
   if key not in cache:
    spec=np.zeros(size//2+1,complex);spec[1:65]=-1j*row*size/2
    vals=np.fft.irfft(spec,n=size)[:size//2+1].astype('<f4');cache[key]=(total,size);cycles.append(vals);total+=len(vals)
   offset,size=cache[key];offsets.append(offset);lengths.append(size)
 return np.concatenate(cycles),np.asarray(offsets,np.int32),np.asarray(lengths,np.int32)
def write(out:Path):
 out.mkdir(parents=True,exist_ok=True);clean,driven,_=generate()
 samples,offsets,lengths=build_arrays(clean,driven);samples.tofile(out/'tablebank.f32')
 content='// Generated, immutable, shared half-cycle tables. No runtime files.\n'+f'bandCount={len(BAND_HZ)};\n'
 index=np.maximum(0,np.minimum(len(BAND_HZ)-2,np.searchsorted(BAND_HZ,np.arange(9001),side='right')-1))
 assert np.max(np.bincount(np.floor(BAND_HZ).astype(int)))<=2, 'index needs more corrections'
 for name,values in [('bandIndex',index),('bandFrequencies',BAND_HZ),('tableOffsets',offsets),('tableLengths',lengths),('tableSamples',samples)]:content+=wave_definition(name,values)
 (out/'tablebank.lib').write_text(content)
 info={'schema':1,'bands_hz':BAND_HZ,'band_count':len(BAND_HZ),'frames':8,'shape_endpoints':2,'drive_endpoints':2,'cycles':len(offsets),'unique_cycles':len(set(map(int,offsets))),'floats':len(samples),'sample_bytes':samples.nbytes,'metadata_bytes':offsets.nbytes+lengths.nbytes+4*len(BAND_HZ)+9001*4,'min_cycle':int(lengths.min()),'max_cycle':int(lengths.max()),'offsets':offsets.tolist(),'lengths':lengths.tolist(),'bank_sha256':hashlib.sha256(content.encode()).hexdigest(),'sample_sha256':hashlib.sha256(samples.tobytes()).hexdigest(),'coefficients_sha256':hashlib.sha256(np.asarray([clean,driven],dtype='<f8').tobytes()).hexdigest(),'phase_interpolation':'four-point Catmull-Rom; reflected half-cycle storage uses exact sine-series odd symmetry','frequency_interpolation':'linear Hz between every exact taper breakpoint; no log-band approximation','frequency_taper':'v2 clamp((19000 - harmonic*maxHz)/3000,0,1)','other_interpolation':'same smoothstep Morph and linear Shape/squared Drive endpoints as v2','bounds':'20-8000Hz carrier, up to28cents; fixed19kHz taper for44.1/48/96kHz','memory_note':'generated compiler may retain read-only initializer AND initialized rdtable; inspect both, do not count only sample_bytes','source':'independent canonical v2 generated spectra, no normalization added'}
 (out/'tablebank.json').write_text(json.dumps(info,indent=2)+'\n');return info
if __name__=='__main__':
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--out',type=Path,required=True);a=p.parse_args();i=write(a.out);print(json.dumps({k:i[k] for k in ('band_count','sample_bytes','unique_cycles','bank_sha256')}))
