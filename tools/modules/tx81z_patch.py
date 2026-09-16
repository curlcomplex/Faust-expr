#!/usr/bin/env python3
"""Checked TX81Z single-voice VCED/ACED -> v10 Faust controls.

Yamaha TX81Z manual pp.71-74: edit buffers are OP4,OP3,OP2,OP1;
packed VMEM uses a DIFFERENT order. This module accepts edit buffers only.
D1L is a level (15=maximum), whereas OPZ SL is attenuation (0=maximum).
Small numerical parameter facts follow ax81z@5848832989c7864092a277e9428d44f817c6173f.
See modules/tx81z/v10/REFERENCE.md for scope and the provisional BC curve.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path

VCED_SIZE = 93
ACED_SIZE = 23
OP_VCED = {1: 39, 2: 26, 3: 13, 4: 0}
OP_ACED = {1: 15, 2: 10, 3: 5, 4: 0}
ACED_ID = b'LM  8976AE'
RATIO_GROUPS = (
    (0,4,8,10,13,16,19,22,25,28,31,34,36,40,42,45),
    (1,5,9,14,18,23,26,30,35,39,43,46,49,52,55,58),
    (2,6,11,15,20,24,29,33,38,44,48,50,53,56,59,61),
    (3,7,12,17,21,27,32,37,41,47,51,54,57,60,62,63),
)
RATIO_MAP = {panel: (multiple,dt2) for dt2,row in enumerate(RATIO_GROUPS)
             for multiple,panel in enumerate(row)}
DET_TO_DT1 = (7,6,5,0,1,2,3)
LOW_OUTPUT_TL = (127,122,118,114,110,107,104,102,100,98,96,94,92,90,88,86,85,84,82,81)
VCED_FIELDS = ('ar','d1r','d2r','rr','d1l','ls','rs','ebs','ame','kvs','out','crs','det')
VCED_MAX = (31,31,31,15,15,99,3,7,1,7,99,63,6)
ACED_FIELDS = ('fixed','range','fine','wave','eg_shift')
ACED_MAX = (1,7,15,7,3)
FUNCTION_FIELDS = ('poly_mode','pitch_bend_range','portamento_mode','portamento_time',
                   'foot_volume','sustain','portamento','chorus','mw_pitch','mw_amplitude',
                   'bc_pitch','bc_amplitude','bc_pitch_bias','bc_eg_bias')
FUNCTION_MAX = (1,12,1,99,99,1,1,1,99,99,99,99,99,99)


def _bounded(value: int, upper: int, name: str) -> int:
    if not isinstance(value, int) or isinstance(value, bool) or not 0 <= value <= upper:
        raise ValueError(f'{name}: expected integer 0..{upper}, got {value!r}')
    return value


def _data(data: bytes, size: int, name: str) -> bytes:
    if not isinstance(data, (bytes, bytearray)) or len(data) != size:
        raise ValueError(f'{name}: expected {size} data bytes')
    if any(value > 127 for value in data):
        raise ValueError(f'{name}: data must be 7-bit')
    return bytes(data)


def _bytes(path: str, size: int) -> bytes:
    return _data(Path(path).read_bytes(), size, path)


def basic_tl(output_level: int) -> int:
    x = _bounded(output_level, 99, 'output level')
    # Source prose says <=20, but supplies entries 0..19 only. The affine
    # section starts at20 (TL79); do not index a nonexistent twenty-first item.
    return LOW_OUTPUT_TL[x] if x < 20 else 99-x


def _ratio_fields(panel_coarse: int) -> tuple[int, int]:
    return RATIO_MAP[_bounded(panel_coarse, 63, 'ratio coarse')]


def _dt1(det: int) -> int:
    return DET_TO_DT1[_bounded(det, 6, 'DET')]


def decode(vced: bytes, aced: bytes) -> dict:
    vced = _data(vced, VCED_SIZE, 'VCED')
    aced = _data(aced, ACED_SIZE, 'ACED')
    operators = {}
    for op in range(1,5):
        b, a = OP_VCED[op], OP_ACED[op]
        p = {key: _bounded(vced[b+i], hi, f'op{op}.{key}')
             for i,(key,hi) in enumerate(zip(VCED_FIELDS,VCED_MAX))}
        p.update({key: _bounded(aced[a+i], hi, f'op{op}.{key}')
                  for i,(key,hi) in enumerate(zip(ACED_FIELDS,ACED_MAX))})
        operators[op] = p
    lfo_fields = ('speed','delay','pmd','amd','sync','wave','pms','ams')
    lfo_max = (99,99,99,99,1,3,7,3)
    functions = {key: _bounded(vced[63+i],hi,key)
                 for i,(key,hi) in enumerate(zip(FUNCTION_FIELDS,FUNCTION_MAX))}
    return {
        'name': vced[77:87].decode('ascii').rstrip(), 'operators': operators,
        'algorithm': _bounded(vced[52],7,'algorithm')+1,
        'feedback': _bounded(vced[53],7,'feedback'),
        'lfo': {key: _bounded(vced[54+i],hi,'lfo.'+key)
                for i,(key,hi) in enumerate(zip(lfo_fields,lfo_max))},
        'transpose': _bounded(vced[62],48,'transpose'),
        'reverb_rate': _bounded(aced[20],7,'reverb rate'),
        'functions': functions,
        'foot_pitch': _bounded(aced[21],99,'foot pitch'),
        'foot_amplitude': _bounded(aced[22],99,'foot amplitude'),
        'reserved_vced': list(vced[87:93]),
    }


def decode_sysex(data: bytes) -> dict:
    """Read exactly one checked VCED + one checked ACED dump in either order.

    Reject duplicates, channel mismatches, unsupported banks and truncation;
    do not silently assume a missing ACED buffer or reinterpret packed VMEM.
    """
    if not isinstance(data, bytes) or len(data) > 65536:
        raise ValueError('expected a bounded binary SysEx file')
    cursor, channel = 0, None
    buffers = {}
    while cursor < len(data):
        if data[cursor] != 0xF0:
            raise ValueError(f'expected SysEx start at byte {cursor}')
        end = data.find(b'\xf7', cursor+1)
        if end < 0:
            raise ValueError('truncated SysEx')
        frame = data[cursor:end+1]
        cursor = end+1
        if len(frame) < 8 or any(x > 127 for x in frame[1:-1]):
            raise ValueError('invalid SysEx data bytes')
        if frame[1] != 0x43 or frame[2] > 15:
            raise ValueError('expected Yamaha single-channel bulk dump')
        if channel is not None and channel != frame[2]:
            raise ValueError('VCED/ACED MIDI channels differ')
        channel = frame[2]
        size = (frame[4]<<7) | frame[5]
        payload = frame[6:-2]
        if len(payload) != size:
            raise ValueError('SysEx declared byte count differs from payload')
        if (sum(payload)+frame[-2]) & 127:
            raise ValueError('Yamaha checksum mismatch')
        if frame[3] == 3 and size == VCED_SIZE:
            key, value = 'VCED', payload
        elif frame[3] == 0x7E and size == len(ACED_ID)+ACED_SIZE and payload[:10] == ACED_ID:
            key, value = 'ACED', payload[10:]
        else:
            raise ValueError('unsupported SysEx format (only TX81Z VCED/ACED edit buffers)')
        if key in buffers:
            raise ValueError('duplicate '+key+' buffer')
        buffers[key] = value
    if set(buffers) != {'VCED','ACED'}:
        raise ValueError('both VCED and ACED are required')
    patch = decode(buffers['VCED'],buffers['ACED'])
    patch['midi_channel'] = channel+1
    return patch


def to_controls(patch: dict) -> tuple[dict, dict]:
    """v10 controls and retained functions outside this single-note adapter."""
    l = patch['lfo']
    c = {'algorithm':patch['algorithm'],'feedback':patch['feedback'],
         'lfoSpeed':l['speed'],'lfoDelay':l['delay'],'pModDepth':l['pmd'],
         'aModDepth':l['amd'],'lfoSync':l['sync'],'lfoWave':l['wave'],
         'pModSens':l['pms'],'aModSens':l['ams'],'transpose':patch['transpose']-24,
         'bcEGBias':patch['functions']['bc_eg_bias']}
    limitations = {}
    for op in range(1,5):
        p = patch['operators'][op]
        q = f'op{op}'
        multiple,dt2 = _ratio_fields(p['crs'])
        fields = {'AR':p['ar'],'D1R':p['d1r'],'D2R':p['d2r'],'RR':p['rr'],
                  'SL':15-p['d1l'],'KS':p['rs'],'LS':p['ls'],'EBS':p['ebs'],
                  'AME':p['ame'],'KVS':p['kvs'],'TL':basic_tl(p['out']),
                  'Mode':p['fixed'],'Coarse':multiple,'DT1':_dt1(p['det']),
                  'DT2':dt2,'Range':p['range'],'Fine':p['fine'],'Wave':p['wave'],
                  'FixedCRS':p['crs'],'Reverb':patch['reverb_rate']}
        c.update({q+key:value for key,value in fields.items()})
        if op != 1:
            c[q+'EGShift'] = p['eg_shift']
        elif p['eg_shift'] != 0:
            limitations[q] = {'unsupported_op1_eg_shift':p['eg_shift']}
    limitations['voice'] = {
        'retained_function_settings': {key:value for key,value in patch['functions'].items()
                                       if key != 'bc_eg_bias'},
        'foot_pitch':patch['foot_pitch'],'foot_amplitude':patch['foot_amplitude'],
        'scope':'one-note voice; wheel/foot routes, bend, portamento and allocation are not implemented by this adapter',
        'bc_curve':'implemented DX100-family approximation, not TX81Z hardware-calibrated',
    }
    return c,limitations


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--sysex',type=Path)
    ap.add_argument('--vced',type=Path)
    ap.add_argument('--aced',type=Path)
    ns = ap.parse_args()
    if ns.sysex is not None and ns.vced is None and ns.aced is None:
        patch = decode_sysex(ns.sysex.read_bytes())
    elif ns.sysex is None and ns.vced is not None and ns.aced is not None:
        patch = decode(_bytes(str(ns.vced),VCED_SIZE),_bytes(str(ns.aced),ACED_SIZE))
    else:
        ap.error('use --sysex FILE or both --vced FILE --aced FILE')
    controls,limits = to_controls(patch)
    print(json.dumps({'patch':patch,'controls':controls,'limitations':limits},indent=2))

if __name__ == '__main__':
    main()
