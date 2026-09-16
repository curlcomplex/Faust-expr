#!/usr/bin/env python3
"""Decode TX81Z VCED+ACED voice data into the working Faust voice controls.

Offsets follow the documented 93-byte VCED and 23-byte ACED layouts. Frequency
coarse is a TX81Z panel ratio index, not the OPZ multiple nibble: the firmware
ratio families correspond to OPZ DT2 groups and multiple slots. Output level is
the inverse of the OPZ 0.75 dB total-level control to within the documented
~0.74 dB TX81Z panel step, so 99 -> TL 0 and 0 -> TL 99. DET is centred at 3.
Level scaling and EG bias remain explicit because the current one-note voice has
no qualified panel policy for them yet.
"""
from __future__ import annotations
import argparse, json
from pathlib import Path

VCED_SIZE=93; ACED_SIZE=23
OP_VCED={1:39,2:13,3:26,4:0}
OP_ACED={1:15,2:5,3:10,4:0}
# TX81Z ratio-coarse ordering. Each row is one OPZ DT2 family; the position in
# the row is the OPZ multiple nibble. This is the compact form of the published
# 64-ratio table and matches independent TX81Z implementations.
RATIO_GROUPS=(
    (0,4,8,10,13,16,19,22,25,28,31,34,36,40,42,45),
    (1,5,9,14,18,23,26,30,35,39,43,46,49,52,55,58),
    (2,6,11,15,20,24,29,33,38,44,48,50,53,56,59,61),
    (3,7,12,17,21,27,32,37,41,47,51,54,57,60,62,63),
)
RATIO_MAP={panel:(multiple,dt2) for dt2,row in enumerate(RATIO_GROUPS) for multiple,panel in enumerate(row)}
# VCED DET is 0..6 with centre=3. OPZ DT1 uses 0..3 for +0..+3 and 5..7 for -1..-3.
DET_TO_DT1=(7,6,5,0,1,2,3)

def _bytes(path: str, size: int) -> bytes:
    data=Path(path).read_bytes()
    if len(data)!=size: raise ValueError(f"{path}: expected {size} data bytes, got {len(data)}")
    if any(x>127 for x in data): raise ValueError(f"{path}: TX81Z data bytes must be 7-bit")
    return data

def decode(vced: bytes, aced: bytes) -> dict:
    if len(vced)!=VCED_SIZE or len(aced)!=ACED_SIZE: raise ValueError("VCED/ACED size mismatch")
    ops={}
    for op in range(1,5):
        b=OP_VCED[op]; a=OP_ACED[op]
        ops[op]={
            'ar':vced[b], 'd1r':vced[b+1], 'd2r':vced[b+2], 'rr':vced[b+3], 'd1l':vced[b+4],
            'ls':vced[b+5], 'rs':vced[b+6], 'ebs':vced[b+7], 'ame':vced[b+8], 'kvs':vced[b+9],
            'out':vced[b+10], 'crs':vced[b+11], 'det':vced[b+12],
            'fixed':aced[a], 'range':aced[a+1], 'fine':aced[a+2], 'wave':aced[a+3], 'eg_shift':aced[a+4],
        }
    name=bytes(vced[77:87]).decode('ascii','replace').rstrip()
    return {'name':name,'operators':ops,'algorithm':vced[52]+1,'feedback':vced[53],
            'lfo':{'speed':vced[54],'delay':vced[55],'pmd':vced[56],'amd':vced[57],'sync':vced[58],'wave':vced[59],'pms':vced[60],'ams':vced[61]},
            'transpose':vced[62],'reverb_rate':aced[20]}

def _ratio_fields(panel_coarse: int) -> tuple[int,int]:
    if panel_coarse not in RATIO_MAP: raise ValueError(f"ratio coarse out of range: {panel_coarse}")
    return RATIO_MAP[panel_coarse]

def _dt1(det: int) -> int:
    if not 0 <= det < len(DET_TO_DT1): raise ValueError(f"DET out of range: {det}")
    return DET_TO_DT1[det]

def to_controls(patch: dict) -> tuple[dict,dict]:
    l=patch['lfo']; c={'algorithm':patch['algorithm'],'feedback':patch['feedback'],'lfoSpeed':l['speed'],'lfoDelay':l['delay'],'pModDepth':l['pmd'],'aModDepth':l['amd'],'lfoSync':l['sync'],'lfoWave':l['wave'],'pModSens':l['pms'],'aModSens':l['ams']}
    unresolved={}
    for op,p in patch['operators'].items():
        q=f'op{op}'; multiple,dt2=_ratio_fields(p['crs'])
        c.update({q+'AR':p['ar'],q+'D1R':p['d1r'],q+'D2R':p['d2r'],q+'RR':p['rr'],q+'SL':p['d1l'],q+'KS':p['rs'],q+'AME':p['ame'],q+'KVS':p['kvs'],q+'TL':max(0,min(127,99-p['out'])),q+'Mode':p['fixed'],q+'Coarse':multiple,q+'DT1':_dt1(p['det']),q+'DT2':dt2,q+'Range':p['range'],q+'Fine':p['fine'],q+'Wave':p['wave'],q+'FixedCRS':p['crs']})
        if op!=1: c[q+'EGShift']=p['eg_shift']
        unresolved[q]={'level_scaling':p['ls'],'eg_bias':p['ebs']}
        if op==1 and p['eg_shift']!=0: unresolved[q]['unexpected_eg_shift']=p['eg_shift']
    unresolved['voice']={'transpose':patch['transpose'],'reverb_rate':patch['reverb_rate']}
    return c,unresolved

def main():
    ap=argparse.ArgumentParser(); ap.add_argument('--vced',required=True); ap.add_argument('--aced',required=True); ns=ap.parse_args()
    patch=decode(_bytes(ns.vced,VCED_SIZE),_bytes(ns.aced,ACED_SIZE)); controls,unresolved=to_controls(patch)
    print(json.dumps({'patch':patch,'controls':controls,'unresolved':unresolved},indent=2))
if __name__=='__main__': main()
