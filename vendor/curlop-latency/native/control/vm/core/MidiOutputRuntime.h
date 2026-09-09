#pragma once

#include "control/vm/machine/Signal.h"
#include "io/MidiRouteManager.h"
#include "shell/CurlopDebug.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <string>

namespace curlop {

// Stateful MIDI-1 host sink shared by the unified renderer and its temporary
// APG oracle. It owns note, pitch-wheel, CC, route and telemetry lifetime;
// MidiOutputProcessor is only the retained APG adapter.
class MidiOutputRuntime
{
public:
    static constexpr int kGate = 0;
    static constexpr int kPitch = 1;
    static constexpr int kVelocity = 2;
    static constexpr int kChannel = 3;
    static constexpr int kCc = 4;
    static constexpr int kCcValue = 5;
    static constexpr int kInputChannels = 6;

    void configureRoute(std::string routeId)
    {
        ownedRouteId_ = std::move(routeId);
        routeId_ = &ownedRouteId_;
    }

    void bindMidiRouteManager(MidiRouteManager* manager) noexcept
    {
        routeManager_ = manager;
    }

    void bindLegacyMidiRoute(
        MidiRouteManager* manager, const std::string* routeId) noexcept
    {
        routeManager_ = manager;
        routeId_ = routeId;
    }

    void clearRouteBinding() noexcept
    {
        routeManager_ = nullptr;
        routeId_ = &ownedRouteId_;
    }

    void panic(juce::MidiBuffer& midi) noexcept
    {
        if (tombstoned_.load(std::memory_order_acquire))
            return;
        panicImpl(midi, true);
    }

    void tombstone(
        juce::MidiBuffer& midi, bool deliverLocalMidi) noexcept
    {
        if (tombstoned_.exchange(true, std::memory_order_acq_rel))
            return;
        panicImpl(midi, deliverLocalMidi);
    }

    bool tombstoned() const noexcept
    {
        return tombstoned_.load(std::memory_order_acquire);
    }

    bool noteActive() const noexcept
    {
        return noteActive_.load(std::memory_order_relaxed);
    }

    bool consumeGeneratedActivityTick() noexcept
    {
        int remaining = generatedActivityTicks_.load(
            std::memory_order_acquire);
        while (remaining > 0)
            if (generatedActivityTicks_.compare_exchange_weak(
                    remaining, remaining - 1,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire))
                return true;
        return false;
    }

    template <typename ReadControl>
    void processControls(int sampleCount,
                         juce::MidiBuffer& midi,
                         ReadControl read) noexcept
    {
        if (sampleCount <= 0
            || tombstoned_.load(std::memory_order_acquire))
            return;
        for (int sample = 0; sample < sampleCount; ++sample) {
            const float gate = std::clamp(
                read(kGate, sample, 0.0f), 0.0f, 1.0f);
            const float pitch = read(kPitch, sample, 0.0f);
            const float velocity = std::clamp(
                read(kVelocity, sample, 0.7f), 0.0f, 1.0f);
            const int channel = std::clamp(
                static_cast<int>(std::lround(
                    read(kChannel, sample, 1.0f))), 1, 16);

            const bool gateOpen = vm::isGateOpen(gate);
            const bool gateWasOpen = vm::isGateOpen(lastGate_);
            if (gateOpen && ! gateWasOpen) {
                if (activeNote_ >= 0)
                    emitMidi(midi, juce::MidiMessage::noteOff(
                        activeChannel_, activeNote_), sample);
                activeChannel_ = channel;
                activeNote_ = pitchToMidiNote(pitch);
                const int velocityByte = juce::jlimit(
                    1, 127,
                    static_cast<int>(std::lround(velocity * 127.0f)));
                emitMidi(midi, juce::MidiMessage::noteOn(
                    channel, activeNote_,
                    static_cast<juce::uint8>(velocityByte)), sample);
                emitPitchWheelIfChanged(midi, sample, pitch);
                CDBG_RT(MIDI_OUT,
                        "module-note-on ch=%d note=%d vel=%d frame=%d",
                        channel, activeNote_, velocityByte, sample);
            } else if (! gateOpen && gateWasOpen && activeNote_ >= 0) {
                emitMidi(midi, juce::MidiMessage::noteOff(
                    activeChannel_, activeNote_), sample);
                CDBG_RT(MIDI_OUT,
                        "module-note-off ch=%d note=%d frame=%d",
                        activeChannel_, activeNote_, sample);
                activeNote_ = -1;
                lastPitchWheel_ = -1;
            } else if (gateOpen && activeNote_ >= 0) {
                emitPitchWheelIfChanged(midi, sample, pitch);
            }

            const float ccValue = read(kCcValue, sample, -1.0f);
            if (std::isfinite(ccValue) && ccValue >= 0.0f) {
                const int cc = std::clamp(
                    static_cast<int>(std::lround(
                        read(kCc, sample, 1.0f))), 0, 127);
                const int value = juce::jlimit(
                    0, 127,
                    static_cast<int>(std::lround(ccValue * 127.0f)));
                if (cc != lastCc_ || value != lastCcValue_
                    || channel != lastCcChannel_) {
                    emitMidi(midi, juce::MidiMessage::controllerEvent(
                        channel, cc, value), sample);
                    lastCc_ = cc;
                    lastCcValue_ = value;
                    lastCcChannel_ = channel;
                    CDBG_RT(MIDI_OUT,
                            "module-cc ch=%d cc=%d val=%d frame=%d",
                            channel, cc, value, sample);
                }
            }
            lastGate_ = gate;
        }
        noteActive_.store(activeNote_ >= 0, std::memory_order_relaxed);
    }

    bool processPreparedControls(const float* const* controls,
                                 int controlCount,
                                 int sampleCount,
                                 juce::MidiBuffer& midi) noexcept
    {
        if (controls == nullptr || controlCount != kInputChannels
            || sampleCount <= 0)
            return false;
        processControls(sampleCount, midi, [controls] (
                int index, int sample, float fallback) {
            const auto* channel = controls[index];
            const float value = channel != nullptr
                ? channel[sample] : fallback;
            return std::isfinite(value) ? value : fallback;
        });
        return true;
    }

private:
    void panicImpl(
        juce::MidiBuffer& midi, bool deliverLocalMidi) noexcept
    {
        if (activeNote_ >= 0) {
            emitMidi(
                midi,
                juce::MidiMessage::noteOff(activeChannel_, activeNote_), 0,
                deliverLocalMidi);
            activeNote_ = -1;
        }
        for (int channel = 1; channel <= 16; ++channel)
            emitMidi(midi, juce::MidiMessage::allNotesOff(channel), 0,
                     deliverLocalMidi);
        lastGate_ = 0.0f;
        lastPitchWheel_ = -1;
        lastCcChannel_ = -1;
        lastCc_ = -1;
        lastCcValue_ = -1;
        noteActive_.store(false, std::memory_order_relaxed);
    }
    static int pitchToMidiNote(float pitch) noexcept
    {
        const float semisFromMiddleC = pitch * 120.0f;
        return juce::jlimit(
            0, 127,
            static_cast<int>(std::lround(60.0f + semisFromMiddleC)));
    }

    void emitPitchWheelIfChanged(
        juce::MidiBuffer& midi, int sample, float pitch) noexcept
    {
        if (activeNote_ < 0)
            return;
        const float semisFromMiddleC = pitch * 120.0f;
        const float activeSemis = static_cast<float>(activeNote_) - 60.0f;
        const float bendSemis = std::clamp(
            semisFromMiddleC - activeSemis, -2.0f, 2.0f);
        const int wheel = juce::jlimit(
            0, 16383,
            static_cast<int>(std::lround(
                8192.0f + (bendSemis / 2.0f) * 8191.0f)));
        if (wheel == lastPitchWheel_)
            return;
        emitMidi(midi,
                 juce::MidiMessage::pitchWheel(activeChannel_, wheel),
                 sample);
        lastPitchWheel_ = wheel;
    }

    void emitMidi(juce::MidiBuffer& midi,
                  const juce::MidiMessage& message,
                  int sample,
                  bool deliverLocalMidi = true) noexcept
    {
        generatedActivityTicks_.store(36, std::memory_order_release);
        if (routeManager_ != nullptr && routeId_ != nullptr
            && routeManager_->enqueueOutput(*routeId_, message, sample))
            return;
        if (deliverLocalMidi)
            midi.addEvent(message, sample);
    }

    std::string ownedRouteId_;
    MidiRouteManager* routeManager_ = nullptr;
    const std::string* routeId_ = &ownedRouteId_;
    float lastGate_ = 0.0f;
    int activeChannel_ = 1;
    int activeNote_ = -1;
    int lastPitchWheel_ = -1;
    int lastCcChannel_ = -1;
    int lastCc_ = -1;
    int lastCcValue_ = -1;
    std::atomic<bool> noteActive_ { false };
    std::atomic<int> generatedActivityTicks_ { 0 };
    std::atomic<bool> tombstoned_ { false };
};

} // namespace curlop
