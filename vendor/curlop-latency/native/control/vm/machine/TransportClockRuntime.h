#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <vector>

namespace curlop {

struct TransportClockPlanView
{
    const float* const* rows = nullptr;
    int rowCount = 0;
    int numSamples = 0;

    const float* row(int channel) const noexcept
    {
        return rows != nullptr && channel >= 0 && channel < rowCount
            ? rows[static_cast<std::size_t>(channel)] : nullptr;
    }
};

// Prepared audio-thread-only transport control source. The host resolves
// play/tempo/position authority; this VM/control-domain runtime projects that
// authority into the eight patchable graph rows.
class TransportClockRuntime
{
public:
    enum OutputChannel {
        PlayGate = 0,
        StartPulse,
        ResetPulse,
        ClockPulse,
        DivisionPhase,
        BeatPhase,
        BarPhase,
        Bpm,
        OutputCount
    };
    enum InputChannel { StepsPerBar = 0, PulseWidth, InputCount };

    void prepare(double sampleRate, int maximumBlockSize)
    {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
        const auto samples = static_cast<std::size_t>(
            std::max(0, maximumBlockSize));
        for (int channel = 0; channel < OutputCount; ++channel) {
            rows_[static_cast<std::size_t>(channel)].resize(samples);
            rowPointers_[static_cast<std::size_t>(channel)] =
                rows_[static_cast<std::size_t>(channel)].data();
            lastValues_[static_cast<std::size_t>(channel)].store(
                0.0f, std::memory_order_relaxed);
        }
        startPulseSamplesRemaining_ = 0;
        resetPulseSamplesRemaining_ = 0;
        lastPlaying_ = false;
        hasTransportContext_ = false;
    }

    void setTransportContext(bool playing, double bpm,
                             std::int64_t blockStartSample,
                             int beatsPerBar) noexcept
    {
        const bool wasPlaying = lastPlaying_;
        lastPlaying_ = playing;
        playing_ = playing;
        bpm_ = bpm > 0.0 ? bpm : 120.0;
        blockStartSample_ = std::max<std::int64_t>(0, blockStartSample);
        beatsPerBar_ = std::max(1, beatsPerBar);
        hasTransportContext_ = true;
        if (playing && ! wasPlaying)
            startPulseSamplesRemaining_ = controlVisiblePulseSamples();
        if (! playing && wasPlaying)
            resetPulseSamplesRemaining_ = controlVisiblePulseSamples();
    }

    void clearTransportContext() noexcept
    {
        hasTransportContext_ = false;
        playing_ = false;
        lastPlaying_ = false;
        startPulseSamplesRemaining_ = 0;
        resetPulseSamplesRemaining_ = 0;
    }

    // Machine-host rebuilds replace prepared storage while the slot clock
    // keeps running. Carry only scalar clock state; row storage remains owned
    // and sized by the replacement runtime.
    void migrateStateFrom(const TransportClockRuntime& previous) noexcept
    {
        hasTransportContext_ = previous.hasTransportContext_;
        playing_ = previous.playing_;
        lastPlaying_ = previous.lastPlaying_;
        bpm_ = previous.bpm_;
        blockStartSample_ = previous.blockStartSample_;
        beatsPerBar_ = previous.beatsPerBar_;
        startPulseSamplesRemaining_ =
            previous.startPulseSamplesRemaining_;
        resetPulseSamplesRemaining_ =
            previous.resetPulseSamplesRemaining_;
        for (int channel = 0; channel < OutputCount; ++channel)
            lastValues_[static_cast<std::size_t>(channel)].store(
                previous.lastValues_[static_cast<std::size_t>(channel)].load(
                    std::memory_order_relaxed),
                std::memory_order_relaxed);
    }

    TransportClockPlanView planBlock(float stepsPerBar,
                                     float pulseWidth,
                                     int numSamples) noexcept
    {
        const int n = std::min(
            std::max(0, numSamples),
            static_cast<int>(rows_[0].size()));
        const bool playing = hasTransportContext_ && playing_;
        const double bpm = bpm_ > 0.0 ? bpm_ : 120.0;
        const double samplesPerBeat = bpm > 0.0
            ? 60.0 * sampleRate_ / bpm : sampleRate_;
        const int beatsPerBar = std::max(1, beatsPerBar_);
        const double steps = std::clamp(
            std::round(std::isfinite(stepsPerBar)
                           ? static_cast<double>(stepsPerBar) : 16.0),
            1.0, 64.0);
        const double cycleBeats = static_cast<double>(beatsPerBar) / steps;
        const float width = std::clamp(
            std::isfinite(pulseWidth) ? pulseWidth : 0.5f,
            0.001f, 1.0f);

        int start = startPulseSamplesRemaining_;
        int reset = resetPulseSamplesRemaining_;
        for (int sample = 0; sample < n; ++sample) {
            const double beat = samplesPerBeat > 0.0
                ? static_cast<double>(blockStartSample_ + sample)
                    / samplesPerBeat
                : 0.0;
            const double beatFraction = positiveFraction(beat);
            const double barFraction = positiveFraction(
                beat / static_cast<double>(beatsPerBar));
            const double divisionFraction = cycleBeats > 0.0
                ? positiveFraction(beat / cycleBeats) : 0.0;
            write(PlayGate, sample, playing ? 1.0f : 0.0f);
            write(StartPulse, sample,
                  playing && start > 0 ? 1.0f : 0.0f);
            write(ResetPulse, sample, reset > 0 ? 1.0f : 0.0f);
            write(ClockPulse, sample,
                  playing && divisionFraction < width ? 1.0f : 0.0f);
            write(DivisionPhase, sample,
                  playing ? static_cast<float>(divisionFraction) : 0.0f);
            write(BeatPhase, sample,
                  playing ? static_cast<float>(beatFraction) : 0.0f);
            write(BarPhase, sample,
                  playing ? static_cast<float>(barFraction) : 0.0f);
            write(Bpm, sample, static_cast<float>(bpm));
            if (start > 0) --start;
            if (reset > 0) --reset;
        }
        startPulseSamplesRemaining_ = start;
        resetPulseSamplesRemaining_ = reset;
        publishLast(n);
        return preparedPlan(n);
    }

    TransportClockPlanView preparedPlan(int numSamples) const noexcept
    {
        const int n = std::min(
            std::max(0, numSamples),
            static_cast<int>(rows_[0].size()));
        return { rowPointers_.data(), OutputCount, n };
    }

    float lastValue(OutputChannel channel) const noexcept
    {
        const int index = static_cast<int>(channel);
        return index >= 0 && index < OutputCount
            ? lastValues_[static_cast<std::size_t>(index)].load(
                  std::memory_order_relaxed)
            : 0.0f;
    }

private:
    static double positiveFraction(double value) noexcept
    {
        if (! std::isfinite(value)) return 0.0;
        const double fraction = value - std::floor(value);
        return fraction < 0.0 ? fraction + 1.0 : fraction;
    }

    int controlVisiblePulseSamples() const noexcept
    {
        constexpr double pulseSeconds = 0.200;
        return std::max(
            1, static_cast<int>(std::ceil(sampleRate_ * pulseSeconds)));
    }

    void write(OutputChannel channel, int sample, float value) noexcept
    {
        rows_[static_cast<std::size_t>(channel)]
             [static_cast<std::size_t>(sample)] = value;
    }

    void publishLast(int sampleCount) noexcept
    {
        if (sampleCount <= 0) return;
        const auto last = static_cast<std::size_t>(sampleCount - 1);
        for (int channel = 0; channel < OutputCount; ++channel)
            lastValues_[static_cast<std::size_t>(channel)].store(
                rows_[static_cast<std::size_t>(channel)][last],
                std::memory_order_relaxed);
    }

    double sampleRate_ = 44100.0;
    bool hasTransportContext_ = false;
    bool playing_ = false;
    bool lastPlaying_ = false;
    double bpm_ = 120.0;
    std::int64_t blockStartSample_ = 0;
    int beatsPerBar_ = 4;
    int startPulseSamplesRemaining_ = 0;
    int resetPulseSamplesRemaining_ = 0;
    std::array<std::vector<float>, OutputCount> rows_;
    std::array<const float*, OutputCount> rowPointers_ {};
    std::array<std::atomic<float>, OutputCount> lastValues_ {};
};

} // namespace curlop
