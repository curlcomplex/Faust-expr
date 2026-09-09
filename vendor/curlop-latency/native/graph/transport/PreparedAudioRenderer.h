#pragma once

#include "control/vm/core/AudioInputRuntime.h"
#include "control/vm/core/HostMidiInputRuntime.h"
#include "control/vm/core/MidiOutputRuntime.h"
#include "graph/transport/VisualInputRuntime.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <cmath>
#include <atomic>
#include <cstddef>
#include <string_view>

namespace curlop::transport {

inline constexpr int kPublishedRendererRetirementFenceBlocks = 2;

inline bool preparedControlSamplesShareRun(float left, float right) noexcept
{
    return left == right
        || (! std::isfinite(left) && ! std::isfinite(right));
}

enum class RendererParameterStatePolicy {
    RebindCurrentRowsBeforeFirstBlock
};

enum class RendererInternalStatePolicy {
    ResetOnReplacement
};

enum class RendererTransitionPolicy {
    AtomicHardSwapWithDeferredReclamation
};

// Rebuilds are host lifecycle events, not an implicit Faust capability.
// Current control values can be rebound to a fresh instance, but Faust does
// not transfer its running delay/envelope/oscillator/feedback state. The host
// currently performs the same atomic hard swap used by APG and retains the
// displaced bundle only until callback readers clear this reclamation fence.
// No production tail or crossfade owner is claimed by this policy.
struct RendererReplacementPolicy {
    RendererParameterStatePolicy parameters =
        RendererParameterStatePolicy::RebindCurrentRowsBeforeFirstBlock;
    RendererInternalStatePolicy internalState =
        RendererInternalStatePolicy::ResetOnReplacement;
    RendererTransitionPolicy transition =
        RendererTransitionPolicy::AtomicHardSwapWithDeferredReclamation;
    int minimumRetirementFenceBlocks =
        kPublishedRendererRetirementFenceBlocks;
};

// Immutable, off-callback prepared audio renderer owned by an ApgBundle.
// The interface is renderer-neutral for retained legacy bundles, while a
// published TC1 Faust renderer is the sole audio executor for its graph.
class PreparedAudioRenderer {
public:
    virtual ~PreparedAudioRenderer() = default;

    // False rejects this block before mutating the host buffer. The owning
    // bundle handles that callback as silence and retains this renderer for
    // the next block; it never delegates a published TC1 graph to APG.
    virtual bool processAudio(juce::AudioBuffer<float>&,
                              juce::MidiBuffer&) noexcept = 0;
    virtual bool bindModuleControls(int, const float*, int, const int*, int,
                                    int) noexcept
    {
        return false;
    }
    // The VM host owns rows for control-domain modules as well as DSP
    // modules. Let a renderer decline rows for boundaries it prepares by a
    // different contract (for example Host MIDI) without poisoning the
    // candidate block as an invalid direct binding.
    virtual bool acceptsModuleControls(int) const noexcept { return true; }
    virtual std::size_t audioInputBoundaryCount() const noexcept { return 0; }
    virtual int audioInputBoundaryGraphIndex(std::size_t) const noexcept
    { return -1; }
    virtual std::uint32_t audioInputBoundaryChannelIndex(
        std::size_t) const noexcept { return 0; }
    virtual bool bindAudioInputRuntime(
        int, AudioInputRuntime*) noexcept { return false; }
    virtual std::size_t visualInputBoundaryCount() const noexcept
    { return 0; }
    virtual int visualInputBoundaryGraphIndex(std::size_t) const noexcept
    { return -1; }
    virtual VisualInputRuntime* visualInputRuntime(int) noexcept
    { return nullptr; }
    virtual std::size_t midiOutputBoundaryCount() const noexcept { return 0; }
    virtual int midiOutputBoundaryGraphIndex(std::size_t) const noexcept
    { return -1; }
    virtual bool bindMidiOutputRuntime(
        int, MidiOutputRuntime*) noexcept { return false; }
    virtual bool bindHostAutomationSource(
        const std::atomic<float>*, int) noexcept
    {
        return false;
    }
    virtual void clearHostAutomationSource() noexcept {}
    virtual std::size_t hostMidiInputBoundaryCount() const noexcept
    { return 0; }
    virtual int hostMidiInputBoundaryGraphIndex(std::size_t) const noexcept
    { return -1; }
    virtual bool bindHostMidiRuntime(
        int, HostMidiInputRuntime*) noexcept
    { return false; }
    virtual bool bindVmControlOutputBoundary(
        int, const float* const*, int, int) noexcept
    {
        return false;
    }
    // Prepared Script V2 input arity is renderer-owned metadata. The VM uses
    // it only while building fixed callback scratch; it never discovers or
    // allocates rows on the audio thread.
    virtual int vmInputChannelCount(int) const noexcept { return 0; }
    // Script V2 CTRL_INPUT reads consume the previous completed renderer
    // block. The row storage is prepared off-thread and remains immutable
    // until the next processAudio call publishes its replacement contents.
    virtual const float* previousVmInputRow(
        int, int, int) const noexcept
    {
        return nullptr;
    }
    virtual RendererReplacementPolicy replacementPolicy() const noexcept = 0;
    virtual std::string_view rendererId() const noexcept = 0;
    virtual std::size_t tapCount() const noexcept = 0;
    virtual std::string_view tapModuleId(std::size_t) const noexcept = 0;
    virtual int tapChannelCount(std::size_t) const noexcept = 0;
    // True when a zero-audio tap is an intentional prepared control-domain
    // boundary rather than a missing renderer output. Publication uses this
    // declaration instead of inferring ownership from retained APG nodes.
    virtual bool ownsControlBoundary(std::size_t) const noexcept
    {
        return false;
    }
    virtual float tapSample(std::size_t, int, int) const noexcept = 0;
    virtual std::size_t tapControlOutputCount(std::size_t) const noexcept
    {
        return 0;
    }
    virtual std::string_view tapControlOutputId(
        std::size_t, std::size_t) const noexcept
    {
        return {};
    }
    virtual float tapControlOutputSample(
        std::size_t, std::size_t, int) const noexcept
    {
        return 0.0f;
    }
    virtual bool exchangeTapControlOutputRange(
        std::size_t, std::size_t, float&, float&, float&) noexcept
    {
        return false;
    }
    // Adoption requires an explicit complete product-observer contract. A
    // renderer may not own audio while silently leaving the canonical meter,
    // CLIP, or Faust-bargraph surfaces attached to an idle fallback graph.
    virtual bool publishesProductObservers() const noexcept = 0;
    // Candidate-owned product observers. The APG meter processors are idle
    // while a prepared renderer owns the block, so a qualified renderer must
    // publish the same module-level telemetry from the audio it actually
    // rendered. Exchange methods retain the highest value since the previous
    // message-thread drain, matching MeterProcessor's peak/RMS contract.
    virtual bool exchangeTapOutputMeter(
        std::size_t, float&, float&) noexcept = 0;
    virtual bool exchangeTapInputMeter(
        std::size_t, float&, float&) noexcept = 0;
    virtual bool exchangeTapClip(std::size_t, float&) noexcept = 0;
    virtual std::size_t tapFaceplateMeterCount(
        std::size_t) const noexcept = 0;
    virtual std::string_view tapFaceplateMeterId(
        std::size_t, std::size_t) const noexcept = 0;
    virtual float tapFaceplateMeterValue(
        std::size_t, std::size_t) const noexcept = 0;
    // Exact renderer-owned audio scratch retained after off-callback
    // preparation. Compiler/factory memory remains backend-owned and is
    // reported separately by qualification tooling where available.
    virtual std::size_t preparedAudioBufferBytes() const noexcept { return 0; }
};

} // namespace curlop::transport
