#!/usr/bin/env python3
"""Analyze native OPZ frequency-law probe and emit engineering conclusions."""
import argparse,csv,json,math
from pathlib import Path

def close(a,b,t=2e-4): return abs(a-b)<=t*max(1,abs(b))

def main():
 p=argparse.ArgumentParser(); p.add_argument('--csv',type=Path,required=True); p.add_argument('--out',type=Path,required=True); a=p.parse_args()
 rows=list(csv.DictReader(a.csv.open()))
 for r in rows:
  for k in ['step','baseline','ratio']: r[k]=float(r[k])
  for k in ['block','code','dt1','coarse','fine','dt2','range','fixed']: r[k]=int(r[k])
 checks=[]
 def check(name,ok,detail=None):
  checks.append({'name':name,'passed':bool(ok),'detail':detail})
  if not ok: raise AssertionError(name+': '+str(detail))
 # The cached multiplier is x.4. coarse 0 begins at 0x08 and ORs fine.
 for r in [x for x in rows if x['kind']=='ratio']:
  raw=((r['coarse']<<4) if r['coarse'] else 8) | r['fine']; expected=raw/16
  check(f"ratio-b{r['block']}-c{r['code']}-{r['coarse']}-{r['fine']}",close(r['ratio'],expected,4e-4),[r['ratio'],expected])
 # DT2 is approximately the documented +600/+781/+950 cent coarse offset, quantized by OPM table.
 cents=[0,600,781,950]
 max_dt2=0
 for r in [x for x in rows if x['kind']=='dt2']:
  expected=2**(cents[r['dt2']]/1200); err=abs(r['ratio']-expected); max_dt2=max(max_dt2,err)
 check('dt2-documented-cents-track',max_dt2<0.003,max_dt2)
 # DT1 must be key dependent and sign symmetric enough to prove it is not a single cents constant.
 dt1=[x for x in rows if x['kind']=='dt1' and x['dt1'] in (1,5)]
 pos=[x['ratio'] for x in dt1 if x['dt1']==1]; neg=[x['ratio'] for x in dt1 if x['dt1']==5]
 check('dt1-key-dependent',max(pos)-min(pos)>1e-5,[min(pos),max(pos)])
 check('dt1-has-positive-and-negative-directions',min(pos)>=1 and max(neg)<=1,[min(pos),max(neg)])
 # Fixed mode should ignore channel pitch. Same fixed fields across all key codes/blocks => same mean phase step.
 groups={}
 for r in [x for x in rows if x['kind']=='fixed']:
  groups.setdefault((r['range'],r['fixed'],r['fine']),[]).append(r['step'])
 spread=max(max(v)-min(v) for v in groups.values())
 check('fixed-independent-of-key',spread<1e-9,spread)
 # ymfm formula: base = ((fixed<<4) or 8) OR fine, then << range. Probe mean must scale exactly with that target integer frequency.
 normalized=[]
 for key,v in groups.items():
  range_,ff,fine=key; target=(((ff<<4) if ff else 8)|fine)<<range_; normalized.append((sum(v)/len(v))/target)
 check('fixed-linear-in-requested-hz',max(normalized)-min(normalized)<1e-4,[min(normalized),max(normalized)])
 result={'status':'completed','rows':len(rows),'checks':checks,'findings':{
  'ratio_mode':'OPZ multiple/fine is an x.4 multiplier. Coarse 0 is special: the base is 0x08 and fine is ORed, so fine 8..15 alias fine 0..7 in that coarse-zero case.',
  'detune2':'DT2 is a coarse pitch offset approximating +0/+600/+781/+950 cents before DT1 and multiplier.',
  'detune1':'DT1 is a keycode-dependent additive chip phase-step correction, not a constant cents detune law.',
  'fixed_mode':'Fixed mode is absolute and key-independent in ymfm; fixed frequency/fine plus range replace ratio-mode detune/multiple because the register fields overlap.',
  'sharing_implication':'Share an FM operator frequency-mode interface and test contract with DX7, not one universal frequency-law implementation. OPZ ratio/fine/DT2/DT1 and fixed-register semantics remain machine-specific.'}}
 a.out.mkdir(parents=True,exist_ok=True); (a.out/'results.json').write_text(json.dumps(result,indent=2)+'\n')
 (a.out/'REPORT.md').write_text('# TX81Z / OPZ frequency-law characterization\n\n'+ '\n'.join(f"- **{k}:** {v}" for k,v in result['findings'].items())+'\n')
 print(json.dumps({'status':'completed','rows':len(rows),'checks':len(checks)}))
if __name__=='__main__': main()
