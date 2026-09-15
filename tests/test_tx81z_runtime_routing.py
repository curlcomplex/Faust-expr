"""Pure evidence/parser contracts. Actual Faust/native work is in the CLI."""
import sys
import unittest
from pathlib import Path
import numpy as np
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools/modules'))
from tx81z_routing_qualification import same_matrix,parse_routes,extract_routing_body

class RoutingContracts(unittest.TestCase):
    def test_identity(self):
        self.assertTrue(same_matrix(np.zeros((8,4,4)),np.zeros((8,4,4))))
    def test_missing_edge_detected(self):
        a=np.zeros((8,4,4));b=a.copy();b[3,2,1]=1
        self.assertFalse(same_matrix(a,b))
    def test_gain_detected(self):
        self.assertFalse(same_matrix(np.ones((8,4,4)),np.ones((8,4,4))*.5))
    def test_nonfinite_rejected(self):
        with self.assertRaises(ValueError):same_matrix(np.full((8,4,4),np.nan),np.zeros((8,4,4)))
    def test_bad_shape_rejected(self):
        with self.assertRaises(ValueError):same_matrix(np.zeros((8,4)),np.zeros((8,4)))
    def test_bad_excerpt_rejected(self):
        with self.assertRaises(ValueError):extract_routing_body('void output_4op(){}')
    def test_csv_order_and_values(self):
        text='algorithm,basis,in2,in3,in4,carrier\n'+''.join(f'{a},{b},0,0,0,1\n' for a in range(8) for b in range(4))
        matrix=parse_routes(text)
        self.assertEqual(matrix.shape,(8,4,4));self.assertTrue((matrix[:,3,:]==1).all())
        with self.assertRaises(ValueError):parse_routes(text.replace('0,0,0,0,0,1','0,1,0,0,0,1'))
        with self.assertRaises(ValueError):parse_routes(text.replace('0,0,0,0,0,1','0,0,0,0,0,2'))
    def test_empty_or_truncated_csv_rejected(self):
        with self.assertRaises(ValueError):parse_routes('wrong header')
        with self.assertRaises(ValueError):parse_routes('algorithm,basis,in2,in3,in4,carrier\n0,0,0,0,0,1')

if __name__=='__main__':unittest.main()
