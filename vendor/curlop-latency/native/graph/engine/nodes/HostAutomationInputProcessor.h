#pragma once

#include "control/vm/core/VMRunnerHost.h"
#include "graph/engine/nodes/ControlCoreProcessor.h"
#include "modules/backend/CurlopDspNode.h"

#include <algorithm>
#include <atomic>
#include <cmath>

namespace curlop {

template <typename ReadControl>
inline void processHostAutomationControls(
    const std::atomic<float>* values,
    int laneCount,
    float* const* outputs,
    int sampleCount,
    ReadControl read) noexcept
{
    for (int sample = 0; sample < sampleCount; ++sample) {
        int lanes[8];
        for (int output = 0; output < 8; ++output) {
            const float fallback = static_cast<float>(output + 1);
            const float value = read(output, sample, fallback);
            lanes[output] = std::clamp(
                static_cast<int>(std::lround(value)), 1,
                kHostAutomationLaneCount) - 1;
        }
        for (int output = 0; output < 8; ++output) {
            const int lane = lanes[output];
            float value = 0.0f;
            if (values != nullptr && lane >= 0 && lane < laneCount) {
                const float raw = values[lane].load(
                    std::memory_order_relaxed);
                value = std::isfinite(raw) ? raw : 0.0f;
            }
            outputs[output][sample] = std::clamp(value, 0.0f, 1.0f);
        }
    }
}

inline bool processPreparedHostAutomationControls(
    const std::atomic<float>* values,
    int laneCount,
    const float* const* controls,
    int controlCount,
    float* const* outputs,
    int outputCount,
    int sampleCount) noexcept
{
    constexpr int outputCountRequired = 8;
    if (controls == nullptr || controlCount != outputCountRequired
        || outputs == nullptr || outputCount != outputCountRequired
        || sampleCount <= 0)
        return false;
    for (int output = 0; output < outputCount; ++output)
        if (outputs[output] == nullptr)
            return false;
    processHostAutomationControls(
        values, laneCount, outputs, sampleCount, [controls] (
            int output, int sample, float fallback) {
        const auto* channel = controls[output];
        const float value = channel != nullptr
            ? channel[sample] : fallback;
        return std::isfinite(value) ? value : fallback;
    });
    return true;
}

class HostAutomationInputProcessor : public DspNode
{
public:
    HostAutomationInputProcessor()
        : DspNode(BusesProperties()
            .withInput("Control In", juce::AudioChannelSet::discreteChannels(kInputChannels))
            .withOutput("Out", juce::AudioChannelSet::discreteChannels(kOutputChannels)))
    {}

    const juce::String getName() const override { return "HostAutomationInput"; }
    void prepareToPlay(double, int) override {}

    void bindParam(int index, ControlCoreProcessor* core)
    {
        if (index >= 0 && index < kOutputCount) laneCores_[index] = core;
    }

    void bindHostAutomationSource(const std::atomic<float>* values,
                                  int laneCount) noexcept
    {
        values_ = values;
        laneCount_ = laneCount;
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        const int n = buffer.getNumSamples();
        if (n <= 0) return;
        float* outs[kOutputCount] {};
        for (int out = 0; out < kOutputCount; ++out)
            outs[out] = buffer.getWritePointer(2 + out);

        processHostAutomationControls(values_, laneCount_, outs, n,
            [this, &buffer] (
                int output, int sample, float fallback) {
            return readParam(buffer, laneCores_[output], output, sample,
                             fallback);
        });
    }

    bool processPreparedControls(const float* const* controls,
                                 int controlCount,
                                 float* const* outputs,
                                 int outputCount,
                                 int sampleCount) noexcept
    {
        return processPreparedHostAutomationControls(
            values_, laneCount_, controls, controlCount, outputs,
            outputCount, sampleCount);
    }

    static constexpr int kOutputCount = 8;
    static constexpr int kInputChannels = kOutputCount;
    static constexpr int kOutputChannels = 2 + kOutputCount;

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

    ControlCoreProcessor* laneCores_[kOutputCount] {};
    const std::atomic<float>* values_ = nullptr;
    int laneCount_ = 0;
};

} // namespace curlop
