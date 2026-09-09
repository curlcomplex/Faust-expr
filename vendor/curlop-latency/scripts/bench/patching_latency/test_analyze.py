#!/usr/bin/env python3
"""Negative controls for the evidence evaluator, not substitute Faust tests."""
import array, math, tempfile, unittest
from pathlib import Path
import analyze

class EvaluationTests(unittest.TestCase):
    def cases(self):
        return [dict(name=f'{m}-{f}-{n}-f{b}',mode=m,family=f,size=n,frames=b,returncode=0)
                for m,f,n,b in analyze.definitions()]
    def test_complete_manifest(self):
        self.assertEqual(len(self.cases()),24)
        analyze.validate_cases(self.cases())
    def test_missing_manifest_rejected(self):
        for cases in ([],self.cases()[:-1]):
            with self.assertRaises(ValueError):analyze.validate_cases(cases)
    def test_duplicate_rejected(self):
        c=self.cases();c[-1]=c[0]
        with self.assertRaises(ValueError):analyze.validate_cases(c)
    def test_wrong_identity_rejected(self):
        c=self.cases();c[0]['frames']=512
        with self.assertRaises(ValueError):analyze.validate_cases(c)
    def test_mismatched_lengths(self):
        with self.assertRaises(ValueError):analyze.maximum_error([0],[0,0])
    def test_nonfinite_metric_rejected(self):
        for v in ([],[float('nan')],[float('inf')]):
            with self.assertRaises(ValueError):analyze.stats(v)
    def test_nearest_rank_small_sample(self):
        self.assertEqual(analyze.stats(range(1,10)),dict(n=9,median=5,p95_nearest_rank=9,max=9))
    def test_bad_audio_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d)/'x.f32'
            for b in (b'',b'123',array.array('f',[float('nan')]).tobytes()):
                p.write_bytes(b)
                with self.assertRaises(ValueError):analyze.samples(p)
    def test_first_sample_error_visible(self):
        self.assertEqual(analyze.maximum_error([.5,0],[0,0]),.5)
    def test_no_gain_or_alignment_fit(self):
        self.assertGreater(analyze.maximum_error([.1,.2,.3],[.2,.4,.6]),.1)
        self.assertGreater(analyze.maximum_error([.1,.2,.3],[0,.1,.2]),.05)
if __name__=='__main__':unittest.main(verbosity=2)
