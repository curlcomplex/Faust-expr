#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "control/vm/core/VMConstNode.h"
#include "graph/engine/modulebuilder/shared.h"
#include "graph/engine/nodes/BufferPlayerProcessor.h"
#include "graph/transport/FeedbackSccProcessor.h"
#include "graph/transport/SignalDescriptor.h"

#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

namespace curlop {

class ControlMeterProcessor;
class WireProcessor;

// Immutable edge identity plus the mutable native wire state used by the
// publication/runtime boundary. This data is shared by the direct Faust/VM
// publisher and the retained APG oracle; it is not builder ownership.
using ApgWireKey = std::tuple<int, int, std::string, std::string>;

struct ApgWireState {
    juce::AudioProcessorGraph::NodeID wireNodeID {};
    juce::AudioProcessorGraph::NodeID sumNodeID {};
    WireProcessor* wire = nullptr;
    transport::SignalDescriptor signalDescriptor =
        transport::SignalDescriptor::legacyStereo();
};
using ApgWireMap = std::map<ApgWireKey, ApgWireState>;
using ApgFeedbackRouteMap = std::map<
    ApgWireKey, std::shared_ptr<transport::FeedbackRouteControl>>;

struct ApgControlWireState {
    juce::AudioProcessorGraph::NodeID presenceNodeID {};
    ControlMeterProcessor* meter = nullptr;
    std::string meterSource;
    std::string targetMeterSource;
    transport::SignalDescriptor signalDescriptor =
        transport::SignalDescriptor::legacyScalar();
};
using ApgControlWireKey = std::tuple<int, int, std::string, std::string>;
using ApgControlWireMap = std::map<ApgControlWireKey, ApgControlWireState>;

struct ScriptV2BuildTiming {
    double parseUs = 0.0;
    double outputDeclsUs = 0.0;
    double inputNamesUs = 0.0;
    double routeCollectUs = 0.0;
    double graphInitUs = 0.0;
    double processorConfigUs = 0.0;
    double processorCreateUs = 0.0;
    double nodeAddUs = 0.0;
    double ioNodeUs = 0.0;
    double inputWireUs = 0.0;
    double meterWireUs = 0.0;
    double metadataUs = 0.0;
    int outputDeclCount = 0;
    int inputNameCount = 0;
    int routeCount = 0;
    int routeOpCount = 0;
};

struct ApgModuleBuildTiming {
    int moduleIndex = -1;
    std::string name;
    std::string base;
    double buildUs = 0.0;
    double addNodeUs = 0.0;
    double prepareUs = 0.0;
    double prepareConfigUs = 0.0;
    double prepareEnableBusesUs = 0.0;
    double prepareGraphUs = 0.0;
    double totalUs = 0.0;
    BufferPlayerProcessor::PrepareTiming buffer;
    ModuleBuilder::FaustJitBuildTiming faust;
    ScriptV2BuildTiming scriptV2;
};

// A module's address at the published outer topology. Prepared feedback
// regions may share one outer node while retaining their own port offsets.
struct ApgModuleEndpoint {
    juce::AudioProcessorGraph::NodeID outerNodeID {};
    int audioInputOffset = 0;
    int audioOutputOffset = 0;
    int controlInputOffset = 0;
    int controlOutputOffset = 0;

    bool valid() const noexcept { return outerNodeID.uid != 0; }
    int audioInputChannel(int localChannel) const noexcept
    {
        return localChannel < 0 ? -1 : audioInputOffset + localChannel;
    }
    int audioOutputChannel(int localChannel) const noexcept
    {
        return localChannel < 0 ? -1 : audioOutputOffset + localChannel;
    }
    int controlInputChannel(int localChannel) const noexcept
    {
        return localChannel < 0 ? -1 : controlInputOffset + localChannel;
    }
    int controlOutputChannel(int localChannel) const noexcept
    {
        return localChannel < 0 ? -1 : controlOutputOffset + localChannel;
    }
};

// Canonical graph edge description. The direct renderer and oracle consume
// the same ports, width/type descriptor and one-sample-feedback declaration.
struct ApgEdge {
    int src;
    int tgt;
    float gain = 1.0f;
    float pan = 0.0f;
    bool enabled = true;
    std::string srcPort;
    std::string tgtPort;
    transport::SignalDescriptor signalDescriptor =
        transport::SignalDescriptor::legacyStereo();
    bool feedbackBoundary = false;
    bool preparedPacketPublisher = false;
};

inline ApgWireKey makeApgWireKey(const ApgEdge& edge)
{
    return { edge.src, edge.tgt, edge.srcPort, edge.tgtPort };
}

inline bool apgControlEdgeCarriesSignal(const ApgEdge& edge)
{
    return edge.enabled && edge.gain > 0.0f;
}

} // namespace curlop
