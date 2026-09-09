#pragma once

#include "control/vm/machine/Machine.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace curlop {

struct HostMidiVoicePlanView {
    const vm::Emission* emissions = nullptr;
    std::size_t count = 0;
    int numSamples = 0;
    std::uint32_t droppedHeld = 0;
    std::uint32_t droppedEmissions = 0;
    std::uint32_t highWaterHeld = 0;
};

class HostMidiVoicePlanner {
public:
    void prepare(std::size_t heldCapacity,
                 std::size_t emissionCapacity)
    {
        held_.assign(heldCapacity, {});
        emissions_.assign(emissionCapacity, {});
        reset();
    }

    void reset() noexcept
    {
        for (auto& held : held_)
            held = {};
        emissionCount_ = 0;
        numSamples_ = 0;
        selectedChannel_ = -1;
        nextVoice_ = 1;
        nextSequence_ = 1;
        droppedHeld_ = 0;
        droppedEmissions_ = 0;
        publishedCount_ = 0;
        activeHeldCount_ = 0;
        highWaterHeld_ = 0;
        blockDroppedHeldStart_ = 0;
        blockDroppedEmissionsStart_ = 0;
        bendSemis_.fill(0.0f);
    }

    void beginBlock(int numSamples) noexcept
    {
        emissionCount_ = 0;
        numSamples_ = std::max(0, numSamples);
        blockDroppedHeldStart_ = droppedHeld_;
        blockDroppedEmissionsStart_ = droppedEmissions_;
        for (auto& held : held_)
            if (held.pendingRelease
                && appendGate(held, 0, false)) {
                held.pendingRelease = false;
                if (! held.active)
                    held = {};
            }
        retrySelectedAssertions();
    }

    void selectChannel(int channel, int frame) noexcept
    {
        channel = std::clamp(channel, 0, 15);
        frame = clampedFrame(frame);
        if (channel == selectedChannel_)
            return;

        if (selectedChannel_ >= 0)
            for (auto& held : held_)
                if (held.active && held.channel == selectedChannel_)
                    withdraw(held, frame);

        selectedChannel_ = channel;
        for (auto& held : held_)
            if (held.active && ! held.pendingRelease
                && held.channel == selectedChannel_
                && canPublishStrike()
                && appendStrike(held, frame)) {
                held.published = true;
                ++publishedCount_;
            }
    }

    void noteOn(int frame, int channel, int note, int velocity) noexcept
    {
        channel = std::clamp(channel, 0, 15);
        note = std::clamp(note, 0, 127);
        velocity = std::clamp(velocity, 0, 127);
        if (velocity == 0) {
            noteOff(frame, channel, note);
            return;
        }
        auto* held = firstFreeHeld();
        if (held == nullptr) {
            ++droppedHeld_;
            return;
        }

        held->active = true;
        held->channel = channel;
        held->note = note;
        held->velocity = static_cast<float>(velocity) / 127.0f;
        held->voice = nextVoice();
        held->voiceIdx = static_cast<std::uint32_t>(
            static_cast<std::size_t>(held - held_.data()));
        held->sequence = nextSequence_++;
        ++activeHeldCount_;
        highWaterHeld_ =
            std::max(highWaterHeld_, activeHeldCount_);

        if (channel == selectedChannel_) {
            if (canPublishStrike()
                && appendStrike(*held, clampedFrame(frame))) {
                held->published = true;
                ++publishedCount_;
            } else {
                // Keep the physical strike in held_ so the next block (or
                // a larger rebuilt generation) can assert it exactly once.
                droppedEmissions_ += 3u;
            }
        }
    }

    void noteOff(int frame, int channel, int note) noexcept
    {
        channel = std::clamp(channel, 0, 15);
        note = std::clamp(note, 0, 127);
        Held* oldest = nullptr;
        for (auto& held : held_)
            if (held.active && held.channel == channel
                && held.note == note
                && (oldest == nullptr
                    || held.sequence < oldest->sequence))
                oldest = &held;
        if (oldest == nullptr)
            return;

        if (oldest->published)
            withdraw(*oldest, clampedFrame(frame));
        oldest->active = false;
        if (activeHeldCount_ > 0u)
            --activeHeldCount_;
        if (! oldest->pendingRelease)
            *oldest = {};
    }

    void pitchWheel(int frame, int channel, int value) noexcept
    {
        channel = std::clamp(channel, 0, 15);
        const float normalized =
            (static_cast<float>(std::clamp(value, 0, 16383))
                - 8192.0f) / 8192.0f;
        bendSemis_[static_cast<std::size_t>(channel)] =
            std::clamp(normalized * 2.0f, -2.0f, 2.0f);
        if (channel != selectedChannel_)
            return;

        const int offset = clampedFrame(frame);
        for (const auto& held : held_)
            if (held.active && held.published
                && held.channel == channel)
                appendPitch(held, offset);
    }

    void allNotesOff(int frame) noexcept
    {
        const int offset = clampedFrame(frame);
        for (auto& held : held_) {
            if (! held.active)
                continue;
            if (held.published)
                withdraw(held, offset);
            held.active = false;
            if (activeHeldCount_ > 0u)
                --activeHeldCount_;
            if (! held.pendingRelease)
                held = {};
        }
    }

    HostMidiVoicePlanView view() const noexcept
    {
        return {
            emissions_.data(),
            emissionCount_,
            numSamples_,
            droppedHeld_ - blockDroppedHeldStart_,
            droppedEmissions_ - blockDroppedEmissionsStart_,
            highWaterHeld_
        };
    }

    std::uint32_t droppedHeld() const noexcept { return droppedHeld_; }
    std::uint32_t droppedEmissions() const noexcept
    {
        return droppedEmissions_;
    }

    bool appendValue(int frame, float value, std::uint32_t outputLane,
                     int channel) noexcept
    {
        vm::Emission emission;
        emission.type = vm::SignalType::Value;
        emission.kind = vm::PacketKind::Set;
        emission.value = std::clamp(value, 0.0f, 1.0f);
        emission.frameOffset = static_cast<float>(clampedFrame(frame));
        emission.channelIndex = static_cast<std::uint32_t>(
            std::max(0, channel));
        emission.stableChannelId =
            static_cast<std::uint64_t>(emission.channelIndex) + 1u;
        emission.lane = vm::namedOutputLane(outputLane);
        return append(emission);
    }

private:
    struct Held {
        bool active = false;
        bool published = false;
        bool pendingRelease = false;
        int channel = 0;
        int note = 0;
        float velocity = 0.0f;
        std::uint32_t voice = 0;
        std::uint32_t voiceIdx = 0;
        std::uint64_t sequence = 0;
    };

    Held* firstFreeHeld() noexcept
    {
        for (auto& held : held_)
            if (! held.active && ! held.pendingRelease)
                return &held;
        return nullptr;
    }

    std::uint32_t nextVoice() noexcept
    {
        if (nextVoice_ == 0)
            ++nextVoice_;
        return nextVoice_++;
    }

    int clampedFrame(int frame) const noexcept
    {
        return numSamples_ > 0
            ? std::clamp(frame, 0, numSamples_ - 1) : 0;
    }

    float pitchOf(const Held& held) const noexcept
    {
        return (static_cast<float>(held.note) - 60.0f
                + bendSemis_[static_cast<std::size_t>(held.channel)])
            / 120.0f;
    }

    bool append(vm::Emission emission) noexcept
    {
        if (emissionCount_ >= emissions_.size()) {
            ++droppedEmissions_;
            return false;
        }
        emission.sourceOrder =
            static_cast<std::uint32_t>(emissionCount_);
        emissions_[emissionCount_++] = emission;
        return true;
    }

    bool hasEmissionCapacity(std::size_t count) const noexcept
    {
        return count <= emissions_.size() - emissionCount_;
    }

    void appendPitch(const Held& held, int frame) noexcept
    {
        vm::Emission emission;
        emission.type = vm::SignalType::Pitch;
        emission.kind = vm::PacketKind::Set;
        emission.value = pitchOf(held);
        emission.frameOffset = static_cast<float>(frame);
        emission.voice = held.voice;
        emission.voiceIdx = held.voiceIdx;
        emission.channelIndex = held.voiceIdx;
        emission.stableChannelId =
            static_cast<std::uint64_t>(held.voiceIdx) + 1u;
        emission.lane = vm::namedOutputLane(1u);
        append(emission);
    }

    void appendVelocity(const Held& held, int frame) noexcept
    {
        vm::Emission emission;
        emission.type = vm::SignalType::Velocity;
        emission.kind = vm::PacketKind::Set;
        emission.value = held.velocity;
        emission.frameOffset = static_cast<float>(frame);
        emission.voice = held.voice;
        emission.voiceIdx = held.voiceIdx;
        emission.channelIndex = held.voiceIdx;
        emission.stableChannelId =
            static_cast<std::uint64_t>(held.voiceIdx) + 1u;
        emission.lane = vm::namedOutputLane(2u);
        append(emission);
    }

    bool appendGate(const Held& held, int frame, bool on) noexcept
    {
        vm::Emission emission;
        emission.type = vm::SignalType::Gate;
        emission.kind = vm::PacketKind::Set;
        emission.value = on ? 1.0f : 0.0f;
        emission.frameOffset = static_cast<float>(frame);
        emission.voice = held.voice;
        emission.voiceIdx = held.voiceIdx;
        emission.channelIndex = held.voiceIdx;
        emission.stableChannelId =
            static_cast<std::uint64_t>(held.voiceIdx) + 1u;
        emission.lane = vm::namedOutputLane(0u);
        return append(emission);
    }

    bool appendStrike(const Held& held, int frame) noexcept
    {
        if (! hasEmissionCapacity(3u)) {
            droppedEmissions_ += 3u;
            return false;
        }
        appendPitch(held, frame);
        appendVelocity(held, frame);
        appendGate(held, frame, true);
        return true;
    }

    bool canPublishStrike() const noexcept
    {
        return hasEmissionCapacity(
            3u + static_cast<std::size_t>(publishedCount_) + 1u);
    }

    void retrySelectedAssertions() noexcept
    {
        if (selectedChannel_ < 0)
            return;
        for (;;) {
            Held* oldest = nullptr;
            for (auto& held : held_)
                if (held.active && ! held.published
                    && ! held.pendingRelease
                    && held.channel == selectedChannel_
                    && (oldest == nullptr
                        || held.sequence < oldest->sequence))
                    oldest = &held;
            if (oldest == nullptr || ! canPublishStrike())
                return;
            if (! appendStrike(*oldest, 0))
                return;
            oldest->published = true;
            ++publishedCount_;
        }
    }

    void withdraw(Held& held, int frame) noexcept
    {
        if (! held.published)
            return;
        if (! appendGate(held, frame, false))
            held.pendingRelease = true;
        held.published = false;
        if (publishedCount_ > 0u)
            --publishedCount_;
    }

    std::vector<Held> held_;
    std::vector<vm::Emission> emissions_;
    std::array<float, 16> bendSemis_ {};
    std::size_t emissionCount_ = 0;
    int numSamples_ = 0;
    int selectedChannel_ = -1;
    std::uint32_t nextVoice_ = 1;
    std::uint64_t nextSequence_ = 1;
    std::uint32_t droppedHeld_ = 0;
    std::uint32_t droppedEmissions_ = 0;
    std::uint32_t publishedCount_ = 0;
    std::uint32_t activeHeldCount_ = 0;
    std::uint32_t highWaterHeld_ = 0;
    std::uint32_t blockDroppedHeldStart_ = 0;
    std::uint32_t blockDroppedEmissionsStart_ = 0;
};

} // namespace curlop
