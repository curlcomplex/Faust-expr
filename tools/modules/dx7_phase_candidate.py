"""#96 slice 3: turn the phase x2 finding into a versioned candidate and test C01-C03.

This is still a comparison candidate, not a change to the Faust libraries.
"""
from __future__ import annotations
import argparse, json, math, os, shlex, time, traceback
from pathlib import Path
import numpy as np
import lab
import dx7_slice1 as h1
import dx7_phase_probe as phase

ROOT=lab.ROOT
RATE=44100
FRAMES=65536
GATE_ON=2048
GATE_OFF=32768
NOTE=57
VELOCITY=100
VERSION='dx7-phase-candidate-288.1'

CASES=(
    {'id':'DX7-C01','family':'C01','note':NOTE,'carrier_level':80,'modulator_level':0},
    {'id':'DX7-C02','family':'C02','note':NOTE,'carrier_level':80,'modulator_level':70},
    {'id':'DX7-C03','family':'C03','note':NOTE,'carrier_level':80,'modulator_level':70},
)


def rms(x):
    x=np.asarray(x,dtype=np.float64)
    return float(np.sqrt(np.mean(x*x))) if len(x) else 0.0


def shape(x):
    x=np.asarray(x,dtype=np.float64)
    f=np.abs(np.fft.rfft(x*np.hanning(len(x))))
    return f/max(float(np.linalg.norm(f)),1e-30)


def distance(a,b):
    return float(np.linalg.norm(shape(a)-shape(b)))


def window_metrics(faust, candidate, msfa):
    # Windows intentionally avoid event boundaries for spectral-shape comparison.
    windows={
        'attack':(GATE_ON+512,GATE_ON+4096),
        'sustain':(GATE_ON+8192,GATE_OFF-2048),
        'release':(GATE_OFF+512,min(FRAMES,GATE_OFF+12288)),
    }
    result={}
    for name,(a,b) in windows.items():
        result[name]={
            'stock_shape_l2':distance(faust[a:b],msfa[a:b]),
            'candidate_shape_l2':distance(candidate[a:b],msfa[a:b]),
            'stock_rms':rms(faust[a:b]),
            'candidate_rms':rms(candidate[a:b]),
            'msfa_rms':rms(msfa[a:b]),
        }
    return result


def write_audition(path, msfa, stock, candidate):
    # Fixed common attenuation, no per-engine normalization.
    gap=np.zeros(int(RATE*0.10),dtype=np.float32)
    audio=np.concatenate((msfa.astype(np.float32)*0.2,gap,
                          stock.astype(np.float32)*0.2,gap,
                          candidate.astype(np.float32)*0.2))
    h1.float_wav(path,audio,RATE)


def execute(out,faust,libraries,archive):
    out=out.resolve(); out.mkdir(parents=True,exist_ok=True)
    if any(out.iterdir()): raise ValueError('use a fresh output directory')
    report={'schema':1,'version':VERSION,'status':'running','checks':[],'cases':{}}
    def check(name,ok,**detail):
        report['checks'].append({'name':name,'passed':bool(ok),**detail})
        if not ok: raise AssertionError(name+': '+str(detail))
    started=time.monotonic()
    try:
        faust=Path(faust).resolve(); libraries=Path(libraries).resolve(); archive=Path(archive).resolve()
        check('faust-2.88',lab.run([str(faust),'-v']).splitlines()[0]=='FAUST Version '+h1.FAUST_VERSION)
        check('libraries',bool(h1.verify_release_libraries(libraries,archive)))

        deps=out/'deps'; deps.mkdir(); msfa=deps/'msfa'
        h1.checkout(msfa,'google/music-synthesizer-for-android',h1.MSFA)
        native=msfa/'app/src/main/jni'; oracle=out/'msfa-render'
        cmd=[os.environ.get('CXX','c++'),*h1.CPP_FLAGS,'-I'+str(native),
             str(ROOT/'tools/modules/dx7_msfa_oracle.cpp'),*[str(native/s) for s in h1.SOURCES],'-o',str(oracle)]
        lab.run(cmd,timeout=120); check('msfa-build',oracle.is_file())

        shim=out/'faust-pinned'
        shim.write_text('#!/bin/sh\nexec '+shlex.quote(str(faust))+' -I '+shlex.quote(str(libraries))+' -I '+shlex.quote(str(libraries/'dx7'))+' "$@"\n')
        shim.chmod(0o755)
        worker=lab.Lab(out/'faust-builds'); worker.faust=str(shim)
        listening=out/'listening'; listening.mkdir(); cases_dir=out/'cases'; cases_dir.mkdir()
        hz=440*2**((NOTE-69)/12)

        rendered={}
        for c in CASES:
            d=cases_dir/c['id']; d.mkdir()
            packed,expected,_=h1.patch_data(c); (d/'patch128.bin').write_bytes(packed)
            score=d/'msfa.tsv'; score.write_text(f'0\tnote\t{NOTE}\n0\tvelocity\t{VELOCITY}\n{GATE_ON}\tgate\t1\n{GATE_OFF}\tgate\t0\n')
            mraw=d/'msfa.f32'
            mdiag=json.loads(lab.run([str(oracle),str(d/'patch128.bin'),str(score),str(mraw),str(RATE),'128',str(FRAMES),'0']))
            check(c['id']+':patch',mdiag['unpacked_patch']==expected)
            msfa_audio=np.fromfile(mraw,dtype='<f4')

            fscore=d/'faust.tsv'; fscore.write_text(f'0\tfreq\t{hz:.12g}\n{GATE_ON}\tgate\t1\n{GATE_OFF}\tgate\t0\n')
            stock_src=d/'stock.dsp'; stock_src.write_text(h1.faust_source(c,libraries))
            cand_src=d/'candidate.dsp'; cand_src.write_text(phase.source_for(c,2.0))
            stock_bin=worker.build(c['id']+'-stock',stock_src)
            cand_bin=worker.build(c['id']+'-phase2',cand_src)
            sraw=d/'stock.f32'; craw=d/'candidate.f32'
            sdiag=json.loads(lab.run([str(stock_bin),str(fscore),str(sraw),str(RATE),'128',str(FRAMES),'0']))
            cdiag=json.loads(lab.run([str(cand_bin),str(fscore),str(craw),str(RATE),'128',str(FRAMES),'0']))
            stock=np.fromfile(sraw,dtype='<f4'); candidate=np.fromfile(craw,dtype='<f4')
            check(c['id']+':valid',sdiag['channels']==1 and cdiag['channels']==1 and len(stock)==FRAMES and len(candidate)==FRAMES and len(msfa_audio)==FRAMES and np.isfinite(stock).all() and np.isfinite(candidate).all() and np.isfinite(msfa_audio).all())

            if c['family']=='C01':
                check('C01:candidate-is-stock',np.array_equal(stock,candidate),max_abs=float(np.max(np.abs(stock-candidate))))

            wm=window_metrics(stock,candidate,msfa_audio)
            overall_stock=h1.compare(stock,msfa_audio,{'sample_rate':RATE,'frames':FRAMES,'gate_on':GATE_ON,'gate_off':GATE_OFF},c['family'])
            overall_cand=h1.compare(candidate,msfa_audio,{'sample_rate':RATE,'frames':FRAMES,'gate_on':GATE_ON,'gate_off':GATE_OFF},c['family'])
            report['cases'][c['id']]={
                'family':c['family'],
                'windows':wm,
                'stock_overall_shape_l2':overall_stock['steady_normalized_magnitude_l2'],
                'candidate_overall_shape_l2':overall_cand['steady_normalized_magnitude_l2'],
                'stock_release_to_minus40db_s':overall_stock['release_to_minus40db_s'],
                'candidate_release_to_minus40db_s':overall_cand['release_to_minus40db_s'],
                'stock_envelope_unit_peak_rmse':overall_stock['envelope_unit_peak_rmse'],
                'candidate_envelope_unit_peak_rmse':overall_cand['envelope_unit_peak_rmse'],
            }
            write_audition(listening/(c['id']+'-MSFA-stock-phase2.wav'),msfa_audio,stock,candidate)
            rendered[c['id']]=(stock,candidate,msfa_audio)

        c02=report['cases']['DX7-C02']; c03=report['cases']['DX7-C03']
        check('C02:phase2-improves-steady-shape',c02['candidate_overall_shape_l2'] < c02['stock_overall_shape_l2'])
        # C03 is the new qualification target: require improvement in at least two of attack/sustain/release,
        # while retaining all actual values even if one window exposes a remaining mismatch.
        improved=[name for name,v in c03['windows'].items() if v['candidate_shape_l2'] < v['stock_shape_l2']]
        check('C03:phase2-improves-most-dynamic-windows',len(improved)>=2,improved=improved)
        report['c03_improved_windows']=improved
        report['status']='passed-candidate'
    except Exception as e:
        report['status']='failed'; report['error']=str(e); (out/'failure.txt').write_text(traceback.format_exc())
        raise
    finally:
        report['wall_seconds']=time.monotonic()-started
        report['counts']={'checks':len(report['checks']),'passed':sum(x['passed'] for x in report['checks'])}
        (out/'results.json').write_text(json.dumps(report,indent=2,allow_nan=False)+'\n')
        lines=['# DX7 phase x2 candidate','',f"Status: **{report['status']}**",'']
        for cid,r in report.get('cases',{}).items():
            lines += [f'## {cid}',f"stock steady shape: {r['stock_overall_shape_l2']:.8f}",f"phase x2 steady shape: {r['candidate_overall_shape_l2']:.8f}"]
            if cid=='DX7-C03':
                for name,v in r['windows'].items(): lines.append(f"{name}: stock {v['stock_shape_l2']:.8f}, phase x2 {v['candidate_shape_l2']:.8f}")
            lines.append('')
        lines += ['Listening files are MSFA -> stock Faust -> phase x2, with one fixed gain for all three.']
        (out/'REPORT.md').write_text('\n'.join(lines)+'\n'); print('\n'.join(lines),flush=True)

if __name__=='__main__':
    p=argparse.ArgumentParser(); p.add_argument('--out',type=Path,required=True); p.add_argument('--faust',type=Path,required=True); p.add_argument('--libraries',type=Path,required=True); p.add_argument('--archive',type=Path,required=True); a=p.parse_args(); execute(a.out,a.faust,a.libraries,a.archive)
