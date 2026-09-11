"""Actual-Faust clap v3 build, contract tests, renders and audible negative control.
Numerical tests are not musical approval. Reference study is a separate report.
"""
from pathlib import Path
import argparse, hashlib, itertools, json, os, subprocess, sys
import numpy as np
from scipy.io import wavfile
from scipy.signal import welch
ROOT=Path(__file__).resolve().parents[2]
DEFAULT=dict(spacing=.40,tone=.48,snap=.62,decay=.32,tail=.35,drive=.08,pitch_hz=210.,velocity=1.,gate=0.)
PRESETS={
 '01_Classic':{},
 '02_Tight':dict(spacing=.18,snap=.86,decay=.24,tail=.30,tone=.56),
 '03_Wide':dict(spacing=.72,snap=.64,decay=.48,tail=.43,tone=.43),
 '04_Dark':dict(spacing=.49,snap=.56,decay=.40,tail=.38,tone=.12),
 '05_Bright':dict(spacing=.40,snap=.83,decay=.38,tail=.32,tone=.86),
 '06_Room':dict(spacing=.50,snap=.55,decay=.78,tail=.82,tone=.44),
 '07_Driven':dict(spacing=.48,snap=.75,decay=.46,tail=.44,tone=.40,drive=.62),
 '08_Dry':dict(spacing=.56,snap=.66,decay=.10,tail=0.,tone=.48),
}
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def cmd(args):return subprocess.check_output([str(a) for a in args],text=True,stderr=subprocess.STDOUT,timeout=120)
def features(x,rate=48000):
    x=np.asarray(x,dtype=float); f,p=welch(np.pad(x,(512,512)),rate,nperseg=min(1024,len(x)),noverlap=min(768,len(x)//2)); p/=p.sum()+1e-30
    e=x*x;c=np.cumsum(e);c/=c[-1]+1e-30
    return dict(low_600_pct=float(100*p[f<600].sum()),mid_1k_4k_pct=float(100*p[(f>=1000)&(f<4000)].sum()),high_6k_pct=float(100*p[f>=6000].sum()),centroid_hz=float((f*p).sum()),t90_ms=float(np.searchsorted(c,.9)*1000/rate))
def run(out):
    out.mkdir(parents=True,exist_ok=True);report=dict(version='0.3.0-experiment',checks=[],renders=[],builds={},human_approved=False,device_qualified=False)
    def check(name,ok,**kw):
        report['checks'].append(dict(name=name,passed=bool(ok),**kw))
        if not ok:raise AssertionError(name+': '+str(kw))
    def build(label,src,vec=False):
        d=out/label;d.mkdir(exist_ok=True)
        args=[os.getenv('FAUST','faust'),'-I',src.parent,'-lang','cpp','-single','-cn','ModuleDSP']
        if vec:args+=['-vec','-lv','0','-vs','32']
        cmd(args+[src,'-o',d/'generated.hpp'])
        cmd([os.getenv('CXX','c++'),'-std=c++17','-O2','-ffp-contract=off','-I'+str(d),ROOT/'tools/modules/render.cpp','-o',d/'render'])
        controls=cmd([d/'render','--controls']);(d/'controls.tsv').write_text(controls)
        report['builds'][label]=dict(source=os.path.relpath(src,ROOT),source_sha256=sha(src),header_sha256=sha(d/'generated.hpp'),controls=controls)
        return d/'render'
    def render(name,exe,p=None,ev=None,rate=48000,block=128,sec=1.3,defaults=DEFAULT):
        values=defaults|(p or {});rows={(0,k):v for k,v in values.items()}
        for n,k,v in ev or [(101,'gate',1),(165,'gate',0)]:rows[n,k]=v
        score=out/(name+'.tsv');raw=out/(name+'.f32')
        score.write_text(''.join(f'{n}\t{k}\t{v:.9g}\n' for (n,k),v in sorted(rows.items())))
        frames=round(sec*rate);dg=json.loads(cmd([exe,score,raw,rate,block,frames,0]));x=np.fromfile(raw,'<f4')
        check(name+':finite',len(x)==frames and np.isfinite(x).all() and abs(x).max()<1.,peak=float(abs(x).max()))
        report['renders'].append(dict(name=name,rate=rate,block=block,sha256=sha(raw),score_sha256=sha(score),diagnostics=dg))
        return x
    try:
        src=ROOT/'modules/clap/v3/clap.dsp';exe=build('scalar',src);vec=build('vector',src,True)
        old=build('rejected-v2',ROOT/'modules/clap/v2/clap.dsp')
        olddef={k:v['default'] for k,v in json.loads((ROOT/'modules/clap/v2/manifest.json').read_text())['controls'].items()}
        previous=render('rejected_v2_default',old,defaults=olddef)
        wavfile.write(out/'00_REJECTED_v2_default.wav',48000,previous)
        result=render('default',exe)
        check('pre-trigger-silence',not np.any(result[:101]))
        silent=render('never-triggered',exe,ev=[(1,'gate',0)],sec=.2);check('untriggered-silence',not np.any(silent))
        zero=render('zero-velocity',exe,dict(velocity=0));check('velocity-zero',not np.any(zero))
        half=render('half-velocity',exe,dict(velocity=.5));check('linear-velocity',np.max(abs(half-result*.5))<2e-7)
        held=render('held-gate',exe,ev=[(101,'gate',1)]);check('noteoff-does-not-choke',np.array_equal(held,result))
        changes=[(101,'gate',1),(165,'gate',0)]+[(4001,k,(900 if k=='pitch_hz' else 1)) for k in DEFAULT if k not in ('gate','velocity')]
        locked=render('latched',exe,ev=changes);check('controls-latch-on-trigger',np.array_equal(locked,result))
        for b in (1,32,64,127,256,512):check('block-invariance-'+str(b),np.array_equal(render('block-'+str(b),exe,block=b),result))
        v=render('vector-parity',vec);check('scalar-vector',np.max(abs(v-result))<3e-5,max_abs=float(np.max(abs(v-result))))
        peaks=[]
        for rate in (44100,48000,96000):
            for name,p in PRESETS.items():
                x=render(name+'-'+str(rate),exe,p,rate=rate,sec=3.)
                peaks.append(float(abs(x).max()))
                if rate==48000:wavfile.write(out/(name+'.wav'),rate,x)
        ev=[];keys=['spacing','tone','snap','decay','tail','drive']
        for i,bits in enumerate(itertools.product((0.,1.),repeat=6)):
            n=101+i*2400;ev +=[(n,k,v) for k,v in zip(keys,bits)]+[(n,'pitch_hz',70 if i%2 else 900),(n,'gate',1),(n+1,'gate',0)]
        corners=render('64-corners',exe,ev=ev,sec=5.);check('corner-mean',abs(float(corners.mean()))<.01)
        ev=[]
        for i in range(96):
            n=101+i*400;ev += [(n,'spacing',(i%7)/6),(n,'tone',(i%11)/10),(n,'gate',1),(n+1,'gate',0)]
        render('96-rapid-retriggers',exe,ev=ev,sec=2.)
        end=render('long-tail',exe,dict(decay=1,tail=1),sec=8.);check('tail-settles',abs(end[-4800:]).max()<1e-7)
        mutation=out/'collapsed.dsp';mutation.write_text(src.read_text().replace('t-.97*gap','t').replace('t-1.98*gap','t').replace('t-last','t'))
        collapsed=build('collapsed',mutation)
        collapsedx=render('collapsed-single-hit',collapsed)
        wavfile.write(out/'diagnostic_collapsed.wav',48000,collapsedx)
        def flutter(x):
            y=x[101:101+round(.025*48000)].astype(float)
            n=48;return [float(np.sqrt(np.mean(y[i:i+n]**2))) for i in range(0,len(y)-n,n)]
        report['flutter_1ms_rms']={'new':flutter(result),'old':flutter(previous),'collapsed':flutter(collapsedx)}
        def onset_contrast(x):
            gap=.004+.024*DEFAULT['spacing']**2
            ratios=[]
            for onset in (.97*gap,1.98*gap):
                at=101+round(onset*48000)
                a=x[at+12:at+120].astype(float);b=x[at-100:at-12].astype(float)
                ratios.append(float(10*np.log10((np.mean(a*a)+1e-20)/(np.mean(b*b)+1e-20))))
            return ratios
        contrasts={name:onset_contrast(x) for name,x in [('new',result),('old',previous),('collapsed',collapsedx)]}
        report['secondary_attack_contrast_db']=contrasts
        check('two-resolved-secondary-attacks',min(contrasts['new'])>10,values=contrasts['new'])
        check('attack-gate-rejects-v2',min(contrasts['old'])<6,values=contrasts['old'])
        check('attack-gate-rejects-collapsed',min(contrasts['collapsed'])<6,values=contrasts['collapsed'])
        report['descriptors']={'new_default':features(result),'old_default':features(previous)}
        check('reject-pitched-snare-negative-control',report['descriptors']['new_default']['low_600_pct']<5 and report['descriptors']['old_default']['low_600_pct']>20,old=report['descriptors']['old_default']['low_600_pct'],new=report['descriptors']['new_default']['low_600_pct'])
        ev=[]
        for i,(name,p) in enumerate(PRESETS.items()):
            n=2400+i*96000;ev += [(n,k,v) for k,v in (DEFAULT|p).items() if k!='gate']+[(n,'gate',1),(n+1,'gate',0),(n+36000,'velocity',.65),(n+36000,'gate',1),(n+36001,'gate',0)]
        bank=render('bank',exe,ev=ev,sec=17.);wavfile.write(out/'clap_v3_bank_raw.wav',48000,bank)
        report['peak_presets']=max(peaks);report['passed']=True
        report['compilers']={'faust':cmd([os.getenv('FAUST','faust'),'--version']),'cxx':cmd([os.getenv('CXX','c++'),'--version']).splitlines()[0]}
        report['presets']=PRESETS
    finally:
        (out/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({'passed':report['passed'],'checks':len(report['checks']),'renders':len(report['renders'])}))
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--out',type=Path,required=True);a=p.parse_args();run(a.out.resolve())
