#!/usr/bin/env python3
"""Add one optional native-factory seam to the exact previously tested retainer.

The source snapshot is immutable. A generated header preserves its routing and
retention logic and adds ownership for externally compiled native DSP kernels.
The default factory path is still the original FaustRuntime LLVM path.
"""
from pathlib import Path
import hashlib,sys
EXPECTED='3331779003feb0ca4b0c999f4f06a888ac7c87eb'
def generate(source: bytes) -> str:
    if hashlib.sha1(b'blob '+str(len(source)).encode()+b'\0'+source).hexdigest()!=EXPECTED:
        raise ValueError('retained runtime source changed; review patch explicitly')
    s=source.decode()
    def replace(a,b):
        nonlocal s
        if s.count(a)!=1: raise ValueError('factory seam anchor absent/ambiguous')
        s=s.replace(a,b)
    replace('namespace retained {','''#include <functional>
namespace retained_units {
struct ExternalDSP {
    std::shared_ptr<void> owner;
    std::unique_ptr<::dsp> instance;
};
using ExternalFactory = std::function<ExternalDSP(const curlop::ModuleEntry&)>;''')
    replace('    std::unique_ptr<::dsp> instance;\n    MapUI ui;',
            '    std::shared_ptr<void> externalOwner; // Outlives the instance, even across plans.\n    std::unique_ptr<::dsp> instance;\n    MapUI ui;')
    replace('    FaustRuntime runtime_;','    FaustRuntime runtime_;\n    ExternalFactory externalFactory_;')
    replace('Graph(int sr,int frames,const std::string& store):sampleRate_(sr),maxFrames_(frames){',
            'Graph(int sr,int frames,const std::string& store,ExternalFactory factory={}):externalFactory_(std::move(factory)),sampleRate_(sr),maxFrames_(frames){')
    replace('''                    std::string error;node->factory=runtime_.acquire(m.dslName,m.code,error);++p->acquired;
                    check(bool(node->factory),"Faust compilation failed: "+error);
                    // Use the established global compiler/lifecycle lock.
                    std::lock_guard<std::mutex> lock(FaustRuntime::compileMutex());
                    node->instance.reset(node->factory->raw()->createDSPInstance());''',
'''                    if (externalFactory_) {
                        auto native=externalFactory_(m); ++p->acquired;
                        node->externalOwner=std::move(native.owner);
                        node->instance=std::move(native.instance);
                    } else {
                        std::string error;
                        node->factory=runtime_.acquire(m.dslName,m.code,error);++p->acquired;
                        check(bool(node->factory),"Faust compilation failed: "+error);
                        std::lock_guard<std::mutex> lock(FaustRuntime::compileMutex());
                        node->instance.reset(node->factory->raw()->createDSPInstance());
                    }
                    std::lock_guard<std::mutex> lifecycleLock(FaustRuntime::compileMutex());''')
    return s
if __name__=='__main__':
    Path(sys.argv[2]).write_text(generate(Path(sys.argv[1]).read_bytes()))
