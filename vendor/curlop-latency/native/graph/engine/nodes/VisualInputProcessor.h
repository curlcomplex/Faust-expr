#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "graph/transport/VisualInputRuntime.h"

#include <vector>

namespace curlop {

// Fixed, allocation-free audio-thread boundary for render-domain inputs.
// Channel 0 is the final real-unit scalar after native control composition;
// channels 1/2 are the exact stereo audio edge feeding the visual socket.
class VisualInputProcessor final : public juce::AudioProcessor {
public:
    static constexpr int kWaveformBins = VisualInputRuntime::kWaveformBins;
    using Snapshot = VisualInputRuntime::Snapshot;
    using Range = VisualInputRuntime::Range;

    explicit VisualInputProcessor(float scalarMinimum = 0.0f, float scalarMaximum = 1.0f)
        : VisualInputProcessor({ { scalarMinimum, scalarMaximum } }, 1)
    {}

    VisualInputProcessor(std::vector<Range> controlRanges, int audioInputCount)
        : ownedRuntime_(std::move(controlRanges), audioInputCount)
    {
        setPlayConfigDetails(
            controlInputCount() + 2 * this->audioInputCount(),
            0, 48000.0, 512);
    }

    bool bindPreparedRuntime(VisualInputRuntime* runtime) noexcept
    {
        if (runtime == nullptr
            || runtime->controlInputCount() != controlInputCount()
            || runtime->audioInputCount() != audioInputCount())
            return false;
        runtime_ = runtime;
        return true;
    }

    const juce::String getName() const override { return "VisualInputProcessor"; }
    void prepareToPlay(double sampleRate, int blockSize) override
    {
        setPlayConfigDetails(
            controlInputCount() + 2 * audioInputCount(), 0,
            sampleRate, blockSize);
        runtime_->reset();
    }
    void releaseResources() override {}

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        const int samples = buffer.getNumSamples();
        runtime_->publishChannels([&buffer] (int channel) {
            return channel < buffer.getNumChannels()
                ? buffer.getReadPointer(channel) : nullptr;
        }, samples);
    }

    bool publishPreparedBlock(const float* const* channels,
                              int channelCount,
                              int samples) noexcept
    {
        return runtime_->publishPreparedBlock(
            channels, channelCount, samples);
    }

    int controlInputCount() const noexcept
    {
        return ownedRuntime_.controlInputCount();
    }
    int audioInputCount() const noexcept
    {
        return ownedRuntime_.audioInputCount();
    }

    Snapshot presentationSnapshot() noexcept
    {
        return runtime_->presentationSnapshot();
    }

    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override {}
    void setStateInformation(const void*, int) override {}

private:
    VisualInputRuntime ownedRuntime_;
    VisualInputRuntime* runtime_ = &ownedRuntime_;
};

} // namespace curlop
