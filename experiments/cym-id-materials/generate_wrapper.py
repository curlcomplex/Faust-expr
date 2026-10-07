#!/usr/bin/env python3
"""Generate a 32-mode banking wrapper for materials-engine.lib. No fit/audio read."""
from pathlib import Path
def generate(path):
    out=Path(path).read_text()+"\n"
    for i in range(32):
        p=f'p{i}';m=f'm{i:02d}'
        out+=f'{p}_f=hslider("{m}_f",100,0,100000,0.00000001);\n'
        out+=f'{p}_d=hslider("{m}_d",1,1e-06,10000,0.00000001);\n'
        for ch in 'LR':
            for s in ['hr','hi','br','bi','sr','si']:
                out+=f'{p}_{s}{ch}=hslider("{m}_{s}{ch}",0,-10,10,0.00000001);\n'
            out+=f'v{i}_{ch}=mode('+','.join([f'{p}_f',f'{p}_d']+[f'{p}_{s}{ch}' for s in ['hr','hi','br','bi','sr','si']])+');\n'
    out+='process=('+','.join(f'v{i}_{ch}' for i in range(32) for ch in 'LR')+') :> (_,_);\n'
    return out
if __name__=="__main__":
    import argparse
    p=argparse.ArgumentParser();p.add_argument("library",type=Path);p.add_argument("output",type=Path)
    a=p.parse_args();a.output.write_text(generate(a.library))
