#pragma once

#include "control/vm/core/MidiOutputRuntime.h"
#include "graph/engine/nodes/ControlCoreProcessor.h"
#include "modules/backend/CurlopDspNode.h"

#include <atomic>
#include <cmath>

namespace curlop {

class MidiOutputProcessor : public DspNode
{
public:
    static constexpr int kGate = MidiOutputRuntime::kGate;
    static constexpr int kPitch = MidiOutputRuntime::kPitch;
    static constexpr int kVelocity = MidiOutputRuntime::kVelocity;
    static constexpr int kChannel = MidiOutputRuntime::kChannel;
    static constexpr int kCc = MidiOutputRuntime::kCc;
    static constexpr int kCcValue = MidiOutputRuntime::kCcValue;
    static constexpr int kInputChannels = MidiOutputRuntime::kInputChannels;

    MidiOutputProcessor()
        : DspNode(BusesProperties()
            .withInput("Control In",
                       juce::AudioChannelSet::discreteChannels(kInputChannels))
            .withOutput("Out", juce::AudioChannelSet::stereo()))
    {}

    const juce::String getName() const override { return "MidiOutput"; }
    bool producesMidi() const override { return true; }
    void prepareToPlay(double, int) override {}

    void bindParam(int index, ControlCoreProcessor* core)
    {
        if (index >= 0 && index < kInputChannels)
            cores_[index] = core;
    }

    void setExternalControlConnected(int index, bool connected) noexcept
    {
        if (index >= 0 && index < kInputChannels)
            externalControlConnected_[index].store(
                connected, std::memory_order_relaxed);
    }

    void bindPreparedRuntime(MidiOutputRuntime* runtime) noexcept
    {
        runtime_ = runtime != nullptr ? runtime : &ownedRuntime_;
    }

    void bindMidiRoute(
        MidiRouteManager* manager, const std::string* routeId) noexcept
    {
        if (runtime_ == &ownedRuntime_)
            ownedRuntime_.bindLegacyMidiRoute(manager, routeId);
        else
            runtime_->bindMidiRouteManager(manager);
    }

    void panic(juce::MidiBuffer& midi) noexcept { runtime_->panic(midi); }
    void tombstone(
        juce::MidiBuffer& midi, bool deliverLocalMidi) noexcept
    {
        runtime_->tombstone(midi, deliverLocalMidi);
    }
    bool noteActive() const noexcept { return runtime_->noteActive(); }
    bool tombstoned() const noexcept { return runtime_->tombstoned(); }
    bool consumeGeneratedActivityTick() noexcept
    {
        return runtime_->consumeGeneratedActivityTick();
    }

    void processBlock(
        juce::AudioBuffer<float>& buffer,
        juce::MidiBuffer& midi) override
    {
        const int sampleCount = buffer.getNumSamples();
        runtime_->processControls(sampleCount, midi, [this, &buffer] (
                int index, int sample, float fallback) {
            return readParam(buffer, index, sample, fallback);
        });
        for (int channel = 0;
             channel < juce::jmin(2, buffer.getNumChannels()); ++channel)
            buffer.clear(channel, 0, sampleCount);
    }

    bool processPreparedControls(const float* const* controls,
                                 int controlCount,
                                 int sampleCount,
                                 juce::MidiBuffer& midi) noexcept
    {
        return runtime_->processPreparedControls(
            controls, controlCount, sampleCount, midi);
    }

private:
    float readParam(const juce::AudioBuffer<float>& buffer,
                    int index,
                    int sample,
                    float fallback) const noexcept
    {
        float value = fallback;
        if (index >= 0 && index < kInputChannels) {
            if (externalControlConnected_[index].load(
                    std::memory_order_relaxed)
                && index < buffer.getNumChannels())
                value = buffer.getSample(index, sample);
            else if (cores_[index] != nullptr)
                value = cores_[index]->getOutputValue();
        }
        return std::isfinite(value) ? value : fallback;
    }

    ControlCoreProcessor* cores_[kInputChannels] {};
    std::atomic<bool> externalControlConnected_[kInputChannels] {};
    MidiOutputRuntime ownedRuntime_;
    MidiOutputRuntime* runtime_ = &ownedRuntime_;
};

} // namespace curlop
