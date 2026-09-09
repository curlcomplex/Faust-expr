"""Reproducible full snare batch plus fair noise-tail ablation and delivery checks.
The original kernels and descriptors are preserved, not retuned in this pass.
--replay uses frozen generated C++ and frozen licensed references, offline.
"""
from __future__ import annotations
import argparse
import json
import math
import os
from pathlib import Path
import shutil
import statistics
import sys
import numpy as np
from scipy.io import wavfile
from scipy.signal import resample_poly
import snare_batch as base

ROOT = Path(__file__).resolve().parents[2]
MOD = ROOT / 'modules/snare-pm'


def validate_events(events, frames, manifest):
    seen = set()
    for frame, name, value in events:
        if isinstance(frame, bool) or not isinstance(frame, int) or not 0 <= frame < frames:
            raise ValueError('invalid sample offset')
        base.validate({name: value}, manifest)
        if (frame, name) in seen:
            raise ValueError('duplicate authored event')
        seen.add((frame, name))


def reference_hashes(path):
    manifest = json.loads((path/'manifest.json').read_text())
    expected = ('SDB-01','SDB-04','SDB-07','SDB-10','SDV-01','SDV-05','SDV-09','SDV-13')
    if tuple(manifest['selection_before_audio_analysis']) != expected:
        raise ValueError('reference selection changed')
    for entry in manifest['records']:
        for suffix, key in (('.mp3','mp3_sha256'),('.wav','wav_sha256')):
            if base.sha(path/(entry['id']+suffix)) != entry[key]:
                raise ValueError('reference digest mismatch')
    return manifest


class Delivery(base.Study):
    def __init__(self, out, references, replay=None):
        super().__init__(out, references)
        self.replay = replay
        self.frozen = json.loads((replay/'report.json').read_text()) if replay else None
        self.report['reference_manifest_sha256'] = base.sha(references/'manifest.json')
        reference_hashes(references)
        if self.frozen:
            if self.frozen['reference_manifest_sha256'] != self.report['reference_manifest_sha256']:
                raise ValueError('use the exact frozen reference manifest')
            for relative, digest in self.frozen['source_files'].items():
                if base.sha(ROOT/relative) != digest:
                    raise ValueError('replay source differs: '+relative)
            self.report['replay_of_source_commit'] = self.frozen.get('source_commit')

    def build(self, label, source, vector=False):
        if self.replay:
            d = self.out/label; d.mkdir(exist_ok=True)
            expected = self.frozen['builds'][label]
            old = self.replay/label/'generated.hpp'
            if base.sha(old) != expected['generated_sha256']:
                raise ValueError('generated code checksum')
            shutil.copyfile(old, d/'generated.hpp')
            base.cmd([os.getenv('CXX','c++'),'-std=c++17','-O2','-ffp-contract=off',
                      '-I'+str(d),ROOT/'tools/modules/render.cpp','-o',d/'render'])
            self.report['builds'][label] = dict(expected)
            exe = d/'render'
            controls = base.cmd([exe,'--controls'])
            self.check(label+':controls', {r.split('\t')[0] for r in controls.splitlines()[1:]} == set(self.defaults))
            self.check(label+':mono', controls.splitlines()[0] == 'io\t0\t1')
        else:
            exe = super().build(label, source, vector)
            d = exe.parent
        self.report['builds'][label]['binary_sha256'] = base.sha(exe)
        return exe

    def render(self, label, exe, params=None, events=None, rate=48000, block=128, seconds=1.2):
        validate_events(events or [], round(seconds*rate), self.man)
        return super().render(label,exe,params,events,rate,block,seconds)

    def execute(self):
        # Existing suite invokes faust -v only for provenance during replay.
        # Do not fake a compiler: replace that one provenance request explicitly.
        real_cmd = base.cmd
        if self.replay:
            def replay_cmd(args, timeout=180):
                if list(map(str,args)) == [os.getenv('FAUST','faust'),'-v']:
                    return 'No Faust invocation: verified generated-C++ replay'
                return real_cmd(args, timeout)
            base.cmd = replay_cmd
        try:
            super().execute()
        finally:
            base.cmd = real_cmd
        self.report['original_suite'] = {'renders':len(self.report['renders']),'checks':len(self.report['checks'])}
        self.report['passed'] = False
        hybrid = self.out/'hybrid/render'
        tailoff = self.build('tail-off','tail-off.dsp')
        tonal = self.build('tonal-diagnostic','tonal-diagnostic.dsp')
        # Match all other stages: the old deterministic entry scales crack by .55.
        # That older two-candidate comparison was not an isolated tail ablation.
        params = []
        for i in range(48):
            rows = (self.out/f'coverage-hybrid-{i}.tsv').read_text().splitlines()
            params.append({k:float(v) for n,k,v in (r.split() for r in rows) if n=='0'})
        desc = []
        for i,p in enumerate(params):
            desc.append(base.descriptor(self.render(f'coverage-tail-off-{i}',tailoff,p,self.hit(101),seconds=1.05)))
        refs = self.refs_desc(); R = np.vstack(list(refs.values()))
        pools = {}
        for label in ('deterministic','hybrid'):
            pools[label] = np.vstack([base.descriptor(np.fromfile(self.out/f'coverage-{label}-{i}.f32',dtype='<f4')) for i in range(48)])
        # Freeze the original scaling. Adding the ablation must not change the ruler.
        scale_pool = np.vstack([R,pools['deterministic'],pools['hybrid']])
        mu = scale_pool.mean(0); sd = np.maximum(scale_pool.std(0),1e-8)
        pools['tail-off'] = np.vstack(desc)
        coverage = {}
        for label,P in pools.items():
            distance = np.sqrt((((R[:,None,:]-P[None,:,:])/sd)**2).mean(axis=2))
            indices = distance.argmin(1)
            coverage[label] = {'mean_nearest':float(distance.min(1).mean()),
                              'median_nearest':float(np.median(distance.min(1))),
                              'per_reference':{name:{'distance':float(distance[i,indices[i]]),'pool_index':int(indices[i])} for i,name in enumerate(refs)}}
        self.report['isolated_tail_coverage'] = coverage
        self.report['coverage_method'] = 'Same frozen 48 settings and original feature scaling. Only tail-off removes noiseSignal with hybrid crack/body/drive unchanged. Reused diagnostic data, NOT held-out validation or a perceptual score.'
        # Negative control: at zero tail amount both paths must agree, including crack.
        a = self.render('tail-zero-hybrid',hybrid,{'shape':0,'punch':.9},self.hit(101))
        b = self.render('tail-zero-ablation',tailoff,{'shape':0,'punch':.9},self.hit(101))
        self.check('isolated-ablation-zero-tail',np.max(abs(a-b))<2e-6,max_abs=float(np.max(abs(a-b))))
        # Direct same-sample lock oracle, across three rates; no timing alignment.
        p = dict(self.patches['Crack']) if 'Crack' in self.patches else dict(list(self.patches.values())[2])
        for rate in (44100,48000,96000):
            onset = round(.02*rate)
            locked = [(onset,k,v) for k,v in p.items()]+self.hit(onset)
            prepared = [(onset-1,k,v) for k,v in p.items()]+self.hit(onset)
            a = self.render(f'lock-onset-{rate}',hybrid,events=locked,rate=rate)
            b = self.render(f'lock-prepared-{rate}',hybrid,events=prepared,rate=rate)
            self.check(f'onset-lock-oracle:{rate}',np.array_equal(a,b))
        z = self.render('velocity-zero',hybrid,{'velocity':0},self.hit(101))
        self.check('fresh-zero-velocity-exact',not np.any(z))
        a = self.render('velocity-tail-original',hybrid,events=self.hit(101))
        b = self.render('velocity-tail-changed',hybrid,events=self.hit(101)+[(3101,'velocity',0)])
        self.check('velocity-remains-latched',np.array_equal(a,b))
        # Deterministic input excludes *all* crack/noise from the rate diagnostic.
        rate_results = []
        for inh, sh, drive in ((0,0,0),(.5,.6,0),(1,1,1)):
            p={'pitch_hz':420,'sweep':0,'punch':0,'decay':.9,'inharm':inh,'shape':sh,'drive':drive}
            lo=self.render(f'rate-48-{inh}-{drive}',tonal,p,self.hit(2400),rate=48000,seconds=1)
            hi=self.render(f'rate-96-{inh}-{drive}',tonal,p,self.hit(4800,128),rate=96000,seconds=1)
            down=resample_poly(hi.astype(float),1,2,window=('kaiser',10.))
            sl=slice(9600,38400);delta=lo[sl]-down[sl]
            ratio=np.sqrt(np.mean(delta**2))/max(1e-15,np.sqrt(np.mean(down[sl]**2)))
            rate_results.append({'inharm':inh,'shape':sh,'drive':drive,'relative_rms_db':float(20*np.log10(max(1e-15,ratio))),'max_abs':float(np.max(abs(delta)))})
        self.report['tonal_rate_diagnostic'] = {'cases':rate_results,'scope':'Equal event times and filtered 2x decimation, no noise; residual includes phase integration/filter differences and is NOT isolated alias energy.'}
        # Same authored patterns for the older alternate, preserved for listening.
        for name in ('anchors','pattern'):
            entry=next(r for r in self.report['renders'] if r['label']==name)
            exe=self.out/'deterministic/render';raw=self.out/(name+'-dry.f32')
            base.cmd([exe,self.out/(name+'.tsv'),raw,entry['rate'],entry['block'],entry['diag']['frames'],0])
            base.write_wav(self.out/('snare-'+name+'-dry.wav'),np.fromfile(raw,dtype='<f4'))
        self.reference_collage(coverage,refs)
        # Warmed four-voice paired timing, rotating order, not per-render startup.
        results=[]
        for label in ('hybrid','hybrid-vector'):
            d=self.out/label
            base.cmd([os.getenv('CXX','c++'),'-std=c++17','-O2','-ffp-contract=off','-fstack-usage',
                      '-I'+str(d),ROOT/'tools/modules/snare_benchmark.cpp','-o',d/'benchmark'])
        for block in (32,64,128,512):
            pairs=[]
            for rep in range(4):
                order=('hybrid','hybrid-vector') if rep%2==0 else ('hybrid-vector','hybrid')
                pair={label:json.loads(base.cmd([self.out/label/'benchmark',block])) for label in order}
                self.check(f'ordinary-new:{block}:{rep}',all(r['ordinary_new_allocations']==0 for r in pair.values()))
                pairs.append(pair)
            results.append({'block':block,'pairs':pairs,'median_paired_scalar_over_vector':statistics.median(p['hybrid']['p50_us']/p['hybrid-vector']['p50_us'] for p in pairs)})
        self.report['paired_four_voice_performance']=results
        self.report['benchmark_scope']='Offline four-voice warm compute including checksum, rotated same-setting pairs. Ordinary new/new[] hook only; not malloc/aligned allocation, device deadlines, UI load or thermal acceptance.'
        self.report['passed']=True

    def reference_collage(self, coverage, refs):
        parts=[];log=[];position=0
        for name in refs:
            rate,x=wavfile.read(self.refs/(name+'.wav'))
            if rate!=48000: raise ValueError('reference sample rate')
            x=np.asarray(x,dtype=float).reshape(-1)
            x=np.pad(x[:57600],(0,max(0,57600-len(x))))
            near=coverage['hybrid']['per_reference'][name]['pool_index']
            y=np.fromfile(self.out/f'coverage-hybrid-{near}.f32',dtype='<f4').astype(float)
            y=np.pad(y,(0,57600-len(y)))
            gain=np.sqrt(np.mean(x*x))/max(1e-15,np.sqrt(np.mean(y*y)))
            log.append({'reference':name,'reference_start_s':position/48000,'candidate_start_s':(position+64800)/48000,'pool_index':near,'candidate_gain':float(gain)})
            parts += [x,np.zeros(7200),y*gain,np.zeros(16800)]
            position+=57600*2+7200+16800
        audio=np.concatenate(parts);attenuation=min(1.,.8/max(1e-15,float(np.max(abs(audio)))))
        base.write_wav(self.out/'snare-reference-nearest.wav',audio*attenuation)
        self.report['reference_audition']={'timeline':log,'shared_gain':attenuation,'processing':'Same 1.2 s excerpts, whole-excerpt RMS match for candidate only, then one shared attenuation. No EQ/reverb/limiting/onset alignment. Nearest fixed-pool exemplar, NOT an individually fitted replica.'}

    def save(self,error=None):
        super().save(error)
        targets=list(MOD.glob('*'))+[ROOT/'tools/modules'/name for name in (
            'render.cpp','snare_batch.py','snare_delivery.py','snare_benchmark.cpp','fetch_snare_references.py')]
        targets += list((ROOT/'tests').glob('*snare*'))
        for path in targets:
            if path.is_file():
                rel=path.relative_to(ROOT);dest=self.out/'source'/rel
                dest.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(path,dest)
                self.report['source_files'][str(rel)]=base.sha(path)
        if self.refs.resolve()!=(self.out/'references').resolve():
            shutil.copytree(self.refs,self.out/'references',dirs_exist_ok=True)
        self.report['listening_processing']='Authored anchors/pattern/controls: fixed kernel gain and PCM16 only. Reference-nearest file has separately logged gains.'
        (self.out/'report.json').write_text(json.dumps(self.report,indent=2)+'\n')


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out',type=Path,required=True)
    p.add_argument('--references',type=Path)
    p.add_argument('--replay',type=Path)
    args=p.parse_args();frozen=args.replay.resolve() if args.replay else None
    refs=args.references.resolve() if args.references else frozen/'references' if frozen else None
    if refs is None:p.error('--references or --replay is required')
    study=Delivery(args.out.resolve(),refs,frozen);error=None
    try:study.execute()
    except Exception as e:error=str(e)
    finally:study.save(error)
    if error:raise SystemExit(error)
if __name__=='__main__':main()
