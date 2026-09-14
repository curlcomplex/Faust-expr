"""Reuse the established synth suite on the selected version paths.
The suite's internal juno106 role is exercised once with each Juno source;
actual source identities are recorded by every build. The legacy run() returns
None on success and raises SystemExit on failure: read its explicit report.
"""
from pathlib import Path
import argparse,json
import synth_batch as batch
from synth_four_voice_checkpoint import DEFAULT as J60_DEFAULT

def report_passed(report):
    checks=report.get('checks',[])
    return report.get('passed') is True and bool(checks) and all(c.get('passed') is True for c in checks)

def run_suite(directory):
    directory.mkdir(parents=True,exist_ok=True)
    report_file=directory/'results.json'
    if report_file.exists():report_file.unlink()
    exit_code=0
    try:batch.run(directory)
    except SystemExit as e:exit_code=e.code if isinstance(e.code,int) else 1
    report=json.loads(report_file.read_text()) if report_file.exists() else {}
    return dict(passed=exit_code==0 and report_passed(report),exit_code=exit_code,
                checks=len(report.get('checks',[])),renders=len(report.get('renders',[])))

def run(out):
    out.mkdir(parents=True,exist_ok=True)
    batch.PATHS['mono101']=batch.ROOT/'modules/mono-101/v4/voice.dsp'
    batch.PATHS['juno106']=batch.ROOT/'modules/juno-106/v3/voice.dsp'
    batch.PATHS['mini']=batch.ROOT/'modules/minimoog/v1/voice.dsp'
    first=run_suite(out/'juno106-family')
    batch.PATHS['juno106']=batch.ROOT/'modules/juno-60/v3/voice.dsp'
    batch.DEFAULTS['juno106']=J60_DEFAULT
    second=run_suite(out/'juno60-family')
    result=dict(passed=first['passed'] and second['passed'],juno106_family=first,juno60_family=second,
      note='Inherited role names are not instrument identities; consult the source paths in each build report.')
    (out/'results.json').write_text(json.dumps(result,indent=2));print('SELECTED_QUALIFICATION',json.dumps(result),flush=True)
    return 0 if result['passed'] else 1
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--out',required=True,type=Path)
    a=p.parse_args();raise SystemExit(run(a.out))
