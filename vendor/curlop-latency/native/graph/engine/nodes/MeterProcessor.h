#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <atomic>
#include <cmath>

namespace curlop {

// Lightweight pass-through processor that tracks peak levels.
// Sits inside module sub-graphs to provide per-module metering.
// Audio thread writes peaks, timer thread reads via exchangePeak().
//
// F-070 (SF-069): doubles as the per-module RECORD TAP point. When tapping is
// enabled, processBlock copies the module's output block into capture_ — a
// meter-owned buffer sized at prepareToPlay. VMRunner reads capture_ on the
// SAME audio thread immediately after the APG render (within the same block),
// so there is no cross-thread sharing of the capture buffer: the audio thread
// both writes it (here) and reads it (VMRunner post-process). tapEnabled_ is
// the only cross-thread field (message thread toggles at record start/stop).
class MeterProcessor : public juce::AudioProcessor {
public:
    explicit MeterProcessor(int channelWidth = 2)
        : channelWidth_(juce::jmax(1, channelWidth)) {
        setPlayConfigDetails(channelWidth_, channelWidth_, 48000.0, 512);
        capture_.setSize(channelWidth_, 512);
    }

    const juce::String getName() const override { return "MeterProcessor"; }

    void prepareToPlay(double sr, int bs) override {
        setPlayConfigDetails(channelWidth_, channelWidth_, sr, bs);
        // Size the record-tap capture to the host block. Off the audio thread.
        capture_.setSize(
            channelWidth_, juce::jmax(1, bs), false, false, true);
    }

    void releaseResources() override {}

    void processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer&) override {
        const int n = buf.getNumSamples();
        // Single pass per channel: peak feeds node/cable glow, RMS feeds
        // authored loudness displays. The per-module meter path is always-on,
        // so keep it allocation-free and bounded.
        float peak = 0.0f;
        double sumSquares = 0.0;
        int rmsSamples = 0;
        const int channels =
            juce::jmin(channelWidth_, buf.getNumChannels());
        for (int channel = 0; channel < channels; ++channel)
        {
            const auto* read = buf.getReadPointer(channel);
            for (int i = 0; i < n; ++i)
            {
                const float sample = read[i];
                const float mag = std::abs(sample);
                if (mag > peak) peak = mag;
                sumSquares += (double) sample * (double) sample;
            }
            rmsSamples += n;
        }
        // Peak-hold: keep the highest value until timer thread reads it
        float prev = peak_.load(std::memory_order_relaxed);
        if (peak > prev) peak_.store(peak, std::memory_order_relaxed);
        const float blockRms = rmsSamples > 0
            ? (float) std::sqrt (sumSquares / (double) rmsSamples)
            : 0.0f;
        prev = rms_.load(std::memory_order_relaxed);
        if (blockRms > prev) rms_.store(blockRms, std::memory_order_relaxed);

        // F-070 record tap: copy this module's output into capture_ so VMRunner
        // can interleave it into the multitrack frame post-render. No alloc:
        // capture_ is pre-sized to the block; we copy min(n, capacity) samples.
        if (tapEnabled_.load(std::memory_order_acquire)) {
            const int cap = capture_.getNumSamples();
            const int cn = juce::jmin(n, cap);
            for (int ch = 0; ch < channelWidth_; ++ch) {
                if (buf.getNumChannels() > ch)
                    capture_.copyFrom(ch, 0, buf.getReadPointer(ch), cn);
                else
                    capture_.clear(ch, 0, cn);
            }
            captureSamples_.store(cn, std::memory_order_release);
        }
    }

    // Timer thread: read and reset the maximum across every declared channel.
    float exchangePeak() {
        return peak_.exchange(0.0f, std::memory_order_relaxed);
    }

    float exchangeRms() {
        return rms_.exchange(0.0f, std::memory_order_relaxed);
    }

    // ── F-070 record tap ────────────────────────────────────────────────────
    // Message thread: enable/disable capture at record start/stop.
    void setTapEnabled(bool on) noexcept { tapEnabled_.store(on, std::memory_order_release); }
    bool isTapEnabled() const noexcept { return tapEnabled_.load(std::memory_order_acquire); }
    // Audio thread (VMRunner post-render, same block): the last captured frame.
    const juce::AudioBuffer<float>& captureBuffer() const noexcept { return capture_; }
    int captureSamples() const noexcept { return captureSamples_.load(std::memory_order_acquire); }
    // Audio-thread render failure boundary: no recorder consumer may reuse a
    // prior completed capture when the current graph block did not complete.
    void invalidateCapture() noexcept
    {
        captureSamples_.store(0, std::memory_order_release);
    }
    int channelWidth() const noexcept { return channelWidth_; }

    // Required overrides (no editor, no state)
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
    const int channelWidth_;
    std::atomic<float> peak_{0.0f};
    std::atomic<float> rms_{0.0f};

    // F-070 record tap (audio-thread-owned buffer; see class comment).
    juce::AudioBuffer<float> capture_;
    std::atomic<bool> tapEnabled_{false};
    std::atomic<int>  captureSamples_{0};
};

} // namespace curlop
