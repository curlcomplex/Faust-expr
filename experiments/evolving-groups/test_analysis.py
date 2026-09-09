import copy,math,unittest
import analyze_adaptive as a
class EvidenceTests(unittest.TestCase):
    def cases(self):return [dict(name=f'{m}-{f}-{n}-g{w}-b{b}',mode=m,family=f,size=n,width=w,frames=b,returncode=0) for m,f,n,w,b in a.definitions()]
    def test_inventory(self):self.assertEqual(len(a.definitions()),26);a.check_manifest(self.cases())
    def test_missing(self):
        with self.assertRaises(ValueError):a.check_manifest(self.cases()[:-1])
    def test_duplicate(self):
        c=self.cases();c[-1]=c[0]
        with self.assertRaises(ValueError):a.check_manifest(c)
    def test_wrong_shape(self):
        c=self.cases();c[0]['frames']=256
        with self.assertRaises(ValueError):a.check_manifest(c)
    def test_no_fit(self):
        with self.assertRaises(ValueError):a.compare([.1,.2],[.2,.4])
    def test_no_shift(self):
        with self.assertRaises(ValueError):a.compare([.1,.2],[0,.1])
    def test_nonfinite(self):
        for x in (float('nan'),float('inf')):
            with self.assertRaises(ValueError):a.compare([x],[0])
    def test_length(self):
        with self.assertRaises(ValueError):a.compare([0],[0,0])
    def test_negative_time(self):
        with self.assertRaises(ValueError):a.stats([-1])
    def test_empty_time(self):
        with self.assertRaises(ValueError):a.stats([])
    def test_hidden_input(self):
        g=dict(modules=[dict(id=i) for i in range(3)],groups=[dict(owner=1,parallel=0,members=[1,2])],edges=[dict(source=0,target=2)])
        with self.assertRaises(ValueError):a.verify_layout(g)
    def test_hidden_output(self):
        g=dict(modules=[dict(id=i) for i in range(3)],groups=[dict(owner=1,parallel=0,members=[1,2])],edges=[dict(source=1,target=0)])
        with self.assertRaises(ValueError):a.verify_layout(g)
    def test_duplicate_group(self):
        g=dict(modules=[dict(id=i) for i in range(3)],groups=[dict(owner=1,parallel=0,members=[1,2])]*2,edges=[])
        with self.assertRaises(ValueError):a.verify_layout(g)
    def test_event_count(self):self.assertEqual(len(a.EVENTS),18)
if __name__=='__main__':unittest.main(verbosity=2)
