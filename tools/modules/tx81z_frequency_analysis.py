#!/usr/bin/env python3
"""Validate the native OPZ probe's integer laws, retaining failed reports too."""
import argparse,csv,hashlib,itertools,json,math
from pathlib import Path

FIELDS=['kind','block','code','dt1','coarse','fine','dt2','range','fixed','step','baseline','ratio']
CODES=[0,1,2,4,5,6,8,9,10,12,13,14]

def expected_keys():
    for b,c in itertools.product([2,4,6],CODES):
        for coarse,fine in itertools.product([0,1,2,7,15],[0,1,7,8,15]):
            yield ('ratio',b,c,0,coarse,fine,0,0,0)
        for dt in range(4): yield ('dt2',b,c,0,1,0,dt,0,0)
        for dt in range(8): yield ('dt1',b,c,dt,1,0,0,0,0)
        for ran,ff,fine in itertools.product([0,1,3,7],[0,1,8,15],[0,7,15]):
            yield ('fixed',b,c,0,0,fine,0,ran,ff)

def parse_csv(path):
    with Path(path).open() as f:
        reader=csv.DictReader(f)
        if reader.fieldnames!=FIELDS: raise ValueError('unexpected frequency CSV fields')
        rows=list(reader)
    if len(rows)!=3060: raise ValueError('expected the complete 3060-row native probe')
    actual=[]
    for r in rows:
        for k in FIELDS[1:9]:r[k]=int(r[k])
        for k in FIELDS[9:]:
            r[k]=float(r[k])
            if not math.isfinite(r[k]):raise ValueError('nonfinite '+k)
        if r['baseline']<=0 or r['step']<0:raise ValueError('invalid native phase step')
        if r['ratio']!=r['step']/r['baseline']:raise ValueError('ratio/step disagreement')
        actual.append(tuple(r[k] for k in FIELDS[:9]))
    if actual!=list(expected_keys()):raise ValueError('missing, duplicate, reordered or unexpected cases')
    return rows

def evaluate(rows,checks):
    def check(name,ok,detail=None):
        checks.append({'name':name,'passed':bool(ok),'detail':detail})
        if not ok: raise AssertionError(name+': '+str(detail))
    for r in (r for r in rows if r['kind']=='ratio'):
        multiple=((r['coarse']<<4) if r['coarse'] else 8)|r['fine']
        expected=(int(r['baseline'])*multiple)>>4
        check(f"ratio-{r['block']}-{r['code']}-{r['coarse']}-{r['fine']}",r['step']==expected,[r['step'],expected])
    cents=[0,600,781,950]
    dt2=max(abs(r['ratio']-2**(cents[r['dt2']]/1200)) for r in rows if r['kind']=='dt2')
    check('dt2-documented-cents-track',dt2<.003,dt2)
    pos=[r['ratio'] for r in rows if r['kind']=='dt1' and r['dt1']==1]
    neg=[r['ratio'] for r in rows if r['kind']=='dt1' and r['dt1']==5]
    check('dt1-key-dependent',max(pos)-min(pos)>1e-5,[min(pos),max(pos)])
    check('dt1-directions',min(pos)>=1 and max(neg)<=1,[min(pos),max(neg)])
    groups={}
    for r in (r for r in rows if r['kind']=='fixed'):
        key=(r['range'],r['fixed'],r['fine']);groups.setdefault(key,[]).append(r['step'])
    for (ran,ff,fine),v in groups.items():
        target=(((ff<<4) if ff else 8)|fine)<<ran
        expected=75*target/4096
        check(f'fixed-{ran}-{ff}-{fine}',all(x==expected for x in v),[min(v),max(v),expected])
    return {'ratio_mode':'Multiply the integer base step by the x.4 coarse/fine value, then truncate with >>4. Coarse-zero fine 0..7 and 8..15 alias because fine is ORed into 0x08.',
            'detune2':'Pinned native DT2 uses 0/384/500/608 increments of 1/64 semitone before key-table lookup.',
            'detune1':'Keycode-dependent additive phase-step correction, applied before the coarse/fine multiplier; not constant cents.',
            'fixed_mode':'In pinned ymfm the requested register frequency is absolute; mean step is exactly 75*frequency/4096 over this full-period probe. Hardware mapping remains explicitly uncertain upstream.',
            'sharing_implication':'OPZ needs its own frequency law and native parameter translator. Generic phase/FM routing interfaces may be reusable; a shared DX7/OPZ knob-to-frequency function is not established.'}

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--csv',type=Path,required=True);p.add_argument('--out',type=Path,required=True);a=p.parse_args()
    if a.out.exists():raise ValueError('use a fresh evidence directory')
    a.out.mkdir(parents=True);report={'status':'running','checks':[],'source_csv_sha256':hashlib.sha256(a.csv.read_bytes()).hexdigest()}
    try:
        rows=parse_csv(a.csv);report['rows']=len(rows);report['findings']=evaluate(rows,report['checks']);report['status']='completed'
        (a.out/'REPORT.md').write_text('# Pinned OPZ integer frequency laws\n\n'+'\n\n'.join(f'**{k}:** {v}' for k,v in report['findings'].items())+'\n')
    except Exception as e:report['status']='failed';report['error']=str(e);raise
    finally:(a.out/'results.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({'status':report['status'],'rows':report['rows'],'checks':len(report['checks'])}))
if __name__=='__main__':main()
