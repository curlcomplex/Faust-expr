"""Opt-in diagnostic extension of the existing Lab; no instrument mutation."""
from __future__ import annotations
import json
import os
from pathlib import Path
import numpy as np
from lab import Lab, ROOT, digest, run, metrics, validate_parameters


class ProbeLab(Lab):
    """Generic probe harness independent of any concrete instrument manifest."""
    def __init__(self, out: Path):
        # Do not call Lab.__init__: that legacy lab intentionally binds to
        # modules/kick-pm. Probe qualification is generic infrastructure and must
        # remain usable from a clean analysis-only integration branch.
        self.out = Path(out)
        self.out.mkdir(parents=True, exist_ok=True)
        self.manifest = {'controls': {}}
        self.experiment = {'purpose': 'generic-probe-diagnostics'}
        self.checks: list[dict] = []
        self.renders: list[dict] = []
        self.builds: dict = {}
        self.defaults = {}
        self.cpp = os.environ.get('CXX', 'c++')
        self.faust = os.environ.get('FAUST', 'faust')

    def build(self, name: str, source: Path, vector=False, *, diagnostic=False) -> Path:
        if diagnostic and vector:
            raise ValueError('probe capture requires the scalar diagnostic build')
        d = (self.out/'diagnostic-builds'/name) if diagnostic else self.out/name
        d.mkdir(parents=True, exist_ok=True)
        library_flags = []
        if os.environ.get('FAUST_LIBRARIES'):
            libraries = Path(os.environ['FAUST_LIBRARIES']).resolve()
            if not (libraries/'stdfaust.lib').is_file():
                raise ValueError('FAUST_LIBRARIES has no stdfaust.lib')
            library_flags = ['-I', str(libraries)]
        # Expanded source records the actual resolved dependency expressions.
        run([self.faust, *library_flags, '-e', str(source), '-o', str(d/'expanded.dsp')])
        flags = [*library_flags, '-lang','cpp','-single','-cn','ModuleDSP']
        if vector:
            flags += ['-vec','-lv','0','-vs','32']
        run([self.faust, *flags, str(d/'expanded.dsp'), '-o', str(d/'generated.hpp')])
        cxx_flags = ['-std=c++17', '-O2', '-ffp-contract=off']
        if diagnostic:
            cxx_flags += ['-DFAUST_EXPR_DIAGNOSTIC=1']
        run([self.cpp, *cxx_flags, '-I'+str(d), str(ROOT/'tools/modules/render.cpp'), '-o', str(d/'render')])
        ui = json.loads(run([str(d/'render'), '--ui-json']))
        if ui['probe_count'] and not diagnostic:
            (d/'render').unlink()
            raise ValueError('active probes require diagnostic=True; refusing a clean build')
        (d/'ui.json').write_text(json.dumps(ui, indent=2, sort_keys=True)+'\n')
        self.builds[name] = {'source_sha256': digest(source), 'expanded_sha256': digest(d/'expanded.dsp'),
                             'generated_sha256': digest(d/'generated.hpp'), 'faust_flags': flags,
                             'cxx_flags':cxx_flags, 'mode':'diagnostic' if diagnostic else 'clean',
                             'benchmark_eligible':not diagnostic,
                             'runner_source_sha256':digest(ROOT/'tools/modules/render.cpp'),
                             'ui_sha256':digest(d/'ui.json'), 'binary_sha256':digest(d/'render')}
        return d/'render'

    def render(self, label: str, executable: Path, params=None, *, rate=48000, block=128,
               seconds=2., events=None, input_audio=None, controls=True,
               diagnostic=False, probe_stride=None) -> np.ndarray:
        if probe_stride is not None and (not diagnostic or isinstance(probe_stride, bool)
                                         or not isinstance(probe_stride, int) or not 1 <= probe_stride <= 96000):
            raise ValueError('probe_stride requires diagnostic=True and an integer in 1..96000')
        folder = self.out/'diagnostic-renders' if diagnostic else self.out
        folder.mkdir(parents=True, exist_ok=True)
        frames = round(rate*seconds)
        ev = list(events or [])
        if controls:
            values = self.defaults | (params or {})
            validate_parameters(self.manifest, values)
            ev = [(0,k,v) for k,v in values.items()] + ev
        # Sorting is stable; duplicate (frame,control) pairs are rejected natively.
        ev.sort(key=lambda e:e[0])
        score = folder/(label+'.tsv')
        score.write_text(''.join(f'{n}\t{k}\t{v:.9g}\n' for n,k,v in ev))
        raw = folder/(label+'.f32')
        args=[str(executable),str(score),str(raw),str(rate),str(block),str(frames),'0']
        if input_audio is not None:
            inp=folder/(label+'-input.f32'); np.asarray(input_audio,dtype='<f4').tofile(inp); args.append(str(inp))
        trace = folder/(label+'.probes.jsonl')
        if diagnostic:
            args.append('--diagnostic')
        if probe_stride is not None:
            args += ['--probes', str(trace), '--probe-stride', str(probe_stride)]
        diag=json.loads(run(args))
        probe_evidence = None
        if probe_stride is not None:
            with trace.open() as capture:
                header = json.loads(next(capture))
                trailer = None
                for row in capture:
                    trailer = json.loads(row)
            if not trailer or trailer.get('type') != 'complete' or trailer['frames'] != frames:
                raise RuntimeError('incomplete probe capture')
            probe_evidence = {'path': str(trace), 'sha256': digest(trace),
                              'header': header, 'complete': trailer,
                              'binary_sha256': digest(executable)}
        x=np.fromfile(raw,dtype='<f4').reshape(frames,diag['channels'])
        self.renders.append({'label':label,'score_sha256':digest(score),'raw_sha256':digest(raw),
                             'diagnostics':diag,'metrics':metrics(x,rate),
                             **({'probe_capture':probe_evidence} if probe_evidence else {})})
        self.check(label+':bounded', np.isfinite(x).all() and float(np.abs(x).max()) < .7)
        return x
