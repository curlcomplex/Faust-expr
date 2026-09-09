#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <string>
#include <utility>
#include "shell/CurlopDebug.h"

namespace curlop {

// F-075 — the control SINK. A single-channel pass-through node that tracks the
// control signal flowing through it: the instantaneous last-sample value (what
// a faceplate value-readout shows) and a peak-hold magnitude (for a bar meter).
// It is the destination of a control wire — a source module's [curlop:cvout]
// channel is routed into a meter's input (slice B), and the value is surfaced
// to the GUI snapshot the faceplate widget reads.
//
// Mirrors MeterProcessor (graph/engine/nodes/MeterProcessor.h): audio thread
// writes via processBlock, the message/timer thread reads via the accessors.
// Unlike MeterProcessor it tracks the *value* (signed last sample) as well as a
// peak — a control reading is the level itself, not just its envelope.
class ControlMeterProcessor : public juce::AudioProcessor {
public:
    ControlMeterProcessor() {
        setPlayConfigDetails(1, 1, 48000.0, 512);
    }

    const juce::String getName() const override { return "ControlMeterProcessor"; }

    void prepareToPlay(double sr, int bs) override {
        setPlayConfigDetails(1, 1, sr, bs);
    }
    void releaseResources() override {}

    void processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer&) override {
        const int n = buf.getNumSamples();
        if (n <= 0 || buf.getNumChannels() < 1) return;
        const float gain = gain_.load(std::memory_order_relaxed);
        auto* wr = buf.getWritePointer(0);
        for (int i = 0; i < n; ++i)
            wr[i] *= gain;
        // Instantaneous value: the last sample of the block — what a numeric
        // value readout displays. Peak: the block magnitude, peak-held until
        // the timer thread reads it (same pattern as MeterProcessor).
        const float last = buf.getSample(0, n - 1);
        lastValue_.store(last, std::memory_order_relaxed);
        float blockMin = buf.getSample(0, 0);
        float blockMax = blockMin;
        for (int i = 1; i < n; ++i)
        {
            const float v = buf.getSample(0, i);
            if (v < blockMin) blockMin = v;
            if (v > blockMax) blockMax = v;
        }
        const float mag = juce::jmax (std::abs (blockMin), std::abs (blockMax));
        const float prev = peak_.load(std::memory_order_relaxed);
        if (mag > prev) peak_.store(mag, std::memory_order_relaxed);
        publishRange (blockMin, blockMax);
        // CONTROL_TAP (T-665): the sink's received control value, throttled
        // ~3Hz per instance (every 32 blocks) so the JSONL stays readable.
        // `last` is the instantaneous level a value readout shows (an LFO
        // sweeping 0→1); `mag` is the block peak. This is the tap that
        // distinguishes a control wire carrying the LFO from one carrying audio.
        if (++ctrlTapBlk_ >= 32) {
            ctrlTapBlk_ = 0;
            CDBG_RT(CONTROL_TAP, "control-meter sink label=%s last=%.5f peak=%.5f min=%.5f max=%.5f",
                    label_.empty() ? "?" : label_.c_str(), last, mag, blockMin, blockMax);
        }
        // Pass-through: a meter is a tap, not a wall — the signal continues so a
        // meter can sit inline without silencing the chain it observes.
        // (buf is in==out; channel 0 already holds the input.)
    }

    // Message/timer thread: the instantaneous value last seen (not reset — a
    // value readout always shows the latest, not a peak-hold).
    float lastValue() const noexcept { return lastValue_.load(std::memory_order_relaxed); }
    // Message/timer thread: read and reset the peak-hold magnitude.
    float exchangePeak() noexcept { return peak_.exchange(0.0f, std::memory_order_relaxed); }
    bool exchangeRange (float& mn, float& mx) noexcept
    {
        if (! rangeSeen_.exchange (false, std::memory_order_acq_rel))
        {
            mn = 0.0f;
            mx = 0.0f;
            return false;
        }
        mn = minValue_.exchange (0.0f, std::memory_order_relaxed);
        mx = maxValue_.exchange (0.0f, std::memory_order_relaxed);
        return true;
    }

    // CONTROL_TAP (T-665): identify this sink in the diagnostic stream. Set at
    // build time to the owning module's dsl name so two meters are tellable
    // apart when both fire under the flag.
    void setLabel(std::string l) { label_ = std::move(l); }
    void setGain(float g) noexcept { gain_.store(std::max(0.0f, g), std::memory_order_relaxed); }

    // Required overrides (no editor, no state).
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
    void publishRange (float mn, float mx) noexcept
    {
        if (! rangeSeen_.exchange (true, std::memory_order_acq_rel))
        {
            minValue_.store (mn, std::memory_order_relaxed);
            maxValue_.store (mx, std::memory_order_relaxed);
            return;
        }
        auto prevMin = minValue_.load (std::memory_order_relaxed);
        while (mn < prevMin
               && ! minValue_.compare_exchange_weak (prevMin, mn, std::memory_order_relaxed))
        {}
        auto prevMax = maxValue_.load (std::memory_order_relaxed);
        while (mx > prevMax
               && ! maxValue_.compare_exchange_weak (prevMax, mx, std::memory_order_relaxed))
        {}
    }

    std::atomic<float> lastValue_{0.0f};
    std::atomic<float> peak_{0.0f};
    std::atomic<float> minValue_{0.0f};
    std::atomic<float> maxValue_{0.0f};
    std::atomic<bool>  rangeSeen_{false};
    std::atomic<float> gain_{1.0f};
    std::string label_;        // CONTROL_TAP: owning module's dsl name
    int         ctrlTapBlk_ = 0;  // CONTROL_TAP throttle counter (per instance)
};

} // namespace curlop
