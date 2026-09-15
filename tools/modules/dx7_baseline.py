"""#96/#112 H0/H1: three DX7 diagnostic families, no upstream DSP edits.

Use the existing Lab.build/render.cpp and qualified lab metrics. This is a
restricted MSFA adapter, not a replacement renderer or a full DX7 instrument.
"""
from __future__ import annotations
import argparse
import copy
import json
import math
import os
from pathlib import Path
import re
import shlex
import shutil
import struct
import subprocess
import sys
import tarfile
import time
import traceback

import numpy as np
import lab

ROOT = lab.ROOT
BRIEF = ROOT / 'modules/dx7/baseline-01'
FAUSTLIBS = '271228a08981fa10b07732f0861421d1e20d4022'
MSFA = 'f67d41d313b7dc85f6fb99e79e515cc9d208cfff'
ADAPTER_VERSION = 'dx7-baseline-01.2'
# C++17 cmath imports std::min/max into the legacy core's unqualified lookup.
# C++11 avoids that collision; stddef.h supplies its omitted size_t declaration.
# These are build-compatibility flags, not upstream source or DSP modifications.
CPP_FLAGS = ['-std=c++11', '-include', 'stddef.h', '-O2', '-ffp-contract=off', '-fwrapv']
SOURCES = ['dx7note.cc', 'fm_core.cc', 'fm_op_kernel.cc', 'env.cc',
           'freqlut.cc', 'exp2.cc', 'sin.cc', 'pitchenv.cc', 'patch.cc']


def write_json(path, data):
    Path(path).write_text(json.dumps(data, indent=2, allow_nan=False) + '\n')


def validate_suite(suite):
    required = {'schema','sample_rate','frames','gate_on','gate_off','velocity','cases'}
    if set(suite) != required or suite['schema'] != 1:
        raise ValueError('unknown case schema/fields')
    for field in required - {'cases'}:
        if type(suite[field]) is not int:
            raise ValueError('integer field required: '+field)
    if suite['sample_rate'] != 44100 or not 0 <= suite['gate_on'] < suite['gate_off'] < suite['frames'] <= 441000:
        raise ValueError('unsupported rate/timing')
    if any(suite[k] % 64 for k in ('gate_on','gate_off','frames')):
        raise ValueError('MSFA events must be 64-frame aligned')
    if not 1 <= suite['velocity'] <= 127 or not suite['cases']:
        raise ValueError('invalid velocity or empty cases')
    ids=set()
    for c in suite['cases']:
        if set(c) != {'id','family','note','carrier_level','modulator_level'}:
            raise ValueError('unknown case fields')
        if not isinstance(c['id'],str) or not re.fullmatch(r'[A-Za-z0-9-]+',c['id']) or c['id'] in ids:
            raise ValueError('invalid/duplicate case id')
        ids.add(c['id'])
        if c['family'] not in ('C01','C02','C03'):
            raise ValueError('unknown diagnostic family')
        for k,hi in [('note',127),('carrier_level',99),('modulator_level',99)]:
            if type(c[k]) is not int or not 0 <= c[k] <= hi:
                raise ValueError('invalid '+k)
        if c['family']=='C01' and c['modulator_level'] != 0:
            raise ValueError('C01 must isolate carrier')


def operator_data(c, number):
    active=number==1 or (number==2 and c['family']!='C01')
    rates=[99,99,99,99]
    levels=[99,99,99,0] if active else [0,0,0,0]
    level=c['carrier_level'] if number==1 else c['modulator_level'] if number==2 else 0
    if c['family']=='C03' and active:
        rates=[85,55,45,65] if number==1 else [90,60,50,65]
        levels=[99,80,70,0] if number==1 else [99,50,30,0]
    return {'number':number,'active':active,'rates':rates,'levels':levels,
            'level':level,'coarse':2 if number==2 else 1}


def patch_data(c):
    """Generate original packed DX7 voice; operator storage order is 6 down to 1.
    Independently expected 156-byte state is checked against upstream UnpackPatch.
    """
    packed=[]; expected=[]
    for number in range(6,0,-1):
        o=operator_data(c,number)
        packed += o['rates']+o['levels']+[0,0,0,0,7<<3,0,o['level'],o['coarse']<<1,0]
        expected += o['rates']+o['levels']+[0,0,0,0,0,0,0,0,o['level'],0,o['coarse'],0,7]
    name=('DX '+c['family']).ljust(10).encode('ascii')
    packed += [99]*4+[50]*4+[0,8,35,0,0,0,1,24]+list(name)
    expected += [99]*4+[50]*4+[0,0,1,35,0,0,0,1,0,0,24]+list(name)+[63]
    if len(packed)!=128 or len(expected)!=156 or max(packed)>127:
        raise ValueError('patch encoder length/range')
    # Single-voice SysEx is an export for later hardware use, not transported by
    # this native adapter. The native adapter tests the packed-voice route.
    payload=expected[:155]
    sysex=bytes([240,67,0,0,1,27]+payload+[(-sum(payload))&127,247])
    return bytes(packed), expected, sysex


def faust_source(c, libraries):
    def operator(number, phase):
        o=operator_data(c,number)
        args=[0,o['coarse'],0,0,o['level'],*o['rates'],*o['levels'],
              0,0,0,0,0,0,0,0, 0,35,0,0,0,1,0,1,
              99,99,99,99,50,50,50,50,0,phase,'freq','velocity','gate']
        assert len(args)==42
        return 'dx.operator('+','.join(map(str,args))+')'
    mod=operator(2,'0') if c['family']!='C01' else '0'
    return ('// Diagnostic projection of stock dx7 algorithm 1: OP2 -> OP1.\n'
            '// Inactive operators are omitted; no output gain or DSP correction.\n'
            'declare name "DX7 baseline '+c['id']+'";\n'
            'dx=library('+json.dumps(str(libraries/'dx7/dx7.lib'))+');\n'
            'gate=button("gate");\n'
            'freq=hslider("freq[unit:Hz]",220,8,20000,.001);\n'
            'velocity=hslider("velocity",1,0,1,.001);\n'
            'modulator='+mod+';\n'
            'process='+operator(1,'modulator')+';\n')


def float_wav(path, audio, rate):
    """IEEE float WAV preserves raw values, including values above full scale."""
    a=np.asarray(audio,dtype='<f4').reshape(-1)
    if not len(a) or not np.isfinite(a).all():
        raise ValueError('invalid WAV samples')
    data=a.tobytes()
    fmt=struct.pack('<HHIIHH',3,1,rate,rate*4,4,32)
    fact=struct.pack('<I',len(a))
    body=b'WAVE'+b'fmt '+struct.pack('<I',len(fmt))+fmt+b'fact'+struct.pack('<I',4)+fact+b'data'+struct.pack('<I',len(data))+data
    Path(path).write_bytes(b'RIFF'+struct.pack('<I',len(body))+body)


def envelope(x, hop=256):
    x=np.asarray(x,dtype=np.float64).reshape(-1)
    n=len(x)//hop
    return np.sqrt(np.mean(x[:n*hop].reshape(n,hop)**2,axis=1))


def compare(a, b, suite, family):
    """Raw comparisons first. Shape-only views are clearly separate diagnostics."""
    a=np.asarray(a,dtype=np.float64).reshape(-1)
    b=np.asarray(b,dtype=np.float64).reshape(-1)
    if a.shape!=b.shape or not len(a) or not np.isfinite(a).all() or not np.isfinite(b).all():
        raise ValueError('invalid comparison')
    start=suite['gate_on']+4096; end=suite['gate_off']-2048
    aa=a[start:end]; bb=b[start:end]
    ea,eb=envelope(a),envelope(b)
    rms=lambda x: float(np.sqrt(np.mean(x*x)))
    ratio=rms(aa)/max(rms(bb),1e-30)
    def shape(x):
        f=np.abs(np.fft.rfft(x*np.hanning(len(x))))
        return f/max(float(np.linalg.norm(f)),1e-30)
    result={'faust':lab.metrics(a,suite['sample_rate']),
            'msfa_core':lab.metrics(b,suite['sample_rate']),
            'raw_residual_rms':rms(a-b),
            'steady_faust_over_msfa_db':20*math.log10(max(ratio,1e-30)),
            'steady_normalized_magnitude_l2':float(np.linalg.norm(shape(aa)-shape(bb))),
            'envelope_raw_rmse':rms(ea-eb),
            'envelope_unit_peak_rmse':rms(ea/max(float(ea.max()),1e-30)-eb/max(float(eb.max()),1e-30)),
            'multires_diagnostic':lab.multires_distance(a,b),
            'raw_residual_is_not_a_fidelity_gate':True,
            'waveform_alignment':'none', 'comparison_gain':'none',
            'scope':'MSFA core Q24 converted to float vs stock Faust operator output; not hardware DAC levels'}
    if family=='C01':
        result['carrier_peak_hz']={'faust':lab.fundamental(aa,suite['sample_rate']),
                                   'msfa':lab.fundamental(bb,suite['sample_rate'])}
    off=suite['gate_off']; hop=256
    def release_seconds(e):
        pre=float(e[max(0,off//hop-2):off//hop].max())
        ix=np.flatnonzero(e[off//hop:]<=pre*.01)
        return float(ix[0]*hop/suite['sample_rate']) if len(ix) else None
    result['release_to_minus40db_s']={'faust':release_seconds(ea),'msfa':release_seconds(eb)}
    return result


def checkout(destination, repo, commit):
    if destination.exists():
        raise ValueError('dependency directory must be fresh')
    lab.run(['git','init','-q',str(destination)])
    lab.run(['git','-C',str(destination),'remote','add','origin','https://github.com/'+repo+'.git'])
    lab.run(['git','-C',str(destination),'fetch','-q','--depth=1','origin',commit],timeout=180)
    lab.run(['git','-C',str(destination),'checkout','-q','--detach','FETCH_HEAD'])
    if lab.run(['git','-C',str(destination),'rev-parse','HEAD']).strip()!=commit:
        raise RuntimeError('dependency commit mismatch')


def plot_case(path, a, b, suite, title):
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    fig, ax=plt.subplots(figsize=(10,3.5))
    t=(np.arange(len(envelope(a)))+.5)*256/suite['sample_rate']
    ax.plot(t,envelope(a),label='stock Faust')
    ax.plot(t,envelope(b),label='MSFA core (Q24 / 2^24)')
    ax.axvline(suite['gate_off']/suite['sample_rate'],linestyle='--',label='note off')
    ax.set(title=title,xlabel='Seconds',ylabel='RMS, raw engine units')
    ax.legend(); fig.tight_layout(); fig.savefig(path,dpi=150); plt.close(fig)


def execute(out):
    out=out.resolve(); out.mkdir(parents=True,exist_ok=True)
    if (out/'results.json').exists() or (out/'deps').exists():
        raise ValueError('use a fresh output directory; never replace baseline evidence')
    started=time.monotonic()
    suite=json.loads((BRIEF/'cases.json').read_text()); validate_suite(suite)
    report={'schema':1,'adapter_version':ADAPTER_VERSION,'status':'running','checks':[],
            'comparisons':{},'renders':[],'builds':{},'scope':'H0/H1 only; no synthesis edits',
            'vdx7':'not run','hardware':'not acquired/not run','jp8000_h2':'not run',
            'lineage':'Faust DX7 is Dexed/MSFA-derived. These are related implementations, not independent hardware confirmations.'}
    def check(name, passed, **detail):
        report['checks'].append({'name':name,'passed':bool(passed),**detail})
        if not passed: raise AssertionError(name+': '+str(detail))
    try:
        report['commit']=lab.run(['git','rev-parse','HEAD']).strip()
        report['platform']=lab.run(['uname','-a']).strip()
        report['faust_version']=lab.run([os.environ.get('FAUST','faust'),'-v']).strip()
        report['cxx_version']=lab.run([os.environ.get('CXX','c++'),'--version']).splitlines()[0]
        deps=out/'deps'; deps.mkdir()
        libraries=deps/'faustlibraries'; msfa=deps/'msfa'
        checkout(libraries,'grame-cncm/faustlibraries',FAUSTLIBS)
        checkout(msfa,'google/music-synthesizer-for-android',MSFA)
        report['dependencies']={'faustlibraries':FAUSTLIBS,'google_msfa':MSFA}
        check('stock-operator-blob',lab.run(['git','-C',str(libraries),'hash-object','dx7/operator.lib']).strip()=='63f8746a6d61fe0a828df3d25378e1e559f38416')
        refs=out/'references'; refs.mkdir()
        for name,d in [('faustlibraries',libraries),('msfa',msfa)]:
            archive=refs/(name+'-source.tar.gz')
            lab.run(['git','-C',str(d),'archive','--format=tar.gz','-o',str(archive),'HEAD'])
        shim=out/'faust-pinned'
        compiler=shutil.which(os.environ.get('FAUST','faust'))
        if compiler is None: raise RuntimeError('Faust not found')
        shim.write_text('#!/bin/sh\nexec '+shlex.quote(compiler)+' -I '+shlex.quote(str(libraries))+' -I '+shlex.quote(str(libraries/'dx7'))+' "$@"\n')
        shim.chmod(0o755)
        worker=lab.Lab(out/'faust-builds'); worker.faust=str(shim)
        native=msfa/'app/src/main/jni'; exe=out/'msfa-render'
        command=[os.environ.get('CXX','c++'),*CPP_FLAGS,'-I'+str(native),str(ROOT/'tools/modules/dx7_msfa_render.cpp'),*[str(native/s) for s in SOURCES],'-o',str(exe)]
        build_start=time.monotonic(); lab.run(command,timeout=120)
        report['msfa_build']={'command':command,'seconds':time.monotonic()-build_start,'binary_sha256':lab.digest(exe),
            'source_sha256':{s:lab.digest(native/s) for s in SOURCES},'mode':'original scalar MSFA core, no app filter/clip/output pad'}
        check('msfa-build',exe.is_file())
        # Preserve exact local source; no private checkout and no source writes.
        lab.run(['git','archive','--format=tar.gz','-o',str(out/'faust-expr-source.tar.gz'),'HEAD'])
        audit=out/'audition'; audit.mkdir()
        rawdir=out/'cases'; rawdir.mkdir()
        arrays={}
        def render_faust(c,d,program,label,block=128,rate=44100):
            hz=440*2**((c['note']-69)/12)
            score=d/(label+'.tsv'); raw=d/(label+'.f32')
            score.write_text(f"0\tfreq\t{hz:.12g}\n0\tvelocity\t{suite['velocity']/127:.12g}\n{suite['gate_on']}\tgate\t1\n{suite['gate_off']}\tgate\t0\n")
            cmd=[str(program),str(score),str(raw),str(rate),str(block),str(suite['frames']),'0']
            diag=json.loads(lab.run(cmd))
            x=np.fromfile(raw,dtype='<f4')
            check(c['id']+':'+label+':valid',diag['channels']==1 and len(x)==suite['frames'] and np.isfinite(x).all())
            report['renders'].append({'case':c['id'],'engine':'faust','label':label,'command':cmd,'raw_sha256':lab.digest(raw),'score_sha256':lab.digest(score),'diagnostics':diag})
            return x
        def render_msfa(c,d,label,block=128):
            raw=d/(label+'.f32'); score=d/'msfa.tsv'
            cmd=[str(exe),str(d/'patch128.bin'),str(score),str(raw),'44100',str(block),str(suite['frames']),'0']
            diag=json.loads(lab.run(cmd))
            x=np.fromfile(raw,dtype='<f4')
            check(c['id']+':'+label+':patch',diag['unpacked_patch']==patch_data(c)[1])
            check(c['id']+':'+label+':valid',len(x)==suite['frames'] and np.isfinite(x).all())
            report['renders'].append({'case':c['id'],'engine':'msfa','label':label,'command':cmd,'raw_sha256':lab.digest(raw),'score_sha256':lab.digest(score),'diagnostics':diag})
            return x
        for c in suite['cases']:
            d=rawdir/c['id']; d.mkdir()
            packed,expected,sysex=patch_data(c)
            (d/'patch128.bin').write_bytes(packed); (d/'patch.syx').write_bytes(sysex)
            write_json(d/'expected-unpacked.json',expected)
            (d/'msfa.tsv').write_text(f"0\tnote\t{c['note']}\n0\tvelocity\t{suite['velocity']}\n{suite['gate_on']}\tgate\t1\n{suite['gate_off']}\tgate\t0\n")
            source=d/'baseline.dsp'; source.write_text(faust_source(c,libraries))
            program=worker.build(c['id'],source)
            a=render_faust(c,d,program,'faust'); b=render_msfa(c,d,'msfa')
            check(c['id']+':faust-cold-repeat',np.array_equal(a,render_faust(c,d,program,'faust-repeat')))
            check(c['id']+':msfa-cold-repeat',np.array_equal(b,render_msfa(c,d,'msfa-repeat')))
            check(c['id']+':faust-block127',np.array_equal(a,render_faust(c,d,program,'faust-block127',127)))
            check(c['id']+':msfa-block127',np.array_equal(b,render_msfa(c,d,'msfa-block127',127)))
            check(c['id']+':audible',float(np.max(np.abs(a)))>1e-4 and float(np.max(np.abs(b)))>1e-4)
            # Silence differences in stock operator initialization are diagnostic,
            # not suppressed by an extra gate or claimed as a synthesis fix.
            result=compare(a,b,suite,c['family']); report['comparisons'][c['id']]=result
            write_json(d/'comparison.json',result)
            write_json(d/'case.json',dict(c,suite={k:v for k,v in suite.items() if k!='cases'},operators=[operator_data(c,n) for n in (1,2)],adapter_version=ADAPTER_VERSION))
            float_wav(d/'faust-raw.wav',a,44100); float_wav(d/'msfa-core-raw.wav',b,44100)
            arrays[c['id']]=(a,b)
            if c['id'] in ('C01-n57-l80','C02-m70','C03-articulated'):
                # One fixed gain for every file, no per-engine normalization.
                audition=np.concatenate([b,np.zeros(4410),a])*.2
                lab.wav(audit/(c['id']+'-MSFA-then-Faust.wav'),audition,44100)
                plot_case(audit/(c['id']+'-envelope.png'),a,b,suite,c['id'])
        report['builds']=worker.builds
        # Infrastructure negative controls compare an engine with its own output,
        # so a pre-existing Faust/MSFA mismatch cannot make a broken test pass.
        base=arrays['C01-n57-l80'][0]
        for name,x in [('gain',base*.5),('delay',np.concatenate([np.zeros(64),base[:-64]])),('mute',np.zeros_like(base))]:
            check('negative-control:'+name,float(np.max(np.abs(base-x)))>1e-4,expected_rejection=True)
        # Changes in real native patch bytes must change the actual oracle output.
        check('oracle:output-level-observable',np.max(np.abs(arrays['C01-n57-l60'][1]-arrays['C01-n57-l99'][1]))>.01)
        check('oracle:modulator-observable',np.max(np.abs(arrays['C02-m50'][1]-arrays['C02-m90'][1]))>.01)
        check('faust:modulator-observable',np.max(np.abs(arrays['C02-m50'][0]-arrays['C02-m90'][0]))>.01)
        chosen=suite['cases'][2]; d=rawdir/chosen['id']
        bad=d/'bad-event.tsv'; bad.write_text('0\tnote\t57\n0\tvelocity\t100\n2049\tgate\t1\n32768\tgate\t0\n')
        p=subprocess.run([str(exe),str(d/'patch128.bin'),str(bad),str(d/'rejected.f32'),'44100','128',str(suite['frames']),'0'],capture_output=True,text=True,timeout=10)
        check('native:reject-unaligned-event',p.returncode!=0,stderr=p.stderr.strip())
        for name,dep in [('faust',libraries),('msfa',msfa)]:
            check('upstream-unchanged:'+name,lab.run(['git','-C',str(dep),'status','--porcelain']).strip()=='')
        report['status']='passed-infrastructure; baseline-differences-not-gated'
        report['audition']={'order':'MSFA then Faust','fixed_gain_all_files':.2,'gap_seconds':.1,'normalization':'none'}
    except Exception as error:
        report['status']='failed'; report['error']=str(error)
        (out/'failure.txt').write_text(traceback.format_exc())
        raise
    finally:
        report['wall_seconds']=time.monotonic()-started
        report['counts']={'checks':len(report['checks']),'passed':sum(c['passed'] for c in report['checks']),'renders':len(report['renders'])}
        write_json(out/'results.json',report)
        lines=['# DX7 baseline 01','',f"Commit: `{report.get('commit','unknown')}`",f"Status: **{report['status']}**",'',
               'Stock Faust operators versus the original Google MSFA core. No DSP edits. No hardware/VDX7 run.',
               'MSFA raw values are Q24 / 2^24, before the app filter and output attenuation. Faust retains its own per-operator 0.5 gain. Raw level differences are therefore not a hardware-output calibration.',
               '', '| Case | Faust / MSFA steady dB | Unit-magnitude L2 | Envelope unit-peak RMSE |','| --- | ---: | ---: | ---: |']
        for name,r in report['comparisons'].items():
            lines.append(f"| {name} | {r['steady_faust_over_msfa_db']:.5f} | {r['steady_normalized_magnitude_l2']:.6f} | {r['envelope_unit_peak_rmse']:.6f} |")
        lines += ['', '## Limits', 'One fresh voice per run; one gate-on/off; algorithm-1 OP2-to-OP1 subgraph; no feedback, scaling, LFO, full algorithm coverage, hardware DAC or target-device performance claim.',
                  '44100 Hz only: original MSFA amplitude envelopes have no rate compensation. Its 64-frame envelope/gain cadence is preserved. Faust evaluates its envelope per sample; pointwise null tests are not a fidelity gate.',
                  'Related implementation lineage prevents treating agreement as independent hardware confirmation. Oracle differences are retained; the harness does not tune either engine.',
                  '', '## Counts', json.dumps(report['counts']), '', 'Full commands, source hashes, native unpacked patches, event files, raw float audio and plots are in the companion artifact.']
        (out/'REPORT.md').write_text('\n'.join(lines)+'\n')
        hashes={str(p.relative_to(out)):lab.digest(p) for p in out.rglob('*') if p.is_file() and 'deps' not in p.relative_to(out).parts and p.name!='SHA256SUMS.json'}
        write_json(out/'SHA256SUMS.json',hashes)
        print('\n'.join(lines),flush=True)
        print('RESULT_SUMMARY '+json.dumps({'status':report['status'],'counts':report['counts'],'comparisons':report['comparisons']},allow_nan=False),flush=True)


if __name__=='__main__':
    parser=argparse.ArgumentParser(); parser.add_argument('--out',type=Path,required=True)
    execute(parser.parse_args().out)
