import tempfile,unittest
from pathlib import Path
import analyze_policy as a
import make_adapter as m
class Check(unittest.TestCase):
    def test_inventory(self):
        self.assertEqual(len(a.definitions()),140);self.assertEqual(len({x['name'] for x in a.definitions()}),140)
    def test_shards(self):
        s=[{x['name'] for x in a.definitions(i)} for i in (0,1)];self.assertFalse(s[0]&s[1]);self.assertEqual(len(s[0]|s[1]),140)
    def test_same_machine_comparison_blocks(self):
        for s in (0,1):
            groups={}
            for c in a.definitions(s):groups.setdefault((c['mode'],c['family'],c['frames'],c['variant']),[]).append(c['policy'])
            self.assertTrue(all(set(v)==set(a.POLICIES) for v in groups.values()))
    def test_deterministic(self):self.assertEqual(a.definitions(),a.definitions())
    def test_bad_audio(self):
        for x,y in [([float('nan')],[0]),([0],[float('inf')]),([.5],[0]),([] ,[]),([0],[0,0])]:
            with self.assertRaises(ValueError):a.compare(x,y)
    def test_no_fit(self):
        with self.assertRaises(ValueError):a.compare([.1,.2],[.2,.4])
    def test_bad_time(self):
        for x in [[],[-1],[float('nan')]]:
            with self.assertRaises(ValueError):a.stats(x)
    def test_reset_accounting(self):
        before={'compiled_units':[{'index':1,'code':'old'}],'modules':[{'id':1,'identity':'a'},{'id':2,'identity':'b'}]}
        after={'compiled_units':[{'index':1,'code':'new'}],'modules':before['modules'],'groups':[{'owner':1,'members':[1,2]}]}
        self.assertEqual(a.invalidation(before,after),({1,2},{1,2},1))
    def test_added_not_collateral(self):
        b={'compiled_units':[],'modules':[]};n={'compiled_units':[{'index':1,'code':'new'}],'modules':[{'id':1,'identity':'a'}],'groups':[]}
        self.assertEqual(a.invalidation(b,n),({1},set(),1))
    def test_duplicate_member(self):
        with self.assertRaises(ValueError):a.membership({'groups':[{'owner':1,'members':[1,2]},{'owner':2,'members':[2,3]}]})
    def test_seam_preserves_renderer(self):
        old=Path(__file__).resolve().parent.parent/'evolving-groups'
        with tempfile.TemporaryDirectory() as d:
            m.generate(old,Path(d));src=(Path(d)/'AdaptivePolicy.generated.h').read_text();base=(old/'AdaptiveGroups.h').read_text()
            self.assertEqual(src.replace(m.POLICY,'').replace('policySeed(view,width_)','seed(view,width_)'),base)
    def test_policy_has_no_trace_access(self):
        for token in ('variant','target','pins','future','trace'):
            # The comment may describe absence of future knowledge; body cannot use it.
            self.assertNotIn(token,m.POLICY[m.POLICY.index('inline Layout'):])
if __name__=='__main__':unittest.main(verbosity=2)
