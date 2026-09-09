#!/usr/bin/env python3
"""Pinned #38 engine; alter only initial boundary selection, not runtime DSP."""
from pathlib import Path
import hashlib, sys
PINS = {'AdaptiveGroups.h':'2e55822d2f63ab304f2eeda497d96974f22f4fe74dd2b2ecda814d92a1d77d75',
        'adaptive_main.cpp':'180efff12ef2615c69b289af0953080f9fe4ada7d483f17e0b09302774ea007d'}
POLICY = '''// Policy uses the current graph only. No future edit targets or trace access.
// 1=individual; 0=complete simple feedback islands only; 2/4/8=#38 caps.
inline Layout policySeed(const View& v,int mode){
    check(mode==0||mode==1||mode==2||mode==4||mode==8,"invalid authoring policy");
    if(mode==1)return {};
    auto candidate=seed(v,mode==0?4:mode);
    if(mode!=0)return candidate;
    Layout loops;
    for(const auto& b:candidate.units){
        std::set<int> ids(b.members.begin(),b.members.end());bool recursive=false;
        for(const auto& e:v.edges)if(e.feedbackBoundary==curlop::FeedbackBoundary::OneSample&&ids.count(e.srcIndex)&&ids.count(e.tgtIndex))recursive=true;
        if(recursive)insert(loops,b);
    }
    return loops;
}
'''
def generate(old,out):
    text={}
    for name,want in PINS.items():
        data=(old/name).read_bytes()
        if hashlib.sha256(data).hexdigest()!=want:raise ValueError('prior source changed: '+name)
        text[name]=data.decode()
    s=text['AdaptiveGroups.h']; anchor='seed(view,width_)'
    if s.count(anchor)!=1 or s.count('class Engine {')!=1:raise ValueError('policy seam changed')
    s=s.replace('class Engine {',POLICY+'class Engine {').replace(anchor,'policySeed(view,width_)')
    out.mkdir(parents=True,exist_ok=True);(out/'AdaptivePolicy.generated.h').write_text(s)
    s=text['adaptive_main.cpp']
    if s.count('int main(int argc,char** argv){')!=1 or s.count('#include "AdaptiveGroups.h"')!=1:raise ValueError('entry seam changed')
    s=s.replace('int main(int argc,char** argv){','int priorAdaptiveEntrypoint(int argc,char** argv){').replace('#include "AdaptiveGroups.h"','#include "AdaptivePolicy.generated.h"')
    (out/'policy_support.generated.h').write_text(s)
if __name__=='__main__':generate(Path(sys.argv[1]),Path(sys.argv[2]))
