// CURLOP Audio Recorder
//
// Captures processBlock output to a WAV file using JUCE's
// AudioFormatWriter::ThreadedWriter for lock-free audio-thread writes.
//
// Usage:
//   1. prepare(sampleRate, channels)  — called from prepareToPlay
//   2. arm()                          — WebView sends RECORDER_ARM
//   3. startRecording(file)           — WebView sends RECORDER_START
//   4. writeBlock(buffer, numSamples) — called every processBlock while recording
//   5. stopRecording()                — WebView sends RECORDER_STOP
//      OR beginTailHold()             — keeps recording until audio < -60dB
//      OR beginLoopCut()              — keeps recording until next loop boundary
//
// Tail hold: after transport stops, the recorder keeps capturing audio
// until the peak level drops below the silence threshold (-60dB ≈ 0.001)
// for a sustained period (consecutive silent blocks). No time cap (B-320) —
// the tail runs as long as the audio is still sounding, so a long reverb/
// release is never cut short.
//
// Loop cut: recorder keeps recording while transport plays, then hard-stops
// at the exact loop boundary for a perfect-loop capture. CurlopProcessor
// calls notifyLoopWrap() at each loop boundary; the recorder auto-stops
// on the first wrap after beginLoopCut() is called.
//
// The ThreadedWriter handles all thread safety: audio thread writes into a
// FIFO, background TimeSliceThread drains to disk. No locks on the audio thread.
#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

#include "io/RecordProfile.h"
#include "shell/CurlopDebug.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <vector>

namespace curlop {

class AudioRecorder {
public:
    // ── T-265 (Phase D-2): typed listener replacing dead RECORDER_STATE wire ──
    // Wire emit was deleted s411 (8df7cefa). TransportBarComponent's record
    // button used to poll isRecording() / isArmed() at 30Hz; this listener
    // makes the visuals event-driven. All four state-changing entry points
    // (arm / disarm / startRecording / stopRecording) are message-thread
    // per existing comments, so listener callbacks always fire on the
    // message thread. Audio-thread auto-stops (notifyLoopWrap +
    // writeBlock tail-hold) route through VMRunner's audio-message request
    // drain before stopRecording(), so the listener fires inside that message-thread
    // call. Cites T-264 audit Debt-1, Phase D.
    class Listener {
    public:
        virtual ~Listener() = default;
        virtual void recordingStarted() {}
        virtual void recordingStopped() {}
        virtual void armedChanged(bool armed) { juce::ignoreUnused(armed); }
    };

    void addListener(Listener* l)    { listeners_.add(l); }
    void removeListener(Listener* l) { listeners_.remove(l); }

    AudioRecorder()
        : writerThread_(std::make_unique<juce::TimeSliceThread>("CURLOP Recorder"))
    {}

    ~AudioRecorder()
    {
        stopRecording();
        if (writerThread_->isThreadRunning())
            writerThread_->stopThread(-1);
    }

    // Called from prepareToPlay — stores format info for later use.
    // maxBlockSize sizes the multitrack staging buffer (F-070) so it never
    // allocates on the audio thread.
    void prepare(double sampleRate, int channels, int maxBlockSize)
    {
        sampleRate_ = sampleRate;
        numChannels_ = channels;
        maxBlockSize_ = juce::jmax(1, maxBlockSize);
    }

    // Arm the recorder. Next startRecording() call will begin capture.
    void arm()
    {
        armed_.store(true, std::memory_order_release);
        DBG("CURLOP Recorder: Armed");
        listeners_.call([](Listener& l) { l.armedChanged(true); });
    }

    // Disarm without recording.
    void disarm()
    {
        armed_.store(false, std::memory_order_release);
        listeners_.call([](Listener& l) { l.armedChanged(false); });
    }

    // Begin recording to the given file. Must be armed first.
    // Called from the message thread.
    //
    // F-070: numChannels is the total width of the multitrack file (master +
    // every module iso track); moduleTaps lists the module taps VMRunner fills
    // into the staging buffer each block (master always occupies channels 0..1).
    // Master-only recording passes numChannels=2 + an empty moduleTaps.
    //
    // SF-072 (§5.1, "bake in the data"): timeReferenceSamples is the bext
    // `TimeReference` written into EVERY file in EVERY mode — the sample offset
    // of this file's first sample from the RecordSession origin (t=0). For a
    // single-clip / session-start capture it is 0; for a later per-clip stem
    // fragment it is the samples elapsed since the session began, so an NLE
    // auto-aligns the fragment to the master timeline with zero manual work.
    bool startRecording(const juce::File& outputFile,
                        int numChannels,
                        std::vector<RecordTap> moduleTaps,
                        int64_t timeReferenceSamples = 0)
    {
        if (!armed_.load(std::memory_order_acquire))
        {
            DBG("CURLOP Recorder: Cannot start — not armed");
            return false;
        }

        stopRecording(); // Clean up any previous session

        // F-070: the multitrack file width + the per-module fill plan. Set
        // before the writer is built (createWriterFor reads numChannels_).
        numChannels_ = juce::jmax(1, numChannels);
        moduleTaps_  = std::move(moduleTaps);
        // Staging buffer the audio thread interleaves into. Sized here (message
        // thread) while recording_ is false, so the audio thread never touches
        // it mid-resize. No audio-thread allocation.
        staging_.setSize(numChannels_, maxBlockSize_, false, false, true);
        staging_.clear();

        // Create WAV writer using modern JUCE API.
        // createWriterFor takes unique_ptr<OutputStream>& — transfers ownership on success.
        juce::WavAudioFormat wavFormat;
        std::unique_ptr<juce::OutputStream> outputStream = outputFile.createOutputStream();
        if (outputStream == nullptr)
        {
            DBG("CURLOP Recorder: Failed to create output stream for " +
                outputFile.getFullPathName());
            return false;
        }

        // T-197: pre-allocate kMaxMarkers cue entries. JUCE WavAudioFormatWriter
        // bakes the cue chunk at construction (no public setMetadataValues),
        // so we reserve fixed slots up-front and patch their `Offset` field
        // in-place after threadedWriter_.reset() finalizes the file.
        // Labels are fixed-width "install_XX" so the LIST/adtl/labl chunk
        // is byte-stable and we never have to rewrite chunk lengths.
        std::unordered_map<juce::String, juce::String> meta;
        meta["NumCuePoints"] = juce::String(kMaxMarkers);
        meta["NumCueLabels"] = juce::String(kMaxMarkers);
        for (int i = 0; i < kMaxMarkers; ++i)
        {
            const int id = i + 1;
            const auto cuePrefix   = "Cue"      + juce::String(i);
            const auto labelPrefix = "CueLabel" + juce::String(i);
            meta[cuePrefix + "Identifier"] = juce::String(id);
            meta[cuePrefix + "Order"]      = juce::String(i);
            meta[cuePrefix + "Offset"]     = "0"; // patched post-close
            meta[labelPrefix + "Identifier"] = juce::String(id);
            // T-197b: pre-allocate fixed-width kMaxLabel-1 chars so each
            // labl chunk has identical byte length. patchCueOffsets() then
            // overwrites the text region in-place with the actual script
            // source per fired marker. Pad with NULs (stops most viewers
            // at the script terminator).
            char lbl[kMaxLabel];
            std::memset(lbl, 0, sizeof(lbl));
            std::snprintf(lbl, sizeof(lbl), "install_%02d", i);
            // Force the placeholder to occupy the full kMaxLabel-1 byte
            // region so labl chunk size is stable. We pad with 'x' (visible
            // char) so JUCE's UTF-8-byte-count uses the full width;
            // patch-time we'll overwrite from byte 0 with the real script.
            const size_t prefixLen = std::strlen(lbl);
            for (size_t k = prefixLen; k < kMaxLabel - 1; ++k) lbl[k] = 'x';
            lbl[kMaxLabel - 1] = '\0';
            meta[labelPrefix + "Text"] = juce::String::fromUTF8(lbl, kMaxLabel - 1);
        }

        // SF-072 §5.1 — bake in the timecode on every file, every mode. The bext
        // chunk is emitted by JUCE whenever any bwav* key is present; we always
        // set TimeReference (sample offset from session t=0, 0 for a session
        // start) plus the originator + origination date/time so the file is a
        // self-describing BWF take an NLE reads directly.
        const auto now = juce::Time::getCurrentTime();
        meta[juce::WavAudioFormat::bwavTimeReference]  = juce::String(timeReferenceSamples);
        meta[juce::WavAudioFormat::bwavOriginator]     = "CURLOP";
        meta[juce::WavAudioFormat::bwavOriginationDate] = now.formatted("%Y-%m-%d");
        meta[juce::WavAudioFormat::bwavOriginationTime] = now.formatted("%H:%M:%S");

        auto options = juce::AudioFormatWriterOptions()
            .withSampleRate(sampleRate_)
            .withNumChannels(numChannels_)
            .withBitsPerSample(24)
            .withMetadataValues(meta);

        auto writer = wavFormat.createWriterFor(outputStream, options);
        if (writer == nullptr)
        {
            DBG("CURLOP Recorder: Failed to create WAV writer");
            return false;
        }

        installThreadedWriter(std::move(writer));
        samplesWritten_.store(0, std::memory_order_release);
        acceptedFrames_.store(0, std::memory_order_release);
        acceptedSamples_.store(0, std::memory_order_release);
        rejectedFrames_.store(0, std::memory_order_release);
        rejectedSamples_.store(0, std::memory_order_release);
        markerHead_.store(0, std::memory_order_release);

        recording_.store(true, std::memory_order_release);
        tailHolding_.store(false, std::memory_order_release);
        loopCutPending_.store(false, std::memory_order_release);
        armed_.store(false, std::memory_order_release);
        consecutiveSilentBlocks_.store(0, std::memory_order_release);

        currentFile_ = outputFile;
        DBG("CURLOP Recorder: Recording started -> " + outputFile.getFullPathName());

        // T-265 (Phase D-2): startRecording flips armed → false on success;
        // fire both transitions so the GUI reflects "recording, not armed".
        listeners_.call([](Listener& l) { l.armedChanged(false); });
        listeners_.call([](Listener& l) { l.recordingStarted(); });
        return true;
    }

    // Enter tail hold mode: keep recording until audio drops below -60dB.
    // Called from message thread when transport stops while recording.
    void beginTailHold()
    {
        if (!recording_.load(std::memory_order_acquire))
            return;

        tailHolding_.store(true, std::memory_order_release);
        consecutiveSilentBlocks_.store(0, std::memory_order_release);
        DBG("CURLOP Recorder: Tail hold started (no time cap, threshold " +
            juce::String(20.0f * std::log10(kSilenceThreshold), 1) + "dB)");
    }

    // Enter loop cut mode: keep recording until the next loop boundary.
    // Called from message thread when record button is pressed during recording.
    void beginLoopCut()
    {
        if (!recording_.load(std::memory_order_acquire))
            return;

        loopCutPending_.store(true, std::memory_order_release);
        DBG("CURLOP Recorder: Loop cut armed — will stop at next loop boundary");
    }

    // Called from processBlock when a loop boundary is crossed.
    // Returns true if loop cut was pending (caller should finalize).
    bool notifyLoopWrap()
    {
        if (loopCutPending_.load(std::memory_order_acquire) &&
            recording_.load(std::memory_order_acquire))
        {
            CDBG_RT(RECORDER, "Loop cut: stopping at loop boundary");
            recording_.store(false, std::memory_order_release);
            loopCutPending_.store(false, std::memory_order_release);
            return true;
        }
        return false;
    }

    // Stop recording and flush to disk. Safe to call from any thread, but
    // see the listener fan-out below — listener fires on the calling thread,
    // so non-message-thread callers (e.g. ~AudioRecorder destructor at app
    // shutdown when no listeners exist) are fine, while audio-thread
    // auto-stops MUST route through VMRunner's audio-message request drain.
    void stopRecording()
    {
        const bool wasRecording =
            recording_.exchange(false, std::memory_order_acq_rel);
        tailHolding_.store(false, std::memory_order_release);
        loopCutPending_.store(false, std::memory_order_release);
        consecutiveSilentBlocks_.store(0, std::memory_order_release);
        writerForAudio_.store(nullptr, std::memory_order_release);

        while (activeAudioWrites_.load(std::memory_order_acquire) != 0)
            juce::Thread::yield();

        // ThreadedWriter destructor flushes remaining data and deletes the writer
        threadedWriter_.reset();

        // T-197: now that the WAV is finalized on disk, patch the pre-allocated
        // cue chunk's Offset fields in-place. Labels stay as the fixed-width
        // "install_XX" placeholders so audacity/sox/etc. show them at the
        // correct sample positions. Unfired slots remain at offset=0.
        patchCueOffsets();

        if (currentFile_.existsAsFile())
        {
            DBG("CURLOP Recorder: Stopped. File size: " +
                juce::String(currentFile_.getSize()) + " bytes");
        }

        // T-265 (Phase D-2): only fire if we were actually recording — keeps
        // listener callbacks idempotent across redundant stopRecording calls
        // (e.g. ~AudioRecorder calls stopRecording even from idle).
        if (wasRecording)
            listeners_.call([](Listener& l) { l.recordingStopped(); });
    }

    // ── F-070 multitrack frame assembly (all on the audio thread) ────────────
    // VMRunner fills the staging buffer each block: master into channels 0..1,
    // each module tap into its channel range (per moduleTaps()), then calls
    // writeStaged(). The staging buffer is sized at startRecording (message
    // thread, while recording_ is false), so there is no audio-thread alloc and
    // no cross-thread resize race. Master-only recording fills just 0..1.
    juce::AudioBuffer<float>& stagingBuffer() noexcept { return staging_; }
    const std::vector<RecordTap>& moduleTaps() const noexcept { return moduleTaps_; }

    // Audio thread: write the assembled multitrack frame (staging_, numSamples)
    // to disk. No allocation, no locks. Returns true if tail hold auto-stopped.
    class AudioFrameScope {
    public:
        AudioFrameScope() = default;
        AudioFrameScope(const AudioFrameScope&) = delete;
        AudioFrameScope& operator=(const AudioFrameScope&) = delete;

        AudioFrameScope(AudioFrameScope&& other) noexcept
            : recorder_(other.recorder_),
              recordingAtStart_(other.recordingAtStart_)
        {
            other.recorder_ = nullptr;
            other.recordingAtStart_ = false;
        }

        AudioFrameScope& operator=(AudioFrameScope&& other) noexcept
        {
            if (this != &other)
            {
                release();
                recorder_ = other.recorder_;
                recordingAtStart_ = other.recordingAtStart_;
                other.recorder_ = nullptr;
                other.recordingAtStart_ = false;
            }
            return *this;
        }

        ~AudioFrameScope() { release(); }

        bool isRecording() const noexcept { return recordingAtStart_; }

    private:
        friend class AudioRecorder;

        explicit AudioFrameScope(AudioRecorder& recorder) noexcept
            : recorder_(&recorder)
        {
            recorder_->activeAudioWrites_.fetch_add(1, std::memory_order_acq_rel);
            recordingAtStart_ = recorder_->recording_.load(std::memory_order_acquire);
        }

        void release() noexcept
        {
            if (recorder_ != nullptr)
            {
                recorder_->activeAudioWrites_.fetch_sub(1, std::memory_order_acq_rel);
                recorder_ = nullptr;
            }
        }

        AudioRecorder* recorder_ = nullptr;
        bool recordingAtStart_ = false;
    };

    AudioFrameScope beginAudioFrame() noexcept { return AudioFrameScope(*this); }

    bool writeStaged(int numSamples)
    {
        auto frame = beginAudioFrame();
        return writeStaged(numSamples, frame);
    }

    bool writeStaged(int numSamples, const AudioFrameScope& frame)
    {
        if (!frame.isRecording())
            return false;

        if (auto* writer = writerForAudio_.load(std::memory_order_acquire))
        {
            if (writer->write(staging_.getArrayOfReadPointers(), numSamples)) {
                samplesWritten_.fetch_add((int64_t) numSamples, std::memory_order_release);
                acceptedFrames_.fetch_add(1, std::memory_order_release);
                acceptedSamples_.fetch_add((int64_t) numSamples, std::memory_order_release);
            } else {
                rejectedFrames_.fetch_add(1, std::memory_order_release);
                rejectedSamples_.fetch_add((int64_t) numSamples, std::memory_order_release);
            }
        }

        // Tail hold is measured on the MASTER channels (0..1) — the master bus
        // is the silence reference, not the iso stems.
        if (tailHolding_.load(std::memory_order_acquire))
        {
            float peak = 0.0f;
            const int mch = juce::jmin(2, staging_.getNumChannels());
            for (int ch = 0; ch < mch; ++ch)
            {
                const float* data = staging_.getReadPointer(ch);
                for (int i = 0; i < numSamples; ++i)
                {
                    float a = std::fabs(data[i]);
                    if (a > peak) peak = a;
                }
            }

            if (peak < kSilenceThreshold)
                consecutiveSilentBlocks_.fetch_add(1, std::memory_order_acq_rel);
            else
                consecutiveSilentBlocks_.store(0, std::memory_order_release);

            // Stop once silence is sustained. No time cap (Neo, B-320): the
            // tail runs as long as the audio is still sounding, so a long
            // reverb/release is never cut short — it stops only when the audio
            // actually decays below the silence threshold.
            if (consecutiveSilentBlocks_.load(std::memory_order_acquire) >= kRequiredSilentBlocks)
            {
                CDBG_RT(RECORDER, "Tail hold complete: silence detected");
                recording_.store(false, std::memory_order_release);
                tailHolding_.store(false, std::memory_order_release);
                return true; // Signal caller to finalize
            }
        }

        return false;
    }

    // T-197: stamp a WAV cue marker at the current recording position.
    // Audio-thread safe: no allocation, no locks. Drops silently if not
    // recording or if the marker ring is full.
    void markCue(const char* label) noexcept
    {
        if (!recording_.load(std::memory_order_acquire))
            return;
        const int idx = markerHead_.load(std::memory_order_acquire);
        if (idx >= kMaxMarkers)
            return;
        Marker& m = markers_[idx];
        m.sample = samplesWritten_.load(std::memory_order_acquire);
        if (label != nullptr)
        {
            std::strncpy(m.label, label, kMaxLabel - 1);
            m.label[kMaxLabel - 1] = '\0';
        }
        else
        {
            m.label[0] = '\0';
        }
        markerHead_.store(idx + 1, std::memory_order_release);
    }

    int64_t getSamplesWritten() const noexcept
    {
        return samplesWritten_.load(std::memory_order_acquire);
    }

    uint64_t getAcceptedFrames() const noexcept
    {
        return acceptedFrames_.load(std::memory_order_acquire);
    }

    int64_t getAcceptedSamples() const noexcept
    {
        return acceptedSamples_.load(std::memory_order_acquire);
    }

    uint64_t getRejectedFrames() const noexcept
    {
        return rejectedFrames_.load(std::memory_order_acquire);
    }

    int64_t getRejectedSamples() const noexcept
    {
        return rejectedSamples_.load(std::memory_order_acquire);
    }

    bool isRecording() const { return recording_.load(std::memory_order_acquire); }
    bool isArmed() const { return armed_.load(std::memory_order_acquire); }
    bool isTailHolding() const { return tailHolding_.load(std::memory_order_acquire); }
    bool isLoopCutPending() const { return loopCutPending_.load(std::memory_order_acquire); }

    const juce::File& getCurrentFile() const { return currentFile_; }

private:
    void installThreadedWriter(std::unique_ptr<juce::AudioFormatWriter> writer)
    {
        // Most processor instances never record. Start the disk worker only
        // once a valid writer exists, avoiding idle thread churn and teardown
        // races across short-lived processor instances and test probes.
        if (! writerThread_->isThreadRunning())
            writerThread_->startThread();

        // ThreadedWriter takes ownership of the writer (raw pointer).
        // FIFO size: ~10 seconds of audio buffer.
        const int fifoSamples = static_cast<int>(sampleRate_ * 10.0);
        threadedWriter_ = std::make_unique<juce::AudioFormatWriter::ThreadedWriter>(
            writer.release(), *writerThread_, fifoSamples);
        writerForAudio_.store(threadedWriter_.get(), std::memory_order_release);
    }

    std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> threadedWriter_;
    std::atomic<juce::AudioFormatWriter::ThreadedWriter*> writerForAudio_{nullptr};
    std::atomic<int> activeAudioWrites_{0};
    std::unique_ptr<juce::TimeSliceThread> writerThread_;
    juce::ListenerList<Listener> listeners_;

    std::atomic<bool> recording_{false};
    std::atomic<bool> armed_{false};
    std::atomic<bool> tailHolding_{false};
    std::atomic<bool> loopCutPending_{false};

    double sampleRate_ = 44100.0;
    int numChannels_ = 2;
    int maxBlockSize_ = 512;
    juce::File currentFile_;

    // F-070 multitrack: the staging frame the audio thread interleaves into,
    // and the per-module fill plan (channel offsets) VMRunner follows. Sized at
    // startRecording; written by writeStaged().
    juce::AudioBuffer<float> staging_;
    std::vector<RecordTap>   moduleTaps_;

    // Tail hold state (audio thread only after beginTailHold)
    std::atomic<int> consecutiveSilentBlocks_{0};

    // T-197: WAV cue markers — single-producer (audio thread) writes,
    // single-consumer (msg thread, in stopRecording) reads.
    // T-197b: kMaxLabel widened so each marker's label can carry the
    // post-edit script source. JUCE's labl chunk is fixed-width because we
    // pre-allocate an identical-length placeholder per slot at construction.
    static constexpr int kMaxMarkers = 64;
    static constexpr int kMaxLabel   = 1024;
    struct Marker { int64_t sample; char label[kMaxLabel]; };
    Marker markers_[kMaxMarkers]{};
    std::atomic<int>     markerHead_{0};
    std::atomic<int64_t> samplesWritten_{0};
    std::atomic<uint64_t> acceptedFrames_{0};
    std::atomic<int64_t> acceptedSamples_{0};
    std::atomic<uint64_t> rejectedFrames_{0};
    std::atomic<int64_t> rejectedSamples_{0};

    // T-197: locate the `cue ` chunk in `currentFile_` and overwrite each
    // Cue record's Offset field with the corresponding markers_[i].sample.
    // Called from the message thread after threadedWriter_.reset() finalizes
    // the file. JUCE writes metadata chunks before the data chunk, so a
    // 64KB header scan is sufficient to find `cue `.
    void patchCueOffsets()
    {
        const int fired = juce::jmin(markerHead_.load(std::memory_order_acquire), kMaxMarkers);
        if (fired <= 0) return;
        if (!currentFile_.existsAsFile()) return;

        // Read the header region. JUCE places metadata chunks before `data`.
        // T-197b: widened to 256 KB so the full LIST/adtl chunk
        // (64 labels × ~1036 bytes ≈ 66 KB) fits inside the scan window.
        constexpr juce::int64 scanCap = 256 * 1024;
        const juce::int64 fileSize = currentFile_.getSize();
        const juce::int64 toRead = juce::jmin(scanCap, fileSize);
        juce::MemoryBlock head;
        head.setSize((size_t) toRead);
        {
            juce::FileInputStream in(currentFile_);
            if (!in.openedOk()) return;
            const int got = in.read(head.getData(), (int) toRead);
            if (got < 12) return;
        }

        const auto* d = static_cast<const uint8_t*>(head.getData());
        if (std::memcmp(d, "RIFF", 4) != 0 || std::memcmp(d + 8, "WAVE", 4) != 0)
            return;

        // Walk RIFF chunks to find `cue ` and the LIST/adtl wrapper.
        juce::int64 cueChunkPos = -1;
        juce::int64 listAdtlPos = -1;
        size_t      listAdtlSize = 0;
        uint32_t numCues = 0;
        size_t pos = 12;
        while (pos + 8 <= head.getSize())
        {
            const uint32_t len = juce::ByteOrder::littleEndianInt(d + pos + 4);
            if (std::memcmp(d + pos, "cue ", 4) == 0)
            {
                if (pos + 12 > head.getSize()) return;
                numCues = juce::ByteOrder::littleEndianInt(d + pos + 8);
                cueChunkPos = (juce::int64) pos;
            }
            else if (std::memcmp(d + pos, "LIST", 4) == 0
                  && pos + 12 <= head.getSize()
                  && std::memcmp(d + pos + 8, "adtl", 4) == 0)
            {
                listAdtlPos  = (juce::int64) pos;
                listAdtlSize = (size_t) len;
            }
            pos += 8 + len + (len & 1u);
        }
        if (cueChunkPos < 0 || numCues == 0) return;

        // Patch each fired cue's Offset (last 4 bytes of each 24-byte Cue record).
        // Cue chunk layout: 8-byte chunk header, 4-byte numCues, then numCues * 24 bytes.
        juce::FileOutputStream out(currentFile_);
        if (!out.openedOk()) return;
        const int patchN = juce::jmin(fired, (int) numCues);
        for (int i = 0; i < patchN; ++i)
        {
            const juce::int64 offsetFieldPos =
                cueChunkPos + 8 + 4 + (juce::int64) i * 24 + 20;
            if (!out.setPosition(offsetFieldPos)) return;
            const uint32_t v = (uint32_t) markers_[i].sample;
            const uint8_t bytes[4] = {
                (uint8_t) (v        & 0xff),
                (uint8_t) ((v >> 8)  & 0xff),
                (uint8_t) ((v >> 16) & 0xff),
                (uint8_t) ((v >> 24) & 0xff)
            };
            out.write(bytes, 4);
        }

        // T-197b: walk LIST/adtl labl subchunks in order; for each fired
        // marker overwrite the text region (after the 4-byte identifier)
        // with the script source. Trailing bytes within the fixed kMaxLabel
        // window become NUL so viewers truncate at the script end.
        if (listAdtlPos >= 0 && listAdtlSize >= 4)
        {
            const size_t listStart = (size_t) listAdtlPos + 8 + 4; // after LIST hdr + "adtl"
            const size_t listEnd   = (size_t) listAdtlPos + 8 + listAdtlSize;
            size_t lp = listStart;
            int labelIdx = 0;
            while (lp + 8 <= listEnd && lp + 8 <= head.getSize() && labelIdx < patchN)
            {
                const uint32_t lblLen = juce::ByteOrder::littleEndianInt(d + lp + 4);
                if (std::memcmp(d + lp, "labl", 4) == 0)
                {
                    // labl payload: 4-byte cuePointID, then null-terminated text.
                    const size_t textRegionStart = lp + 8 + 4;
                    const size_t textRegionLen   = (size_t) lblLen >= 4u ? (size_t) lblLen - 4u : 0u;
                    if (textRegionLen > 0)
                    {
                        const Marker& m = markers_[labelIdx];
                        const juce::int64 writePos = (juce::int64) textRegionStart;
                        if (!out.setPosition(writePos)) return;
                        char payload[kMaxLabel];
                        std::memset(payload, 0, sizeof(payload));
                        std::strncpy(payload, m.label, kMaxLabel - 1);
                        const size_t writeLen = juce::jmin(textRegionLen, (size_t) kMaxLabel);
                        out.write(payload, writeLen);
                    }
                    ++labelIdx;
                }
                lp += 8 + lblLen + (lblLen & 1u);
            }
        }

        out.flush();
        DBG("CURLOP Recorder: patched " + juce::String(patchN) + " WAV cue offsets + labels");
    }

    // -60dB in linear amplitude
    static constexpr float kSilenceThreshold = 0.001f;
    // At 48kHz/512 block size, ~93 blocks/sec. 50 blocks ≈ 0.5s of sustained silence.
    static constexpr int kRequiredSilentBlocks = 50;
    // No max-tail cap (Neo, B-320): the tail runs until the audio decays below
    // kSilenceThreshold for kRequiredSilentBlocks, however long that takes.
};

} // namespace curlop
