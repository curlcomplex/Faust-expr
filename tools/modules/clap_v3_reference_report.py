"""Short public-demo comparison for criticism/review, never runtime sampling.
Pin source hash and timestamps. No nearest-neighbour reference selection or
parameter optimizer. First 200ms measurements; 300ms excerpt auditions.
"""
import argparse,hashlib,json,subprocess
from pathlib import Path
import numpy as np
from scipy.io import wavfile
from scipy.signal import welch
from scipy.spatial.distance import jensenshannon
from clap_v3_delivery import features
HASH='37e0d94eccf9475bcf60f4e6eeced339cf24081f1acfa1713544b38258a9884f'
TIMES={'Vermona_A':35.04077083333333,'Vermona_B':95.04491666666667}
URL='https://www.vermona.com/fileadmin/user_upload/products/drm1mk4/demos/106_drm1mk4_clap.mp3'
def digest(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def measure(x):
    x=np.asarray(x[:9600],float)
    f,p=welch(np.pad(x,(512,512)),48000,nperseg=1024,noverlap=768);p/=p.sum()
    edges=[0,300,600,1000,2000,4000,6000,12000,24001]
    bands=np.array([p[(f>=a)&(f<b)].sum() for a,b in zip(edges,edges[1:])])
    c=np.cumsum(x*x);c/=c[-1]
    return features(x),bands,c

def run(refs,render,out):
    out.mkdir(parents=True,exist_ok=True)
    mp3=refs/'106_drm1mk4_clap.mp3'
    if digest(mp3)!=HASH:raise ValueError('Reference changed: review timestamps before reuse')
    decoded=out/'_decoded.wav'
    subprocess.run(['ffmpeg','-v','error','-y','-i',str(mp3),'-ac','1','-ar','48000','-c:a','pcm_f32le',str(decoded)],check=True)
    sr,whole=wavfile.read(decoded);decoded.unlink()
    data={k:whole[round(t*sr):round(t*sr)+14400].astype(float) for k,t in TIMES.items()}
    for k,n in [('Old_v2','rejected_v2_default'),('New_v3','default')]:
        data[k]=np.fromfile(render/(n+'.f32'),'<f4')[101:101+14400].astype(float)
    m={k:measure(x) for k,x in data.items()}
    report={'reference':{'manufacturer':'VERMONA','instrument':'DRM1 MKIV clap channel','url':URL,'sha256':HASH,'timestamps_seconds':TIMES,'settings':'Unknown manufacturer demonstration, not controlled settings. Two reviewed clustered-clap examples, not all sounds in the sweeping demo.','rights':'Short excerpts for A/B critique only. No source samples in the instrument.'},'measurement':'First 200 ms, onset aligned; Welch with half-window boundary padding, 1024-sample Hann and 256-sample hop. Frequency bands 0/300/600/1000/2000/4000/6000/12000/24000 Hz. CDF distance is area between normalized energy CDFs. These are descriptions, not a semantic classifier or listening score.','features':{k:v[0] for k,v in m.items()},'distances':{}}
    for key in TIMES:
        report['distances'][key]={}
        for synth in ['Old_v2','New_v3']:
            report['distances'][key][synth]={'band_JS_distance':float(jensenshannon(m[key][1],m[synth][1])),'energy_timing_CDF_distance_ms':float(np.mean(abs(m[key][2]-m[synth][2]))*200)}
    gains={k:.1/np.sqrt(np.mean(x[:9600]**2)) for k,x in data.items()}
    headroom=min(1.,.85/max(np.max(abs(data[k]*gains[k])) for k in data))
    clips={}
    for k,x in data.items():
        y=x*gains[k]*headroom;y[-240:]*=np.linspace(1,0,240);clips[k]=y
    order=['Old_v2','Vermona_B','New_v3','Vermona_A','New_v3']
    timeline=np.zeros(48000*10);cues=[]
    for i,k in enumerate(order):
        for offset in (.1, .85):
            start=round((i*2+offset)*48000);timeline[start:start+14400]+=clips[k]
        cues.append({'start_seconds':i*2,'label':k,'hits_at_seconds':[i*2+.1,i*2+.85]})
    wavfile.write(out/'01_old_reference_new.wav',48000,(timeline*32767).round().astype('<i2'))
    sr,bank=wavfile.read(render/'clap_v3_bank_raw.wav');bank_gain=.85/max(abs(bank))
    wavfile.write(out/'02_eight_claps.wav',sr,(bank.astype(float)*bank_gain*32767).round().astype('<i2'))
    for path in sorted(render.glob('0[1-8]_*.wav')):
        sr,x=wavfile.read(path);wavfile.write(out/path.name,sr,(x.astype(float)*bank_gain*32767).round().astype('<i2'))
    report['audition']={'gains_before_common_headroom':gains,'common_headroom_gain':headroom,'bank_uniform_gain':float(bank_gain),'cues':cues,'processing':'RMS match first200ms, one common headroom factor, 5ms end fades on 300ms comparison excerpts. Bank has only a single global gain, preserving velocity and relative preset levels. No EQ, compression, limiting or reverb. PCM16/48kHz; raw floats retained in render evidence.'}
    report['output_sha256']={p.name:digest(p) for p in sorted(out.glob('*.wav'))}
    (out/'reference_report.json').write_text(json.dumps(report,indent=2)+'\n')
    (out/'LISTEN.md').write_text('# Clap v3 auditions\n\n01_old_reference_new.wav: 0s rejected v2; 2s VERMONA B; 4s new v3; 6s VERMONA A; 8s new v3. Two hits per block.\n\n02_eight_claps.wav: Classic, Tight, Wide, Dark, Bright, Room/long noise tail, Driven, Dry. One pair every two seconds; second hit velocity 0.65.\n\nReferences are short excerpts of the VERMONA DRM1 MKIV manufacturer demonstration, not a sample pack or endorsement. Source/offsets/gains/hashes and measurement limits are in reference_report.json. The new instrument is entirely synthesized and does not read these files.\n')
    print(json.dumps(report['distances']))
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--references',type=Path,required=True);p.add_argument('--render',type=Path,required=True);p.add_argument('--out',type=Path,required=True);a=p.parse_args();run(a.references,a.render,a.out)
