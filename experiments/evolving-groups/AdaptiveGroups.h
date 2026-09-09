#pragma once
// Host boundary policy layered on #34's compiler and #33's retainer.
// It does not replace Faust's arithmetic optimizer or choose a new worker pool.
#include "stable_dynamic.generated.h"
#include <unordered_set>

namespace adaptive {
using namespace groups;
using Edge=curlop::EdgeEffective;
struct Pins {std::set<int> inputs,outputs;};
struct View {
    const GraphState& graph;
    std::map<int,const ModuleEntry*> modules;
    std::vector<Edge> edges;
    std::vector<int> order;
    explicit View(const GraphState& g,int frames):graph(g),edges(g.computeEffectiveEdges()){
        int max=-1;std::set<std::string> ids;
        for(auto& m:g.modules()){
            check(m.index>=0&&m.index<256&&!modules.count(m.index)&&!m.moduleId.empty()&&ids.insert(m.moduleId).second,"invalid stable module identity");
            check((m.lineageId=="core.faust_jit"||m.lineageId=="core.output")&&m.physicalVoices<=1&&
                m.processingMode==curlop::ModuleProcessingMode::Native&&m.oversamplingFactor==curlop::ModuleOversamplingFactor::X1&&
                m.controlInputs.empty()&&m.controlOutputs.empty()&&m.authoredParamValues.empty()&&
                m.visualInputs.empty()&&m.visualOutputs.empty()&&m.audioInputs.size()<=1&&m.audioOutputs.size()<=1,
                "unsupported module domain/polyphony");
            for(const auto* ports:{&m.signalInputs,&m.signalOutputs})for(const auto& port:*ports)
                check(port.descriptor.width()==2&&port.descriptor.rate()==curlop::transport::SignalRate::Audio,"unsupported canonical port");
            modules[m.index]=&m;max=std::max(max,m.index);
        }
        std::vector<curlop::transport::PreparedSignalRouteDefinition> routes;
        for(auto& e:edges){
            check(modules.count(e.srcIndex)&&modules.count(e.tgtIndex),"dangling route");
            check(!e.modulation&&e.signalDescriptor.width()==2&&e.signalDescriptor.rate()==curlop::transport::SignalRate::Audio,"unsupported route domain");
            check(std::isfinite(e.gain)&&std::isfinite(e.pan),"invalid wire value");
            curlop::transport::PreparedSignalRouteDefinition r;r.sourceGraphIdx=e.srcIndex;r.destinationGraphIdx=e.tgtIndex;
            r.descriptor=e.signalDescriptor;r.feedbackBoundary=e.feedbackBoundary==curlop::FeedbackBoundary::OneSample;routes.push_back(r);
        }
        auto schedule=curlop::transport::PreparedSignalSchedule::build(max+1,std::move(routes),frames);
        check(schedule&&!schedule->hasDeferredCycles()&&!schedule->unsupportedRouteCount(),"undeclared cycle or unsupported schedule");
        for(int i:schedule->preparedModuleOrder())if(modules.count(i))order.push_back(i);
    }
    bool effect(int id)const{
        auto& m=*modules.at(id);
        return m.lineageId=="core.faust_jit"&&m.audioInputs.size()==1&&m.audioOutputs.size()==1;
    }
    std::vector<int> adjacent(int id,bool incoming,bool delayed=false)const{
        std::vector<int> r;for(auto& e:edges)if((e.feedbackBoundary==curlop::FeedbackBoundary::OneSample)==delayed&&
            (incoming?e.tgtIndex:e.srcIndex)==id)r.push_back(incoming?e.srcIndex:e.tgtIndex);return r;
    }
};
inline Boundary boundary(std::vector<int> members,bool parallel=false){
    check(!members.empty(),"empty boundary");Boundary b;b.first=members.front();b.last=members.back();b.members=std::move(members);b.parallel=parallel;return b;
}
inline void insert(Layout& l,const Boundary& b){
    if(b.members.size()<2)return;
    for(int id:b.members)check(l.owner.emplace(id,b.first).second,"duplicate grouped member");l.units.push_back(b);
}
inline std::string edgeKey(const Edge& e,bool includeEndpoints=true){
    std::ostringstream s;if(includeEndpoints)s<<e.srcIndex<<','<<e.tgtIndex<<';';
    s<<number(e.gain)<<','<<number(e.pan)<<','<<e.audible<<','<<int(e.feedbackBoundary)<<','<<e.srcPort<<','<<e.tgtPort;return s.str();
}
inline std::string parallelKey(const View& v,int id){
    std::vector<std::string> pieces;
    for(auto& e:v.edges)if(e.srcIndex==id||e.tgtIndex==id){
        std::string direction=e.tgtIndex==id?"in":"out";
        pieces.push_back(direction+std::to_string(e.tgtIndex==id?e.srcIndex:e.tgtIndex)+":"+edgeKey(e,false));
    }
    std::sort(pieces.begin(),pieces.end());std::string s;for(auto& x:pieces)s+=std::to_string(x.size())+":"+x;return s;
}
inline Layout seed(const View& v,int width){
    check(width>=2&&width<=16,"invalid target group bound");Layout l;std::set<int> used;
    // Preserve complete simple feedback loops as compiled islands, even when
    // longer than the normal chain cap. Existing schedule validates causality.
    for(auto& e:v.edges)if(e.feedbackBoundary==curlop::FeedbackBoundary::OneSample){
        std::vector<int> path;std::set<int> seen;int at=e.tgtIndex;
        while(v.modules.count(at)&&v.effect(at)&&!used.count(at)&&seen.insert(at).second&&path.size()<64){
            path.push_back(at);if(at==e.srcIndex)break;auto next=v.adjacent(at,false);
            if(next.size()!=1)break;at=next[0];
        }
        if(path.size()>1&&path.back()==e.srcIndex){
            std::set<int> inside(path.begin(),path.end());bool legal=true;
            for(auto& x:v.edges){bool a=inside.count(x.srcIndex),b=inside.count(x.tgtIndex);
                if(a&&!b&&x.srcIndex!=path.back())legal=false;
                if(!a&&b&&x.tgtIndex!=path.front())legal=false;
                if(a&&b&&x.feedbackBoundary==curlop::FeedbackBoundary::OneSample&&(&x!=&e))legal=false;
            }
            if(legal){insert(l,boundary(path));used.insert(path.begin(),path.end());}
        }
    }
    // Independent siblings with exactly the same external port/wire contract.
    std::map<std::string,std::vector<int>> siblings;
    for(int id:v.order)if(v.effect(id)&&!used.count(id)&&v.adjacent(id,true,true).empty()&&v.adjacent(id,false,true).empty())
        siblings[parallelKey(v,id)].push_back(id);
    for(auto& pair:siblings)if(pair.second.size()>1){
        auto& ids=pair.second;for(std::size_t i=0;i<ids.size();i+=width){auto end=std::min(ids.size(),i+std::size_t(width));
            if(end-i>1){std::vector<int> chunk(ids.begin()+i,ids.begin()+end);insert(l,boundary(chunk,true));used.insert(chunk.begin(),chunk.end());}}
    }
    for(int start:v.order)if(v.effect(start)&&!used.count(start)){
        std::vector<int> members;int at=start;
        while(v.effect(at)&&!used.count(at)&&members.size()<std::size_t(width)){
            members.push_back(at);used.insert(at);auto next=v.adjacent(at,false);
            if(next.size()!=1||!v.modules.count(next[0])||!v.effect(next[0])||v.adjacent(next[0],true).size()!=1||
               !v.adjacent(at,false,true).empty()||!v.adjacent(next[0],true,true).empty())break;
            at=next[0];
        }insert(l,boundary(members));
    }return l;
}
inline Layout maintain(const View& v,const Layout& prior,const Pins& pins){
    for(auto* set:{&pins.inputs,&pins.outputs})for(int id:*set)check(v.modules.count(id)&&v.effect(id),"pin targets absent or unsupported module");
    std::vector<Boundary> pending;
    for(auto& old:prior.units){std::vector<int> members;for(int id:old.members)if(v.modules.count(id)&&v.effect(id))members.push_back(id);
        if(members.size()>1)pending.push_back(boundary(members,old.parallel));}
    // Monotone splitting: an edit never silently recompacts adjacent groups.
    // New nodes stay individual until an explicit replan/compact operation.
    for(int iteration=0;iteration<256;++iteration){
        bool changed=false;std::vector<Boundary> next;
        for(auto& b:pending){
            std::set<int> inside(b.members.begin(),b.members.end());
            if(b.parallel){
                std::map<std::string,std::vector<int>> classes;
                for(int id:b.members){auto k=parallelKey(v,id);
                    if(pins.inputs.count(id)||pins.outputs.count(id))k+="/pinned/"+std::to_string(id);
                    classes[k].push_back(id);}
                if(classes.size()>1){changed=true;for(auto& c:classes)if(c.second.size()>1)next.push_back(boundary(c.second,true));}
                else next.push_back(b);continue;
            }
            std::set<std::size_t> cuts{0,b.members.size()};
            auto pos=[&](int id){return std::size_t(std::find(b.members.begin(),b.members.end(),id)-b.members.begin());};
            for(int id:b.members){if(pins.inputs.count(id))cuts.insert(pos(id));if(pins.outputs.count(id))cuts.insert(pos(id)+1);}
            for(auto& e:v.edges){bool a=inside.count(e.srcIndex),c=inside.count(e.tgtIndex);
                if(a&&!c)cuts.insert(pos(e.srcIndex)+1);
                if(!a&&c)cuts.insert(pos(e.tgtIndex));
                // A formerly compiled island may no longer have the supported
                // single-entry/single-exit cycle. Split it, don't lose an edge.
                if(a&&c&&((e.feedbackBoundary==curlop::FeedbackBoundary::OneSample&&(e.srcIndex!=b.last||e.tgtIndex!=b.first))||
                   (e.feedbackBoundary!=curlop::FeedbackBoundary::OneSample&&pos(e.srcIndex)>=pos(e.tgtIndex))))
                    for(std::size_t i=1;i<b.members.size();++i)cuts.insert(i);
            }
            if(cuts.size()==2)next.push_back(b);
            else {changed=true;auto a=cuts.begin(),c=std::next(a);for(;c!=cuts.end();++a,++c)if(*c-*a>1)
                next.push_back(boundary(std::vector<int>(b.members.begin()+*a,b.members.begin()+*c)));}
        }
        pending=std::move(next);if(!changed){Layout result;for(auto& b:pending)insert(result,b);return result;}
    }throw std::runtime_error("boundary refinement did not converge");
}
inline int owner(const Layout& l,int id){auto it=l.owner.find(id);return it==l.owner.end()?id:it->second;}
inline std::vector<int> members(const Layout& l,int id){for(auto& b:l.units)if(b.first==id)return b.members;return {id};}
inline std::string unitKey(const View& v,const Boundary& b){
    std::ostringstream s;s<<b.parallel<<':';auto field=[&](const std::string& x){s<<x.size()<<':'<<x;};
    for(int id:b.members){auto& m=*v.modules.at(id);s<<id<<':';field(m.moduleId);field(m.lineageId);field(m.code);
        for(auto& p:m.params){field(p.name);field(p.sourceId);s<<number(p.min)<<number(p.max)<<number(p.defaultValue);}
        std::map<std::string,float> values(m.paramValues.begin(),m.paramValues.end());for(auto& p:values){field(p.first);s<<number(p.second);}}
    std::set<int> ids(b.members.begin(),b.members.end());
    // Keep the product's effective fan-in order; no floating-point reordering.
    for(auto& e:v.edges)if(ids.count(e.srcIndex)&&ids.count(e.tgtIndex))field(edgeKey(e));return s.str();
}
class SourceCache {
    std::map<std::string,ModuleEntry> units_;
public:
    std::size_t hits=0,misses=0;
    ModuleEntry compiled(const View& v,const Boundary& b){
        auto key=unitKey(v,b);auto it=units_.find(key);if(it!=units_.end()){++hits;return it->second;}
        ++misses;auto m=compileUnit(v.graph,b);
        // An authored object replaced at the same graph index is not the same
        // live state, even when its DSP source happens to be identical.
        m.code+="\n// authored identities:";for(int id:b.members){auto& text=v.modules.at(id)->moduleId;
            m.code+=' ';for(unsigned char c:text){static const char* hex="0123456789abcdef";m.code+=hex[c>>4];m.code+=hex[c&15];}}
        m.code+='\n';if(units_.size()>=512)units_.erase(units_.begin());units_.emplace(std::move(key),m);return m;
    }
    GraphState collapseCached(const View& v,const Layout& l){
        std::vector<ModuleEntry> ms;std::vector<EdgeEntry> es;
        for(auto& m:v.graph.modules())if(!l.owner.count(m.index))ms.push_back(m);
        for(auto& b:l.units)ms.push_back(compiled(v,b));
        std::map<int,const Boundary*> boundaries;for(auto& b:l.units)boundaries[b.first]=&b;
        std::map<std::pair<int,int>,std::vector<Edge>> buckets;
        for(auto& e:v.edges){int a=owner(l,e.srcIndex),b=owner(l,e.tgtIndex);
            if(a==b&&l.owner.count(e.srcIndex)&&l.owner.count(e.tgtIndex))continue;
            if(boundaries.count(a))check(boundaries[a]->parallel||e.srcIndex==boundaries[a]->last,"hidden output remained");
            if(boundaries.count(b))check(boundaries[b]->parallel||e.tgtIndex==boundaries[b]->first,"hidden input remained");
            buckets[{a,b}].push_back(e);}
        for(auto& pair:buckets){auto& edges=pair.second;auto& e=edges.front();auto a=pair.first.first,b=pair.first.second;std::size_t count=1;
            if(boundaries.count(a)&&boundaries[a]->parallel)count=boundaries[a]->members.size();
            if(boundaries.count(b)&&boundaries[b]->parallel)count=boundaries[b]->members.size();
            check(edges.size()==count,"ambiguous grouped fan-in/out");for(auto& x:edges)check(edgeKey(x,false)==edgeKey(e,false),"mismatched grouped wire contracts");
            EdgeEntry r;r.srcIndex=a;r.tgtIndex=b;r.srcPort="OUT";r.tgtPort="IN";r.signalDescriptor=e.signalDescriptor;
            r.gain=e.gain;r.pan=e.pan;r.feedbackBoundary=e.feedbackBoundary;r.audible=e.audible;r.muted=!e.audible;es.push_back(r);}
        GraphState g;check(g.replace(std::move(ms),std::move(es),{}),"adaptive collapsed graph rejected");return g;
    }
};
inline bool sameProgram(const ModuleEntry& a,const ModuleEntry& b){
    if(a.moduleId!=b.moduleId||a.lineageId!=b.lineageId||a.code!=b.code||a.audioInputs!=b.audioInputs||a.audioOutputs!=b.audioOutputs||a.params.size()!=b.params.size())return false;
    for(std::size_t i=0;i<a.params.size();++i){auto& x=a.params[i];auto& y=b.params[i];if(x.name!=y.name||x.sourceId!=y.sourceId||x.min!=y.min||x.max!=y.max)return false;}return true;
}
struct Impact {
    std::set<int> resetMembers;std::set<int> resetFeedbackSources;
    std::size_t created=0,reused=0,acquired=0,hits=0,misses=0;
    double planningUs=0,preparationUs=0,totalUs=0;
};
class Engine {
    int width_,frames_;SourceCache cache_;
    RGraph graph_;GraphState shaped_,original_;
public:
    Layout layout;std::unique_ptr<RPlan> plan;
    explicit Engine(int width,int frames,const std::string& directory):width_(width),frames_(frames),graph_(48000,frames,directory){}
    Impact apply(const GraphState& g,const Pins& pins={},bool compact=false,bool preserveOnly=false){
        auto start=std::chrono::steady_clock::now();auto micros=[](auto a,auto b){return std::chrono::duration<double,std::micro>(b-a).count();};
        View view(g,frames_);auto nextLayout=maintain(view,(!plan||compact)?seed(view,width_):layout,pins);
        auto hits=cache_.hits,misses=cache_.misses;auto nextGraph=cache_.collapseCached(view,nextLayout);Impact impact;
        std::map<int,const ModuleEntry*> old;for(auto& m:shaped_.modules())old[m.index]=&m;
        for(auto& m:nextGraph.modules()){
            bool retain=old.count(m.index)&&sameProgram(*old[m.index],m);
            if(retain)++impact.reused;else{++impact.created;if(m.lineageId!="core.output")++impact.acquired;
                auto ids=members(nextLayout,m.index);impact.resetMembers.insert(ids.begin(),ids.end());}}
        if(plan)for(auto& e:view.edges)if(e.feedbackBoundary==curlop::FeedbackBoundary::OneSample){
            bool oldInternal=layout.owner.count(e.srcIndex)&&layout.owner.count(e.tgtIndex)&&owner(layout,e.srcIndex)==owner(layout,e.tgtIndex);
            bool newInternal=nextLayout.owner.count(e.srcIndex)&&nextLayout.owner.count(e.tgtIndex)&&owner(nextLayout,e.srcIndex)==owner(nextLayout,e.tgtIndex);
            if((oldInternal||newInternal)&&(impact.resetMembers.count(e.srcIndex)||impact.resetMembers.count(e.tgtIndex)))impact.resetFeedbackSources.insert(e.srcIndex);}
        if(plan&&preserveOnly&&!impact.resetMembers.empty())throw std::runtime_error("restart-required: active group computation/boundary changed");
        auto planned=std::chrono::steady_clock::now();auto next=graph_.prepare(nextGraph,plan.get(),plan?plan->revision+1:1);
        // Boundary history belongs to an authored cable, not a temporary group
        // index. If the cable remains external across a split, preserve its z^-1
        // object even when source/target compiled-unit indices have changed.
        if(plan)for(auto& e:view.edges)if(e.feedbackBoundary==curlop::FeedbackBoundary::OneSample){
            bool wasInternal=layout.owner.count(e.srcIndex)&&layout.owner.count(e.tgtIndex)&&owner(layout,e.srcIndex)==owner(layout,e.tgtIndex);
            bool nowInternal=nextLayout.owner.count(e.srcIndex)&&nextLayout.owner.count(e.tgtIndex)&&owner(nextLayout,e.srcIndex)==owner(nextLayout,e.tgtIndex);
            if(wasInternal||nowInternal)continue;
            bool sameCable=false;
            for(auto& oldEdge:original_.computeEffectiveEdges())if(oldEdge.srcIndex==e.srcIndex&&oldEdge.tgtIndex==e.tgtIndex&&oldEdge.srcPort==e.srcPort&&oldEdge.tgtPort==e.tgtPort&&oldEdge.feedbackBoundary==e.feedbackBoundary)
                sameCable=entry(original_,e.srcIndex).moduleId==entry(g,e.srcIndex).moduleId&&entry(original_,e.tgtIndex).moduleId==entry(g,e.tgtIndex).moduleId;
            if(sameCable)for(auto& oldRoute:plan->routes)if(oldRoute.delay&&oldRoute.source==owner(layout,e.srcIndex)&&oldRoute.target==owner(layout,e.tgtIndex))
                for(auto& newRoute:next->routes)if(newRoute.delay&&newRoute.source==owner(nextLayout,e.srcIndex)&&newRoute.target==owner(nextLayout,e.tgtIndex))newRoute.delay=oldRoute.delay;
        }
        check(next->created==impact.created&&next->reused==impact.reused&&next->acquired==impact.acquired,"runtime invalidation contradicts logical program diff");
        if(plan)for(auto& m:nextGraph.modules())if(old.count(m.index)&&sameProgram(*old[m.index],m))check(next->nodes[m.index]==plan->nodes[m.index],"unchanged unit was recreated");
        layout=std::move(nextLayout);shaped_=std::move(nextGraph);original_=g;plan.swap(next);
        auto end=std::chrono::steady_clock::now();impact.planningUs=micros(start,planned);impact.preparationUs=micros(planned,end);impact.totalUs=micros(start,end);
        impact.hits=cache_.hits-hits;impact.misses=cache_.misses-misses;return impact;
    }
    void render(juce::AudioBuffer<float>& b){check(RGraph::render(*plan,b),"adaptive rendering rejected");}
    const GraphState& shaped()const{return shaped_;}
};
} // namespace adaptive
