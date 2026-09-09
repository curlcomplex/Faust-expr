#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>

#ifndef CURLOP_ENABLE_LEGACY_APG_ORACLE
#define CURLOP_ENABLE_LEGACY_APG_ORACLE 0
#endif

#include "control/vm/core/VMConstNode.h"      // ModuleLayout, VMProcessData (the APG channel contract)
#include "control/vm/core/VMEventBridge.h"    // GUI display bridge (atomics snapshot)
#include "control/vm/machine/Machine.h"       // F-066: the control-data computer
#include "control/vm/machine/Delivery.h"      // F-066: declared-control composition
#include "control/vm/machine/StepClockRuntime.h"
#include "control/vm/machine/TransportClockRuntime.h"
#include "control/vm/params/ParamStore.h"
#include "control/vm/params/ParamFIFO.h"
#include "control/vm/params/AudioParamState.h"
#include "control/vm/params/ParamTypes.h"
#include "control/surfaces/script/ScriptCompiler.h"  // CompileResultV2 (source artifacts)
#include "io/NativeBridgeServer.h"  // ParamMapping
#include "graph/state/GraphPublicationTypes.h"
#include "graph/state/GraphStateApplier.h"
#include "graph/engine/nodes/AudioInputEndpointProcessor.h"
#include "graph/engine/nodes/ControlCoreProcessor.h"
#include "graph/engine/nodes/ControlMeterProcessor.h"
#include "graph/engine/nodes/HostAutomationInputProcessor.h"
#include "graph/engine/nodes/HostMidiInputProcessor.h"
#include "graph/engine/nodes/MeterProcessor.h"
#include "graph/engine/nodes/MidiOutputProcessor.h"
#include "graph/engine/nodes/ScriptNodeProcessor.h"
#include "graph/engine/nodes/VisualInputProcessor.h"
#include "graph/transport/PreparedExecutionPlan.h"
#include "graph/transport/PreparedSignalSchedule.h"
#if CURLOP_ENABLE_LEGACY_APG_ORACLE
#include "graph/transport/CompiledRenderPlan.h"
#include "graph/transport/RealtimeWorkerTeam.h"
#endif
#include "modules/backend/FaustNode.h"
#include "graph/transport/PreparedAudioRenderer.h"
#include "graph/transport/ControlRangeHandoff.h"
#include "graph/engine/buffer/BufferStorage.h"

#include <memory>
#include <vector>
#include <array>
#include <deque>
#include <unordered_map>
#include <string>
#include <atomic>
#include <cstring>
#include <algorithm>
#include <functional>
#include <cstdint>
#include <limits>
#include <mutex>

namespace curlop {

class ScriptNodeProcessor;     // forward — graph/ScriptNodeProcessor.h
class ControlCoreProcessor;    // forward — graph/ControlCoreProcessor.h
class MeterProcessor;          // forward — graph/MeterProcessor.h
class ControlMeterProcessor;   // forward — graph/engine/nodes/ControlMeterProcessor.h
class VisualInputProcessor;
class FaustNode;            // forward — dsp/FaustNode.h (libfaust pimpl)
class StepSequencerProcessor;  // forward — graph/engine/nodes/StepSequencerProcessor.h
class TransportClockProcessor; // forward — graph/engine/nodes/TransportClockProcessor.h
class HostMidiInputProcessor;
class HostAutomationInputProcessor;
class MidiOutputProcessor;
class BufferPlayerProcessor;
namespace ModuleBuilder {
    class LevelGainProcessor;  // forward — graph/engine/modulebuilder/shared.h
}

// ── ApgBundle (T-375 / D-APG-1) ─────────────────────────────────────────
// Coherent unit of APG state. Owns the live AudioProcessorGraph + every
// non-owning per-module pointer the audio thread reads when rendering it.
// Built off-thread (ApgClipBuilder::buildClipApg) and published into the
// slot via std::atomic<ApgBundle*> exchange — the audio thread acquire-
// loads once per block (EngineSlot::apgBundleAcquire) and reads the
// graph + parallel arrays consistently through that single pointer.
// The displaced bundle is retired through CurlopProcessor's
// pendingDeleteApgBundles_ queue (audioBlockSeq + 2 fence).
//
// Why a struct, not separate atomics: today the graph and the parallel
// arrays drift relative to each other — the message thread can update
// `apgGraph` and the per-module pointer arrays in two separate steps,
// leaving the audio thread one block where it sees new graph + old
// pointers (or vice versa). Bundling them turns a multi-field update
// into a single pointer publish. The audit (D-APG-1) calls the prior
// scheme a layering violation; this closes it.
//
// Lifetime: heap-allocated on the message thread, owned by the atomic
// pointer in EngineSlot::apgBundle. Slot destructor + reset() delete
// inline (no concurrent audio reader by then). Production swaps go
// through the retire queue.
//
struct ApgBundle {
    // Monotonic identity of this immutable publication. It lets host-facing
    // observers distinguish a rebuilt bundle with unchanged latency from the
    // previous active bundle without reading mutable slot state.
    std::uint64_t publicationGeneration = 0;

    // Shared host-boundary state outlives both the prepared renderer and the
    // retained APG graph so either renderer can own the next block exactly
    // once without losing captured audio, held-note or logical-voice state.
    std::vector<std::unique_ptr<AudioInputRuntime>>
        preparedAudioInputRuntimes;
    std::vector<int> preparedAudioInputRuntimeGraphIndices;
    std::vector<int> preparedAudioInputRuntimeApgIndices;
    std::vector<std::unique_ptr<HostMidiInputRuntime>>
        preparedHostMidiRuntimes;
    std::vector<int> preparedHostMidiRuntimeGraphIndices;
    std::vector<std::unique_ptr<MidiOutputRuntime>>
        preparedMidiOutputRuntimes;
    std::vector<int> preparedMidiOutputRuntimeGraphIndices;
    std::vector<int> preparedMidiOutputRuntimeApgIndices;
    std::vector<std::array<int, 2>> directMidiInputSchemaIndices;
    mutable std::vector<std::array<std::vector<float>, 4>> directMidiInputScratch;
    std::vector<std::array<int, 6>> directMidiOutputSchemaIndices;
    mutable std::vector<std::array<std::vector<float>, 6>> directMidiOutputScratch;
    mutable std::atomic<bool> midiOutputsTombstoned { false };

    // The differential oracle alone retains a JUCE graph executor. Product
    // bundles carry only the unified Faust renderer and shared boundary state.
#if CURLOP_ENABLE_LEGACY_APG_ORACLE
    std::unique_ptr<juce::AudioProcessorGraph> apgGraph;
#endif

    // The old compiled-plan worker experiment is retained only beside the APG
    // differential oracle. Normal TC1 publication has exactly one audio
    // executor: the prepared unified Faust renderer.
#if CURLOP_ENABLE_LEGACY_APG_ORACLE
    std::unique_ptr<transport::CompiledRenderPlan> compiledRenderPlan;
    std::unique_ptr<transport::RealtimeWorkerTeam> compiledWorkerTeam;
    int compiledWorkerCount = 0;
    int compiledVoiceCohortCount = 0;
#endif

    // Checkpoint 4 candidate seam. A prepared renderer may publish through the
    // same immutable bundle without changing the graph builder's default.
    std::unique_ptr<transport::PreparedAudioRenderer> preparedAudioRenderer;
    // Audio-thread-owned result of the latest processAudio call. Recorder taps
    // must follow the renderer that actually produced the current block rather
    // than reusing candidate output after a per-block fallback.
    mutable std::atomic<bool> preparedAudioRenderedLastBlock { false };
    // A rejected prepared block is handled as silence. The renderer remains
    // authoritative for the next callback: TC1 never transfers a published
    // Faust graph to APG at audio-thread time.
    mutable std::atomic<bool> preparedAudioRendererEnabled { true };
    // Built off-callback when a candidate is attached. Recorder taps retain
    // the bundle's module-row identity while the renderer is free to order its
    // internal taps independently.
    std::vector<int> preparedTapByModuleIndex;

    // A control-only native source can publish an immutable VM contract
    // without creating an otherwise idle AudioProcessorGraph. The handoffs
    // preserve the same audio-thread-producer/message-thread-consumer meter
    // contract as the unified Faust renderer.
    bool directControlOnlyPublication = false;
    bool directMidiOnlyPublication = false;
    std::vector<std::size_t> directControlTelemetryOffsets;
    std::unique_ptr<transport::ControlRangeHandoff[]>
        directControlOutputHolds;

    void prepareDirectControlOutputTelemetry()
    {
        directControlTelemetryOffsets.assign(
            static_cast<std::size_t>(std::max(0, apgModuleCount)),
            std::numeric_limits<std::size_t>::max());
        std::size_t count = 0;
        for (int module = 0; module < apgModuleCount; ++module) {
            if (module >= static_cast<int>(apgControlOutputNames.size()))
                continue;
            directControlTelemetryOffsets[static_cast<std::size_t>(module)] =
                count;
            count += apgControlOutputNames[static_cast<std::size_t>(module)]
                .size();
        }
        directControlOutputHolds = count > 0
            ? std::make_unique<transport::ControlRangeHandoff[]>(count)
            : nullptr;
    }

    bool prepareDirectMidiOnlyBoundaries(int blockSize)
    {
        if (! directMidiOnlyPublication || apgModuleCount <= 0 || blockSize <= 0)
            return false;
        std::vector<std::unique_ptr<HostMidiInputRuntime>> inputs;
        std::vector<int> inputGraphIndices;
        std::vector<std::array<int, 2>> inputSchemas;
        std::vector<std::array<std::vector<float>, 4>> inputScratch;
        std::vector<std::unique_ptr<MidiOutputRuntime>> outputs;
        std::vector<int> outputGraphIndices;
        std::vector<int> outputApgIndices;
        std::vector<std::array<int, 6>> outputSchemas;
        std::vector<std::array<std::vector<float>, 6>> outputScratch;
        constexpr std::array<const char*, 6> expected {
            "GATE", "PITCH", "VELOCITY", "CHANNEL", "CC", "CC_VALUE" };
        for (int module = 0; module < apgModuleCount; ++module) {
            const auto index = static_cast<std::size_t>(module);
            if (index >= moduleLineageIds.size()
                || index >= bytecodeToGraphIdx.size())
                return false;
            const int graphIndex = bytecodeToGraphIdx[index];
            if (graphIndex < 0)
                return false;
            if (juce::String(moduleLineageIds[index]).equalsIgnoreCase(
                    "core.midi_in")) {
                if (index >= apgControlInputNames.size())
                    return false;
                std::array<int, 2> schemaIndices {};
                schemaIndices.fill(-1);
                constexpr std::array<const char*, 2> expectedInputs {
                    "CHANNEL", "CC" };
                for (std::size_t control = 0;
                     control < expectedInputs.size(); ++control) {
                    if (index >= layouts.size())
                        return false;
                    const int schemaIndex = layouts[index].indexOf(
                        expectedInputs[control]);
                    if (schemaIndex < 0)
                        return false;
                    schemaIndices[control] = schemaIndex;
                }
                auto runtime = std::make_unique<HostMidiInputRuntime>();
                runtime->setPreparedPacketCapacity(preparedPacketCapacity);
                runtime->setPreparedCcPacketOutput(true);
                runtime->prepare();
                inputGraphIndices.push_back(graphIndex);
                inputSchemas.push_back(schemaIndices);
                std::array<std::vector<float>, 4> rows;
                for (auto& row : rows)
                    row.assign(static_cast<std::size_t>(blockSize), 0.0f);
                inputScratch.push_back(std::move(rows));
                inputs.push_back(std::move(runtime));
                continue;
            }
            if (! juce::String(moduleLineageIds[index]).equalsIgnoreCase(
                    "core.midi_out")
                || index >= apgControlInputNames.size())
                return false;
            std::array<int, 6> schemaIndices {};
            schemaIndices.fill(-1);
            for (std::size_t control = 0; control < expected.size(); ++control) {
                if (index >= layouts.size())
                    return false;
                const int schemaIndex = layouts[index].indexOf(expected[control]);
                if (schemaIndex < 0)
                    return false;
                schemaIndices[control] = schemaIndex;
            }
            auto runtime = std::make_unique<MidiOutputRuntime>();
            if (index < apgMidiOutputRouteIds.size())
                runtime->configureRoute(apgMidiOutputRouteIds[index]);
            outputGraphIndices.push_back(graphIndex);
            outputApgIndices.push_back(module);
            outputSchemas.push_back(schemaIndices);
            std::array<std::vector<float>, 6> rows;
            for (auto& row : rows)
                row.assign(static_cast<std::size_t>(blockSize), 0.0f);
            outputScratch.push_back(std::move(rows));
            outputs.push_back(std::move(runtime));
        }
        if (inputs.empty() || outputs.empty())
            return false;
        preparedHostMidiRuntimes = std::move(inputs);
        preparedHostMidiRuntimeGraphIndices = std::move(inputGraphIndices);
        directMidiInputSchemaIndices = std::move(inputSchemas);
        directMidiInputScratch = std::move(inputScratch);
        preparedMidiOutputRuntimes = std::move(outputs);
        preparedMidiOutputRuntimeGraphIndices = std::move(outputGraphIndices);
        preparedMidiOutputRuntimeApgIndices = std::move(outputApgIndices);
        directMidiOutputSchemaIndices = std::move(outputSchemas);
        directMidiOutputScratch = std::move(outputScratch);
        midiOutputsTombstoned.store(false, std::memory_order_relaxed);
        return true;
    }

    bool processDirectMidiOnlyInputs(const VMProcessData& processData,
                                     int sampleCount) const noexcept
    {
        if (! directMidiOnlyPublication || sampleCount <= 0)
            return false;
        for (std::size_t input = 0;
             input < preparedHostMidiRuntimes.size(); ++input) {
            if (input >= preparedHostMidiRuntimeGraphIndices.size()
                || input >= directMidiInputSchemaIndices.size()
                || input >= directMidiInputScratch.size())
                return false;
            const int graphIndex = preparedHostMidiRuntimeGraphIndices[input];
            if (graphIndex < 0
                || graphIndex >= static_cast<int>(processData.modules.size()))
                return false;
            const auto& module = processData.modules[
                static_cast<std::size_t>(graphIndex)];
            const auto& schemaIndices = directMidiInputSchemaIndices[input];
            auto& scratch = directMidiInputScratch[input];
            if (sampleCount > static_cast<int>(scratch[0].size()))
                return false;
            const float* controls[2] {};
            for (std::size_t control = 0; control < schemaIndices.size(); ++control) {
                const int schemaIndex = schemaIndices[control];
                if (schemaIndex < 0
                    || schemaIndex >= module.paramBuffer.numParams())
                    return false;
                controls[control] = module.paramBuffer[schemaIndex];
            }
            if (! preparedHostMidiRuntimes[input]->process(
                    scratch[0].data(), scratch[1].data(), scratch[2].data(),
                    scratch[3].data(), sampleCount,
                    [controls] (int control, int sample, float fallback) {
                        const auto* row = control >= 0 && control < 2
                            ? controls[control] : nullptr;
                        const float value = row != nullptr ? row[sample] : fallback;
                        return std::isfinite(value) ? value : fallback;
                    }))
                return false;
        }
        return ! preparedHostMidiRuntimes.empty();
    }

    bool processDirectMidiOnlyOutputs(const VMProcessData& processData,
                                      int sampleCount,
                                      juce::MidiBuffer& midi) const noexcept
    {
        if (! directMidiOnlyPublication || sampleCount <= 0)
            return false;
        bool processed = false;
        for (std::size_t output = 0;
             output < preparedMidiOutputRuntimes.size(); ++output) {
            if (output >= preparedMidiOutputRuntimeGraphIndices.size()
                || output >= directMidiOutputSchemaIndices.size()
                || output >= directMidiOutputScratch.size())
                return false;
            const int graphIndex = preparedMidiOutputRuntimeGraphIndices[output];
            if (graphIndex < 0
                || graphIndex >= static_cast<int>(processData.modules.size()))
                return false;
            const auto& module = processData.modules[
                static_cast<std::size_t>(graphIndex)];
            const auto& schemaIndices = directMidiOutputSchemaIndices[output];
            auto& scratch = directMidiOutputScratch[output];
            if (sampleCount > static_cast<int>(scratch[0].size()))
                return false;
            const float* controls[6] {};
            for (std::size_t control = 0; control < schemaIndices.size(); ++control) {
                const int schemaIndex = schemaIndices[control];
                if (schemaIndex < 0
                    || schemaIndex >= module.paramBuffer.numParams())
                    return false;
                const auto* row = module.paramBuffer[schemaIndex];
                if (control == static_cast<std::size_t>(MidiOutputRuntime::kPitch)) {
                    for (int sample = 0; sample < sampleCount; ++sample) {
                        const float hz = row[sample];
                        scratch[control][static_cast<std::size_t>(sample)] =
                            std::isfinite(hz) && hz > 0.0f
                                ? vm::noteToSignal(
                                    12.0f * std::log2(hz / 261.6256f))
                                : 0.0f;
                    }
                } else {
                    std::memcpy(scratch[control].data(), row,
                                static_cast<std::size_t>(sampleCount)
                                    * sizeof(float));
                }
                controls[control] = scratch[control].data();
            }
            preparedMidiOutputRuntimes[output]->processPreparedControls(
                controls, static_cast<int>(schemaIndices.size()), sampleCount, midi);
            processed = true;
        }
        return processed;
    }

    bool hasDirectControlOutputTelemetry(int moduleIndex) const noexcept
    {
        if (! directControlOnlyPublication
            || directControlOutputHolds == nullptr)
            return false;
        const int module = apgModuleIndexForGraphIndex(moduleIndex);
        return module >= 0
            && module < static_cast<int>(directControlTelemetryOffsets.size())
            && directControlTelemetryOffsets[static_cast<std::size_t>(module)]
                != std::numeric_limits<std::size_t>::max();
    }

    void publishDirectControlOutputRows(
        int graphModuleIndex, const float* const* rows, int rowCount,
        int sampleCount) const noexcept
    {
        const int module = apgModuleIndexForGraphIndex(graphModuleIndex);
        if (! directControlOnlyPublication
            || directControlOutputHolds == nullptr
            || module < 0
            || module >= static_cast<int>(directControlTelemetryOffsets.size())
            || directControlTelemetryOffsets[static_cast<std::size_t>(module)]
                == std::numeric_limits<std::size_t>::max()
            || rows == nullptr || rowCount <= 0)
            return;
        const auto offset = directControlTelemetryOffsets[
            static_cast<std::size_t>(module)];
        const auto outputCount = std::min<std::size_t>(
            static_cast<std::size_t>(rowCount),
            apgControlOutputNames[static_cast<std::size_t>(module)].size());
        const int samples = std::max(0, sampleCount);
        for (std::size_t output = 0; output < outputCount; ++output) {
            const auto* row = rows[output];
            if (row == nullptr)
                continue;
            float minimum = 0.0f;
            float maximum = 0.0f;
            float last = 0.0f;
            if (samples > 0) {
                minimum = row[0];
                maximum = minimum;
                for (int sample = 1; sample < samples; ++sample) {
                    minimum = std::min(minimum, row[sample]);
                    maximum = std::max(maximum, row[sample]);
                }
                last = row[samples - 1];
            }
            directControlOutputHolds[offset + output].publish(
                minimum, maximum, last);
        }
    }

    int apgModuleIndexForGraphIndex(int graphIndex) const noexcept
    {
        for (int module = 0;
             module < static_cast<int>(bytecodeToGraphIdx.size());
             ++module)
            if (bytecodeToGraphIdx[static_cast<std::size_t>(module)]
                == graphIndex)
                return module;
        return graphIndex >= 0 && graphIndex < apgModuleCount
            ? graphIndex : -1;
    }

    bool adoptPreparedAudioRenderer(
        std::unique_ptr<transport::PreparedAudioRenderer> candidate)
    {
        if (candidate == nullptr)
            return false;
        if (! candidate->publishesProductObservers())
            return false;

        std::vector<int> tapMap(
            static_cast<std::size_t>(std::max(0, apgModuleCount)), -1);
        for (int moduleIndex = 0; moduleIndex < apgModuleCount; ++moduleIndex) {
            if (moduleIndex >= static_cast<int>(moduleNodeIds.size())
                || moduleIndex >= static_cast<int>(apgAudioOutputWidths.size()))
                return false;
            const int requiredWidth = apgAudioOutputWidths[
                static_cast<std::size_t>(moduleIndex)];
            const auto& nodeId = moduleNodeIds[
                static_cast<std::size_t>(moduleIndex)];
            bool retainedControlBoundary = false;
            if (requiredWidth <= 0 && nodeId.empty())
                continue;
            if (nodeId.empty())
                return false;
            for (std::size_t tap = 0; tap < candidate->tapCount(); ++tap) {
                if (candidate->tapModuleId(tap) != nodeId)
                    continue;
                if (requiredWidth <= 0
                    || candidate->tapChannelCount(tap) == requiredWidth) {
                    tapMap[static_cast<std::size_t>(moduleIndex)] =
                        static_cast<int>(tap);
                    break;
                }
                // VM and host-control lanes intentionally have no audio tap.
                // The replacement declares that ownership itself; retained
                // APG node pointers are not publication authority.
                retainedControlBoundary = retainedControlBoundary
                    || (candidate->tapChannelCount(tap) == 0
                        && candidate->ownsControlBoundary(tap));
                if (retainedControlBoundary)
                    tapMap[static_cast<std::size_t>(moduleIndex)] =
                        static_cast<int>(tap);
            }
            if (requiredWidth > 0
                && tapMap[static_cast<std::size_t>(moduleIndex)] < 0
                && ! retainedControlBoundary)
                return false;
        }
        std::vector<std::unique_ptr<AudioInputRuntime>> audioInputRuntimes;
        std::vector<int> audioInputRuntimeModuleIndices;
        audioInputRuntimes.reserve(candidate->audioInputBoundaryCount());
        audioInputRuntimeModuleIndices.reserve(
            candidate->audioInputBoundaryCount());
        for (std::size_t boundary = 0;
             boundary < candidate->audioInputBoundaryCount(); ++boundary) {
            const int graphIndex =
                candidate->audioInputBoundaryGraphIndex(boundary);
            const int moduleIndex =
                apgModuleIndexForGraphIndex(graphIndex);
            auto runtime = std::make_unique<AudioInputRuntime>(
                candidate->audioInputBoundaryChannelIndex(boundary));
            if (! candidate->bindAudioInputRuntime(
                    graphIndex, runtime.get()))
                return false;
            audioInputRuntimeModuleIndices.push_back(moduleIndex);
            audioInputRuntimes.push_back(std::move(runtime));
        }
        std::vector<std::unique_ptr<HostMidiInputRuntime>> midiRuntimes;
        std::vector<int> midiRuntimeModuleIndices;
        midiRuntimes.reserve(candidate->hostMidiInputBoundaryCount());
        midiRuntimeModuleIndices.reserve(
            candidate->hostMidiInputBoundaryCount());
        for (std::size_t boundary = 0;
             boundary < candidate->hostMidiInputBoundaryCount();
             ++boundary) {
            const int graphIndex =
                candidate->hostMidiInputBoundaryGraphIndex(boundary);
            const int moduleIndex =
                apgModuleIndexForGraphIndex(graphIndex);
            auto runtime = std::make_unique<HostMidiInputRuntime>();
            runtime->setPreparedPacketCapacity(preparedPacketCapacity);
            runtime->prepare();
            if (! candidate->bindHostMidiRuntime(
                    graphIndex, runtime.get()))
                return false;
            midiRuntimeModuleIndices.push_back(moduleIndex);
            midiRuntimes.push_back(std::move(runtime));
        }
        std::vector<std::unique_ptr<MidiOutputRuntime>> outputRuntimes;
        std::vector<int> outputRuntimeModuleIndices;
        outputRuntimes.reserve(candidate->midiOutputBoundaryCount());
        outputRuntimeModuleIndices.reserve(
            candidate->midiOutputBoundaryCount());
        for (std::size_t boundary = 0;
             boundary < candidate->midiOutputBoundaryCount(); ++boundary) {
            const int graphIndex =
                candidate->midiOutputBoundaryGraphIndex(boundary);
            const int moduleIndex =
                apgModuleIndexForGraphIndex(graphIndex);
            auto runtime = std::make_unique<MidiOutputRuntime>();
            if (moduleIndex >= 0
                && moduleIndex < static_cast<int>(
                    apgMidiOutputRouteIds.size()))
                runtime->configureRoute(apgMidiOutputRouteIds[
                    static_cast<std::size_t>(moduleIndex)]);
            if (! candidate->bindMidiOutputRuntime(
                    graphIndex, runtime.get()))
                return false;
            outputRuntimeModuleIndices.push_back(moduleIndex);
            outputRuntimes.push_back(std::move(runtime));
        }
        for (std::size_t boundary = 0;
             boundary < candidate->visualInputBoundaryCount(); ++boundary) {
            const int graphIndex =
                candidate->visualInputBoundaryGraphIndex(boundary);
            auto* runtime = candidate->visualInputRuntime(graphIndex);
            if (runtime == nullptr)
                return false;
            const int moduleIndex = apgModuleIndexForGraphIndex(graphIndex);
            if (moduleIndex < 0
                || moduleIndex >= static_cast<int>(apgVisualInputs.size()))
                continue;
            if (auto* processor = apgVisualInputs[
                    static_cast<std::size_t>(moduleIndex)]) {
                if (processor->controlInputCount()
                        != runtime->controlInputCount()
                    || processor->audioInputCount()
                        != runtime->audioInputCount())
                    return false;
            }
        }
        // Bind only after every visual adapter has validated. A later mismatch
        // must not leave an earlier APG adapter pointing into a rejected,
        // soon-to-be-destroyed candidate renderer.
        for (std::size_t boundary = 0;
             boundary < candidate->visualInputBoundaryCount(); ++boundary) {
            const int graphIndex =
                candidate->visualInputBoundaryGraphIndex(boundary);
            const int moduleIndex = apgModuleIndexForGraphIndex(graphIndex);
            if (moduleIndex < 0
                || moduleIndex >= static_cast<int>(apgVisualInputs.size()))
                continue;
            if (auto* processor = apgVisualInputs[
                    static_cast<std::size_t>(moduleIndex)])
                processor->bindPreparedRuntime(
                    candidate->visualInputRuntime(graphIndex));
        }
        for (std::size_t runtime = 0;
             runtime < audioInputRuntimes.size(); ++runtime) {
            const int moduleIndex =
                audioInputRuntimeModuleIndices[runtime];
            if (moduleIndex < 0
                || moduleIndex >= static_cast<int>(
                    apgAudioInputEndpoints.size()))
                continue;
            if (auto* processor = apgAudioInputEndpoints[
                    static_cast<std::size_t>(moduleIndex)])
                processor->bindPreparedRuntime(
                    audioInputRuntimes[runtime].get());
        }
        for (std::size_t runtime = 0;
             runtime < midiRuntimes.size(); ++runtime) {
            const int moduleIndex = midiRuntimeModuleIndices[runtime];
            if (moduleIndex < 0
                || moduleIndex >= static_cast<int>(
                    apgHostMidiInputs.size()))
                continue;
            if (auto* processor = apgHostMidiInputs[
                    static_cast<std::size_t>(moduleIndex)])
                processor->bindPreparedRuntime(midiRuntimes[runtime].get());
        }
        for (std::size_t runtime = 0;
             runtime < outputRuntimes.size(); ++runtime) {
            const int moduleIndex = outputRuntimeModuleIndices[runtime];
            if (moduleIndex < 0
                || moduleIndex >= static_cast<int>(apgMidiOutputs.size()))
                continue;
            if (auto* processor = apgMidiOutputs[
                    static_cast<std::size_t>(moduleIndex)])
                processor->bindPreparedRuntime(outputRuntimes[runtime].get());
        }
        preparedTapByModuleIndex = std::move(tapMap);
        preparedAudioInputRuntimes = std::move(audioInputRuntimes);
        preparedAudioInputRuntimeGraphIndices.clear();
        preparedAudioInputRuntimeGraphIndices.reserve(
            preparedAudioInputRuntimes.size());
        for (std::size_t boundary = 0;
             boundary < candidate->audioInputBoundaryCount(); ++boundary)
            preparedAudioInputRuntimeGraphIndices.push_back(
                candidate->audioInputBoundaryGraphIndex(boundary));
        preparedAudioInputRuntimeApgIndices =
            std::move(audioInputRuntimeModuleIndices);
        preparedHostMidiRuntimes = std::move(midiRuntimes);
        preparedHostMidiRuntimeGraphIndices.clear();
        preparedHostMidiRuntimeGraphIndices.reserve(
            preparedHostMidiRuntimes.size());
        for (std::size_t boundary = 0;
             boundary < candidate->hostMidiInputBoundaryCount(); ++boundary)
            preparedHostMidiRuntimeGraphIndices.push_back(
                candidate->hostMidiInputBoundaryGraphIndex(boundary));
        preparedMidiOutputRuntimes = std::move(outputRuntimes);
        preparedMidiOutputRuntimeGraphIndices.clear();
        preparedMidiOutputRuntimeGraphIndices.reserve(
            preparedMidiOutputRuntimes.size());
        for (std::size_t boundary = 0;
             boundary < candidate->midiOutputBoundaryCount(); ++boundary)
            preparedMidiOutputRuntimeGraphIndices.push_back(
                candidate->midiOutputBoundaryGraphIndex(boundary));
        preparedMidiOutputRuntimeApgIndices =
            std::move(outputRuntimeModuleIndices);
        midiOutputsTombstoned.store(false, std::memory_order_relaxed);
        preparedAudioRenderer = std::move(candidate);
        preparedAudioRenderedLastBlock.store(false, std::memory_order_relaxed);
        preparedAudioRendererEnabled.store(true, std::memory_order_relaxed);
        return true;
    }

    bool bindPreparedHostAutomationSource(
        const std::atomic<float>* values, int laneCount) const noexcept
    {
        return preparedAudioRenderer != nullptr
            && preparedAudioRenderer->bindHostAutomationSource(
                values, laneCount);
    }

    void clearPreparedHostAutomationSource() const noexcept
    {
        if (preparedAudioRenderer != nullptr)
            preparedAudioRenderer->clearHostAutomationSource();
    }

    AudioInputRuntime* preparedAudioInputRuntimeForGraphIndex(
        int graphModuleIndex) const noexcept
    {
        for (std::size_t runtime = 0;
             runtime < preparedAudioInputRuntimes.size(); ++runtime)
            if (runtime < preparedAudioInputRuntimeGraphIndices.size()
                && preparedAudioInputRuntimeGraphIndices[runtime]
                    == graphModuleIndex)
                return preparedAudioInputRuntimes[runtime].get();
        return nullptr;
    }

    AudioInputRuntime* preparedAudioInputRuntimeForApgIndex(
        int moduleIndex) const noexcept
    {
        if (moduleIndex < 0)
            return nullptr;
        for (std::size_t runtime = 0;
             runtime < preparedAudioInputRuntimes.size(); ++runtime)
            if (runtime < preparedAudioInputRuntimeApgIndices.size()
                && preparedAudioInputRuntimeApgIndices[runtime]
                    == moduleIndex)
                return preparedAudioInputRuntimes[runtime].get();
        const int mappedGraphIndex = moduleIndex < static_cast<int>(
                bytecodeToGraphIdx.size())
            ? bytecodeToGraphIdx[static_cast<std::size_t>(moduleIndex)]
            : -1;
        const int graphIndex = mappedGraphIndex >= 0
            ? mappedGraphIndex : moduleIndex;
        return preparedAudioInputRuntimeForGraphIndex(graphIndex);
    }

    bool bindPreparedAudioInputSource(
        HostInputBlockView input) const noexcept
    {
        for (auto& runtime : preparedAudioInputRuntimes)
            runtime->bind(input);
        return ! preparedAudioInputRuntimes.empty();
    }

    void clearPreparedAudioInputBindings() const noexcept
    {
        for (auto& runtime : preparedAudioInputRuntimes)
            runtime->clear();
    }

    VisualInputRuntime* preparedVisualInputRuntimeForGraphIndex(
        int graphModuleIndex) const noexcept
    {
        return preparedAudioRenderer != nullptr
            ? preparedAudioRenderer->visualInputRuntime(graphModuleIndex)
            : nullptr;
    }

    VisualInputRuntime* preparedVisualInputRuntimeForApgIndex(
        int moduleIndex) const noexcept
    {
        if (moduleIndex < 0)
            return nullptr;
        const int mappedGraphIndex = moduleIndex < static_cast<int>(
                bytecodeToGraphIdx.size())
            ? bytecodeToGraphIdx[static_cast<std::size_t>(moduleIndex)]
            : -1;
        const int graphIndex = mappedGraphIndex >= 0
            ? mappedGraphIndex : moduleIndex;
        return preparedVisualInputRuntimeForGraphIndex(graphIndex);
    }

    bool hasVisualInputForApgIndex(int moduleIndex) const noexcept
    {
        if (preparedVisualInputRuntimeForApgIndex(moduleIndex) != nullptr)
            return true;
        return moduleIndex >= 0
            && moduleIndex < static_cast<int>(apgVisualInputs.size())
            && apgVisualInputs[static_cast<std::size_t>(moduleIndex)] != nullptr;
    }

    void setPreparedVisualInputPublishingEnabled(bool enabled) const noexcept
    {
        if (preparedAudioRenderer == nullptr)
            return;
        for (std::size_t boundary = 0;
             boundary < preparedAudioRenderer->visualInputBoundaryCount();
             ++boundary) {
            const int graphIndex =
                preparedAudioRenderer->visualInputBoundaryGraphIndex(boundary);
            if (auto* runtime = preparedAudioRenderer
                    ->visualInputRuntime(graphIndex))
                runtime->setPublishingEnabled(enabled);
        }
    }

    std::size_t preparedHostMidiInputCount() const noexcept
    {
        return preparedAudioRenderer != nullptr
            ? preparedAudioRenderer->hostMidiInputBoundaryCount()
            : preparedHostMidiRuntimes.size();
    }

    int preparedHostMidiInputGraphIndex(std::size_t boundary) const noexcept
    {
        return preparedAudioRenderer != nullptr
            ? preparedAudioRenderer->hostMidiInputBoundaryGraphIndex(boundary)
            : boundary < preparedHostMidiRuntimeGraphIndices.size()
                ? preparedHostMidiRuntimeGraphIndices[boundary] : -1;
    }

    bool bindPreparedHostMidiSource(
        int graphModuleIndex,
        const HostMidiNoteState* notes,
        const std::atomic<float>* ccs,
        int channels,
        int ccCount,
        const HostMidiBlockState* blockState) const noexcept
    {
        for (std::size_t runtime = 0;
             runtime < preparedHostMidiRuntimes.size(); ++runtime)
            if (runtime < preparedHostMidiRuntimeGraphIndices.size()
                && preparedHostMidiRuntimeGraphIndices[runtime]
                    == graphModuleIndex) {
                preparedHostMidiRuntimes[runtime]->bindSource(
                    notes, ccs, channels, ccCount, blockState);
                return true;
            }
        return false;
    }

    bool bindPreparedHostMidiPlanHook(
        int graphModuleIndex,
        void* context,
        HostMidiInputRuntime::PlanHook hook) const noexcept
    {
        for (std::size_t runtime = 0;
             runtime < preparedHostMidiRuntimes.size(); ++runtime)
            if (runtime < preparedHostMidiRuntimeGraphIndices.size()
                && preparedHostMidiRuntimeGraphIndices[runtime]
                    == graphModuleIndex) {
                preparedHostMidiRuntimes[runtime]->bindPlanHook(
                    context, hook, graphModuleIndex);
                return true;
            }
        return false;
    }

    void clearPreparedHostMidiBindings() const noexcept
    {
        for (auto& runtime : preparedHostMidiRuntimes) {
            runtime->clearSource();
            runtime->clearPlanHook();
            runtime->clearPreparedCandidateBlock();
        }
    }

    void clearPreparedHostMidiPlanHooks() const noexcept
    {
        for (auto& runtime : preparedHostMidiRuntimes)
            runtime->clearPlanHook();
    }

    MidiOutputRuntime* preparedMidiOutputRuntimeForGraphIndex(
        int graphModuleIndex) const noexcept
    {
        for (std::size_t runtime = 0;
             runtime < preparedMidiOutputRuntimes.size(); ++runtime)
            if (runtime < preparedMidiOutputRuntimeGraphIndices.size()
                && preparedMidiOutputRuntimeGraphIndices[runtime]
                    == graphModuleIndex)
                return preparedMidiOutputRuntimes[runtime].get();
        return nullptr;
    }

    MidiOutputRuntime* preparedMidiOutputRuntimeForApgIndex(
        int moduleIndex) const noexcept
    {
        if (moduleIndex < 0)
            return nullptr;
        for (std::size_t runtime = 0;
             runtime < preparedMidiOutputRuntimes.size(); ++runtime)
            if (runtime < preparedMidiOutputRuntimeApgIndices.size()
                && preparedMidiOutputRuntimeApgIndices[runtime] == moduleIndex)
                return preparedMidiOutputRuntimes[runtime].get();
        const int mappedGraphIndex = moduleIndex < static_cast<int>(
                bytecodeToGraphIdx.size())
            ? bytecodeToGraphIdx[static_cast<std::size_t>(moduleIndex)]
            : -1;
        const int graphIndex = mappedGraphIndex >= 0
            ? mappedGraphIndex : moduleIndex;
        return preparedMidiOutputRuntimeForGraphIndex(graphIndex);
    }

    void bindPreparedMidiOutputRouteManager(
        MidiRouteManager* manager) const noexcept
    {
        for (auto& runtime : preparedMidiOutputRuntimes)
            runtime->bindMidiRouteManager(manager);
    }

    void clearPreparedMidiOutputBindings() const noexcept
    {
        for (auto& runtime : preparedMidiOutputRuntimes)
            runtime->clearRouteBinding();
    }

    void tombstoneMidiOutputs(
        juce::MidiBuffer& midi, bool deliverLocalMidi) const noexcept
    {
        // The message thread may publish clipId=-1 one callback before its
        // queued global panic becomes visible. The first tombstone callback
        // must therefore release hardware-routed notes itself, while the
        // route is still attached, and every later callback must be inert.
        if (! midiOutputsTombstoned.exchange(
                true, std::memory_order_acq_rel)) {
            for (auto& runtime : preparedMidiOutputRuntimes)
                runtime->tombstone(midi, deliverLocalMidi);
            for (auto* processor : apgMidiOutputs)
                if (processor != nullptr)
                    processor->tombstone(midi, deliverLocalMidi);
        }
        clearPreparedMidiOutputBindings();
        for (auto* processor : apgMidiOutputs)
            if (processor != nullptr)
                processor->bindMidiRoute(nullptr, nullptr);
    }

    void panicMidiOutputs(juce::MidiBuffer& midi) const noexcept
    {
        for (auto& runtime : preparedMidiOutputRuntimes)
            runtime->panic(midi);
        for (int module = 0;
             module < static_cast<int>(apgMidiOutputs.size()); ++module)
            if (preparedMidiOutputRuntimeForApgIndex(module) == nullptr)
                if (auto* processor = apgMidiOutputs[
                        static_cast<std::size_t>(module)])
                    processor->panic(midi);
    }

    bool consumeMidiOutputTelemetry(
        int moduleIndex, bool& active, bool& generated) const noexcept
    {
        if (auto* runtime = preparedMidiOutputRuntimeForApgIndex(moduleIndex)) {
            active = runtime->noteActive();
            generated = runtime->consumeGeneratedActivityTick();
            return true;
        }
        if (moduleIndex < 0
            || moduleIndex >= static_cast<int>(apgMidiOutputs.size()))
            return false;
        auto* processor = apgMidiOutputs[
            static_cast<std::size_t>(moduleIndex)];
        if (processor == nullptr)
            return false;
        active = processor->noteActive();
        generated = processor->consumeGeneratedActivityTick();
        return true;
    }

    bool hasAudioRenderer() const noexcept {
        return (preparedAudioRenderer != nullptr
                && preparedAudioRendererEnabled.load(std::memory_order_acquire))
#if CURLOP_ENABLE_LEGACY_APG_ORACLE
            || apgGraph != nullptr
            || (compiledRenderPlan != nullptr
            && compiledWorkerTeam != nullptr)
#endif
            ;
    }

    // A timed-out compiled callback may return while a worker still owns a
    // region context in this bundle. The ordinary audio-block fence protects
    // callback readers; retirement must additionally wait for that wave.
    bool compiledWorkerWaveComplete() const noexcept {
#if CURLOP_ENABLE_LEGACY_APG_ORACLE
        return compiledWorkerTeam == nullptr || compiledWorkerTeam->completed();
#else
        return true;
#endif
    }

    bool processAudio(juce::AudioBuffer<float>& buffer,
                      juce::MidiBuffer& midi) const noexcept {
        if (preparedAudioRenderer != nullptr
            && preparedAudioRendererEnabled.load(std::memory_order_acquire)) {
            if (preparedAudioRenderer->processAudio(buffer, midi)) {
                preparedAudioRenderedLastBlock.store(
                    true, std::memory_order_release);
                return true;
            }
            // A unified renderer must either render this callback or produce
            // an explicit silent block. It must not hand a live TC1 graph to
            // the retained APG/compiled compatibility paths, which may own
            // stateful host and VM boundaries differently. Do not disable the
            // renderer: its rejection is per-block and the next bound block
            // may be valid.
            buffer.clear();
            preparedAudioRenderedLastBlock.store(false,
                                                  std::memory_order_release);
            return true;
        }
#if CURLOP_ENABLE_LEGACY_APG_ORACLE
        if (compiledRenderPlan != nullptr && compiledWorkerTeam != nullptr) {
            const bool rendered = compiledRenderPlan->processEligibleWave(
                *compiledWorkerTeam, buffer, buffer.getNumSamples());
            preparedAudioRenderedLastBlock.store(false, std::memory_order_release);
            return rendered;
        }
#endif
#if CURLOP_ENABLE_LEGACY_APG_ORACLE
        if (apgGraph != nullptr) {
            apgGraph->processBlock(buffer, midi);
            // Publish observer ownership only after the fallback block is
            // complete. A 120 Hz drain racing this callback therefore sees
            // the latest completed block, never an in-flight placeholder.
            preparedAudioRenderedLastBlock.store(false, std::memory_order_release);
            return true;
        }
#endif
        preparedAudioRenderedLastBlock.store(false, std::memory_order_release);
        return false;
    }

    bool bindPreparedRendererControls(
        int graphModuleIndex, const float* rows, int rowStride,
        const int* rowMap, int rowCount, int sampleCount) const noexcept
    {
        return preparedAudioRenderer != nullptr
            && preparedAudioRendererEnabled.load(std::memory_order_acquire)
            && (! preparedAudioRenderer->acceptsModuleControls(
                    graphModuleIndex)
                || preparedAudioRenderer->bindModuleControls(
                    graphModuleIndex, rows, rowStride, rowMap, rowCount,
                    sampleCount));
    }

    // Audio-thread tap copy. Candidate lookup is a prepared integer index;
    // there is no string search, allocation, resize or lock in this path.
    bool copyModuleTapToBuffer(
        int moduleIndex, int requestedWidth,
        juce::AudioBuffer<float>& destination, int destinationChannel,
        int sampleCount) const noexcept
    {
        if (moduleIndex < 0 || requestedWidth <= 0 || sampleCount <= 0
            || destinationChannel < 0
            || destinationChannel + requestedWidth
                > destination.getNumChannels())
            return false;

        const int samples = std::min(sampleCount, destination.getNumSamples());
        if (preparedAudioRenderer != nullptr
            && preparedAudioRenderedLastBlock.load(std::memory_order_acquire)) {
            if (moduleIndex >= static_cast<int>(
                    preparedTapByModuleIndex.size()))
                return false;
            const int tap = preparedTapByModuleIndex[
                static_cast<std::size_t>(moduleIndex)];
            if (tap < 0
                || requestedWidth
                    > preparedAudioRenderer->tapChannelCount(
                        static_cast<std::size_t>(tap)))
                return false;
            for (int channel = 0; channel < requestedWidth; ++channel) {
                auto* write = destination.getWritePointer(
                    destinationChannel + channel);
                for (int sample = 0; sample < samples; ++sample)
                    write[sample] = preparedAudioRenderer->tapSample(
                        static_cast<std::size_t>(tap), channel, sample);
            }
            return true;
        }

        if (moduleIndex >= static_cast<int>(apgMeters.size()))
            return false;
        const auto* meter = apgMeters[static_cast<std::size_t>(moduleIndex)];
        if (meter == nullptr)
            return false;
        const auto& capture = meter->captureBuffer();
        const int captured = std::min(samples, meter->captureSamples());
        for (int channel = 0; channel < requestedWidth; ++channel) {
            const int target = destinationChannel + channel;
            if (channel < capture.getNumChannels() && captured > 0) {
                destination.copyFrom(
                    target, 0, capture, channel, 0, captured);
                if (captured < samples)
                    destination.clear(target, captured, samples - captured);
            } else {
                destination.clear(target, 0, samples);
            }
        }
        return true;
    }

    bool preparedRendererOwnsLatestBlock() const noexcept
    {
        return preparedAudioRenderer != nullptr
            && preparedAudioRenderedLastBlock.load(std::memory_order_acquire);
    }

    int preparedTapForModule(int moduleIndex) const noexcept
    {
        if (preparedAudioRenderer == nullptr || moduleIndex < 0
            || moduleIndex >= static_cast<int>(preparedTapByModuleIndex.size()))
            return -1;
        return preparedTapByModuleIndex[static_cast<std::size_t>(moduleIndex)];
    }

    bool exchangePreparedOutputMeter(
        int moduleIndex, float& peak, float& rms) const noexcept
    {
        const int tap = preparedTapForModule(moduleIndex);
        return tap >= 0 && preparedAudioRenderer->exchangeTapOutputMeter(
            static_cast<std::size_t>(tap), peak, rms);
    }

    bool exchangePreparedInputMeter(
        int moduleIndex, float& peak, float& rms) const noexcept
    {
        const int tap = preparedTapForModule(moduleIndex);
        return tap >= 0 && preparedAudioRenderer->exchangeTapInputMeter(
            static_cast<std::size_t>(tap), peak, rms);
    }

    bool exchangePreparedClip(int moduleIndex, float& clip) const noexcept
    {
        const int tap = preparedTapForModule(moduleIndex);
        return tap >= 0 && preparedAudioRenderer->exchangeTapClip(
            static_cast<std::size_t>(tap), clip);
    }

    std::size_t preparedFaceplateMeterCount(int moduleIndex) const noexcept
    {
        const int tap = preparedTapForModule(moduleIndex);
        return tap >= 0 ? preparedAudioRenderer->tapFaceplateMeterCount(
            static_cast<std::size_t>(tap)) : 0;
    }

    std::string_view preparedFaceplateMeterId(
        int moduleIndex, std::size_t meter) const noexcept
    {
        const int tap = preparedTapForModule(moduleIndex);
        return tap >= 0 ? preparedAudioRenderer->tapFaceplateMeterId(
            static_cast<std::size_t>(tap), meter) : std::string_view();
    }

    float preparedFaceplateMeterValue(
        int moduleIndex, std::size_t meter) const noexcept
    {
        const int tap = preparedTapForModule(moduleIndex);
        return tap >= 0 ? preparedAudioRenderer->tapFaceplateMeterValue(
            static_cast<std::size_t>(tap), meter) : 0.0f;
    }

    bool exchangePreparedControlOutputRange(
        int moduleIndex, std::size_t output,
        float& minimum, float& maximum, float& last) const noexcept
    {
        const int tap = preparedTapForModule(moduleIndex);
        if (tap >= 0
            && preparedAudioRenderer->exchangeTapControlOutputRange(
                static_cast<std::size_t>(tap), output,
                minimum, maximum, last))
            return true;
        const int module = apgModuleIndexForGraphIndex(moduleIndex);
        if (! directControlOnlyPublication
            || directControlOutputHolds == nullptr
            || module < 0
            || module >= static_cast<int>(directControlTelemetryOffsets.size())
            || directControlTelemetryOffsets[static_cast<std::size_t>(module)]
                == std::numeric_limits<std::size_t>::max()
            || module >= static_cast<int>(apgControlOutputNames.size())
            || output >= apgControlOutputNames[
                static_cast<std::size_t>(module)].size())
            return false;
        return directControlOutputHolds[
            directControlTelemetryOffsets[static_cast<std::size_t>(module)]
                + output].exchange(minimum, maximum, last);
    }

    // Non-owning per-module pointers, parallel-indexed by bytecode module idx.
    std::vector<ScriptNodeProcessor*> apgScriptNodes;
    std::vector<std::vector<ControlCoreProcessor*>> apgCores;
    std::vector<std::vector<std::unique_ptr<ControlCoreProcessor>>> apgOwnedCores;
    std::vector<MeterProcessor*> apgMeters;
    std::vector<MeterProcessor*> apgInputMeters;
    std::vector<std::vector<int>> apgControlInputExternalCounts;
    std::vector<std::vector<std::string>> apgControlInputNames;
    std::vector<std::vector<curlop::vm::ControlInput>> apgControlInputs;
    std::vector<int> apgControlInputChannelCounts;
    std::vector<std::vector<std::string>> apgControlOutputNames;
    std::vector<std::vector<ControlMeterProcessor*>> apgControlOutputMeters;
    std::vector<VisualInputProcessor*> apgVisualInputs;
    std::vector<int> apgAudioOutputWidths;
    std::vector<std::pair<int, int>> apgRecordingTopologyEdges;
    std::vector<int> apgModuleLatencySamples;
    std::vector<int> apgModulePathLatencySamples;
    int apgGraphLatencySamples = 0;
    std::vector<int> apgAudioInputStartChannels;
    std::vector<std::vector<curlop::vm::SignalType>> apgControlOutputTypes;
    // B-232 Phase 1b: per-module brick-wall limiter pointer (non-null only
    // for the output module). Drained by CurlopProcessor::drainSlotMeters
    // to feed the CLIP LED via meterAccumulator.
    std::vector<ModuleBuilder::LevelGainProcessor*> apgOutputLimiters;
    std::vector<FaustNode*> apgFaustNodes;
    std::vector<StepSequencerProcessor*> apgStepSequencers;
    std::vector<TransportClockProcessor*> apgTransportClocks;
    std::vector<HostMidiInputProcessor*> apgHostMidiInputs;
    std::vector<HostAutomationInputProcessor*> apgHostAutomationInputs;
    std::vector<AudioInputEndpointProcessor*> apgAudioInputEndpoints;
    std::vector<std::uint32_t> apgAudioInputChannelIndices;
    std::vector<MidiOutputProcessor*> apgMidiOutputs;
    std::vector<std::string> apgMidiInputRouteIds;
    std::vector<std::string> apgMidiOutputRouteIds;
    std::vector<BufferPlayerProcessor*> apgBufferPlayers;

    // Graph topology — for live edge reconcile (setGraphEdges).
    std::vector<juce::AudioProcessorGraph::NodeID> apgNodeIDs;
    // Prepared outer endpoint per module. This is distinct from apgNodeIDs:
    // feedback-SCC members may share one composite outer node while retaining
    // their own declared port offsets.
    std::vector<ApgModuleEndpoint> apgModuleEndpoints;
    juce::AudioProcessorGraph::NodeID apgAudioOutNodeID {};
    ApgWireMap apgWires;
    ApgFeedbackRouteMap apgFeedbackRoutes;
    ApgControlWireMap apgControlWires;
    // B-1732: graph-indexed signal route authority and its prepared mutable
    // block storage publish and retire as part of this same coherent bundle.
    std::unique_ptr<transport::PreparedSignalSchedule> signalSchedule;
    // FF-032 Section 6: off-thread topology classification for a future
    // callback-safe executor. The current APG renderer remains serial until a
    // candidate passes the multicore selection matrix.
    std::unique_ptr<transport::PreparedExecutionPlan> executionPlan;

    int apgModuleCount = 0;
    std::vector<juce::String> moduleDslNames;
    std::vector<std::string> moduleNodeIds;
    std::vector<std::string> moduleLineageIds;
    // B-1736: node-qualified identity for exact native packet publishers.
    // Zero means this module has no prepared native output adapter.
    std::vector<curlop::vm::SourceIdentity>
        preparedNativeSourceIdentities;
    std::size_t preparedPacketCapacity =
        static_cast<std::size_t>(kEventVectorReserve);
    std::vector<ModuleLayout> layouts;
    std::vector<int> bytecodeToGraphIdx;

    // Grow the five module-indexed vectors to at least newSize entries.
    // Called by the bundle BUILDER (off-thread); the bundle is then
    // published whole via atomic exchange. Same shape as the old
    // EngineSlot::ensureApgCapacity which it replaces.
    void ensureCapacity(int newSize) {
        const int cur = (int)apgScriptNodes.size();
        if (newSize <= cur) return;
        apgScriptNodes  .resize(newSize, nullptr);
        apgCores        .resize(newSize);
        apgOwnedCores   .resize(newSize);
        apgMeters       .resize(newSize, nullptr);
        apgInputMeters  .resize(newSize, nullptr);
        apgControlInputExternalCounts.resize(newSize);
        apgControlInputNames.resize(newSize);
        apgControlInputs.resize(newSize);
        apgControlInputChannelCounts.resize(newSize, 0);
        apgControlOutputNames.resize(newSize);
        apgControlOutputMeters.resize(newSize);
        apgAudioOutputWidths.resize(newSize, 0);
        apgModuleLatencySamples.resize(newSize, 0);
        apgModulePathLatencySamples.resize(newSize, 0);
        apgAudioInputStartChannels.resize(newSize, 0);
        apgControlOutputTypes.resize(newSize);
        apgOutputLimiters.resize(newSize, nullptr);
        apgFaustNodes.resize(newSize, nullptr);
        apgStepSequencers.resize(newSize, nullptr);
        apgTransportClocks.resize(newSize, nullptr);
        apgHostMidiInputs.resize(newSize, nullptr);
        apgHostAutomationInputs.resize(newSize, nullptr);
        apgAudioInputEndpoints.resize(newSize, nullptr);
        apgAudioInputChannelIndices.resize(newSize, 0);
        apgMidiOutputs.resize(newSize, nullptr);
        apgMidiInputRouteIds.resize(newSize);
        apgMidiOutputRouteIds.resize(newSize);
        apgBufferPlayers.resize(newSize, nullptr);
        moduleNodeIds.resize(newSize);
        moduleLineageIds.resize(newSize);
        preparedNativeSourceIdentities.resize(newSize, 0u);
        apgNodeIDs      .resize(newSize, juce::AudioProcessorGraph::NodeID{});
        apgModuleEndpoints.resize(newSize);
        layouts         .resize(newSize);
        bytecodeToGraphIdx.resize(newSize, -1);
    }

    int apgCapacity() const { return static_cast<int>(apgNodeIDs.size()); }
};

using ApgBundleRetireFn = std::function<void(ApgBundle*)>;

struct EngineSlotResetTiming {
    double totalUs = 0.0;
    double apgBundleRetireUs = 0.0;
    double vmStateUs = 0.0;
    double paramPipelineUs = 0.0;
    double processDataUs = 0.0;
    double eventBridgeUs = 0.0;
    double layoutAndBufferStoreUs = 0.0;
    double miscUs = 0.0;
};

// ── Slot lifecycle states ────────────────────────────────────────────────
enum class SlotState : uint8_t {
    Free,        // Available. All state cleared.
    Compiling,   // JS building graph + bytecode (off audio thread).
    Ready,       // Compiled + pre-warmed, waiting for quantize boundary.
    Active,      // Currently producing audio. VM dispatches here.
    Decaying,    // GATE=0 sent, tail fading, still rendering on worker thread.
    PendingFree  // Audio thread finished with slot; awaiting message-thread reset (B-154).
};

// ── Swap quantize modes (Phase 44: REQ-SWAP-01..04) ─────────────────────
enum class SwapMode : uint8_t {
    Loop = 0,       // swap at loop wrap (default)
    Bar  = 1,       // swap at bar boundary
    Beat = 2,       // swap at beat boundary
    Immediate = 3   // swap at next processBlock
};

// ── Per-slot MIDI CC mapping (moved from VMRunner) ───────────────────────
struct MidiCCMapping {
    int8_t channel = -1;
    uint8_t cc = 0;
    float minVal = 0.0f;
    float maxVal = 1.0f;
    bool isOffset = false;
    bool isActive() const { return channel >= 0; }
};

// ── Per-slot graph mappings (same shape as CurlopProcessor::DeckMappings) ─
struct SlotMappings {
    struct ParamSlot {
        std::uint32_t module = 0;
        std::uint32_t paramType = 0;
    };

    std::unordered_map<std::string, int32_t> nodeKeyMap;
    std::unordered_map<std::string, ParamSlot> keyToParamSlot;
    std::unordered_map<int32_t, ParamSlot> nodeIdToParamSlot;
    std::unordered_map<int32_t, std::string> nodeIdToKey;
    std::unordered_map<std::string, int> graphDslNameToIndex;
    std::atomic<int> graphModuleCount { 0 };

    void clear() {
        nodeKeyMap.clear();
        keyToParamSlot.clear();
        nodeIdToParamSlot.clear();
        nodeIdToKey.clear();
        graphDslNameToIndex.clear();
        graphModuleCount.store(0, std::memory_order_relaxed);
    }
};

// ── EngineSlot ───────────────────────────────────────────────────────────
// Self-contained audio slot. Bytecode, graph runtime, param state, and
// audio buffers are born together and die together. No shared mutable
// state between slots — eliminates B-071, B-079, B-080, I-043 by design.
struct EngineSlot {
    std::atomic<std::size_t> preparedPacketCapacityRequested {
        static_cast<std::size_t>(kEventVectorReserve)
    };

    std::size_t preparedPacketCapacityForBuild() const noexcept
    {
        return preparedPacketCapacityRequested.load(
            std::memory_order_acquire);
    }

    std::size_t requestPreparedPacketCapacityGrowth() noexcept
    {
        auto current = preparedPacketCapacityRequested.load(
            std::memory_order_relaxed);
        for (;;) {
            if (current
                > std::numeric_limits<std::size_t>::max() / 2u)
                return current;
            const auto grown = current * 2u;
            if (preparedPacketCapacityRequested.compare_exchange_weak(
                    current, grown,
                    std::memory_order_release,
                    std::memory_order_relaxed))
                return grown;
        }
    }

    int slotId = -1;
    // Thread safety: written by main thread (BridgeMessageHandler),
    // read by audio thread (VMRunner::processBlock swap check).
    // std::atomic with release/acquire ensures all slot state written before the
    // transition is visible to the audio thread when it reads the new state.
    std::atomic<SlotState> state { SlotState::Free };

    // ── Paired lifecycle: compiled together, installed together ──
    //
    // T-375 (D-APG-1): the slot's whole APG state — graph + per-module
    // metadata + nodeIDs + wires — lives behind a single atomic pointer.
    // Every topology change (clip switch / module add or remove / faust
    // schema change) builds a fresh ApgBundle off-thread and publishes it
    // via exchange; the audio thread acquire-loads once per processBlock
    // (apgBundleAcquire) and reads everything through the bundle. The
    // displaced bundle is retired through CurlopProcessor::retireApgBundle
    // with a `safeAtSeq = audioBlockSeq + 2` fence (T-374 retire queue;
    // now retires the whole bundle rather than just the graph).
    //
    // Replaces the prior pair: `apgGraph` (atomic graph pointer, T-374)
    // plus a dozen parallel-array fields written under callbackLock after
    // the swap. That layout had a multi-block window where the audio
    // thread saw new graph + stale arrays; bundling them eliminates the
    // window (single pointer publish). D-APG-1 audit closes here.
    std::atomic<ApgBundle*> apgBundle { nullptr };
    std::atomic<std::uint64_t> apgPublicationGeneration { 1 };

    // Host context changes and graph rebuilds occur off the audio callback.
    // Keeping the latest workgroup at slot scope lets a newly built compiled
    // bundle join the same host group before publication.
    void setAudioWorkgroup(juce::AudioWorkgroup group)
    {
#if CURLOP_ENABLE_LEGACY_APG_ORACLE
        if (auto* bundle = apgBundle.load(std::memory_order_acquire);
            bundle != nullptr && bundle->compiledWorkerTeam != nullptr)
            // This call can originate on the host audio thread. The team
            // publishes with a double-buffered lock-free handoff.
            bundle->compiledWorkerTeam->setWorkgroup(group);
#endif
        // The cached value is only for off-thread future-plan construction.
        // Never block the host callback behind an off-thread graph build.
        if (audioWorkgroupMutex.try_lock()) {
            audioWorkgroup = std::move(group);
            audioWorkgroupMutex.unlock();
        }
    }

    juce::AudioWorkgroup getAudioWorkgroup() const
    {
        std::lock_guard<std::mutex> lock(audioWorkgroupMutex);
        return audioWorkgroup;
    }

    /** Acquire-load the live ApgBundle pointer. Returns nullptr when the
     *  slot has no graph installed. The audio thread MUST cache the
     *  result for the duration of a processBlock — re-loading mid-block
     *  re-introduces the very inconsistency the bundle exists to fix.
     *  T-375. */
    const ApgBundle* apgBundleAcquire() const noexcept {
        return apgBundle.load(std::memory_order_acquire);
    }

    // Production publishers exchange complete immutable bundles through this
    // seam so host readback can observe every new publication exactly once.
    // Tests may provide a nonzero generation explicitly for a fixed oracle.
    ApgBundle* exchangeApgBundle(ApgBundle* replacement) noexcept;

private:
    mutable std::mutex audioWorkgroupMutex;
    juce::AudioWorkgroup audioWorkgroup;

public:

    /** Bundle-aware moduleDslNames accessor. Returns the live bundle's
     *  names vector when one is installed, or a stable empty vector
     *  otherwise (slot before its first BYTE). Used in the BYTE-drain
     *  path where moduleDslNames is consulted before the per-clip
     *  applyFromGraphState builds the new bundle. T-375. */
    const std::vector<juce::String>& moduleDslNamesView() const noexcept {
        static const std::vector<juce::String> kEmpty;
        const auto* b = apgBundle.load(std::memory_order_acquire);
        return b ? b->moduleDslNames : kEmpty;
    }

    // ── F-066 Phase 5: per-module machine host ──────────────────────────
    //
    // One ModuleVm per graph module: the set of Machines driving it (one per
    // source script program that targets it — T-309: core.script nodes are
    // peer sequencers), the Delivery that composes their emissions onto the
    // module's declared inputs, and the rowMap that lands the composed
    // per-sample buffers on the EXISTING paramBuffer channel contract
    // (gate/pitch/vel rows + declared rows — the APG transport is graph
    // topology and survives this swap; see F-066-phase5-cp3-swap-edit-map).
    //
    // Lane namespacing: each source's lanes are compiler-local (baked into
    // bytecode operands), so the host namespaces them at composition time —
    // source k's lanes occupy [sourceLaneOffsets[k], +laneCount) in the
    // module's concatenated lane space; Delivery bindings are built against
    // the concatenated space and processMulti applies the per-set offset.
    struct ModuleVm {
        juce::String dslName;
        std::string nodeId;
        std::string sourceFingerprint;
        int graphIdx = -1;
        bool localOutputOwner = false;
        curlop::vm::SourceIdentity preparedNativeSourceIdentity = 0;

        // Parallel per-source arrays (index = position in the install's
        // source list; null machine = source doesn't target this module).
        std::vector<std::unique_ptr<curlop::vm::Machine>> machines;
        std::vector<curlop::vm::Delivery::SourceTimingContext> sourceTimingContexts;
        // Number of entries written this block; storage is sized at install
        // time so the audio callback never grows the context table.
        size_t sourceTimingContextCount = 0;
        std::vector<int> sourceOwnerGraphIdx;
        // B-1739: exact per-source StepClockPlan eligibility. Set only when a
        // self-owned local-output program successfully installs its authored
        // source-ordinal projection for this Step processor.
        std::vector<std::uint8_t> preparedStepMachine;
        // Unified-renderer owner for the native step timing contract. Its
        // input map is channel -> ParamStore schema index and is prepared off
        // the audio thread. APG's StepSequencerProcessor is not required.
        struct PreparedStepClockOwner {
            explicit PreparedStepClockOwner(
                curlop::StepClockRuntime::Mode mode)
                : runtime(mode), mode(mode) {}
            curlop::StepClockRuntime runtime;
            curlop::StepClockRuntime::Mode mode;
            std::vector<int> paramSchemaIdx;
        };
        struct PreparedTransportClockOwner {
            curlop::TransportClockRuntime runtime;
            std::array<int, curlop::TransportClockRuntime::InputCount>
                paramSchemaIdx { -1, -1 };
        };
        struct PreparedControlClockOwners {
            std::unique_ptr<PreparedStepClockOwner> step;
            std::unique_ptr<PreparedTransportClockOwner> transport;
        };
        // One pointer keeps ModuleVm compact: HostTransportPhase1Tests is a
        // legacy near-stack-limit fixture, and every additional inline field
        // multiplies across its many scoped ModuleVm locals.
        std::unique_ptr<PreparedControlClockOwners> preparedControlClocks;
        PreparedStepClockOwner* preparedStepClockOwner() noexcept
        {
            return preparedControlClocks != nullptr
                ? preparedControlClocks->step.get() : nullptr;
        }
        const PreparedStepClockOwner* preparedStepClockOwner() const noexcept
        {
            return preparedControlClocks != nullptr
                ? preparedControlClocks->step.get() : nullptr;
        }
        PreparedTransportClockOwner* preparedTransportClockOwner() noexcept
        {
            return preparedControlClocks != nullptr
                ? preparedControlClocks->transport.get() : nullptr;
        }
        const PreparedTransportClockOwner*
        preparedTransportClockOwner() const noexcept
        {
            return preparedControlClocks != nullptr
                ? preparedControlClocks->transport.get() : nullptr;
        }
        // B-1731: prepared, stable source namespace for Machine-local voice
        // ids. Interned from the persistent graph-node id rather than graph
        // index, source-list order, or source text, so reorder/recompile/index
        // reuse cannot alias note lifecycle. Zero is legacy/direct delivery.
        std::vector<curlop::vm::SourceIdentity> sourceIdentities;
        std::vector<uint32_t> sourceLaneOffsets;
        // SF-059 FOLLOW: per-source map from Machine::lastFiredStepOrdinal
        // (nth Step instr) to the source script's top-level step index.
        std::vector<std::vector<int>> stepSourceOrdinals;
        // T-687: source k tempo ratio. No explicit @bpm -> 1.0. Explicit
        // @bpm:X -> X / launchMasterBpm, then runtime source BPM follows the
        // current master/host BPM proportionally.
        std::vector<double> sourceBpmRatio;
        // T-519 — per-source tempo modulator (bpm:~lfo) + its live phase (beats,
        // master-clocked). kind==0 = fixed bpm. Phase persists across blocks.
        std::vector<curlop::script::BpmModulator> bpmMod;
        std::vector<double>                       bpmModPhaseBeats;
        // T-569 — per-source timescale modulator (top-level timescale:~lfo): a
        // tempo MULTIPLIER on srcBpm, evaluated per block in VMRunner, master-
        // clocked, phase persists across blocks. kind==0 = static timescale.
        std::vector<curlop::script::TimescaleModulator> tsMod;
        std::vector<double>                             tsModPhaseBeats;

        std::unique_ptr<curlop::vm::Delivery> delivery;
        curlop::vm::ModuleDeclaration declaration;   // synthesized from layout+schema
        std::vector<int> rowMap;                     // declaration input -> paramBuffer row (-1 = skip)
        std::vector<int> knobSchemaIdx;              // declaration input -> ParamStore schema index (-1 = note input)
        struct PreparedAuthoredBinding {
            int declarationInputIndex = -1;
            PreparedAuthoredParameterBinding binding;
        };
        // Prepared during graph application. VMRunner resolves only these
        // immutable target/timing values against the live BPM before the
        // ordinary knob sweep; no parser or GraphState access is audio-side.
        std::vector<PreparedAuthoredBinding> authoredBindings;
        std::vector<juce::String> laneParams;        // concatenated lane -> canonical param name
        std::vector<int> laneInputIndex;             // concatenated lane -> declaration input index (-1 = unbound)

        // B-1730: lifecycle-bearing graph packets keep their complete
        // Emission identity until this receiving ModuleVm's Delivery allocator.
        // Both tables are prepared with the APG bundle on the message thread;
        // the callback performs only indexed route writes/reads.
        struct PreparedOutgoingPacketRoute {
            std::size_t routeIndex = 0;
            std::uint32_t sourceLane = 0;
            curlop::vm::SourceIdentity sourceIdentity = 0;
            bool acceptsTransformedSourceIdentity = false;
        };
        struct PreparedOutgoingPacketFanout {
            std::size_t firstRoute = 0;
            std::size_t routeCount = 0;
            curlop::vm::SourceIdentity sourceIdentity = 0;
            bool acceptsTransformedSourceIdentity = false;
        };
        std::vector<PreparedOutgoingPacketRoute>
            preparedOutgoingPacketRoutes;
        // Indexed by decoded source-local lane. Each valid entry names one
        // contiguous range above, so callback publication is O(actual fanout)
        // rather than O(all graph routes) for every emission.
        std::vector<PreparedOutgoingPacketFanout>
            preparedOutgoingPacketFanoutByLane;
        std::vector<
            curlop::transport::PreparedSignalSchedule::PacketTargetBinding>
            preparedIncomingPacketRoutes;
        std::size_t preparedIncomingPacketCapacity = 0;
        std::vector<curlop::vm::Emission> preparedIncomingPacketScratch;
        std::uint64_t preparedPacketDrops = 0;
        std::size_t preparedLocalPacketCapacity = 0;
        std::size_t preparedIncomingTransformAmplification = 1;

        // ~midi routes (compile metadata — feeds host-synthesized emissions
        // from the live CC matrix; decided Neo s489).
        struct MidiLane {
            int cc = 1, channel = 0;
            float minNorm = -1.0f, maxNorm = 1.0f;   // normalized target range
            bool isOffset = false;
            uint32_t lane = 0;                        // concatenated lane id
        };
        std::vector<MidiLane> midiLanes;

        // Audio-thread per-block scratch — reserved at install (message thread).
        std::vector<curlop::vm::Emission> mergedScratch;
        std::vector<const std::vector<curlop::vm::StreamRow>*> streamSetScratch;
        std::vector<uint32_t> laneOffsetScratch;
        std::vector<const float*> externalInputRowsScratch;
        std::vector<uint32_t> mergeSortIdxScratch;
        std::vector<curlop::vm::Emission> mergeSortTmpScratch;
        // B-1733: the module-boundary phase lands Delivery before its APG
        // processor, then the whole-graph post phase consumes diagnostics,
        // display, and MIDI from the same prepared result.
        bool postRenderPending = false;
        // B-1739: per-slot prepared-boundary cascade state. Hook ownership is
        // fixed with the installed APG bundle; done is zeroed once per block.
        // Keeping both flags in the off-thread-built ModuleVm avoids callback
        // allocation while dependency-ready non-hook targets are drained.
        bool preparedBoundaryHookOwned = false;
        bool preparedBoundaryDone = false;

        // SPEC-010 tap counters (audio-thread only; diagnostic).
        uint64_t emsTotal      = 0;   // all emissions merged since install
        uint64_t valueEmsTotal = 0;   // Value-typed only (locks / ctrl routes)
        // B-278 GATE_EDGE forensic carry — last landed gate value per
        // declared input (sized at install, message thread; audio-thread
        // read/write only).
        std::vector<float> gateEdgeCarry;
    };

    // Audio-thread owned; swapped wholesale by applyInstall.
    std::vector<ModuleVm> moduleVms;
    // B-1732: prepared graph-index authority for the installed ModuleVm host.
    // The lookup is built on the message thread and swapped with moduleVms;
    // immutable transport records retain graph identity, never ModuleVm
    // pointers whose addresses change across installs.
    std::vector<int> moduleVmIndexByGraphIdx;

    int moduleVmIndexForGraphIdx(int graphIdx) const noexcept {
        return graphIdx >= 0
                && static_cast<size_t>(graphIdx) < moduleVmIndexByGraphIdx.size()
            ? moduleVmIndexByGraphIdx[static_cast<size_t>(graphIdx)]
            : -1;
    }

    ModuleVm* moduleVmForGraphIdx(int graphIdx) noexcept {
        const int moduleVmIdx = moduleVmIndexForGraphIdx(graphIdx);
        return moduleVmIdx >= 0
                && static_cast<size_t>(moduleVmIdx) < moduleVms.size()
            ? &moduleVms[static_cast<size_t>(moduleVmIdx)]
            : nullptr;
    }

    const ModuleVm* moduleVmForGraphIdx(int graphIdx) const noexcept {
        const int moduleVmIdx = moduleVmIndexForGraphIdx(graphIdx);
        return moduleVmIdx >= 0
                && static_cast<size_t>(moduleVmIdx) < moduleVms.size()
            ? &moduleVms[static_cast<size_t>(moduleVmIdx)]
            : nullptr;
    }

    // ── F-066 Phase 5: slot transport clock (replaces VMState.clock) ────
    // Loop length = the clip's primary loop (first source program's LOOP
    // header — the clockVmState() analog). VMRunner advances samplePosition
    // per block; loopCount derives at the wrap. Machines' frame clocks
    // advance in lockstep through process().
    double   loopLengthBeats   = 0.0;
    int64_t  loopLengthSamples = 0;
    int64_t  samplePosition    = 0;
    uint32_t loopCount         = 0;

    // ── F-066 Phase 5: install envelope ──────────────────────────────────
    // ONE envelope shape for every install: clip-level state (layouts, btg,
    // schema snapshots, processData sizing) + the COMPLETE rebuilt ModuleVm
    // vector (all sources). Node-script edits rebuild the whole vector from
    // the slot's message-thread sourceArtifacts — superseding installs
    // replace each other in the single pendingInstall slot.
    struct SequencerInstall {
        int clipId = -1;
        uint64_t authorityGeneration = 0;
        uint64_t projectAuthorityEpoch = 0;
        uint64_t clipSwitchTraceId = 0;
        // B-1746/B-1749: graph, prepared signal schedule, and ModuleVm route
        // bindings are one install generation. The candidate bundle is built
        // off-thread and exchanged by applyInstall beside the machine-host
        // swap; the displaced bundle rides the consumed envelope to safe
        // message-thread retirement.
        std::unique_ptr<ApgBundle> newApgBundle;
        std::unique_ptr<ApgBundle> retiredApgBundle;
        std::vector<ModuleLayout> layouts;
        std::vector<int> bytecodeToGraphIdx;
        int moduleCount = 0;
        size_t moduleCapacity = 0;

        struct SchemaSnapshot {
            uint32_t module_id = 0;
            std::string dsl_name;
            int voice_count = 1;
            std::vector<ParamDescriptor> schema;
            std::vector<float> shared_values;
        };
        std::vector<SchemaSnapshot> schemaSnapshots;
        std::vector<uint32_t> removedModuleIds;
        std::vector<ParamMapping> paramMappings;

        // Complete AudioParamState backing is prepared before this envelope is
        // published. applyInstall only swaps it into the callback-local state.
        AudioParamState::PreparedState preparedAudioParamState;
        bool hasPreparedAudioParamState = false;
        void prepareAudioParamState() {
            hasPreparedAudioParamState = false;
            if (schemaSnapshots.empty() && removedModuleIds.empty())
                return;
            AudioParamState::PreparedState prepared;
            // Match the old direct-apply order exactly: an explicit removal
            // wins if a malformed envelope names the same module twice.
            for (const auto moduleId : removedModuleIds)
                prepared.removeModule(moduleId);
            for (const auto& snap : schemaSnapshots)
                prepared.installSchemaSnapshot(
                    snap.module_id, snap.voice_count, snap.shared_values);
            preparedAudioParamState = std::move(prepared);
            hasPreparedAudioParamState = true;
        }

        bool hasLoopLength = false;
        double loop_length = 0.0;
        int64_t loop_length_samples = 0;

        std::vector<VMProcessData::ModuleState> newProcessDataModules;
        std::vector<VMProcessData::ModuleState> newOutgoingDataModules;
        void resizeNewProcessData(size_t numModules) {
            newProcessDataModules.assign(numModules, VMProcessData::ModuleState{});
            newOutgoingDataModules.assign(numModules, VMProcessData::ModuleState{});
        }
        void sizeNewProcessDataRows(const std::vector<ModuleVm>& moduleVms,
                                    int blockSize) {
            if (newProcessDataModules.empty()) return;
            const int bs = blockSize > 0 ? blockSize : VMProcessData::MAX_BLOCK;
            for (size_t m = 0; m < newProcessDataModules.size(); ++m) {
                int need = (m < layouts.size()) ? (int) layouts[m].totalParams : 0;
                int voices = (m < layouts.size()) ? (int) layouts[m].voices : 1;
                const int moduleVmIdx =
                    m < moduleVmIndexByGraphIdx.size()
                        ? moduleVmIndexByGraphIdx[m] : -1;
                if (moduleVmIdx >= 0
                    && static_cast<size_t>(moduleVmIdx) < moduleVms.size()) {
                    for (int row : moduleVms[static_cast<size_t>(moduleVmIdx)].rowMap)
                        need = std::max(need, row + 1);
                } else {
                    // This helper also accepts an independently prepared
                    // ModuleVm vector in characterization tests. Production
                    // envelopes prepare the dense lookup before calling it;
                    // preserve the helper's original graph-index semantics
                    // without manufacturing a callback-time fallback.
                    for (const auto& moduleVm : moduleVms) {
                        if (moduleVm.graphIdx != static_cast<int>(m))
                            continue;
                        for (int row : moduleVm.rowMap)
                            need = std::max(need, row + 1);
                        break;
                    }
                }
                const int rows = std::max(VMProcessData::MAX_PARAMS, need);
                const int vox = std::max(VMProcessData::MAX_PARAMS / 2, voices);
                newProcessDataModules[m].paramBuffer.reallocate(rows, bs);
                newProcessDataModules[m].voicePitch.assign((size_t) vox, 440.0f);
                if (m < newOutgoingDataModules.size()) {
                    newOutgoingDataModules[m].paramBuffer.reallocate(rows, bs);
                    newOutgoingDataModules[m].voicePitch.assign((size_t) vox, 440.0f);
                }
            }
        }

        std::shared_ptr<VMEventBridge> newEventBridge;
        std::shared_ptr<VMEventBridge> retiredEventBridge;
        void resizeNewEventBridge(size_t cap) {
            newEventBridge = std::make_shared<VMEventBridge>(cap);
        }

        // Bridge allocation and identity strings are message-thread work.
        // The callback must receive a fully normalized bridge.
        bool hasPreparedEventBridge = false;
        void prepareEventBridge() {
            size_t bridgeCount = moduleCount > 0
                ? static_cast<size_t>(moduleCount) : 0u;
            for (const auto& mv : newModuleVms)
                if (mv.graphIdx >= 0)
                    bridgeCount = std::max(
                        bridgeCount, static_cast<size_t>(mv.graphIdx) + 1u);
            for (const auto graphIdx : bytecodeToGraphIdx)
                if (graphIdx >= 0)
                    bridgeCount = std::max(
                        bridgeCount, static_cast<size_t>(graphIdx) + 1u);
            const size_t bridgeCapacity = std::max({
                bridgeCount, moduleCapacity, layouts.size(),
                bytecodeToGraphIdx.size() });
            if (bridgeCount > 0
                && (!newEventBridge
                    || newEventBridge->snapshotCapacity()
                        < static_cast<int>(bridgeCount)))
                resizeNewEventBridge(bridgeCapacity);
            if (newEventBridge) {
                newEventBridge->setModuleCount(static_cast<int>(bridgeCount));
                for (const auto& mv : newModuleVms) {
                    newEventBridge->setModuleNodeId(mv.graphIdx, mv.nodeId);
                    newEventBridge->setModuleSourceFingerprint(
                        mv.graphIdx, mv.sourceFingerprint);
                }
            }
            if (moduleCount == 0 && bridgeCount > 0)
                moduleCount = static_cast<int>(bridgeCount);
            if (moduleCapacity == 0 && bridgeCapacity > 0)
                moduleCapacity = bridgeCapacity;
            hasPreparedEventBridge = true;
        }

        void resizeNewLayout(size_t numModules) {
            layouts.assign(numModules, ModuleLayout{});
            bytecodeToGraphIdx.assign(numModules, -1);
        }

        // The machine host — complete, all sources.
        std::vector<ModuleVm> newModuleVms;
        std::vector<int> moduleVmIndexByGraphIdx;

        void prepareModuleVmLookup(size_t graphCapacity = 0) {
            size_t required = graphCapacity;
            for (const auto& moduleVm : newModuleVms)
                if (moduleVm.graphIdx >= 0)
                    required = std::max(
                        required,
                        static_cast<size_t>(moduleVm.graphIdx) + 1u);

            moduleVmIndexByGraphIdx.assign(required, -1);
            for (size_t moduleVmIdx = 0;
                 moduleVmIdx < newModuleVms.size();
                 ++moduleVmIdx) {
                const int graphIdx = newModuleVms[moduleVmIdx].graphIdx;
                if (graphIdx >= 0)
                    moduleVmIndexByGraphIdx[static_cast<size_t>(graphIdx)] =
                        static_cast<int>(moduleVmIdx);
            }
        }

        std::string scriptSource;

        // Legacy staged-install transport intent. User Play and Audition use
        // the independent START_TRANSPORT command so a rejected graph cannot
        // suppress transport.
        bool  startTransport = false;
        float bpm            = 0.0f;
        uint64_t transportStartSequence = 0;

        // Intrusive owner-transfer link for RT-safe deferred deletion. The
        // audio thread may hand consumed installs to a message-thread retire
        // stack without allocating a wrapper node.
        SequencerInstall* retireNext = nullptr;

        SequencerInstall() = default;
    };
    std::atomic<SequencerInstall*> pendingInstall { nullptr };

    // ── F-066 Phase 5: message-thread source mirror ──────────────────────
    // The compile artifacts that produced the current moduleVms — one per
    // source script (core.script node). stageNodeProgramOnSlot updates its
    // entry and rebuilds the full install envelope from this list.
    // MESSAGE-THREAD ONLY.
    struct SourceArtifact {
        std::string ownerLineage;                   // owning module lineage
        int ownerModuleIdx = -1;
        std::shared_ptr<const curlop::script::CompileResultV2> art;
        std::string sourceFingerprint;
        // Persistent graph-node instance identity. Unlike ownerModuleIdx and
        // ownerLineage/dslName, this survives reorder/edit and is not reused
        // when a deleted graph index is filled by a different source.
        std::string ownerNodeId;
        curlop::vm::SourceIdentity preparedSourceIdentity = 0;
    };
    std::vector<SourceArtifact> sourceArtifacts;

    // MESSAGE-THREAD ONLY. Interns persistent source node ids into POD values
    // that can travel through the audio-thread packet path without allocation.
    curlop::vm::SourceIdentity prepareSourceIdentity(
        const std::string& ownerNodeId);

    EngineSlot() {
        // T-327 (T-295 slice 3a): size module-indexed vectors to
        // kDefaultInitialModules immediately so the slot is usable before
        // any reset() / applyInstall. reset() re-assigns to clear values.
        publishEventBridge(std::make_shared<VMEventBridge>());
        processData.modules.assign(::curlop::kDefaultInitialModules, VMProcessData::ModuleState{});
        outgoingData.modules.assign(::curlop::kDefaultInitialModules, VMProcessData::ModuleState{});
        layouts       .assign(::curlop::kDefaultInitialModules, ModuleLayout{});
        bytecodeToGraphIdx.assign(::curlop::kDefaultInitialModules, -1);
    }

    ~EngineSlot() {
        // T-375: ApgBundle (owns graph + parallel arrays) is the unit
        // of atomic ownership. Slot teardown happens on the message thread
        // with no audio thread on the slot — direct delete via exchange.
        delete apgBundle.exchange(nullptr, std::memory_order_acq_rel);
        delete pendingInstall.exchange(nullptr, std::memory_order_acq_rel);
    }

    /** Per-clip module count seen by the install envelope's clip-graph-level
     *  state vectors (layouts / btg / midiCCMap / gateHoldSamples). NOT the
     *  APG module count — that lives on ApgBundle::apgModuleCount. */
    int moduleCapacity() const { return static_cast<int>(processData.modules.size()); }

    // ── Per-slot param pipeline (eliminates R-011 class) ──
    ParamStore paramStore;
    ParamFIFO paramFifo { 4096 };
    AudioParamState audioParamState;
    // Message-thread staging consumed by buildModuleVms while a replacement
    // bundle/install is prepared. moduleVms receive their own copies.
    std::vector<PreparedAuthoredParameterBinding> preparedAuthoredParameterBindings;

    // ── Per-slot VM execution state ──
    VMProcessData processData;
    VMProcessData outgoingData;  // frozen copy for decaying slot

    using EventBridgePtr = std::shared_ptr<VMEventBridge>;

    EventBridgePtr eventBridgeSnapshot() const
    {
        return std::atomic_load_explicit(&eventBridge, std::memory_order_acquire);
    }

    VMEventBridge* eventBridgeForAudio() const noexcept
    {
        return eventBridgeRaw.load(std::memory_order_acquire);
    }

    void publishEventBridge(EventBridgePtr bridge)
    {
        auto* raw = bridge.get();
        std::atomic_store_explicit(&eventBridge, std::move(bridge), std::memory_order_release);
        eventBridgeRaw.store(raw, std::memory_order_release);
    }

    // Callback install path: retain the old owning pointer inside the consumed
    // envelope, so its destructor runs only during message-thread retirement.
    EventBridgePtr exchangeEventBridgeForRetire(EventBridgePtr bridge) noexcept
    {
        auto* raw = bridge.get();
        auto displaced = std::atomic_exchange_explicit(
            &eventBridge, std::move(bridge), std::memory_order_acq_rel);
        eventBridgeRaw.store(raw, std::memory_order_release);
        return displaced;
    }

    // F-066 Phase 5: GUI display bridge (atomics snapshot — value + presence
    // per (module, param)). Written by VMRunner from Delivery state each
    // block; read by ParamStateReporter at 120 Hz. Shared ownership lets
    // message-thread reporters keep the old bridge alive across live installs.
    EventBridgePtr eventBridge;
    std::atomic<VMEventBridge*> eventBridgeRaw { nullptr };

    // ── Per-slot graph mappings (eliminates shadow swap race I-043) ──
    SlotMappings maps;
    std::vector<ModuleLayout> layouts;
    std::vector<int>          bytecodeToGraphIdx;
    std::vector<ParamMapping> paramMappings;
    std::unordered_map<std::string, std::shared_ptr<buffer::BufferStorage>> bufferStoresByNodeId;
    juce::File projectFolderForAssets;

    double lastProcessedBpm = 0.0;

    // ── Per-slot audio output ──
    juce::AudioBuffer<float> buffer;
    float tailFadeGain = 1.0f;
    int tailFadeRemaining = 0;
    int tailFadeTotal = 0;

    // ── Per-slot silence detection (REQ-TRANS-03) ──
    int silenceBlockCount = 0;  // consecutive blocks below -60dB threshold
    static constexpr int SILENCE_BLOCKS_THRESHOLD = 18;  // ~93ms at 48kHz/256
    static constexpr float SILENCE_THRESHOLD = 0.001f;   // -60dB ≈ 0.001 linear
    std::atomic<uint64_t> pendingFreeSafeAtSeq { 0 };

    // ── Per-slot hard stop ──
    float hardStopGain = 1.0f;
    int hardStopRampSamples = 0;

    // T-393 removed the old 32-block restart counter. Legacy staged installs
    // may still carry a transport intent; user launch uses the independent
    // START_TRANSPORT command and is intentionally not gated on installation.

    // ── Identity ──
    // Thread safety: written by main thread (BYTE handler), read by audio thread
    // (swap notify via callAsync captures clipId). Atomic prevents torn reads.
    std::atomic<int> clipId { -1 };
    std::atomic<uint64_t> authorityGeneration { 0 };
    std::atomic<uint64_t> projectAuthorityEpoch { 0 };
    uint64_t clipSwitchTraceId = 0;

    // ── Per-clip swap parameters (Phase 44: REQ-SWAP-01..04, REQ-TRANS-02) ──
    SwapMode swapMode = SwapMode::Loop;
    int fadeOutSamples = -1;   // -1 = default 4-beat fade; 0 = hard cut; >0 = explicit per-clip value in samples
    int beatsPerBar = 4;       // time signature numerator for bar-quantized swap

    /// Return slot to Free state, clearing all owned resources.
    void reset(const ApgBundleRetireFn& retireApgBundle = {});
    EngineSlotResetTiming lastResetTiming;

    // Clear project-owned compile/mapping state while retaining the Active
    // shell and its APG fade tail. Caller must serialize against processBlock.
    void clearProjectRuntimeSideTables();
    SequencerInstall* applyRuntimeTombstone(uint64_t generation,
                                            int rampSamples,
                                            SequencerInstall* emptyInstall);

    /// Render numBlocks of silence to settle el.sm() smoothers.
    ///
    /// ORDERING INVARIANT (non-negotiable):
    ///   build graph → set param values → prewarm → mark ready
    ///
    /// If prewarm runs before param values are applied via SET_PROPERTY,
    /// el.sm() smoothers settle to ZERO instead of target values, and
    /// B-079 (cold-start fade-in) returns. Callers must enforce this order.
    void prewarm(int numBlocks, int blockSize);

    /// F-066 Phase 5: build the complete ModuleVm vector for this slot from
    /// its message-thread sourceArtifacts + the given layouts + the
    /// paramStore schemas. MESSAGE-THREAD ONLY (allocates; prepares
    /// Machines + Deliveries). `layoutsSrc` may be the slot's own layouts
    /// or an install's pending layouts.
    std::vector<ModuleVm> buildModuleVms(const std::vector<ModuleLayout>& layoutsSrc,
                                         const std::vector<juce::String>& dslNames,
                                         double sampleRate,
                                         double launchMasterBpm,
                                         const ApgBundle* preparedBundle = nullptr) const;

    /// F-066 Phase 5: rebuild the machine host from sourceArtifacts and
    /// stage it (node-script edit path; clip-level fields preserved).
    /// MESSAGE-THREAD ONLY. Supersedes any prior staged install.
    void stageMachineHostRebuild(double sampleRate,
                                 bool startTransport = false, float bpm = 0.0f,
                                 const std::string& scriptSource = std::string(),
                                 const std::vector<ModuleLayout>* layoutOverride = nullptr,
                                 const ApgBundle* preparedBundle = nullptr);

    /// F-066 Phase 5: install a SequencerInstall envelope into this slot.
    /// Caller transfers ownership of `pi`. Message-thread callers normally
    /// let applyInstall delete the consumed envelope inline. Audio-thread
    /// callers pass DeleteConsumedInstall::No and retire the returned pointer
    /// through a message-thread queue, because the envelope can own vectors,
    /// Machines/Deliveries, and bridge refs whose destructors may free.
    /// Clip-level fields apply when present (empty = preserve); moduleVms
    /// swaps wholesale, with surviving modules' Delivery state carried across
    /// (graphIdx-keyed) so in-flight gates and locks survive a live edit.
    enum class DeleteConsumedInstall : uint8_t { Yes, No };
    bool installMatchesRuntimeAuthority(const SequencerInstall& install) const noexcept
    {
        return install.clipId == clipId.load(std::memory_order_acquire)
            && install.authorityGeneration
                == authorityGeneration.load(std::memory_order_acquire)
            && install.projectAuthorityEpoch
                == projectAuthorityEpoch.load(std::memory_order_acquire);
    }
    SequencerInstall* applyInstall(SequencerInstall* pi,
                                   DeleteConsumedInstall deleteConsumed = DeleteConsumedInstall::Yes);

    // Test seam for the only fallible phase of an audio-thread publication.
    // The injected failure occurs before any live slot state changes.
    static void setInstallPreparationThrowForTests(bool enabled) noexcept;

private:
    void prepareInstallStateMigration(SequencerInstall& install);
    std::unordered_map<std::string, curlop::vm::SourceIdentity>
        sourceIdentityByNodeId_;
    curlop::vm::SourceIdentity nextSourceIdentity_ = 1;
};

} // namespace curlop
