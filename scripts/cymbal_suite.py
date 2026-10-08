#!/usr/bin/env python3
"""Execute the real Faust binary, save unnormalised audio, and check behaviour."""
from __future__ import annotations
import argparse
import hashlib
import json
import subprocess
import tempfile
from pathlib import Path
import numpy as np
from scipy.io import wavfile
from scipy.signal import resample_poly


def centroid(audio,rate,start=.102,end=.180):
    x=audio[int(start*rate):int(end*rate),0].astype(float)
    power=np.abs(np.fft.rfft(x*np.hanning(len(x))))**2
    freq=np.fft.rfftfreq(len(x),1/rate)
    return float(np.sum(freq*power)/max(1e-30,np.sum(power)))


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--renderer',required=True);parser.add_argument('--out',required=True)
    args=parser.parse_args();out=Path(args.out);out.mkdir(parents=True,exist_ok=True)
    exe=str(Path(args.renderer).resolve());checks=[];renders={}; cache={}
    def check(name,condition,**data):
        checks.append(dict(name=name,passed=bool(condition),**data))
        print(('PASS ' if condition else 'FAIL ')+name,flush=True)
    def render(name,params=None,seconds=4,rate=96000,block=128,events=None,save=True):
        params=params or {}; file=out/(name+'.f32')
        cmd=[exe,str(file),'--rate',str(rate),'--seconds',str(seconds),'--block',str(block)]
        for key,value in params.items():cmd+=['--set',f'{key}={value}']
        if events is None:events=[(.1,'gate',1),(.101,'gate',0)]
        for t,key,value in events:cmd+=['--event',f'{t:.9f}:{key}={value}']
        completed=subprocess.run(cmd,text=True,capture_output=True,timeout=100)
        if completed.returncode:raise RuntimeError(f'{name}: {completed.stderr}')
        result=json.loads(completed.stdout)
        audio=np.fromfile(file,dtype='<f4').reshape(-1,2)
        check(name+' finite/count',len(audio)==round(rate*seconds) and np.isfinite(audio).all())
        check(name+' initial silence',np.max(np.abs(audio[:int(.08*rate)]))==0)
        result.update(parameters=params,events=events,raw_sha256=hashlib.sha256(file.read_bytes()).hexdigest(),
                      attack_centroid_hz=centroid(audio,rate),normalised=False)
        renders[name]=result
        if save:
            playable=resample_poly(audio,1,rate//48000,axis=0) if rate in (96000,192000) else audio
            wavrate=48000 if rate in (96000,192000) else rate
            check(name+' safe audition peak',float(np.max(np.abs(playable)))<.98,peak=float(np.max(np.abs(playable))))
            if np.max(np.abs(playable))>=.98:raise RuntimeError('Refusing to clip an audition WAV')
            wavfile.write(out/(name+'.wav'),wavrate,np.rint(playable*32767).astype('<i2'))
        if name!='edge-wood':file.unlink()
        return audio,result
    try:
        for where,r in [('bell',.16),('bow',.55),('edge',.92)]:
            for beater_name,b in [('wood',0),('felt',2)]:
                name=f'{where}-{beater_name}';audio,info=render(name,{'strike_radius':r,'beater':b})
                cache[name]=audio
                check(name+' audible',info['peak']>1e-5,peak=info['peak'])
        base=cache['edge-wood']
        check('bell/bow/edge are distinct',all(np.linalg.norm(cache[x]-base)>1e-3 for x in ['bell-wood','bow-wood']))
        for name,mat in [('steel',1),('glass',2),('wood-body',3)]:render(name,{'material':mat,'strike_radius':.7})
        for name,b in [('nylon',1),('rubber',3),('metal-striker',4)]:render(name,{'beater':b,'strike_radius':.92},seconds=2)
        render('small-16cm',{'diameter_m':.16,'strike_radius':.85})
        render('giant-220cm',{'diameter_m':2.2,'strike_radius':.85},seconds=6)
        sweep=[(.1,'gate',1),(.101,'gate',0)]+[(.7+j*.03,'diameter_m',.44*5**(j/60)) for j in range(61)]
        render('growing-while-ringing',{'strike_radius':.92},seconds=5,events=sweep)
        render('bell-shape-hammer-morph',{'strike_radius':.92},seconds=5,
               events=[(.1,'gate',1),(.101,'gate',0),(.8,'bell_diameter_ratio',.5),
                       (1.4,'bell_height_ratio',.25),(2,'hammering',1),(2.6,'material',2),
                       (3.2,'gate',1),(3.201,'gate',0)])
        # Mechanism ablations: same model; no samples, added reverb or output distortion.
        soft,_=render('soft-strike',{'velocity':.2,'strike_radius':.92},seconds=3)
        hard,_=render('hard-strike',{'velocity':.85,'strike_radius':.92},seconds=3)
        linear,_=render('linear-hard-strike',{'velocity':.85,'strike_radius':.92,'nonlinearity':0},seconds=3)
        sim=float(abs(np.vdot(hard.ravel(),linear.ravel()))/max(1e-30,np.linalg.norm(hard)*np.linalg.norm(linear)))
        check('nonlinear state coupling changes the render',sim<.995,normalised_correlation=sim)
        linwood,iw=render('linear-wood-control',{'beater':0,'nonlinearity':0},seconds=1,save=False)
        linfelt,iff=render('linear-felt-control',{'beater':2,'nonlinearity':0},seconds=1,save=False)
        check('felt contact excites a lower attack centroid',iff['attack_centroid_hz']<iw['attack_centroid_hz'],wood_hz=iw['attack_centroid_hz'],felt_hz=iff['attack_centroid_hz'])
        near,_=render('position-neighbour',{'strike_radius':.9201},seconds=1,save=False)
        # Strong nonlinear motion can diverge in phase from a tiny perturbation.
        # Check the first impact millisecond, plus the full linear spatial response;
        # do not require chaotic nonlinear tails to remain phase-locked for a second.
        window=slice(int(.1*96000),int(.101*96000))
        a,b=near[window].astype(float).ravel(),base[window].astype(float).ravel()
        corr=float(np.vdot(a,b)/(np.linalg.norm(a)*np.linalg.norm(b)))
        check('nearby positions have continuous initial impacts',corr>.98,normalised_correlation=corr,window_seconds=.001)
        linear_base,_=render('linear-position-base',{'strike_radius':.92,'nonlinearity':0},seconds=1,save=False)
        linear_near,_=render('linear-position-neighbour',{'strike_radius':.9201,'nonlinearity':0},seconds=1,save=False)
        a,b=linear_base.astype(float).ravel(),linear_near.astype(float).ravel()
        corr=float(np.vdot(a,b)/(np.linalg.norm(a)*np.linalg.norm(b)))
        check('linear spatial response is continuous over the full tail',corr>.999,normalised_correlation=corr)
        moved,_=render('move-without-strike',{'strike_radius':.92},seconds=4,save=False,
                       events=[(.1,'gate',1),(.101,'gate',0),(.8,'strike_radius',.16)])
        delta=float(np.max(np.abs(moved-base)))
        check('moving the beater does not recolour an existing tail',delta<1e-6,max_abs_difference=delta)
        repeated,_=render('repeat-exact',{'strike_radius':.92},seconds=1,save=False)
        check('deterministic repeat',np.array_equal(repeated,base[:len(repeated)]))
        one,_=render('sample-block-one',{'strike_radius':.92},seconds=1,block=1,save=False)
        uneven,_=render('sample-block-257',{'strike_radius':.92},seconds=1,block=257,save=False)
        check('block size independent',np.max(np.abs(one-uneven))<1e-6,max_abs_difference=float(np.max(np.abs(one-uneven))))
        for rate in (44100,48000):render(f'rate-{rate}',{'strike_radius':.92},rate=rate,seconds=1,save=False)
        silence,info=render('no-strike',seconds=1,events=[],save=False)
        check('no self excitation from zero state',np.count_nonzero(silence)==0 and info['final_energy']==0)
        mount,_=render('central-support',{'strike_radius':0},seconds=1,save=False)
        check('clamped mounting point is not a bell sample',np.count_nonzero(mount)==0)
        cleared,_=render('clear-state',{'strike_radius':.92},seconds=1.5,save=False,
                         events=[(.1,'gate',1),(.101,'gate',0),(.7,'clear',1),(.701,'clear',0)])
        check('clear empties resonator state',np.max(np.abs(cleared[int(1.2*96000):]))<1e-5)
        render('repeated-strikes',{'strike_radius':.8},seconds=4,
               events=[event for t in (.1,.5,.9,1.3,1.7,2.1) for event in [(t,'gate',1),(t+.001,'gate',0)]])
        stress=[{'diameter_m':.01,'thickness_mm':6,'stiffness_scale':4},
                {'diameter_m':40,'thickness_mm':.15,'density_scale':4},
                {'diameter_m':.01,'material':3,'loss_scale':.1},
                {'diameter_m':40,'proportional_thickness':1,'material':2},
                {'bell_diameter_ratio':.12,'bell_height_ratio':.35,'taper':.9},
                {'bell_diameter_ratio':.55,'bell_height_ratio':0,'bow_height_ratio':0,'taper':0},
                {'nonlinearity':1,'hammering':1,'velocity':1,'beater_mass_g':200,'tip_radius_mm':.2,'beater':4},
                {'material':3,'grain_anisotropy':.9,'grain_angle_deg':180,'loss_scale':12},
                {'choke':1}, {'velocity':0}]
        for i,settings in enumerate(stress):
            _,result=render(f'extreme-{i}',settings,seconds=.6,rate=48000,save=False)
            check(f'extreme-{i} bounded',result['peak']<2 and result['max_energy_sampled']<100,peak=result['peak'])
        bad=subprocess.run([exe,str(out/'invalid.f32'),'--set','nonexistent=1'],text=True,capture_output=True)
        check('unknown controls fail loudly',bad.returncode!=0)
        # Same gain across montage; gaps do not reset or normalise individual clips.
        order=['bell-wood','bow-wood','edge-wood','bell-felt','bow-felt','edge-felt','glass','wood-body','small-16cm','giant-220cm','growing-while-ringing']
        pieces=[]; timeline=[]; cursor=0
        for name in order:
            sr,a=wavfile.read(out/(name+'.wav'))
            timeline.append({'name':name,'start_seconds':cursor,'duration_seconds':len(a)/sr})
            pieces.extend([a,np.zeros((int(.4*sr),2),dtype=a.dtype)]);cursor+=len(a)/sr+.4
        wavfile.write(out/'audition.wav',48000,np.concatenate(pieces))
        (out/'audition-timeline.json').write_text(json.dumps(timeline,indent=2)+'\n')
    except Exception as error:
        check('suite completed',False,error=str(error))
        raise
    finally:
        report={'commit':(out/'commit.txt').read_text().strip() if (out/'commit.txt').exists() else 'local',
                'checks':checks,'passed':all(x['passed'] for x in checks), 'renders':renders,
                'render_method':'Actual compiled Faust, double internal states; 96k auditions downsampled to 48k; no loudness normalisation'}
        (out/'results.json').write_text(json.dumps(report,indent=2)+'\n')
    if not all(x['passed'] for x in checks):raise SystemExit(1)
    print(f'{len(checks)} checks passed; {len(renders)} actual Faust renders.')

if __name__=='__main__':main()
