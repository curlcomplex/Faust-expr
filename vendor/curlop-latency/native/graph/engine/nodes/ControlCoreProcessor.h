#pragma once
#include "modules/backend/CurlopDspNode.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <string>

namespace curlop {

// Knob-as-graph-node. Outputs a constant audio-rate signal.
// Three GUI layers: knob position, absolute lock needle, offset indicator.
// Output ch0: current value (knob or absolute lock)
class ControlCoreProcessor : public DspNode
{
public:
    ControlCoreProcessor(const std::string& name, float initial = 0.5f,
                         float min = 0.0f, float max = 1.0f)
        : DspNode(BusesProperties()
            .withOutput("Value", juce::AudioChannelSet::mono())),
          paramName_(name), knobPosition_(initial), initial_(std::clamp(initial, min, max)), min_(min), max_(max)
    {}

    const juce::String getName() const override { return "ControlCore"; }
    void prepareToPlay(double, int) override {}

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        float value = absoluteLockActive_.load(std::memory_order_relaxed)
            ? absoluteValue_.load(std::memory_order_relaxed)
            : knobPosition_.load(std::memory_order_relaxed);

        value = std::isfinite(value) ? std::clamp(value, min_, max_) : initial_;

        auto* out = buffer.getWritePointer(0);
        for (int s = 0; s < buffer.getNumSamples(); ++s)
            out[s] = value;
    }

    // --- Control interface ---
    void setKnobPosition(float v) {
        knobPosition_.store(std::clamp(v, min_, max_), std::memory_order_relaxed);
    }
    void setAbsoluteLock(float v) {
        absoluteValue_.store(std::clamp(v, min_, max_), std::memory_order_relaxed);
        absoluteLockActive_.store(true, std::memory_order_release);
    }
    void releaseAbsoluteLock() {
        absoluteLockActive_.store(false, std::memory_order_release);
    }

    // --- GUI readback ---
    float getKnobPosition() const { return knobPosition_.load(std::memory_order_relaxed); }
    float getAbsoluteValue() const { return absoluteValue_.load(std::memory_order_relaxed); }
    bool isAbsoluteLockActive() const { return absoluteLockActive_.load(std::memory_order_relaxed); }
    float getOutputValue() const {
        return absoluteLockActive_.load(std::memory_order_relaxed)
            ? absoluteValue_.load(std::memory_order_relaxed)
            : knobPosition_.load(std::memory_order_relaxed);
    }
    const std::string& paramName() const { return paramName_; }
    float getMin() const { return min_; }
    float getMax() const { return max_; }

private:
    std::string paramName_;
    std::atomic<float> knobPosition_;
    float initial_;
    std::atomic<float> absoluteValue_ { 0.0f };
    std::atomic<bool> absoluteLockActive_ { false };
    float min_, max_;
};

} // namespace curlop
