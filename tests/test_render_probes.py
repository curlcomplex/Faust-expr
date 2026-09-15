"""#106: native UI contracts and opt-in, actual pinned-Faust probe qualification.

FAUST_PROBE_INTEGRATION=1 runs real generated DSP, not mock signal equations.
"""
from __future__ import annotations
import importlib.util
import ctypes
import json
import os
import re
from pathlib import Path
import shutil
import sys
import subprocess
import tempfile
import unittest

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
FIXTURES = ROOT/'tests/fixtures/probes'

def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod

sys.path.insert(0,str(ROOT/'tools/modules'))
lab_module = load('probe_lab', ROOT/'tools/modules/probe_lab.py')
analysis = load('probe_toolchain', ROOT/'tools/modules/faust_analysis.py')

# This stub tests the C++ collector, not Faust DSP. Real integration is below.
STUB = '''class ModuleDSP { public:
    void init(int) {} int getNumInputs(){return 0;} int getNumOutputs(){return 1;}
    void buildUserInterface(UI*) {} void compute(int, float**, float**) {}
};'''
HARNESS = r'''
#define main unused_score_main
#include "render.cpp"
#undef main
#include <cassert>
#include <functional>
static void rejects(const std::function<void()>& f) {
    bool caught=false; try { f(); } catch (const std::exception&) { caught=true; }
    if (!caught) throw std::runtime_error("expected rejection");
}
int main(int argc,char** argv) {
    if(argc!=2) return 2;
    std::string test=argv[1]; float a=.5f,b=.25f,m=2.5f,n=-.5f;
    UI ui;
    if(test=="hierarchy") {
        ui.declare(nullptr,"description","stage \"A\"\nnext"); ui.openVerticalBox("A/B");
        ui.declare(&a,"unit","linear"); ui.addHorizontalSlider("gain",&a,.5f,0,1,.01f);
        ui.addHorizontalBargraph("Level",&m,-1,1); ui.declare(&m,"probe","10"); ui.declare(&m,"hidden","1");
        ui.closeBox(); ui.openVerticalBox("A%2FB");
        ui.addHorizontalSlider("gain",&b,.25f,0,1,.01f);
        ui.declare(&n,"probe","11"); ui.addVerticalBargraph("Level",&n,-1,1); ui.closeBox();
        ui.finish(); assert(ui.zones.size()==2); rejects([&]{ui.set("gain",.1f);});
        ui.set("/A%2FB/gain",.75f); assert(a==.75f && b==.25f);
        ui.set("/A%252FB/gain",.5f); assert(b==.5f);
        auto values=ui.meter_values(); assert(*std::max_element(values.begin(),values.end())==2.5f); ui.json(std::cout,0,1);
    } else if(test=="read-only") {
        ui.addHorizontalSlider("gain",&a,.5f,0,1,.01f);
        ui.openVerticalBox("Meter"); ui.addHorizontalBargraph("gain",&m,-1,1); ui.declare(&m,"probe","8"); ui.closeBox();
        ui.finish(); assert(ui.zones.size()==1); ui.set("gain",.7f); assert(a==.7f && m==2.5f);
        rejects([&]{ui.set("/Meter/gain",.1f);}); rejects([&]{ui.set("probe:8",.1f);});
        rejects([&]{ui.set("gain",2.f);}); rejects([&]{ui.finishCleanBenchmark();});
    } else if(test=="duplicate-id") {
        ui.declare(&m,"probe","1"); ui.addHorizontalBargraph("one",&m,-1,1);
        ui.declare(&n,"probe","1"); ui.addHorizontalBargraph("two",&n,-1,1);
        rejects([&]{ui.finish();});
    } else if(test=="pointer-access") {
        ui.addHorizontalSlider("gain",&a,.5f,0,1,.01f); ui.addHorizontalBargraph("meter",&a,-1,1);
        rejects([&]{ui.finish();});
    } else if(test=="writable-probe") {
        ui.declare(&a,"probe","1"); ui.addHorizontalSlider("gain",&a,.5f,0,1,.01f);
        rejects([&]{ui.finish();});
    } else if(test=="invalid-layout") {
        rejects([&]{ui.closeBox();});
        ui.declare(&a,"unit","Hz"); rejects([&]{ui.declare(&a,"unit","dB");});
        ui.addHorizontalSlider("x",&a,.5f,0,1,.01f);
        rejects([&]{ui.addHorizontalSlider("x",&b,.25f,0,1,.01f);});
        ui.openTabBox("Unclosed"); rejects([&]{ui.finish();});
    } else if(test=="json-escaping") {
        ui.openHorizontalBox("spaces / %");
        ui.addHorizontalSlider("quoted\"\t\n\\",&a,.5f,0,1,.01f); ui.closeBox();
        ui.finishCleanBenchmark(); ui.json(std::cout,0,1);
    } else return 3;
}
'''

class NativeProbeUI(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which(os.environ.get('CXX','c++'))
        if not compiler:
            raise unittest.SkipTest('native UI contracts require a C++17 compiler')
        cls.temp = tempfile.TemporaryDirectory(); cls.addClassCleanup(cls.temp.cleanup)
        root = Path(cls.temp.name)
        (root/'generated.hpp').write_text(STUB)
        (root/'harness.cpp').write_text(HARNESS)
        cls.exe = root/'contracts'
        subprocess.run([compiler,'-std=c++17','-O1','-I'+str(root),'-I'+str(ROOT/'tools/modules'),
                        str(root/'harness.cpp'),'-o',str(cls.exe)],check=True,capture_output=True,text=True,timeout=90)

    def execute(self, test):
        return subprocess.check_output([str(self.exe),test],text=True,timeout=10)

    def test_hierarchy_metadata_before_after_and_collision_free_paths(self):
        ui=json.loads(self.execute('hierarchy'))
        self.assertEqual(ui['probe_count'],2)
        self.assertEqual(ui['groups'][0]['metadata']['description'],'stage "A"\nnext')
        self.assertEqual({m['label'] for m in ui['meters']},{'Level'})
        self.assertEqual(len({m['path'] for m in ui['meters']}),2)
        self.assertTrue(all(m['default'] is None for m in ui['meters']))

    def test_read_only_never_writable_or_in_control_contract(self): self.execute('read-only')
    def test_duplicate_probe_identity_is_rejected(self): self.execute('duplicate-id')
    def test_read_only_pointer_cannot_alias_writable_zone(self): self.execute('pointer-access')
    def test_probe_metadata_on_writable_control_is_rejected(self): self.execute('writable-probe')
    def test_invalid_layout_and_conflicting_metadata_are_rejected(self): self.execute('invalid-layout')
    def test_json_and_path_escaping(self):
        ui=json.loads(self.execute('json-escaping'))
        z=ui['controls'][0]
        self.assertEqual(z['label'],'quoted"\t\n\\')
        self.assertNotIn('\t',z['path']); self.assertIn('%2F',z['path'])


@unittest.skipUnless(os.environ.get('FAUST_PROBE_INTEGRATION')=='1',
                     'requires explicitly enabled pinned Faust probe integration')
class RealFaustProbes(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp=tempfile.TemporaryDirectory(); cls.addClassCleanup(cls.temp.cleanup)
        cls.root=Path(cls.temp.name)
        cls.evidence=Path(os.environ.get('FAUST_PROBE_EVIDENCE',cls.root/'evidence')).resolve()
        cls.evidence.mkdir(parents=True,exist_ok=True)
        faust=analysis.executable(os.environ['FAUST'])
        libraries=Path(os.environ['FAUST_LIBRARIES']).resolve()
        version=analysis.run([faust,'--version'])
        if version.splitlines()[0]!='FAUST Version 2.88.0': raise ValueError('requires pinned Faust 2.88.0')
        manifest=analysis.library_manifest(libraries)
        archive=analysis.verify_release_archive(Path(os.environ['FAUST_ARCHIVE']),manifest)
        cls.lab=lab_module.ProbeLab(cls.evidence)
        cls.exes={}
        for name in ('clean','disabled','enabled','duplicate_labels','multiband','untagged','nonfinite_meter'):
            cls.exes[name]=cls.lab.build(name,FIXTURES/(name+'.dsp'),diagnostic=name not in ('clean','disabled','untagged'))
        cls.exes['original']=cls.lab.build('original', ROOT/'tests/fixtures/probe_debug.dsp',diagnostic=True)
        # Prove that bypassing ProbeLab.build cannot slip active probes into a clean render.
        folder=cls.exes['enabled'].parent
        cls.unsafe=folder/'incorrectly-clean-render'
        lab_module.run([cls.lab.cpp,'-std=c++17','-O2','-I'+str(folder),str(ROOT/'tools/modules/render.cpp'),'-o',str(cls.unsafe)])
        provenance={'faust_version':version,'faust_binary_sha256':analysis.sha(faust),
                    'release_archive_sha256':archive,'library_manifest':manifest,
                    'runner_source_sha256':analysis.sha(ROOT/'tools/modules/render.cpp'),
                    'cxx_version':analysis.run([cls.lab.cpp,'--version']),
                    'execution_lane':os.environ.get('EXECUTION_LANE','assistant-sandbox'),
                    'github_sha':os.environ.get('GITHUB_SHA'), 'github_run_id':os.environ.get('GITHUB_RUN_ID'),
                    'builds':cls.lab.builds}
        (cls.evidence/'provenance.json').write_text(json.dumps(provenance,indent=2,sort_keys=True)+'\n')

    def ui(self,name):
        return json.loads(lab_module.run([str(self.exes[name]),'--ui-json']))

    def render(self,name,samples,*,block=128,stride=31,events=(),label=None,capture=True):
        folder=self.evidence/(label or self._testMethodName+'-'+name+'-'+str(block)+'-'+str(stride))
        folder.mkdir(parents=True,exist_ok=True)
        inp,score,raw,trace=(folder/n for n in ('input.f32','score.tsv','audio.f32','probes.jsonl'))
        np.asarray(samples,dtype='<f4').tofile(inp)
        score.write_text(''.join(f'{n}\t{k}\t{v}\n' for n,k,v in events))
        args=[self.exes[name],score,raw,48000,block,len(samples),0,inp]
        if name not in ('clean','disabled','untagged'): args+=['--diagnostic']
        if name not in ('clean','disabled','untagged') and capture:
            args+=['--probes',trace,'--probe-stride',stride]
        result=json.loads(analysis.run(args))
        audio=np.fromfile(raw,dtype='<f4').reshape(len(samples),result['channels'])
        records=[json.loads(line) for line in trace.read_text().splitlines()] if trace.exists() else []
        (folder/'result.json').write_text(json.dumps(result,indent=2)+'\n')
        return audio,result,records

    def test_legacy_controls_are_identical_with_probes_on_off_removed(self):
        contracts=[lab_module.run([str(self.exes[n]),'--controls']) for n in ('clean','disabled','enabled')]
        self.assertEqual(contracts[0],contracts[1]); self.assertEqual(contracts[0],contracts[2])
        self.assertEqual(contracts[0].splitlines(),['io\t1\t1','gain\t0\t1\t0.5\t0'])
        self.assertEqual(self.ui('enabled')['probe_count'],6)
        self.assertEqual(self.ui('disabled')['probe_count'],0)
        self.assertEqual(self.ui('clean')['probe_count'],0)
        self.assertTrue(all(m['metadata']['hidden']=='1' for m in self.ui('enabled')['meters']))

    def test_one_sample_transients_events_windows_and_block_invariance(self):
        x=np.zeros(2053,np.float32); x[3]=.6; x[511]=-.4; x[1024:1500]=.2; x[-1]=.5
        events=[(0,'gain',1),(512,'gain',.5),(1501,'gain',.75)]
        expected=x.copy(); expected[512:1501]*=.5; expected[1501:]*=.75
        clean,_,_=self.render('clean',x,events=events)
        disabled,_,_=self.render('disabled',x,events=events)
        np.testing.assert_array_equal(clean[:,0],expected)
        np.testing.assert_array_equal(disabled,clean)
        baseline=None
        for block in (1,127,256,511):
            audio,diag,trace=self.render('enabled',x,block=block,stride=31,events=events)
            np.testing.assert_array_equal(audio,clean)
            self.assertIsNone(diag['instrumented_compute_ns']); self.assertFalse(diag['benchmark_eligible'])
            self.assertEqual(diag['compute_calls'],len(x))
            idx=next(i for i,z in enumerate(trace[0]['ui']['meters']) if z['probe_id']=='11')
            self.assertEqual(trace[-1],{'type':'complete','frames':len(x),'windows':67})
            self.assertEqual(trace[0]['sampling'],'after-each-compute-1')
            for row in trace[1:-1]:
                a,b=row['frame_begin'],row['frame_end_exclusive']; v=row['values'][idx]
                self.assertAlmostEqual(v['min'],float(expected[a:b].min()),places=7)
                self.assertAlmostEqual(v['max'],float(expected[a:b].max()),places=7)
                self.assertAlmostEqual(v['last'],float(expected[b-1]),places=7)
                self.assertEqual(row['last_frame'],b-1)
                self.assertAlmostEqual(row['last_time_seconds'],(b-1)/48000,places=10)
            if baseline is None: baseline=trace[1:]
            else: self.assertEqual(trace[1:],baseline)
        (self.evidence/'transient-summary.json').write_text(json.dumps({
            'frames':len(x),'blocks':[1,127,256,511],'stride':31,'windows':67,
            'single_sample_peak_frame':3,'captured_raw_probe_max':float(expected.max()),
            'audio_bit_identical':True,'meter_windows_identical':True},indent=2)+'\n')

    def test_stride_one_and_fresh_state_repeatability(self):
        x=np.random.default_rng(106).normal(0,.1,503).astype('<f4')
        a,_,ta=self.render('enabled',x,stride=1,label='repeat-a')
        b,_,tb=self.render('enabled',x,stride=1,label='repeat-b')
        np.testing.assert_array_equal(a,b); self.assertEqual(ta,tb)
        idx=next(i for i,z in enumerate(ta[0]['ui']['meters']) if z['probe_id']=='11')
        self.assertEqual(len(ta)-2,len(x))
        for n,row in enumerate(ta[1:-1]): self.assertAlmostEqual(row['values'][idx]['last'],float(x[n]*.5),places=7)

    def test_real_level_envelope_dc_slew_values(self):
        audio,_,trace=self.render('enabled',np.full(48000,.4,np.float32),stride=480)
        final={z['probe_id']:trace[-2]['values'][i]['last'] for i,z in enumerate(trace[0]['ui']['meters'])}
        for p in ('11','12','13','14','15'): self.assertAlmostEqual(final[p],.2,delta=5e-4)
        self.assertLess(abs(final['16']),1e-5)
        np.testing.assert_array_equal(audio[:,0],np.full(48000,.2,np.float32))

    def test_duplicate_labels_preserve_paths_ids_and_metadata(self):
        ui=self.ui('duplicate_labels')
        self.assertEqual({m['label'] for m in ui['meters']},{'Level'})
        self.assertEqual({m['probe_id'] for m in ui['meters']},{'101','102'})
        left=next(g for g in ui['groups'] if g['label']=='Left')
        self.assertEqual(left['metadata']['description'],'left stage')
        controls={z['path'].split('/')[-2]:z['path'] for z in ui['controls']}
        audio,_,trace=self.render('duplicate_labels',np.ones(65,np.float32),
                                 events=[(0,controls['Left'],.75),(0,controls['Right'],.125)])
        np.testing.assert_array_equal(audio,np.tile(np.array([.75,.125],np.float32),(65,1)))
        self.assertEqual({v['last'] for v in trace[-2]['values']},{.75,.125})
        self.assertTrue(all(row.split('\t')[0].startswith('/') for row in
                            lab_module.run([str(self.exes['duplicate_labels']),'--controls']).splitlines()[1:]))

    def test_multiband_derived_probe_ids(self):
        ui=self.ui('multiband')
        self.assertEqual({m['probe_id'] for m in ui['meters']},{str(i) for i in range(200,208)})
        x=np.sin(np.arange(997)*.21).astype('<f4')*.1
        audio,_,trace=self.render('multiband',x,stride=127)
        np.testing.assert_array_equal(audio[:,0],x)
        self.assertEqual(len(trace[-2]['values']),8)

    def test_raw_meter_is_not_clamped_to_ui_range(self):
        audio,_,trace=self.render('enabled',np.full(65,2,np.float32),events=[(0,'gain',1)])
        idx=next(i for i,z in enumerate(trace[0]['ui']['meters']) if z['probe_id']=='11')
        self.assertEqual(trace[-2]['values'][idx]['last'],2)
        self.assertEqual(float(audio.max()),2)

    def test_lab_records_capture_identity_and_separates_builds(self):
        x=np.zeros(101,np.float32);x[1]=.3
        self.lab.render('lab-capture',self.exes['enabled'],controls=False,input_audio=x,
                        seconds=len(x)/48000,diagnostic=True,probe_stride=17)
        result=self.lab.renders[-1]
        self.assertIn('diagnostic-renders',result['probe_capture']['path'])
        self.assertEqual(result['probe_capture']['binary_sha256'],analysis.sha(self.exes['enabled']))
        self.assertFalse(self.lab.builds['enabled']['benchmark_eligible'])
        self.assertTrue(self.lab.builds['clean']['benchmark_eligible'])
        with self.assertRaisesRegex(ValueError,'scalar'):
            self.lab.build('not-vector',FIXTURES/'enabled.dsp',vector=True,diagnostic=True)
        with self.assertRaisesRegex(ValueError,'diagnostic=True'):
            self.lab.build('reject-active',FIXTURES/'enabled.dsp')
        self.assertFalse((self.evidence/'reject-active/render').exists())
        (self.evidence/'lab-capture-report.json').write_text(json.dumps(result,indent=2)+'\n')

    def test_illegal_writes_and_benchmark_mode_are_rejected(self):
        root=self.root/'rejections';root.mkdir(exist_ok=True)
        score,inp,raw,trace=(root/n for n in ('score.tsv','input.f32','out.f32','probe.jsonl'))
        np.ones(64,'<f4').tofile(inp)
        def reject(exe,rows='',options=(),needle=None):
            score.write_text(rows)
            p=subprocess.run([str(a) for a in [exe,score,raw,48000,32,64,0,inp,*options]],
                             capture_output=True,text=True,timeout=10)
            self.assertNotEqual(p.returncode,0)
            if needle:self.assertIn(needle,p.stderr)
        for meter in self.ui('enabled')['meters']:
            reject(self.exes['enabled'],f'0\t{meter["path"]}\t0\n',['--diagnostic'], 'read-only')
        reject(self.exes['enabled'],options=(),needle='dedicated')
        reject(self.unsafe,options=(),needle='dedicated')
        reject(self.unsafe,options=['--diagnostic'],needle='dedicated')
        reject(self.exes['clean'],options=['--diagnostic'],needle='dedicated')
        reject(self.exes['clean'],options=['--probes',trace],needle='dedicated')
        reject(self.exes['enabled'],options=['--diagnostic','--probe-stride','0'],needle='stride')
        reject(self.exes['duplicate_labels'],'0\tgain\t0.5\n',['--diagnostic'], 'ambiguous')
        path=self.ui('enabled')['controls'][0]['path']
        reject(self.exes['enabled'],f'0\tgain\t0.5\n0\t{path}\t0.5\n',['--diagnostic'],'duplicate event')
        reject(self.exes['enabled'],options=['--diagnostic','--probes',inp],needle='paths must differ')

    def test_original_branch_fixture_is_preserved_and_qualified(self):
        ui=self.ui('original')
        self.assertEqual({m['probe_id'] for m in ui['meters']},{'1060','1061'})
        x=((np.arange(256)%17-8)/8).astype(np.float32)
        audio,_,trace=self.render('original',x,stride=8)
        np.testing.assert_array_equal(audio[:,0],x*.5)
        self.assertEqual(trace[-1]['windows'],32)

    def test_untagged_bargraphs_remain_read_only_without_dirtying_clean_builds(self):
        ui=self.ui('untagged')
        self.assertEqual(ui['probe_count'],0)
        self.assertTrue(ui['benchmark_eligible'])
        self.assertEqual(len(ui['controls']),1)
        self.assertEqual(len(ui['meters']),1)
        self.assertIsNone(ui['meters'][0]['probe_id'])
        audio,result,_=self.render('untagged',np.ones(97,np.float32))
        self.assertTrue(result['benchmark_eligible'])
        self.assertIsInstance(result['instrumented_compute_ns'],int)
        np.testing.assert_array_equal(audio[:,0],np.full(97,.5,np.float32))

    def test_nonfinite_meter_fails_even_with_finite_audio(self):
        # No hidden NaN can be serialized as valid diagnostic evidence.
        with self.assertRaisesRegex(RuntimeError,'nonfinite meter'):
            self.render('nonfinite_meter',np.full(16,-.1,np.float32),label='nonfinite')
        trace=self.evidence/'nonfinite/probes.jsonl'
        self.assertTrue(trace.is_file())
        self.assertNotIn('"type":"complete"',trace.read_text())

    def test_fitting_abis_work_clean_and_reject_diagnostic_builds(self):
        folder=self.evidence/'fit-abi'; folder.mkdir(exist_ok=True)
        # Tiny actual-Faust gated constant, so expected samples are exact.
        source=folder/'fit.dsp'
        source.write_text('gate=button("gate");level=hslider("level",.5,0,1,.01);process=gate*level;\n')
        lab_module.run([self.lab.faust,'-lang','cpp','-single','-cn','ModuleDSP',str(source),'-o',str(folder/'generated.hpp')])
        values=np.array([0,.5],np.float64);output=np.empty(64,np.float32)
        dp=ctypes.POINTER(ctypes.c_double); fp=ctypes.POINTER(ctypes.c_float)
        for file,fn in (('fit_api.cpp','module_render'),('color_fit_api.cpp','color_render')):
            for diagnostic in (False,True):
                binary=folder/(file+('-diagnostic' if diagnostic else '-clean')+'.so')
                flags=['-DFAUST_EXPR_DIAGNOSTIC=1'] if diagnostic else []
                lab_module.run([self.lab.cpp,'-std=c++17','-O2','-shared','-fPIC',*flags,'-I'+str(folder),
                                str(ROOT/'tools/modules'/file),'-o',str(binary)])
                function=getattr(ctypes.CDLL(str(binary)),fn)
                function.restype=ctypes.c_int
                function.argtypes=[dp,ctypes.c_int,ctypes.c_int,ctypes.c_int]+([ctypes.c_int]*2 if fn=='color_render' else [])+[fp]
                args=[values.ctypes.data_as(dp),2,48000,64]+([16,32] if fn=='color_render' else [])+[output.ctypes.data_as(fp)]
                code=function(*args)
                self.assertEqual(code,-6 if diagnostic else 0)
                if not diagnostic:
                    expected=np.zeros(64,np.float32);expected[:16 if fn=='color_render' else 1]=.5
                    np.testing.assert_array_equal(output,expected)


    def test_all_existing_benchmark_wrappers_finalize_and_refuse_diagnostics(self):
        # Exercise wrapper compatibility, NOT actual instrument or device performance.
        # A tiny actual-Faust gated fixture carries the union of required controls.
        wrappers=sorted((ROOT/'tools/modules').glob('*benchmark.cpp'))
        wrappers=[p for p in wrappers if '#include "render.cpp"' in p.read_text()]
        self.assertEqual(len(wrappers),10)
        names=set()
        for wrapper in wrappers:
            names.update(re.findall(r'->set\("([^"\n]+)"',wrapper.read_text()))
        names.discard('gate')
        declarations=['gate=button("gate");']
        declarations += [f'c{i}=hslider("{name}",.5,0,10000,.01);' for i,name in enumerate(sorted(names))]
        signal='attach(gate*.25,'+'+'.join(f'c{i}' for i in range(len(names)))+')'
        records=[]
        for channels in (1,2):
            folder=self.evidence/f'benchmark-wrapper-{channels}'
            folder.mkdir(exist_ok=True)
            source=folder/'fixture.dsp'
            source.write_text('\n'.join(declarations)+f'\nprocess={signal}'+(' <: _,_;' if channels==2 else ';')+'\n')
            lab_module.run([self.lab.faust,'-lang','cpp','-single','-cn','ModuleDSP',str(source),'-o',str(folder/'generated.hpp')])
            for wrapper in wrappers:
                if (wrapper.name.startswith('morph_')) != (channels==2): continue
                for diagnostic in (False,True):
                    binary=folder/(wrapper.stem+('-diagnostic' if diagnostic else '-clean'))
                    flags=['-DFAUST_EXPR_DIAGNOSTIC=1'] if diagnostic else []
                    lab_module.run([self.lab.cpp,'-std=c++17','-O2',*flags,'-I'+str(folder),str(wrapper),'-o',str(binary)])
                    result=subprocess.run([str(binary),'16'],capture_output=True,text=True,timeout=15)
                    if diagnostic:
                        self.assertNotEqual(result.returncode,0)
                        self.assertIn('benchmark rejects diagnostic',result.stderr)
                        self.assertFalse(result.stdout.strip())
                    else:
                        self.assertEqual(result.returncode,0,result.stderr)
                        stats=json.loads(result.stdout)
                        self.assertTrue(np.isfinite(stats['checksum']))
                    records.append({'wrapper':wrapper.name,'diagnostic':diagnostic,
                                    'expected_rejection':diagnostic,'exit_code':result.returncode,
                                    'wrapper_sha256':analysis.sha(wrapper)})
        (self.evidence/'benchmark-wrapper-checks.json').write_text(json.dumps({
            'scope':'control/guard compatibility on a tiny actual-Faust fixture, NOT instrument performance',
            'checks':records},indent=2)+'\n')


if __name__=='__main__': unittest.main()
