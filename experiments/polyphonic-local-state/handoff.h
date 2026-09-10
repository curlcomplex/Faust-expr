#pragma once
// Included inside namespace combined after Scene. Reuses the exact PR45 DSP,
// allocator, mixing, compiler and reference implementation.
struct ReadyCode {std::uint64_t revision=1;ps::Compute compute=nullptr;};
struct HandoffOwner {
    CompileResult build;
    std::unique_ptr<Library> library;
    ReadyCode code;
    std::atomic<const ReadyCode*> ready{nullptr};
    std::atomic<int> built{0}; // 0=pending, 1=compiled, 2=prepared, -1=failed
    std::atomic<bool> request{false};
    std::string error;
    double loadBegin=0,loadEnd=0,published=0;
    std::thread::id loadingThread;job_probe::Policy compilerPolicy;
};
struct CompilerJoin {std::atomic<bool>& cancel;std::thread& thread;~CompilerJoin(){cancel.store(true,std::memory_order_release);if(thread.joinable())thread.join();}};
struct CallbackRecord {
    double scheduled=0,entry=0,renderBegin=0,renderEnd=0,end=0,cpu=0;
    double jobCpu=0,jobMaxWall=0,jobMaxOffCpu=0,jobLastStart=0;
    int workerTC=0,workerOther=0,callerTC=0,policyErrors=0;
    int phase=0; // 0=before, 1=editable-B, 2=pending-A, 3=optimized-B
};
static const char* phaseName(int value) {
    return std::array<const char*,4>{"before","editable-B","pending-A","optimized-B"}[std::size_t(value)];
}
static std::string option(const char* name,const char* fallback) {
    const char* value=std::getenv(name);return value?value:fallback;
}
static void prepareCode(HandoffOwner& owner,Engine& e,const fs::path& binary,int block) {
    owner.loadingThread=std::this_thread::get_id();owner.loadBegin=now();
    owner.library=std::make_unique<Library>(binary);
    require(std::string(owner.library->schema())==e.a.schema()&&owner.library->bytes()==e.a.bytes(),"prepared handoff schema mismatch");
    // New code touches only an independently allocated scratch state here.
    // Never run old/new code concurrently on any sounding voice's state.
    {State scratch(*owner.library);Audio audio(e.planes);
     for(int k=0;k<4;++k)owner.library->compute(scratch.ptr,block,audio.outputPointers.data());}
    owner.code.compute=owner.library->compute;
    owner.loadEnd=now();
}
static void liveTest(Engine& e,const fs::path& root,const fs::path& out,int participants,int grain,int block,int voices,const std::string& policy) {
    const auto handoff=option("PS_HANDOFF","prepared");
    const bool ownerLoad=handoff=="owner",prebuilt=handoff=="prebuilt";
    const auto fault=option("PS_HANDOFF_FAULT","none");
    const bool failCompile=fault=="compile",stale=fault=="stale",badSchema=fault=="schema";
    require(handoff=="prepared"||ownerLoad||prebuilt,"unknown handoff mode");
    require(fault=="none"||failCompile||stale||badSchema,"unknown handoff fault");
    require(fault=="none"||(policy=="deferred"&&!ownerLoad&&!prebuilt),"fault controls require prepared/deferred");
    const bool probing=option("PS_JOB_PROBE","0")=="1";
    job_probe::enabled=probing;
    // Library owner outlives Scene, including the Tracktion workers it owns.
    HandoffOwner prepared;
    require(prepared.ready.is_lock_free()&&prepared.request.is_lock_free()&&prepared.built.is_lock_free(),"handoff atomics are not lock-free");
    Scene scene(e,4,voices,0,participants,grain,true);scene.notes(voices);
    job_probe::owner=std::this_thread::get_id();const auto callerPolicy=job_probe::policy();
    const auto ids=scene.identities();
    juce::AudioBuffer<float> audio(2,maxBlock);
    constexpr std::size_t capacity=4096;
    std::vector<CallbackRecord> records(capacity);
    std::vector<float> capture(capacity*std::size_t(block)*2,0.f);
    std::vector<unsigned char> topology(capacity,0);
    // Precompiled no-compiler control pays all load/prewarm costs before timing.
    if(prebuilt){prepareCode(prepared,e,root/"kernel-B.dylib",block);}
    double request=0,readyTime=0;bool adopted=false,sent=false;
    int postBlocks=0;std::size_t completed=0;std::uint64_t consumed=0;
    // Thread is created BEFORE playback. The render owner only publishes one
    // atomic request; no thread creation, future::get or future destruction.
    std::atomic<bool> cancel{false};
    std::thread compiler([&]{
      prepared.compilerPolicy=job_probe::policy();
      while(!prepared.request.load(std::memory_order_acquire)&&!cancel.load(std::memory_order_acquire))std::this_thread::sleep_for(std::chrono::microseconds(100));
      if(cancel.load(std::memory_order_acquire))return;
      try {
        if(prebuilt){
            prepared.build.started=now();
            std::this_thread::sleep_for(std::chrono::milliseconds(1100));
            prepared.build.finished=now();prepared.build.code=0;
        }else{
            prepared.build=compileLive(root,out,e.pb,"new-instrument-B",failCompile);
            if(prepared.build.code!=0){prepared.built.store(-1,std::memory_order_release);return;}
        }
        if(ownerLoad){prepared.built.store(1,std::memory_order_release);return;}
        if(!prebuilt)prepareCode(prepared,e,badSchema?root.parent_path()/"memory-8"/"kernel-B.dylib":prepared.build.binary,block);
        if(stale)prepared.code.revision=0;
        prepared.published=now();
        prepared.ready.store(&prepared.code,std::memory_order_release);
        prepared.built.store(2,std::memory_order_release);
      }catch(const std::exception& error){prepared.error=error.what();prepared.built.store(-1,std::memory_order_release);}
    });
    CompilerJoin guard{cancel,compiler};
    const auto start=Clock::now(),deadline=start+std::chrono::seconds(10);
    auto schedule=start;
    const auto interval=std::chrono::nanoseconds(std::llround(1e9*block/48000.0));
    const double periodUs=block*1e6/48000.0;
    while(completed<capacity&&Clock::now()<deadline){
        std::this_thread::sleep_until(schedule);
        auto& row=records[completed];row.scheduled=us(schedule);row.entry=now();const auto cpu0=job_probe::cpuUs();
        if(!sent&&consumed>=4096){
            request=now();if(policy!="deferred")scene.stateMode(true,true,policy=="whole");
            sent=true;prepared.request.store(true,std::memory_order_release);
        }
        // Deliberately retained control: the SAME loader/prewarm work is done
        // on the owner. Full entry->end records expose, rather than hide, it.
        if(ownerLoad&&sent&&!adopted&&prepared.built.load(std::memory_order_acquire)==1){
            prepareCode(prepared,e,prepared.build.binary,block);
            prepared.published=now();prepared.ready.store(&prepared.code,std::memory_order_release);
            prepared.built.store(2,std::memory_order_release);
        }
        const auto* code=prepared.ready.load(std::memory_order_acquire);
        if(!adopted&&code&&code->revision==1&&code->compute){
            scene.stateMode(true,false,false,code->compute);adopted=true;readyTime=now();
        }
        row.phase=!sent?0:adopted?3:policy=="deferred"?2:1;
        row.renderBegin=now();scene.render(audio,block);row.renderEnd=now();
        const auto base=completed*std::size_t(block)*2;
        for(int s=0;s<block;++s)for(int c=0;c<2;++c)capture[base+std::size_t(s)*2+c]=audio.getSample(c,s);
        topology[completed]=scene.configs[0].b;
        if(probing){
            for(std::size_t i=0;i<scene.slots.size();i+=std::size_t(grain)){
                const auto& t=scene.slots[i].timing;
                row.jobCpu+=t.cpu;row.policyErrors+=t.scheduling.result!=0;
                if(t.caller)row.callerTC+=t.scheduling.realtime();else if(t.scheduling.realtime())++row.workerTC;else ++row.workerOther;
                row.jobMaxWall=std::max(row.jobMaxWall,t.end-t.begin);
                row.jobMaxOffCpu=std::max(row.jobMaxOffCpu,t.end-t.begin-t.cpu);
                row.jobLastStart=std::max(row.jobLastStart,t.begin-row.renderBegin);
            }
        }
        row.cpu=job_probe::cpuUs()-cpu0;row.end=now();
        ++completed;consumed+=block;schedule+=interval;
        if(adopted&&++postBlocks==128)break;
        if(fault!="none"&&sent&&prepared.built.load(std::memory_order_acquire)!=0&&now()-request>1400000.0)break;
    }
    cancel.store(true,std::memory_order_release);compiler.join(); // outside playback, before any owner retires
    capture.resize(completed*std::size_t(block)*2);
    raw(out/"live.f32",capture);
    std::ofstream trace(out/"callbacks.tsv"),detail(out/"deadline.tsv");
    trace<<"sample\tframes\tphase\tbegin_us\tend_us\n"<<std::setprecision(16);
    detail<<"sample\tframes\tphase\tscheduled_us\tentry_us\trender_begin_us\trender_end_us\tend_us\towner_cpu_us\tjob_cpu_us\tjob_max_wall_us\tjob_max_off_cpu_us\tjob_last_start_us\tworker_tc_jobs\tworker_other_jobs\tcaller_tc_jobs\tpolicy_errors\n"<<std::setprecision(16);
    for(std::size_t k=0;k<completed;++k){const auto& r=records[k];
        trace<<k*block<<'\t'<<block<<'\t'<<phaseName(r.phase)<<'\t'<<r.renderBegin<<'\t'<<r.renderEnd<<'\n';
        detail<<k*block<<'\t'<<block<<'\t'<<phaseName(r.phase)<<'\t'<<r.scheduled<<'\t'<<r.entry<<'\t'<<r.renderBegin<<'\t'<<r.renderEnd<<'\t'<<r.end<<'\t'<<r.cpu<<'\t'<<r.jobCpu<<'\t'<<r.jobMaxWall<<'\t'<<r.jobMaxOffCpu<<'\t'<<r.jobLastStart<<'\t'<<r.workerTC<<'\t'<<r.workerOther<<'\t'<<r.callerTC<<'\t'<<r.policyErrors<<'\n';
    }
    trace.flush();detail.flush();
    require(scene.identities()==ids,"handoff replaced sounding voice state");
    if(fault=="none")require(adopted&&postBlocks==128,"prepared code was not adopted within the bound: "+prepared.error);
    else {require(!adopted,"invalid code was adopted");if(failCompile)require(prepared.build.code>0&&prepared.build.code!=124,"failure negative was a timeout, not rejection");if(failCompile||badSchema)require(prepared.built.load()==-1,"failure negative did not reject");if(stale)require(prepared.built.load()==2,"stale negative was not published");}
    // Verification and output-change detection never compete with playback.
    Scene reference(e,4,voices,2,1,1,false),unchanged(e,4,voices,2,1,1,false);
    reference.notes(voices);unchanged.notes(voices);
    std::vector<float> expected,counterfactual;expected.reserve(capture.size());counterfactual.reserve(capture.size());
    for(std::size_t k=0;k<completed;++k){
        reference.stateMode(topology[k],true);reference.render(audio,block);append(expected,unchecked(audio,block));
        unchanged.render(audio,block);append(counterfactual,unchecked(audio,block));
    }
    raw(out/"reference.f32",expected);raw(out/"counterfactual-A.f32",counterfactual);compare(capture,expected);
    if(fault!="none")compare(capture,counterfactual);
    V r=obj();prop(r,"context",option("PS_RT_CONTEXT","legacy"));
    auto policyJson=[](const job_probe::Policy& p){V x=obj();prop(x,"result",p.result);prop(x,"default",p.defaultPolicy);prop(x,"period",double(p.period));prop(x,"computation",double(p.computation));prop(x,"constraint",double(p.constraint));return x;};
    prop(r,"caller_thread_policy",policyJson(callerPolicy));prop(r,"compiler_thread_policy",policyJson(prepared.compilerPolicy));prop(r,"policy",policy);prop(r,"handoff",handoff);prop(r,"fault",fault);prop(r,"job_probe",probing);
    prop(r,"request_us",request);prop(r,"ready_us",readyTime);prop(r,"compile_begin_us",prepared.build.started);prop(r,"compile_end_us",prepared.build.finished);
    prop(r,"load_begin_us",prepared.loadBegin);prop(r,"load_end_us",prepared.loadEnd);prop(r,"published_us",prepared.published);
    prop(r,"compiler_exit",prepared.build.code);prop(r,"adopted",adopted);prop(r,"loader_on_render_owner",prepared.loadingThread==std::this_thread::get_id());prop(r,"precompiled_control",prebuilt);
    prop(r,"voices",4*voices);prop(r,"participants",participants);prop(r,"grain",grain);prop(r,"sample_frames",double(consumed));
    prop(r,"compiled_instruments",prebuilt?0:1);prop(r,"unaffected_optimized_instruments",policy=="whole"?0:3);prop(r,"state_objects_replaced",V(0));
    prop(r,"workers_observed",double(scene.observedThreads()));prop(r,"period_us",periodUs);prop(r,"error",prepared.error);
    writeJson(out/"result.json",r);
}
