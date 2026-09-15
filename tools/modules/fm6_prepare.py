"""Prepare the versioned FM6 Faust source from verified, pinned dependencies.

Generated modules are included in the delivery artifact. No network requests,
no upstream edits, no installed-library mutations. Run before qualification.
"""
from pathlib import Path
import re,json,hashlib,shutil
import argparse
import fm6_programs
EXPECTED_LIBRARIES={'operator.lib': '8433045f93ccb9912081fe702306aff11d0d0ba309921b51800bb8797c3e03d4', 'env.lib': '93acbafb35befe9bdaa88fa6b6b1e69ad0c7e381306898ddd571d4f809eaf68f', 'lfo.lib': 'ab2fa104d5f564735313f308816731ba0ae46ca8ca800a56c5c6f8d2241a77e6', 'pitchenv.lib': '875bd6e81e6f1ae1fb94d3dc5e53f76f4624503d8eb96fd8511ce8592466e496'}
EXPECTED_ROUTER="5039002970c22183944aa9719b3342231ceee441244b8a7dfdf039d2fafaa07d"

def prepare(libraries,msfa,out,license_file):
    libraries=Path(libraries).resolve();msfa=Path(msfa).resolve();out=Path(out).resolve()
    for name,digest in EXPECTED_LIBRARIES.items():
        if hashlib.sha256((libraries/'dx7'/name).read_bytes()).hexdigest()!=digest:
            raise ValueError('Unverified Faust dependency: '+name)
    if hashlib.sha256((msfa/'app/src/main/jni/fm_core.cc').read_bytes()).hexdigest()!=EXPECTED_ROUTER:
        raise ValueError('Unverified MSFA routing source')
    mod=out/'v1';mod.mkdir(parents=True,exist_ok=True)
    vendor=mod/'engine';vendor.mkdir(exist_ok=True)
    up=libraries/'dx7'
    for f in ('operator.lib','env.lib','lfo.lib','pitchenv.lib'):
        text=(up/f).read_text()
        header='// Derived from Faust 2.88.0 dx7/'+f+'. See ../PROVENANCE.json.\n// Original attributed functions: David Braun, Apache-2.0.\n'
        if f=='env.lib':
            text=text.replace('((keyDown : keyUp : accurateEnvelope : cond1)', '((coldInit : keyDown : keyUp : accurateEnvelope : cond1)')
            target='  // init_sr\n'
            insert='''  // A newly constructed envelope is idle, not stage 0 with zero increments.
      // The first real gate edge starts the original envelope state transition.
      started = (gate > 0) : max ~ _;
      coldInit(a,b,c,d,e,f,g) = (0,4,0,0,0,0,0,a,b,c,d,e,f,g)
          : ba.selectbus(7,2,started);
    
    '''
            text=text.replace(target,insert+target)
            text=text.replace('staticcount_c = int(staticcount_b * sr_multiplier);','staticcount_c = int(staticcount_b / sr_multiplier);')
            # Avoid out-of-table indices when inactive branches evaluate their expressions.
            text=text.replace('raw_exp = exp_scale_data(group);','raw_exp = exp_scale_data(min(32,max(0,int(group))));')
            text=text.replace('_ScaleCurve((offset+1) / 3,','_ScaleCurve(int((offset+1) / 3),').replace('_ScaleCurve(-1*(offset-1) / 3,','_ScaleCurve(int(-1*(offset-1) / 3),')
        if f=='operator.lib':
            text=text.replace('velocity = gain*127.0;', 'velocity = (gain*127.0) : ba.sAndH(gate > gate\');')
            text=text.replace('output = q24_to_linear(outlevel) * sineWave(freq) * .5;', '''// Explicit fresh-voice lifecycle: keep an unstarted voice silent, allow release
      // after note-off, and latch note velocity. Never multiply a release tail by gate.
      started = (gate > 0) : max ~ _;
      audibleVelocity = (gain > 0) : ba.sAndH(gate > gate');
      // Native FmCore treats gains below 1120 Q24 as inactive.
      amplitude = q24_to_linear(outlevel);
      output = ba.if(amplitude >= 1120.0/16777216.0, amplitude, 0)
          * sineWave(freq) * .5 * started * audibleVelocity;''')
        (vendor/f).write_text(header+text)
    # Decode the published MSFA routing flags into acyclic edges + delayed feedback.
    text=(msfa/'app/src/main/jni/fm_core.cc').read_text()
    block=text.split('const FmAlgorithm algorithms[32] = {',1)[1].split('\n};',1)[0]
    algs=[[int(x,16) for x in re.findall('0x[0-9a-f]+',line)] for line in block.splitlines() if '0x' in line]
    assert len(algs)==32 and all(len(a)==6 for a in algs)
    edges=[];carriers=[];fbins=[];fbouts=[]
    for flags in algs:
        buses=[set(),set(),set()]; incoming=[set() for _ in range(6)];fi=[];fo=[]
        for idx,f in enumerate(flags):
            incoming[idx]=buses[(f>>4)&3].copy() if ((f>>4)&3) else set()
            outbus=f&3
            if not f&4:buses[outbus]=set()
            buses[outbus].add(idx)
            if f&0x40:fi.append(idx)
            if f&0x80:fo.append(idx)
        edges.append(incoming);carriers.append(buses[0]);fbins.append(fi);fbouts.append(fo)
    controls={}
    def ctl(name,default,lo,hi,step=1,unit='DX7 parameter',timing='live'):
        controls[name]={'default':default,'min':lo,'max':hi,'step':step,'unit':unit,'timing':timing}
        return f'hslider("{name}",{default},{lo},{hi},{step})'
    lines=['// FM6 Classic v1: six runtime operators, 32 routings, one delayed feedback loop.',
           '// Engine lineage and changes are recorded in PROVENANCE.json. Not hardware certified.',
           'ba=library("basics.lib"); ma=library("maths.lib"); si=library("signals.lib");',
           'dx=library("engine/operator.lib");',
           'gate=button("gate");']
    controls['gate']={'default':0,'min':0,'max':1,'step':1,'unit':'boolean gate','timing':'event edge'}
    lines += ['freq='+ctl('freq',220,8,20000,.001,'Hz')+';', 'velocity='+ctl('velocity',1,0,1,.001,'normalized','latched note-on')+';']
    lines += ['algorithm='+ctl('algorithm',5,1,32)+' : int;',
              'feedback='+ctl('feedback',0,0,7)+' : int;',
              'brightness='+ctl('brightness',1,0,2,.001,'phase depth multiplier')+' : si.smooth(ba.tau2pole(.002));',
              'level='+ctl('level',.2,0,1,.001,'linear output gain')+' : si.smooth(ba.tau2pole(.002));',
              'transpose='+ctl('transpose',0,-24,24,1,'semitones')+';',
              'osc_sync='+ctl('osc_sync',1,0,1)+';']
    for name,default,hi in [('lfo_wave',0,5),('lfo_speed',35,99),('lfo_delay',0,99),('lfo_pmd',0,99),('lfo_amd',0,99),('lfo_sync',1,1),('lfo_pms',0,7)]:
        lines.append(name+'='+ctl(name,default,0,hi)+';')
    for i in range(1,5):
        for key,default in [('rate',99),('level',50)]:
            name=f'pitch_{key}{i}';lines.append(name+'='+ctl(name,default,0,99)+';')
    params=[('mode',0,0,1),('coarse',1,0,31),('fine',0,0,99),('detune',0,-7,7),('level',0,0,99)]
    params += [(f'rate{i}',99,0,99) for i in range(1,5)]+[(f'env{i}',99 if i<4 else 0,0,99) for i in range(1,5)]
    params += [('velocity',0,0,7),('ampmod',0,0,3),('rate_scale',0,0,7),('breakpoint',0,0,99),('left_depth',0,0,99),('right_depth',0,0,99),('left_curve',0,0,3),('right_curve',0,0,3)]
    for op in range(1,7):
        for key,default,lo,hi in params:
            if key=='level':default=80 if op==1 else 0
            name=f'op{op}_{key}';lines.append(name+'='+ctl(name,default,lo,hi)+';')
        args=[f'op{op}_{p[0]}' for p in params]+['lfo_wave','lfo_speed','lfo_delay','lfo_pmd','lfo_amd','lfo_sync','lfo_pms','osc_sync']+[f'pitch_rate{i}' for i in range(1,5)]+[f'pitch_level{i}' for i in range(1,5)]+['transpose','phase','freq','velocity','gate']
        assert len(args)==42
        lines.append(f'op{op}(phase)=dx.operator('+','.join(args)+');')
    def table(vals):return '(algorithm-1) : rdtable(waveform{'+','.join(map(str,vals))+'})'
    for i in range(6):
        for j in range(i):
            lines.append(f'e{i}_{j}='+table([int(j in edges[a][i]) for a in range(32)])+';')
        for kind,arr in [('c',carriers),('fi',fbins),('fo',fbouts)]:
            lines.append(f'{kind}{i}='+table([int(i in arr[a]) for a in range(32)])+';')
    lines += ['// MSFA scalar feedback: (y[n-1]+y[n-2]) / 2^(9-feedback).',
              '// Operator half-scale units are converted exactly once at every phase input.',
              'feedback_gain=(feedback>0)*pow(2.0,float(feedback)-9.0);',
              'on=gate>gate\';',
              'network(prev1,prev2)=feedback_out,ba.if(on,0,prev1),audio with {',
              ' fb=(prev1+prev2)*feedback_gain*(1-on);']
    for i in range(6):
        expr='+'.join(f'y{j}*e{i}_{j}' for j in range(i)) or '0'
        lines.append(f' y{i}=op{6-i}(2*((({expr})*brightness)+fb*fi{i}));')
    lines+=[' feedback_out='+ '+'.join(f'y{i}*fo{i}' for i in range(6))+';',
            ' audio='+ '+'.join(f'y{i}*c{i}' for i in range(6))+';', '};',
            'voice = (network ~ si.bus(2)) : !,!,_ : *(level);']
    (mod/'voice.lib').write_text('\n'.join(lines)+'\n')
    (mod/'voice.dsp').write_text('declare name "FM6 Classic";\ndeclare version "1.0.0-candidate";\nfm=library("voice.lib");\nprocess=fm.voice;\n')
    manifest={'schema':1,'id':'fm6-classic','version':'1.0.0-candidate','sound_version':1,'role':'instrument','inputs':0,'outputs':1,'source':'v1/voice.dsp','controls':controls,'status':'candidate; awaiting qualification and listening','polyphony':'host-owned, one six-operator voice per instance','latency_frames':0,'notes':'Runtime 32 algorithms, exactly six operators. Algorithm changes are live and may click; normally select between notes. Brightness and output gain are smoothed and remain live during release. The root instrument output is the preserved Faust half-scale convention times level.'}
    (out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    prov={'source_release':'Faust 2.88.0','release_archive_sha256':'e4e175cf236924b5b7d4784cbb8c50cc01e211159e169655dd9e6d8f92b871d9','original_files':{f:hashlib.sha256((up/f).read_bytes()).hexdigest() for f in ('operator.lib','env.lib','lfo.lib','pitchenv.lib')},'msfa_commit':'f67d41d313b7dc85f6fb99e79e515cc9d208cfff','routing_source':'app/src/main/jni/fm_core.cc','license':'Attributed engine functions Apache-2.0; see retained original attribution and source license text. General Faust libraries retain their LGPL with static linking exception.','changes':['Explicit x2 conversion at operator-to-phase boundaries, not final gain.','Six-operator dynamic routing instead of 32 complete runtime voices.','Two-sample feedback history including reset on a fresh note.','Idle envelope state until first gate; silence unstarted/zero-velocity voices without cutting release.','Latch velocity at note-on.','Use the native 1120-Q24 inactive-operator threshold for silent zero-level/released operators.','Correct static-envelope sample count scaling to SR/44100.','Clamp keyboard exponential table indices and truncate groups as C++ does.','Continuous smoothed brightness/output controls for release-tail expression.'],'limits':['Not bit-exact or hardware-certified.','Original MSFA ignores multi-operator feedback in algorithms4/6; do not use it to certify those feedback loops.','Stock Faust LFO/AM and ratio-detune approximations are retained, not falsely described as calibrated.']}
    (mod/'PROVENANCE.json').write_text(json.dumps(prov,indent=2)+'\n')
    (mod/'ROUTING.json').write_text(json.dumps({'storage':'operator6 down to1','flags':algs},indent=2)+'\n')
    shutil.copyfile(license_file,mod/'LICENSE-APACHE-2.0.txt')
    fm6_programs.write_presets(out/'presets.json')
    print('Created',len(controls),'controls and six original programs')

if __name__=='__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('--libraries',type=Path,required=True)
    parser.add_argument('--msfa',type=Path,required=True)
    parser.add_argument('--out',type=Path,default=Path(__file__).resolve().parents[2]/'modules/fm6-classic')
    parser.add_argument('--license-file',type=Path,default=Path('/usr/share/common-licenses/Apache-2.0'))
    a=parser.parse_args();prepare(a.libraries,a.msfa,a.out,a.license_file)
