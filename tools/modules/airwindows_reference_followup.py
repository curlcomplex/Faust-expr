"""Precision follow-up to the preserved first complete Airwindows comparison.
Runs the original comparison unchanged, then adds explicit residual gates and
longer phase tests. These are numerical port criteria, NOT musical approval.
"""
import argparse,json,time
from pathlib import Path
import numpy as np
from airwindows_reference_pass import run as base_run,ReferenceLab,DEFAULTS,NEW,stimulus,errors,digest

# New absolute criteria (the first pass only required relative improvement).
# These explicitly distinguish ordinary single precision from the double oracle.
LIMITS={'tape':{'revised':2e-5,'double':2e-5},'ensemble':{'revised':.005,'double':1e-5}}
def run(out):
    base_run(out)
    base=Path(out);report=json.loads((base/'results.json').read_text())
    L=ReferenceLab(base/'precision');L.report.update(version='airwindows-precision-0.2.1',limits=LIMITS,measurements=[],musical_approval=False)
    c=L.check;r=L.render_audio;start=time.perf_counter()
    try:
        for row in report['comparisons']:
            if row['implementation'] in ('revised','double') and row['kind']=='mixed':
                limit=LIMITS[row['module']][row['implementation']]
                c(':'.join(str(row[k]) for k in ('module','preset','rate','implementation'))+':absolute-residual',row['relative_rms']<limit,relative_rms=row['relative_rms'],limit=limit)
        # Extended unseen settings: separate sample rates, more head counts,
        # wet/dry and brightness extrema. These become development cases once read.
        cases=[(44100,{'voices':2,'fullness':0,'brighten':0,'mix':1}),
               (48000,{'voices':6,'fullness':0,'brighten':1,'mix':1}),
               (96000,{'voices':48,'fullness':0,'brighten':1,'mix':1}),
               (48000,{'voices':48,'fullness':1,'brighten':1,'mix':1}),
               (44100,{'voices':31,'fullness':.123,'brighten':.27,'mix':.37})]
        for index,(sr,p) in enumerate(cases):
            x=stimulus(sr,3);ys={label:r(f'extra-{index}-{label}',base/('ensemble-original' if label=='original' else 'ensemble-v2'+('-double' if label=='double' else ''))/'render',p,x,sr=sr) for label in ('original','revised','double')}
            for label in ('revised','double'):
                e=errors(ys[label],ys['original']);L.report['measurements'].append(dict(case=f'extra-{index}',module='ensemble',rate=sr,parameters=p,implementation=label,**e))
                c(f'extra-{index}-{label}:residual',e['relative_rms']<LIMITS['ensemble'][label],**e)
        # Same prolonged stimulus reveals float phase accumulation that a short
        # compiled smoke test cannot. A source mutation removes only compensation.
        p=DEFAULTS['ensemble'];x=stimulus(48000,20)
        ys={label:r('long-'+label,base/('ensemble-original' if label=='original' else 'ensemble-v2'+('-double' if label=='double' else ''))/'render',p,x) for label in ('original','revised','double')}
        for label in ('revised','double'):
            e=errors(ys[label],ys['original']);L.report['measurements'].append(dict(case='20-second-drift',module='ensemble',rate=48000,implementation=label,**e))
            c('long-'+label+':residual',e['relative_rms']<LIMITS['ensemble'][label],**e)
        text=NEW['ensemble'].read_text();old='ne=select2(active,e,difference-y);'
        assert text.count(old)==1
        badsrc=L.out/'ensemble-no-compensation.dsp';badsrc.write_text(text.replace(old,'ne=select2(active,e,0);'))
        badexe=L.faust('negative-phase-drift',badsrc)
        bad=r('long-unguarded',badexe,p,x);baderr=errors(bad,ys['original']);gooderr=errors(ys['revised'],ys['original'])
        c('phase-drift-mutant-rejected',baderr['relative_rms']>LIMITS['ensemble']['revised'] and baderr['relative_rms']>10*gooderr['relative_rms'],without_compensation=baderr,with_compensation=gooderr)
        d=abs(ys['revised'].astype(float)-ys['original'].astype(float))
        L.report['long_difference_distribution']={'quantiles':[.5,.9,.99,.999,.9999,1],'absolute':np.quantile(d,[.5,.9,.99,.999,.9999,1]).tolist(),'samples_over_1e_4_fraction':float((d>1e-4).mean())}
        # Both cold impulse and dynamic controls remain inspectable; no isolated
        # interpolation-boundary peaks are hidden by the global RMS statistic.
        L.report['other_case_residuals']=[x for x in report['comparisons'] if x['kind']!='mixed']
        L.report['passed']=True
    except Exception as exc:
        L.report.update(passed=False,error=repr(exc),compiler_output=getattr(exc,'output',None))
        raise
    finally:
        L.report['suite_wall_seconds']=time.perf_counter()-start
        (L.out/'results.json').write_text(json.dumps(L.report,indent=2))
        print(json.dumps({'precision_passed':L.report.get('passed',False),'checks':len(L.report['checks']),'renders':len(L.report['renders']),'error':L.report.get('error')}),flush=True)
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--out',required=True);run(p.parse_args().out)
