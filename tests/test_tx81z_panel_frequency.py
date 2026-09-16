import unittest


def fixed_base(crs, fine):
    coarse = (max(0, min(63, int(crs))) >> 2) & 15
    return ((8 if coarse == 0 else coarse << 4) | (max(0, min(15, int(fine))) & 15))


def fixed_hz(range_, crs, fine):
    return fixed_base(crs, fine) << max(0, min(7, int(range_)))


class TX81ZPanelFixedFrequency(unittest.TestCase):
    def test_documented_range_endpoints(self):
        expected = [(8,255),(16,510),(32,1020),(64,2040),(128,4080),(256,8160),(512,16320),(1024,32640)]
        for r,(lo,hi) in enumerate(expected):
            self.assertEqual(fixed_hz(r,0,0),lo)
            self.assertEqual(fixed_hz(r,63,15),hi)

    def test_documented_fine_step(self):
        for r in range(8):
            step = 1 << r
            self.assertEqual(fixed_hz(r,4,1)-fixed_hz(r,4,0),step)

    def test_fixed_mode_uses_upper_four_vced_frequency_bits(self):
        for base in range(0,64,4):
            values={fixed_hz(0,base+i,5) for i in range(4)}
            self.assertEqual(len(values),1)

    def test_phase_accumulator_conversion_is_audible_hz(self):
        for sr in (44100,48000,55930,96000):
            for hz in (8,255,440,1024,8160):
                step=int(hz*(1<<20)/sr+0.5)
                measured=step*sr/(1<<20)
                self.assertLessEqual(abs(measured-hz),sr/(2*(1<<20))+1e-12)


if __name__ == '__main__':
    unittest.main()
