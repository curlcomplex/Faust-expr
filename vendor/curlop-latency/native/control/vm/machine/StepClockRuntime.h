#pragma once

#include "control/vm/machine/StepClockPlan.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace curlop {

// Prepared, audio-thread-only clock state for the native step-sequencer
// contract. This belongs to the VM/control domain: APG's
// StepSequencerProcessor is a compatibility wrapper around the same runtime,
// while the unified Faust renderer can drive it before rendering audio.
class StepClockRuntime
{
public:
    enum class Mode { Trigger, Pitch };

    struct InputView {
        void* context = nullptr;
        float (*read)(void*, int channel, int sample,
                      float fallback) noexcept = nullptr;

        float value(int channel, int sample, float fallback) const noexcept
        {
            return read != nullptr
                ? read(context, channel, sample, fallback)
                : fallback;
        }
    };

    static constexpr int kSteps = 16;
    static constexpr double kStepsPerBeat = 4.0;

    explicit StepClockRuntime(Mode mode) noexcept : mode_(mode) {}

    void prepare(double sampleRate, int maximumBlockSize)
    {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
        const auto samples = static_cast<std::size_t>(
            std::max(0, maximumBlockSize));
        clockEvents_.resize(samples + 1u);
        sampledGate_.resize(samples);
        sampledPitch_.resize(samples);
        sampledVelocity_.resize(samples);
        manualStep_ = 0;
        activeStep_.store(0, std::memory_order_relaxed);
        lastPitch_ = 0.0f;
        prevInc_ = prevDec_ = prevReset_ = false;
        priorGate_ = false;
        priorSourceOrdinal_ = 0;
        priorIteration_ = 0;
        priorPitch_ = 0.0f;
        priorVelocity_ = 0.0f;
        clockEventCount_ = 0;
    }

    void setTransportContext(bool playing, double bpm,
                             std::int64_t blockStartSample) noexcept
    {
        transportPlaying_ = playing;
        transportBpm_ = bpm > 0.0 ? bpm : 120.0;
        blockStartSample_ = std::max<std::int64_t>(0, blockStartSample);
        hasTransportContext_ = true;
    }

    void clearTransportContext() noexcept
    {
        hasTransportContext_ = false;
        transportPlaying_ = false;
    }

    int activeStep() const noexcept
    {
        return activeStep_.load(std::memory_order_relaxed);
    }

    StepClockPlanView planBlock(InputView inputs, int numSamples) noexcept
    {
        const int n = std::min(
            std::max(0, numSamples), static_cast<int>(sampledGate_.size()));
        clockEventCount_ = 0;

        for (int sample = 0; sample < n; ++sample) {
            const float rate = sanitizeRate(inputs.value(
                rateInputChannel(mode_), sample, 1.0f));
            const bool transportClocked =
                hasTransportContext_ && std::abs(rate) > 0.0001f;

            int step = manualStep_;
            std::uint64_t iteration = priorIteration_;
            float gate = 0.0f;
            float pitch = lastPitch_;
            float velocity = 0.0f;

            if (! transportClocked || transportPlaying_) {
                step = currentStepForSample(rate, sample);
                iteration = currentIterationForSample(rate, sample);
                if (std::abs(rate) <= 0.0001f)
                    applyManualControls(inputs, sample, step);
                activeStep_.store(step, std::memory_order_relaxed);

                const float stepLevel = std::clamp(inputs.value(
                    stepInputBase(mode_) + step, sample, 0.0f), 0.0f, 1.0f);
                const bool gateGap =
                    isLastSampleBeforeTransportStepBoundary(rate, sample);
                gate = stepLevel > 0.0f && ! gateGap ? 1.0f : 0.0f;

                if (mode_ == Mode::Pitch) {
                    if (gate > 0.0f) {
                        const float sampledPitch = inputs.value(
                            pitchInputBase(mode_) + step, sample, lastPitch_);
                        if (std::isfinite(sampledPitch))
                            lastPitch_ = sampledPitch;
                    }
                    pitch = lastPitch_;
                    const float velocityControl = std::clamp(inputs.value(
                        velocityInputChannel(mode_), sample, 0.7f), 0.0f, 1.0f);
                    velocity = gate > 0.0f
                        ? velocityControl * stepLevel : 0.0f;
                }
            } else {
                activeStep_.store(manualStep_, std::memory_order_relaxed);
            }

            sampledGate_[static_cast<std::size_t>(sample)] = gate;
            sampledPitch_[static_cast<std::size_t>(sample)] = pitch;
            sampledVelocity_[static_cast<std::size_t>(sample)] = velocity;

            const bool gateHigh = gate > 0.0f;
            std::uint8_t flags = 0;
            if (priorGate_ && ! gateHigh)
                flags = StepClockFlags::GateOff;
            else if (! priorGate_ && gateHigh)
                flags = StepClockFlags::GateOn;
            else if (gateHigh
                     && (priorSourceOrdinal_ != static_cast<std::uint32_t>(step)
                         || priorPitch_ != pitch
                         || priorVelocity_ != velocity))
                flags = StepClockFlags::Reaim;

            if (flags != 0 && clockEventCount_ < clockEvents_.size()) {
                const bool withdrawing = flags == StepClockFlags::GateOff;
                clockEvents_[clockEventCount_++] = {
                    static_cast<std::uint32_t>(sample),
                    withdrawing ? priorSourceOrdinal_
                                : static_cast<std::uint32_t>(step),
                    withdrawing ? priorIteration_ : iteration,
                    pitch,
                    velocity,
                    flags
                };
            }

            priorGate_ = gateHigh;
            priorSourceOrdinal_ = static_cast<std::uint32_t>(step);
            priorIteration_ = iteration;
            priorPitch_ = pitch;
            priorVelocity_ = velocity;
        }

        return { clockEvents_.data(), clockEventCount_, n };
    }

    StepClockPlanView preparedPlan(int numSamples) const noexcept
    {
        const int n = std::min(
            std::max(0, numSamples), static_cast<int>(sampledGate_.size()));
        return { clockEvents_.data(), clockEventCount_, n };
    }

    const float* sampledGate() const noexcept { return sampledGate_.data(); }
    const float* sampledPitch() const noexcept { return sampledPitch_.data(); }
    const float* sampledVelocity() const noexcept { return sampledVelocity_.data(); }

    static int inputChannelsFor(Mode mode) noexcept
    {
        return mode == Mode::Pitch ? 38 : 21;
    }
    static int rateInputChannel(Mode) noexcept { return 0; }
    static int incInputChannel(Mode) noexcept { return 1; }
    static int decInputChannel(Mode) noexcept { return 2; }
    static int resetInputChannel(Mode) noexcept { return 3; }
    static int selectInputChannel(Mode) noexcept { return 4; }
    static int velocityInputChannel(Mode mode) noexcept
    {
        return mode == Mode::Pitch ? 5 : -1;
    }
    static int stepInputBase(Mode mode) noexcept
    {
        return mode == Mode::Pitch ? 6 : 5;
    }
    static int pitchInputBase(Mode mode) noexcept
    {
        return mode == Mode::Pitch ? 22 : -1;
    }

    static std::string inputName(Mode mode, int channel)
    {
        switch (channel) {
            case 0: return "RATE";
            case 1: return "INC";
            case 2: return "DEC";
            case 3: return "RESET";
            case 4: return "SELECT";
            default: break;
        }
        if (mode == Mode::Pitch && channel == 5)
            return "VELOCITY";
        const int stepBase = stepInputBase(mode);
        const int pitchBase = pitchInputBase(mode);
        const char* prefix = nullptr;
        int oneBased = 0;
        if (channel >= stepBase && channel < stepBase + kSteps) {
            prefix = "STEP";
            oneBased = channel - stepBase + 1;
        } else if (mode == Mode::Pitch
                   && channel >= pitchBase && channel < pitchBase + kSteps) {
            prefix = "PITCH";
            oneBased = channel - pitchBase + 1;
        }
        if (prefix == nullptr) return {};
        char name[16] {};
        std::snprintf(name, sizeof(name), "%s_%02d", prefix, oneBased);
        return name;
    }

private:
    static float sanitizeRate(float value) noexcept
    {
        return std::isfinite(value)
            ? std::clamp(value, -1.0f, 1.0f) : 1.0f;
    }

    int currentStepForSample(float rate, int sample) const noexcept
    {
        if (! hasTransportContext_ || ! transportPlaying_
            || std::abs(rate) <= 0.0001f)
            return manualStep_;
        const double samplesPerBeat = 60.0 * sampleRate_ / transportBpm_;
        const double beat = samplesPerBeat > 0.0
            ? static_cast<double>(blockStartSample_ + sample) / samplesPerBeat
            : 0.0;
        int step = static_cast<int>(
            std::floor(beat * kStepsPerBeat * std::abs(rate)));
        step %= kSteps;
        if (step < 0) step += kSteps;
        return rate < 0.0f ? (kSteps - 1) - step : step;
    }

    std::uint64_t currentIterationForSample(float rate, int sample) const noexcept
    {
        if (! hasTransportContext_ || ! transportPlaying_
            || std::abs(rate) <= 0.0001f)
            return priorIteration_;
        const double samplesPerBeat = 60.0 * sampleRate_ / transportBpm_;
        const double beat = samplesPerBeat > 0.0
            ? static_cast<double>(blockStartSample_ + sample) / samplesPerBeat
            : 0.0;
        const double position = std::floor(
            beat * kStepsPerBeat * std::abs(rate));
        return position > 0.0
            ? static_cast<std::uint64_t>(position)
                / static_cast<std::uint64_t>(kSteps)
            : 0u;
    }

    bool isLastSampleBeforeTransportStepBoundary(
        float rate, int sample) const noexcept
    {
        return hasTransportContext_ && transportPlaying_
            && std::abs(rate) > 0.0001f
            && currentStepForSample(rate, sample)
                != currentStepForSample(rate, sample + 1);
    }

    void applyManualControls(InputView inputs, int sample, int& step) noexcept
    {
        const bool reset = inputs.value(
            resetInputChannel(mode_), sample, 0.0f) >= 0.5f;
        const bool inc = inputs.value(
            incInputChannel(mode_), sample, 0.0f) >= 0.5f;
        const bool dec = inputs.value(
            decInputChannel(mode_), sample, 0.0f) >= 0.5f;
        const float select = inputs.value(
            selectInputChannel(mode_), sample, 0.0f);
        if (reset && ! prevReset_) manualStep_ = 0;
        if (inc && ! prevInc_) manualStep_ = (manualStep_ + 1) % kSteps;
        if (dec && ! prevDec_) manualStep_ = (manualStep_ + kSteps - 1) % kSteps;
        if (std::isfinite(select) && select >= 1.0f)
            manualStep_ = std::clamp(
                static_cast<int>(std::lround(select)) - 1, 0, kSteps - 1);
        prevReset_ = reset;
        prevInc_ = inc;
        prevDec_ = dec;
        step = manualStep_;
    }

    Mode mode_;
    double sampleRate_ = 44100.0;
    bool hasTransportContext_ = false;
    bool transportPlaying_ = false;
    double transportBpm_ = 120.0;
    std::int64_t blockStartSample_ = 0;
    int manualStep_ = 0;
    std::atomic<int> activeStep_ { 0 };
    float lastPitch_ = 0.0f;
    bool prevInc_ = false;
    bool prevDec_ = false;
    bool prevReset_ = false;
    std::vector<StepClockEvent> clockEvents_;
    std::vector<float> sampledGate_;
    std::vector<float> sampledPitch_;
    std::vector<float> sampledVelocity_;
    std::size_t clockEventCount_ = 0;
    bool priorGate_ = false;
    std::uint32_t priorSourceOrdinal_ = 0;
    std::uint64_t priorIteration_ = 0;
    float priorPitch_ = 0.0f;
    float priorVelocity_ = 0.0f;
};

} // namespace curlop
