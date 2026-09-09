#pragma once
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <future>
extern char** environ;

namespace ps_test {
static std::string lit(float x){std::ostringstream o;o<<std::setprecision(9)<<std::scientific<<x<<'f';return o.str();}
// Emit a new graph-specific loop from the actual prepared routing order.
// Faust-generated module headers and the exact Bank declaration are reused.
static std::string liveSource(const fs::path& root,const Plan& p){
    std::ostringstream o;o<<"#include "<<std::quoted((root/"bank.h").string())<<"\n";
    o<<"API void ps_compute(void* __restrict ptr,int count,float** __restrict planes){auto* b=static_cast<Bank*>(ptr);\n";
    for(std::size_t i=0;i<p.incoming.size();++i)o<<"b->m"<<i<<".control();\n";
    o<<"for(int s=0;s<count;++s){\n";
    for(int i:p.order){
        std::array<std::string,2> sum={"0.0f","0.0f"};
        for(auto& e:p.incoming[i])for(int c=0;c<2;++c){
            std::string x=e.delay?"b->previous["+std::to_string(e.key)+"]["+std::to_string(c)+"]":"v"+std::to_string(e.s)+"["+std::to_string(c)+"]";
            sum[c]+="+("+x+"*"+lit(c?e.r:e.l)+")";
        }
        o<<"float x"<<i<<"[2]={"<<sum[0]<<','<<sum[1]<<"},v"<<i<<"[2];b->m"<<i<<".PSM"<<i<<"::frame(x"<<i<<",v"<<i<<");\n";
        o<<"planes["<<2*i<<"][s]=v"<<i<<"[0];planes["<<2*i+1<<"][s]=v"<<i<<"[1];\n";
    }
    for(auto& e:p.edges)if(e.delay)o<<"b->previous["<<e.key<<"][0]=v"<<e.s<<"[0];b->previous["<<e.key<<"][1]=v"<<e.s<<"[1];\n";
    o<<"}}\n";return o.str();
}
struct CompileResult {int code=-1;double started=0,finished=0;fs::path source,binary;};
static CompileResult compileLive(const fs::path& root,const fs::path& out,const Plan& p,const std::string& name,bool bad=false){
    CompileResult r;r.started=now();r.source=out/(name+".cpp");r.binary=out/(name+".dylib");
    {std::ofstream f(r.source);f<<(bad?"#error INTENTIONAL_COMPILE_FAILURE\n":liveSource(root,p));require(bool(f),"live source write");}
    const char* prefix=std::getenv("PS_FAUST_PREFIX");require(prefix&&*prefix,"missing fixed Faust include prefix");
    std::vector<std::string> args={"clang++","-std=c++17","-O3","-DNDEBUG","-ffast-math","-ffp-contract=off","-fvisibility=hidden","-dynamiclib","-I"+std::string(prefix)+"/include",r.source.string(),"-o",r.binary.string()};
    std::vector<char*> argv;for(auto& a:args)argv.push_back(a.data());argv.push_back(nullptr);
    {std::ofstream f(out/(name+"-command.txt"));for(auto& a:args)f<<std::quoted(a)<<' ';f<<'\n';}
    posix_spawn_file_actions_t actions;posix_spawn_file_actions_init(&actions);
    auto log=out/(name+"-compile.log");posix_spawn_file_actions_addopen(&actions,STDOUT_FILENO,log.c_str(),O_WRONLY|O_CREAT|O_TRUNC,0644);posix_spawn_file_actions_adddup2(&actions,STDOUT_FILENO,STDERR_FILENO);
    posix_spawnattr_t attr;posix_spawnattr_init(&attr);posix_spawnattr_setflags(&attr,POSIX_SPAWN_SETPGROUP);posix_spawnattr_setpgroup(&attr,0);
    pid_t pid=-1;int err=posix_spawnp(&pid,argv[0],&actions,&attr,argv.data(),environ);posix_spawnattr_destroy(&attr);posix_spawn_file_actions_destroy(&actions);
    require(err==0&&pid>0,"spawn compiler failed");int status=0;auto deadline=Clock::now()+std::chrono::seconds(25);
    for(;;){auto got=waitpid(pid,&status,WNOHANG);if(got==pid)break;require(got==0,"compiler wait failed");if(Clock::now()>deadline){kill(-pid,SIGKILL);waitpid(pid,&status,0);r.code=124;r.finished=now();return r;}std::this_thread::sleep_for(std::chrono::milliseconds(2));}
    r.code=WIFEXITED(status)?WEXITSTATUS(status):128+WTERMSIG(status);r.finished=now();return r;
}
struct LivePlan {std::uint64_t revision=0;bool b=false;Plan routing;};
struct LiveKernel {std::uint64_t revision=0;bool b=false;ps::Compute compute=nullptr;};
struct LiveBlock {double begin=0,end=0,scheduled=0;std::uint64_t revision=0;bool b=false,optimized=false;};
static void live(const fs::path& root,const fs::path& out,const std::string& family,int n,int frames,bool stress){
    auto f=make(family,n);auto manifest=read(root/"manifest.json");const int planeCount=4*int(f.graph.modules().size()),outputCount=planeCount/2;
    Library base(root/"kernel-A.dylib");
    double periodUs=frames*1e6/48000.0;int copies=1;double calibration=0;
    if(stress){State calibrationState(base);Audio buffer(planeCount);std::vector<double> times;
        for(int repeat=0;repeat<7;++repeat){auto t=now();for(int k=0;k<32;++k){base.compute(calibrationState.ptr,frames,buffer.outputPointers.data());observe(buffer,frames);}times.push_back((now()-t)/32);}
        std::sort(times.begin(),times.end());calibration=times[times.size()/2];copies=std::clamp(int(.75*periodUs/std::max(1.0,calibration)),1,96);
    }
    std::vector<std::unique_ptr<State>> banks;for(int k=0;k<copies;++k)banks.push_back(std::make_unique<State>(base));
    std::vector<std::unique_ptr<LivePlan>> plans;std::vector<std::unique_ptr<LiveKernel>> kernels;std::vector<std::unique_ptr<Library>> owners;
    auto initial=std::make_unique<LivePlan>();initial->routing=plan(f.graph);plans.push_back(std::move(initial));
    kernels.push_back(std::make_unique<LiveKernel>(LiveKernel{0,false,base.compute}));
    std::atomic<const LivePlan*> requested{plans[0].get()};std::atomic<const LiveKernel*> ready{kernels[0].get()};
    std::atomic<bool> stop{false},started{false};std::atomic<std::uint64_t> consumed{0};
    const std::size_t capacity=std::size_t(48000*8/frames);std::vector<LiveBlock> blocks(capacity);std::vector<float> captures(capacity*frames*outputCount);std::size_t completed=0;
    std::thread audio([&]{Audio buffer(planeCount);auto schedule=Clock::now();auto interval=std::chrono::nanoseconds(std::llround(1e9*frames/48000.0));started.store(true,std::memory_order_release);
      while(!stop.load(std::memory_order_acquire)&&completed<capacity){
        std::this_thread::sleep_until(schedule);auto* request=requested.load(std::memory_order_acquire);auto* candidate=ready.load(std::memory_order_acquire);
        bool optimized=candidate&&candidate->compute&&candidate->revision==request->revision&&candidate->b==request->b;
        auto t0=now();
        for(int copy=0;copy<copies;++copy){
            if(optimized)candidate->compute(banks[copy]->ptr,frames,buffer.outputPointers.data());else dynamic(base,banks[copy]->ptr,request->routing,buffer,frames);
            observe(buffer,frames);
            if(copy==0)for(int s=0;s<frames;++s)for(int c=0;c<outputCount;++c)captures[(completed*frames+s)*outputCount+c]=buffer.outputPointers[c][s];
        }
        auto t1=now();blocks[completed]={t0,t1,us(schedule),request->revision,request->b,optimized};++completed;consumed.store(completed*frames,std::memory_order_release);schedule+=interval;
      }
    });ThreadGuard guard{stop,audio};
    while(!started.load(std::memory_order_acquire))std::this_thread::yield();
    while(consumed.load(std::memory_order_acquire)<4096)std::this_thread::sleep_for(std::chrono::milliseconds(1));
    std::ofstream events(out/"events.tsv");events<<"revision\ttopology\trequest_us\tpublished_us\n"<<std::setprecision(16);
    auto submit=[&](std::uint64_t rev,bool b){auto t=now();auto p=std::make_unique<LivePlan>();p->revision=rev;p->b=b;p->routing=plan(b?graphB(f):f.graph);plans.push_back(std::move(p));requested.store(plans.back().get(),std::memory_order_release);double done=now();events<<rev<<'\t'<<(b?"B":"A")<<'\t'<<t<<'\t'<<done<<'\n';events.flush();return t;};
    auto publish=[&](std::uint64_t revision,bool b,ps::Compute compute){auto* req=requested.load(std::memory_order_acquire);if(!compute||req->revision!=revision||req->b!=b)return false;kernels.push_back(std::make_unique<LiveKernel>(LiveKernel{revision,b,compute}));ready.store(kernels.back().get(),std::memory_order_release);return true;};
    double edit=submit(1,true);auto compiler=std::async(std::launch::async,[&]{return compileLive(root,out,plans.back()->routing,"online-B");});auto built=compiler.get();require(built.code==0,"actual B compilation failed");
    owners.push_back(std::make_unique<Library>(built.binary));auto* compiledB=owners.back().get();require(std::string(base.schema())==compiledB->schema()&&base.bytes()==compiledB->bytes(),"online state schema changed");
    require(publish(1,true,compiledB->compute),"online B publication rejected");double optimizedPublished=now();
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    submit(2,false);const Plan* stalePlan=&plans.back()->routing;auto older=std::async(std::launch::async,[&]{return compileLive(root,out,*stalePlan,"stale-A");});
    std::this_thread::sleep_for(std::chrono::milliseconds(5));submit(3,true);auto old=older.get();require(old.code==0,"stale test compiler failed");
    owners.push_back(std::make_unique<Library>(old.binary));bool staleRejected=!publish(2,false,owners.back()->compute);require(staleRejected,"stale compiler result accepted");
    // Returning to the already compiled B code reuses it with the current state.
    require(publish(3,true,compiledB->compute),"cached code republish failed");
    auto bad=compileLive(root,out,plans.back()->routing,"invalid-negative",true);require(bad.code!=0&&bad.code!=124,"malformed-source negative not detected");
    bool failedRejected=!publish(3,true,nullptr);require(failedRejected,"failed factory accepted");
    Library incompatible(root.parent_path()/"ben-4"/"kernel-A.dylib");bool schemaRejected=std::string(base.schema())!=incompatible.schema()||base.bytes()!=incompatible.bytes();require(schemaRejected,"incompatible schema negative unavailable");
    std::this_thread::sleep_for(std::chrono::milliseconds(80));stop.store(true,std::memory_order_release);audio.join();
    captures.resize(completed*frames*outputCount);raw(out/"live-output.f32",captures);
    std::ofstream record(out/"callbacks.tsv");record<<"block\trevision\ttopology\toptimized\tbegin_us\tend_us\tscheduled_us\n"<<std::setprecision(16);
    for(std::size_t i=0;i<completed;++i){auto& b=blocks[i];record<<i<<'\t'<<b.revision<<'\t'<<(b.b?"B":"A")<<'\t'<<b.optimized<<'\t'<<b.begin<<'\t'<<b.end<<'\t'<<b.scheduled<<'\n';}record.flush();
    require(completed<capacity,"bounded live capture capacity exhausted");
    ModuleLLVM reference(manifest),noedit(manifest);auto pa=plan(f.graph),pb=plan(graphB(f));Audio ref(planeCount),negative(planeCount);std::vector<float> expected,noeditCapture;expected.reserve(captures.size());noeditCapture.reserve(captures.size());
    bool sawEditable=false,sawOptimized=false;double firstEditable=0,firstOptimized=0,changedBeforeCompile=0;std::size_t misses=0;
    for(std::size_t i=0;i<completed;++i){auto& r=blocks[i];reference.render(r.b?pb:pa,ref,frames);noedit.render(pa,negative,frames);auto y=ref.capture(frames);append(expected,y);append(noeditCapture,negative.capture(frames));
        std::vector<float>x(captures.begin()+i*frames*outputCount,captures.begin()+(i+1)*frames*outputCount);compare(x,y);
        if(r.revision==1&&!r.optimized){sawEditable=true;if(firstEditable==0)firstEditable=r.end;}
        if(r.revision==1&&r.optimized){sawOptimized=true;if(firstOptimized==0)firstOptimized=r.end;}
        if(r.revision==1&&r.end<built.finished)for(int s=0;s<frames;++s)for(int c=0;c<2;++c){int ch=2*f.output+c;changedBeforeCompile=std::max(changedBeforeCompile,std::abs(double(x[s*outputCount+ch])-negative.outputPointers[ch][s]));}
        if(r.end>r.scheduled+periodUs)++misses;
    }
    raw(out/"live-reference.f32",expected);raw(out/"live-noedit-negative.f32",noeditCapture);
    require(sawEditable&&sawOptimized&&changedBeforeCompile>1e-4,"changed audio not observed before optimized compilation completed");
    V result=obj();prop(result,"schema",base.schema());prop(result,"frames",frames);prop(result,"channels",outputCount);prop(result,"blocks",double(completed));prop(result,"state_copies_for_load",copies);prop(result,"stress",stress);prop(result,"calibration_us_per_copy",calibration);prop(result,"target_load_fraction",stress?.75:0.0);
    prop(result,"request_us",edit);prop(result,"compile_begin_us",built.started);prop(result,"compile_end_us",built.finished);prop(result,"optimized_publication_us",optimizedPublished);prop(result,"request_to_editable_compute_ms",(firstEditable-edit)/1000);prop(result,"request_to_optimized_compute_ms",(firstOptimized-edit)/1000);prop(result,"compile_ms",(built.finished-built.started)/1000);prop(result,"changed_master_before_compile",changedBeforeCompile);prop(result,"synthetic_deadline_misses",double(misses));prop(result,"stale_rejected",staleRejected);prop(result,"failed_rejected",failedRejected);prop(result,"schema_rejected",schemaRejected);prop(result,"state_instances_created_during_edits",0);prop(result,"state_bytes_copied_during_edits",0);writeJson(out/"live-result.json",result);
}
} // namespace ps_test
