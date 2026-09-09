"""Run the first actual-Faust kick experiment. Requires Faust, C++17 and NumPy.
No hardware-fit or sonic-acceptance claim is made by a passing run.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time
import wave
import numpy as np

ROOT = Path(__file__).resolve().parents[2]
MODULE = ROOT / 'modules/kick-pm'


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(args: list[str], *, timeout: int = 120) -> str:
    p = subprocess.run(args, cwd=ROOT, capture_output=True, text=True, timeout=timeout)
    if p.returncode:
        raise RuntimeError(f'command failed: {args!r}\n{p.stdout}\n{p.stderr}')
    return p.stdout


def validate_parameters(manifest: dict, params: dict) -> None:
    for name, value in params.items():
        if name not in manifest['controls'] or isinstance(value, bool) or not isinstance(value, (int, float)):
            raise ValueError(f'invalid parameter {name}')
        c = manifest['controls'][name]
        if not math.isfinite(value) or not c['min'] <= value <= c['max']:
            raise ValueError(f'invalid value {name}')
        if c['unit'].startswith('boolean') and value not in (0, 1):
            raise ValueError(f'nonbinary value {name}')


def metrics(x: np.ndarray, rate: int) -> dict:
    x = np.asarray(x, dtype=np.float64)
    if x.ndim == 1:
        x = x[:, None]
    if not len(x) or not np.isfinite(x).all():
        raise ValueError('empty/nonfinite audio')
    peak = float(np.abs(x).max())
    if peak == 0:
        return {'peak': 0., 'rms': 0., 'dc': 0., 'onset_frame': None, 'energy_below_80_fraction': 0.}
    energy = np.mean(x*x, axis=1)
    onset = int(np.flatnonzero(np.max(np.abs(x), axis=1) > peak*0.005)[0])
    # Full power spectrum includes sub-bass. Per-channel powers avoid cancellation.
    p = np.mean(np.abs(np.fft.rfft(x, axis=0))**2, axis=1)
    freq = np.fft.rfftfreq(len(x), 1/rate)
    cum = np.cumsum(energy) / max(float(energy.sum()), 1e-30)
    return {'peak': peak, 'rms': float(np.sqrt(energy.mean())), 'dc': float(np.mean(x)),
            'onset_frame': onset, 'energy_below_80_fraction': float(p[(freq >= 15) & (freq < 80)].sum()/max(p.sum(), 1e-30)),
            'energy_duration_90_s': float(max(0, np.searchsorted(cum, .9)-onset)/rate),
            'maximum_sample_jump': float(np.abs(np.diff(x, axis=0)).max()) if len(x)>1 else 0.}


def multires_distance(a: np.ndarray, b: np.ndarray) -> dict:
    """Diagnostic log spectra; gain-preserving and separately gain-invariant.
    No onset alignment or realism scale. Same rate/length required by caller.
    """
    a, b = np.asarray(a).reshape(-1), np.asarray(b).reshape(-1)
    if a.shape != b.shape or not len(a) or not np.isfinite(a).all() or not np.isfinite(b).all():
        raise ValueError('comparison dimensions/values')
    report = {}
    for n in (256, 1024, 8192):
        def spectrum(x):
            y = np.pad(x, (0, max(0, n-len(x))))
            frames = np.lib.stride_tricks.sliding_window_view(y, n)[::max(1, n//4)]
            return 20*np.log10(np.maximum(np.abs(np.fft.rfft(frames*np.hanning(n), axis=1))/n, 1e-8))
        diff = spectrum(a)-spectrum(b)
        report[str(n)] = {'log_magnitude_rmse_db': float(np.sqrt(np.mean(diff*diff))),
                          'gain_invariant_rmse_db': float(np.sqrt(np.mean((diff-diff.mean())**2)))}
    return report


def fundamental(x: np.ndarray, rate: int) -> float:
    """Full-band peak on a deliberately clean sine fixture, NOT an arbitrary kick f0 detector."""
    y = np.asarray(x).reshape(-1).astype(np.float64)
    y = y-y.mean()
    size = 1 << max(18, (len(y)-1).bit_length())
    mag = np.abs(np.fft.rfft(y*np.hanning(len(y)), n=size))
    mag[0] = 0
    k = int(mag.argmax())
    if 0 < k < len(mag)-1:
        z = np.log(np.maximum(mag[k-1:k+2], 1e-30))
        den = z[0]-2*z[1]+z[2]
        offset = .5*(z[0]-z[2])/den if den else 0
    else:
        offset = 0
    return float((k+offset)*rate/size)


def wav(path: Path, x: np.ndarray, rate: int) -> None:
    x = np.asarray(x)
    if not np.isfinite(x).all() or np.abs(x).max() >= 1:
        raise ValueError('refusing clipped/nonfinite listening file')
    if x.ndim == 1:
        x = x[:, None]
    with wave.open(str(path), 'wb') as f:
        f.setnchannels(x.shape[1]); f.setsampwidth(2); f.setframerate(rate)
        f.writeframes(np.rint(x*32767).astype('<i2').tobytes())


class Lab:
    def __init__(self, out: Path):
        self.out = out
        self.out.mkdir(parents=True, exist_ok=True)
        self.manifest = json.loads((MODULE/'manifest.json').read_text())
        self.experiment = json.loads((MODULE/'experiment.json').read_text())
        self.checks: list[dict] = []
        self.renders: list[dict] = []
        self.builds: dict = {}
        self.defaults = {k:v['default'] for k,v in self.manifest['controls'].items()}
        self.cpp = os.environ.get('CXX', 'c++')
        self.faust = os.environ.get('FAUST', 'faust')

    def check(self, name: str, passed: bool, **details):
        self.checks.append({'name': name, 'passed': bool(passed), **details})
        if not passed:
            raise AssertionError(name+': '+str(details))

    def build(self, name: str, source: Path, vector=False) -> Path:
        d = self.out/name; d.mkdir(exist_ok=True)
        # Expanded source records the actual resolved dependency expressions.
        run([self.faust, '-e', str(source), '-o', str(d/'expanded.dsp')])
        flags = ['-lang','cpp','-single','-cn','ModuleDSP']
        if vector:
            flags += ['-vec','-lv','0','-vs','32']
        run([self.faust, *flags, str(d/'expanded.dsp'), '-o', str(d/'generated.hpp')])
        run([self.cpp, '-std=c++17','-O2','-ffp-contract=off','-I'+str(d), str(ROOT/'tools/modules/render.cpp'), '-o', str(d/'render')])
        self.builds[name] = {'source_sha256': digest(source), 'expanded_sha256': digest(d/'expanded.dsp'),
                             'generated_sha256': digest(d/'generated.hpp'), 'faust_flags': flags,
                             'cxx_flags':['-std=c++17','-O2','-ffp-contract=off'],
                             'binary_sha256':digest(d/'render')}
        return d/'render'

    def render(self, label: str, executable: Path, params=None, *, rate=48000, block=128,
               seconds=2., events=None, input_audio=None, controls=True) -> np.ndarray:
        frames = round(rate*seconds)
        ev = list(events or [])
        if controls:
            values = self.defaults | (params or {})
            validate_parameters(self.manifest, values)
            ev = [(0,k,v) for k,v in values.items()] + ev
        # Sorting is stable; duplicate (frame,control) pairs are rejected natively.
        ev.sort(key=lambda e:e[0])
        score = self.out/(label+'.tsv')
        score.write_text(''.join(f'{n}\t{k}\t{v:.9g}\n' for n,k,v in ev))
        raw = self.out/(label+'.f32')
        args=[str(executable),str(score),str(raw),str(rate),str(block),str(frames),'0']
        if input_audio is not None:
            inp=self.out/(label+'-input.f32'); np.asarray(input_audio,dtype='<f4').tofile(inp); args.append(str(inp))
        diag=json.loads(run(args))
        x=np.fromfile(raw,dtype='<f4').reshape(frames,diag['channels'])
        self.renders.append({'label':label,'score_sha256':digest(score),'raw_sha256':digest(raw),
                             'diagnostics':diag,'metrics':metrics(x,rate)})
        self.check(label+':bounded', np.isfinite(x).all() and float(np.abs(x).max()) < .7)
        return x

    @staticmethod
    def hit(frame, gate_frames=1):
        return [(frame,'gate',1),(frame+gate_frames,'gate',0)]

    def execute(self):
        exe=self.build('scalar',MODULE/'kick.dsp')
        vec=self.build('vector',MODULE/'kick.dsp',True)
        gate_exe=self.build('gate-fixture',ROOT/'tools/modules/gate_fixture.dsp')
        effect_exe=self.build('effect-fixture',ROOT/'tools/modules/effect_fixture.dsp')
        rows=run([str(exe),'--controls']).splitlines()
        self.check('manifest:io',rows[0]=='io\t0\t1')
        actual={r.split('\t')[0]:list(map(float,r.split('\t')[1:])) for r in rows[1:]}
        self.check('manifest:ids',set(actual)==set(self.defaults))
        for name,c in self.manifest['controls'].items():
            self.check('manifest:'+name,np.allclose(actual[name][:3],[c['min'],c['max'],c['default']],rtol=0,atol=1e-6))
        silence=self.render('silence',exe,seconds=.3)
        self.check('silence:exact',np.max(np.abs(silence))==0)
        pitches=[]
        for rate in (44100,48000):
            for hz in (32.7032,52,110):
                p={'frequency_hz':hz,'sweep':0,'square':0,'triangle':0,'drive':0,'decay_s':1.8}
                x=self.render(f'pitch-{rate}-{hz}',exe,p,rate=rate,events=self.hit(rate//10),seconds=1.5)
                measured=fundamental(x[round(.3*rate):round(1.3*rate)],rate)
                pitches.append({'rate':rate,'expected':hz,'measured':measured})
                self.check(f'pitch:{rate}:{hz}',abs(measured-hz)<.75,measured_hz=measured)
        self.pitch_results=pitches
        base=self.experiment['anchors']['harmonic']|{'feedback_mode':1}
        events=self.hit(4800)+self.hit(13237)+[(21003,'square',.9),(24811,'mod_envelope',.2)]+self.hit(29001)
        ref=self.render('segments-reference',exe,base,seconds=1.,events=events)
        for block in (1,32,64,127,256,512):
            x=self.render(f'segments-{block}',exe,base,seconds=1.,block=block,events=events)
            error=float(np.abs(x-ref).max())
            self.check(f'segments:{block}',error<=1e-6,max_error=error)
        x=self.render('vector-parity',vec,base,seconds=1.,events=events)
        self.check('vector:parity',np.abs(x-ref).max()<=.0002,max_error=float(np.abs(x-ref).max()))
        self.check('pretrigger:silence',np.max(np.abs(ref[:4800]))==0)
        short=self.render('short-gate',exe,base,events=self.hit(4800),seconds=1.)
        held=self.render('held-gate',exe,base,events=self.hit(4800,24000),seconds=1.)
        self.check('trigger-only:gate-off-independent',np.array_equal(short,held))
        for velocity in (0,.25,.5):
            x=self.render(f'velocity-{velocity}',exe,base|{'velocity':velocity},events=self.hit(4800),seconds=1.)
            self.check(f'velocity:{velocity}',np.abs(x-short*velocity).max()<=2e-6)
        x=self.render('velocity-latched',exe,base,events=self.hit(4800)+[(12000,'velocity',0)],seconds=1.)
        self.check('velocity:latched',np.array_equal(x,short))
        a=self.render('no-triangle-a',exe,base|{'triangle':0,'feedback_mode':0},events=self.hit(4800),seconds=1.)
        b=self.render('no-triangle-b',exe,base|{'triangle':0,'feedback_mode':1},events=self.hit(4800),seconds=1.)
        self.check('ablation:identical-at-zero-triangle',np.array_equal(a,b))
        for label,p in self.experiment['anchors'].items():
            a=self.render(label+'-a',exe,p|{'feedback_mode':0},events=self.hit(4800))
            b=self.render(label+'-b',exe,p|{'feedback_mode':1},events=self.hit(4800))
            self.check(label+':ablation-effective',float(np.abs(a-b).max())>1e-5)
            self.comparisons[label]={'candidate_difference_not_reference_error':multires_distance(a,b)}
        # Deterministic, authored corner probes; not exhaustive stability proof.
        for i in range(16):
            p={'frequency_hz':200 if i&1 else 20,'decay_s':2.5 if i&2 else .025,
               'sweep':1,'punch':1,'square':1 if i&4 else 0,'triangle':1 if i&8 else 0,
               'drive':1,'mod_envelope':(i%3)/2,'feedback_mode':1}
            self.render(f'corner-{i}',exe,p,seconds=.4,events=self.hit(1001)+self.hit(6003))
        g=self.render('sustained-fixture',gate_exe,controls=False,seconds=.4,events=self.hit(1000,8000))
        self.check('fixture:gate-length',np.max(np.abs(g[:1000]))==0 and np.max(np.abs(g[1000:9000]))>.1 and np.max(np.abs(g[9000:]))==0)
        inp=np.zeros((4800,2),np.float32); inp[123,0]=.8; inp[4777,1]=-.8
        fx=self.render('effect-fixture',effect_exe,controls=False,seconds=.1,input_audio=inp,block=127)
        self.check('fixture:input-channels',np.array_equal(fx,inp*np.array([.5,.25],np.float32)))
        # Native boundary mutation tests (these must fail, not produce audio).
        invalid=['0 missing 1\n','0 drive nan\n','0 drive 2\n','0 punch 0.5\n','-1 gate 1\n',
                 '20 gate 1\n10 gate 0\n','0 gate 0\n0 gate 1\n','1000 gate 1\n','0 drive 0 junk\n']
        for i,text in enumerate(invalid):
            path=self.out/f'invalid-{i}.tsv'; path.write_text(text)
            p=subprocess.run([str(exe),str(path),str(self.out/f'invalid-{i}.f32'),'48000','128','1000','0'],capture_output=True,text=True,timeout=5)
            self.check(f'native-reject:{i}',p.returncode!=0)
        # One persistent instance and one fixed recording gain for the audition.
        audition=[]; timeline=[]
        for j,(label,p) in enumerate(self.experiment['anchors'].items()):
            for mode in (0,1):
                start=(j*2+mode)*4*48000+4800
                audition += [(start,k,v) for k,v in (p|{'feedback_mode':mode,'velocity':1}).items()]
                audition += self.hit(start)+[(start+72000,'velocity',.6)]+self.hit(start+72000)
                timeline.append({'seconds':start/48000,'anchor':label,'candidate':'B-feedback' if mode else 'A-feedforward','second_hit_seconds':(start+72000)/48000})
        sound=self.render('audition',exe,seconds=32,events=audition)
        wav(self.out/'audition.wav',sound,48000)
        self.timeline=timeline
        for mode in (0,1):
            score=[]
            for step in range(32):
                if step%4==0 or step in (7,15,22,23,30,31):
                    p=list(self.experiment['anchors'].values())[(step//8)%4]
                    onset=4800+step*6000
                    score += [(onset,k,v) for k,v in (p|{'feedback_mode':mode,'velocity':1 if step%4==0 else .55}).items()]+self.hit(onset)
            x=self.render(f'pattern-{mode}',exe,seconds=5.,events=score)
            wav(self.out/f'pattern-{mode}.wav',x,48000)

    def save(self, failure=None):
        sources={}
        for path in list(MODULE.glob('*'))+list((ROOT/'tools/modules').glob('*'))+list((ROOT/'tests').glob('test_module_lab.py')):
            if path.is_file():
                rel=path.relative_to(ROOT); sources[str(rel)]=digest(path)
                dest=self.out/'source'/rel; dest.parent.mkdir(parents=True,exist_ok=True); shutil.copyfile(path,dest)
        try:
            commit=run(['git','rev-parse','HEAD']).strip()
        except Exception:
            commit=None
        report={'experiment':'kick-pm-01','source_commit':commit,'source_files':sources,'builds':self.builds,
                'checks':self.checks,'renders':self.renders,'pitch_results':getattr(self,'pitch_results',[]),
                'comparisons':self.comparisons,'timeline':getattr(self,'timeline',[]),
                'faust_version':run([self.faust,'-v']).strip(),'cxx_version':run([self.cpp,'--version']).strip(),
                'passed':failure is None and all(x['passed'] for x in self.checks),'failure':failure,
                'reference_calibrated':False,'human_listening_approved':False,'target_realtime_qualified':False,
                'listening_processing':'PCM16 conversion only; fixed kernel gain, no normalization, EQ, reverb or limiter',
                'timing_scope':'instrumented single-instance offline compute only; not callback or multi-voice target acceptance'}
        (self.out/'results.json').write_text(json.dumps(report,indent=2)+'\n')
        print(json.dumps({'passed':report['passed'],'checks':len(self.checks),'renders':len(self.renders),'failure':failure}))


def main():
    p=argparse.ArgumentParser(description=__doc__); p.add_argument('--out',type=Path,default=ROOT/'build/kick-pm-01'); args=p.parse_args()
    lab=Lab(args.out.resolve()); lab.comparisons={}
    failure=None
    try:
        lab.execute()
    except Exception as e:
        failure=str(e)
    finally:
        lab.save(failure)
    if failure:
        raise SystemExit(failure)

if __name__=='__main__': main()
