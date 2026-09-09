#pragma once

#include <cstdint>

#include <juce_audio_processors/juce_audio_processors.h>

#include "control/vm/core/AudioInputRuntime.h"

namespace curlop {

class AudioInputEndpointProcessor final : public juce::AudioProcessor
{
public:
    explicit AudioInputEndpointProcessor(std::uint32_t channelIndex)
        : juce::AudioProcessor(
              BusesProperties().withOutput(
                  "AUDIO", juce::AudioChannelSet::mono(), true)),
          ownedRuntime_(channelIndex)
    {}

    void bindPreparedRuntime(AudioInputRuntime* runtime) noexcept
    {
        runtime_ = runtime != nullptr ? runtime : &ownedRuntime_;
    }

    void bind(HostInputBlockView input) noexcept
    {
        runtime_->bind(input);
    }

    std::uint64_t lastRenderedGeneration() const noexcept
    {
        return runtime_->lastRenderedGeneration();
    }

    std::uint32_t channelIndex() const noexcept
    {
        return runtime_->channelIndex();
    }

    const juce::String getName() const override
    {
        return "Audio Input";
    }

    void prepareToPlay(double, int) override {}
    void releaseResources() override {}

    void processBlock(
        juce::AudioBuffer<float>& buffer,
        juce::MidiBuffer&) override
    {
        runtime_->copyTo(buffer);
    }

    bool isBusesLayoutSupported(
        const BusesLayout& layouts) const override
    {
        return layouts.getMainInputChannelSet()
                == juce::AudioChannelSet::disabled()
            && layouts.getMainOutputChannelSet()
                == juce::AudioChannelSet::mono();
    }

    double getTailLengthSeconds() const override { return 0.0; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override {}
    void setStateInformation(const void*, int) override {}

private:
    AudioInputRuntime ownedRuntime_;
    AudioInputRuntime* runtime_ = &ownedRuntime_;
};

} // namespace curlop
