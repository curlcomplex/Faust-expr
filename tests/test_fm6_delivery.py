"""FM6 contracts. Audio qualification is the separate fm6_delivery command."""
import copy,json,sys,unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT/'tools/modules'))
import fm6_delivery as fm

@unittest.skipUnless((fm.MODULE/'manifest.json').is_file(), 'FM6 generated source requires fm6_prepare.py; dedicated workflow runs it first')
class FM6Tests(unittest.TestCase):
    def test_all_presets_encodable(self):
        ps=json.loads((fm.MODULE/'presets.json').read_text())['presets']
        self.assertEqual(len(ps),6)
        for p in ps:
            packed,expected,sx=fm.encode(p['controls'],p['title'])
            self.assertEqual(len(packed),128);self.assertEqual(len(expected),156)
            self.assertEqual(len(sx),163);self.assertEqual(sum(sx[6:-1])%128,0)
    def test_patch_high_bit_fields_roundtrip(self):
        p={'algorithm':32,'feedback':7,'op6_mode':1,'op6_coarse':31,'op6_detune':-7,'op6_left_curve':3,'op6_right_curve':2,'op6_rate_scale':7,'op6_velocity':7,'op6_ampmod':3,'transpose':-24,'lfo_wave':5,'lfo_pms':7}
        a,b,_=fm.encode(p)
        self.assertEqual(a[11],11);self.assertEqual(a[12],7);self.assertEqual(a[13],31)
        self.assertEqual(a[15],63);self.assertEqual(b[17:21],[1,31,0,0])
        self.assertEqual(b[134:137],[31,7,1]);self.assertEqual(b[142:145],[5,7,0])
    def test_malformed_patch_rejected(self):
        for p in [{'op1_coarse':32},{'algorithm':0},{'feedback':7.5},{'velocity':True},{'level':float('nan')},{'unknown':1}]:
            with self.subTest(p=p),self.assertRaises(ValueError):fm.encode(p)
    def test_distinct_sound_identity(self):
        m=fm.manifest();self.assertEqual(m['id'],'fm6-classic');self.assertEqual(m['sound_version'],1)
        self.assertEqual(m['outputs'],1);self.assertEqual(len(m['controls']),150)
    def test_graph_has_six_operators_not_32_voices(self):
        s=(fm.MODULE/'v1/voice.lib').read_text()
        self.assertEqual(s.count('=dx.operator('),6)
        self.assertIn('network ~ si.bus(2)',s)
        self.assertNotIn('par(i,32',s)
        self.assertIn('pow(2.0,float(feedback)-9.0)',s)
    def test_routing_disjoint_algorithm_one_chains(self):
        data=json.loads((fm.MODULE/'v1/ROUTING.json').read_text())['flags']
        buses=[set(),set(),set()];incoming=[]
        for i,f in enumerate(data[0]):
            incoming.append(buses[(f>>4)&3].copy() if (f>>4)&3 else set())
            out=f&3
            if not f&4:buses[out]=set()
            buses[out].add(i)
        self.assertEqual(incoming,[set(),{0},{1},{2},set(),{4}])
        self.assertEqual(buses[0],{3,5})
        # Specifically guard against treating the output accumulator as phase input.
        s=(fm.MODULE/'v1/voice.lib').read_text()
        self.assertIn('e4_3=(algorithm-1) : rdtable(waveform{0,',s)
    def test_lifecycle_and_live_tail_contract(self):
        s=(fm.MODULE/'v1/engine/operator.lib').read_text()
        self.assertIn('started * audibleVelocity',s);self.assertIn('ba.sAndH(gate > gate\')',s)
        self.assertIn('1120.0/16777216.0',s)
        e=(fm.MODULE/'v1/engine/env.lib').read_text()
        self.assertIn('coldInit : keyDown',e);self.assertIn('staticcount_b / sr_multiplier',e)
    def test_reference_does_not_certify_unsupported_features(self):
        s=(ROOT/'tools/modules/fm6_msfa_oracle.cpp').read_text()
        self.assertIn('algorithm==4||algorithm==6',s);self.assertIn('if(patch[140])',s)
    def test_event_grid_and_velocity_accents(self):
        e=fm.musical_score([57,60,64,67]);self.assertTrue(all(n%64==0 for n,_,_ in e))
        self.assertEqual(len([1 for _,k,v in e if k=='gate' and v]),4)
    def test_whole_baseline_not_overwritten(self):
        import hashlib
        h=hashlib.sha1()
        p=ROOT/'tools/modules/dx7_slice1.py';b=p.read_bytes();h.update(f'blob {len(b)}\0'.encode()+b)
        self.assertEqual(h.hexdigest(),'1742e4a5542c8f95021e9575b6643d00ecb7d99b')
if __name__=='__main__':unittest.main()
