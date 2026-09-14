"""Single-file Faust review delivery with verified expansion/audio parity.
Review exports are pinned snapshots, not automatic factory promotion.
"""
from pathlib import Path
import argparse, json, os, re, tempfile, traceback
import numpy as np
from synth_batch import SynthLab, ROOT, COMMON, DEFAULTS, phrase
from synth_four_voice_checkpoint import DEFAULT as J60_DEFAULT
from acid_batch import controls
from hats_v2_delivery import command, digest
SUBJECTS={
 'Juno-60':('modules/juno-60/v3/voice.dsp',J60_DEFAULT),
 'Juno-106':('modules/juno-106/v3/voice.dsp',DEFAULTS['juno106']),
 'SH-101':('modules/mono-101/v4/voice.dsp',DEFAULTS['mono101']),
 'Mini':('modules/minimoog/v3/voice.dsp',DEFAULTS['mini']),
}
def run(out):
    lab=SynthLab(out);(out/'scripts').mkdir(exist_ok=True);(out/'audition').mkdir(exist_ok=True);c=lab.check
    manifest=dict(schema=1,commit=os.getenv('GITHUB_SHA','local'),delivery='Single self-contained Faust source per instrument',
        purpose='Pinned review snapshot; not shipping approval',instrument_freeze_complete=False,
        hardware_approved=False,owner_approved=False,subjects={})
    try:
        for name,(path,base) in SUBJECTS.items():
            src=(ROOT/path).resolve();canonical=lab.build(name+'-canonical',src)
            with tempfile.TemporaryDirectory(prefix='faust-single-script-') as temp:
                work=Path(temp);wrapper=work/(name+'.dsp');wrapper.write_text('import('+json.dumps(str(src))+');\n')
                command(['faust','-e','-I',src.parent,'-I',COMMON,wrapper,'-o',work/'expanded.dsp'])
                expanded=(work/'expanded.dsp').read_text()
                c(name+':no-external-imports',not re.search(r'\b(?:import|library)\s*\(',expanded))
                destination=out/'scripts'/(name+'.dsp');destination.write_text(expanded)
                build=out/(name+'-standalone');build.mkdir(exist_ok=True)
                # Deliberately no custom include directories: it must load alone.
                command(['faust','-lang','cpp','-single','-cn','ModuleDSP',destination,'-o',build/'generated.hpp'])
                command(['c++','-std=c++17','-O2','-ffp-contract=off','-I'+str(build),ROOT/'tools/modules/render.cpp','-o',build/'render'])
                standalone=build/'render';c(name+':same-controls',controls(canonical)==controls(standalone));maxerr=0.0
                for sr in (44100,48000,96000):
                    events=[(round(.1*sr),'gate',1),(round(.5*sr),'cutoff',800),(round(.8*sr),'freq',440),
                            (round(1.1*sr),'gate',0),(round(1.2*sr),'cutoff',300)]
                    p=base|{'release':.6}
                    a=lab.render(name+f'-canonical-{sr}',canonical,p,events,sr=sr,frames=2*sr)
                    b=lab.render(name+f'-standalone-{sr}',standalone,p,events,sr=sr,frames=2*sr)
                    err=float(np.max(abs(a-b)));maxerr=max(maxerr,err)
                    c(name+f':exact-expanded-audio-{sr}',np.array_equal(a,b),max_error=err)
                audio=lab.render(name+'-review-phrase',standalone,base,phrase(root=220 if name.startswith('Juno') else 110),frames=384000)
                lab.wav(name+'-review-phrase.wav',audio)
                manifest['subjects'][name]=dict(canonical_path=path,canonical_sha256=digest(src),exported_file=name+'.dsp',
                    exported_sha256=digest(destination),audio_sha256=digest(out/'audition'/(name+'-review-phrase.wav')),
                    expanded_audio_max_error=maxerr,reference_and_listening_status='See exact checkpoint PR; no implicit approval')
        c('all-four-script-exports',len(manifest['subjects'])==4)
    except Exception as e:
        lab.report['exception']=traceback.format_exc();c('export-completed',False);print(lab.report['exception'],flush=True)
        if getattr(e,'output',None):print(e.output,flush=True)
    lab.report['passed']=bool(lab.report['checks']) and all(x.get('passed') is True for x in lab.report['checks'])
    manifest['standalone_export_checks_passed']=lab.report['passed']
    (out/'scripts'/'manifest.json').write_text(json.dumps(manifest,indent=2));(out/'results.json').write_text(json.dumps(lab.report,indent=2))
    print('SCRIPT_EXPORTS',json.dumps({'passed':lab.report['passed'],'subjects':manifest['subjects'],
        'checks':len(lab.report['checks']),'renders':len(lab.report['renders'])}),flush=True)
    return 0 if lab.report['passed'] else 1
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--out',type=Path,required=True)
    a=p.parse_args();raise SystemExit(run(a.out))
