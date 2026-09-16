#!/usr/bin/env python3
"""TX81Z v10 patch reconstruction, numerical policy and actual-Faust recordings.
Run on the established trusted Mac queue. Reuses the retained lab renderer and
unchanged native ymfm adapter. Python prepares controls/analyzes; it does not
synthesize substitute audio. No hardware-fidelity or realtime-device claim.
"""
from __future__ import annotations
import argparse
import copy
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time
import urllib.request
import wave
import zipfile

import numpy as np
from scipy.signal import resample_poly
from tx81z_patch import decode_sysex, to_controls
from tx81z_prepare import prepare

ROOT = Path(__file__).resolve().parents[2]
RENDERER_SHA = '8a0ff6cf77960d0b5c61fbb9f8831681e6dc05bd1ac8fd20718244f507f94e02'
MATH_BLOB = 'b23df480991d653deea9ab5434ab5eff3cf1da59'
MATH_URL = 'https://raw.githubusercontent.com/iflyhigh/ax81z/5848832989c7864092a277e9428d44f817c6173f/Math.md'
PATCH_NAMES = ('Filter1','Filter2','FilterBass')
NATIVE_RATE = 55930
AUDIO_TOL = 1e-7


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def fetch(url, path, limit=65536):
    request = urllib.request.Request(url,headers={'User-Agent':'Faust-expr-reference-qualification/1'})
    with urllib.request.urlopen(request,timeout=25) as response:
        data = response.read(limit+1)
    if len(data) > limit:
        raise ValueError('reference exceeds bounded download: '+url)
    Path(path).write_bytes(data)
    return data


def rms(x):
    return float(np.sqrt(np.mean(np.asarray(x,dtype=np.float64)**2)))


class Qualification:
    def __init__(self, args):
        self.a = args
        self.out = args.out.resolve()
        self.out.mkdir(parents=True,exist_ok=False)
        for name in ('logs','references','listening','delivery'):
            (self.out/name).mkdir()
        self.report = {'schema':'faust-expr/tx81z-patch/v1','status':'running',
                       'checks':[],'renders':[],'builds':{},'references':{},
                       'native_comparisons':[],'listening':[],
                       'limits':['BC numerical curve is a disclosed DX100-family approximation, not TX81Z hardware calibration',
                                 'native pairs use the same decoded control policy, not independent controller firmware',
                                 'host-rate adaptation retains two host samples of feedback',
                                 'wheel/foot routes, bend, portamento, allocation and analog output stage are outside this adapter']}
        self.commands = []
        self.start = time.monotonic()

    def run(self, command, timeout=180):
        command = list(map(str,command))
        start = time.monotonic()
        result = subprocess.run(command,cwd=ROOT,text=True,capture_output=True,timeout=timeout)
        record = {'command':command,'seconds':time.monotonic()-start,'returncode':result.returncode}
        self.commands.append(record)
        p = self.out/'logs'/f'{len(self.commands):03d}.txt'
        p.write_text(result.stdout+'\n'+result.stderr)
        record['log'] = str(p.relative_to(self.out))
        if result.returncode:
            raise RuntimeError('command failed: '+' '.join(command)+'\n'+result.stderr[-3000:]+result.stdout[-1500:])
        return result.stdout

    def check(self, name, passed, **detail):
        self.report['checks'].append({'name':name,'passed':bool(passed),**detail})
        if not passed:
            raise AssertionError(name+': '+json.dumps(detail))

    def build(self, name, source=None, vector=False, native=False):
        dest = self.out/name
        dest.mkdir()
        start = time.monotonic()
        if native:
            shutil.copy2(ROOT/'tools/modules/ymfm_opz_voice_reference.hpp',dest/'generated.hpp')
        else:
            flags = ['-lang','cpp','-single','-cn','ModuleDSP']
            if vector:
                flags += ['-vec','-lv','0','-vs','32']
            self.run([self.a.faust,*flags,'-I',ROOT/'modules/tx81z/v5',source,'-o',dest/'generated.hpp'])
        shutil.copy2(self.a.renderer,dest/'render.cpp')
        command = ['c++','-std=c++17','-O2','-ffp-contract=off','-I'+str(dest),dest/'render.cpp']
        if native:
            command += ['-I'+str(self.a.ymfm/'src'),self.a.ymfm/'src/ymfm_opz.cpp']
        self.run(command+['-o',dest/'render'])
        (dest/'controls.tsv').write_text(self.run([dest/'render','--controls']))
        self.report['builds'][name] = {'seconds':time.monotonic()-start,'generated_sha256':sha(dest/'generated.hpp'),
                                      'renderer_sha256':sha(dest/'render.cpp'),'kind':'native core' if native else 'compiled Faust',
                                      'source':str(source) if source else 'unchanged ymfm adapter','vector':vector}
        return dest/'render'

    def render(self, name, exe, params, events=(), rate=NATIVE_RATE, seconds=.85, block=128, frames=None):
        if frames is None:
            frames = round(seconds*rate)
        ev = [(0,key,value) for key,value in params.items()]+list(events)
        ev.sort(key=lambda x:x[0])
        if len({(frame,key) for frame,key,value in ev}) != len(ev):
            raise ValueError('duplicate score events in '+name)
        score = self.out/(name+'.tsv'); raw = self.out/(name+'.f32')
        score.write_text(''.join(f'{frame}\t{key}\t{value:.12g}\n' for frame,key,value in ev))
        diagnostic = json.loads(self.run([exe,score,raw,rate,block,frames,0]))
        x = np.fromfile(raw,dtype='<f4')
        channels = diagnostic['channels']
        self.check(name+':finite-size',len(x)==frames*channels and bool(np.isfinite(x).all()))
        self.report['renders'].append({'name':name,'rate':rate,'frames':frames,'channels':channels,'block':block,
                                       'kind':'audio' if channels==1 else 'numerical policy, NOT audio',
                                       'raw':raw.name,'raw_sha256':sha(raw),'score':score.name,'score_sha256':sha(score),
                                       'diagnostics':diagnostic})
        return x if channels==1 else x.reshape(frames,channels)

    @staticmethod
    def gate(rate=NATIVE_RATE):
        return [(round(.05*rate),'gate',1),(round(.65*rate),'gate',0)]

    def load_references(self):
        raw = fetch(MATH_URL,self.out/'references/math.md')
        blob = hashlib.sha1(b'blob '+str(len(raw)).encode()+b'\0'+raw).hexdigest()
        self.check('reference:math-pin',blob==MATH_BLOB,actual_blob=blob)
        text = raw.decode()
        def table(name):
            hit = re.search(r'\b'+name+r'\s*\[\]\s*=\s*\{(.*?)\};',text,re.S)
            if not hit:
                raise ValueError('missing reference table '+name)
            return [int(x,0) for x in re.findall(r'0x[0-9A-Fa-f]+|\d+',hit[1])]
        self.kls = table('c_table_KLS')
        self.vel = table('c_table_MIDI_VELOCITY')
        self.low = table('c_table_BASIC_TL')
        self.check('reference:dimensions',[len(self.kls),len(self.vel),len(self.low)]==[29,128,20])
        self.report['references']['parameter_notes'] = {'url':MATH_URL,'blob':blob,'sha256':sha(self.out/'references/math.md')}
        self.patches = {}
        for name in PATCH_NAMES:
            url = 'https://mgregory22.me/tx81z/patches/'+name+'.syx'
            path = self.out/'references'/(name+'.syx')
            raw = fetch(url,path)
            self.check(name+':published-file-size',len(raw)==142,bytes=len(raw))
            patch = decode_sysex(raw)
            controls,limits = to_controls(patch)
            self.patches[name] = (patch,controls)
            data = {'patch':patch,'controls':controls,'limitations':limits}
            (self.out/'references'/(name+'.json')).write_text(json.dumps(data,indent=2))
            self.report['references'][name] = {'url':url,'sha256':sha(path),'bytes':len(raw),'name':patch['name'],
                                             'source':'Matt Gregory original downloadable tutorial patch',
                                             'storage':'raw downloaded patch retained privately; no patch bank republished in source/delivery',
                                             'limitations':limits}
        # Independently transcribed published programming steps, not the decoder's
        # own offset constants: asymmetric values expose OP2/OP3 swaps.
        p,_ = self.patches['Filter2']
        self.check('Filter2:published-algorithm',p['algorithm']==1)
        self.check('Filter2:published-operator-levels',[p['operators'][i]['out'] for i in range(1,5)]==[90,65,75,82])
        self.check('Filter2:published-half-sines',[p['operators'][i]['wave'] for i in (3,4)]==[2,2])
        p,_ = self.patches['Filter1']
        self.check('Filter1:published-algorithm-and-sweep',p['algorithm']==7 and p['operators'][4]['ar']==5 and p['operators'][4]['d2r']==7)

    def kls_tl(self, ls, note):
        n = max(13,min(108,int(note)))-13
        code = (n//12)*16+n%12+(n%12)//3
        return (((ls*165)>>6)*self.kls[max(0,code-14)//4])>>8

    def kvs_tl(self, kvs, vel):
        return 0 if kvs==0 else ((32*kvs*self.vel[vel])>>8)+15-2*kvs

    @staticmethod
    def bias(ebs,depth,cc2):
        return (127-cc2)*ebs*depth//2540

    @staticmethod
    def trim(algorithm, op):
        return ((0,0,0,0),(0,0,0,0),(0,0,0,0),(0,0,0,0),
                (8,0,8,0),(13,13,13,0),(13,13,13,0),(16,16,16,16))[algorithm][op-1]

    def numerical_policy(self):
        source = self.out/'policy-probe.dsp'
        source.write_text('import("stdfaust.lib");\np=library("'+str(ROOT/'modules/tx81z/v10/panel_policy.lib')+'");\n'
            'ls=nentry("ls",0,0,99,1); note=nentry("note",13,13,108,1); kvs=nentry("kvs",0,0,7,1);\n'
            'vel=nentry("vel",127,0,127,1); ebs=nentry("ebs",0,0,7,1); depth=nentry("depth",0,0,99,1);\n'
            'cc=nentry("cc",127,0,127,1); lev=nentry("lev",99,0,99,1); a=nentry("a",0,0,7,1); op=nentry("op",1,1,4,1);\n'
            'process=float(p.levelScaleTL(ls,note)),float(p.velocityTL(kvs,vel)),float(p.breathBiasTL(ebs,depth,cc)),float(p.basicTL(lev)),float(p.algorithmTrim(a,op));\n')
        exe = self.build('policy',source)
        current = {'ls':0,'note':13,'kvs':0,'vel':127,'ebs':0,'depth':0,'cc':127,'lev':99,'a':0,'op':1}
        states = []
        for ls in range(100):
            for note in range(13,109):
                states.append(current|{'ls':ls,'note':note})
        for kvs in range(8):
            for vel in range(128):
                states.append(current|{'kvs':kvs,'vel':vel})
        for ebs in range(8):
            for depth in (0,50,99):
                for cc in (0,1,32,64,96,127):
                    states.append(current|{'ebs':ebs,'depth':depth,'cc':cc})
        for lev in range(100): states.append(current|{'lev':lev})
        for a in range(8):
            for op in range(1,5): states.append(current|{'a':a,'op':op})
        events, expected, previous = [], [], {}
        for i,s in enumerate(states):
            events += [(i,k,v) for k,v in s.items() if previous.get(k)!=v]
            expected.append([self.kls_tl(s['ls'],s['note']),self.kvs_tl(s['kvs'],s['vel']),
                             self.bias(s['ebs'],s['depth'],s['cc']),self.low[s['lev']] if s['lev']<20 else 99-s['lev'],self.trim(s['a'],s['op'])])
            previous = s
        actual = self.render('policy-grid',exe,{},events,rate=48000,frames=len(states),block=127)
        expected = np.asarray(expected,dtype=np.float32)
        self.check('policy:exact-reference-grid',np.array_equal(actual,expected),cases=len(states),values=int(expected.size),max_abs=float(np.max(abs(actual-expected))))
        self.report['policy_scope'] = {'kls_cases':9600,'kvs_cases':1024,'bc_cases':144,'level_cases':100,'trim_cases':32,
                                       'bc_reference':'XCent 0.12.7 disclosed DX100-family formula, NOT independent TX81Z confirmation'}

    @staticmethod
    def neutral():
        p = {'gate':0,'freq':220,'velocity':1,'level':1,'algorithm':1,'feedback':0,'transpose':0,
             'pModDepth':0,'aModDepth':0,'lfoSpeed':70,'lfoDelay':0,'lfoSync':1,'lfoWave':2,
             'pModSens':3,'aModSens':0,'breath':0,'bcEGBias':0}
        for i in range(1,5):
            fields = {'Wave':0,'Mode':0,'Coarse':1,'Fine':0,'DT1':0,'DT2':0,'Range':0,'FixedCRS':0,
                      'TL':0 if i==1 else 127,'AR':31,'D1R':0,'D2R':0,'SL':0,'RR':15,'KS':0,'Reverb':0,
                      'KVS':0,'LS':0,'EBS':0,'AME':0}
            p.update({f'op{i}'+k:v for k,v in fields.items()})
            if i!=1: p[f'op{i}EGShift']=0
        return p

    def behavior(self):
        p = self.neutral()
        def r(name,changes=None,events=None,**kwargs):
            return self.render(name,self.scalar,p|(changes or {}),self.gate() if events is None else events,**kwargs)
        b = r('neutral')
        self.check('voice:audible',rms(b)>.01)
        self.check('voice:idle-silent',np.max(abs(b[:2000]))==0)
        self.check('voice:release-silent',np.max(abs(b[-3000:]))==0)
        self.check('velocity:zero-is-silent',np.max(abs(r('zero-velocity',{'velocity':0})))==0)
        self.check('velocity:KVS0-independent',np.array_equal(b,r('kvs0-soft',{'velocity':.2})))
        low = r('kvs7-soft',{'velocity':.2,'op1KVS':7})
        high = r('kvs7-hard',{'op1KVS':7})
        self.check('velocity:KVS7-effective',rms(low)<rms(high)*.5,soft_rms=rms(low),hard_rms=rms(high))
        # The earlier Deicsonze-style unity-at-max test was wrong for these
        # published TX81Z integer laws: KVS7 has one residual TL step at127.
        self.check('velocity:full-level-offset-not-erased',np.array_equal(high,r('one-TL-step',{'op1TL':1})))
        c1 = r('note24',{'freq':32.7031956626})
        self.check('scaling:low-note-null',np.array_equal(c1,r('note24-LS99',{'freq':32.7031956626,'op1LS':99})))
        c3 = r('note48',{'freq':130.8127826503})
        c3ls = r('note48-LS99',{'freq':130.8127826503,'op1LS':99})
        self.check('scaling:nonlinear-reference-at48',np.array_equal(c3ls,r('note48-reference-TL',{'freq':130.8127826503,'op1TL':self.kls_tl(99,48)})))
        self.check('scaling:not-old-C3-flat-curve',rms(c3ls)<rms(c3)*.8)
        self.check('bias:EBS0-null',np.array_equal(b,r('bias-EBS0',{'bcEGBias':99})))
        self.check('bias:depth0-null',np.array_equal(b,r('bias-depth0',{'op1EBS':7})))
        self.check('bias:full-breath-null',np.array_equal(b,r('bias-full-breath',{'op1EBS':7,'bcEGBias':99,'breath':1})))
        bb = r('bias-no-breath',{'op1EBS':7,'bcEGBias':99})
        self.check('bias:correct-polarity-and-depth',rms(bb)<rms(b)*.2,no_breath_rms=rms(bb),full_breath_rms=rms(b))
        delayed = self.gate()+[(18000,'breath',1),(30000,'breath',.25)]
        live = r('bias-live',{'op1EBS':7,'bcEGBias':99},sorted(delayed))
        self.check('bias:live-response',rms(live[20000:29000])>rms(live[8000:15000])*4)
        release_events = [(1000,'gate',1),(12000,'gate',0),(18000,'breath',1)]
        tail = r('bias-release',{'op1EBS':7,'bcEGBias':99,'op1RR':1},release_events)
        control = r('bias-release-control',{'op1EBS':7,'bcEGBias':99,'op1RR':1},release_events[:2])
        self.check('bias:release-remains-controllable',np.max(abs(tail[18000:]-control[18000:]))>1e-4)
        pm = r('pitch-modulation',{'pModDepth':80,'pModSens':6})
        self.check('LFO:PM-retained',np.max(abs(pm-b))>.01)
        self.check('LFO:AME-off-null',np.array_equal(b,r('AM-disabled',{'aModDepth':90,'aModSens':3})))
        am = r('AM-enabled',{'aModDepth':90,'aModSens':3,'op1AME':1})
        self.check('LFO:AM-retained',rms(am)<rms(b))
        self.check('transpose:one-octave',np.array_equal(r('transposed',{'transpose':12}),r('pitched',{'freq':440})))
        fixed = {'op1Mode':1,'op1FixedCRS':32,'op1Range':3}
        self.check('fixed:keyboard-independent',np.array_equal(r('fixed-low',fixed|{'freq':110}),r('fixed-high',fixed|{'freq':880})))
        self.check('repeat:cold-identical',np.array_equal(b,r('repeat')))
        for block in (1,127,511):
            self.check('block:'+str(block),np.array_equal(b,r('block-'+str(block),block=block)))
        for rate in (44100,48000,96000):
            x = self.render('host-rate-'+str(rate),self.scalar,p,self.gate(rate),rate=rate)
            self.check('host-rate:'+str(rate)+':audible',rms(x)>.01)
        xv = self.render('vector-neutral',self.vector,p,self.gate())
        self.check('vector:neutral-equivalence',np.max(abs(b-xv))<=AUDIO_TOL,max_abs=float(np.max(abs(b-xv))))

    def native_pairs(self):
        suffixes = ('Wave','Mode','Coarse','Fine','DT1','DT2','Range','TL','AR','D1R','D2R','SL','RR','KS','Reverb')
        allowed = {f'op{i}'+s for i in range(1,5) for s in suffixes}|{'algorithm','feedback'}
        for name,(patch,c) in self.patches.items():
            p = self.neutral()|c
            # Explicitly control-isolated comparison. Native adapter does not
            # implement firmware LFO/BC/EG-shift/fixed-Hz semantics.
            p.update({'pModDepth':0,'aModDepth':0,'bcEGBias':0,'velocity':.8,'freq':220})
            for i in range(1,5):
                p[f'op{i}Mode']=0
                if i!=1: p[f'op{i}EGShift']=0
            note = 57+p['transpose']; vel = round(127*.8)
            npolicy = {k:v for k,v in p.items() if k in allowed}
            npolicy.update({'gate':0,'freq':220*2**(p['transpose']/12),'velocity':1,'level':1})
            for i in range(1,5):
                q=f'op{i}'
                npolicy[q+'TL']=min(127,p[q+'TL']+self.kvs_tl(p[q+'KVS'],vel)+self.kls_tl(p[q+'LS'],note)+self.trim(p['algorithm']-1,i))
            ev = [(1000,'gate',1),(60000,'gate',0)]
            x = self.render(name+'-core-Faust',self.scalar,p,ev,seconds=1.5)
            y = self.render(name+'-core-native',self.native,npolicy,ev,seconds=1.5)
            maximum = float(np.max(abs(x-y)))
            self.check(name+':native-core-regression',maximum<=AUDIO_TOL,max_abs=maximum,tolerance=AUDIO_TOL)
            self.report['native_comparisons'].append({'patch':name,'max_abs':maximum,'tolerance':AUDIO_TOL,
                'scope':'same decoded controls, translated TL policy; LFO/BC/EG-shift/fixed mode isolated; not firmware/hardware fidelity'})

    def listen(self, name, x, rate, description):
        y = resample_poly(x.astype(np.float64),4800,5593)*.8 if rate==55930 else x.astype(np.float64)*.8
        self.check(name+':no-playback-clipping',np.max(abs(y))<1,peak=float(np.max(abs(y))))
        path = self.out/'listening'/(name+'.wav')
        pcm = np.round(y*32767).astype('<i2')
        with wave.open(str(path),'wb') as w:
            w.setnchannels(1); w.setsampwidth(2); w.setframerate(48000); w.writeframes(pcm.tobytes())
        self.report['listening'].append({'file':path.name,'sha256':sha(path),'seconds':len(y)/48000,
            'description':description,'source_rate':rate,'output_rate':48000,'fixed_gain':.8,'effects':'none','normalization':'none'})

    def music(self):
        self.music_data = {}
        for name,(patch,c) in self.patches.items():
            p = self.neutral()|c
            if name=='FilterBass':
                ev=[]
                for i,(note,velocity) in enumerate(zip((36,36,43,36,48,46,43,38,36,43),(.95,.55,.85,.7,1,.65,.9,.55,1,.8))):
                    start=round((.1+i*.5)*NATIVE_RATE)
                    ev += [(start,'freq',440*2**((note-69)/12)),(start,'velocity',velocity),(start,'gate',1),(start+round(.4*NATIVE_RATE),'gate',0)]
                seconds=6.1
            else:
                note=36 if name=='Filter1' else 43
                ev=[(5593,'freq',440*2**((note-69)/12)),(5593,'velocity',.9),(5593,'gate',1),(round(6.1*NATIVE_RATE),'gate',0)]
                seconds=7.4
            x = self.render(name+'-music',self.scalar,p,ev,seconds=seconds)
            self.check(name+':music-audible',rms(x)>.0001,rms=rms(x))
            self.listen(name,x,NATIVE_RATE,'Actual Faust playback of Matt Gregory '+name+' SysEx; dry, no physical TX81Z recording')
            self.music_data[name]=(p,ev,seconds,x)
        p,ev,seconds,x = self.music_data['FilterBass']
        y = self.render('FilterBass-vector',self.vector,p,ev,seconds=seconds)
        self.check('vector:musical-equivalence',np.max(abs(x-y))<=AUDIO_TOL,max_abs=float(np.max(abs(x-y))))
        # A deliberately labelled variant, not the original author's patch.
        p=copy.deepcopy(p); p.update({'bcEGBias':99,'op1EBS':7,'breath':0})
        ev=[(5593,'freq',110),(5593,'gate',1)]
        for i,amount in enumerate((0,.2,.4,.6,.8,1,.5,0)):
            ev.append((round((.2+i*.5)*NATIVE_RATE),'breath',amount))
        ev.append((round(4.3*NATIVE_RATE),'gate',0))
        x=self.render('FilterBass-breath-variant',self.scalar,p,ev,seconds=5.2)
        self.listen('FilterBass-breath-variant',x,NATIVE_RATE,'Declared FilterBass variant: carrier EBS7/BC depth99, stepped breath; provisional family controller curve')

    def package(self):
        source = ROOT/'modules/tx81z/v10/voice.dsp'
        top = ROOT/'modules/tx81z'
        pattern = re.compile(r'^(\w+)\s*=\s*library\("([^"\n]+)"\);\s*$',re.M)
        closure = {}
        def expand(path,stack=()):
            path=path.resolve()
            if path in stack or not path.is_relative_to(top):
                raise ValueError('invalid dependency closure')
            text=path.read_text()
            closure[str(path.relative_to(ROOT))]=sha(path)
            if path!=source:
                text=re.sub(r'^import\("stdfaust.lib"\);\s*$','',text,flags=re.M)
                text=re.sub(r'^declare\s+[^\n]+\n','',text,flags=re.M)
            def replace(match):
                dependency=(path.parent/match[2]).resolve()
                return match[1]+'=environment {\n'+expand(dependency,stack+(path,))+'\n};'
            return pattern.sub(replace,text)
        result=expand(source)
        if 'library(' in result:
            raise ValueError('unresolved library in standalone export')
        licence=(top/'YMFM-LICENSE.txt').read_text()
        path=self.out/'delivery/TX81Z_v10.dsp'
        path.write_text('// Self-contained TX81Z v10 source; standard Faust library required.\n'+'\n'.join('// '+line for line in licence.splitlines())+'\n'+result)
        exe=self.build('standalone',path)
        p,ev,seconds,x=self.music_data['FilterBass']
        y=self.render('FilterBass-standalone',exe,p,ev,seconds=seconds)
        self.check('export:musical-equivalence',np.max(abs(x-y))<=AUDIO_TOL,max_abs=float(np.max(abs(x-y))))
        self.report['source_closure']=closure
        self.report['standalone_sha256']=sha(path)
        for pth in ('modules/tx81z/v10/REFERENCE.md','tools/modules/tx81z_patch.py',
                    'tools/modules/tx81z_patch_qualification.py','tests/test_tx81z_patch.py'):
            path_src=ROOT/pth
            if path_src.exists():
                target=self.out/'delivery'/pth
                target.parent.mkdir(parents=True,exist_ok=True)
                shutil.copy2(path_src,target)
        (self.out/'delivery/SOURCE.json').write_text(json.dumps({'commit':self.report['commit'],'closure':closure},indent=2))

    def execute(self):
        self.check('execution:physical-M1',sys.platform=='darwin' and os.getenv('RUNNER_NAME')=='Felix-M1-Pro-CURLOP')
        self.report['commit']=self.run(['git','rev-parse','HEAD']).strip()
        self.run(['git','diff','--exit-code','HEAD','--'])
        self.check('renderer:unchanged',sha(self.a.renderer)==RENDERER_SHA)
        self.report['compiler']={'faust':self.run([self.a.faust,'--version']),
                                'faust_sha256':sha(self.a.faust),'cxx':self.run(['c++','--version'])}
        print('Reconciling actual patch bytes and pinned parameter facts',flush=True)
        self.load_references()
        self.report['tables']=prepare(self.a.ymfm,ROOT/'modules/tx81z/v5/opz_tables.lib')
        self.numerical_policy()
        print('Compiling voice, native regression adapter and vector variant',flush=True)
        source=ROOT/'modules/tx81z/v10/voice.dsp'
        self.scalar=self.build('scalar',source)
        self.native=self.build('native',native=True)
        self.vector=self.build('vector',source,vector=True)
        self.behavior()
        self.native_pairs()
        print('Rendering complete dry examples from the downloaded SysEx files',flush=True)
        self.music()
        self.package()
        self.report['status']='completed'

    def save(self):
        self.report['elapsed_seconds']=time.monotonic()-self.start
        self.report['commands']=self.commands
        self.report['check_count']=len(self.report['checks'])
        self.report['render_count']=len(self.report['renders'])
        (self.out/'results.json').write_text(json.dumps(self.report,indent=2)+'\n')
        if self.report['status']=='completed':
            shutil.copy2(self.out/'results.json',self.out/'delivery/results.json')
            for path in (self.out/'listening').glob('*.wav'):
                shutil.copy2(path,self.out/'delivery'/path.name)
            # This archive contains project source, our recordings and provenance;
            # NOT private runner scripts, queue logs, downloaded patches or firmware.
            with zipfile.ZipFile(self.out/'TX81Z_v10_delivery.zip','w',zipfile.ZIP_DEFLATED) as z:
                for path in sorted((self.out/'delivery').rglob('*')):
                    if path.is_file(): z.write(path,path.relative_to(self.out/'delivery'))
        manifest={str(p.relative_to(self.out)):sha(p) for p in sorted(self.out.rglob('*')) if p.is_file() and p.name!='SHA256SUMS.json'}
        (self.out/'SHA256SUMS.json').write_text(json.dumps(manifest,indent=2)+'\n')


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    for key in ('out','renderer','ymfm','faust'):
        ap.add_argument('--'+key,type=Path,required=True)
    args=ap.parse_args()
    for key in ('renderer','ymfm','faust'):
        setattr(args,key,getattr(args,key).resolve())
    q=Qualification(args)
    try:
        q.execute()
    except Exception as error:
        q.report['status']='failed'; q.report['error']=str(error)
        raise
    finally:
        q.save()
    print(json.dumps({'status':q.report['status'],'checks':q.report['check_count'],
                      'renders':q.report['render_count'],'listening':len(q.report['listening'])}))

if __name__=='__main__': main()
