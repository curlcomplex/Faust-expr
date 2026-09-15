// Offline actual-Faust runner. No audio device, clipping, normalization or FX.
// Self-contained architecture: some qualification jobs copy this source verbatim.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#define FAUSTFLOAT float
#ifndef FAUST_EXPR_DIAGNOSTIC
#define FAUST_EXPR_DIAGNOSTIC 0
#endif
// Diagnostics are built explicitly, never selected just by a filename or CLI flag.
constexpr bool diagnosticBuild = FAUST_EXPR_DIAGNOSTIC != 0;
struct dsp { virtual ~dsp() = default; };
struct Meta { void declare(const char*, const char*) {} };

static std::string jsonString(const std::string& text) {
    std::ostringstream out; out << '"';
    for (unsigned char c : text) {
        if (c == '"' || c == '\\') out << '\\' << char(c);
        else if (c < 32) out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << unsigned(c) << std::dec;
        else out << char(c);
    }
    return out.str() + '"';
}
// Percent-encoded UTF-8 components: injective and safe in whitespace-delimited scores.
static std::string pathComponent(const std::string& label) {
    std::ostringstream out;
    for (unsigned char c : label) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.') out << char(c);
        else out << '%' << std::uppercase << std::hex << std::setw(2) << std::setfill('0') << unsigned(c) << std::dec;
    }
    return out.str();
}
using Metadata = std::map<std::string, std::string>;
static void writeMetadata(std::ostream& out, const Metadata& metadata) {
    out << '{'; bool first = true;
    for (const auto& [key, value] : metadata) {
        if (!first) out << ','; first = false;
        out << jsonString(key) << ':' << jsonString(value);
    }
    out << '}';
}

struct UI {
    // No writable zone pointer is exposed by this descriptor API.
    struct Zone {
        std::string label, path, kind;
        float lo, hi, def, step;
        bool boolean, writable;
        Metadata metadata;
    };
    struct Group { std::string label, path, kind; Metadata metadata; };
    // Legacy consumers get writable controls ONLY, in stable selector order.
    std::map<std::string, Zone> zones;
private:
    struct Binding { Zone zone; float* p; };
    std::map<std::string, Binding> bindings;
    std::map<float*, Metadata> declarations;
    std::vector<Group> stack, groups;
    std::vector<Zone> meters;
    Metadata pendingGroup;
    bool finished = false;
    size_t probeCount = 0;
    void mutableUI() const { if (finished) throw std::runtime_error("UI already finalized"); }
    std::string path(const std::string& label) const {
        return (stack.empty() ? "" : stack.back().path) + '/' + pathComponent(label);
    }
    void open(const char* label, const char* kind) {
        mutableUI(); Group group{label, path(label), kind, pendingGroup}; pendingGroup.clear();
        groups.push_back(group); stack.push_back(group);
    }
    void add(const char* label, float* p, float lo, float hi, float def, float step,
             const char* kind, bool boolean, bool writable) {
        mutableUI();
        if (!p || !std::isfinite(lo) || !std::isfinite(hi) || lo > hi)
            throw std::runtime_error("invalid UI zone");
        Zone zone{label, path(label), kind, lo, hi, def, step, boolean, writable, {}};
        if (!bindings.emplace(zone.path, Binding{zone, p}).second)
            throw std::runtime_error("duplicate UI path: " + zone.path);
    }
    const Binding& control(const std::string& name) const {
        if (!finished) throw std::runtime_error("UI not finalized");
        auto alias = zones.find(name);
        auto entry = bindings.find(alias == zones.end() ? name : alias->second.path);
        if (entry == bindings.end() || !entry->second.zone.writable)
            throw std::runtime_error("invalid control (unknown, ambiguous or read-only): " + name);
        return entry->second;
    }
    static void writeZone(std::ostream& out, const Zone& z) {
        out << "{\"path\":" << jsonString(z.path) << ",\"label\":" << jsonString(z.label)
            << ",\"kind\":" << jsonString(z.kind) << ",\"min\":" << z.lo << ",\"max\":" << z.hi
            << ",\"default\":";
        if (z.writable) out << z.def; else out << "null"; // meters have no parameter default
        out << ",\"step\":" << z.step << ",\"writable\":" << (z.writable ? "true" : "false")
            << ",\"boolean\":" << (z.boolean ? "true" : "false") << ",\"probe_id\":";
        auto probe = z.metadata.find("probe");
        if (probe == z.metadata.end()) out << "null"; else out << jsonString(probe->second);
        out << ",\"metadata\":"; writeMetadata(out, z.metadata); out << '}';
    }
public:
    void openTabBox(const char* label) { open(label, "tab"); }
    void openHorizontalBox(const char* label) { open(label, "horizontal"); }
    void openVerticalBox(const char* label) { open(label, "vertical"); }
    void closeBox() {
        mutableUI(); if (stack.empty()) throw std::runtime_error("unbalanced UI close"); stack.pop_back();
    }
    void declare(float* p, const char* key, const char* value) {
        mutableUI(); auto& metadata = p ? declarations[p] : pendingGroup;
        auto found = metadata.find(key);
        if (found != metadata.end() && found->second != value)
            throw std::runtime_error("conflicting UI metadata: " + std::string(key));
        metadata[key] = value;
    }
    void addButton(const char* n, float* p) { add(n,p,0,1,0,1,"button",true,true); }
    void addCheckButton(const char* n, float* p) { add(n,p,0,1,0,1,"checkbox",true,true); }
    void addVerticalSlider(const char* n,float* p,float d,float l,float h,float s) { add(n,p,l,h,d,s,"vslider",false,true); }
    void addHorizontalSlider(const char* n,float* p,float d,float l,float h,float s) { add(n,p,l,h,d,s,"hslider",false,true); }
    void addNumEntry(const char* n,float* p,float d,float l,float h,float s) { add(n,p,l,h,d,s,"nentry",false,true); }
    void addVerticalBargraph(const char* n,float* p,float l,float h) { add(n,p,l,h,0,0,"vbargraph",false,false); }
    void addHorizontalBargraph(const char* n,float* p,float l,float h) { add(n,p,l,h,0,0,"hbargraph",false,false); }
    void finish() {
        mutableUI();
        if (!stack.empty() || !pendingGroup.empty()) throw std::runtime_error("unbalanced UI groups/metadata");
        std::map<std::string, size_t> labels; std::set<std::string> ids;
        // A pointer must not be both an input control and a read-only meter.
        std::map<float*, bool> access;
        for (auto& [path, binding] : bindings) {
            auto& z = binding.zone; z.metadata = declarations[binding.p];
            auto previous = access.emplace(binding.p, z.writable);
            if (!previous.second && previous.first->second != z.writable)
                throw std::runtime_error("zone aliases writable control and read-only meter");
            if (z.writable) ++labels[z.label];
            auto probe = z.metadata.find("probe");
            if (probe != z.metadata.end()) {
                if (z.writable || probe->second.empty() || !ids.insert(probe->second).second)
                    throw std::runtime_error("invalid or duplicate probe ID: " + probe->second);
                ++probeCount;
            }
        }
        for (const auto& [path, binding] : bindings) {
            const auto& z = binding.zone;
            if (!z.writable) { meters.push_back(z); continue; }
            // Keep old unique labels; duplicate/unsafe labels require their full path.
            bool safe = !z.label.empty() && z.label.front() != '/' && z.label.front() != '#';
            for (unsigned char c : z.label) if (c <= 32 || c == 127) safe = false;
            std::string selector = labels[z.label] == 1 && safe ? z.label : z.path;
            if (!zones.emplace(selector, z).second) throw std::runtime_error("control selector collision");
        }
        finished = true;
    }
    void validate(const std::string& name, float x) const {
        const auto& z = control(name).zone;
        if (!std::isfinite(x) || x < z.lo || x > z.hi || (z.boolean && x != 0 && x != 1))
            throw std::runtime_error("invalid control: " + name);
    }
    void set(const std::string& name, float x) { validate(name,x); *control(name).p = x; }
    std::string controlPath(const std::string& name) const { return control(name).zone.path; }
    size_t probe_count() const { return probeCount; }
    const std::vector<Zone>& meter_catalog() const { return meters; }
    std::vector<float> meter_values() const {
        std::vector<float> result; result.reserve(meters.size());
        for (const auto& z : meters) {
            float value = *bindings.at(z.path).p;
            if (!std::isfinite(value)) throw std::runtime_error("nonfinite meter: " + z.path);
            result.push_back(value); // UI ranges are display hints, NOT clipping limits.
        }
        return result;
    }
    void json(std::ostream& out, int ni, int no) const {
        out << std::setprecision(9) << "{\"schema\":1,\"inputs\":" << ni << ",\"outputs\":" << no
            << ",\"diagnostic_build\":" << (diagnosticBuild ? "true" : "false")
            << ",\"benchmark_eligible\":" << (!diagnosticBuild && !probeCount ? "true" : "false")
            << ",\"probe_count\":" << probeCount << ",\"groups\":[";
        bool first = true;
        for (const auto& g : groups) {
            if (!first) out << ','; first = false;
            out << "{\"path\":" << jsonString(g.path) << ",\"label\":" << jsonString(g.label)
                << ",\"kind\":" << jsonString(g.kind) << ",\"metadata\":";
            writeMetadata(out,g.metadata); out << '}';
        }
        out << "],\"controls\":["; first = true;
        for (const auto& [selector, z] : zones) {
            if (!first) out << ','; first = false; writeZone(out,z);
        }
        out << "],\"meters\":["; first = true;
        for (const auto& z : meters) { if (!first) out << ','; first = false; writeZone(out,z); }
        out << "]}";
    }
};
#include "generated.hpp"

long long integer(const std::string& s) {
    size_t used=0; auto x=std::stoll(s,&used);
    if (used!=s.size()) throw std::runtime_error("invalid integer"); return x;
}
float real(const std::string& s) {
    size_t used=0; float x=std::stof(s,&used);
    if (used!=s.size() || !std::isfinite(x)) throw std::runtime_error("invalid float"); return x;
}
struct Event { int frame; std::string name; float value; };

// Each meter is observed AFTER every compute(1). A stride only reduces storage:
// min/max/last retain all observed values in [frame_begin, frame_end_exclusive).
// These are meter values, not inferred raw signal peaks or whole-file RMS values.
struct ProbeCapture {
    std::ofstream out;
    std::vector<float> minimum, maximum, last;
    int begin=0, stride, rate, rows=0;
    ProbeCapture(const std::string& file, const UI& ui, int sr, int block, int hop, int ni, int no)
        : out(file), stride(hop), rate(sr) {
        if (!out) throw std::runtime_error("probe output open failed");
        out << "{\"type\":\"header\",\"schema\":1,\"mode\":\"diagnostic\",\"benchmark_eligible\":false,"
            << "\"sampling\":\"after-each-compute-1\",\"reduction\":\"meter-min-max-last\","
            << "\"frame_origin\":0,\"rate\":" << rate << ",\"requested_block_frames\":" << block
            << ",\"effective_compute_frames\":1,\"stride_frames\":" << stride << ",\"ui\":";
        ui.json(out,ni,no); out << "}\n";
    }
    void observe(int frame, const UI& ui, int frames) {
        last=ui.meter_values();
        if (frame==begin) { minimum=last; maximum=last; }
        else for (size_t i=0; i<last.size(); ++i) {
            minimum[i]=std::min(minimum[i],last[i]); maximum[i]=std::max(maximum[i],last[i]);
        }
        if (frame+1-begin < stride && frame+1!=frames) return;
        out << std::setprecision(12) << "{\"type\":\"window\",\"frame_begin\":" << begin
            << ",\"frame_end_exclusive\":" << frame+1 << ",\"last_frame\":" << frame
            << ",\"last_time_seconds\":" << double(frame)/rate << ",\"values\":[";
        for (size_t i=0; i<last.size(); ++i) {
            if (i) out << ',';
            out << "{\"min\":" << minimum[i] << ",\"max\":" << maximum[i] << ",\"last\":" << last[i] << '}';
        }
        out << "]}\n"; if (!out) throw std::runtime_error("probe output write failed");
        begin=frame+1; ++rows;
    }
    void complete(int frames) {
        out << "{\"type\":\"complete\",\"frames\":" << frames << ",\"windows\":" << rows << "}\n";
        out.close(); if (!out) throw std::runtime_error("probe output close failed");
    }
};

int main(int argc,char** argv) { try {
    const uint32_t endian=1;
    if (*reinterpret_cast<const char*>(&endian)!=1) throw std::runtime_error("requires little endian");
    auto module=std::make_unique<ModuleDSP>();
    if (argc==2 && (std::string(argv[1])=="--controls" || std::string(argv[1])=="--ui-json")) {
        module->init(48000); UI ui; module->buildUserInterface(&ui); ui.finish();
        if (std::string(argv[1])=="--ui-json") { ui.json(std::cout,module->getNumInputs(),module->getNumOutputs()); std::cout<<'\n'; return 0; }
        std::cout<<std::setprecision(9);
        std::cout<<"io\t"<<module->getNumInputs()<<"\t"<<module->getNumOutputs()<<"\n";
        for (const auto& [n,z]:ui.zones)
            std::cout<<n<<"\t"<<z.lo<<"\t"<<z.hi<<"\t"<<z.def<<"\t"<<z.boolean<<"\n";
        return 0;
    }
    if (argc<7) throw std::runtime_error("usage: runner SCORE OUT.f32 RATE BLOCK FRAMES RESERVED_ZERO [INPUT.f32] [--diagnostic --probes FILE --probe-stride N]");
    const auto r=integer(argv[3]), b=integer(argv[4]), total=integer(argv[5]);
    if (r<8000 || r>96000 || b<1 || b>8192 || total<1 || total>r*60 || integer(argv[6])!=0)
        throw std::runtime_error("invalid dimensions");
    std::string inputFile, probeFile; bool diagnostic=false; long long stride=1; std::set<std::string> options;
    for (int i=7; i<argc; ++i) {
        const std::string arg=argv[i];
        if (arg.rfind("--",0)!=0 && inputFile.empty()) { inputFile=arg; continue; }
        if (!options.insert(arg).second) throw std::runtime_error("duplicate option: "+arg);
        if (arg=="--diagnostic") diagnostic=true;
        else if (arg=="--probes" && i+1<argc) probeFile=argv[++i];
        else if (arg=="--probe-stride" && i+1<argc) stride=integer(argv[++i]);
        else throw std::runtime_error("invalid option: "+arg);
    }
    if (stride<1 || stride>96000 || (options.count("--probe-stride") && probeFile.empty()))
        throw std::runtime_error("invalid probe stride/capture options");
    const int rate=int(r), block=int(b), frames=int(total);
    module->init(rate); UI ui; module->buildUserInterface(&ui); ui.finish();
    if ((diagnosticBuild && !diagnostic) || (!diagnosticBuild && (diagnostic || !probeFile.empty() || ui.probe_count())))
        throw std::runtime_error("diagnostic probes require a dedicated FAUST_EXPR_DIAGNOSTIC build and --diagnostic; not benchmark/release eligible");
    if (!probeFile.empty() && ui.meter_catalog().empty()) throw std::runtime_error("no read-only meters to capture");
    // Refuse path aliases before opening outputs (including symlinks/hardlinks).
    const std::vector<std::string> paths={argv[1],argv[2],inputFile,probeFile};
    for (size_t i=0;i<paths.size();++i) for (size_t j=i+1;j<paths.size();++j) {
        if (paths[i].empty() || paths[j].empty()) continue;
        if (std::filesystem::weakly_canonical(paths[i])==std::filesystem::weakly_canonical(paths[j]) ||
            (std::filesystem::exists(paths[i]) && std::filesystem::exists(paths[j]) && std::filesystem::equivalent(paths[i],paths[j])))
            throw std::runtime_error("score, input, audio and probe output paths must differ");
    }
    const int ni=module->getNumInputs(), no=module->getNumOutputs();
    // Eight bounded channels allow typed control-lane probes; not voice allocation.
    if (ni<0 || ni>8 || no<1 || no>8 || (ni!=0)!=(!inputFile.empty())) throw std::runtime_error("I/O contract");
    // Bound total capture work/file size; stride does not conceal per-sample cost.
    if (!probeFile.empty() && static_cast<long long>(ui.meter_catalog().size())*frames>20000000)
        throw std::runtime_error("probe capture exceeds 20000000 meter observations; shorten the diagnostic render");
    std::ifstream score(argv[1]); if (!score) throw std::runtime_error("score open failed");
    std::vector<Event> events; std::set<std::pair<int,std::string>> keys;
    std::string line; int previous=-1;
    while (std::getline(score,line)) {
        if (line.empty() || line[0]=='#') continue;
        std::istringstream in(line); std::string f,n,v,extra;
        if (!(in>>f>>n>>v) || (in>>extra)) throw std::runtime_error("bad score row");
        auto frame=integer(f); float value=real(v);
        if (frame<previous || frame<0 || frame>=frames || events.size()>=100000)
            throw std::runtime_error("event ordering/range");
        ui.validate(n,value); n=ui.controlPath(n);
        if (!keys.emplace(int(frame),n).second) throw std::runtime_error("duplicate event");
        events.push_back({int(frame),n,value}); previous=int(frame);
    }
    std::vector<float> input(size_t(frames)*ni), output(size_t(frames)*no);
    if (ni) {
        std::ifstream f(inputFile,std::ios::binary|std::ios::ate);
        if (!f || f.tellg()!=std::streamoff(input.size()*sizeof(float))) throw std::runtime_error("input size");
        f.seekg(0); f.read(reinterpret_cast<char*>(input.data()),std::streamsize(input.size()*sizeof(float)));
        if (!f || !std::all_of(input.begin(),input.end(),[](float x){return std::isfinite(x);})) throw std::runtime_error("invalid input");
    }
    std::unique_ptr<ProbeCapture> capture;
    if (!probeFile.empty()) capture=std::make_unique<ProbeCapture>(probeFile,ui,rate,block,int(stride),ni,no);
    std::vector<std::vector<float>> ins(ni,std::vector<float>(block)), outs(no,std::vector<float>(block));
    std::vector<float*> ip(ni),op(no);
    for (int c=0;c<ni;++c) ip[c]=ins[c].data(); for (int c=0;c<no;++c) op[c]=outs[c].data();
    size_t event=0; double peak=0; long long computeNs=0; int calls=0;
    for (int n=0;n<frames;) {
        while (event<events.size() && events[event].frame==n) { ui.set(events[event].name,events[event].value); ++event; }
        int count=std::min(capture?1:block,frames-n);
        if (event<events.size()) count=std::min(count,events[event].frame-n);
        if (count<=0) throw std::runtime_error("nonprogressing score");
        for (int c=0;c<ni;++c) for (int j=0;j<count;++j) ins[c][j]=input[size_t(n+j)*ni+c];
        if (diagnosticBuild) module->compute(count,ni?ip.data():nullptr,op.data());
        else {
            const auto start=std::chrono::steady_clock::now();
            module->compute(count,ni?ip.data():nullptr,op.data());
            computeNs+=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count();
        }
        ++calls;
        for (int j=0;j<count;++j) for (int c=0;c<no;++c) {
            float x=outs[c][j]; if (!std::isfinite(x)) throw std::runtime_error("nonfinite output");
            peak=std::max(peak,double(std::abs(x))); output[size_t(n+j)*no+c]=x;
        }
        if (capture) capture->observe(n,ui,frames);
        n+=count;
    }
    std::ofstream out(argv[2],std::ios::binary); out.write(reinterpret_cast<const char*>(output.data()),std::streamsize(output.size()*sizeof(float))); out.close();
    if (!out) throw std::runtime_error("output write failed");
    if (capture) capture->complete(frames);
    std::cout<<std::setprecision(12)<<"{\"frames\":"<<frames<<",\"rate\":"<<rate<<",\"channels\":"<<no
        <<",\"block\":"<<block<<",\"compute_calls\":"<<calls<<",\"peak\":"<<peak<<",\"instrumented_compute_ns\":";
    if (diagnosticBuild) std::cout<<"null"; else std::cout<<computeNs;
    std::cout<<",\"mode\":\""<<(diagnosticBuild?"diagnostic":"clean")<<"\",\"benchmark_eligible\":"<<(diagnosticBuild?"false":"true")
        <<",\"probe_windows\":"<<(capture?capture->rows:0)<<"}\n";
    return 0;
} catch (const std::exception& e) { std::cerr<<e.what()<<"\n"; return 1; } }
