#pragma once
// Fixed execution boundaries, not a new heuristic compiler partitioner.
// Source changes/internal connections alter one unit. External wiring does not.
#include <map>
#include <set>
#include <limits>
#include <iomanip>
#include <sstream>
#include <dlfcn.h>
#include <functional>
#include <faust/gui/meta.h>
#include "retained_units.generated.h"

namespace groups {
using curlop::GraphState;
using curlop::ModuleEntry;
using curlop::EdgeEntry;
using RGraph=retained_units::Graph;
using RPlan=retained_units::Plan;
inline void check(bool value,const std::string& reason){if(!value)throw std::runtime_error(reason);}
inline std::string number(float value){std::ostringstream o;o<<std::setprecision(std::numeric_limits<float>::max_digits10)<<std::scientific<<value;return o.str();}
inline std::string fingerprint(const std::string& text){
    std::uint64_t value=14695981039346656037ull;
    for(unsigned char c:text){value^=c;value*=1099511628211ull;}
    std::ostringstream o;o<<std::hex<<std::setw(16)<<std::setfill('0')<<value;return o.str();
}
inline std::array<float,2> gains(const curlop::EdgeEffective& e){
    float gain=e.audible?std::clamp(e.gain,0.f,2.f):0.f;
    if(e.feedbackBoundary==curlop::FeedbackBoundary::OneSample)return {gain,gain};
    if(e.signalDescriptor.layout()&&*e.signalDescriptor.layout()=="stereo"){
        float a=(std::clamp(e.pan,-1.f,1.f)+1.f)*.25f*3.14159265358979323846f;
        return {std::cos(a)*gain,std::sin(a)*gain};
    }
    return {gain,gain};
}
struct Boundary {int first=0,last=0; bool parallel=false; std::vector<int> members;};
struct Layout {std::vector<Boundary> units;std::map<int,int> owner;};
inline Layout layout(int count,int width,bool parallel){
    check(count>0&&width>0&&count%width==0,"invalid fixed group shape");
    Layout l;
    for(int start=1;start<=count;start+=width){
        Boundary b;b.first=start;b.last=start+width-1;b.parallel=parallel;
        for(int i=start;i<=b.last;++i){b.members.push_back(i);l.owner[i]=start;}
        l.units.push_back(b);
    }return l;
}
inline const ModuleEntry& entry(const GraphState& g,int i){
    for(auto& m:g.modules())if(m.index==i)return m;
    throw std::runtime_error("missing member");
}
inline std::string selected(int i,int channel){return "(v"+std::to_string(i)+(channel==0?" : _,!)":" : !,_)");}
inline ModuleEntry compileUnit(const GraphState& g,const Boundary& b){
    auto edges=g.computeEffectiveEdges(); std::set<int> members(b.members.begin(),b.members.end());
    auto unit=entry(g,b.first);unit.moduleId="stable-group-"+std::to_string(b.first);
    unit.dslName="group"+std::to_string(b.first);unit.code.clear();unit.params.clear();unit.paramValues.clear();
    std::ostringstream declarations,body;std::vector<const curlop::EdgeEffective*> feedback;
    for(auto& e:edges)if(members.count(e.srcIndex)&&members.count(e.tgtIndex)){
        check(!e.modulation,"group control route unsupported");
        if(e.feedbackBoundary==curlop::FeedbackBoundary::OneSample)feedback.push_back(&e);
        else check(e.srcIndex<e.tgtIndex,"only forward internal DAG edges supported in this slice");
    }
    check(feedback.size()<=1,"multiple local feedback routes not supported");
    if(!feedback.empty())check(!b.parallel&&feedback[0]->srcIndex==b.last&&feedback[0]->tgtIndex==b.first,
                               "feedback must close the fixed group, not cross its boundary");
    for(int id:b.members){
        auto& m=entry(g,id);check(m.lineageId=="core.faust_jit"&&m.audioInputs.size()==1&&m.audioOutputs.size()==1,"invalid group member domain");
        auto source=m.code;
        for(auto p:m.params){
            std::string oldLabel="\""+(p.sourceId.empty()?p.name:p.sourceId);
            auto at=source.find(oldLabel);
            check(at!=std::string::npos&&source.find(oldLabel,at+1)==std::string::npos,"ambiguous parameter label; no textual guessing");
            std::string newLabel="m"+std::to_string(id)+"_"+p.name;
            source.replace(at,oldLabel.size(),"\""+newLabel);
            auto value=m.paramValues.find(p.name);float v=value==m.paramValues.end()?p.defaultValue:value->second;
            p.name=newLabel;p.sourceId=newLabel;unit.params.push_back(p);unit.paramValues[newLabel]=v;
        }
        declarations<<"e"<<id<<" = environment {\n"<<source<<"\n};\n";
        std::array<std::string,2> input={"0.0","0.0"};
        // Exposed input is fixed, even when no external cable is connected.
        if(b.parallel||id==b.first)input={"x","y"};
        if(!feedback.empty()&&id==b.first){input[0]+="+fbx";input[1]+="+fby";}
        for(auto& e:edges)if(e.tgtIndex==id&&members.count(e.srcIndex)&&e.feedbackBoundary!=curlop::FeedbackBoundary::OneSample){
            auto gain=gains(e);
            for(int c=0;c<2;++c)input[c]+="+("+selected(e.srcIndex,c)+"*"+number(gain[c])+")";
        }
        body<<"v"<<id<<" = e"<<id<<".process("<<input[0]<<","<<input[1]<<");\n";
    }
    std::string result="v"+std::to_string(b.last);
    if(b.parallel){
        std::array<std::string,2> sums={"0.0","0.0"};
        for(int id:b.members)for(int c=0;c<2;++c)sums[c]+="+"+selected(id,c);
        result="("+sums[0]+"),("+sums[1]+")";
    }
    std::ostringstream code;code<<declarations.str();
    if(feedback.empty())code<<"process(x,y) = "<<result<<" with {\n"<<body.str()<<"};\n";
    else {
        auto gain=gains(*feedback[0]);
        code<<"unit(fbx,fby,x,y) = "<<result<<" with {\n"<<body.str()<<"};\n"
            <<"process = unit ~ (*("<<number(gain[0])<<"),*("<<number(gain[1])<<"));\n";
    }
    unit.code=code.str();return unit;
}
inline GraphState collapse(const GraphState& g,const Layout& l){
    std::vector<ModuleEntry> modules;std::vector<EdgeEntry> routes;
    for(auto& m:g.modules())if(!l.owner.count(m.index))modules.push_back(m);
    for(auto& b:l.units)modules.push_back(compileUnit(g,b));
    auto mapIndex=[&](int i){auto it=l.owner.find(i);return it==l.owner.end()?i:it->second;};
    auto boundary=[&](int owner)->const Boundary&{for(auto& b:l.units)if(b.first==owner)return b;throw std::runtime_error("missing boundary");};
    using Key=std::pair<int,int>;
    std::map<Key,std::vector<curlop::EdgeEffective>> buckets;
    for(auto e:g.computeEffectiveEdges()){
        int s=mapIndex(e.srcIndex),t=mapIndex(e.tgtIndex);
        if(s==t){check(l.owner.count(e.srcIndex),"standalone self edge unsupported");continue;}
        if(l.owner.count(e.srcIndex)){auto& b=boundary(s);check(b.parallel||e.srcIndex==b.last,"connection exits a hidden internal port");}
        if(l.owner.count(e.tgtIndex)){auto& b=boundary(t);check(b.parallel||e.tgtIndex==b.first,"connection enters a hidden internal port");}
        buckets[{s,t}].push_back(e);
    }
    for(auto& pair:buckets){
        auto k=pair.first;auto& old=pair.second;auto e=old.front();
        std::size_t expected=1;
        if(l.owner.count(e.tgtIndex)&&boundary(k.second).parallel)expected=boundary(k.second).members.size();
        if(l.owner.count(e.srcIndex)&&boundary(k.first).parallel)expected=boundary(k.first).members.size();
        check(old.size()==expected,"partial fan-in/out cannot be represented by this fixed group port");
        for(auto& x:old)check(x.gain==e.gain&&x.pan==e.pan&&x.audible==e.audible&&x.feedbackBoundary==e.feedbackBoundary,"incompatible boundary wire attributes");
        EdgeEntry route;route.srcIndex=k.first;route.tgtIndex=k.second;
        route.srcPort="OUT";route.tgtPort="IN";route.signalDescriptor=e.signalDescriptor;
        route.feedbackBoundary=e.feedbackBoundary;route.modulation=e.modulation;
        route.gain=e.gain;route.pan=e.pan;route.muted=!e.audible;route.audible=e.audible;
        routes.push_back(std::move(route));
    }
    GraphState out;check(out.replace(std::move(modules),std::move(routes),{}),"collapsed graph rejected");return out;
}
// Source identity is checked before loading. Libraries are retained by each
// Node's externalOwner and cannot disappear while a running instance uses them.
class NativeLibrary {
    void* handle_=nullptr;
public:
    using Create=::dsp*(*)();Create create=nullptr;
    explicit NativeLibrary(const std::filesystem::path& file){
        handle_=dlopen(file.c_str(),RTLD_NOW|RTLD_LOCAL);
        check(handle_!=nullptr,"native library load failed: "+file.filename().string());
        create=reinterpret_cast<Create>(dlsym(handle_,"stable_group_create"));
        if(!create){dlclose(handle_);handle_=nullptr;throw std::runtime_error("native factory symbol missing");}
    }
    ~NativeLibrary(){if(handle_)dlclose(handle_);}
    NativeLibrary(const NativeLibrary&)=delete;
};
class NativeFactories {
    std::filesystem::path root_;std::string variant_;
    std::map<std::string,std::shared_ptr<NativeLibrary>> libraries_;
public:
    NativeFactories(std::filesystem::path root,std::string variant):root_(std::move(root)),variant_(std::move(variant)){}
    retained_units::ExternalDSP create(const ModuleEntry& m){
        auto key=fingerprint(m.code);auto& library=libraries_[key];
        if(!library){
            std::ifstream f(root_/"sources"/(key+".dsp"));std::ostringstream source;source<<f.rdbuf();
            check(bool(f)&&source.str()==m.code,"native factory source identity mismatch");
            library=std::make_shared<NativeLibrary>(root_/variant_/(key+".dylib"));
        }
        return {library,std::unique_ptr<::dsp>(library->create())};
    }
};
} // namespace groups
