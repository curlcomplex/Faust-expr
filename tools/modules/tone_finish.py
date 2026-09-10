"""Finish the Tone 0.2 delivery with all controls and audible gate comparison.
Run the complete qualification first. No changes to the accepted DSP equations.
"""
from __future__ import annotations
import argparse,json
from pathlib import Path
import numpy as np
from scipy.io import wavfile
from scipy.signal import resample_poly
from tone_playable import Study,ROOT,sha,write_wav

class Delivery(Study):
    def run(self):
        super().run()
        exe=self.out/'scalar/render'
        events=[];order=self.man['musical_control_order']
        for j,key in enumerate(order):
            for i in range(8):
                n=2400+j*144000+i*18000
                patch=self.patches['Keys']|{'pitch_hz':110,'gate_mode':0,'decay':.2}
                patch[key]=55*16**(i/7) if key=='pitch_hz' else i/7
                events.extend([(n,k,v) for k,v in patch.items()]+self.hit(n,64))
        self.audio('tone-controls.wav',self.render('all-controls',exe,events=events,seconds=25))
        self.report['auditions']['tone-controls.wav']={'order':order,'seconds_per_control':3,'steps':8,'processing':'fixed gain; PCM16 only; controls updated before each attack'}
        modes=[]
        for mode in (0,1):
            p=self.patches['Keys']|{'gate_mode':mode,'decay':.45,'punch':0}
            modes.append(self.render('articulation-'+str(mode),exe,p,self.hit(2400,67200),seconds=5))
        self.audio('tone-articulation.wav',np.concatenate([modes[0],np.zeros(24000,dtype=np.float32),modes[1]]))
        self.report['auditions']['tone-articulation.wav']={'sequence':['one-shot','held until note-off'],'note_length_s':1.4,'gap_s':.5,'processing':'identical patch and fixed gain, PCM16 only'}
        if self.refs:
            selections=self.report['reference_coverage']['results']['full']['nearest'];pieces=[];details=[]
            for key,choice in selections.items():
                rate,r=wavfile.read(self.refs/(key+'.wav'));r=np.asarray(r,dtype=float)
                if r.ndim>1:r=r.mean(axis=1)
                if rate!=48000:r=resample_poly(r,48000,rate)
                ref=r[:72000]
                idx=choice['pool_index'];candidate=np.fromfile(self.out/f'coverage-full-{idx}.f32',dtype='<f4')[101:72101].astype(float)
                rms_ref=float(np.sqrt(np.mean(ref**2)));rms_candidate=float(np.sqrt(np.mean(candidate**2)))
                gain=rms_ref/max(1e-12,rms_candidate)
                pieces.extend([np.pad(ref,(0,72000-len(ref))),np.zeros(9600),candidate*gain,np.zeros(19200)])
                details.append({'reference':key,'candidate_pool_index':idx,'synth_gain':gain,'excerpt_max_seconds':1.5,'candidate_score_preroll_removed_samples':101})
            joined=np.concatenate(pieces);shared=min(1.,.8/max(1e-12,float(np.max(abs(joined)))))
            self.audio('tone-reference-nearest.wav',joined*shared)
            self.report['auditions']['tone-reference-nearest.wav']={'sequence':'reference then closest authored full-PM pool example; not fitted replica','processing':'one RMS gain per synth excerpt, one global attenuation; no EQ/reverb/limiter','shared_gain':shared,'excerpts':details}
        import wave
        for p in self.out.glob('*.wav'):
            with wave.open(str(p)) as f:self.check('delivery-container:'+p.name,len(f.readframes(f.getnframes()))==f.getnframes()*f.getsampwidth()*f.getnchannels())
        self.report['passed']=True
    def save(self,error=None):
        super().save(error)
        self.report['source_files']['tools/modules/tone_finish.py']=sha(ROOT/'tools/modules/tone_finish.py')
        (self.out/'report.json').write_text(json.dumps(self.report,indent=2)+'\n')

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--out',type=Path,required=True);p.add_argument('--references',type=Path);p.add_argument('--replay',type=Path);a=p.parse_args()
    s=Delivery(a.out.resolve(),a.references.resolve() if a.references else None,a.replay.resolve() if a.replay else None);error=None
    try:s.run()
    except Exception as exc:error=str(exc)
    finally:s.save(error)
    if error:raise SystemExit(error)
if __name__=='__main__':main()
