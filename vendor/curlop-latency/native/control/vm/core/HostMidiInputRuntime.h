#pragma once

#include "control/vm/core/HostMidiVoicePlan.h"
#include "control/vm/core/VMConstNode.h"
#include "control/vm/core/VMRunnerHost.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <cstddef>

namespace curlop {

class HostMidiInputRuntime
{
public:
    using PlanHook = void (*)(
        void*, int, HostMidiVoicePlanView) noexcept;

    static constexpr int kControlCount = 2;
    static constexpr int kOutputCount = 4;

    void setPreparedPacketCapacity(std::size_t capacity) noexcept
    {
        preparedPacketCapacity_ = std::max<std::size_t>(4u, capacity);
    }

    std::size_t preparedPacketCapacity() const noexcept
    {
        return preparedPacketCapacity_;
    }

    void prepare() noexcept
    {
        voicePlanner_.prepare(
            preparedPacketCapacity_, preparedPacketCapacity_);
    }

    void setPreparedCcPacketOutput(bool enabled) noexcept
    {
        preparedCcPacketOutput_ = enabled;
        hasPreparedCcEmission_ = false;
        preparedCcChannel_ = -1;
        preparedCcNumber_ = -1;
        preparedCcValue_ = 0.0f;
    }

    void bindSource(const HostMidiNoteState* notes,
                    const std::atomic<float>* ccRowMajor,
                    int channels,
                    int ccs,
                    const HostMidiBlockState* blockState = nullptr) noexcept
    {
        sourceChanged_ = sourceChanged_
            || noteStates_ != notes
            || ccValues_ != ccRowMajor
            || channels_ != channels
            || ccs_ != ccs
            || blockState_ != blockState;
        noteStates_ = notes;
        ccValues_ = ccRowMajor;
        channels_ = channels;
        ccs_ = ccs;
        blockState_ = blockState;
    }

    void clearSource() noexcept
    {
        bindSource(nullptr, nullptr, 0, 0, nullptr);
    }

    HostMidiVoicePlanView voicePlan() const noexcept
    {
        return voicePlanner_.view();
    }

    void bindPlanHook(
        void* context, PlanHook hook, int graphIndex) noexcept
    {
        planHookContext_ = context;
        planHook_ = hook;
        planHookGraphIndex_ = graphIndex;
    }

    void clearPlanHook() noexcept
    {
        planHookContext_ = nullptr;
        planHook_ = nullptr;
        planHookGraphIndex_ = -1;
    }

    // The unified renderer prepares this stateful source before its final
    // eligibility check because the VM hook may itself update receiver
    // controls. If that late check rejects, the retained APG consumes these
    // exact prepared rows instead of advancing MIDI and the voice planner a
    // second time. A successful candidate disarms the one-block handoff.
    void beginPreparedCandidateBlock() noexcept
    {
        preparedCandidateCapture_ = true;
        preparedCandidateReuse_ = false;
        preparedCandidateSamples_ = 0;
        preparedCandidateOutputs_.fill(nullptr);
    }

    void commitPreparedCandidateBlock() noexcept
    {
        clearPreparedCandidateBlock();
    }

    void clearPreparedCandidateBlock() noexcept
    {
        preparedCandidateCapture_ = false;
        preparedCandidateReuse_ = false;
        preparedCandidateSamples_ = 0;
        preparedCandidateOutputs_.fill(nullptr);
    }

    template <typename ReadControl>
    bool process(float* gateOut,
                 float* pitchOut,
                 float* velocityOut,
                 float* ccOut,
                 int numSamples,
                 ReadControl readControl) noexcept
    {
        if (gateOut == nullptr || pitchOut == nullptr
            || velocityOut == nullptr || ccOut == nullptr
            || numSamples <= 0)
            return false;

        const std::array<float*, kOutputCount> outputs {
            gateOut, pitchOut, velocityOut, ccOut
        };
        if (preparedCandidateReuse_) {
            if (numSamples != preparedCandidateSamples_
                || std::any_of(
                    preparedCandidateOutputs_.begin(),
                    preparedCandidateOutputs_.end(),
                    [] (const float* row) { return row == nullptr; })) {
                clearPreparedCandidateBlock();
                return false;
            }
            for (std::size_t channel = 0; channel < outputs.size(); ++channel)
                std::memcpy(
                    outputs[channel], preparedCandidateOutputs_[channel],
                    static_cast<std::size_t>(numSamples) * sizeof(float));
            clearPreparedCandidateBlock();
            return true;
        }

        HostMidiNoteSnapshot notes[kHostMidiMaxChannels];
        float ccs[kHostMidiMaxChannels][kHostMidiMaxCCs];
        int eventIndex = 0;
        const bool useBlockState =
            blockState_ != nullptr && blockState_->numSamples > 0;
        const bool blockOverflow =
            useBlockState && blockState_->droppedEvents > 0;
        if (useBlockState) {
            for (int channel = 0; channel < kHostMidiMaxChannels; ++channel) {
                notes[channel] = blockState_->startNotes[channel];
                for (int cc = 0; cc < kHostMidiMaxCCs; ++cc)
                    ccs[channel][cc] = blockState_->startCCs[channel][cc];
            }
        }

        voicePlanner_.beginBlock(numSamples);
        if (sourceChanged_) {
            voicePlanner_.allNotesOff(0);
            voicePlanner_.selectChannel(readChannel(readControl, 0), 0);
            if (blockState_ != nullptr)
                for (int held = 0;
                     held < blockState_->startHeldCount;
                     ++held) {
                    const auto& note = blockState_->startHeld[held];
                    voicePlanner_.noteOn(
                        0, note.channel, note.note, note.velocity);
                }
            sourceChanged_ = false;
        }
        if (blockOverflow) {
            voicePlanner_.allNotesOff(0);
            for (auto& note : notes) {
                note.gate = 0.0f;
                note.velocity = 0.0f;
                note.note = -1;
            }
        }

        for (int sample = 0; sample < numSamples; ++sample) {
            if (useBlockState && ! blockOverflow)
                applyEventsThroughSample(notes, ccs, eventIndex, sample);

            const int channel = readChannel(readControl, sample);
            const int cc = readCC(readControl, sample);
            voicePlanner_.selectChannel(channel, sample);

            float gate = 0.0f;
            float pitch = 0.0f;
            float velocity = 0.0f;
            if (useBlockState
                && channel >= 0 && channel < kHostMidiMaxChannels) {
                const auto& state = notes[channel];
                gate = sanitize(state.gate, 0.0f);
                pitch = sanitize(state.pitch, 0.0f);
                velocity = sanitize(state.velocity, 0.0f);
            }
            else if (noteStates_ != nullptr
                     && channel >= 0 && channel < channels_) {
                const auto& state = noteStates_[channel];
                gate = sanitize(
                    state.gate.load(std::memory_order_relaxed), 0.0f);
                pitch = sanitize(
                    state.pitch.load(std::memory_order_relaxed), 0.0f);
                velocity = sanitize(
                    state.velocity.load(std::memory_order_relaxed), 0.0f);
            }

            float ccValue = 0.0f;
            if (useBlockState
                && channel >= 0 && channel < kHostMidiMaxChannels
                && cc >= 0 && cc < kHostMidiMaxCCs) {
                const float raw = ccs[channel][cc];
                ccValue = std::isfinite(raw) ? raw : 0.0f;
            }
            else if (ccValues_ != nullptr
                     && channel >= 0 && channel < channels_
                     && cc >= 0 && cc < ccs_) {
                const float raw = ccValues_[channel * ccs_ + cc].load(
                    std::memory_order_relaxed);
                ccValue = std::isfinite(raw) ? raw : 0.0f;
            }

            gateOut[sample] = std::clamp(gate, 0.0f, 1.0f);
            pitchOut[sample] = pitch;
            velocityOut[sample] = std::clamp(velocity, 0.0f, 1.0f);
            ccOut[sample] = std::clamp(ccValue, 0.0f, 1.0f);
            if (preparedCcPacketOutput_
                && (! hasPreparedCcEmission_
                    || preparedCcChannel_ != channel
                    || preparedCcNumber_ != cc
                    || preparedCcValue_ != ccOut[sample])) {
                if (voicePlanner_.appendValue(sample, ccOut[sample], 3u,
                                               channel)) {
                    hasPreparedCcEmission_ = true;
                    preparedCcChannel_ = channel;
                    preparedCcNumber_ = cc;
                    preparedCcValue_ = ccOut[sample];
                }
            }
        }
        if (planHook_ != nullptr)
            planHook_(
                planHookContext_, planHookGraphIndex_, voicePlanner_.view());
        if (preparedCandidateCapture_) {
            for (std::size_t channel = 0; channel < outputs.size(); ++channel)
                preparedCandidateOutputs_[channel] = outputs[channel];
            preparedCandidateSamples_ = numSamples;
            preparedCandidateCapture_ = false;
            preparedCandidateReuse_ = true;
        }
        return true;
    }

private:
    template <typename ReadControl>
    static int readChannel(ReadControl& readControl, int sample) noexcept
    {
        const float value = readControl(0, sample, 1.0f);
        return std::clamp(static_cast<int>(std::lround(value)), 1, 16) - 1;
    }

    template <typename ReadControl>
    static int readCC(ReadControl& readControl, int sample) noexcept
    {
        const float value = readControl(1, sample, 1.0f);
        return std::clamp(static_cast<int>(std::lround(value)), 0, 127);
    }

    static float sanitize(float value, float fallback) noexcept
    {
        return std::isfinite(value) ? value : fallback;
    }

    void applyEventsThroughSample(
        HostMidiNoteSnapshot (&notes)[kHostMidiMaxChannels],
        float (&ccs)[kHostMidiMaxChannels][kHostMidiMaxCCs],
        int& eventIndex,
        int sample) noexcept
    {
        while (eventIndex < blockState_->eventCount
               && blockState_->events[eventIndex].sample <= sample) {
            const auto& event = blockState_->events[eventIndex++];
            if (event.type == HostMidiBlockEventType::AllNotesOff) {
                for (auto& state : notes) {
                    state.gate = 0.0f;
                    state.velocity = 0.0f;
                    state.note = -1;
                }
                voicePlanner_.allNotesOff(event.sample);
                continue;
            }
            const int channel = event.channel;
            if (channel < 0 || channel >= kHostMidiMaxChannels)
                continue;
            auto& state = notes[channel];
            switch (event.type) {
                case HostMidiBlockEventType::Controller:
                    ccs[channel][event.data1] = std::clamp(
                        static_cast<float>(event.value) / 127.0f,
                        0.0f, 1.0f);
                    break;
                case HostMidiBlockEventType::NoteOn:
                    if (event.value == 0) {
                        if (state.note == event.data1) {
                            state.gate = 0.0f;
                            state.velocity = 0.0f;
                        }
                        voicePlanner_.noteOff(
                            event.sample, channel, event.data1);
                        break;
                    }
                    state.note = event.data1;
                    state.pitch = (static_cast<float>(event.data1) - 60.0f
                                   + state.pitchBendSemis) / 120.0f;
                    state.velocity = std::clamp(
                        static_cast<float>(event.value) / 127.0f,
                        0.0f, 1.0f);
                    state.gate = 1.0f;
                    voicePlanner_.noteOn(
                        event.sample, channel, event.data1, event.value);
                    break;
                case HostMidiBlockEventType::NoteOff:
                    if (state.note == event.data1) {
                        state.gate = 0.0f;
                        state.velocity = 0.0f;
                    }
                    voicePlanner_.noteOff(
                        event.sample, channel, event.data1);
                    break;
                case HostMidiBlockEventType::PitchWheel: {
                    const float normalized =
                        (static_cast<float>(event.value) - 8192.0f) / 8192.0f;
                    state.pitchBendSemis = std::clamp(
                        normalized * 2.0f, -2.0f, 2.0f);
                    if (state.note >= 0)
                        state.pitch = (static_cast<float>(state.note) - 60.0f
                                       + state.pitchBendSemis) / 120.0f;
                    voicePlanner_.pitchWheel(
                        event.sample, channel, event.value);
                    break;
                }
                case HostMidiBlockEventType::AllNotesOff:
                    break;
            }
        }
    }

    const HostMidiNoteState* noteStates_ = nullptr;
    const std::atomic<float>* ccValues_ = nullptr;
    const HostMidiBlockState* blockState_ = nullptr;
    HostMidiVoicePlanner voicePlanner_;
    int channels_ = 0;
    int ccs_ = 0;
    bool sourceChanged_ = true;
    void* planHookContext_ = nullptr;
    PlanHook planHook_ = nullptr;
    int planHookGraphIndex_ = -1;
    std::array<const float*, kOutputCount> preparedCandidateOutputs_ {};
    int preparedCandidateSamples_ = 0;
    bool preparedCandidateCapture_ = false;
    bool preparedCandidateReuse_ = false;
    bool preparedCcPacketOutput_ = false;
    bool hasPreparedCcEmission_ = false;
    int preparedCcChannel_ = -1;
    int preparedCcNumber_ = -1;
    float preparedCcValue_ = 0.0f;
    std::size_t preparedPacketCapacity_ =
        static_cast<std::size_t>(::curlop::kEventVectorReserve);
};

} // namespace curlop
