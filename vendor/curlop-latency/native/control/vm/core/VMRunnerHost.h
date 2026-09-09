#pragma once
//
// VMRunnerHost — the declared dependency surface VMRunner needs from its
// owner (CurlopProcessor in production, a stub in unit tests).
//
// Extracted s470 (SF-057 testing pivot). Replaces the previous `CurlopProcessor&
// proc_` reference inside VMRunner. The motivation: VMRunner's phase methods
// (writePblockParamBuffers, applyPblockNoteEvents, applyPblockParamEvents,
// recomposeAfterEvents, applyPblockAbsoluteLockEvent, etc.) — the bug surface
// for SF-055 / B-232 first-loop param-lock corruption — are testable in
// isolation, but only if VMRunner can be constructed without instantiating
// CurlopProcessor (which pulls in NativeBridgeServer + AudioRecorder + the
// juce::AudioProcessor base — infrastructure that doesn't belong in a unit
// test).
//
// VMRunnerHost is a struct of references + std::function trampolines for the
// 28 distinct CurlopProcessor fields/methods VMRunner reads. CurlopProcessor's
// constructor brace-initialises one inline from its own fields; tests construct
// one from cheap stubs. Production behaviour: byte-identical (every read still
// goes through the same memory location). Test behaviour: VMRunner is now a
// pure C++ object with a declared dependency contract.
//
// Boundary methods (processBlock entry, drainPblockParamCommands,
// readPblockIncomingMidiCC, runPblockPostProcess, etc.) read most of these
// fields; phase methods are mostly clean except writePblockParamBuffers which
// reads midiCCValues_ for live MIDI-CC dispatch. Tests that only exercise
// phase methods can leave the boundary-only fields pointing at default-
// constructed stubs that are never touched.
//

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "io/MidiOutputManager.h"
#include "io/AudioRecorder.h"
#include "transport/reporter/TransportReporter.h"
#include "control/vm/params/ParamBufferCmd.h"
#include "transport/reporter/MeterAccumulator.h"

// s373: unified choc — see ParamFIFO.h.
#include "choc/containers/choc_SingleReaderSingleWriterFIFO.h"

namespace curlop {

class MidiRouteManager;

// MIDI CC table dimensions. Must match CurlopProcessor's MIDI_MAX_CHANNELS /
// MIDI_MAX_CC (16 / 128). A static_assert in CurlopProcessor.cpp guards drift.
inline constexpr int kHostMidiMaxChannels = 16;
inline constexpr int kHostMidiMaxCCs      = 128;
inline constexpr int kHostAutomationLaneCount = 128;

using MidiCCMatrix = std::atomic<float>[kHostMidiMaxChannels][kHostMidiMaxCCs];
struct HostMidiNoteState {
    std::atomic<float> gate { 0.0f };
    std::atomic<float> pitch { 0.0f };
    std::atomic<float> velocity { 0.0f };
    std::atomic<float> pitchBendSemis { 0.0f };
    std::atomic<int> note { -1 };
};
using MidiNoteStateArray = HostMidiNoteState[kHostMidiMaxChannels];
using HostAutomationValues = std::atomic<float>[kHostAutomationLaneCount];
using ParamCmdQueue = choc::fifo::SingleReaderSingleWriterFIFO<ParamBufferCmd>;

struct HostMidiNoteSnapshot {
    float gate = 0.0f;
    float pitch = 0.0f;
    float velocity = 0.0f;
    float pitchBendSemis = 0.0f;
    int note = -1;
};

enum class HostMidiBlockEventType : uint8_t {
    Controller,
    NoteOn,
    NoteOff,
    PitchWheel,
    AllNotesOff,
};

struct HostMidiBlockEvent {
    int sample = 0;
    HostMidiBlockEventType type = HostMidiBlockEventType::Controller;
    uint8_t channel = 0;
    uint8_t data1 = 0;
    int value = 0;
};

inline constexpr int kHostMidiBlockMaxEvents = 1024;

struct HostMidiHeldSnapshot {
    uint8_t channel = 0;
    uint8_t note = 0;
    uint8_t velocity = 0;
    uint64_t sequence = 0;
};

struct HostMidiBlockState {
    HostMidiNoteSnapshot startNotes[kHostMidiMaxChannels];
    float startCCs[kHostMidiMaxChannels][kHostMidiMaxCCs] {};
    HostMidiBlockEvent events[kHostMidiBlockMaxEvents];
    int eventCount = 0;
    int numSamples = 0;
    uint32_t droppedEvents = 0;
    HostMidiHeldSnapshot held[kHostMidiBlockMaxEvents];
    int heldCount = 0;
    HostMidiHeldSnapshot startHeld[kHostMidiBlockMaxEvents];
    int startHeldCount = 0;
    uint64_t nextHeldSequence = 1;

    void beginBlock(int samples,
                    const MidiNoteStateArray& notes,
                    const MidiCCMatrix& ccs) noexcept
    {
        numSamples = std::max(0, samples);
        eventCount = 0;
        droppedEvents = 0;
        startHeldCount = heldCount;
        std::copy_n(held, heldCount, startHeld);
        for (int ch = 0; ch < kHostMidiMaxChannels; ++ch) {
            startNotes[ch].gate = notes[ch].gate.load(std::memory_order_relaxed);
            startNotes[ch].pitch = notes[ch].pitch.load(std::memory_order_relaxed);
            startNotes[ch].velocity = notes[ch].velocity.load(std::memory_order_relaxed);
            startNotes[ch].pitchBendSemis = notes[ch].pitchBendSemis.load(std::memory_order_relaxed);
            startNotes[ch].note = notes[ch].note.load(std::memory_order_relaxed);
            for (int cc = 0; cc < kHostMidiMaxCCs; ++cc)
                startCCs[ch][cc] = ccs[ch][cc].load(std::memory_order_relaxed);
        }
    }

    void beginBlock(
        int samples,
        const HostMidiNoteSnapshot (&notes)[kHostMidiMaxChannels],
        const float (&ccs)[kHostMidiMaxChannels][kHostMidiMaxCCs]) noexcept
    {
        numSamples = std::max(0, samples);
        eventCount = 0;
        droppedEvents = 0;
        startHeldCount = heldCount;
        std::copy_n(held, heldCount, startHeld);
        for (int ch = 0; ch < kHostMidiMaxChannels; ++ch) {
            startNotes[ch] = notes[ch];
            for (int cc = 0; cc < kHostMidiMaxCCs; ++cc)
                startCCs[ch][cc] = ccs[ch][cc];
        }
    }

    bool push(HostMidiBlockEventType type, int sample, int channel, int data1, int value) noexcept
    {
        if (numSamples <= 0) return false;
        if (eventCount >= kHostMidiBlockMaxEvents) {
            ++droppedEvents;
            heldCount = 0;
            return false;
        }
        auto& ev = events[eventCount++];
        ev.sample = std::clamp(sample, 0, numSamples - 1);
        ev.type = type;
        ev.channel = (uint8_t) std::clamp(channel, 0, kHostMidiMaxChannels - 1);
        ev.data1 = (uint8_t) std::clamp(data1, 0, 127);
        ev.value = value;
        updateHeld(ev);
        return true;
    }

private:
    void updateHeld(const HostMidiBlockEvent& event) noexcept
    {
        if (event.type == HostMidiBlockEventType::AllNotesOff) {
            heldCount = 0;
            return;
        }
        if (event.type == HostMidiBlockEventType::NoteOn
            && event.value > 0) {
            if (heldCount >= kHostMidiBlockMaxEvents)
                return;
            held[heldCount++] = {
                event.channel,
                event.data1,
                static_cast<uint8_t>(
                    std::clamp(event.value, 0, 127)),
                nextHeldSequence++
            };
            return;
        }
        if (event.type != HostMidiBlockEventType::NoteOff
            && ! (event.type == HostMidiBlockEventType::NoteOn
                  && event.value == 0))
            return;
        int oldest = -1;
        for (int index = 0; index < heldCount; ++index)
            if (held[index].channel == event.channel
                && held[index].note == event.data1
                && (oldest < 0
                    || held[index].sequence
                        < held[oldest].sequence))
                oldest = index;
        if (oldest < 0)
            return;
        for (int index = oldest + 1; index < heldCount; ++index)
            held[index - 1] = held[index];
        --heldCount;
    }
};

struct HostMidiInputSource {
    const HostMidiNoteState* notes = nullptr;
    const std::atomic<float>* ccs = nullptr;
    int channels = 0;
    int ccCount = 0;
    const HostMidiBlockState* blockState = nullptr;
};

// Optional route-source seam for hosted/test environments that do not own a
// JUCE device manager. Production standalone routing continues to use the
// concrete MidiRouteManager below; both paths resolve the same prepared source
// contract on the audio thread.
class HostMidiInputSourceProvider {
public:
    virtual ~HostMidiInputSourceProvider() = default;
    virtual HostMidiInputSource inputSourceForRoute(
        const std::string& routeId,
        HostMidiInputSource hostDefault) const noexcept = 0;
};

// SF-079 (FF-023) Phase 1 — note-on events forwarded audio thread → message
// thread for action dispatch. The audio thread (readPblockIncomingMidi) is the
// sole writer; CurlopProcessor::timerCallback is the sole reader (SRSW), where
// each note is matched against MidiActionBindings and dispatched via
// invokeDirectly. invokeDirectly must NOT run on the audio thread — hence the
// queue, the mirror of paramCmdQueue_ (message → audio) running in reverse.
struct MidiNoteEvent { uint8_t channel; uint8_t note; uint8_t velocity; };
using MidiNoteEventQueue = choc::fifo::SingleReaderSingleWriterFIFO<MidiNoteEvent>;

struct HostTransportSnapshot {
    bool hosted = false;
    bool available = false;
    bool playing = false;
    bool recording = false;
    bool looping = false;
    bool hasBpm = false;
    bool hasPpq = false;
    bool hasTimeSamples = false;
    double bpm = 120.0;
    double ppq = 0.0;
    int64_t timeSamples = 0;
};

namespace AudioThreadMessageRequest {
    constexpr uint32_t stopMasterRecorder = 1u << 0;
    constexpr uint32_t stopStemRecorders  = 1u << 1;
    constexpr uint32_t clearRecordTaps    = 1u << 2;
    constexpr uint32_t notifySlotSwapped  = 1u << 3;
    constexpr uint32_t freePendingSlots   = 1u << 4;
    constexpr uint32_t retireTombstonePublication = 1u << 5;
    constexpr uint32_t beginExternalStopTail = 1u << 6;
    constexpr uint32_t rebuildPreparedCapacity = 1u << 7;
}

struct AudioSlotSwapNotification {
    int slotId = -1;
    int clipId = -1;
    int fromSlotId = -1;
    int fromClipId = -1;
    uint64_t traceId = 0;
    int64_t samplePosition = 0;
    uint64_t audioBlockSequence = 0;
    uint8_t swapMode = 0;
};

using AudioSlotSwapNotificationQueue =
    choc::fifo::SingleReaderSingleWriterFIFO<AudioSlotSwapNotification>;

struct VMRunnerHost {
    // ── Audio I/O references (boundary use) ────────────────────────────────
    MidiOutputManager&  midiOutput_;
    AudioRecorder&      recorder_;            // F-070 SF-072 master-only continuous 2-track
    AudioRecorder&      stemsRecorder_;       // F-070 SF-072 Stems iso-track polywav (master-less)
    AudioRecorder&      companionRecorder_;   // F-070 SF-071 Everything companion polywav
    TransportReporter&  transportReporter_;
    MeterAccumulator&   meterAccumulator_;

    // ── Param command queue (boundary use, drainPblockParamCommands) ───────
    ParamCmdQueue&      paramCmdQueue_;

    // ── MIDI CC live table (phase use — writePblockParamBuffers reads this
    //    for the live MIDI-CC SetPacket emit at lines 1187+ in VMRunner.cpp).
    MidiCCMatrix&       midiCCValues_;
    MidiNoteStateArray& midiNoteStates_;
    HostMidiBlockState& hostMidiBlockState_;
    HostAutomationValues& hostAutomationValues_;

    // ── MIDI note-on forward queue (boundary use, readPblockIncomingMidi).
    //    SF-079 Phase 1 — audio thread pushes note-ons; the message thread
    //    matches them against MidiActionBindings and fires the action.
    MidiNoteEventQueue& midiNoteEvents_;

    // ── Transport / timing atomics (boundary use) ──────────────────────────
    std::atomic<double>&    currentBpm_;
    std::atomic<bool>&      transportPlaying_;
    std::atomic<bool>&      transportStateDirty_;
    std::atomic<bool>&      softStopped_;
    std::atomic<bool>&      hostTransportHosted_;
    std::atomic<bool>&      hostTransportAvailable_;
    std::atomic<bool>&      hostTransportPlaying_;
    std::atomic<bool>&      hostTransportRecording_;
    std::atomic<bool>&      hostTransportLooping_;
    std::atomic<bool>&      hostTransportHasBpm_;
    std::atomic<bool>&      hostTransportHasPpq_;
    std::atomic<bool>&      hostTransportSyncEnabled_;
    std::atomic<bool>&      hostTransportPpqLocked_;
    std::atomic<double>&    hostTransportBpm_;
    std::atomic<double>&    hostTransportPpq_;
    std::atomic<int64_t>&   hostTransportTimeSamples_;
    std::atomic<int>&       hostTransportSeekCount_;
    std::atomic<bool>&      hostTransportAutoLaunchRequested_;
    std::atomic<bool>&      hostTransportLocalSilence_;
    std::atomic<uint64_t>&  playbackStartCancelledThrough_;

    // ── Diagnostics + heartbeat (boundary use) ─────────────────────────────
    std::atomic<uint64_t>&  audioBlockSeq_;
    std::atomic<bool>&      chainLogReset_;
    std::atomic<double>&    cpuLoadVM_;
    std::atomic<double>&    cpuLoadDSP_;
    std::atomic<int64_t>&   vmSnap_samplePosition_;
    std::atomic<int64_t>&   vmSnap_loopLengthSamples_;

    // ── MIDI flags (boundary use) ──────────────────────────────────────────
    std::atomic<bool>&      midiCCDirty_;
    std::atomic<bool>&      midiPanicRequested_;

    // ── Staged-clip handoff (boundary use, applyPblockIncomingActivation) ──
    std::atomic<bool>&      stagedLoopReady_;
    std::atomic<double>&    stagedLoopLength_;
    std::atomic<int64_t>&   stagedLoopLengthSamples_;
    std::atomic<double>&    stagedSwapBpm_;
    std::atomic<uint32_t>&  stagedGlobalSeed_;

    // ── Preview loop (T-422, boundary use). Authoring affordance: reflected
    //    into every program's vmState.config each block on the audio thread, so
    //    the VM wraps sample_position within the cursor's scope. active=false →
    //    full-loop wrap (byte-identical baseline). Bounds are beats.
    std::atomic<bool>&      previewLoopActive_;
    std::atomic<double>&    previewLoopStartBeats_;
    std::atomic<double>&    previewLoopEndBeats_;

    // ── Audio thread → message thread requests ─────────────────────────────
    // VMRunner sets bits only; CurlopProcessor::timerCallback owns message-
    // thread side effects. Ordered slot-swap listener delivery uses the SPSC
    // queue because recorder fragment boundaries must not be coalesced.
    std::atomic<uint32_t>&  audioMessageRequests_;
    AudioSlotSwapNotificationQueue& audioSlotSwapNotifications_;

    // ── Method trampolines for non-field calls. CurlopProcessor's
    //    inherited juce::AudioProcessor::getSampleRate() is unreachable from
    //    a stub host without inheriting from AudioProcessor; the lambda
    //    indirection sidesteps that. updateHeartbeatMasterPeak is a member
    //    function on CurlopProcessor and gets the same treatment for the
    //    same reason. Both called from boundary methods only.
    std::function<double()>      getSampleRate;
    std::function<void(float)>   updateHeartbeatMasterPeak;
    std::function<void(const juce::AudioBuffer<float>&, int)> publishVisualizerTap;
    // Audio thread -> owner handoff for consumed EngineSlot::SequencerInstall.
    // Kept as void* here to avoid making VMRunnerHost depend on EngineSlot.h;
    // VMRunner and CurlopProcessor own the typed boundary.
    std::function<bool(void*)> retireConsumedInstall;

    // s505 (T-544) — every incoming CC (channel 1..16, cc 0..127, value 0..1)
    // forwarded so CurlopProcessor can drive master BPM from a mapped CC (and
    // capture during BPM-CC learn). Called from readPblockIncomingMidi on the
    // audio thread — RT-safe (atomics only). Optional (null in stubs).
    std::function<void(int, int, float)> onMidiCc;

    // Optional standalone per-device MIDI routing. Null in tests and hosted
    // plugin builds, where modules use the host/default aggregate route.
    MidiRouteManager* midiRouteManager = nullptr;
    HostMidiInputSourceProvider* midiInputSourceProvider = nullptr;
};

} // namespace curlop
