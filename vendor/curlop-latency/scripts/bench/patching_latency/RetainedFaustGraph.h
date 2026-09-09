#pragma once
// Experimental authoring backend, not installed as a product fallback.
// Existing FaustRuntime owns code; existing PreparedSignalSchedule owns causal
// order. Plans retain running Faust instances instead of reinitialising them.
#include "modules/backend/FaustRuntime.h"
#include "graph/state/GraphState.h"
#include "graph/transport/PreparedSignalSchedule.h"
#include "graph/transport/FeedbackBoundaryState.h"
#include <faust/dsp/dsp.h>
#include <faust/gui/MapUI.h>
#include <array>
#include <algorithm>
#include <cmath>
#include <mutex>
#include <stdexcept>
#include <memory>
#include <sstream>
#include <unordered_map>

namespace retained {
using namespace curlop;
using namespace curlop::transport;
inline void check(bool b,const std::string& s){if(!b)throw std::runtime_error(s);}

// All Node construction/destruction is on the preparation owner, never callback.
struct Node {
    FaustRuntime::FactoryPtr factory; // Must outlive instance.
    std::unique_ptr<::dsp> instance;
    MapUI ui;
    std::string id, source, contract;
    int index=-1, inputs=0;
    bool output=false;
    std::array<std::vector<float>,2> in,out;
    Node(int frames){for(auto* bank:{&in,&out})for(auto& c:*bank)c.resize(frames);}
};
struct Route {
    int source=-1,target=-1;
    std::array<float,2> gains{};
    std::shared_ptr<FeedbackBoundaryState> delay;
    std::string key;
};
struct Plan {
    std::vector<std::shared_ptr<Node>> nodes;
    std::unique_ptr<PreparedSignalSchedule> schedule;
    std::vector<Route> routes;
    std::vector<std::vector<std::size_t>> incoming;
    std::vector<std::pair<float*,float>> controls;
    int maxFrames=0, output=-1;
    std::size_t created=0,reused=0,acquired=0;
    bool singleSample=false;
    float outputLevel=1;
    std::uint64_t revision=0;
    const void* owner=nullptr;
};
class Graph {
    FaustRuntime runtime_;
    int sampleRate_,maxFrames_;
    static std::string signature(const ModuleEntry& m){
        std::ostringstream s;s<<m.lineageId<<':'<<m.audioInputs.size()<<':'<<m.audioOutputs.size();
        for(auto& p:m.params)s<<'|'<<p.name<<':'<<p.sourceId<<':'<<p.min<<':'<<p.max;
        return s.str();
    }
public:
    Graph(int sr,int frames,const std::string& store):sampleRate_(sr),maxFrames_(frames){
        check(sr>0&&frames>0,"invalid preparation rate/block size");
        runtime_.forceBackend(FaustRuntime::Backend::Jit);runtime_.setStoreDirectory(store);
    }
    std::unique_ptr<Plan> prepare(const GraphState& g,const Plan* previous,std::uint64_t revision){
        check(!previous||previous->owner==this,"plan belongs to another renderer lifetime");
        auto p=std::make_unique<Plan>();p->maxFrames=maxFrames_;p->revision=revision;p->owner=this;
        int maxIndex=-1;for(auto& m:g.modules())maxIndex=std::max(maxIndex,m.index);
        check(maxIndex>=0&&maxIndex<256,"bounded authoring graph index capacity exceeded");
        p->nodes.resize(maxIndex+1);p->incoming.resize(maxIndex+1);
        for(auto& m:g.modules()){
            check(m.index>=0&&!p->nodes[m.index],"invalid/duplicate module index");
            check((m.lineageId=="core.faust_jit"||m.lineageId=="core.output")&&
                m.physicalVoices<=1&&m.processingMode==ModuleProcessingMode::Native&&
                m.oversamplingFactor==ModuleOversamplingFactor::X1&&
                m.controlInputs.empty()&&m.controlOutputs.empty()&&
                m.authoredParamValues.empty()&&m.visualInputs.empty()&&m.visualOutputs.empty()&&
                m.audioInputs.size()<=1&&m.audioOutputs.size()<=1,
                "unsupported module domain/ports/polyphony; no fallback");
            for(const auto* ports:{&m.signalInputs,&m.signalOutputs})
                for(const auto& port:*ports)check(port.descriptor.width()==2&&port.descriptor.rate()==SignalRate::Audio,
                    "non-stereo canonical port unsupported");
            auto contract=signature(m);std::shared_ptr<Node> node;
            if(previous&&std::size_t(m.index)<previous->nodes.size()){
                auto old=previous->nodes[m.index];
                if(old&&old->id==m.moduleId&&old->source==m.code&&old->contract==contract)node=old;
            }
            if(node){++p->reused;}else{
                node=std::make_shared<Node>(maxFrames_);node->id=m.moduleId;
                node->source=m.code;node->contract=contract;node->index=m.index;
                node->output=m.lineageId=="core.output";
                if(!node->output){
                    std::string error;node->factory=runtime_.acquire(m.dslName,m.code,error);++p->acquired;
                    check(bool(node->factory),"Faust compilation failed: "+error);
                    // Use the established global compiler/lifecycle lock.
                    std::lock_guard<std::mutex> lock(FaustRuntime::compileMutex());
                    node->instance.reset(node->factory->raw()->createDSPInstance());
                    check(bool(node->instance),"no DSP instance");
                    node->instance->init(sampleRate_);node->instance->buildUserInterface(&node->ui);
                    node->inputs=node->instance->getNumInputs();
                    check((node->inputs==0||node->inputs==2)&&node->instance->getNumOutputs()==2,
                          "supported cut is stereo synth/effect only");
                }else node->inputs=2;
                ++p->created;
            }
            p->nodes[m.index]=node;
            if(node->output){
                check(p->output<0,"multiple output endpoints unsupported");p->output=m.index;
                auto found=m.paramValues.find("LEVEL");p->outputLevel=found==m.paramValues.end()?1:found->second;
                check(std::isfinite(p->outputLevel),"nonfinite output level");
            }else{
                for(auto& entry:m.params){
                    auto* zone=node->ui.getParamZone(entry.sourceId.empty()?entry.name:entry.sourceId);
                    check(zone!=nullptr,"parameter zone not found: "+entry.name);
                    auto found=m.paramValues.find(entry.name);float v=found==m.paramValues.end()?entry.defaultValue:found->second;
                    check(std::isfinite(v)&&std::isfinite(entry.min)&&std::isfinite(entry.max)&&entry.min<=entry.max,
                          "invalid parameter");
                    p->controls.push_back({zone,std::clamp(v,entry.min,entry.max)});
                }
            }
        }
        check(p->output>=0,"no output");
        std::vector<PreparedSignalRouteDefinition> defs;
        for(auto& e:g.computeEffectiveEdges()){
            check(e.srcIndex>=0&&e.tgtIndex>=0&&e.srcIndex<=maxIndex&&e.tgtIndex<=maxIndex&&
                p->nodes[e.srcIndex]&&p->nodes[e.tgtIndex]&&!p->nodes[e.srcIndex]->output&&
                p->nodes[e.tgtIndex]->inputs==2&&(e.srcPort.empty()||e.srcPort=="OUT")&&
                (e.tgtPort.empty()||e.tgtPort=="IN")&&
                !e.modulation&&e.signalDescriptor.width()==2&&
                e.signalDescriptor.rate()==SignalRate::Audio&&std::isfinite(e.gain)&&std::isfinite(e.pan),
                "unsupported route/descriptor; no fallback");
            Route r;r.source=e.srcIndex;r.target=e.tgtIndex;
            r.key=p->nodes[r.source]->id+"\n"+p->nodes[r.target]->id;
            float gain=e.audible?std::clamp(e.gain,0.0f,2.0f):0.0f;
            r.gains={gain,gain};
            const bool feedback=e.feedbackBoundary==FeedbackBoundary::OneSample;
            if(feedback){
                if(previous)for(auto& old:previous->routes)if(old.delay&&old.key==r.key)r.delay=old.delay;
                if(!r.delay)r.delay=std::make_shared<FeedbackBoundaryState>(e.signalDescriptor);
                p->singleSample=true;
            }else if(e.signalDescriptor.layout()&&*e.signalDescriptor.layout()=="stereo"){
                float angle=(std::clamp(e.pan,-1.0f,1.0f)+1.0f)*.25f*3.14159265358979323846f;
                r.gains={std::cos(angle)*gain,std::sin(angle)*gain};
            }
            PreparedSignalRouteDefinition d;d.sourceGraphIdx=r.source;d.destinationGraphIdx=r.target;
            d.sourceChannel=0;d.destinationChannel=0;d.descriptor=e.signalDescriptor;d.feedbackBoundary=feedback;
            defs.push_back(d);p->incoming[r.target].push_back(p->routes.size());p->routes.push_back(std::move(r));
        }
        p->schedule=PreparedSignalSchedule::build(maxIndex+1,std::move(defs),maxFrames_);
        check(p->schedule&&!p->schedule->hasDeferredCycles()&&p->schedule->unsupportedRouteCount()==0,
              "invalid causal schedule; undeclared cycle");
        return p;
    }
    // Sole audio owner. No allocation, locks, ownership transitions, or init.
    static bool render(const Plan& p,juce::AudioBuffer<float>& host) noexcept {
        const int frames=host.getNumSamples();if(frames<=0||frames>p.maxFrames||host.getNumChannels()!=2)return false;
        for(auto& setting:p.controls)*setting.first=setting.second;
        const int quantum=p.singleSample?1:frames;
        for(int offset=0;offset<frames;offset+=quantum){
            for(int idx:p.schedule->preparedModuleOrder()){
                auto& holder=p.nodes[idx];if(!holder)continue;auto& n=*holder;
                for(auto& channel:n.in)std::fill_n(channel.data()+offset,quantum,0.f);
                for(auto routeIndex:p.incoming[idx]){
                    auto& r=p.routes[routeIndex];auto& src=*p.nodes[r.source];
                    std::array<float,2> prior{};if(r.delay&&!r.delay->readFrame(prior.data(),2))return false;
                    for(int c=0;c<2;++c)for(int s=0;s<quantum;++s){
                        float x=r.delay?prior[c]:src.out[c][offset+s];
                        n.in[c][offset+s]+=x*r.gains[c];
                    }
                }
                if(n.output){
                    for(int c=0;c<2;++c)for(int s=0;s<quantum;++s)n.out[c][offset+s]=n.in[c][offset+s]*p.outputLevel;
                }else{
                    float* inputs[]={n.in[0].data()+offset,n.in[1].data()+offset};
                    float* outputs[]={n.out[0].data()+offset,n.out[1].data()+offset};
                    n.instance->compute(quantum,n.inputs?inputs:nullptr,outputs);
                }
            }
            for(auto& r:p.routes)if(r.delay){
                auto& src=*p.nodes[r.source];float current[]={src.out[0][offset],src.out[1][offset]};
                if(!r.delay->commitFrame(current,2))return false;
            }
        }
        for(int c=0;c<2;++c)std::copy_n(p.nodes[p.output]->out[c].data(),frames,host.getWritePointer(c));
        return true;
    }
};
} // namespace retained
