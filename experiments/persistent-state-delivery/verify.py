#!/usr/bin/env python3
"""Final read-only reconciliation. No native benchmark evaluator is imported.
Recheck online evidence with the separate reader, then validate source correction,
reuse identities and actual master-sample change timing. Do not call deadline
failures a product pass just because numerical continuity passed.
"""
from pathlib import Path
import argparse,csv,hashlib,json,shutil,statistics,tarfile
import numpy as np

def read(p):return json.loads(p.read_text())
def digest(p):
    h=hashlib.sha256()
    with p.open('rb') as f:
        for b in iter(lambda:f.read(1048576),b''):h.update(b)
    return h.hexdigest()
def tsv(p):
    with p.open() as f:return list(csv.DictReader(f,delimiter='\t'))
def archive_files(path):
    with tarfile.open(path) as tf:return {m.name:tf.extractfile(m).read() for m in tf.getmembers() if m.isfile()}
def csvwrite(path,rows,fields):
    with path.open('w') as f:
        w=csv.DictWriter(f,fieldnames=fields);w.writeheader();w.writerows(rows)
def main(online,prior,out,repo):
    out.mkdir(parents=True,exist_ok=True);identity=read(online/'identity.json');assert identity['head']=='6c0359889aa0898787ed14815c561217aec40731' and identity['online_passed']
    old_identity=read(prior/'identity.json');assert old_identity['head']=='008be1e5e9b8572ac3f9ef937e5cb391c9566b3c'
    old_sources=archive_files(prior/'executed-experiment-source.tar.gz');new_sources=archive_files(online/'executed-source.tar.gz')
    for name,want in identity['source_sha256'].items():assert hashlib.sha256(new_sources[name]).hexdigest()==want,name
    prefix='experiments/persistent-state-next/'
    original=old_sources[prefix+'live.h'];assert new_sources[prefix+'live.h']==original
    corrected=original
    for field in ('state_instances_created_during_edits','state_bytes_copied_during_edits'):
        a=f'prop(result,"{field}",0)'.encode();b=f'prop(result,"{field}",V(0))'.encode();assert corrected.count(a)==1;corrected=corrected.replace(a,b)
    assert new_sources[prefix+'live-typed.generated.h']==corrected
    main_old=old_sources[prefix+'main.generated.cpp'];main_new=new_sources[prefix+'main.generated.cpp'];assert main_old.count(b'#include "live.h"')==1
    assert main_new==main_old.replace(b'#include "live.h"',b'#include "live-typed.generated.h"')
    assert new_sources[prefix+'audit_live.py']==old_sources[prefix+'audit_live.py']
    static=read(prior/'static-state/independent-audit.json');reg=read(prior/'regression/original-regression-independent.json');contract=read(prior/'contract/postflight-contract.json')
    assert static['gate_positive'] and len(static['gate_cases'])==19 and len(static['transitions'])==19
    assert reg['passed'] and len(reg['cases'])==46 and contract['all_module_frame_calls_inlined']
    assert all(c['indirect_calls']==0 for c in contract['compiled_loop_inspection'])
    repeated=read(out/'live-independent-audit.json');native=read(online/'independent/live-independent-audit.json');assert repeated['passed'] and native['passed'] and repeated==native
    # Required source + binary identity checks on the exact reused kernels.
    kernel_count=0
    for name,pins in identity['kernel_pins'].items():
        folder=online/'kernels'/name;manifest=read(folder/'manifest.json');assert manifest['schema']==pins['schema']
        for path,want in {**manifest['source_hashes'],**manifest['binary_hashes']}.items():assert digest(folder/path)==want
        assert manifest['binary_hashes']==pins['binary_hashes'];kernel_count+=len(manifest['binary_hashes'])
    rows=[];phase_rows=[];qualified_raw={};qualified_samples=0
    for item in repeated['cases']:
        name=item['name'];folder=online/'live'/name;meta=read(folder/'live-result.json');blocks=tsv(folder/'callbacks.tsv');channels=meta['channels'];frames=meta['frames'];size=(channels//2)-3
        x=np.fromfile(folder/'live-output.f32',dtype='<f4').reshape(-1,channels);z=np.fromfile(folder/'live-noedit-negative.f32',dtype='<f4').reshape(x.shape)
        master=2*(size+1);delta=np.max(np.abs(x[:,master:master+2].astype('f8')-z[:,master:master+2].astype('f8')),axis=1)
        before=[];hit=None
        for index,block in enumerate(blocks):
            lo=index*frames;hi=lo+frames
            if block['revision']=='0':before.extend(delta[lo:hi].tolist())
            if block['revision']=='1' and block['optimized']=='0' and hit is None:
                points=np.flatnonzero(delta[lo:hi]>1e-5)
                if len(points):hit=(index,lo+int(points[0]),float(block['end_us']))
        assert hit is not None and hit[2]<meta['compile_end_us']
        assert max(before,default=0)<=1e-5
        row={'name':name,'stress':meta['stress'],'copies':meta['state_copies_for_load'],
             'request_to_first_editable_block_ms':item['edit_response_ms'],
             'request_to_first_master_changed_block_ms':(hit[2]-meta['request_us'])/1000,
             'first_master_changed_absolute_frame':hit[1],
             'request_to_optimized_block_ms':item['optimized_response_ms'],'compile_ms':item['compile_ms'],
             'initial_median_budget_fraction':item['phases']['initial-optimized']['median_budget_fraction'],
             'editable_median_budget_fraction':item['phases']['editable-during-compile']['median_budget_fraction'],
             'optimized_B_median_budget_fraction':item['phases']['new-optimized']['median_budget_fraction'],
             'editable_compute_over_budget':item['phases']['editable-during-compile']['compute_over_budget'],
             'editable_block_count':item['phases']['editable-during-compile']['compute_us']['count'],
             'max_waveform_error':item['max_waveform_error']}
        rows.append(row)
        for phase,p in item['phases'].items():phase_rows.append({'name':name,'phase':phase,'count':p['compute_us']['count'],'median_compute_us':p['compute_us']['median'],'p95_compute_us':p['compute_us']['p95'],'max_compute_us':p['compute_us']['maximum'],'median_budget_fraction':p['median_budget_fraction'],'compute_over_budget':p['compute_over_budget'],'synthetic_schedule_misses':p['synthetic_schedule_misses']})
        for f in folder.glob('*.f32'):
            key=name+'/'+f.name;qualified_raw[key]=digest(f);qualified_samples+=f.stat().st_size//4
    performance=[]
    for c in static['gate_cases']:
        for kind,medians in c['median_ns_per_frame'].items():
            performance.append({'name':c['name'],'measurement':kind,**medians,'shared_over_whole_llvm':c['ratios'][kind]})
    csvwrite(out/'live-response-and-load.csv',rows,list(rows[0]));csvwrite(out/'live-phase-tail-costs.csv',phase_rows,list(phase_rows[0]));csvwrite(out/'steady-state-processing.csv',performance,list(performance[0]))
    # Preserve complete sample records from two state-sensitive live tests.
    memorydir=out/'selected-full-memory-evidence';memorydir.mkdir(exist_ok=True)
    for name in ('live-memory-8-b128','live-stress-memory-8-b128'):
        src=online/'live'/name;dst=memorydir/name;dst.mkdir(exist_ok=True)
        for f in src.iterdir():
            if f.suffix in ('.f32','.json','.tsv','.cpp') or f.name.endswith('-command.txt'):shutil.copy2(f,dst/f.name)
    for filename in ('identity.json','metadata-correction.json','executed-source.tar.gz','environment.txt'):shutil.copy2(online/filename,out/('online-'+filename))
    shutil.copytree(prior,out/'preceding-gate-audits',dirs_exist_ok=True)
    for filename in ('independent-audit.json','live-independent-audit.json'):
        # live auditor output already in destination; static audit kept in copied preceding tree.
        pass
    report=repo/'research/persistent-state-transition-result.md'
    if report.exists():shutil.copy2(report,out/'Faust-persistent-state-results.md')
    result={'source_correction_verified':'Only two report-zero expressions and the header include changed; DSP and strict live evaluator unchanged',
            'static_native_head':old_identity['head'],'online_native_head':identity['head'],
            'static_cases':19,'precompiled_continuity_cases':19,'unchanged_original_regressions':46,'online_cases':8,
            'typed_online_raw_captures':len(qualified_raw),'typed_online_float_samples':qualified_samples,'typed_online_raw_sha256':qualified_raw,
            'first_changed_master_block_threshold':1e-5,'first_changed_master_block_before_compilation':rows,
            'verified_reused_native_kernel_binaries':kernel_count,'verified_online_source_files':len(identity['source_sha256']),
            'required_output_cost_ratios':static['ratios'],'case_median_cost_ratios':{k:statistics.median(c['ratios'][k] for c in static['gate_cases']) for k in ['dsp','with-observers']},
            'real_time_acceptance':False,'reason':'Normal VM thread misses schedule; heavy parallel editable median exceeds the full callback budget. Numerical success is not real-time acceptance.'}
    (out/'final-reconciliation.json').write_text(json.dumps(result,indent=2)+'\n')
    print('FINAL_RECONCILIATION_PASS',19,19,46,8,'cases; online raw',len(qualified_raw),'floats',qualified_samples,flush=True)
    print('COST_RATIOS',result['required_output_cost_ratios'],'CASE_MEDIAN',result['case_median_cost_ratios'],flush=True)
    for row in rows:print('FINAL_RESPONSE_AND_LOAD',json.dumps(row),flush=True)
    print('REALTIME_ACCEPTANCE',False,flush=True)
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('online',type=Path);p.add_argument('prior',type=Path);p.add_argument('out',type=Path);p.add_argument('repo',type=Path);a=p.parse_args();main(a.online,a.prior,a.out,a.repo)
