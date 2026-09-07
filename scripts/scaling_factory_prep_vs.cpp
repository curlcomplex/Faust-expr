#include <faust/dsp/llvm-dsp.h>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
static bool writeFile(const std::string&p,const std::string&s){std::ofstream f(p,std::ios::binary|std::ios::trunc);f.write(s.data(),std::streamsize(s.size()));return bool(f);}
int main(int argc,char**argv){
 if(argc!=6){std::cerr<<"usage: prep scalar|sch <voices> <vecsize> <out.bc> <scheduler.ll>\n";return 2;}
 std::string mode=argv[1],out=argv[4],sched=argv[5];int voices=std::atoi(argv[2]),vs=std::atoi(argv[3]);if(voices<8||voices>1024)return 2;
 std::ostringstream src;src<<"import(\"stdfaust.lib\");\nvoice(i)=os.osc(70+i*3.17):fi.lowpass(2,1200+i*41):*(0.01);\nbank=par(i,"<<voices<<",voice(i)):>_;\nprocess=bank,bank;\n";
 std::vector<std::string> os;if(mode=="sch"){if(vs<1||sched=="-")return 2;os={"-sch","-vs",std::to_string(vs),"-L",sched};}else if(mode!="scalar")return 2;
 std::vector<const char*>av;for(auto&x:os)av.push_back(x.c_str());std::string err;auto*f=createDSPFactoryFromString("VS"+mode+std::to_string(voices)+"x"+std::to_string(vs),src.str(),int(av.size()),av.data(),getDSPMachineTarget(),err,-1);
 if(!f){std::cerr<<err<<"\n";return 3;}auto bc=writeDSPFactoryToBitcode(f);if(bc.empty()||!writeFile(out,bc))return 4;std::cout<<f->getCompileOptions()<<"\n";return deleteDSPFactory(f)?0:5;
}
