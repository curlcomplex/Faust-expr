#!/usr/bin/env python3
"""Actual Faust audio, using the reviewed main renderer on an explicit lane.
Python scores events, mixes independently rendered host voices, measures
samples and writes audition derivatives. It never synthesizes an oscillator.
"""
from __future__ import annotations
import argparse, hashlib, json, os, platform, shutil, subprocess, wave, zipfile
from pathlib import Path
import numpy as np

LAB_SHA = '82926f023410ae8367eec3c5d842edbbe34ab439'
ARCHIVE_SHA = 'e4e175cf236924b5b7d4784cbb8c50cc01e211159e169655dd9e6d8f92b871d9'
RATE = 48000
PLAYBACK_GAIN = 0.25
ROOT = Path(__file__).resolve().parents[2]

def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()

def command(args, log, timeout=180):
    print('RUN', ' '.join(str(x) for x in args), flush=True)
    try:
        p = subprocess.run([str(x) for x in args], capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired as e:
        Path(log).write_text('TIMEOUT\n'+str(e.stdout or '')+'\n'+str(e.stderr or ''))
        raise
    Path(log).write_text(p.stdout + p.stderr)
    if p.returncode:
        raise RuntimeError(f'{Path(str(args[0])).name} failed ({p.returncode}); see {log}\n{p.stderr[-4000:]}')
    return p.stdout

def hz(note):
    return 440.0 * 2.0**((note-69)/12.0)

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument('--lab-root', required=True, type=Path)
    ap.add_argument('--out', required=True, type=Path)
    ap.add_argument('--faust', default=os.environ.get('FAUST') or shutil.which('faust'))
    ap.add_argument('--faust-libraries', type=Path)
    ap.add_argument('--faust-archive', type=Path)
    ap.add_argument('--execution-lane', choices=['owner-mac','github-hosted'], default='owner-mac')
    args=ap.parse_args()
    if args.execution_lane == 'owner-mac':
        if platform.system() != 'Darwin' or platform.machine() != 'arm64':
            raise RuntimeError('owner-mac lane requires macOS ARM64')
    elif platform.system() != 'Linux' or os.environ.get('GITHUB_ACTIONS') != 'true':
        raise RuntimeError('github-hosted lane requires explicit Linux Actions execution')
    if not args.faust:
        raise RuntimeError('Faust 2.88 is unavailable')
    out=args.out.resolve(); out.mkdir(parents=True,exist_ok=False)
    lab=args.lab_root.resolve()
    if command(['git','-C',lab,'rev-parse','HEAD'],out/'lab-sha.txt').strip()!=LAB_SHA:
        raise RuntimeError('Analysis/renderer checkout is not the reviewed main revision')
    if command(['git','-C',lab,'status','--porcelain','--untracked-files=no'],out/'lab-status.txt').strip():
        raise RuntimeError('Tracked main renderer/analysis files were modified')
    revision=command(['git','-C',ROOT,'rev-parse','HEAD'],out/'source-sha.txt').strip()
    if command(['git','-C',ROOT,'status','--porcelain','--untracked-files=no'],out/'source-status.txt').strip():
        raise RuntimeError('Tracked candidate source was modified after checkout')
    ver=command([args.faust,'--version'],out/'faust-version.txt')
    if 'FAUST Version 2.88.0' not in ver.splitlines():
        raise RuntimeError('Expected Faust 2.88.0; refusing another compiler')
    lib=(args.faust_libraries or Path(os.environ.get('FAUST_LIBRARIES') or Path(args.faust).resolve().parent.parent/'share/faust')).resolve()
    if not (lib/'stdfaust.lib').is_file():
        raise RuntimeError('Provide the matching Faust 2.88 libraries')
    archive_verified=False
    if args.faust_archive:
        if sha(args.faust_archive)!=ARCHIVE_SHA: raise RuntimeError('Faust release archive digest mismatch')
        archive_verified=True
    if args.execution_lane=='github-hosted' and not archive_verified:
        raise RuntimeError('Hosted lane requires the verified Faust 2.88 release archive')
    cxx=shutil.which(os.environ.get('CXX','c++'))
    if not cxx: raise RuntimeError('C++ compiler unavailable')
    command([cxx,'--version'],out/'cxx-version.txt')
    rows=[]; checks=[]; outputs={}; builds={}
    def build(version,vector=False,probe=False):
        key=version+('-vector' if vector else '')+('-probe' if probe else '')
        b=out/'build'/key; b.mkdir(parents=True)
        module=ROOT/'modules/jp-8000'/version; dsp=module/'voice.dsp'
        if probe:
            dsp=b/'probe.dsp'
            dsp.write_text('import("stdfaust.lib"); ss=library("supersaw_core.lib");\n'
              'freq=hslider("freq",220,20,8000,.01); detune=hslider("detune",1,0,1,.001); mix=hslider("mix",1,0,1,.001);\n'
              'process=ss.source(freq,detune,mix,1)*0.1;\n')
        flags=['-vec','-vs','32'] if vector else []
        command([args.faust,'-e','-I',lib,'-I',module,dsp,'-o',b/'expanded.dsp'],b/'expand.log')
        command([args.faust,'-lang','cpp','-single','-cn','ModuleDSP',*flags,b/'expanded.dsp','-o',b/'generated.hpp'],b/'faust.log')
        shutil.copyfile(lab/'tools/modules/render.cpp',b/'render.cpp')
        command([cxx,'-std=c++17','-O2','-ffp-contract=off','-I',b,b/'render.cpp','-o',b/'render'],b/'cxx.log')
        ui=json.loads(command([b/'render','--ui-json'],b/'ui.json'))
        if ui['inputs']!=0 or ui['outputs']!=1: raise RuntimeError('Unexpected voice IO')
        builds[key]={'dsp_sha256':sha(dsp),'library_sha256':sha(module/'supersaw_core.lib'),
          'expanded_sha256':sha(b/'expanded.dsp'),'generated_sha256':sha(b/'generated.hpp'),
          'executable_sha256':sha(b/'render'),'faust_flags':['-single',*flags],'cxx_flags':['-O2','-ffp-contract=off']}
        return b/'render'
    def render(binary,tag,events,seconds,rate=RATE,block=64):
        d=out/'raw'; d.mkdir(exist_ok=True)
        score=d/(tag+'.score'); raw=d/(tag+'.f32')
        events=sorted(events,key=lambda e:e[0])
        score.write_text(''.join(f'{round(t*rate)} {name} {value:.10g}\n' for t,name,value in events))
        report=json.loads(command([binary,score,raw,rate,block,round(seconds*rate),0],d/(tag+'.json')))
        data=np.fromfile(raw,dtype='<f4')
        if data.size!=round(seconds*rate) or not np.isfinite(data).all(): raise RuntimeError('Invalid rendered data: '+tag)
        if np.max(np.abs(data))<1e-6: raise RuntimeError('Silent render: '+tag)
        rows.append({'id':tag,'rate':rate,'block':block,'score_sha256':sha(score),'audio_sha256':sha(raw),'render':report})
        return data
    def score(note,detune,mix,seed=1,on=.1,off=3.,attack=.02,release=.8):
        return [(0,'freq',hz(note)),(0,'detune',detune/127),(0,'mix',mix/127),
          (0,'phaseSeed',seed),(0,'attack',attack),(0,'decay',.25),(0,'sustain',.85),
          (0,'release',release),(0,'level',.12),(0,'velocity',.9),(on,'gate',1),(off,'gate',0)]
    def wav(name,data):
        data=np.asarray(data,dtype=np.float64)*PLAYBACK_GAIN
        if not np.isfinite(data).all() or np.max(np.abs(data))>=1:
            raise RuntimeError('Refusing clipped audition '+name)
        d=out/'listening'; d.mkdir(exist_ok=True); p=d/(name+'.wav')
        with wave.open(str(p),'wb') as w:
            w.setnchannels(1); w.setsampwidth(2); w.setframerate(RATE)
            w.writeframes(np.rint(data*32767).astype('<i2').tobytes())
        outputs[name]={'sha256':sha(p),'seconds':len(data)/RATE,'fixed_gain':PLAYBACK_GAIN,'peak':float(np.max(np.abs(data)))}
    v2=build('v2'); v1=build('v1'); vector=build('v2',vector=True)
    wide_score=score(57,112,127,seed=17,off=5.,attack=.04,release=1.5)
    wide=render(v2,'wide-hold',wide_score,7.)
    old=render(v1,'previous-v1-wide-hold',[e for e in wide_score if e[1]!='phaseSeed'],7.)
    wav('01-wide-supersaw',wide)
    wav('02-previous-Faust-then-v2',np.concatenate([old,np.zeros(RATE//2),wide]))
    chord_notes=[[48,55,60,64],[45,52,57,60],[41,48,53,57],[43,50,55,59]]
    progression=[]
    for ci,notes in enumerate(chord_notes):
        parts=[render(v2,f'chord-{ci}-voice-{j}',score(note,104,119,seed=19+ci*17+j*271,off=2.2,attack=.06,release=1.),3.4) for j,note in enumerate(notes)]
        progression.append(np.sum(np.stack(parts),axis=0))
    wav('03-four-note-chords',np.concatenate(progression))
    extremes=[]
    for note in [33,45,57,69,81]:
        extremes.append(render(v2,f'max-register-{note}',score(note,127,127,seed=note+3,off=1.5,attack=.015,release=.5),2.3))
    wav('04-detune127-mix127-registers',np.concatenate(extremes))
    for name,control,values,fixed_d,fixed_m in [('05-upper-detune-sweep','detune',[80,96,112,120,124,127],80,127),('06-mix-sweep-high-detune','mix',[0,32,64,96,127],112,0)]:
        duration=len(values)*1.25+1.2
        ev=score(57,fixed_d,fixed_m,seed=211,off=duration-.9,attack=.015,release=.7)
        ev += [(i*1.25+.2,control,val/127) for i,val in enumerate(values)]
        wav(name,render(v2,name,ev,duration))
    ev=score(57,112,127,seed=83,on=.1,off=.6,release=.25)
    for i in range(1,8): ev.extend([(i*.9+.1,'gate',1),(i*.9+.6,'gate',0)])
    wav('07-repeated-notes',render(v2,'repeated-notes',ev,7.6))
    for rate in (44100,48000,96000):
        ev=score(57,127,127,seed=71,off=.7,release=.2)
        a=render(v2,f'check-{rate}-block64',ev,1.2,rate,64)
        b=render(v2,f'check-{rate}-block257',ev,1.2,rate,257)
        c=render(vector,f'check-{rate}-vector',ev,1.2,rate,64)
        if not np.array_equal(a,b): raise RuntimeError('Block partition changes scalar audio')
        if np.max(np.abs(a-c))>2e-5: raise RuntimeError('Scalar/vector mismatch')
        if np.max(np.abs(a[:round(.09*rate)]))>1e-8: raise RuntimeError('Audio before note-on')
        if np.max(np.abs(a[-round(.15*rate):]))>1e-5: raise RuntimeError('Release did not finish')
        checks.append({'rate':rate,'block_invariant':True,'scalar_vector_peak_error':float(np.max(np.abs(a-c))),'silent_before_note':True,'release_finished':True})
    repeat=render(v2,'repeatability-check',wide_score,7.)
    if not np.array_equal(wide,repeat): raise RuntimeError('Cold replay changed')
    probe=build('v2',probe=True)
    raw=render(probe,'seven-fundamentals',[(0,'freq',220),(0,'detune',1),(0,'mix',1)],8.)
    segment=raw[RATE:]; spectrum=np.abs(np.fft.rfft(segment*np.hanning(len(segment))))
    axis=np.fft.rfftfreq(len(segment),1/RATE)
    offsets=[0,-.11002313,-.06288439,-.01952356,.01991221,.06216538,.10745242]
    peaks=[]
    for offset in offsets:
        target=220*(1+offset); mask=np.flatnonzero(np.abs(axis-target)<.6)
        k=mask[np.argmax(spectrum[mask])]
        if abs(axis[k]-target)>.3 or spectrum[k]<1.: raise RuntimeError('Missing/incorrect supersaw component')
        peaks.append({'expected_hz':target,'observed_hz':float(axis[k])})
    if len({round(x['observed_hz'],2) for x in peaks})!=7: raise RuntimeError('Collapsed supersaw voices')
    manifest={'schema':2,'status':'rendered-and-checked','source_sha':revision,'lab_sha':LAB_SHA,
      'execution_lane':args.execution_lane,'run_id':os.environ.get('GITHUB_RUN_ID'),
      'machine':{'system':platform.system(),'architecture':platform.machine()},
      'compiler_sha256':sha(Path(args.faust).resolve()),'faust_version':'2.88.0','release_archive_verified':archive_verified,
      'renderer_sha256':sha(lab/'tools/modules/render.cpp'),
      'library_sha256':{n:sha(lib/n) for n in ['stdfaust.lib','oscillators.lib','filters.lib','envelopes.lib']},
      'sample_rate':RATE,'outputs':outputs,'renders':rows,'checks':checks,'fundamentals':peaks,'builds':builds,
      'scope':'actual Faust v2 source audition; v1 comparison is prior Faust, not hardware/oracle',
      'limitations':['HPF and phase lifecycle are candidate choices, not hardware-calibrated','no firmware/hardware oracle was rendered','Table 2 interpolated between published points','no FX; chord polyphony is external summation of actual Faust voice renders']}
    (out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    (out/'LISTENING.txt').write_text(f'Actual Faust 2.88.0. Lane: {args.execution_lane}. Source: {revision}. All examples dry/mono.\n'
      '01: Detune 112 / Mix 127 held note.\n02: Previous Faust v1, gap, new v2 (NOT hardware reference).\n'
      '03: Four-note chord progression, Detune 104 / Mix 119, four independent Faust instances.\n'
      '04: Detune 127 / Mix 127, MIDI notes 33,45,57,69,81.\n'
      '05: Upper detune values 80,96,112,120,124,127 at Mix 127.\n'
      '06: Mix 0,32,64,96,127 at Detune 112.\n07: Repeated identical notes; free-running seeded phases.\n'
      'Same fixed playback gain 0.25 on every WAV; no normalization, limiting, EQ or reverb.\n')
    with zipfile.ZipFile(out/'supersaw-listening.zip','w',zipfile.ZIP_DEFLATED) as z:
        for p in sorted((out/'listening').glob('*.wav')): z.write(p,'listening/'+p.name)
        z.write(out/'LISTENING.txt','LISTENING.txt'); z.write(out/'manifest.json','manifest.json')
    print(json.dumps({'status':'passed','renders':len(rows),'listening_files':len(outputs),'fundamentals':peaks}),flush=True)

if __name__=='__main__':
    main()
