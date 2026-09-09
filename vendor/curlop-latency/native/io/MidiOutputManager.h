// CURLOP MIDI Output Manager
//
// Converts VM NoteEvents into proper MIDI note-on/note-off messages
// with active note tracking. Handles:
//   - Gate on  -> note-on (with retrigger: note-off first if already active)
//   - Gate off -> note-off for the specific note
//   - Panic    -> all notes off on all channels (transport stop)
//
// Module index maps to MIDI channel: module 0 = ch 1, module 1 = ch 2, etc.
// Wraps at 16 channels (module_idx % 16).
#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include "control/vm/machine/Machine.h"   // vm::Emission (F-066)
#include "control/vm/machine/Signal.h"
#include "shell/CurlopDebug.h"            // T-464: MIDI_OUT tap (SPEC-010)
#include <array>
#include <vector>

namespace curlop {

class MidiOutputManager {
public:
    void prepare(size_t voiceReserve)
    {
        for (auto& stages : voiceStage_)
            stages.assign(voiceReserve, VoiceStage {});
        clearActiveNotes();
        rtOverflowDropsThisBlock_ = 0;
    }

    // F-066 Phase 5: convert one module's machine emissions into MIDI.
    // Pitch/velocity packets stage per-voice state; a gate-on emits note-on
    // (with retrigger note-off), a gate-off emits note-off for the note the
    // on used. Module index maps to MIDI channel (module_idx % 16 + 1).
    void processEmissions(int moduleIdx,
                          const std::vector<vm::Emission>& ems,
                          juce::MidiBuffer& midiBuffer, int blockSize)
    {
        const int channel = (moduleIdx % 16) + 1;
        const int channelIdx = channel - 1;
        for (const auto& e : ems)
        {
            const int offset = juce::jlimit(0, blockSize - 1, (int) e.frameOffset);
            // B-425: key staging by chord position, not the unbounded per-note
            // instance id. prepare() sizes these lanes off the audio thread.
            if (e.voiceIdx >= voiceStage_[(size_t) channelIdx].size()) {
                ++rtOverflowDropsThisBlock_;
                continue;
            }
            auto& stage = voiceStage_[(size_t) channelIdx][(size_t) e.voiceIdx];

            if (e.type == vm::SignalType::Pitch && e.kind == vm::PacketKind::Set) {
                stage.midiNote = juce::jlimit(0, 127, (int) std::lround(
                    vm::noteToMidi(vm::signalToNote(e.value))));
            } else if (e.type == vm::SignalType::Velocity && e.kind == vm::PacketKind::Set) {
                stage.velocity = e.value;
            } else if (e.type == vm::SignalType::Gate && e.kind == vm::PacketKind::Set) {
                if (e.value > 0.5f) {
                    if (isActive(channelIdx, stage.midiNote)) {
                        midiBuffer.addEvent(
                            juce::MidiMessage::noteOff(channel, stage.midiNote), offset);
                        setActive(channelIdx, stage.midiNote, false);
                    }
                    int velocity = juce::jlimit(1, 127, (int) (stage.velocity * 127.0f));
                    midiBuffer.addEvent(
                        juce::MidiMessage::noteOn(channel, stage.midiNote,
                            (juce::uint8) velocity), offset);
                    setActive(channelIdx, stage.midiNote, true);
                    stage.activeNote = stage.midiNote;
                    CDBG_RT(MIDI_OUT, "note-on ch=%d note=%d vel=%d frame=%d voiceIdx=%u",
                            channel, stage.midiNote, velocity, offset, e.voiceIdx);
                } else if (stage.activeNote >= 0) {
                    if (isActive(channelIdx, stage.activeNote)) {
                        midiBuffer.addEvent(
                            juce::MidiMessage::noteOff(channel, stage.activeNote), offset);
                        setActive(channelIdx, stage.activeNote, false);
                        CDBG_RT(MIDI_OUT, "note-off ch=%d note=%d frame=%d voiceIdx=%u",
                                channel, stage.activeNote, offset, e.voiceIdx);
                    }
                    stage.activeNote = -1;
                }
            }
        }

        // SPEC-010 steady tap (~1 Hz at 512/48k): emissions seen + notes
        // held, so "no note lines" is distinguishable from "flag off".
        if (CurlopDebug::on(CurlopDebug::MIDI_OUT) && ! ems.empty()
            && (summaryTapBlocks_++ % 100 == 0)) {
            CDBG_RT(MIDI_OUT, "summary m=%d ems=%zu notesActive=%d",
                    moduleIdx, ems.size(), activeNoteCount());
        }
    }

    // Send note-off for all active notes, then CC 123 (All Notes Off) on
    // all 16 channels. Called on transport stop.
    void panic(juce::MidiBuffer& midiBuffer)
    {
        for (int ch = 0; ch < kChannels; ++ch)
            for (int note = 0; note < kNotes; ++note)
                if (activeNotes_[(size_t) ch][(size_t) note])
                    midiBuffer.addEvent(
                        juce::MidiMessage::noteOff(ch + 1, note), 0);
        clearActiveNotes();
        for (auto& stages : voiceStage_)
            for (auto& stage : stages)
                stage.activeNote = -1;

        // Belt-and-suspenders: CC 123 on all channels
        for (int ch = 1; ch <= 16; ++ch)
        {
            midiBuffer.addEvent(juce::MidiMessage::allNotesOff(ch), 0);
        }
    }

    bool hasActiveNotes() const { return activeNoteCount() > 0; }
    int activeNoteCount() const
    {
        int n = 0;
        for (const auto& ch : activeNotes_)
            for (bool active : ch)
                if (active) ++n;
        return n;
    }

    uint32_t drainRtOverflowDrops()
    {
        const uint32_t n = rtOverflowDropsThisBlock_;
        rtOverflowDropsThisBlock_ = 0;
        return n;
    }

private:
    static constexpr int kChannels = 16;
    static constexpr int kNotes = 128;

    struct VoiceStage { int midiNote = 60; float velocity = 0.7f; int activeNote = -1; };

    bool isActive(int channelIdx, int note) const noexcept
    {
        return channelIdx >= 0 && channelIdx < kChannels && note >= 0 && note < kNotes
            && activeNotes_[(size_t) channelIdx][(size_t) note];
    }

    void setActive(int channelIdx, int note, bool active) noexcept
    {
        if (channelIdx >= 0 && channelIdx < kChannels && note >= 0 && note < kNotes)
            activeNotes_[(size_t) channelIdx][(size_t) note] = active;
    }

    void clearActiveNotes() noexcept
    {
        for (auto& ch : activeNotes_)
            ch.fill(false);
    }

    std::array<std::array<bool, kNotes>, kChannels> activeNotes_ {};
    std::array<std::vector<VoiceStage>, kChannels> voiceStage_;
    uint32_t rtOverflowDropsThisBlock_ = 0;
    int summaryTapBlocks_ = 0;
};

} // namespace curlop
