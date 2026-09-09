import unittest
import analyze_retained as a
class Checks(unittest.TestCase):
    def test_inventory(self):self.assertEqual(len(a.definitions()),46);self.assertEqual(len(set(a.definitions())),46)
    def test_spike(self):
        with self.assertRaises(ValueError):a.compare([.5,0],[0,0])
    def test_length(self):
        with self.assertRaises(ValueError):a.compare([0],[0,0])
    def test_nan(self):
        with self.assertRaises(ValueError):a.compare([float('nan')],[0])
    def test_infinity(self):
        with self.assertRaises(ValueError):a.compare([0],[float('inf')])
    def test_no_gain(self):
        with self.assertRaises(ValueError):a.compare([.1,.2],[.2,.4])
    def test_no_shift(self):
        with self.assertRaises(ValueError):a.compare([.1,.2],[0,.1])
    def test_bound(self):self.assertEqual(a.compare([0],[0]),0)
if __name__=='__main__':unittest.main(verbosity=2)
