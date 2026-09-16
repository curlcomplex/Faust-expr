#!/usr/bin/env python3
"""Decode TX81Z VCED+ACED voice data without inventing unsettled mappings.

Offsets follow TX81Z Programmer's documented 93-byte VCED and 23-byte ACED
layouts. `to_controls` emits only mappings that are direct in the current Faust
voice. Output-level, coarse-ratio, DET, level scaling, EG bias and KVS remain
explicitly unmapped until their TX81Z->engine policies are implemented.
"""
from __future__ import annotations
import argparse, json
from pathlib import Path

VCED_SIZE=93; ACED_SIZE=23
OP_VCED={1:39,2:13,3:26,4:0}
OP_ACED={1:15,2:5,3:10,4:0}

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

def to_controls(patch: dict) -> tuple[dict,dict]:
    l=patch['lfo']; c={'algorithm':patch['algorithm'],'feedback':patch['feedback'],'lfoSpeed':l['speed'],'lfoDelay':l['delay'],'pModDepth':l['pmd'],'aModDepth':l['amd'],'lfoSync':l['sync'],'lfoWave':l['wave'],'pModSens':l['pms'],'aModSens':l['ams']}
    unresolved={}
    for op,p in patch['operators'].items():
        q=f'op{op}'
        c.update({q+'AR':p['ar'],q+'D1R':p['d1r'],q+'D2R':p['d2r'],q+'RR':p['rr'],q+'SL':p['d1l'],q+'KS':p['rs'],q+'AME':p['ame'],q+'Mode':p['fixed'],q+'Range':p['range'],q+'Fine':p['fine'],q+'Wave':p['wave'],q+'FixedCRS':p['crs']})
        if op!=1: c[q+'EGShift']=p['eg_shift']
        unresolved[q]={'output_level':p['out'],'ratio_coarse':p['crs'],'detune':p['det'],'level_scaling':p['ls'],'eg_bias':p['ebs'],'key_velocity_sensitivity':p['kvs']}
        if op==1 and p['eg_shift']!=0: unresolved[q]['unexpected_eg_shift']=p['eg_shift']
    unresolved['voice']={'transpose':patch['transpose'],'reverb_rate':patch['reverb_rate']}
    return c,unresolved

def main():
    ap=argparse.ArgumentParser(); ap.add_argument('--vced',required=True); ap.add_argument('--aced',required=True); ns=ap.parse_args()
    patch=decode(_bytes(ns.vced,VCED_SIZE),_bytes(ns.aced,ACED_SIZE)); controls,unresolved=to_controls(patch)
    print(json.dumps({'patch':patch,'controls':controls,'unresolved':unresolved},indent=2))
if __name__=='__main__': main()
