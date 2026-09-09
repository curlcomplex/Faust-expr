#pragma once

#include "graph/transport/PreparedAudioRenderer.h"

#include <memory>
#include <string>

namespace curlop {
struct ApgBundle;
class GraphState;
}

namespace curlop::transport {

struct FaustGraphRendererTiming {
    double totalUs = 0.0;
    double graphPlanUs = 0.0;
    double factoryUs = 0.0;
    double instanceInitUs = 0.0;
};

struct FaustGraphRendererBuild {
    std::unique_ptr<PreparedAudioRenderer> renderer;
    std::string diagnostic;
    FaustGraphRendererTiming timing;
    // A graph in this class must not fall back to APG: its identity is within
    // TC1 authoring, but its required unified ownership has not been lowered.
    bool rejectPublication = false;
};

// Derive a prepared renderer from the canonical editable graph. Faust owns the
// audio/control topology and retained taps; native host inputs and separate
// render-domain sinks bind at the prepared boundary. Unsupported domains are
// rejected explicitly before publication.
FaustGraphRendererBuild buildFaustGraphRenderer(
    const GraphState&, double sampleRate, int maximumBlockSize);

// Transitional attachment seam. This mutates the immutable bundle only after
// the renderer, stable tap mapping and native boundary bindings are prepared.
// Any unsupported graph or identity mismatch leaves the existing published
// renderer untouched.
bool tryAttachFaustGraphRenderer(
    ApgBundle&, const GraphState&, double sampleRate, int maximumBlockSize,
    std::string& diagnostic);

} // namespace curlop::transport
