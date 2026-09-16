// Offline compute-only measurement around the SAME generated DSP and reviewed UI.
// No audio I/O, per-callback timing, diagnostics, or claims about device deadlines.
#include <cstdlib>
#include <new>
static bool countNew = false;
static unsigned long long newCalls = 0;
void* operator new(std::size_t n) {
    if (countNew) ++newCalls;
    if (void* p=std::malloc(n?n:1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p,std::size_t) noexcept { std::free(p); }
void operator delete[](void* p,std::size_t) noexcept { std::free(p); }
#define main retained_renderer_main
#include "render.cpp"
#undef main
int main(int argc,char** argv) {
    try {
        if (argc!=2) throw std::runtime_error("usage: bench parameter-score.tsv");
        constexpr int rate=48000, block=128, frames=rate*4;
        ModuleDSP d; d.init(rate); UI ui; d.buildUserInterface(&ui); ui.finish();
        ui.finishCleanBenchmark();
        std::ifstream score(argv[1]); if (!score) throw std::runtime_error("score open");
        int frame; std::string name; float value;
        while (score>>frame>>name>>value) {
            if (frame!=0) throw std::runtime_error("benchmark controls must be at frame zero");
            ui.set(name,value);
        }
        if (!score.eof()) throw std::runtime_error("bad score");
        if (d.getNumInputs()!=0 || d.getNumOutputs()!=1) throw std::runtime_error("voice I/O");
        float buffer[block]={}; float* outputs[]={buffer};
        double ns[7]={}; unsigned long long calls[7]={}; double checksum=0;
        for (int trial=0;trial<7;++trial) {
            d.instanceClear(); ui.set("gate",0); d.compute(block,nullptr,outputs);
            ui.set("gate",1);
            for (int w=0;w<8;++w) d.compute(block,nullptr,outputs);
            newCalls=0; countNew=true;
            const auto start=std::chrono::steady_clock::now();
            for (int n=0;n<frames;n+=block) d.compute(std::min(block,frames-n),nullptr,outputs);
            const auto end=std::chrono::steady_clock::now();
            countNew=false;
            ns[trial]=std::chrono::duration<double,std::nano>(end-start).count(); calls[trial]=newCalls;
            for (float v:buffer) { if (!std::isfinite(v)) throw std::runtime_error("nonfinite DSP"); checksum+=std::abs(v); }
        }
        std::cout<<std::setprecision(12)<<"{\"sample_rate\":"<<rate<<",\"block\":"<<block
            <<",\"frames_per_trial\":"<<frames<<",\"instance_bytes\":"<<sizeof(ModuleDSP)
            <<",\"output_buffer_bytes\":"<<sizeof(buffer)<<",\"compute_ns\":[";
        for(int i=0;i<7;++i) { if(i)std::cout<<','; std::cout<<ns[i]; }
        std::cout<<"],\"operator_new_calls\":[";
        for(int i=0;i<7;++i) { if(i)std::cout<<','; std::cout<<calls[i]; }
        std::cout<<"],\"output_checksum\":"<<checksum<<"}\n";
    } catch (const std::exception& e) { countNew=false; std::cerr<<e.what()<<'\n'; return 1; }
}
