"""#109 contracts and opt-in pinned Faust/interp-tracer qualification."""
import importlib.util, os, pathlib, tempfile, unittest
ROOT=pathlib.Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('cd',ROOT/'tools/modules/compiler_diagnostics.py');cd=importlib.util.module_from_spec(spec);spec.loader.exec_module(cd)
FIX=ROOT/'tests/fixtures/diagnostics'

class Contracts(unittest.TestCase):
 def test_compile_policy(self):
  self.assertFalse(cd.classify_compile({'returncode':0,'stdout':'warning: x','stderr':''})['hard_failure'])
  self.assertTrue(cd.classify_compile({'returncode':1,'stdout':'','stderr':'error'})['hard_failure'])
 def test_fixtures_are_distinct(self):
  self.assertNotEqual(cd.sha(FIX/'safe.dsp'),cd.sha(FIX/'domain_error.dsp'))

@unittest.skipUnless(os.environ.get('COMPILER_DIAGNOSTICS_INTEGRATION')=='1','requires pinned Faust diagnostics toolchain')
class RealDiagnostics(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  cls.tmp=tempfile.TemporaryDirectory();cls.addClassCleanup(cls.tmp.cleanup);cls.root=pathlib.Path(cls.tmp.name)
  cls.faust=os.environ['FAUST'];cls.libs=os.environ['FAUST_LIBRARIES'];cls.tracer=os.environ['INTERP_TRACER']
 def d(self,name,tracer=False,structural=False):
  return cd.diagnose(FIX/name,self.faust,self.libs,self.root/name,self.tracer if tracer else None,4,structural)
 def test_safe_wall_me_compile(self):
  r=self.d('safe.dsp');self.assertFalse(r['compiler_classification']['hard_failure']);self.assertIn('2.88.0',r['faust_version'])
 def test_me_rejects_known_domain_error(self):
  r=self.d('domain_error.dsp');self.assertTrue(r['compiler_classification']['hard_failure'])
  self.assertTrue('log' in (r['compiler']['stdout']+r['compiler']['stderr']).lower() or 'domain' in (r['compiler']['stdout']+r['compiler']['stderr']).lower())
 def test_tracer_surfaces_hidden_divzero(self):
  r=self.d('divzero_hidden.dsp',True);text=r['interp_tracer']['stdout']+r['interp_tracer']['stderr']
  self.assertTrue('DIV_BY_ZERO' in text or 'div' in text.lower());self.assertTrue(r['interp_tracer']['hard_failure'] or 'DIV_BY_ZERO' in text)
 def test_tracer_surfaces_nonfinite_output_case(self):
  r=self.d('divzero_output.dsp',True);text=r['interp_tracer']['stdout']+r['interp_tracer']['stderr']
  self.assertTrue(any(x in text for x in ('DIV_BY_ZERO','FP_INFINITE','FP_NAN')))
 def test_structural_is_backend_labeled(self):
  r=self.d('safe.dsp',False,True);self.assertEqual(r['structural']['backend'],'ocpp');self.assertIn('never runtime-performance',r['structural']['interpretation'])

if __name__=='__main__':unittest.main()
