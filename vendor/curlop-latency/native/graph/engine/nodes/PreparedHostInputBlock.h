#pragma once

#include <algorithm>
#include <cstdint>

#include <juce_audio_basics/juce_audio_basics.h>

namespace curlop {

struct HostInputBlockView
{
    const float* const* channels = nullptr;
    int channelCount = 0;
    int sampleCount = 0;
    std::uint64_t generation = 0;

    const float* channel(int index) const noexcept
    {
        return channels != nullptr
            && index >= 0
            && index < channelCount
            ? channels[index]
            : nullptr;
    }
};

// One allocation-free callback snapshot of the processor input bus. Storage
// changes only in prepare(); capture() copies the current host/device input
// before VMRunner clears or renders the output buffer.
class PreparedHostInputBlock
{
public:
    void prepare(int maximumChannels, int maximumSamples)
    {
        const auto channels = std::max(0, maximumChannels);
        const auto samples = std::max(0, maximumSamples);
        storage_.setSize(channels, samples, false, true, false);
        storage_.clear();
        channelCount_ = 0;
        sampleCount_ = 0;
        generation_ = 0;
    }

    void capture(const juce::AudioBuffer<float>& callbackBuffer,
                 int inputChannelCount,
                 int sampleCount) noexcept
    {
        ++generation_;
        channelCount_ = std::clamp(
            inputChannelCount, 0,
            std::min(storage_.getNumChannels(),
                     callbackBuffer.getNumChannels()));
        sampleCount_ = std::clamp(
            sampleCount, 0,
            std::min(storage_.getNumSamples(),
                     callbackBuffer.getNumSamples()));

        for (int channel = 0; channel < channelCount_; ++channel)
            storage_.copyFrom(
                channel, 0, callbackBuffer, channel, 0, sampleCount_);
    }

    HostInputBlockView view() const noexcept
    {
        return {
            storage_.getArrayOfReadPointers(),
            channelCount_,
            sampleCount_,
            generation_
        };
    }

    const float* preparedChannelAddress(int channel) const noexcept
    {
        return channel >= 0 && channel < storage_.getNumChannels()
            ? storage_.getReadPointer(channel)
            : nullptr;
    }

private:
    juce::AudioBuffer<float> storage_;
    int channelCount_ = 0;
    int sampleCount_ = 0;
    std::uint64_t generation_ = 0;
};

} // namespace curlop
