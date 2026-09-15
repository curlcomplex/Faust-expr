import unittest

# Independent expected dependency/carrier sets from pinned ymfm s_algorithm_ops.
EXPECTED = {
  0: ({2:{1},3:{2},4:{3}}, {4}),
  1: ({3:{1,2},4:{3}}, {4}),
  2: ({3:{2},4:{1,3}}, {4}),
  3: ({2:{1},4:{2,3}}, {4}),
  4: ({2:{1},4:{3}}, {2,4}),
  5: ({2:{1},3:{1},4:{1}}, {2,3,4}),
  6: ({2:{1}}, {2,3,4}),
  7: ({}, {1,2,3,4}),
}
class Algorithms(unittest.TestCase):
  def test_all_eight_present(self): self.assertEqual(set(EXPECTED),set(range(8)))
  def test_serial(self): self.assertEqual(EXPECTED[0],({2:{1},3:{2},4:{3}},{4}))
  def test_parallel(self): self.assertEqual(EXPECTED[7],({}, {1,2,3,4}))
  def test_all_have_output(self):
    for _,carriers in EXPECTED.values(): self.assertTrue(carriers)
if __name__=='__main__': unittest.main()
