#!/usr/bin/env python3
"""Check the default float Faust build, including a linear precision comparison."""
from __future__ import annotations
import argparse
import json
import subprocess
import tempfile
from pathlib import Path
import numpy as np


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--single',required=True)
    parser.add_argument('--double',required=True)
    parser.add_argument('--out',required=True)
    args=parser.parse_args()
    checks=[]
    with tempfile.TemporaryDirectory(prefix='cymbal-precision-') as temp:
        def render(exe,settings,seconds=.7):
            file=Path(temp)/'audio.f32'
            cmd=[str(Path(exe).resolve()),str(file),'--rate','48000','--seconds',str(seconds),
                 '--event','.1:gate=1','--event','.101:gate=0']
            for k,v in settings.items():cmd+=['--set',f'{k}={v}']
            run=subprocess.run(cmd,text=True,capture_output=True,timeout=30,check=True)
            return np.fromfile(file,dtype='<f4'),json.loads(run.stdout)
        cases=[{}, {'material':2}, {'material':3}, {'beater':2},
               {'diameter_m':.01,'thickness_mm':6}, {'diameter_m':40,'thickness_mm':.15},
               {'diameter_m':40,'proportional_thickness':1,'material':2},
               {'hammering':1,'nonlinearity':1,'velocity':1,'beater_mass_g':200},
               {'taper':.9,'bell_diameter_ratio':.12,'bell_height_ratio':.35},
               {'choke':1}, {'velocity':0}]
        for i,settings in enumerate(cases):
            audio,info=render(args.single,settings)
            checks.append({'name':f'float case {i}','passed':bool(np.isfinite(audio).all()
                            and info['max_energy_sampled']<100 and info['peak']<2),
                           'settings':settings,'peak':info['peak'],'energy':info['max_energy_sampled']})
        a,_=render(args.single,{'nonlinearity':0},1)
        b,_=render(args.double,{'nonlinearity':0},1)
        error=float(np.linalg.norm(a.astype(float)-b)/np.linalg.norm(b))
        checks.append({'name':'float/double linear response','passed':error<.005,
                       'relative_rms_error':error,'threshold':.005})
    result={'passed':all(c['passed'] for c in checks),'checks':checks,
            'note':'No phase-identity claim for chaotic nonlinear tails across precisions.'}
    Path(args.out).write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result,indent=2))
    if not result['passed']:raise SystemExit(1)

if __name__=='__main__':main()
