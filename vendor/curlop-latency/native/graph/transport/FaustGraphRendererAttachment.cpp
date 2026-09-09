#include "graph/transport/FaustGraphRenderer.h"

#include "runtime/EngineSlot.h"

#include <utility>

namespace curlop::transport {

bool tryAttachFaustGraphRenderer(
    ApgBundle& bundle, const GraphState& state, double sampleRate,
    int maximumBlockSize, std::string& diagnostic)
{
    auto built = buildFaustGraphRenderer(
        state, sampleRate, maximumBlockSize);
    if (built.renderer == nullptr) {
        diagnostic = "unsupported graph for unified Faust renderer";
        if (! built.diagnostic.empty())
            diagnostic += ": " + built.diagnostic;
        return false;
    }
    if (! bundle.adoptPreparedAudioRenderer(std::move(built.renderer))) {
        diagnostic =
            "prepared Faust tap identities do not match the APG bundle";
        return false;
    }
    diagnostic.clear();
    return true;
}

} // namespace curlop::transport
