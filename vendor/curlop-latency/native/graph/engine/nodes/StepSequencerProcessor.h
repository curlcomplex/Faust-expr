#pragma once

#include "control/vm/machine/StepClockRuntime.h"
#include "graph/engine/nodes/ControlCoreProcessor.h"
#include "control/surfaces/script/ScriptOutputSockets.h"
#include "modules/backend/CurlopDspNode.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <vector>

namespace curlop {

// APG compatibility wrapper for the VM/control-owned StepClockRuntime.
// New render paths own StepClockRuntime directly and do not call this node.
class StepSequencerProcessor : public DspNode
{
public:
    using Mode = StepClockRuntime::Mode;
    using PreparedStepClockHook = void (*)(
        void*, StepSequencerProcessor&, int, StepClockPlanView) noexcept;

    static constexpr int kSteps = StepClockRuntime::kSteps;
    static constexpr double kStepsPerBeat = StepClockRuntime::kStepsPerBeat;

    explicit StepSequencerProcessor(
        Mode mode,
        std::vector<ScriptOutputSocketDecl> extraOutputs = {})
        : DspNode(BusesProperties()
            .withInput("Control In", juce::AudioChannelSet::discreteChannels(
                inputChannelsFor(mode)))
            .withOutput("Out", juce::AudioChannelSet::discreteChannels(
                outputChannelsFor(mode, static_cast<int>(extraOutputs.size()))))),
          mode_(mode),
          ownedRuntime_(mode),
          extraOutputCount_(static_cast<int>(extraOutputs.size())),
          outputRowsCopy_(static_cast<std::size_t>(
              outputChannelsFor(mode, static_cast<int>(extraOutputs.size())) - 2),
              -1)
    {}

    const juce::String getName() const override { return "StepSequencer"; }

    void prepareToPlay(double sampleRate,
                       int maximumExpectedSamplesPerBlock) override
    {
        ownedRuntime_.prepare(sampleRate, maximumExpectedSamplesPerBlock);
        publishedActiveStep_.store(0, std::memory_order_relaxed);
        if (runtime_ == nullptr)
            runtime_ = &ownedRuntime_;
    }

    void bindParam(int index, ControlCoreProcessor* core)
    {
        if (index < 0) return;
        if (index >= static_cast<int>(params_.size()))
            params_.resize(static_cast<std::size_t>(index) + 1u, nullptr);
        params_[static_cast<std::size_t>(index)] = core;
    }

    void setTransportContext(bool playing, double bpm,
                             std::int64_t blockStartSample)
    {
        runtime().setTransportContext(playing, bpm, blockStartSample);
    }

    void clearProjectSequencerBindings() noexcept
    {
        runtime().clearTransportContext();
        outputBufferData_ = nullptr;
        outputBufferStride_ = 0;
        outputRowCount_ = 0;
    }

    int activeStep() const noexcept
    {
        return publishedActiveStep_.load(std::memory_order_acquire);
    }

    void bindPreparedStepClockRuntime(
        StepClockRuntime* runtime,
        bool reusePreparedPlanOnce = false) noexcept
    {
        runtime_ = runtime != nullptr ? runtime : &ownedRuntime_;
        reusePreparedPlanOnce_ = reusePreparedPlanOnce;
    }

    bool usesStepClockRuntime(
        const StepClockRuntime* runtime) const noexcept
    {
        return runtime_ == runtime;
    }

    void disarmPreparedStepClockPlanReuse() noexcept
    {
        reusePreparedPlanOnce_ = false;
    }

    void armPreparedStepClockPlanReuse(
        const StepClockRuntime* runtime) noexcept
    {
        reusePreparedPlanOnce_ = runtime_ == runtime;
    }

    StepClockPlanView planBlock(
        const juce::AudioBuffer<float>& buffer) noexcept
    {
        if (reusePreparedPlanOnce_) {
            reusePreparedPlanOnce_ = false;
            return runtime().preparedPlan(buffer.getNumSamples());
        }
        struct Context {
            const StepSequencerProcessor* owner;
            const juce::AudioBuffer<float>* buffer;
        } context { this, &buffer };
        return runtime().planBlock({
            &context,
            [] (void* opaque, int index, int sample,
                float fallback) noexcept -> float {
                const auto& input = *static_cast<Context*>(opaque);
                float value = fallback;
                if (index >= 0
                    && index < static_cast<int>(input.owner->params_.size())) {
                    const auto* core = input.owner->params_[
                        static_cast<std::size_t>(index)];
                    if (core != nullptr)
                        value = core->getOutputValue();
                }
                if (index >= 0
                    && index < input.buffer->getNumChannels())
                    value += input.buffer->getSample(index, sample);
                return std::isfinite(value) ? value : fallback;
            }
        }, buffer.getNumSamples());
    }

    void bindOutputBufferRows(const float* data, int stride,
                              const int* rows, int numRows)
    {
        outputBufferData_ = data;
        outputBufferStride_ = stride;
        outputRowCount_ = std::min(
            numRows, static_cast<int>(outputRowsCopy_.size()));
        for (int i = 0; i < outputRowCount_; ++i)
            outputRowsCopy_[static_cast<std::size_t>(i)] = rows[i];
    }

    void bindPreparedStepClockHook(void* context,
                                   PreparedStepClockHook hook,
                                   int graphIdx) noexcept
    {
        preparedStepClockHookContext_ = context;
        preparedStepClockHook_ = hook;
        preparedStepClockGraphIdx_ = graphIdx;
    }

    void clearPreparedStepClockHook() noexcept
    {
        preparedStepClockHookContext_ = nullptr;
        preparedStepClockHook_ = nullptr;
        preparedStepClockGraphIdx_ = -1;
    }

    void processBlock(juce::AudioBuffer<float>& buffer,
                      juce::MidiBuffer&) override
    {
        const int samples = buffer.getNumSamples();
        if (samples <= 0) return;
        const auto plan = planBlock(buffer);
        const int plannedSamples = std::clamp(plan.numSamples, 0, samples);
        publishedActiveStep_.store(
            runtime().activeStep(), std::memory_order_release);
        if (preparedStepClockHook_ != nullptr)
            preparedStepClockHook_(preparedStepClockHookContext_, *this,
                                   preparedStepClockGraphIdx_, plan);

        if (outputBufferData_ != nullptr && outputRowCount_ > 0) {
            for (int i = 0; i < outputRowCount_; ++i) {
                const int channel = 2 + i;
                if (channel >= buffer.getNumChannels()) break;
                const int row = outputRowsCopy_[static_cast<std::size_t>(i)];
                auto* out = buffer.getWritePointer(channel);
                if (row >= 0) {
                    std::memcpy(out,
                                outputBufferData_
                                    + static_cast<std::size_t>(row)
                                        * static_cast<std::size_t>(outputBufferStride_),
                                sizeof(float)
                                    * static_cast<std::size_t>(plannedSamples));
                    std::fill(out + plannedSamples, out + samples, 0.0f);
                } else {
                    std::fill_n(out, samples, 0.0f);
                }
            }
            return;
        }

        auto* gate = buffer.getWritePointer(2);
        auto* pitch = mode_ == Mode::Pitch ? buffer.getWritePointer(3) : nullptr;
        auto* velocity = mode_ == Mode::Pitch ? buffer.getWritePointer(4) : nullptr;
        std::copy_n(runtime().sampledGate(), plannedSamples, gate);
        std::fill(gate + plannedSamples, gate + samples, 0.0f);
        if (mode_ == Mode::Pitch) {
            std::copy_n(runtime().sampledPitch(), plannedSamples, pitch);
            std::copy_n(runtime().sampledVelocity(), plannedSamples, velocity);
            std::fill(pitch + plannedSamples, pitch + samples, 0.0f);
            std::fill(velocity + plannedSamples, velocity + samples, 0.0f);
        }
        for (int i = 0; i < extraOutputCount_; ++i) {
            const int channel = outputChannelsFor(mode_) + i;
            if (channel >= buffer.getNumChannels()) break;
            std::fill_n(buffer.getWritePointer(channel), samples, 0.0f);
        }
    }

    static int inputChannelsFor(Mode mode)
    { return StepClockRuntime::inputChannelsFor(mode); }
    static int outputChannelsFor(Mode mode)
    { return mode == Mode::Pitch ? 5 : 3; }
    static int outputChannelsFor(Mode mode, int extraOutputCount)
    { return outputChannelsFor(mode) + std::max(0, extraOutputCount); }
    static int rateInputChannel(Mode mode)
    { return StepClockRuntime::rateInputChannel(mode); }
    static int incInputChannel(Mode mode)
    { return StepClockRuntime::incInputChannel(mode); }
    static int decInputChannel(Mode mode)
    { return StepClockRuntime::decInputChannel(mode); }
    static int resetInputChannel(Mode mode)
    { return StepClockRuntime::resetInputChannel(mode); }
    static int selectInputChannel(Mode mode)
    { return StepClockRuntime::selectInputChannel(mode); }
    static int velocityInputChannel(Mode mode)
    { return StepClockRuntime::velocityInputChannel(mode); }
    static int stepInputBase(Mode mode)
    { return StepClockRuntime::stepInputBase(mode); }
    static int pitchInputBase(Mode mode)
    { return StepClockRuntime::pitchInputBase(mode); }

private:
    StepClockRuntime& runtime() noexcept
    { return runtime_ != nullptr ? *runtime_ : ownedRuntime_; }
    const StepClockRuntime& runtime() const noexcept
    { return runtime_ != nullptr ? *runtime_ : ownedRuntime_; }

    Mode mode_;
    StepClockRuntime ownedRuntime_;
    StepClockRuntime* runtime_ = &ownedRuntime_;
    bool reusePreparedPlanOnce_ = false;
    std::atomic<int> publishedActiveStep_ { 0 };
    std::vector<ControlCoreProcessor*> params_;
    int extraOutputCount_ = 0;
    const float* outputBufferData_ = nullptr;
    int outputBufferStride_ = 0;
    int outputRowCount_ = 0;
    std::vector<int> outputRowsCopy_;
    void* preparedStepClockHookContext_ = nullptr;
    PreparedStepClockHook preparedStepClockHook_ = nullptr;
    int preparedStepClockGraphIdx_ = -1;
};

} // namespace curlop
