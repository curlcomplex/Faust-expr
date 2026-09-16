#!/usr/bin/env python3
"""#97: actual-Faust wave/voice qualification; unchanged ymfm table oracle.
Uses the existing native renderer, score format, and lab metrics/WAV writer.
No hardware, chip-clock, OPZ envelope, full-routing or fidelity certification.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import tarfile
import numpy as np
from lab import metrics, wav

ROOT = Path(__file__).resolve().parents[2]
PIN = '81aec25ccbb98f4873a255f7551ac4dadac59b4a'
RENDERER_SHA = '8a0ff6cf77960d0b5c61fbb9f8831681e6dc05bd1ac8fd20718244f507f94e02'
ARCHIVE_SHA = 'e4e175cf236924b5b7d4784cbb8c50cc01e211159e169655dd9e6d8f92b871d9'
# Declared before execution. Analytic sine vs quantized, half-indexed log table.
TABLE_TOLERANCE = .01
VECTOR_TOLERANCE = 2e-4
PLAYBACK_GAIN = .5


def sha(p):
    return hashlib.sha256(Path(p).read_bytes()).hexdigest()


def identity(directory, pattern='*'):
    return {str(p.relative_to(directory)): sha(p) for p in sorted(directory.rglob(pattern)) if p.is_file()}


def compare_table(candidate, reference, tolerance=TABLE_TOLERANCE):
    a, b = np.asarray(candidate), np.asarray(reference)
    if a.shape != (1024,) or b.shape != a.shape or not np.isfinite(a).all() or not np.isfinite(b).all():
        raise ValueError('expected two finite 1024-point phase grids')
    error = float(np.max(np.abs(a-b)))
    return {'passed': error <= tolerance, 'max_abs': error,
            'rms_error': float(np.sqrt(np.mean((a-b)**2))), 'tolerance': tolerance}


class Qualification:
    def __init__(self, args):
        self.a = args
        self.out = args.out.resolve()
        if self.out.exists():
            raise ValueError('use a fresh output directory; never overwrite evidence')
        self.out.mkdir(parents=True)
        self.commands, self.checks, self.renders, self.builds = [], [], [], {}
        self.module = ROOT/'modules/tx81z'
        self.initial_source = identity(self.module)
        self.library_identity = identity(args.libraries, '*.lib')
        self.report = {'schema': 'faust-expr/tx81z-wave-qualification/v1', 'status': 'running',
                       'execution_lane': os.getenv('EXECUTION_LANE', 'assistant-sandbox'),
                       'source_commit': os.getenv('SOURCE_COMMIT'), 'platform': platform.platform(),
                       'oracle': {'status': 'not_run'}, 'checks': self.checks, 'renders': self.renders,
                       'builds': self.builds, 'commands': self.commands,
                       'source_sha256': self.initial_source, 'libraries_sha256': self.library_identity,
                       'playback_gain': PLAYBACK_GAIN,
                       'scope': 'Analytic OPZ waveform construction and provisional serial voice; not complete TX81Z.'}

    def run(self, args, cwd=ROOT, timeout=180):
        args = [str(a) for a in args]
        p = subprocess.run(args, cwd=cwd, capture_output=True, text=True, timeout=timeout)
        index = len(self.commands)
        (self.out/f'command-{index:03d}.stdout').write_text(p.stdout)
        (self.out/f'command-{index:03d}.stderr').write_text(p.stderr)
        self.commands.append({'argv': args, 'cwd': str(cwd), 'returncode': p.returncode,
                              'stdout': f'command-{index:03d}.stdout', 'stderr': f'command-{index:03d}.stderr'})
        if p.returncode:
            raise RuntimeError(f'command failed {args}: {p.stderr[-4000:]}')
        return p.stdout

    def check(self, name, passed, **detail):
        self.checks.append({'name': name, 'passed': bool(passed), **detail})
        if not passed:
            raise AssertionError(f'{name}: {detail}')

    def build(self, name, source, vector=False):
        d = self.out/name; d.mkdir()
        inc = ['-I', self.a.libraries, '-I', source.parent]
        self.run([self.a.faust, *inc, '-e', source, '-o', d/'expanded.dsp'])
        flags = ['-lang', 'cpp', '-single', '-cn', 'ModuleDSP']
        if vector:
            flags += ['-vec', '-lv', '0', '-vs', '32']
        self.run([self.a.faust, *inc, *flags, d/'expanded.dsp', '-o', d/'generated.hpp'])
        shutil.copy2(self.a.renderer, d/'render.cpp')
        self.run([self.a.cxx, '-std=c++17', '-O2', '-ffp-contract=off', '-I'+str(d),
                  d/'render.cpp', '-o', d/'render'])
        controls = self.run([d/'render', '--controls'])
        (d/'controls.tsv').write_text(controls)
        self.builds[name] = {'files': identity(d), 'source': str(source), 'source_sha256': sha(source),
                             'faust_flags': flags}
        return d/'render'

    def render(self, name, exe, params=None, events=(), rate=48000, seconds=1.5, block=128, input_data=None):
        frames = len(input_data) if input_data is not None else round(rate*seconds)
        ev = [(0, k, v) for k, v in (params or {}).items()] + list(events)
        ev.sort(key=lambda e: e[0])
        score = self.out/(name+'.tsv')
        score.write_text(''.join(f'{i}\t{k}\t{v:.12g}\n' for i,k,v in ev))
        raw = self.out/(name+'.f32')
        cmd = [exe, score, raw, rate, block, frames, 0]
        input_info = None
        if input_data is not None:
            ip = self.out/(name+'-input.f32'); np.asarray(input_data, dtype='<f4').tofile(ip)
            input_info = {'file': ip.name, 'sha256': sha(ip)}; cmd.append(ip)
        diag = json.loads(self.run(cmd))
        if diag['channels'] != 1:
            raise ValueError('TX81Z qualification expects one output channel')
        x = np.fromfile(raw, dtype='<f4').reshape(frames, diag['channels'])[:,0]
        self.check(name+':finite', np.isfinite(x).all())
        self.renders.append({'name': name, 'raw': raw.name, 'raw_sha256': sha(raw),
                             'score': score.name, 'score_sha256': sha(score), 'input': input_info,
                             'diagnostics': diag, 'metrics': metrics(x, rate)})
        return x

    def audition(self, name, arrays, gap=.2):
        silence = np.zeros(round(48000*gap))
        x = np.concatenate([np.concatenate([a, silence]) for a in arrays])*PLAYBACK_GAIN
        wav(self.out/'listening'/name, x, 48000)

    def execute(self):
        self.check('renderer:pinned-unchanged', sha(self.a.renderer) == RENDERER_SHA)
        version = self.run([self.a.faust, '--version'])
        self.check('faust:2.88.0', 'FAUST Version 2.88.0\n' in version)
        self.report['compiler'] = {'version': version, 'sha256': sha(self.a.faust),
                                   'cxx_version': self.run([self.a.cxx, '--version'])}
        if self.a.archive:
            self.check('archive:sha256', sha(self.a.archive) == ARCHIVE_SHA)
            with tarfile.open(self.a.archive) as t:
                members = {m.name.split('/libraries/',1)[1]: hashlib.sha256(t.extractfile(m).read()).hexdigest()
                           for m in t.getmembers() if m.isfile() and '/libraries/' in m.name and m.name.endswith('.lib')}
            self.check('libraries:match-complete-release', members == self.library_identity)
            self.report['library_verification'] = 'complete official release archive'
        else:
            self.report['library_verification'] = 'retained PR115 artifact; full archive not rechecked in this run'
        scalar = self.build('v2', self.module/'v2/voice.dsp')
        vector = self.build('v2-vector', self.module/'v2/voice.dsp', True)
        # Frozen draft v1 never compiled: ba.select2 is invalid Faust syntax.
        # Preserve its equations in a syntax-repaired, factored control.
        compat = self.out/'v1-syntax-control'; compat.mkdir()
        shutil.copy2(self.module/'v1/voice.dsp',compat/'voice.dsp')
        original = (self.module/'v1/opz_core.lib').read_text().replace('ba.select2(', 'select2(')
        (compat/'preserved-core.lib').write_text(original)
        factored = (self.module/'v2/opz_core.lib').read_text().replace('select2(n>=6, base, abs(base))', 'base')
        (compat/'opz_core.lib').write_text(factored)
        self.report['v1_control'] = 'uncompiled v1 equations: selector syntax repair and common-sine factoring; phase-grid identity checked separately'
        old = self.build('v1', compat/'voice.dsp')
        tables, oldtables = [], []
        phase = np.arange(1024, dtype=np.float32)/1024
        for variant, destination in [('v1', oldtables), ('v2', tables)]:
            src = self.out/f'{variant}-phase.dsp'
            core = compat/'preserved-core.lib' if variant == 'v1' else self.module/'v2/opz_core.lib'
            src.write_text(f'opz=library("{core}");\nprocess=_ <: opz.waveAt(_,nentry("wave",0,0,7,1));\n')
            exe = self.build(variant+'-phase', src)
            for w in range(8):
                destination.append(self.render(f'{variant}-table-{w}', exe, {'wave': w}, input_data=phase))
            if variant == 'v2':
                for shift in [-2, 3]:
                    wrapped = self.render(f'phase-wrap-{shift}', exe, {'wave': 6}, input_data=phase+shift)
                    self.check(f'phase-wrap-{shift}:identity', np.array_equal(wrapped, destination[6]))
        src = self.out/'v1-factored-phase.dsp'
        src.write_text(f'opz=library("{compat/"opz_core.lib"}");\nprocess=_ <: opz.waveAt(_,nentry("wave",0,0,7,1));\n')
        ex = self.build('v1-factored-phase', src)
        for w in range(8):
            result = self.render(f'v1-factored-table-{w}',ex,{'wave':w},input_data=phase)
            self.check(f'v1-factoring:{w}:identity',np.array_equal(result,oldtables[w]))
        for i in range(8):
            for j in range(i):
                self.check(f'eight-distinct:{i}:{j}', not np.array_equal(tables[i],tables[j]))
            self.check(f'table-{i}:bounded', np.max(np.abs(tables[i])) <= 1.000001)
            if i < 6:
                self.check(f'preserved-wave:{i}', np.array_equal(tables[i], oldtables[i]))
        self.check('v1:duplicate-negative-control', np.array_equal(oldtables[4],oldtables[6]) and np.array_equal(oldtables[5],oldtables[7]))
        for w in [6,7]:
            self.check(f'wave-{w}:rectified', np.min(tables[w]) >= 0 and np.min(tables[w-2]) < -.99)
        if self.a.ymfm:
            source = self.a.ymfm.resolve()
            self.check('ymfm:pinned', self.run(['git','rev-parse','HEAD'], source).strip() == PIN)
            before = self.run(['git','status','--porcelain','--untracked-files=no'], source)
            self.check('ymfm:clean', not before.strip())
            refexe = self.out/'ymfm-wave-reference'
            self.run([self.a.cxx,'-std=c++17','-O2','-I'+str(source/'src'),
                      ROOT/'tools/modules/ymfm_opz_wave_reference.cpp', source/'src/ymfm_opz.cpp', '-o',refexe])
            text = self.run([refexe]); (self.out/'ymfm-tables.csv').write_text(text)
            self.check('ymfm:cold-repeat', self.run([refexe]) == text)
            rows = np.loadtxt(self.out/'ymfm-tables.csv', delimiter=',', skiprows=1)
            self.check('ymfm:table-shape', rows.shape == (8192,4))
            self.check('ymfm:phase-order', np.array_equal(rows[:,0],np.repeat(np.arange(8),1024)) and np.array_equal(rows[:,1],np.tile(np.arange(1024),8)))
            reference = rows[:,3].reshape(8,1024)
            comparisons = []
            for w in range(8):
                result = compare_table(tables[w], reference[w]); comparisons.append(result)
                self.check(f'ymfm-wave:{w}', **result)
            for w in [6,7]:
                self.check(f'ymfm:reject-v1-duplicate-{w}', not compare_table(oldtables[w],reference[w])['passed'])
            self.check('ymfm:reject-gain-mutant', not compare_table(tables[0]*.5,reference[0])['passed'])
            self.check('ymfm:unchanged', self.run(['git','status','--porcelain','--untracked-files=no'], source) == before)
            self.report['oracle'] = {'status':'completed', 'kind':'opz_registers waveform tables; not chip audio',
                                     'commit':PIN, 'source_sha256':identity(source/'src'), 'binary_sha256':sha(refexe),
                                     'csv_sha256':sha(self.out/'ymfm-tables.csv'), 'comparisons':comparisons}
        (self.out/'listening').mkdir()
        gate = [(2400,'gate',1),(36000,'gate',0)]
        carriers, modulators = [], []
        for w in range(8):
            carriers.append(self.render(f'carrier-{w}', scalar, {'op1Wave':w,'op2Level':0}, gate))
            modulators.append(self.render(f'modulator-{w}',scalar,{'op2Wave':w,'op2Level':.2,'op3Level':0},gate))
            self.check(f'carrier-{w}:audible', np.max(np.abs(carriers[-1]))>.01)
            self.check(f'modulator-{w}:audible', np.max(np.abs(modulators[-1]))>.01)
        self.audition('01-eight-carriers-0-to-7.wav',carriers)
        self.audition('02-eight-modulators-0-to-7.wav',modulators)
        p = {'op1Wave':6,'op2Wave':7,'op2Level':.18,'op3Level':.13,'freq':110}
        repeated = [(2400,'gate',1),(17003,'gate',0),(19001,'gate',1),(34011,'gate',0)]
        base = self.render('lifecycle-reference',scalar,p,repeated)
        for block in [1,127,256,511]:
            x=self.render(f'block-{block}',scalar,p,repeated,block=block)
            self.check(f'block-{block}:identical',np.array_equal(base,x))
        x=self.render('cold-repeat',scalar,p,repeated)
        self.check('cold:identical',np.array_equal(base,x))
        x=self.render('vector-parity',vector,p,repeated)
        self.check('vector:parity',np.max(np.abs(base-x))<=VECTOR_TOLERANCE,max_abs=float(np.max(np.abs(base-x))))
        self.check('startup:silent',np.max(np.abs(base[:2400]))==0)
        self.check('release:audible-tail',np.max(np.abs(base[34011:37000]))>.001)
        self.check('release:decayed',np.max(np.abs(base[-4800:]))<.00001)
        x=self.render('zero-velocity',scalar,p|{'velocity':0},repeated)
        self.check('velocity-zero:silent',np.max(np.abs(x))==0)
        for rate in [44100,96000]:
            x=self.render(f'rate-{rate}',scalar,p,[(rate//20,'gate',1),(rate//2,'gate',0)],rate=rate)
            self.check(f'rate-{rate}:audible',np.max(np.abs(x))>.01)
        self.audition('03-v1-then-v2-wave6.wav',[self.render('previous-wave6',old,{'op1Wave':6,'op2Level':0},gate),carriers[6]])
        programs = [
            ('04-rectified-bass.wav',{'op1Wave':6,'op2Wave':1,'op2Level':.12,'op3Level':0,'attack':.002,'decay':.15,'sustain':.18,'release':.12},[36,36,43,39,36,46,43,34],.25),
            ('05-glass-sequence.wav',{'op1Wave':0,'op2Wave':7,'op2Ratio':3.5,'op2Level':.32,'op3Level':.1,'attack':.002,'decay':.6,'sustain':.05,'release':.6},[60,67,63,72,70,67,63,58],.5),
            ('06-hollow-keys.wav',{'op1Wave':7,'op2Wave':4,'op2Ratio':1.5,'op2Level':.09,'op3Level':0,'attack':.01,'decay':.35,'sustain':.4,'release':.35},[48,55,60,63,58,55,51,46],.4)]
        for filename, params, notes, step in programs:
            events=[]
            for i,n in enumerate(notes):
                t=2400+round(i*step*48000)
                events.extend([(t,'freq',440*2**((n-69)/12)),(t,'gate',1),(t+round(step*.68*48000),'gate',0)])
            x=self.render(filename[:-4],scalar,params,events,seconds=len(notes)*step+1)
            self.audition(filename,[x])
        self.check('sources:unchanged',self.initial_source==identity(self.module))
        self.check('libraries:unchanged',self.library_identity==identity(self.a.libraries,'*.lib'))
        self.report['listening'] = identity(self.out/'listening')
        self.report['status'] = 'completed'

    def save(self):
        self.report['check_count'] = len(self.checks)
        self.report['render_count'] = len(self.renders)
        (self.out/'results.json').write_text(json.dumps(self.report,indent=2)+'\n')
        (self.out/'SHA256SUMS.json').write_text(json.dumps(identity(self.out),indent=2)+'\n')


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ['out','faust','libraries','renderer']:
        p.add_argument('--'+name,type=Path,required=True)
    p.add_argument('--ymfm',type=Path); p.add_argument('--archive',type=Path)
    p.add_argument('--cxx',default=os.getenv('CXX','c++'))
    a=p.parse_args()
    for name in ['faust','libraries','renderer','ymfm','archive']:
        if getattr(a,name) is not None: setattr(a,name,getattr(a,name).resolve())
    q=Qualification(a)
    try: q.execute()
    except Exception as e:
        q.report['status']='failed'; q.report['error']=str(e); raise
    finally: q.save()
    print(json.dumps({'status':q.report['status'],'checks':len(q.checks),'renders':len(q.renders),'oracle':q.report['oracle']['status']}))


if __name__=='__main__': main()
