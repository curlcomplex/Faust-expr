#pragma once
#include <juce_audio_processors/juce_audio_processors.h>

namespace curlop {

// Base class for all CURLOP DSP nodes in the AudioProcessorGraph.
// Eliminates ~20 lines of AudioProcessor boilerplate per node.
// Subclasses override: getName(), prepareToPlay(), processBlock().
class DspNode : public juce::AudioProcessor
{
public:
    using AudioProcessor::AudioProcessor;
    using BusesProperties = AudioProcessor::BusesProperties;

    void releaseResources() override {}
    double getTailLengthSeconds() const override { return 0; }
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
};

} // namespace curlop
