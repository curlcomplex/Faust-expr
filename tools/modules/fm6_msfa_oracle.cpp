// Offline transport adapter only: unmodified Apache-2.0 Google MSFA core.
// Reject known unsupported oracle behavior rather than falsely certifying it.
#include <array>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include "synth.h"
#include "controllers.h"
#include "dx7note.h"
#include "freqlut.h"
#include "exp2.h"
#include "sin.h"
#include "patch.h"
#include "lfo.h"
struct Event { int frame; std::string key; int value; };
int integer(const char* s) { size_t n=0; long v=std::stol(s,&n); if(n!=std::strlen(s)||v<0||v>10000000) throw std::runtime_error("integer argument");return int(v); }
int main(int argc,char**argv) try {
 if(argc!=6) throw std::runtime_error("usage: oracle packed128 score.tsv out.f32 frames block");
 int frames=integer(argv[4]),block=integer(argv[5]);
 if(frames<64||frames%64||block<1||block>4096) throw std::runtime_error("whole 64-frame native quanta required");
 std::array<char,128> packed{}; std::ifstream pf(argv[1],std::ios::binary);
 if(!pf.read(packed.data(),128)||pf.peek()!=EOF) throw std::runtime_error("128-byte patch required");
 for(unsigned char b:packed) if(b>127) throw std::runtime_error("7-bit patch required");
 std::array<char,156> patch{}; UnpackPatch(packed.data(),patch.data());
 int algorithm=patch[134]+1,feedback=patch[135];
 if(algorithm<1||algorithm>32||feedback>7) throw std::runtime_error("algorithm/feedback range");
 if(feedback && (algorithm==4||algorithm==6)) throw std::runtime_error("original MSFA does not implement multi-operator feedback; oracle unsupported");
 if(patch[140]) throw std::runtime_error("original MSFA does not implement AM depth; oracle unsupported");
 std::vector<Event> events;std::ifstream score(argv[2]);if(!score)throw std::runtime_error("score unavailable");
 std::string line;int last=-1;
 while(std::getline(score,line)) { Event e;std::string extra;std::istringstream row(line);
  if(!(row>>e.frame>>e.key>>e.value)||(row>>extra)||e.frame<0||e.frame>=frames||e.frame<last||e.frame%64)throw std::runtime_error("ordered 64-frame score required");
  if(e.key=="note" || e.key=="velocity") {if(e.value<0||e.value>127)throw std::runtime_error("note/velocity range");}
  else if(e.key=="gate") {if(e.value<0||e.value>1)throw std::runtime_error("gate range");}
  else throw std::runtime_error("unsupported event");
  for(const auto&old:events)if(old.frame==e.frame&&old.key==e.key)throw std::runtime_error("duplicate event");
  events.push_back(e);last=e.frame;
 }
 Freqlut::init(44100);Exp2::init();Sin::init();PitchEnv::init(44100);Lfo::init(44100);
 Controllers ctrl{};ctrl.values_[kControllerPitch]=0x2000;
 Dx7Note voice{};Lfo lfo{};lfo.reset(patch.data()+137);
 int note=57,velocity=100;bool started=false,down=false;size_t event=0;
 std::vector<float> audio(frames);std::array<int32_t,64> buf{};
 for(int n=0;n<frames;n+=64) {
  while(event<events.size()&&events[event].frame==n){auto e=events[event++];
   if(e.key=="note")note=e.value;else if(e.key=="velocity")velocity=e.value;
   else if(e.value){if(down)throw std::runtime_error("duplicate note-on; use a low gate interval");
    // Fresh-note semantics, consistent with Dx7Note::init; do not erase its DSP equations.
    voice=Dx7Note{};voice.init(patch.data(),note+int(patch[144])-24,velocity);lfo.keydown();started=true;down=true;
   }else {if(!down)throw std::runtime_error("note-off without note-on");voice.keyup();down=false;}
  }
  buf.fill(0);const int32_t val=lfo.getsample(),delay=lfo.getdelay();
  if(started)voice.compute(buf.data(),val,delay,&ctrl);
  for(int i=0;i<64;i++)audio[n+i]=float(buf[i]/16777216.0);
 }
 std::ofstream out(argv[3],std::ios::binary);if(!out)throw std::runtime_error("cannot open output");
 for(int n=0;n<frames;n+=block){int take=std::min(block,frames-n);out.write(reinterpret_cast<const char*>(audio.data()+n),take*sizeof(float));}
 if(!out)throw std::runtime_error("write failure");
 std::cout<<"{\"frames\":"<<frames<<",\"channels\":1,\"rate\":44100,\"native_quantum\":64,\"algorithm\":"<<algorithm<<",\"feedback\":"<<feedback<<",\"unpacked\":[";
 for(int i=0;i<156;i++)std::cout<<(i?",":"")<<int(static_cast<unsigned char>(patch[i]));
 std::cout<<"]}\n";return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<"\n";return 1;}
