#pragma once

#include "control/vm/core/HostMidiInputRuntime.h"
#include "graph/engine/nodes/ControlCoreProcessor.h"
#include "modules/backend/CurlopDspNode.h"

#include <algorithm>
#include <atomic>
#include <cmath>

namespace curlop {

class HostMidiInputProcessor : public DspNode
{
public:
    using PreparedHostMidiHook = HostMidiInputRuntime::PlanHook;

    HostMidiInputProcessor()
        : DspNode(BusesProperties()
            .withInput("Control In", juce::AudioChannelSet::discreteChannels(kInputChannels))
            .withOutput("Out", juce::AudioChannelSet::discreteChannels(kOutputChannels)))
    {}

    const juce::String getName() const override { return "MidiInput"; }
    void setPreparedPacketCapacity(std::size_t capacity) noexcept
    {
        runtime_->setPreparedPacketCapacity(capacity);
    }

    std::size_t preparedPacketCapacity() const noexcept
    {
        return runtime_->preparedPacketCapacity();
    }

    void prepareToPlay(double, int) override
    {
        runtime_->prepare();
    }

    void bindParam(int index, ControlCoreProcessor* core)
    {
        if (index == 0) channelCore_ = core;
        else if (index == 1) ccCore_ = core;
    }

    void bindHostMidiSource(const HostMidiNoteState* notes,
                            const std::atomic<float>* ccRowMajor,
                            int channels,
                            int ccs,
                            const HostMidiBlockState* blockState = nullptr) noexcept
    {
        runtime_->bindSource(
            notes, ccRowMajor, channels, ccs, blockState);
    }

    void bindPreparedRuntime(HostMidiInputRuntime* runtime) noexcept
    {
        runtime_ = runtime != nullptr ? runtime : &ownedRuntime_;
    }

    void bindPreparedHostMidiHook(
        void* context,
        PreparedHostMidiHook hook,
        int graphIdx) noexcept
    {
        runtime_->bindPlanHook(context, hook, graphIdx);
    }

    void clearPreparedHostMidiHook() noexcept
    {
        runtime_->clearPlanHook();
    }

    HostMidiVoicePlanView voicePlan() const noexcept
    {
        return runtime_->voicePlan();
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        processPreparedBlock(buffer, buffer.getNumSamples());
    }

    bool processPreparedBlock(juce::AudioBuffer<float>& buffer,
                              int numSamples) noexcept
    {
        if (numSamples <= 0 || numSamples > buffer.getNumSamples()
            || buffer.getNumChannels() < kOutputChannels)
            return false;
        const int n = numSamples;

        auto* gateOut = buffer.getWritePointer(2);
        auto* pitchOut = buffer.getWritePointer(3);
        auto* velocityOut = buffer.getWritePointer(4);
        auto* ccOut = buffer.getWritePointer(5);

        if (! runtime_->process(
                gateOut, pitchOut, velocityOut, ccOut, n,
                [this, &buffer] (int control, int sample, float fallback) {
                    return readParam(
                        buffer,
                        control == 0 ? channelCore_ : ccCore_,
                        control, sample, fallback);
                }))
            return false;

        return true;
    }

    static constexpr int kInputChannels = 2;
    static constexpr int kOutputChannels = 6;

private:
    float readParam(const juce::AudioBuffer<float>& buffer,
                    ControlCoreProcessor* core,
                    int index,
                    int sample,
                    float fallback) const
    {
        float value = fallback;
        if (core != nullptr) value = core->getOutputValue();
        if (index >= 0 && index < buffer.getNumChannels())
            value += buffer.getSample(index, sample);
        return std::isfinite(value) ? value : fallback;
    }

    ControlCoreProcessor* channelCore_ = nullptr;
    ControlCoreProcessor* ccCore_ = nullptr;
    HostMidiInputRuntime ownedRuntime_;
    HostMidiInputRuntime* runtime_ = &ownedRuntime_;
};

} // namespace curlop
