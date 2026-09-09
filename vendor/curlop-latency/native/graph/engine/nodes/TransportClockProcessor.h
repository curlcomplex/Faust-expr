#pragma once

#include "control/vm/machine/TransportClockRuntime.h"
#include "graph/engine/nodes/ControlCoreProcessor.h"
#include "modules/backend/CurlopDspNode.h"
#include "shell/CurlopDebug.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>

namespace curlop {

class TransportClockProcessor : public DspNode
{
public:
    enum OutputChannel {
        AudioL = 0,
        AudioR = 1,
        PlayGate = 2,
        StartPulse = 3,
        ResetPulse = 4,
        ClockPulse = 5,
        DivisionPhase = 6,
        BeatPhase = 7,
        BarPhase = 8,
        Bpm = 9,
        OutputCount = 10
    };

    enum InputChannel {
        StepsPerBar = 0,
        PulseWidth = 1,
        InputCount = 2
    };

    TransportClockProcessor()
        : DspNode(BusesProperties()
            .withInput("Control In", juce::AudioChannelSet::discreteChannels(InputCount))
            .withOutput("Out", juce::AudioChannelSet::discreteChannels(OutputCount)))
    {}

    const juce::String getName() const override { return "TransportClock"; }

    void prepareToPlay(double sampleRate,
                       int maximumExpectedSamplesPerBlock) override
    {
        preparedMaximumBlockSize_ = std::max(
            0, maximumExpectedSamplesPerBlock);
        ownedRuntime_.prepare(sampleRate, preparedMaximumBlockSize_);
        if (runtime_ == nullptr)
            runtime_ = &ownedRuntime_;
        for (auto& value : publishedLastValues_)
            value.store(0.0f, std::memory_order_relaxed);
        debugBlockCounter_ = 0;
    }

    void bindParam(int index, ControlCoreProcessor* core)
    {
        if (index == StepsPerBar) stepsPerBarCore_ = core;
        if (index == PulseWidth) pulseWidthCore_ = core;
    }

    void setTransportContext(bool playing, double bpm, int64_t blockStartSample,
                             int beatsPerBar)
    {
        runtime().setTransportContext(
            playing, bpm, blockStartSample, beatsPerBar);
    }

    void clearProjectClockBindings() noexcept
    {
        runtime().clearTransportContext();
    }

    float lastValue(OutputChannel ch) const noexcept
    {
        const int idx = static_cast<int>(ch);
        if (idx < 0 || idx >= OutputCount) return 0.0f;
        return publishedLastValues_[(size_t) idx].load(
            std::memory_order_acquire);
    }

    void bindPreparedTransportClockRuntime(
        TransportClockRuntime* runtime,
        bool reusePreparedPlanOnce = false) noexcept
    {
        runtime_ = runtime != nullptr ? runtime : &ownedRuntime_;
        reusePreparedPlanOnce_ = reusePreparedPlanOnce;
    }

    bool usesTransportClockRuntime(
        const TransportClockRuntime* runtime) const noexcept
    {
        return runtime_ == runtime;
    }

    void disarmPreparedTransportClockPlanReuse() noexcept
    {
        reusePreparedPlanOnce_ = false;
    }

    void armPreparedTransportClockPlanReuse(
        const TransportClockRuntime* runtime) noexcept
    {
        reusePreparedPlanOnce_ = runtime_ == runtime;
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        const int n = buffer.getNumSamples();
        if (n <= 0) return;

        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            buffer.clear(ch, 0, n);

        const auto plan = reusePreparedPlanOnce_
            ? runtime().preparedPlan(n)
            : runtime().planBlock(
                readParam(StepsPerBar, 16.0f),
                readParam(PulseWidth, 0.5f), n);
        reusePreparedPlanOnce_ = false;
        const int planned = std::clamp(plan.numSamples, 0, n);
        for (int output = 0;
             output < TransportClockRuntime::OutputCount; ++output) {
            const int channel = 2 + output;
            if (channel >= buffer.getNumChannels()) break;
            auto* destination = buffer.getWritePointer(channel);
            const auto* source = plan.row(output);
            if (source != nullptr)
                std::copy_n(source, planned, destination);
            std::fill(destination + planned, destination + n, 0.0f);
        }
        rememberLast(buffer, n);
        publishDebug();
    }

private:
    float readParam(int index, float fallback) const
    {
        const ControlCoreProcessor* core = index == StepsPerBar ? stepsPerBarCore_ : pulseWidthCore_;
        if (core == nullptr) return fallback;
        const float v = core->getOutputValue();
        return std::isfinite(v) ? v : fallback;
    }

    void rememberLast(const juce::AudioBuffer<float>& buffer, int n)
    {
        const int last = n - 1;
        for (int ch = 0; ch < OutputCount; ++ch) {
            const float v = ch < buffer.getNumChannels()
                ? buffer.getSample(ch, last)
                : 0.0f;
            publishedLastValues_[(size_t) ch].store(
                v, std::memory_order_release);
        }
    }

    void publishDebug()
    {
        if (++debugBlockCounter_ < 32)
            return;
        debugBlockCounter_ = 0;
        CDBG_RT(TRANSPORT_UPDATE,
                "transport-clock playing=%d bpm=%.3f stepsPerBar=%.0f pulseWidth=%.3f clock=%.3f phase=%.3f beat=%.3f bar=%.3f",
                lastValue(PlayGate) > 0.5f ? 1 : 0,
                (double) lastValue(Bpm),
                (double) readParam(StepsPerBar, 16.0f),
                (double) readParam(PulseWidth, 0.5f),
                (double) lastValue(ClockPulse),
                (double) lastValue(DivisionPhase),
                (double) lastValue(BeatPhase),
                (double) lastValue(BarPhase));
    }

    TransportClockRuntime& runtime() noexcept
    { return runtime_ != nullptr ? *runtime_ : ownedRuntime_; }

    int preparedMaximumBlockSize_ = 512;
    TransportClockRuntime ownedRuntime_;
    TransportClockRuntime* runtime_ = &ownedRuntime_;
    bool reusePreparedPlanOnce_ = false;
    ControlCoreProcessor* stepsPerBarCore_ = nullptr;
    ControlCoreProcessor* pulseWidthCore_ = nullptr;
    std::atomic<float> publishedLastValues_[OutputCount] {};
    int debugBlockCounter_ = 0;
};

} // namespace curlop
