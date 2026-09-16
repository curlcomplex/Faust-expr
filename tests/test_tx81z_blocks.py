import pathlib, unittest
ROOT=pathlib.Path(__file__).parents[1]
class TX81ZBlocks(unittest.TestCase):
 def test_extracted_surface_is_small_and_machine_specific(self):
  s=(ROOT/'modules/tx81z/v11/blocks.lib').read_text()
  for name in ('phaseStep','phase','envelope','totalTL','op','feedbackOp','in3','in2','in1','carriers'):
   self.assertIn(name+'(',s)
  self.assertNotIn('generic',s.lower())
 def test_recomposition_uses_extracted_boundaries(self):
  s=(ROOT/'modules/tx81z/v11/recomposed_voice.dsp').read_text()
  for name in ('b.envelope','b.phaseStep','b.phase','b.totalTL','b.feedbackOp','b.op','b.in3','b.in2','b.in1','b.carriers'):
   self.assertIn(name,s)
 def test_alternate_is_not_full_voice_alias(self):
  s=(ROOT/'modules/tx81z/v11/alternate.dsp').read_text()
  self.assertIn('b.envelope',s); self.assertIn('b.op',s); self.assertNotIn('v10/voice.dsp',s)
if __name__=='__main__': unittest.main()
