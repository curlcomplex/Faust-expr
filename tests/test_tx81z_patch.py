import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('tx81z_patch', ROOT/'tools/modules/tx81z_patch.py')
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)


def voice():
    v, a = bytearray(93), bytearray(23)
    v[77:87] = b'TESTVOICE '
    v[62] = 24
    for b in (0,13,26,39):
        v[b:b+13] = bytes([31,0,0,8,15,0,0,0,0,0,99,4,3])
    return v,a


def frame(kind, payload, channel=0):
    return bytes([0xF0,0x43,channel,kind,len(payload)>>7,len(payload)&127])+bytes(payload)+bytes([(-sum(payload))&127,0xF7])


def sysex(v,a):
    return frame(0x7E,m.ACED_ID+bytes(a))+frame(3,v)


class TX81ZPatchTests(unittest.TestCase):
    def test_asymmetric_edit_buffer_operator_order(self):
        v,a = voice()
        # Actual order4,2,3,1 from Edisyn and independently documented real
        # patches, not the manual's incorrect middle-operator labels.
        # Literal offsets prevent testing only the decoder's own constants.
        v[26:39] = bytes([30,21,12,8,6,43,2,4,1,5,77,31,3])
        a[10:15] = bytes([0,4,9,6,3])
        v[13:26] = bytes([29,20,11,7,5,42,1,3,0,4,66,13,2])
        a[5:10] = bytes([1,3,8,5,2])
        p = m.decode(bytes(v),bytes(a)); c,u = m.to_controls(p)
        self.assertEqual((c['op3AR'],c['op3TL'],c['op3LS'],c['op3EBS']),(30,22,43,4))
        self.assertEqual((c['op2AR'],c['op2TL'],c['op2LS'],c['op2EBS']),(29,33,42,3))
        self.assertEqual((c['op3Wave'],c['op3EGShift'],c['op3Mode']),(6,3,0))
        self.assertEqual((c['op2Wave'],c['op2EGShift'],c['op2Mode']),(5,2,1))
        self.assertEqual((c['op2SL'],c['op3SL']),(10,9))
        self.assertNotIn('op1',u)

    def test_full_voice_patch_controls(self):
        v,a = voice()
        v[52:63] = bytes([7,6,91,42,73,66,1,3,5,2,36])
        v[76] = 87; a[20] = 5
        v[39:52] = bytes([31,22,13,9,7,44,3,5,1,6,88,37,4])
        a[15:20] = bytes([1,6,12,7,0])
        p = m.decode(bytes(v),bytes(a)); c,u = m.to_controls(p)
        self.assertEqual(p['name'],'TESTVOICE')
        self.assertEqual((c['algorithm'],c['feedback'],c['transpose'],c['bcEGBias']),(8,6,12,87))
        self.assertEqual((c['lfoSpeed'],c['lfoWave'],c['pModSens'],c['aModSens']),(91,3,5,2))
        self.assertEqual((c['op1Coarse'],c['op1DT2'],c['op1DT1']),(7,3,1))
        self.assertEqual((c['op1TL'],c['op1KVS'],c['op1SL']),(11,6,8))
        self.assertEqual([c[f'op{i}Reverb'] for i in range(1,5)],[5]*4)
        self.assertNotIn('op1EGShift',c)
        self.assertIn('bc_curve',u['voice'])

    def test_d1l_is_inverted_to_chip_attenuation(self):
        v,a = voice()
        for level in range(16):
            v[43] = level
            c,_ = m.to_controls(m.decode(bytes(v),bytes(a)))
            self.assertEqual(c['op1SL'],15-level)

    def test_low_output_table_and_affine_boundary(self):
        self.assertEqual([m.basic_tl(x) for x in (0,1,19,20,21,90,99)], [127,122,81,79,78,9,0])
        values = [m.basic_tl(x) for x in range(100)]
        self.assertTrue(all(x >= y for x,y in zip(values,values[1:])))

    def test_all_ratio_coarse_values_are_bijective(self):
        self.assertEqual(set(m.RATIO_MAP),set(range(64)))
        self.assertEqual(set(m.RATIO_MAP.values()),{(i,j) for i in range(16) for j in range(4)})
        self.assertEqual([m.RATIO_MAP[x] for x in (0,1,2,3,4,37,63)],[(0,0),(0,1),(0,2),(0,3),(1,0),(7,3),(15,3)])

    def test_detune_encoding_and_bounds(self):
        self.assertEqual([m._dt1(x) for x in range(7)],[7,6,5,0,1,2,3])
        for bad in (-1,7,True,3.0):
            with self.subTest(bad=bad), self.assertRaises(ValueError): m._dt1(bad)

    def test_actual_sysex_frames_either_order(self):
        v,a = voice()
        x = frame(3,v); y = frame(0x7E,m.ACED_ID+bytes(a))
        p = m.decode_sysex(x+y)
        self.assertEqual(p,m.decode_sysex(y+x))
        self.assertEqual(p['midi_channel'],1)
        self.assertEqual(m.to_controls(p),m.to_controls(m.decode(bytes(v),bytes(a))))

    def test_checksum_corruption_rejected(self):
        v,a = voice(); data = bytearray(sysex(v,a)); data[20] ^= 1
        with self.assertRaisesRegex(ValueError,'checksum'): m.decode_sysex(bytes(data))

    def test_count_corruption_rejected(self):
        v,a = voice(); data = bytearray(sysex(v,a)); data[5] -= 1
        with self.assertRaisesRegex(ValueError,'byte count'): m.decode_sysex(bytes(data))

    def test_truncation_and_non_sysex_bytes_rejected(self):
        v,a = voice(); data = sysex(v,a)
        for broken in (data[:-1],b'junk'+data,data+b'junk',b'',data[:40]):
            with self.subTest(size=len(broken)), self.assertRaises(ValueError): m.decode_sysex(broken)

    def test_duplicates_and_missing_buffers_rejected(self):
        v,a = voice(); x = frame(3,v); y = frame(0x7E,m.ACED_ID+bytes(a))
        for broken in (x,y,x+x+y,y+x+y):
            with self.subTest(size=len(broken)), self.assertRaises(ValueError): m.decode_sysex(broken)

    def test_channel_and_manufacturer_rejected(self):
        v,a = voice()
        with self.assertRaisesRegex(ValueError,'channels'):
            m.decode_sysex(frame(3,v,1)+frame(0x7E,m.ACED_ID+bytes(a),2))
        data = bytearray(sysex(v,a)); data[1] = 0x42
        with self.assertRaisesRegex(ValueError,'Yamaha'): m.decode_sysex(bytes(data))

    def test_unsupported_bank_is_not_treated_as_edit_buffer(self):
        v,a = voice()
        with self.assertRaisesRegex(ValueError,'unsupported'):
            m.decode_sysex(frame(4,v)+frame(0x7E,m.ACED_ID+bytes(a)))

    def test_raw_size_and_seven_bit_validation(self):
        v,a = voice()
        with self.assertRaises(ValueError): m.decode(bytes(v[:-1]),bytes(a))
        v[0] = 128
        with self.assertRaisesRegex(ValueError,'7-bit'): m.decode(bytes(v),bytes(a))

    def test_semantic_ranges_are_not_silently_clamped(self):
        for offset,value in ((0,32),(8,2),(10,100),(12,7),(52,8),(60,8),(62,49),(76,100)):
            v,a = voice(); v[offset] = value
            with self.subTest(offset=offset), self.assertRaises(ValueError): m.decode(bytes(v),bytes(a))
        v,a = voice(); a[18] = 8
        with self.assertRaises(ValueError): m.decode(bytes(v),bytes(a))

    def test_nonzero_operator_one_shift_stays_explicit(self):
        v,a = voice(); a[19] = 1
        c,u = m.to_controls(m.decode(bytes(v),bytes(a)))
        self.assertNotIn('op1EGShift',c)
        self.assertEqual(u['op1']['unsupported_op1_eg_shift'],1)

if __name__ == '__main__':
    unittest.main()
