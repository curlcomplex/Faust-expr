"""#107 contracts plus opt-in real Faust trend qualification."""
import importlib.util, math, os, pathlib, tempfile, unittest
import numpy as np
ROOT=pathlib.Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('st',ROOT/'tools/modules/spectral_trace.py');st=importlib.util.module_from_spec(spec);spec.loader.exec_module(st)

class Contracts(unittest.TestCase):
 def test_names_do_not_alias_fft_metrics(self):
  self.assertTrue(all('filterbank' in x for x in st.FIELDS[1:]))
  self.assertEqual(st.CONFIG['bands_per_octave'],3)

@unittest.skipUnless(os.environ.get('SPECTRAL_TRACE_INTEGRATION')=='1','requires pinned Faust')
class RealDescriptors(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  cls.tmp=tempfile.TemporaryDirectory(); cls.addClassCleanup(cls.tmp.cleanup); cls.root=pathlib.Path(cls.tmp.name)
  cls.runner,cls.prov=st.build(cls.root/'build',os.environ['FAUST'],os.environ.get('CXX','c++'),os.environ['FAUST_LIBRARIES'])
 def measure(self,name,x,rate=48000,stride=240):
  p=self.root/(name+'.f32');np.asarray(x,dtype='<f4').tofile(p);return st.analyze(p,rate,self.root/name,self.runner,self.prov,stride)
 def settled(self,r,start_s): return [p for p in r['trace'] if p['time_seconds']>=start_s]
 def test_centroid_tracks_frequency_direction(self):
  r=48000;t=np.arange(r*2)/r
  low=self.measure('low',0.2*np.sin(2*np.pi*500*t)); high=self.measure('high',0.2*np.sin(2*np.pi*4000*t))
  lc=np.median([p[st.FIELDS[1]] for p in self.settled(low,1)]);hc=np.median([p[st.FIELDS[1]] for p in self.settled(high,1)])
  self.assertGreater(hc,lc*3)
 def test_spread_increases_for_two_separated_tones(self):
  r=48000;t=np.arange(r*2)/r
  one=self.measure('one',0.1*np.sin(2*np.pi*1000*t)); two=self.measure('two',0.1*(np.sin(2*np.pi*500*t)+np.sin(2*np.pi*4000*t)))
  a=np.median([p[st.FIELDS[2]] for p in self.settled(one,1)]);b=np.median([p[st.FIELDS[2]] for p in self.settled(two,1)])
  self.assertGreater(b,a*1.5)
 def test_flux_marks_onset_more_than_steady_state(self):
  r=48000;t=np.arange(r*2)/r;x=np.zeros(r*2);x[r//2:]=0.2*np.sin(2*np.pi*1000*t[:len(x)-r//2])
  report=self.measure('onset',x,stride=48); points=report['trace']; onset=max(p[st.FIELDS[3]] for p in points if .49<=p['time_seconds']<=.60); steady=np.median([p[st.FIELDS[3]] for p in points if 1.2<=p['time_seconds']<=1.8])
  self.assertGreater(onset,max(steady*10,1e-6))
 def test_bright_to_dark_trace_moves_down(self):
  r=48000;n=r*3;t=np.arange(n)/r;f=np.where(t<1.5,4000.,500.);phase=2*np.pi*np.cumsum(f)/r;x=.15*np.sin(phase)
  report=self.measure('bright-dark',x); early=np.median([p[st.FIELDS[1]] for p in report['trace'] if .7<p['time_seconds']<1.3]);late=np.median([p[st.FIELDS[1]] for p in report['trace'] if 2.2<p['time_seconds']<2.8]);self.assertGreater(early,late*3)

if __name__=='__main__':unittest.main()
