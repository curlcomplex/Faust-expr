"""#96 next slice: isolate Faust modulator amplitude -> carrier phase/index units.

This consumes the executed H1 adapter without changing upstream Faust/MSFA DSP.
It sweeps operator-2 output level and applies diagnostic phase multipliers only
at the OP2 -> OP1 connection. A multiplier is evidence only, not a synth fix.
"""
from __future__ import annotations
import argparse, json, math, os, shlex, tarfile, time, traceback
from pathlib import Path
import numpy as np
import lab
import dx7_slice1 as h1

ROOT=lab.ROOT
LEVELS=(40,55,70,85,95)
SCALES=(0.5,1.0,2.0)
RATE=44100; FRAMES=65536; GATE_ON=2048; GATE_OFF=32768; NOTE=57; VELOCITY=100
VERSION='dx7-phase-index-288.1'


def source_for(c, scale):
    src=h1.faust_source(c,Path('/pinned'))
    if scale==1.0: return src
    needle='process=dx.operator('
    if src.count(needle)!=1: raise ValueError('unexpected carrier source')
    src=src.replace(needle,f'phase_mod=modulator*{scale:.17g};\nprocess=dx.operator(',1)
    tail='modulator,freq,velocity,gate);'
    if src.count(tail)!=1: raise ValueError('unexpected carrier phase argument')
    return src.replace(tail,'phase_mod,freq,velocity,gate);',1)


def spectral_shape(x,start,end):
    y=np.asarray(x[start:end],dtype=np.float64)
    f=np.abs(np.fft.rfft(y*np.hanning(len(y))))
    return f/max(float(np.linalg.norm(f)),1e-30)


def shape_distance(a,b): return float(np.linalg.norm(a-b))


def execute(out,faust,libraries,archive):
    out=out.resolve(); out.mkdir(parents=True,exist_ok=True)
    if any(out.iterdir()): raise ValueError('use a fresh output directory')
    started=time.monotonic(); report={'schema':1,'version':VERSION,'status':'running','levels':list(LEVELS),'scales':list(SCALES),'rows':[],'checks':[]}
    def check(name,ok,**detail):
        report['checks'].append({'name':name,'passed':bool(ok),**detail})
        if not ok: raise AssertionError(name+': '+str(detail))
    try:
        faust=Path(faust).resolve(); libraries=Path(libraries).resolve(); archive=Path(archive).resolve()
        check('faust-release',lab.run([str(faust),'-v']).splitlines()[0]=='FAUST Version '+h1.FAUST_VERSION)
        hashes=h1.verify_release_libraries(libraries,archive); check('libraries-verified',bool(hashes))
        deps=out/'deps'; deps.mkdir(); msfa=deps/'msfa'; h1.checkout(msfa,'google/music-synthesizer-for-android',h1.MSFA)
        native=msfa/'app/src/main/jni'; oracle=out/'msfa-render'
        cmd=[os.environ.get('CXX','c++'),*h1.CPP_FLAGS,'-I'+str(native),str(ROOT/'tools/modules/dx7_msfa_oracle.cpp'),*[str(native/s) for s in h1.SOURCES],'-o',str(oracle)]
        lab.run(cmd,timeout=120); check('msfa-build',oracle.is_file())
        shim=out/'faust-pinned'; shim.write_text('#!/bin/sh\nexec '+shlex.quote(str(faust))+' -I '+shlex.quote(str(libraries))+' -I '+shlex.quote(str(libraries/'dx7'))+' "$@"\n'); shim.chmod(0o755)
        worker=lab.Lab(out/'faust-builds'); worker.faust=str(shim)
        cases=out/'cases'; cases.mkdir(); start=GATE_ON+4096; end=GATE_OFF-2048
        best=[]
        for level in LEVELS:
            c={'id':f'DX7-P{level}','family':'C02','note':NOTE,'carrier_level':80,'modulator_level':level}
            d=cases/f'level-{level}'; d.mkdir(); packed,expected,_=h1.patch_data(c); (d/'patch128.bin').write_bytes(packed)
            score=d/'msfa.tsv'; score.write_text(f'0\tnote\t{NOTE}\n0\tvelocity\t{VELOCITY}\n{GATE_ON}\tgate\t1\n{GATE_OFF}\tgate\t0\n')
            raw=d/'msfa.f32'; diag=json.loads(lab.run([str(oracle),str(d/'patch128.bin'),str(score),str(raw),str(RATE),'128',str(FRAMES),'0']))
            check(f'level-{level}:patch',diag['unpacked_patch']==expected)
            msfa_audio=np.fromfile(raw,dtype='<f4'); ref=spectral_shape(msfa_audio,start,end)
            distances={}
            for scale in SCALES:
                tag=str(scale).replace('.','p'); src=d/f'faust-scale-{tag}.dsp'; src.write_text(source_for(c,scale))
                program=worker.build(f'P{level}-S{tag}',src); fscore=d/f'faust-scale-{tag}.tsv'; fraw=d/f'faust-scale-{tag}.f32'
                hz=440*2**((NOTE-69)/12); fscore.write_text(f'0\tfreq\t{hz:.12g}\n{GATE_ON}\tgate\t1\n{GATE_OFF}\tgate\t0\n')
                fdiag=json.loads(lab.run([str(program),str(fscore),str(fraw),str(RATE),'128',str(FRAMES),'0']))
                audio=np.fromfile(fraw,dtype='<f4'); check(f'level-{level}:scale-{scale}:valid',fdiag['channels']==1 and len(audio)==FRAMES and np.isfinite(audio).all())
                distances[str(scale)]=shape_distance(spectral_shape(audio,start,end),ref)
            winner=min(distances,key=distances.get); best.append(float(winner))
            report['rows'].append({'modulator_level':level,'shape_l2_by_phase_scale':distances,'best_scale':float(winner),'best_l2':distances[winner]})
        # The probe must discriminate at least two scales somewhere; otherwise it did not isolate anything useful.
        spread=max(max(r['shape_l2_by_phase_scale'].values())-min(r['shape_l2_by_phase_scale'].values()) for r in report['rows'])
        check('probe-sensitive-to-phase-scale',spread>0.01,max_distance_spread=spread)
        report['consistent_best_scale']=best[0] if all(x==best[0] for x in best) else None
        report['interpretation']='A consistent winner across levels supports a phase-unit scaling hypothesis; it is not automatically a shipping correction. Hardware/independent-oracle validation remains separate.'
        report['status']='passed-probe'
    except Exception as e:
        report['status']='failed'; report['error']=str(e); (out/'failure.txt').write_text(traceback.format_exc()); raise
    finally:
        report['wall_seconds']=time.monotonic()-started; report['counts']={'checks':len(report['checks']),'passed':sum(x['passed'] for x in report['checks'])}
        (out/'results.json').write_text(json.dumps(report,indent=2,allow_nan=False)+'\n')
        lines=['# DX7 phase/index probe','',f"Status: **{report['status']}**",'', '| Mod level | scale .5 | scale 1 | scale 2 | best |','| ---: | ---: | ---: | ---: | ---: |']
        for r in report.get('rows',[]):
            d=r['shape_l2_by_phase_scale']; lines.append(f"| {r['modulator_level']} | {d['0.5']:.6f} | {d['1.0']:.6f} | {d['2.0']:.6f} | {r['best_scale']:.1f} |")
        lines += ['', 'Distances are unit-normalized steady FFT-magnitude L2 diagnostics, not authenticity scores.', 'No upstream DSP is changed. The only ablation is a versioned multiplier at the Faust OP2 -> OP1 phase connection.']
        (out/'REPORT.md').write_text('\n'.join(lines)+'\n'); print('\n'.join(lines),flush=True)

if __name__=='__main__':
    p=argparse.ArgumentParser(); p.add_argument('--out',type=Path,required=True); p.add_argument('--faust',type=Path,required=True); p.add_argument('--faust-libraries',type=Path,required=True); p.add_argument('--faust-archive',type=Path,required=True); a=p.parse_args(); execute(a.out,a.faust,a.faust_libraries,a.faust_archive)
