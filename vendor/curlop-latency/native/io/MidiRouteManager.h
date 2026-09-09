#pragma once

#include "control/vm/core/VMRunnerHost.h"
#include "shell/CurlopDebug.h"
#include "shell/WireJson.h"

#include "choc/containers/choc_SingleReaderSingleWriterFIFO.h"

#include <juce_audio_devices/juce_audio_devices.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <vector>

class CurlopProcessorAuthorityTests;
struct GraphBenchMidiAllocationProbe;

namespace curlop {

class MidiRouteManager final : private juce::MidiInputCallback {
    friend class ::CurlopProcessorAuthorityTests;
    friend struct ::GraphBenchMidiAllocationProbe;

public:
    using InputSource = HostMidiInputSource;

    MidiRouteManager()
    {
        outputQueue_.reset(4096);
    }

    ~MidiRouteManager() override = default;

    void syncWithDeviceManager(juce::AudioDeviceManager& deviceManager)
    {
        auto inputs = juce::MidiInput::getAvailableDevices();
        auto outputs = juce::MidiOutput::getAvailableDevices();
        std::vector<std::string> inputIds;
        std::vector<std::string> outputIds;
        inputIds.reserve((size_t) inputs.size());
        outputIds.reserve((size_t) outputs.size());
        for (const auto& info : inputs) inputIds.push_back(info.identifier.toStdString());
        for (const auto& info : outputs) outputIds.push_back(info.identifier.toStdString());
        const bool listChanged = table_.load(std::memory_order_relaxed) == nullptr
                              || inputIds != lastInputIds_
                              || outputIds != lastOutputIds_;

        for (auto& state : states_) {
            state->inputPresent.store(false, std::memory_order_relaxed);
            state->outputPresent.store(false, std::memory_order_relaxed);
        }

        for (const auto& info : inputs) {
            auto* state = ensureState(info, true);
            if (registeredInputIds_.insert(info.identifier).second) {
                deviceManager.setMidiInputDeviceEnabled(info.identifier, true);
                deviceManager.addMidiInputDeviceCallback(info.identifier, this);
                CDBG(MIDI_ROUTE, "registered MIDI input route id=%s name=%s",
                     info.identifier.toRawUTF8(), info.name.toRawUTF8());
            }
            state->inputPresent.store(true, std::memory_order_relaxed);
        }

        for (const auto& info : outputs) {
            auto* state = ensureState(info, false);
            state->outputPresent.store(true, std::memory_order_relaxed);
            if (state->outputPtr.load(std::memory_order_acquire) == nullptr) {
                state->output = juce::MidiOutput::openDevice(info.identifier);
                state->outputPtr.store(state->output.get(), std::memory_order_release);
                CDBG(MIDI_ROUTE, "opened MIDI output route id=%s name=%s ok=%s",
                     info.identifier.toRawUTF8(), info.name.toRawUTF8(),
                     state->outputPtr.load(std::memory_order_relaxed) != nullptr ? "yes" : "no");
            }
        }

        if (listChanged) {
            lastInputIds_ = std::move(inputIds);
            lastOutputIds_ = std::move(outputIds);
            publishTable(inputs, outputs);
        }
    }

    InputSource inputSourceForRoute(const std::string& routeId,
                                    InputSource hostDefault) const noexcept
    {
        if (routeId.empty())
            return hostDefault;
        if (auto* state = findInput(routeId)) {
            return InputSource { &state->notes[0], &state->ccs[0][0],
                                 kHostMidiMaxChannels, kHostMidiMaxCCs,
                                 &state->preparedBlock };
        }
        missingInputLookups_.fetch_add(1, std::memory_order_relaxed);
        return InputSource {};
    }

    void prepareInputBlocks(
        int numSamples,
        double sampleRate,
        double blockEndSeconds =
            std::numeric_limits<double>::quiet_NaN()) noexcept
    {
        if (! std::isfinite(blockEndSeconds))
            blockEndSeconds =
                juce::Time::getMillisecondCounterHiRes() * 0.001;
        const auto* table = table_.load(std::memory_order_acquire);
        if (table == nullptr)
            return;
        for (auto* state : table->inputs)
            prepareInputBlock(
                *state, numSamples, sampleRate, blockEndSeconds);
    }

    bool enqueueOutput(const std::string& routeId,
                       const juce::MidiMessage& message,
                       int sampleOffset) noexcept
    {
        if (routeId.empty())
            return false;
        auto* state = findOutput(routeId);
        auto* output = state != nullptr
            ? state->outputPtr.load(std::memory_order_acquire)
            : nullptr;
        if (output == nullptr) {
            missingOutputLookups_.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        OutputEvent ev;
        ev.output = output;
        ev.message = message;
        ev.sampleOffset = sampleOffset;
        if (!outputQueue_.push(std::move(ev))) {
            outputDrops_.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        return true;
    }

    void drainOutputQueue()
    {
        OutputEvent ev;
        int sent = 0;
        while (outputQueue_.pop(ev)) {
            if (outputSendObserver_) {
                outputSendObserver_(ev.message);
                ++sent;
            } else if (ev.output != nullptr) {
                ev.output->sendMessageNow(ev.message);
                ++sent;
            }
        }
        if (sent > 0)
            CDBG(MIDI_ROUTE, "drained hardware MIDI output messages=%d", sent);
    }

    size_t clearHardwareOutputBacklog() noexcept
    {
        size_t cleared = 0;
        OutputEvent ev;
        while (outputQueue_.pop(ev))
            ++cleared;
        return cleared;
    }

    bool consumeDirty() noexcept
    {
        return dirty_.exchange(false, std::memory_order_acq_rel);
    }

    juce::String buildStateJson() const
    {
        juce::Array<juce::var> inputs;
        juce::Array<juce::var> outputs;
        const auto* table = table_.load(std::memory_order_acquire);
        if (table != nullptr) {
            for (const auto* state : table->inputs) {
                inputs.add(curlop::wire::makeMidiRouteDeviceVar(
                    state->identifier,
                    state->name,
                    state->inputPresent.load(std::memory_order_relaxed)));
            }
            for (const auto* state : table->outputs) {
                outputs.add(curlop::wire::makeMidiRouteDeviceVar(
                    state->identifier,
                    state->name,
                    state->outputPresent.load(std::memory_order_relaxed)));
            }
        }

        return curlop::wire::makeMidiRouteStateJson(
            inputs,
            outputs,
            outputDrops_.load(std::memory_order_relaxed),
            missingInputLookups_.load(std::memory_order_relaxed),
            missingOutputLookups_.load(std::memory_order_relaxed),
            inputDrops_.load(std::memory_order_relaxed),
            inputHighWater_.load(std::memory_order_relaxed));
    }

private:
    struct TimestampedInputEvent {
        HostMidiBlockEvent event;
        double timestampSeconds = 0.0;
    };

    struct RouteState {
        juce::String identifier;
        std::string identifierStd;
        juce::String name;
        MidiNoteStateArray notes;
        MidiCCMatrix ccs;
        std::unique_ptr<juce::MidiOutput> output;
        std::atomic<juce::MidiOutput*> outputPtr { nullptr };
        std::atomic<bool> inputPresent { false };
        std::atomic<bool> outputPresent { false };
        choc::fifo::SingleReaderSingleWriterFIFO<TimestampedInputEvent>
            inputQueue;
        HostMidiBlockState preparedBlock;
        HostMidiNoteSnapshot renderNotes[kHostMidiMaxChannels];
        float renderCCs[kHostMidiMaxChannels][kHostMidiMaxCCs] {};
        std::atomic<bool> inputOverflowed { false };
        std::atomic<bool> inputEpochResetRequested { false };
        std::atomic<uint32_t> inputDrops { 0 };
        double timestampOffsetSeconds =
            std::numeric_limits<double>::quiet_NaN();
        double lastMappedTimestampSeconds = 0.0;
    };

    struct RouteTable {
        std::vector<RouteState*> inputs;
        std::vector<RouteState*> outputs;
    };

    struct OutputEvent {
        juce::MidiOutput* output = nullptr;
        juce::MidiMessage message;
        int sampleOffset = 0;
    };

    RouteState* ensureState(const juce::MidiDeviceInfo& info, bool input)
    {
        for (auto& state : states_) {
            if (state->identifier == info.identifier) {
                state->name = info.name;
                return state.get();
            }
        }
        auto state = std::make_unique<RouteState>();
        state->identifier = info.identifier;
        state->identifierStd = info.identifier.toStdString();
        state->name = info.name;
        state->inputQueue.reset(kHostMidiBlockMaxEvents);
        for (int ch = 0; ch < kHostMidiMaxChannels; ++ch) {
            state->notes[ch].gate.store(0.0f, std::memory_order_relaxed);
            state->notes[ch].pitch.store(0.0f, std::memory_order_relaxed);
            state->notes[ch].velocity.store(0.0f, std::memory_order_relaxed);
            state->notes[ch].pitchBendSemis.store(0.0f, std::memory_order_relaxed);
            state->notes[ch].note.store(-1, std::memory_order_relaxed);
            state->renderNotes[ch].note = -1;
            for (int cc = 0; cc < kHostMidiMaxCCs; ++cc) {
                state->ccs[ch][cc].store(std::numeric_limits<float>::quiet_NaN(),
                                         std::memory_order_relaxed);
                state->renderCCs[ch][cc] =
                    std::numeric_limits<float>::quiet_NaN();
            }
        }
        state->inputPresent.store(input, std::memory_order_relaxed);
        auto* ptr = state.get();
        states_.push_back(std::move(state));
        return ptr;
    }

    void publishTable(const juce::Array<juce::MidiDeviceInfo>& inputs,
                      const juce::Array<juce::MidiDeviceInfo>& outputs)
    {
        const auto* previous =
            table_.load(std::memory_order_acquire);
        auto table = std::make_unique<RouteTable>();
        for (const auto& info : inputs)
            if (auto* state = findState(info.identifier.toStdString()))
                table->inputs.push_back(state);
        for (const auto& info : outputs)
            if (auto* state = findState(info.identifier.toStdString()))
                table->outputs.push_back(state);

        for (auto& owned : states_) {
            auto* state = owned.get();
            const bool wasInput =
                previous != nullptr
                && std::find(
                       previous->inputs.begin(),
                       previous->inputs.end(),
                       state)
                    != previous->inputs.end();
            const bool isInput =
                std::find(
                    table->inputs.begin(),
                    table->inputs.end(),
                    state)
                != table->inputs.end();
            if (previous != nullptr && wasInput != isInput)
                state->inputEpochResetRequested.store(
                    true, std::memory_order_release);
            state->inputPresent.store(
                isInput, std::memory_order_release);
            state->outputPresent.store(
                std::find(
                    table->outputs.begin(),
                    table->outputs.end(),
                    state)
                    != table->outputs.end(),
                std::memory_order_release);
        }

        tables_.push_back(std::move(table));
        table_.store(tables_.back().get(), std::memory_order_release);
        dirty_.store(true, std::memory_order_release);
    }

    RouteState* findState(const std::string& id) const noexcept
    {
        for (const auto& state : states_)
            if (state->identifierStd == id)
                return state.get();
        return nullptr;
    }

    RouteState* findInput(const std::string& id) const noexcept
    {
        const auto* table = table_.load(std::memory_order_acquire);
        if (table == nullptr) return nullptr;
        for (auto* state : table->inputs)
            if (state->identifierStd == id)
                return state;
        return nullptr;
    }

    RouteState* findInput(const juce::String& id) const noexcept
    {
        const auto* table = table_.load(std::memory_order_acquire);
        if (table == nullptr) return nullptr;
        for (auto* state : table->inputs)
            if (state->identifier == id)
                return state;
        return nullptr;
    }

    RouteState* findOutput(const std::string& id) const noexcept
    {
        const auto* table = table_.load(std::memory_order_acquire);
        if (table == nullptr) return nullptr;
        for (auto* state : table->outputs)
            if (state->identifierStd == id)
                return state;
        return nullptr;
    }

    void handleIncomingMidiMessage(juce::MidiInput* source,
                                   const juce::MidiMessage& message) override
    {
        if (source == nullptr) return;
        auto* state = findInput(source->getIdentifier());
        if (state == nullptr) return;
        applyIncoming(*state, message);
    }

    void applyIncoming(RouteState& state, const juce::MidiMessage& msg) noexcept
    {
        constexpr float kPitchBendRangeSemis = 2.0f;
        HostMidiBlockEvent event;
        bool hasEvent = false;
        if (msg.isController()) {
            const int ch = msg.getChannel() - 1;
            const int cc = msg.getControllerNumber();
            if (ch >= 0 && ch < kHostMidiMaxChannels
                && cc >= 0 && cc < kHostMidiMaxCCs) {
                state.ccs[ch][cc].store(msg.getControllerValue() / 127.0f,
                                        std::memory_order_relaxed);
                event = { 0, HostMidiBlockEventType::Controller,
                          static_cast<uint8_t>(ch),
                          static_cast<uint8_t>(cc),
                          msg.getControllerValue() };
                hasEvent = true;
            }
        } else if (msg.isNoteOn()) {
            const int ch = msg.getChannel() - 1;
            if (ch >= 0 && ch < kHostMidiMaxChannels) {
                auto& st = state.notes[ch];
                const float bend = st.pitchBendSemis.load(std::memory_order_relaxed);
                st.note.store(msg.getNoteNumber(), std::memory_order_relaxed);
                st.pitch.store((msg.getNoteNumber() - 60.0f + bend) / 120.0f,
                               std::memory_order_relaxed);
                st.velocity.store(msg.getVelocity() / 127.0f, std::memory_order_relaxed);
                st.gate.store(1.0f, std::memory_order_relaxed);
                event = { 0, HostMidiBlockEventType::NoteOn,
                          static_cast<uint8_t>(ch),
                          static_cast<uint8_t>(msg.getNoteNumber()),
                          static_cast<int>(msg.getVelocity()) };
                hasEvent = true;
            }
        } else if (msg.isNoteOff()) {
            const int ch = msg.getChannel() - 1;
            if (ch >= 0 && ch < kHostMidiMaxChannels) {
                auto& st = state.notes[ch];
                if (st.note.load(std::memory_order_relaxed) == msg.getNoteNumber()) {
                    st.gate.store(0.0f, std::memory_order_relaxed);
                    st.velocity.store(0.0f, std::memory_order_relaxed);
                }
                event = { 0, HostMidiBlockEventType::NoteOff,
                          static_cast<uint8_t>(ch),
                          static_cast<uint8_t>(msg.getNoteNumber()), 0 };
                hasEvent = true;
            }
        } else if (msg.isPitchWheel()) {
            const int ch = msg.getChannel() - 1;
            if (ch >= 0 && ch < kHostMidiMaxChannels) {
                auto& st = state.notes[ch];
                const float wheelNorm = ((float) msg.getPitchWheelValue() - 8192.0f) / 8192.0f;
                const float bend = std::clamp(wheelNorm * kPitchBendRangeSemis,
                                              -kPitchBendRangeSemis,
                                               kPitchBendRangeSemis);
                st.pitchBendSemis.store(bend, std::memory_order_relaxed);
                const int note = st.note.load(std::memory_order_relaxed);
                if (note >= 0)
                    st.pitch.store((note - 60.0f + bend) / 120.0f,
                                   std::memory_order_relaxed);
                event = { 0, HostMidiBlockEventType::PitchWheel,
                          static_cast<uint8_t>(ch), 0,
                          msg.getPitchWheelValue() };
                hasEvent = true;
            }
        }
        double timestamp = msg.getTimeStamp();
        if (! std::isfinite(timestamp) || timestamp <= 0.0)
            timestamp =
                juce::Time::getMillisecondCounterHiRes() * 0.001;
        if (hasEvent && ! state.inputQueue.push(
                TimestampedInputEvent { event, timestamp })) {
            state.inputDrops.fetch_add(1, std::memory_order_relaxed);
            inputDrops_.fetch_add(1, std::memory_order_relaxed);
            state.inputOverflowed.store(true, std::memory_order_release);
        } else if (hasEvent) {
            const auto used =
                static_cast<int>(state.inputQueue.getUsedSlots());
            auto high =
                inputHighWater_.load(std::memory_order_relaxed);
            while (used > high
                   && ! inputHighWater_.compare_exchange_weak(
                       high, used, std::memory_order_relaxed)) {}
        }
    }

    static void applyPreparedEvent(RouteState& state,
                                   const HostMidiBlockEvent& event) noexcept
    {
        if (event.type == HostMidiBlockEventType::AllNotesOff) {
            for (auto& note : state.renderNotes) {
                note.gate = 0.0f;
                note.velocity = 0.0f;
                note.note = -1;
            }
            return;
        }
        const int ch = event.channel;
        if (ch < 0 || ch >= kHostMidiMaxChannels)
            return;
        auto& note = state.renderNotes[ch];
        switch (event.type) {
            case HostMidiBlockEventType::Controller:
                state.renderCCs[ch][event.data1] =
                    std::clamp(static_cast<float>(event.value) / 127.0f,
                               0.0f, 1.0f);
                break;
            case HostMidiBlockEventType::NoteOn:
                note.note = event.data1;
                note.pitch =
                    (static_cast<float>(event.data1) - 60.0f
                     + note.pitchBendSemis) / 120.0f;
                note.velocity =
                    std::clamp(static_cast<float>(event.value) / 127.0f,
                               0.0f, 1.0f);
                note.gate = 1.0f;
                break;
            case HostMidiBlockEventType::NoteOff:
                if (note.note == event.data1) {
                    note.gate = 0.0f;
                    note.velocity = 0.0f;
                }
                break;
            case HostMidiBlockEventType::PitchWheel: {
                const float normalized =
                    (static_cast<float>(event.value) - 8192.0f) / 8192.0f;
                note.pitchBendSemis =
                    std::clamp(normalized * 2.0f, -2.0f, 2.0f);
                if (note.note >= 0)
                    note.pitch =
                        (static_cast<float>(note.note) - 60.0f
                         + note.pitchBendSemis) / 120.0f;
                break;
            }
            case HostMidiBlockEventType::AllNotesOff:
                break;
        }
    }

    static void prepareInputBlock(RouteState& state,
                                  int numSamples,
                                  double sampleRate,
                                  double blockEndSeconds) noexcept
    {
        if (state.inputEpochResetRequested.exchange(
                false, std::memory_order_acq_rel)) {
            TimestampedInputEvent discarded;
            while (state.inputQueue.pop(discarded)) {}
            state.preparedBlock = {};
            for (int channel = 0;
                 channel < kHostMidiMaxChannels;
                 ++channel) {
                state.renderNotes[channel] = {};
                state.renderNotes[channel].note = -1;
                state.notes[channel].gate.store(
                    0.0f, std::memory_order_relaxed);
                state.notes[channel].pitch.store(
                    0.0f, std::memory_order_relaxed);
                state.notes[channel].velocity.store(
                    0.0f, std::memory_order_relaxed);
                state.notes[channel].pitchBendSemis.store(
                    0.0f, std::memory_order_relaxed);
                state.notes[channel].note.store(
                    -1, std::memory_order_relaxed);
                for (int cc = 0; cc < kHostMidiMaxCCs; ++cc) {
                    state.renderCCs[channel][cc] =
                        std::numeric_limits<float>::quiet_NaN();
                    state.ccs[channel][cc].store(
                        std::numeric_limits<float>::quiet_NaN(),
                        std::memory_order_relaxed);
                }
            }
            state.timestampOffsetSeconds =
                std::numeric_limits<double>::quiet_NaN();
            state.lastMappedTimestampSeconds = 0.0;
            state.inputOverflowed.store(
                false, std::memory_order_release);
            state.inputDrops.store(
                0, std::memory_order_release);
            state.preparedBlock.beginBlock(
                numSamples, state.renderNotes, state.renderCCs);
            state.preparedBlock.push(
                HostMidiBlockEventType::AllNotesOff,
                0, 0, 0, 0);
            return;
        }
        state.preparedBlock.beginBlock(
            numSamples, state.renderNotes, state.renderCCs);
        TimestampedInputEvent queued;
        if (state.inputOverflowed.exchange(
                false, std::memory_order_acq_rel)) {
            while (state.inputQueue.pop(queued)) {}
            const auto drops =
                state.inputDrops.exchange(0, std::memory_order_acq_rel);
            state.preparedBlock.push(
                HostMidiBlockEventType::AllNotesOff, 0, 0, 0, 0);
            state.preparedBlock.droppedEvents += drops;
            applyPreparedEvent(
                state,
                { 0, HostMidiBlockEventType::AllNotesOff, 0, 0, 0 });
            return;
        }
        const double safeRate =
            std::isfinite(sampleRate) && sampleRate > 0.0
                ? sampleRate : 48000.0;
        const double blockStartSeconds =
            blockEndSeconds
            - static_cast<double>(std::max(0, numSamples))
                / safeRate;
        int lastSample = 0;
        while (state.inputQueue.pop(queued)) {
            if (! std::isfinite(state.timestampOffsetSeconds)) {
                state.timestampOffsetSeconds =
                    std::abs(
                        queued.timestampSeconds - blockEndSeconds)
                            > 10.0
                        ? blockEndSeconds - queued.timestampSeconds
                        : 0.0;
            }
            double mapped =
                queued.timestampSeconds
                + state.timestampOffsetSeconds;
            mapped = std::max(
                mapped, state.lastMappedTimestampSeconds);
            state.lastMappedTimestampSeconds = mapped;
            const int sample =
                std::clamp(
                    static_cast<int>(std::llround(
                        (mapped - blockStartSeconds) * safeRate)),
                    0, std::max(0, numSamples - 1));
            lastSample = std::max(lastSample, sample);
            state.preparedBlock.push(
                queued.event.type, lastSample,
                queued.event.channel, queued.event.data1,
                queued.event.value);
            applyPreparedEvent(state, queued.event);
        }
        state.preparedBlock.droppedEvents +=
            state.inputDrops.exchange(0, std::memory_order_acq_rel);
    }

    std::vector<std::unique_ptr<RouteState>> states_;
    std::vector<std::unique_ptr<RouteTable>> tables_;
    std::set<juce::String> registeredInputIds_;
    std::atomic<RouteTable*> table_ { nullptr };
    choc::fifo::SingleReaderSingleWriterFIFO<OutputEvent> outputQueue_;
    std::function<void(const juce::MidiMessage&)> outputSendObserver_;
    std::atomic<bool> dirty_ { true };
    mutable std::atomic<int> missingInputLookups_ { 0 };
    mutable std::atomic<int> missingOutputLookups_ { 0 };
    mutable std::atomic<int> outputDrops_ { 0 };
    std::atomic<int> inputDrops_ { 0 };
    std::atomic<int> inputHighWater_ { 0 };
    std::vector<std::string> lastInputIds_;
    std::vector<std::string> lastOutputIds_;
};

} // namespace curlop
