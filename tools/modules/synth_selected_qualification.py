"""Reuse the established synth suite on the selected version paths.
The suite's internal juno106 role is exercised once with each Juno source; the
report records the actual source paths. No new audio engine or test framework.
"""
from pathlib import Path
import argparse,json
import synth_batch as batch
from synth_four_voice_checkpoint import DEFAULT as J60_DEFAULT

def run(out):
    out.mkdir(parents=True,exist_ok=True)
    batch.PATHS['mono101']=batch.ROOT/'modules/mono-101/v4/voice.dsp'
    batch.PATHS['juno106']=batch.ROOT/'modules/juno-106/v3/voice.dsp'
    batch.PATHS['mini']=batch.ROOT/'modules/minimoog/v1/voice.dsp'
    first=batch.run(out/'juno106-family')
    batch.PATHS['juno106']=batch.ROOT/'modules/juno-60/v3/voice.dsp'
    batch.DEFAULTS['juno106']=J60_DEFAULT
    second=batch.run(out/'juno60-family')
    result={'passed':first==0 and second==0,'juno106_suite_exit':first,'juno60_suite_exit':second,
      'note':'The inherited juno106 role name refers to the source recorded in each build, not a claim the two Juno identities are interchangeable.'}
    (out/'results.json').write_text(json.dumps(result,indent=2));print('SELECTED_QUALIFICATION',json.dumps(result),flush=True)
    return 0 if result['passed'] else 1
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--out',required=True,type=Path)
    a=p.parse_args();raise SystemExit(run(a.out))
