#!/usr/bin/env python3
"""PR45 buffer contract repair and negative controls. No tolerance changes."""

def once(s, old, new):
    if s.count(old) != 1:
        raise ValueError('repair anchor missing/ambiguous: '+old[:100])
    return s.replace(old, new)

def apply(native):
    native=once(native,'tracktion::engine::MidiMessageArray midi;int64_t clock=0;',
                'tracktion::engine::MidiMessageArray midi;int64_t clock=0;bool clearDestination=true;')
    native=once(native,'juce::AudioBuffer<float> view(outputs,2,n);midi.clear();player->process(',
                'juce::AudioBuffer<float> view(outputs,2,n);if(clearDestination)view.clear();midi.clear();player->process(')
    helper=r'''// Tracktion's NodePlayer ADDS into pc.buffers.audio. This adapter promises
// overwrite semantics. A dirty destination is therefore part of the test input.
static void poison(juce::AudioBuffer<float>& b,int frames){
    for(int s=0;s<frames;++s){b.setSample(0,s,.375f);b.setSample(1,s,-.625f);}
}
static std::vector<float> unchecked(const juce::AudioBuffer<float>& b,int n){
    std::vector<float>x(std::size_t(n)*2);for(int s=0;s<n;++s)for(int c=0;c<2;++c)x[2*s+c]=b.getSample(c,s);return x;
}
static void bufferContract(Engine& e,const fs::path& out){
    // Same code with precisely the missing clear switched off reproduces the
    // original defect deterministically; neither arm relies on uninitialised RAM.
    Scene repaired(e,1,2,0,1,4,true),broken(e,1,2,0,1,4,true),reference(e,1,2,2,1,1,false);
    repaired.notes(1);broken.notes(1);reference.notes(1);broken.clearDestination=false;
    juce::AudioBuffer<float>a(2,maxBlock),b(2,maxBlock),r(2,maxBlock);
    std::vector<float> good,bad,expected;double defect=0;
    for(int block=0;block<3;++block){
        poison(a,13);poison(b,13);poison(r,13);
        repaired.render(a,13);broken.render(b,13);reference.render(r,13);
        auto aa=unchecked(a,13),bb=unchecked(b,13),rr=unchecked(r,13);
        append(good,aa);append(bad,bb);append(expected,rr);
        for(std::size_t i=0;i<bb.size();++i)defect=std::max(defect,std::abs(double(bb[i])-rr[i]));
    }
    raw(out/"buffer-fixed.f32",good);raw(out/"buffer-original-defect.f32",bad);raw(out/"buffer-reference.f32",expected);
    compare(good,expected);require(defect>.01,"missing-clear negative did not reproduce wrong output");
    V meta=obj();prop(meta,"missing_clear_max_error",defect);prop(meta,"frames",39);prop(meta,"only_clear_differs",true);
    writeJson(out/"buffer-contract.json",meta);
}
'''
    native=once(native,'static void conformance(Engine& e,',helper+'static void conformance(Engine& e,')
    native=once(native,'    Scene candidate(e,2,8,0,participants,grain,true),reference(e,2,8,2,1,1,false);',
                '    bufferContract(e,out);\n    Scene candidate(e,2,8,0,participants,grain,true),reference(e,2,8,2,1,1,false);')
    native=once(native,'        candidate.render(a,n);reference.render(b,n);',
                '        poison(a,n);poison(b,n);candidate.render(a,n);reference.render(b,n);')
    old='juce::AudioBuffer<float> av(a.getArrayOfWritePointers(),2,n),bv(b.getArrayOfWritePointers(),2,n);auto aa=interleave(av),bb=interleave(bv);compare(aa,bb);compareVoices(candidate,reference);append(x,aa);append(y,bb);'
    new=r'''auto aa=unchecked(a,n),bb=unchecked(b,n);
        try{compare(aa,bb);compareVoices(candidate,reference);}
        catch(...){raw(out/"failure-candidate.f32",aa);raw(out/"failure-reference.f32",bb);raw(out/"prior-candidate.f32",x);raw(out/"prior-reference.f32",y);V failure=obj();prop(failure,"sample_offset",offset);prop(failure,"frames",n);writeJson(out/"failure.json",failure);throw;}
        append(x,aa);append(y,bb);'''
    native=once(native,old,new)
    # Destroy/join Scene before unloading freshly compiled code it could borrow.
    native=once(native,'    Scene scene(e,4,voices,0,participants,grain,true);',
                '    std::unique_ptr<Library> compiled;Scene scene(e,4,voices,0,participants,grain,true);')
    native=once(native,'std::future<CompileResult> compiling;std::unique_ptr<Library> compiled;',
                'std::future<CompileResult> compiling;')
    old='auto y=interleave(view);std::vector<float>x(capture.begin()+k*block*2,capture.begin()+(k+1)*block*2);compare(x,y);append(expected,y);'
    new=r'''auto y=interleave(view);std::vector<float>x(capture.begin()+k*block*2,capture.begin()+(k+1)*block*2);
        try{compare(x,y);}catch(...){raw(out/"failure-live.f32",x);raw(out/"failure-reference.f32",y);std::ofstream(out/"failure-block.txt")<<k<<'\n';throw;}append(expected,y);'''
    native=once(native,old,new)
    return native
