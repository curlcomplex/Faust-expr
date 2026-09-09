#pragma once

#include <juce_core/juce_core.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace curlop {

// Prepared, allocation-free audio-thread boundary between the audio renderer
// and the presentation/render domain. The unified renderer owns this state;
// VisualInputProcessor is only the transitional APG adapter.
class VisualInputRuntime
{
public:
    static constexpr int kWaveformBins = 64;

    struct Snapshot {
        float scalarValue = 0.0f;
        std::array<float, kWaveformBins> waveform {};
        std::vector<float> controls;
        std::vector<std::array<float, kWaveformBins>> waveforms;
        std::uint64_t generation = 0;
        std::uint64_t samplePosition = 0;
        bool stale = true;
    };

    struct Range { float minimum = 0.0f; float maximum = 1.0f; };

    VisualInputRuntime(std::vector<Range> controlRanges, int audioInputCount)
        : ranges_(std::move(controlRanges)),
          controlCount_(static_cast<int>(ranges_.size())),
          audioInputCount_(juce::jmax(0, audioInputCount)),
          controls_(controlCount_ > 0
              ? std::make_unique<std::atomic<float>[]>(
                    static_cast<std::size_t>(controlCount_))
              : nullptr),
          waveforms_(audioInputCount_ > 0
              ? std::make_unique<std::atomic<float>[]>(
                    static_cast<std::size_t>(audioInputCount_)
                        * kWaveformBins)
              : nullptr)
    {
        reset();
    }

    int controlInputCount() const noexcept { return controlCount_; }
    int audioInputCount() const noexcept { return audioInputCount_; }

    void reset() noexcept
    {
        for (int control = 0; control < controlCount_; ++control)
            controls_[static_cast<std::size_t>(control)].store(
                ranges_[static_cast<std::size_t>(control)].minimum,
                std::memory_order_relaxed);
        for (int sample = 0;
             sample < audioInputCount_ * kWaveformBins; ++sample)
            waveforms_[static_cast<std::size_t>(sample)].store(
                0.0f, std::memory_order_relaxed);
        generation_.store(0, std::memory_order_relaxed);
        samplePosition_.store(0, std::memory_order_relaxed);
        fresh_.store(false, std::memory_order_relaxed);
        publishingEnabled_.store(true, std::memory_order_relaxed);
    }

    void setPublishingEnabled(bool enabled) noexcept
    {
        publishingEnabled_.store(enabled, std::memory_order_release);
        if (! enabled)
            fresh_.store(false, std::memory_order_release);
    }

    bool publishingEnabled() const noexcept
    {
        return publishingEnabled_.load(std::memory_order_acquire);
    }

    template <typename ChannelAt>
    bool publishChannels(ChannelAt channelAt, int samples) noexcept
    {
        if (samples <= 0 || ! publishingEnabled())
            return false;
        for (int control = 0; control < controlCount_; ++control) {
            const auto* channel = channelAt(control);
            const float last = channel != nullptr
                ? channel[samples - 1] : 0.0f;
            const auto range = ranges_[static_cast<std::size_t>(control)];
            controls_[static_cast<std::size_t>(control)].store(
                std::isfinite(last)
                    ? juce::jlimit(range.minimum, range.maximum, last)
                    : range.minimum,
                std::memory_order_relaxed);
        }
        for (int audio = 0; audio < audioInputCount_; ++audio) {
            const int firstChannel = controlCount_ + audio * 2;
            const auto* left = channelAt(firstChannel);
            const auto* right = channelAt(firstChannel + 1);
            for (int bin = 0; bin < kWaveformBins; ++bin) {
                const int index = juce::jlimit(
                    0, samples - 1,
                    static_cast<int>((static_cast<std::int64_t>(bin)
                        * samples) / kWaveformBins));
                const float l = left != nullptr
                        && std::isfinite(left[index])
                    ? left[index] : 0.0f;
                const float r = right != nullptr
                        && std::isfinite(right[index])
                    ? right[index] : l;
                waveforms_[static_cast<std::size_t>(audio)
                        * kWaveformBins + static_cast<std::size_t>(bin)].store(
                    juce::jlimit(-1.0f, 1.0f, (l + r) * 0.5f),
                    std::memory_order_relaxed);
            }
        }
        samplePosition_.fetch_add(
            static_cast<std::uint64_t>(samples),
            std::memory_order_relaxed);
        generation_.fetch_add(1, std::memory_order_release);
        fresh_.store(true, std::memory_order_release);
        return true;
    }

    bool publishPreparedBlock(const float* const* channels,
                              int channelCount,
                              int samples) noexcept
    {
        if (channels == nullptr
            || channelCount != controlCount_ + 2 * audioInputCount_)
            return false;
        return publishChannels([channels] (int channel) {
            return channels[channel];
        }, samples);
    }

    Snapshot presentationSnapshot() noexcept
    {
        Snapshot result;
        result.generation = generation_.load(std::memory_order_acquire);
        result.controls.resize(static_cast<std::size_t>(controlCount_));
        for (int control = 0; control < controlCount_; ++control)
            result.controls[static_cast<std::size_t>(control)] =
                controls_[static_cast<std::size_t>(control)].load(
                    std::memory_order_relaxed);
        result.waveforms.resize(static_cast<std::size_t>(audioInputCount_));
        for (int audio = 0; audio < audioInputCount_; ++audio)
            for (int bin = 0; bin < kWaveformBins; ++bin)
                result.waveforms[static_cast<std::size_t>(audio)]
                    [static_cast<std::size_t>(bin)] =
                        waveforms_[static_cast<std::size_t>(audio)
                            * kWaveformBins
                            + static_cast<std::size_t>(bin)].load(
                                std::memory_order_relaxed);
        if (! result.controls.empty())
            result.scalarValue = result.controls.front();
        if (! result.waveforms.empty())
            result.waveform = result.waveforms.front();
        result.samplePosition = samplePosition_.load(
            std::memory_order_relaxed);
        result.stale = ! fresh_.exchange(false, std::memory_order_acq_rel);
        return result;
    }

private:
    const std::vector<Range> ranges_;
    const int controlCount_;
    const int audioInputCount_;
    std::unique_ptr<std::atomic<float>[]> controls_;
    std::unique_ptr<std::atomic<float>[]> waveforms_;
    std::atomic<std::uint64_t> generation_ { 0 };
    std::atomic<std::uint64_t> samplePosition_ { 0 };
    std::atomic<bool> fresh_ { false };
    std::atomic<bool> publishingEnabled_ { true };
};

} // namespace curlop
