#pragma once

#include "graph/engine/buffer/BufferReadCursor.h"
#include "graph/engine/buffer/BufferStorage.h"
#include "graph/engine/nodes/ControlCoreProcessor.h"
#include "modules/backend/CurlopDspNode.h"
#include <array>
#include <atomic>
#include <cstdint>
#include <cmath>
#include <chrono>
#include <memory>
#include <vector>

namespace curlop {

class BufferPlayerProcessor : public DspNode
{
public:
    static constexpr int kVisualBins = buffer::BufferStorage::kVisualBins;

    using VisualSnapshot = buffer::BufferStorage::VisualSnapshot;

    enum Input {
        PlayGate = 0,
        RecGate,
        Position,
        Rate,
        Start,
        End,
        Loop,
        Interp,
        Gain,
        FadeInMs,
        FadeOutMs,
        Count,
        RecAudioL = Count,
        RecAudioR,
        TotalInputChannels
    };

    struct PrepareTiming {
        double totalUs = 0.0;
        double storagePrepareUs = 0.0;
        double scratchSetSizeUs = 0.0;
        double stateResetUs = 0.0;
        double publishStateUs = 0.0;
    };

    explicit BufferPlayerProcessor(std::shared_ptr<buffer::BufferStorage> storage = {})
        : DspNode(BusesProperties()
            .withInput("In", juce::AudioChannelSet::discreteChannels(TotalInputChannels))
            .withOutput("Out", juce::AudioChannelSet::stereo())),
          storage_(storage ? std::move(storage) : std::make_shared<buffer::BufferStorage>())
    {
        for (auto& flag : externalControlConnected_)
            flag.store(false, std::memory_order_relaxed);
    }

    const juce::String getName() const override { return "BufferPlayer"; }

    void bindParam(int index, ControlCoreProcessor* core)
    {
        if (index < 0)
            return;
        if (index >= (int) params_.size())
            params_.resize((size_t) index + 1, nullptr);
        params_[(size_t) index] = core;
    }

    void setExternalControlConnected(int index, bool connected)
    {
        if (index < 0 || index >= Count)
            return;
        externalControlConnected_[(size_t) index].store(connected,
                                                        std::memory_order_relaxed);
    }

    void prepareToPlay(double sampleRate, int blockSize) override
    {
        const auto prepareStart = std::chrono::steady_clock::now();
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
        const auto storageStart = std::chrono::steady_clock::now();
        storage_->prepare(sampleRate);
        const auto storageEnd = std::chrono::steady_clock::now();
        scratch_.setSize(2, juce::jmax(1, blockSize), false, false, true);
        const auto scratchEnd = std::chrono::steady_clock::now();
        previousRecGateHigh_ = false;
        previousPlayGateHigh_ = false;
        releaseActive_ = false;
        fadeInRemaining_ = 0;
        fadeInTotal_ = 0;
        fadeOutRemaining_ = 0;
        fadeOutTotal_ = 0;
        state_.reset();
        const auto resetEnd = std::chrono::steady_clock::now();
        publishCursorState();
        const auto publishEnd = std::chrono::steady_clock::now();
        lastPrepareTiming_.storagePrepareUs = std::chrono::duration<double, std::micro>(
            storageEnd - storageStart).count();
        lastPrepareTiming_.scratchSetSizeUs = std::chrono::duration<double, std::micro>(
            scratchEnd - storageEnd).count();
        lastPrepareTiming_.stateResetUs = std::chrono::duration<double, std::micro>(
            resetEnd - scratchEnd).count();
        lastPrepareTiming_.publishStateUs = std::chrono::duration<double, std::micro>(
            publishEnd - resetEnd).count();
        lastPrepareTiming_.totalUs = std::chrono::duration<double, std::micro>(
            publishEnd - prepareStart).count();
    }

    PrepareTiming lastPrepareTiming() const noexcept { return lastPrepareTiming_; }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        const int n = buffer.getNumSamples();
        if (n <= 0)
            return;

        if (scratch_.getNumSamples() < n) {
            buffer.clear();
            return;
        }

        const bool recGateHigh = readGateControl(buffer, RecGate, 0.0f) > 0.0f;
        storage_->setRecording(recGateHigh);
        if (recGateHigh) {
            if (!previousRecGateHigh_) {
                storage_->beginRecording();
            }

            recordInput(buffer, n);
            previousRecGateHigh_ = true;
        }

        const bool justStoppedRecording = previousRecGateHigh_;
        if (!recGateHigh)
            previousRecGateHigh_ = false;
        if (!recGateHigh && justStoppedRecording) {
            storage_->promoteRecordingToCommitted();
            storage_->requestVisualRebuild();
        }

        if (storage_->capturedSamples() <= 0) {
            storage_->setPlaying(false);
            buffer.clear();
            return;
        }

        buffer::ReadParams params;
        params.playGate = readGateControl(buffer, PlayGate, 0.0f);
        params.position = readControl(buffer, Position, 0, 0.0f);
        params.rate = readControl(buffer, Rate, 0, 1.0f);
        params.start = readControl(buffer, Start, 0, 0.0f);
        params.end = readControl(buffer, End, 0, 1.0f);
        params.loop = readControl(buffer, Loop, 0, 1.0f) > 0.5f;
        params.interpolation = interpolationFrom(readControl(buffer, Interp, 0, 1.0f));
        requestObservedResidency(params, n);
        const float gain = juce::jlimit(0.0f, 1.0f, readControl(buffer, Gain, 0, 0.4f));
        const int fadeInSamples = msToSamples(readControl(buffer, FadeInMs, 0, 2.0f));
        const int fadeOutSamples = msToSamples(readControl(buffer, FadeOutMs, 0, 2.0f));
        const bool gateHigh = params.playGate > 0.0f;

        if (!gateHigh) {
            if (!releaseActive_ && previousPlayGateHigh_ && fadeOutSamples > 0 && state_.active) {
                releaseActive_ = true;
                fadeOutTotal_ = fadeOutSamples;
                fadeOutRemaining_ = fadeOutSamples;
            }

            if (!releaseActive_) {
                state_.reset();
                previousPlayGateHigh_ = false;
                storage_->setPlaying(false);
                buffer.clear();
                return;
            }

            params.playGate = 1.0f;
            buffer::BufferReadCursor::render(capturedView(),
                                             scratch_,
                                             state_,
                                             params,
                                             0,
                                             n);
            applyFadeOut(scratch_, n);
            if (fadeOutRemaining_ <= 0 || !state_.active) {
                releaseActive_ = false;
                state_.reset();
            }
            previousPlayGateHigh_ = false;
            publishCursorState();

            buffer.clear();
            for (int ch = 0; ch < juce::jmin(2, buffer.getNumChannels()); ++ch)
                buffer.copyFrom(ch, 0, scratch_.getReadPointer(ch), n, gain);
            return;
        }

        if (!previousPlayGateHigh_ || releaseActive_) {
            releaseActive_ = false;
            fadeOutRemaining_ = 0;
            fadeInTotal_ = fadeInSamples;
            fadeInRemaining_ = fadeInSamples;
        }

        if (!recGateHigh && justStoppedRecording && previousPlayGateHigh_) {
            state_.active = false;
            state_.previousGateHigh = false;
        }
        previousPlayGateHigh_ = gateHigh;

        buffer::BufferReadCursor::render(capturedView(),
                                         scratch_,
                                         state_,
                                         params,
                                         0,
                                         n);
        applyFadeIn(scratch_, n);
        if (state_.endedThisBlock && fadeOutSamples > 0)
            applyTailFadeOut(scratch_, state_.renderedSamples, fadeOutSamples);
        publishCursorState();

        buffer.clear();
        for (int ch = 0; ch < juce::jmin(2, buffer.getNumChannels()); ++ch)
            buffer.copyFrom(ch, 0, scratch_.getReadPointer(ch), n, gain);
    }

    VisualSnapshot visualSnapshot() noexcept
    {
        return storage_->visualSnapshot();
    }

private:
    float readControl(const juce::AudioBuffer<float>& buffer, int channel, int sample, float fallback) const noexcept
    {
        if (channel < 0 || sample < 0 || sample >= buffer.getNumSamples())
            return fallback;

        float value = fallback;
        if (channel < (int) params_.size()) {
            const auto* core = params_[(size_t) channel];
            if (core != nullptr)
                value = core->getOutputValue();
        }
        const bool hasExternalControl =
            channel < Count
            && externalControlConnected_[(size_t) channel].load(std::memory_order_relaxed);
        if (hasExternalControl && channel < buffer.getNumChannels())
            value += buffer.getSample(channel, sample);
        return std::isfinite(value) ? value : fallback;
    }

    float readGateControl(const juce::AudioBuffer<float>& buffer, int channel, float fallback) const noexcept
    {
        if (channel < 0)
            return fallback;

        float value = fallback;
        if (channel < (int) params_.size()) {
            const auto* core = params_[(size_t) channel];
            if (core != nullptr)
                value = core->getOutputValue();
        }

        const bool hasExternalControl =
            channel < Count
            && externalControlConnected_[(size_t) channel].load(std::memory_order_relaxed);
        if (!hasExternalControl || channel >= buffer.getNumChannels())
            return std::isfinite(value) ? value : fallback;

        float maxValue = std::isfinite(value) ? value : fallback;
        const auto* samples = buffer.getReadPointer(channel);
        for (int i = 0; i < buffer.getNumSamples(); ++i) {
            const float sampleValue = value + samples[i];
            if (std::isfinite(sampleValue))
                maxValue = juce::jmax(maxValue, sampleValue);
        }
        return maxValue;
    }

    static buffer::InterpolationMode interpolationFrom(float value) noexcept
    {
        const int mode = juce::jlimit(0, 4, (int) std::round(value));
        return static_cast<buffer::InterpolationMode>(mode);
    }

    int msToSamples(float milliseconds) const noexcept
    {
        const float clamped = juce::jlimit(0.0f, 100.0f, finiteOrZero(milliseconds));
        return (int) std::round((double) clamped * sampleRate_ * 0.001);
    }

    void applyFadeIn(juce::AudioBuffer<float>& audio, int numSamples) noexcept
    {
        if (fadeInRemaining_ <= 0 || fadeInTotal_ <= 0)
            return;

        const int channels = audio.getNumChannels();
        for (int i = 0; i < numSamples && fadeInRemaining_ > 0; ++i) {
            const int elapsed = fadeInTotal_ - fadeInRemaining_;
            const float gain = fadeInTotal_ <= 1 ? 1.0f
                : (float) elapsed / (float) fadeInTotal_;
            for (int ch = 0; ch < channels; ++ch)
                audio.setSample(ch, i, audio.getSample(ch, i) * gain);
            --fadeInRemaining_;
        }
    }

    void applyFadeOut(juce::AudioBuffer<float>& audio, int numSamples) noexcept
    {
        if (fadeOutRemaining_ <= 0 || fadeOutTotal_ <= 0) {
            audio.clear();
            return;
        }

        const int channels = audio.getNumChannels();
        for (int i = 0; i < numSamples; ++i) {
            if (fadeOutRemaining_ <= 0) {
                audio.clear(i, numSamples - i);
                return;
            }

            const float gain = (float) fadeOutRemaining_ / (float) fadeOutTotal_;
            for (int ch = 0; ch < channels; ++ch)
                audio.setSample(ch, i, audio.getSample(ch, i) * gain);
            --fadeOutRemaining_;
        }
    }

    void applyTailFadeOut(juce::AudioBuffer<float>& audio,
                          int renderedSamples,
                          int fadeOutSamples) noexcept
    {
        const int tail = juce::jmin(renderedSamples, fadeOutSamples);
        if (tail <= 0)
            return;

        const int start = renderedSamples - tail;
        const int channels = audio.getNumChannels();
        for (int i = 0; i < tail; ++i) {
            const float gain = (float) (tail - i) / (float) tail;
            for (int ch = 0; ch < channels; ++ch)
                audio.setSample(ch, start + i, audio.getSample(ch, start + i) * gain);
        }
    }

    void recordInput(const juce::AudioBuffer<float>& buffer, int numSamples) noexcept
    {
        const int writePosition = storage_->recordingWritePosition();
        storage_->requestRecordingPreparedSamples(writePosition + numSamples + (int) std::round(sampleRate_ * 4.0));
        const int capacity = storage_->recordingCapacitySamples();
        const int writable = juce::jmin(numSamples, capacity - writePosition);
        if (writable <= 0) {
            storage_->markRecordingOverrun();
            return;
        }

        const bool hasLeft = buffer.getNumChannels() > RecAudioL;
        const bool hasRight = buffer.getNumChannels() > RecAudioR;
        for (int i = 0; i < writable; ++i) {
            const float left = hasLeft ? finiteOrZero(buffer.getSample(RecAudioL, i)) : 0.0f;
            const float right = hasRight ? finiteOrZero(buffer.getSample(RecAudioR, i)) : left;
            if (!storage_->writeRecordingStereoFrame(writePosition + i, left, right)) {
                storage_->markRecordingOverrun();
                storage_->publishRecordingSamples(juce::jmax(storage_->recordingSamples(),
                                                             writePosition + i));
                return;
            }
        }

        storage_->setRecordingWritePosition(writePosition + writable);
        storage_->publishRecordingSamples(juce::jmax(storage_->recordingSamples(),
                                                     storage_->recordingWritePosition()));
        if (writable < numSamples)
            storage_->markRecordingOverrun();
    }

    buffer::BufferView capturedView() const noexcept
    {
        return storage_->view();
    }

    void requestObservedResidency(const buffer::ReadParams& params, int numSamples) noexcept
    {
        const int samples = storage_->capturedSamples();
        if (samples <= 0)
            return;

        const float startNorm = juce::jlimit(0.0f, 1.0f, finiteOrZero(params.start));
        const float endNorm = juce::jlimit(0.0f, 1.0f, finiteOrZero(params.end));
        const int rangeStart = juce::jlimit(0, samples, (int) std::floor(startNorm * (float) samples));
        const int rangeEnd = juce::jlimit(0, samples, (int) std::ceil(endNorm * (float) samples));
        if (rangeEnd <= rangeStart) {
            storage_->requestResidentFrameRange(0, samples);
            return;
        }

        const int length = rangeEnd - rangeStart;
        const float pos = juce::jlimit(0.0f, 1.0f, finiteOrZero(params.position));
        const double targetFrame = length <= 1
            ? (double) rangeStart
            : (double) rangeStart + (double) pos * (double) (length - 1);

        double lo = targetFrame;
        double hi = targetFrame;
        const bool scrubMode = std::abs(params.rate) <= 1.0e-6f;
        if (params.playGate > 0.0f) {
            const double observedStart = state_.active ? state_.playheadFrame : targetFrame;
            lo = juce::jmin(lo, observedStart);
            hi = juce::jmax(hi, observedStart);
            if (scrubMode) {
                lo = juce::jmin(lo, targetFrame);
                hi = juce::jmax(hi, targetFrame);
            } else {
                const double travelEnd = observedStart + (double) params.rate * (double) juce::jmax(1, numSamples);
                const int halo = interpolationHalo(params.interpolation)
                    + (int) std::ceil(std::abs(params.rate) * 2.0f);
                if (params.loop && crossesLoopBoundary(observedStart, travelEnd, rangeStart, rangeEnd)) {
                    requestWrappedResidency(observedStart, travelEnd, rangeStart, rangeEnd, halo);
                    return;
                }
                lo = juce::jmin(lo, travelEnd);
                hi = juce::jmax(hi, travelEnd);
            }
        }

        const int halo = interpolationHalo(params.interpolation) + (int) std::ceil(std::abs(params.rate) * 2.0f);
        storage_->requestResidentFrameRange((int) std::floor(lo) - halo,
                                            (int) std::ceil(hi) + halo + 1);
    }

    void requestWrappedResidency(double observedStart,
                                 double travelEnd,
                                 int rangeStart,
                                 int rangeEnd,
                                 int halo) noexcept
    {
        const int length = rangeEnd - rangeStart;
        if (length <= 0)
            return;
        if (std::abs(travelEnd - observedStart) >= (double) length) {
            storage_->requestResidentFrameRange((int) std::floor(observedStart) - halo,
                                                (int) std::ceil(observedStart) + halo + 1);
            return;
        }

        if (travelEnd >= (double) rangeEnd) {
            const double overflow = travelEnd - (double) rangeEnd;
            storage_->requestResidentFrameRanges((int) std::floor(observedStart) - halo,
                                                 rangeEnd,
                                                 rangeStart,
                                                 (int) std::ceil((double) rangeStart + overflow) + halo + 1);
            return;
        }

        const double underflow = (double) rangeStart - travelEnd;
        storage_->requestResidentFrameRanges(rangeStart,
                                             (int) std::ceil(observedStart) + halo + 1,
                                             (int) std::floor((double) rangeEnd - underflow) - halo,
                                             rangeEnd);
    }

    static bool crossesLoopBoundary(double start,
                                    double end,
                                    int rangeStart,
                                    int rangeEnd) noexcept
    {
        if (rangeEnd <= rangeStart)
            return false;
        return (start < (double) rangeEnd && end >= (double) rangeEnd)
            || (start >= (double) rangeStart && end < (double) rangeStart);
    }

    static int interpolationHalo(buffer::InterpolationMode mode) noexcept
    {
        switch (mode) {
            case buffer::InterpolationMode::Hold: return 1;
            case buffer::InterpolationMode::Linear: return 2;
            case buffer::InterpolationMode::CatmullRom:
            case buffer::InterpolationMode::Lagrange4: return 4;
            case buffer::InterpolationMode::WindowedSinc8: return 8;
        }
        return 2;
    }

    static float finiteOrZero(float value) noexcept
    {
        return std::isfinite(value) ? value : 0.0f;
    }

    void publishCursorState() noexcept
    {
        const int capturedSamples = storage_->capturedSamples();
        const bool active = state_.active && state_.previousGateHigh && capturedSamples > 0;
        storage_->setPlaying(active);
        float normalized = 0.0f;
        if (capturedSamples > 1)
            normalized = (float) juce::jlimit(0.0, 1.0, state_.playheadFrame / (double) (capturedSamples - 1));
        storage_->setPlayhead(normalized);
    }

    std::shared_ptr<buffer::BufferStorage> storage_;
    juce::AudioBuffer<float> scratch_;
    buffer::ReadState state_;
    double sampleRate_ = 48000.0;
    PrepareTiming lastPrepareTiming_;
    bool releaseActive_ = false;
    int fadeInRemaining_ = 0;
    int fadeInTotal_ = 0;
    int fadeOutRemaining_ = 0;
    int fadeOutTotal_ = 0;
    bool previousRecGateHigh_ = false;
    bool previousPlayGateHigh_ = false;
    std::vector<ControlCoreProcessor*> params_;
    std::array<std::atomic<bool>, Count> externalControlConnected_;
};

} // namespace curlop
