#pragma once

#include "graph/engine/nodes/PreparedHostInputBlock.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <limits>

namespace curlop {

// Prepared selected-channel view of the current host input block. The unified
// renderer and its temporary APG oracle share this state so VMRunner may clear
// the callback buffer before rendering without destroying input authority.
class AudioInputRuntime
{
public:
    explicit AudioInputRuntime(std::uint32_t channelIndex) noexcept
        : channelIndex_(channelIndex)
    {}

    void bind(HostInputBlockView input) noexcept { input_ = input; }
    void clear() noexcept { input_ = {}; }

    std::uint32_t channelIndex() const noexcept { return channelIndex_; }

    const float* channelData(int offset, int sampleCount) noexcept
    {
        const auto input = input_;
        lastRenderedGeneration_.store(
            input.generation, std::memory_order_relaxed);
        if (channelIndex_ > static_cast<std::uint32_t>(
                std::numeric_limits<int>::max())
            || offset < 0 || sampleCount < 0
            || offset > input.sampleCount
            || sampleCount > input.sampleCount - offset)
            return nullptr;
        const auto* source = input.channel(
            static_cast<int>(channelIndex_));
        return source != nullptr ? source + offset : nullptr;
    }

    void copyTo(juce::AudioBuffer<float>& buffer) noexcept
    {
        buffer.clear();
        if (buffer.getNumChannels() <= 0) {
            clear();
            return;
        }
        const int sampleCount = std::min(
            buffer.getNumSamples(), input_.sampleCount);
        const auto* source = channelData(0, sampleCount);
        if (source != nullptr && sampleCount > 0)
            buffer.copyFrom(0, 0, source, sampleCount);
        clear();
    }

    std::uint64_t lastRenderedGeneration() const noexcept
    {
        return lastRenderedGeneration_.load(std::memory_order_relaxed);
    }

private:
    std::uint32_t channelIndex_ = 0;
    HostInputBlockView input_;
    std::atomic<std::uint64_t> lastRenderedGeneration_ { 0 };
};

} // namespace curlop
