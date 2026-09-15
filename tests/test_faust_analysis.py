import importlib.util, pathlib, tempfile, unittest
import numpy as np
ROOT=pathlib.Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('fa',ROOT/'tools/modules/faust_analysis.py'); fa=importlib.util.module_from_spec(spec);spec.loader.exec_module(fa)

class AnalysisContract(unittest.TestCase):
 def test_source_keeps_integrated_approximation_explicit(self):
  text=(ROOT/'tools/modules/faust_analysis.py').read_text()
  self.assertIn('streaming_approx',text)
  self.assertIn('No normalization',text)
 def test_meter_contract_is_six_outputs(self):
  text=(ROOT/'tools/modules/faust_analysis_meter.dsp').read_text()
  for token in ('an.true_peak','an.loudness_momentary','an.loudness_shortterm','an.loudness_integrated'):
   self.assertIn(token,text)
 def test_sha_is_content_hash(self):
  with tempfile.TemporaryDirectory() as d:
   p=pathlib.Path(d)/'x';p.write_bytes(b'abc')
   self.assertEqual(fa.sha(p),'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad')

if __name__=='__main__': unittest.main()
