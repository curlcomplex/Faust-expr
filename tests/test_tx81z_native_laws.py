"""Input/evidence tests only; real Faust/native execution is the hosted law suite."""
import csv
import math
import sys
import tempfile
import unittest
from pathlib import Path
import numpy as np
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools/modules'))
from tx81z_frequency_analysis import FIELDS,expected_keys,parse_csv,evaluate
from tx81z_prepare import prepare,standalone_source
from tx81z_laws_qualification import residual


class FrequencyInput(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.addCleanup(self.tmp.cleanup)
        self.path=Path(self.tmp.name)/'native.csv'
        self.rows=[]
        for key in expected_keys():
            r=dict(zip(FIELDS,key));base=4001+r['block']*97+r['code']*11
            if r['kind']=='ratio':step=(base*(((r['coarse']<<4) if r['coarse'] else 8)|r['fine']))>>4
            elif r['kind']=='dt2':step=int(base*2**([0,600,781,950][r['dt2']]/1200))
            elif r['kind']=='dt1':step=base+((r['dt1']&3)*(r['block']+r['code']+1))*(-1 if r['dt1']>=4 else 1)
            else:step=75*((((r['fixed']<<4) if r['fixed'] else 8)|r['fine'])<<r['range'])/4096
            r.update(step=step,baseline=base,ratio=step/base);self.rows.append(r)

    def save(self):
        with self.path.open('w') as f:
            w=csv.DictWriter(f,fieldnames=FIELDS);w.writeheader();w.writerows(self.rows)
        return self.path

    def test_complete_quantized_fixture(self):
        checks=[];rows=parse_csv(self.save());evaluate(rows,checks)
        self.assertEqual(len(rows),3060);self.assertEqual(len(checks),951)
        self.assertTrue(all(c['passed'] for c in checks))

    def test_ideal_ratio_is_not_integer_law(self):
        r=next(r for r in self.rows if r['kind']=='ratio' and r['coarse']==0 and r['fine']==1)
        r['step']=r['baseline']*9/16;r['ratio']=r['step']/r['baseline']
        with self.assertRaises((AssertionError,ValueError)):evaluate(parse_csv(self.save()),[])

    def test_step_mutation_detected(self):
        self.rows[0]['step']+=1;self.rows[0]['ratio']=self.rows[0]['step']/self.rows[0]['baseline']
        with self.assertRaises(AssertionError):evaluate(parse_csv(self.save()),[])

    def test_missing_case(self):
        self.rows.pop()
        with self.assertRaises(ValueError):parse_csv(self.save())

    def test_duplicate_case(self):
        self.rows[-1]=self.rows[0].copy()
        with self.assertRaises(ValueError):parse_csv(self.save())

    def test_nonfinite(self):
        self.rows[0]['step']=float('nan')
        with self.assertRaises(ValueError):parse_csv(self.save())

    def test_stale_ratio(self):
        self.rows[0]['ratio']+=.01
        with self.assertRaises(ValueError):parse_csv(self.save())

    def test_bad_header(self):
        self.path.write_text('kind,other\n')
        with self.assertRaises(ValueError):parse_csv(self.path)


class EvidenceInput(unittest.TestCase):
    def test_identity(self):
        self.assertEqual(residual(np.arange(32),np.arange(32)),(0.,0.))

    def test_one_sample_fault(self):
        x=np.zeros(32);y=x.copy();y[19]=1
        self.assertEqual(residual(x,y)[0],1)

    def test_shape_empty_and_nonfinite(self):
        for a,b in [(np.zeros(2),np.zeros(3)),([],[]),([np.nan],[0]),([0],[np.inf])]:
            with self.assertRaises(ValueError):residual(a,b)

    def test_source_hash_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);(root/'src').mkdir();(root/'src/ymfm_fm.ipp').write_text('unreviewed source')
            with self.assertRaises(ValueError):prepare(root,root/'tables.lib')
            self.assertFalse((root/'tables.lib').exists())

    def test_scoped_export_preserves_expressions(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);(root/'v5').mkdir();(root/'YMFM-LICENSE.txt').write_text('fixture licence')
            source=root/'v5/voice.dsp';source.write_text('declare name "Test";\nimport("stdfaust.lib");\nopz=library("opz_core.lib");\nprocess=opz.f;\n')
            (root/'v5/opz_core.lib').write_text('import("stdfaust.lib");\nt=library("opz_tables.lib");\nf=t.x*(1.0/32768.0);\n')
            (root/'v5/opz_tables.lib').write_text('x=7;\n')
            out,hashes=standalone_source(source)
            self.assertNotIn('library(',out);self.assertEqual(out.count('import('),1)
            self.assertIn('1.0/32768.0',out);self.assertEqual(len(hashes),2)
            (root/'v5/opz_core.lib').write_text('t=library("../unknown.lib");\n')
            with self.assertRaises(ValueError):standalone_source(source)

if __name__=='__main__':unittest.main()
