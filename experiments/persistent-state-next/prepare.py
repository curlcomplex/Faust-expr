#!/usr/bin/env python3
"""Hash-pinned adapter over the first passing experiment. Original source stays intact.
Remove diagnostic input outputs from all compiled timing arms, retain real product
captures, and expose a native live-build mode. No DSP state fields are rewritten.
"""
from pathlib import Path
import hashlib,json
HERE=Path(__file__).resolve().parent;OLD=HERE.parent/'persistent-state'
PINS={'main.cpp':'069ce74dbcb48da3849625ada3a8bc8fe245f58a64345a4e6a16e7d1ad5ea9c8','generate.py':'a55ceb3f1fbed4e116a3356b576ded287e75a20791b540ccfa9517787d3121bf','analyze.py':'0a23130f7fd26f606bd991bb3f5f4a1d21c39bde1535bb7fd80b507a13dcc99a'}
def replace(s,a,b):
    if s.count(a)!=1:raise ValueError('adapter anchor mismatch: '+a[:100])
    return s.replace(a,b)
def main():
    for name,want in PINS.items():
        if hashlib.sha256((OLD/name).read_bytes()).hexdigest()!=want:raise ValueError('source pin mismatch '+name)
    s=(OLD/'generate.py').read_text()
    s=replace(s,"outputs+=['(v%d : _,!)'%m['index'],'(v%d : !,_)'%m['index'], '(in%d : _,!)'%m['index'],'(in%d : !,_)'%m['index']]","outputs+=['(v%d : _,!)'%m['index'],'(v%d : !,_)'%m['index']]")
    s=replace(s,'j=slots[i]*4','j=slots[i]*2')
    s=replace(s,"code+='planes[%d][s]=v%d[0];planes[%d][s]=v%d[1];planes[%d][s]=x%d[0];planes[%d][s]=x%d[1];\\n'%(j,i,j+1,i,j+2,i,j+3,i)","code+='planes[%d][s]=v%d[0];planes[%d][s]=v%d[1];\\n'%(j,i,j+1,i)")
    (HERE/'generate.py').write_text(s)
    s=(OLD/'main.cpp').read_text()
    s=replace(s,'std::vector<float*> pointers;','std::vector<float*> pointers,outputPointers;')
    s=replace(s,'for(int c=0;c<channels;++c)pointers[c]=data[c].data();','for(int c=0;c<channels;++c){pointers[c]=data[c].data();if(c%4<2)outputPointers.push_back(pointers[c]);}')
    s=replace(s,'std::vector<float> a(std::size_t(n)*data.size());for(int i=0;i<n;++i)for(std::size_t c=0;c<data.size();++c)a[std::size_t(i)*data.size()+c]=data[c][i];return a;','std::vector<float> a(std::size_t(n)*outputPointers.size());for(int i=0;i<n;++i)for(std::size_t c=0;c<outputPointers.size();++c)a[std::size_t(i)*outputPointers.size()+c]=outputPointers[c][i];return a;')
    s=replace(s,'for(auto& ch:a.data){double sum=0;float peak=0;for(int s=0;s<frames;++s){float x=ch[s];','for(auto* ch:a.outputPointers){double sum=0;float peak=0;for(int s=0;s<frames;++s){float x=ch[s];')
    s=replace(s,'d->compute(n,nullptr,a.pointers.data());','d->compute(n,nullptr,a.outputPointers.data());')
    s=replace(s,'require(llvm.channels()==channels,"whole plane count");','require(llvm.channels()==channels/2,"whole output-tap plane count");')
    for a,b in [('frames,a.pointers.data())','frames,a.outputPointers.data())'),('frames,c.pointers.data())','frames,c.outputPointers.data())'),('count,audio.pointers.data())','count,audio.outputPointers.data())'),('count,negative.pointers.data())','count,negative.outputPointers.data())')]:
        if a not in s:raise ValueError('missing pointer adapter '+a)
        s=s.replace(a,b)
    if s.count('prop(result,"channels",channels);')!=2:raise ValueError('channel count anchor')
    s=s.replace('prop(result,"channels",channels);','prop(result,"channels",channels/2);')
    helper='''static std::vector<float> productTaps(Renderer& r,const GraphState& g,int frames){
      std::vector<std::size_t> taps;for(auto& m:g.modules()){std::size_t t=0;while(t<r.tapCount()&&r.tapModuleId(t)!=m.moduleId)++t;require(t<r.tapCount(),"missing named product tap");taps.push_back(t);}
      std::vector<float>x;x.reserve(std::size_t(frames)*taps.size()*2);for(int i=0;i<frames;++i)for(auto t:taps)for(int c=0;c<2;++c)x.push_back(r.tapSample(t,c,i));return x;
    }
'''
    s=replace(s,'static void staticGate(',helper+'static void staticGate(')
    start=s.index('    std::vector<float> ca,cb,cc,cd;for(');end=s.index('    raw(out/"shared-optimized.f32"',start)
    s=s[:start]+'''    std::vector<float> ca,cb,cc,cd,cp,ch;
    for(int offset=0;offset<8192;offset+=frames){
      kernel.compute(bank.ptr,frames,a.outputPointers.data());llvm.render(b,frames);wholecpp.compute(cpp.ptr,frames,c.outputPointers.data());dynamic(kernel,modular.ptr,p,d,frames);process(*product.renderer,host,midi);
      auto aa=a.capture(frames),bb=b.capture(frames),cx=c.capture(frames),dd=d.capture(frames),pp=productTaps(*product.renderer,fixture.graph,frames);
      try{compare(aa,bb);compare(aa,cx);compare(aa,dd);compare(aa,pp);matchProduct(*product.renderer,fixture.graph,a,frames);}
      catch(...){raw(out/"failure-shared.f32",aa);raw(out/"failure-llvm.f32",bb);raw(out/"failure-cpp.f32",cx);raw(out/"failure-modular.f32",dd);raw(out/"failure-product.f32",pp);std::ofstream(out/"failure-offset.txt")<<offset;throw;}
      append(ca,aa);append(cb,bb);append(cc,cx);append(cd,dd);append(cp,pp);append(ch,interleave(host));
    }
    raw(out/"actual-product-taps.f32",cp);raw(out/"actual-product-host.f32",ch);
'''+s[end:]
    s=replace(s,'ModuleLLVM reference(manifest);','ModuleLLVM reference(manifest),unchanged(manifest);')
    s=replace(s,'Audio audio(channels),ref(channels),negative(channels);','Audio audio(channels),ref(channels),negative(channels),noedit(channels);')
    s=replace(s,'std::vector<float> actual,expected,resetAudio;','std::vector<float> actual,expected,resetAudio,noeditAudio;')
    s=replace(s,'*gain=value;reference.gain(value);','*gain=value;reference.gain(value);unchanged.gain(value);')
    s=replace(s,'reference.render(topologyB?pb:pa,ref,count);compare(audio.capture(count),ref.capture(count));','reference.render(topologyB?pb:pa,ref,count);unchanged.render(pa,noedit,count);compare(audio.capture(count),ref.capture(count));append(noeditAudio,noedit.capture(count));')
    s=replace(s,'raw(out/"transition.f32",actual);','''raw(out/"no-edit-negative.f32",noeditAudio);double changedMaster=0;std::size_t chn=channels/2;
    for(int i=4096;i<12288;++i)for(int c=0;c<2;++c){auto ix=std::size_t(i)*chn+2*f.output+c;changedMaster=std::max(changedMaster,std::abs(double(actual[ix])-noeditAudio[ix]));}
    require(changedMaster>1e-4,"no observable change against the no-edit negative");raw(out/"transition.f32",actual);''')
    s=replace(s,'prop(result,"schema",aLib.schema());','prop(result,"edited_master_vs_noedit",changedMaster);prop(result,"schema",aLib.schema());')
    s=replace(s,'} // namespace\nint main(', '} // namespace\n#include "live.h"\nint main(')
    s=replace(s,'else throw std::runtime_error("unknown mode");','else if(mode=="live"||mode=="live-stress")ps_test::live(root,out,family,n,frames,mode=="live-stress");else throw std::runtime_error("unknown mode");')
    (HERE/'main.generated.cpp').write_text(s)
    s=(OLD/'analyze.py').read_text().replace('channel 13','channel 7').replace('ch=13;positions=','ch=7;positions=')
    s=s.replace("for backend in ['shared-optimized','whole-cpp','shared-modular']:","for backend in ['shared-optimized','whole-cpp','shared-modular','actual-product-taps']:")
    (HERE/'analyze.py').write_text(s)
    (HERE/'api.h').write_bytes((OLD/'api.h').read_bytes())
    (HERE/'test_analysis.py').write_bytes((OLD/'test_analysis.py').read_bytes())
    (HERE/'adapter-manifest.json').write_text(json.dumps({'source_sha256':PINS,'generated_sha256':{p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in [HERE/'generate.py',HERE/'main.generated.cpp',HERE/'analyze.py',HERE/'api.h']}},indent=2)+'\n')
    print('PINNED_ADAPTER_OK: output taps only; original state/DSP declarations unchanged')
if __name__=='__main__':main()
