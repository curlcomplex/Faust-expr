#pragma once

#include "graph/engine/buffer/BufferReadCursor.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace curlop {
namespace buffer {

class BufferStorage
{
public:
    static constexpr int kVisualBins = 256;
    static constexpr int kChunkSamples = 65536;
    static constexpr int kDeckCount = 4;
    static constexpr int kVisualRebuildBudgetSamples = 32768;
    static constexpr int kRecordingVisualBinsPerSnapshot = kVisualBins;
    static constexpr int kSoftResidentChunkBudget = 96;
    static constexpr int kMaxChunks =
        (std::numeric_limits<int>::max() / kChunkSamples) + 1;

    struct VisualSnapshot {
        int capturedSamples = 0;
        int recordingSamples = 0;
        int recordingCapacitySamples = 0;
        bool recording = false;
        bool playing = false;
        bool recordOverrun = false;
        bool assetBacked = false;
        bool warming = false;
        float playhead = 0.0f;
        float residentStart = 0.0f;
        float residentEnd = 0.0f;
        float requestedResidentStart = 0.0f;
        float requestedResidentEnd = 0.0f;
        uint32_t generation = 0;
        std::array<float, kVisualBins> mins {};
        std::array<float, kVisualBins> maxs {};
        std::array<float, kVisualBins> recordingMins {};
        std::array<float, kVisualBins> recordingMaxs {};
        std::array<float, kVisualBins> resident {};
        std::array<float, kVisualBins> requested {};
    };

    struct FrameRange {
        int start = 0;
        int end = 0;
        bool valid = false;
    };

    struct ChunkRange {
        int start = 0;
        int end = 0;
    };

    BufferStorage()
        : growThread_([this] { growLoop(); })
    {}

    ~BufferStorage()
    {
        stopGrowThread_.store(true, std::memory_order_release);
        growCv_.notify_one();
        if (growThread_.joinable())
            growThread_.join();

        for (auto& deck : decks_)
            deck.bank.reset();
    }

    BufferStorage(const BufferStorage&) = delete;
    BufferStorage& operator=(const BufferStorage&) = delete;

    void prepare(double sampleRate)
    {
        const int sr = juce::jmax(1, (int) std::round(sampleRate));
        preparedSampleRate_ = sr;
        const int targetChunks = chunksForSamples(sr * 4);
        ensureChunksPreparedBlocking(committedDeck(), 1);
        if (auto* staging = stagingDeckOrNull())
            ensureChunksPreparedBlocking(*staging, 1);
        for (auto& deck : decks_)
            requestPreparedSamples(deck, targetChunks * kChunkSamples);
        growCv_.notify_one();
        rebuildVisualBinsIfNeeded(committedDeck());
    }

    void clearCapture() noexcept
    {
        clearDeckRuntime(committedDeck(), true);
        visualPlaying_.store(false, std::memory_order_relaxed);
        visualPlayhead_.store(0.0f, std::memory_order_relaxed);
    }

    int capturedSamples() const noexcept { return committedDeck().capturedSamples.load(std::memory_order_acquire); }
    int recordingSamples() const noexcept
    {
        const auto* deck = stagingDeckOrNull();
        return deck != nullptr ? deck->capturedSamples.load(std::memory_order_acquire) : 0;
    }
    int preparedSampleRate() const noexcept { return preparedSampleRate_; }
    bool hasCapturedAudio() const noexcept { return capturedSamples() > 0; }
    bool isAssetBacked() const noexcept { return committedDeck().assetBacked.load(std::memory_order_acquire); }
    bool isDirty() const noexcept { return dirty_.load(std::memory_order_relaxed); }
    void clearDirty() noexcept { dirty_.store(false, std::memory_order_relaxed); }
    uint32_t dirtyGeneration() const noexcept { return dirtyGeneration_.load(std::memory_order_acquire); }
    bool clearDirtyIfGeneration(uint32_t generation) noexcept
    {
        if (dirtyGeneration_.load(std::memory_order_acquire) != generation)
            return false;
        dirty_.store(false, std::memory_order_release);
        return true;
    }
    // Save rollback restores the pre-commit persistence obligation when the
    // project file could not be committed after writing a buffer asset.
    void markDirtyForPersistenceRollback() noexcept { markDirty(); }
    int writePosition() const noexcept { return committedDeck().writePosition.load(std::memory_order_relaxed); }
    void setWritePosition(int v) noexcept { committedDeck().writePosition.store(juce::jmax(0, v), std::memory_order_relaxed); }
    int recordingWritePosition() const noexcept
    {
        auto* deck = stagingDeckOrNull();
        return deck != nullptr ? deck->writePosition.load(std::memory_order_relaxed) : 0;
    }
    void setRecordingWritePosition(int v) noexcept
    {
        if (auto* deck = stagingDeckOrNull())
            deck->writePosition.store(juce::jmax(0, v), std::memory_order_relaxed);
    }
    bool recordOverrun() const noexcept
    {
        for (const auto& deck : decks_)
            if (deck.recordOverrun.load(std::memory_order_relaxed))
                return true;
        return false;
    }
    void clearRecordOverrun() noexcept
    {
        for (auto& deck : decks_)
            deck.recordOverrun.store(false, std::memory_order_relaxed);
    }
    void requestVisualRebuild() noexcept
    {
        committedDeck().visualRebuildRequested.store(true, std::memory_order_release);
        visualGeneration_.fetch_add(1, std::memory_order_relaxed);
    }
    void markRecordOverrun() noexcept
    {
        committedDeck().recordOverrun.store(true, std::memory_order_relaxed);
        visualGeneration_.fetch_add(1, std::memory_order_relaxed);
    }
    void markRecordingOverrun() noexcept
    {
        if (auto* deck = stagingDeckOrNull())
            deck->recordOverrun.store(true, std::memory_order_relaxed);
        visualGeneration_.fetch_add(1, std::memory_order_relaxed);
    }

    int capacitySamples() const noexcept
    {
        return capacitySamples(committedDeck());
    }

    int recordingCapacitySamples() const noexcept
    {
        const auto* deck = stagingDeckOrNull();
        return deck != nullptr ? capacitySamples(*deck) : 0;
    }

    void requestPreparedSamples(int requiredSamples) noexcept
    {
        requestPreparedSamples(committedDeck(), requiredSamples);
    }

    void requestRecordingPreparedSamples(int requiredSamples) noexcept
    {
        if (auto* deck = stagingDeckOrNull())
            requestPreparedSamples(*deck, requiredSamples);
    }

    bool writeStereoFrame(int frame, float left, float right) noexcept
    {
        return writeStereoFrame(committedDeck(), frame, left, right);
    }

    bool writeRecordingStereoFrame(int frame, float left, float right) noexcept
    {
        auto* deck = stagingDeckOrNull();
        return deck != nullptr && writeStereoFrame(*deck, frame, left, right);
    }

    float sampleAt(int channel, int frame) const noexcept
    {
        const ScopedDeckRead read(*this, committedDeckIndex_.load(std::memory_order_acquire));
        return sampleAt(read.deck(), channel, frame);
    }

    float recordingSampleAt(int channel, int frame) const noexcept
    {
        const int index = stagingDeckIndex_.load(std::memory_order_acquire);
        if (!validDeckIndex(index))
            return 0.0f;
        const ScopedDeckRead read(*this, index);
        return sampleAt(read.deck(), channel, frame);
    }

    BufferView view() const noexcept
    {
        int index = committedDeckIndex_.load(std::memory_order_acquire);
        if (!validDeckIndex(index))
            index = 0;
        retainDeck(index);

        BufferView view;
        view.owner = this;
        view.readSample = &BufferStorage::readSampleThunk;
        view.retainToken = &BufferStorage::retainDeckThunk;
        view.releaseToken = &BufferStorage::releaseDeckThunk;
        view.ownerToken = index;
        view.numChannels = 2;
        view.numSamples = decks_[(size_t) index].capturedSamples.load(std::memory_order_acquire);
        return view;
    }

    void publishCapturedSamples(int v) noexcept
    {
        auto& deck = committedDeck();
        const int captured = juce::jlimit(0, capacitySamples(deck), v);
        deck.capturedSamples.store(captured, std::memory_order_release);
        deck.visualCapturedSamples.store(captured, std::memory_order_relaxed);
        markDirty();
        visualGeneration_.fetch_add(1, std::memory_order_relaxed);
    }

    void publishRecordingSamples(int v) noexcept
    {
        auto* deck = stagingDeckOrNull();
        if (deck == nullptr)
            return;
        const int captured = juce::jlimit(0, capacitySamples(*deck), v);
        deck->capturedSamples.store(captured, std::memory_order_release);
        deck->visualCapturedSamples.store(captured, std::memory_order_relaxed);
        visualGeneration_.fetch_add(1, std::memory_order_relaxed);
    }

    bool copyCapturedAudio(juce::AudioBuffer<float>& dest, int& outSamples, int& outSampleRate) const
    {
        const ScopedDeckRead read(*this, committedDeckIndex_.load(std::memory_order_acquire));
        const auto& deck = read.deck();
        const int samples = deck.capturedSamples.load(std::memory_order_acquire);
        if (samples <= 0)
            return false;

        dest.setSize(2, samples, false, false, true);
        for (int i = 0; i < samples; ++i) {
            dest.setSample(0, i, sampleAt(deck, 0, i));
            dest.setSample(1, i, sampleAt(deck, 1, i));
        }
        outSamples = samples;
        outSampleRate = juce::jmax(1, preparedSampleRate_);
        return true;
    }

    bool loadCapturedAudio(const juce::AudioBuffer<float>& source,
                           int samples,
                           int sampleRate)
    {
        if (samples <= 0 || source.getNumChannels() <= 0)
            return false;

        preparedSampleRate_ = juce::jmax(1, sampleRate);
        reclaimRetiredDecks();
        const int targetIndex = findWritableDeck(true);
        if (!validDeckIndex(targetIndex))
            return false;

        auto& deck = decks_[(size_t) targetIndex];
        if (!ensureChunksPreparedBlocking(deck, chunksForSamples(samples)))
            return false;

        clearDeckForAssetLoad(deck, false);
        const bool mono = source.getNumChannels() == 1;
        for (int i = 0; i < samples; ++i) {
            const float left = source.getSample(0, i);
            const float right = mono ? left : source.getSample(1, i);
            if (!writeStereoFrame(deck, i, left, right))
                return false;
        }

        deck.capturedSamples.store(samples, std::memory_order_release);
        deck.writePosition.store(samples, std::memory_order_relaxed);
        deck.visualCapturedSamples.store(samples, std::memory_order_relaxed);
        visualRecording_.store(false, std::memory_order_relaxed);
        visualPlaying_.store(false, std::memory_order_relaxed);
        visualPlayhead_.store(0.0f, std::memory_order_relaxed);
        dirty_.store(false, std::memory_order_relaxed);
        rebuildVisualBins(deck);
        const int oldCommitted = committedDeckIndex_.load(std::memory_order_acquire);
        committedDeckIndex_.store(targetIndex, std::memory_order_release);
        retireDeckAfterPromotion(oldCommitted);
        const int stagingIndex = stagingDeckIndex_.load(std::memory_order_acquire);
        if (validDeckIndex(oldCommitted)
            && oldCommitted != targetIndex
            && oldCommitted != stagingIndex
            && decks_[(size_t) oldCommitted].readers.load(std::memory_order_acquire) == 0)
            clearDeckForAssetLoad(decks_[(size_t) oldCommitted], false);
        return true;
    }

    bool loadAssetFile(const juce::File& file, bool forceReload = false)
    {
        std::lock_guard<std::mutex> ioLock(assetIoMutex_);
        if (!file.existsAsFile())
            return false;

        const auto fingerprint = fingerprintFor(file);
        const auto& currentDeck = committedDeck();
        if (!forceReload
            && currentDeck.assetBacked.load(std::memory_order_acquire)
            && currentDeck.assetFile == file
            && currentDeck.assetFingerprint == fingerprint
            && currentDeck.capturedSamples.load(std::memory_order_acquire) > 0)
            return true;

        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader(fm.createReaderFor(file));
        if (!reader || reader->lengthInSamples <= 0 || reader->numChannels <= 0)
            return false;

        const int samples = (int) juce::jmin<juce::int64>(reader->lengthInSamples,
                                                          std::numeric_limits<int>::max());
        if (samples <= 0)
            return false;

        reclaimRetiredDecks();
        const int targetIndex = findWritableDeck(true);
        if (!validDeckIndex(targetIndex))
            return false;

        preparedSampleRate_ = juce::jmax(1, (int) std::round(reader->sampleRate));
        auto& deck = decks_[(size_t) targetIndex];
        clearDeckForAssetLoad(deck, false);

        deck.assetFile = file;
        deck.assetFingerprint = fingerprint;
        deck.assetGeneration.fetch_add(1, std::memory_order_acq_rel);
        deck.assetBacked.store(true, std::memory_order_release);
        deck.capturedSamples.store(samples, std::memory_order_release);
        deck.writePosition.store(samples, std::memory_order_relaxed);
        deck.visualCapturedSamples.store(samples, std::memory_order_relaxed);
        deck.assetChannels.store((int) reader->numChannels, std::memory_order_relaxed);
        visualPlaying_.store(false, std::memory_order_relaxed);
        visualPlayhead_.store(0.0f, std::memory_order_relaxed);
        dirty_.store(false, std::memory_order_relaxed);
        buildVisualBinsFromReader(deck, *reader, samples);
        const int oldCommitted = committedDeckIndex_.load(std::memory_order_acquire);
        committedDeckIndex_.store(targetIndex, std::memory_order_release);
        retireDeckAfterPromotion(oldCommitted);
        const int stagingIndex = stagingDeckIndex_.load(std::memory_order_acquire);
        if (validDeckIndex(oldCommitted)
            && oldCommitted != targetIndex
            && oldCommitted != stagingIndex
            && decks_[(size_t) oldCommitted].readers.load(std::memory_order_acquire) == 0)
            clearDeckForAssetLoad(decks_[(size_t) oldCommitted], false);
        visualGeneration_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    void requestResidentFrameRange(int startFrame, int endFrame) noexcept
    {
        requestResidentFrameRanges(startFrame, endFrame, 0, 0);
    }

    void requestResidentFrameRanges(int startFrameA,
                                    int endFrameA,
                                    int startFrameB,
                                    int endFrameB) noexcept
    {
        auto& deck = committedDeck();
        if (!deck.assetBacked.load(std::memory_order_acquire))
            return;

        const int samples = deck.capturedSamples.load(std::memory_order_acquire);
        if (samples <= 0)
            return;

        FrameRange framesA = normalizedFrameRange(samples, startFrameA, endFrameA);
        FrameRange framesB = normalizedFrameRange(samples, startFrameB, endFrameB);
        if (!framesA.valid && !framesB.valid)
            return;
        if (!framesA.valid)
            std::swap(framesA, framesB);

        const int budgetA = framesB.valid ? juce::jmax(1, kSoftResidentChunkBudget / 2)
                                          : kSoftResidentChunkBudget;
        const int budgetB = framesB.valid ? juce::jmax(1, kSoftResidentChunkBudget - budgetA)
                                          : 0;
        const ChunkRange chunksA = chunkRangeForFrames(framesA, budgetA);
        const ChunkRange chunksB = framesB.valid ? chunkRangeForFrames(framesB, budgetB)
                                                 : ChunkRange {};

        if (deck.requestedWarmStartChunk.load(std::memory_order_acquire) == chunksA.start
            && deck.requestedWarmEndChunk.load(std::memory_order_acquire) == chunksA.end
            && deck.requestedWarm2StartChunk.load(std::memory_order_acquire) == chunksB.start
            && deck.requestedWarm2EndChunk.load(std::memory_order_acquire) == chunksB.end)
            return;

        const int requestedStart = framesB.valid ? juce::jmin(framesA.start, framesB.start) : framesA.start;
        const int requestedEnd = framesB.valid ? juce::jmax(framesA.end, framesB.end) : framesA.end;
        deck.requestedResidentStartFrame.store(requestedStart, std::memory_order_release);
        deck.requestedResidentEndFrame.store(requestedEnd, std::memory_order_release);
        deck.requestedWarmStartChunk.store(chunksA.start, std::memory_order_release);
        deck.requestedWarmEndChunk.store(chunksA.end, std::memory_order_release);
        deck.requestedWarm2StartChunk.store(chunksB.start, std::memory_order_release);
        deck.requestedWarm2EndChunk.store(chunksB.end, std::memory_order_release);
        // Audio thread callers publish the request only; growLoop polls this
        // state on its bounded wait instead of being woken from processBlock.
    }

    void requestResidentRangeNormalized(float normalizedStart,
                                        float normalizedEnd,
                                        float normalizedPosition) noexcept
    {
        auto& deck = committedDeck();
        const int samples = deck.capturedSamples.load(std::memory_order_acquire);
        if (samples <= 0)
            return;

        const float a = juce::jlimit(0.0f, 1.0f, std::isfinite(normalizedStart) ? normalizedStart : 0.0f);
        const float b = juce::jlimit(0.0f, 1.0f, std::isfinite(normalizedEnd) ? normalizedEnd : 1.0f);
        const float lo = juce::jmin(a, b);
        const float hi = juce::jmax(a, b);
        const int startFrame = juce::jlimit(0, samples - 1, (int) std::floor(lo * (float) samples));
        const int endFrame = juce::jlimit(startFrame + 1, samples, (int) std::ceil(hi * (float) samples));
        const float pos = juce::jlimit(0.0f, 1.0f, std::isfinite(normalizedPosition) ? normalizedPosition : 0.0f);
        const int center = startFrame + (int) std::round(pos * (float) (endFrame - startFrame - 1));
        const int halo = juce::jmax(1, kChunkSamples / 2);
        requestResidentFrameRange(center - halo, center + halo);
    }

    void beginRecording() noexcept
    {
        if (!ensureWritableStagingDeck()) {
            markRecordingOverrun();
            visualRecording_.store(false, std::memory_order_relaxed);
            return;
        }
        auto& deck = stagingDeck();
        clearDeckRuntime(deck, false);
        deck.visualRebuildRequested.store(true, std::memory_order_release);
        visualRecording_.store(true, std::memory_order_relaxed);
    }

    bool promoteRecordingToCommitted() noexcept
    {
        const int stagingIndex = stagingDeckIndex_.load(std::memory_order_acquire);
        if (!validDeckIndex(stagingIndex)) {
            visualRecording_.store(false, std::memory_order_relaxed);
            return false;
        }
        auto& staged = decks_[(size_t) stagingIndex];
        if (staged.capturedSamples.load(std::memory_order_acquire) <= 0) {
            clearDeckRuntime(staged, false);
            visualRecording_.store(false, std::memory_order_relaxed);
            return false;
        }

        const int oldCommitted = committedDeckIndex_.load(std::memory_order_acquire);
        const int newCommitted = stagingIndex;
        committedDeckIndex_.store(newCommitted, std::memory_order_release);
        stagingDeckIndex_.store(-1, std::memory_order_release);
        retireDeckAfterPromotion(oldCommitted);
        staged.visualRebuildRequested.store(true, std::memory_order_release);
        visualRecording_.store(false, std::memory_order_relaxed);
        markDirty();
        promotedGeneration_.fetch_add(1, std::memory_order_relaxed);
        visualGeneration_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    uint32_t promotedGeneration() const noexcept
    {
        return promotedGeneration_.load(std::memory_order_relaxed);
    }

    uint32_t recordingVisualComposeCount() const noexcept
    {
        return recordingVisualComposeCount_.load(std::memory_order_relaxed);
    }

    void setRecording(bool v) noexcept { visualRecording_.store(v, std::memory_order_relaxed); }
    void setPlaying(bool v) noexcept { visualPlaying_.store(v, std::memory_order_relaxed); }
    void setPlayhead(float v) noexcept { visualPlayhead_.store(v, std::memory_order_relaxed); }

    void resetVisualBins() noexcept
    {
        resetVisualBins(committedDeck());
        visualGeneration_.fetch_add(1, std::memory_order_relaxed);
    }

    void updateVisualBin(int frame, float left, float right, int sampleCount) noexcept
    {
        updateVisualBin(committedDeck(), frame, left, right, sampleCount);
    }

    void rebuildVisualBins() noexcept
    {
        rebuildVisualBins(committedDeck());
    }

    VisualSnapshot visualSnapshot() noexcept
    {
        reclaimRetiredDecks();
        const int committedIndex = committedDeckIndex_.load(std::memory_order_acquire);
        const ScopedDeckRead committedRead(*this, committedIndex);
        auto& committed = const_cast<Deck&>(committedRead.deck());
        advanceVisualRebuild(committed, kVisualRebuildBudgetSamples);

        const bool recording = visualRecording_.load(std::memory_order_relaxed);
        const int stagingIndex = stagingDeckIndex_.load(std::memory_order_acquire);
        std::optional<ScopedDeckRead> stagingRead;
        Deck* staging = nullptr;
        if (recording && validDeckIndex(stagingIndex)) {
            stagingRead.emplace(*this, stagingIndex);
            staging = &const_cast<Deck&>(stagingRead->deck());
            composeRecordingVisualBinsIfNeeded(*staging);
        }

        VisualSnapshot s;
        s.capturedSamples = committed.visualCapturedSamples.load(std::memory_order_relaxed);
        s.recordingSamples = staging != nullptr
            ? staging->visualCapturedSamples.load(std::memory_order_relaxed)
            : 0;
        s.recordingCapacitySamples = staging != nullptr
            ? capacitySamples(*staging)
            : 0;
        s.recording = recording && staging != nullptr;
        s.playing = visualPlaying_.load(std::memory_order_relaxed);
        s.recordOverrun = recordOverrun();
        s.assetBacked = committed.assetBacked.load(std::memory_order_acquire);
        s.warming = s.assetBacked
            && ((committed.requestedWarmEndChunk.load(std::memory_order_acquire)
                    > committed.requestedWarmStartChunk.load(std::memory_order_acquire))
                || (committed.requestedWarm2EndChunk.load(std::memory_order_acquire)
                    > committed.requestedWarm2StartChunk.load(std::memory_order_acquire)))
            && !requestedRangeResident(committed);
        s.playhead = visualPlayhead_.load(std::memory_order_relaxed);
        const int captured = juce::jmax(1, s.capturedSamples);
        s.residentStart = (float) committed.residentStartFrame.load(std::memory_order_acquire) / (float) captured;
        s.residentEnd = (float) committed.residentEndFrame.load(std::memory_order_acquire) / (float) captured;
        s.requestedResidentStart =
            (float) committed.requestedResidentStartFrame.load(std::memory_order_acquire) / (float) captured;
        s.requestedResidentEnd =
            (float) committed.requestedResidentEndFrame.load(std::memory_order_acquire) / (float) captured;
        s.generation = visualGeneration_.load(std::memory_order_relaxed);
        for (int i = 0; i < kVisualBins; ++i) {
            s.mins[(size_t) i] = committed.visualMin[(size_t) i].load(std::memory_order_relaxed);
            s.maxs[(size_t) i] = committed.visualMax[(size_t) i].load(std::memory_order_relaxed);
            s.recordingMins[(size_t) i] = staging != nullptr
                ? staging->visualMin[(size_t) i].load(std::memory_order_relaxed)
                : 0.0f;
            s.recordingMaxs[(size_t) i] = staging != nullptr
                ? staging->visualMax[(size_t) i].load(std::memory_order_relaxed)
                : 0.0f;
        }
        populateResidencyBins(committed, s);
        return s;
    }

    private:
    struct Chunk {
        Chunk()
        {
            audio.setSize(2, kChunkSamples, false, false, true);
            audio.clear();
        }

        juce::AudioBuffer<float> audio;
        std::atomic<bool> resident { false };
        std::atomic<uint32_t> visualGeneration { 0 };
        std::array<std::atomic<float>, kVisualBins> visualMin {};
        std::array<std::atomic<float>, kVisualBins> visualMax {};
        std::array<std::atomic<bool>, kVisualBins> visualHas {};
    };

    struct ChunkBank {
        ~ChunkBank()
        {
            for (auto& slot : chunks)
                delete slot.exchange(nullptr, std::memory_order_acq_rel);
        }

        std::array<std::atomic<Chunk*>, kMaxChunks> chunks {};
        std::atomic<int> preparedChunks { 0 };
        std::atomic<int> requestedChunks { 0 };
    };

    struct AssetFingerprint {
        juce::int64 size = 0;
        juce::int64 modificationMs = 0;

        bool operator==(const AssetFingerprint& other) const noexcept
        {
            return size == other.size && modificationMs == other.modificationMs;
        }
    };

    struct Deck {
        std::unique_ptr<ChunkBank> bank { std::make_unique<ChunkBank>() };
        std::atomic<int> capturedSamples { 0 };
        std::atomic<int> writePosition { 0 };
        int lastVisualRebuildSamples = 0;
        int visualRebuildCursor = 0;
        bool visualRebuilding = false;
        std::array<std::atomic<float>, kVisualBins> visualMin {};
        std::array<std::atomic<float>, kVisualBins> visualMax {};
        std::atomic<int> visualCapturedSamples { 0 };
        std::atomic<bool> recordOverrun { false };
        std::atomic<bool> visualRebuildRequested { false };
        std::atomic<bool> assetBacked { false };
        std::atomic<int> assetChannels { 0 };
        std::atomic<int> requestedWarmStartChunk { 0 };
        std::atomic<int> requestedWarmEndChunk { 0 };
        std::atomic<int> requestedWarm2StartChunk { 0 };
        std::atomic<int> requestedWarm2EndChunk { 0 };
        std::atomic<int> residentStartFrame { 0 };
        std::atomic<int> residentEndFrame { 0 };
        std::atomic<int> requestedResidentStartFrame { 0 };
        std::atomic<int> requestedResidentEndFrame { 0 };
        juce::File assetFile;
        AssetFingerprint assetFingerprint;
        std::atomic<uint32_t> assetGeneration { 0 };
        std::atomic<uint32_t> chunkVisualGeneration { 1 };
        int lastComposedRecordingSamples = -1;
        mutable std::atomic<int> readers { 0 };
    };

    struct ScopedDeckRead {
        ScopedDeckRead(const BufferStorage& ownerIn, int indexIn) noexcept
            : owner(ownerIn), index((indexIn >= 0 && indexIn < kDeckCount) ? indexIn : 0)
        {
            owner.decks_[(size_t) index].readers.fetch_add(1, std::memory_order_acq_rel);
        }

        ~ScopedDeckRead()
        {
            owner.decks_[(size_t) index].readers.fetch_sub(1, std::memory_order_acq_rel);
        }

        ScopedDeckRead(const ScopedDeckRead&) = delete;
        ScopedDeckRead& operator=(const ScopedDeckRead&) = delete;

        const Deck& deck() const noexcept { return owner.decks_[(size_t) index]; }

        const BufferStorage& owner;
        int index = 0;
    };

    static bool validDeckIndex(int index) noexcept
    {
        return index >= 0 && index < kDeckCount;
    }

    static AssetFingerprint fingerprintFor(const juce::File& file)
    {
        return { file.getSize(), file.getLastModificationTime().toMilliseconds() };
    }

    Deck& committedDeck() noexcept
    {
        return decks_[(size_t) committedDeckIndex_.load(std::memory_order_acquire)];
    }

    const Deck& committedDeck() const noexcept
    {
        return decks_[(size_t) committedDeckIndex_.load(std::memory_order_acquire)];
    }

    Deck& stagingDeck() noexcept
    {
        return decks_[(size_t) stagingDeckIndex_.load(std::memory_order_acquire)];
    }

    Deck* stagingDeckOrNull() noexcept
    {
        const int index = stagingDeckIndex_.load(std::memory_order_acquire);
        return validDeckIndex(index) ? &decks_[(size_t) index] : nullptr;
    }

    const Deck* stagingDeckOrNull() const noexcept
    {
        const int index = stagingDeckIndex_.load(std::memory_order_acquire);
        return validDeckIndex(index) ? &decks_[(size_t) index] : nullptr;
    }

    void markDirty() noexcept
    {
        dirtyGeneration_.fetch_add(1, std::memory_order_acq_rel);
        dirty_.store(true, std::memory_order_release);
    }

    void updateVisualBin(Deck& deck, int frame, float left, float right, int sampleCount) noexcept
    {
        if (sampleCount <= 0)
            return;
        const int bin = juce::jlimit(0, kVisualBins - 1, (int) (((int64_t) frame * kVisualBins) / sampleCount));
        const float mn = juce::jmin(left, right);
        const float mx = juce::jmax(left, right);
        auto& amin = deck.visualMin[(size_t) bin];
        auto& amax = deck.visualMax[(size_t) bin];
        if (mn < amin.load(std::memory_order_relaxed))
            amin.store(mn, std::memory_order_relaxed);
        if (mx > amax.load(std::memory_order_relaxed))
            amax.store(mx, std::memory_order_relaxed);
    }

    void updateChunkVisualBin(Deck& deck, Chunk& chunk, int offset, float left, float right) noexcept
    {
        const uint32_t generation = deck.chunkVisualGeneration.load(std::memory_order_acquire);
        if (chunk.visualGeneration.load(std::memory_order_acquire) != generation)
            resetChunkVisuals(chunk, generation);

        const int bin = juce::jlimit(0, kVisualBins - 1, (int) (((int64_t) offset * kVisualBins) / kChunkSamples));
        const float mn = juce::jmin(left, right);
        const float mx = juce::jmax(left, right);
        auto& amin = chunk.visualMin[(size_t) bin];
        auto& amax = chunk.visualMax[(size_t) bin];
        if (mn < amin.load(std::memory_order_relaxed))
            amin.store(mn, std::memory_order_relaxed);
        if (mx > amax.load(std::memory_order_relaxed))
            amax.store(mx, std::memory_order_relaxed);
        chunk.visualHas[(size_t) bin].store(true, std::memory_order_release);
    }

    void resetChunkVisuals(Chunk& chunk, uint32_t generation) noexcept
    {
        for (int i = 0; i < kVisualBins; ++i) {
            chunk.visualMin[(size_t) i].store(0.0f, std::memory_order_relaxed);
            chunk.visualMax[(size_t) i].store(0.0f, std::memory_order_relaxed);
            chunk.visualHas[(size_t) i].store(false, std::memory_order_relaxed);
        }
        chunk.visualGeneration.store(generation, std::memory_order_release);
    }

    void mergeVisualBin(std::atomic<float>& destMin,
                        std::atomic<float>& destMax,
                        float mn,
                        float mx) noexcept
    {
        if (mn < destMin.load(std::memory_order_relaxed))
            destMin.store(mn, std::memory_order_relaxed);
        if (mx > destMax.load(std::memory_order_relaxed))
            destMax.store(mx, std::memory_order_relaxed);
    }

    void mergeChunkSummaryBin(const Chunk& chunk,
                              int localBin,
                              std::atomic<float>& destMin,
                              std::atomic<float>& destMax) noexcept
    {
        localBin = juce::jlimit(0, kVisualBins - 1, localBin);
        if (!chunk.visualHas[(size_t) localBin].load(std::memory_order_acquire))
            return;

        mergeVisualBin(destMin,
                       destMax,
                       chunk.visualMin[(size_t) localBin].load(std::memory_order_relaxed),
                       chunk.visualMax[(size_t) localBin].load(std::memory_order_relaxed));
    }

    void mergeChunkSummaryRange(Deck& deck,
                                int frameStart,
                                int frameEnd,
                                std::atomic<float>& destMin,
                                std::atomic<float>& destMax) noexcept
    {
        if (frameEnd <= frameStart)
            return;

        const int startChunk = frameStart / kChunkSamples;
        const int endChunk = (frameEnd - 1) / kChunkSamples;
        for (int chunkIndex = startChunk; chunkIndex <= endChunk; ++chunkIndex) {
            const auto* chunk = chunkAt(deck, chunkIndex);
            if (chunk == nullptr)
                continue;

            const int chunkFrameStart = chunkIndex * kChunkSamples;
            const int localStart = juce::jlimit(0, kChunkSamples, frameStart - chunkFrameStart);
            const int localEnd = juce::jlimit(localStart + 1, kChunkSamples, frameEnd - chunkFrameStart);
            const int localBinStart = juce::jlimit(0, kVisualBins - 1,
                (int) (((int64_t) localStart * kVisualBins) / kChunkSamples));
            const int localBinEnd = juce::jlimit(localBinStart, kVisualBins - 1,
                (int) ((((int64_t) localEnd * kVisualBins) + kChunkSamples - 1) / kChunkSamples) - 1);
            for (int localBin = localBinStart; localBin <= localBinEnd; ++localBin)
                mergeChunkSummaryBin(*chunk, localBin, destMin, destMax);
        }
    }

    void composeRecordingVisualBinsIfNeeded(Deck& deck) noexcept
    {
        const int samples = deck.capturedSamples.load(std::memory_order_acquire);
        if (samples == deck.lastComposedRecordingSamples)
            return;

        resetVisualBins(deck);
        if (samples <= 0) {
            deck.visualCapturedSamples.store(0, std::memory_order_relaxed);
            deck.lastComposedRecordingSamples = samples;
            return;
        }

        const int bins = juce::jmin(kVisualBins, kRecordingVisualBinsPerSnapshot);
        for (int bin = 0; bin < bins; ++bin) {
            const int frameStart = (int) (((int64_t) bin * samples) / kVisualBins);
            const int frameEnd = juce::jlimit(frameStart + 1, samples,
                (int) ((((int64_t) bin + 1) * samples + kVisualBins - 1) / kVisualBins));
            auto& destMin = deck.visualMin[(size_t) bin];
            auto& destMax = deck.visualMax[(size_t) bin];
            mergeChunkSummaryRange(deck, frameStart, frameEnd, destMin, destMax);
        }

        deck.lastVisualRebuildSamples = samples;
        deck.visualRebuilding = false;
        deck.visualRebuildCursor = samples;
        deck.visualCapturedSamples.store(samples, std::memory_order_relaxed);
        deck.lastComposedRecordingSamples = samples;
        recordingVisualComposeCount_.fetch_add(1, std::memory_order_relaxed);
    }

    void rebuildVisualBins(Deck& deck) noexcept
    {
        resetVisualBins(deck);
        const int samples = deck.capturedSamples.load(std::memory_order_acquire);
        if (samples <= 0)
            return;

        for (int i = 0; i < samples; ++i)
            updateVisualBin(deck, i, sampleAt(deck, 0, i), sampleAt(deck, 1, i), samples);

        deck.lastVisualRebuildSamples = samples;
        deck.visualRebuilding = false;
        deck.visualRebuildCursor = samples;
        deck.visualCapturedSamples.store(samples, std::memory_order_relaxed);
        visualGeneration_.fetch_add(1, std::memory_order_relaxed);
    }

    void rebuildVisualBinsIfNeeded(Deck& deck) noexcept
    {
        const int samples = deck.capturedSamples.load(std::memory_order_acquire);
        const bool requested = deck.visualRebuildRequested.load(std::memory_order_acquire);
        if (!requested
            && !deck.visualRebuilding
            && deck.lastVisualRebuildSamples == samples)
            return;

        deck.visualRebuildRequested.store(false, std::memory_order_release);
        rebuildVisualBins(deck);
    }

    void advanceVisualRebuild(Deck& deck, int budgetSamples) noexcept
    {
        if (deck.visualRebuildRequested.exchange(false, std::memory_order_acq_rel)) {
            resetVisualBins(deck);
            deck.visualRebuilding = true;
            deck.visualRebuildCursor = 0;
        }

        if (!deck.visualRebuilding)
            return;

        const int samples = deck.capturedSamples.load(std::memory_order_acquire);
        if (samples <= 0) {
            deck.visualRebuilding = false;
            deck.visualCapturedSamples.store(0, std::memory_order_relaxed);
            return;
        }

        const int start = juce::jlimit(0, samples, deck.visualRebuildCursor);
        const int end = juce::jlimit(start, samples, start + juce::jmax(1, budgetSamples));
        for (int i = start; i < end; ++i)
            updateVisualBin(deck, i, sampleAt(deck, 0, i), sampleAt(deck, 1, i), samples);

        deck.visualRebuildCursor = end;
        deck.visualCapturedSamples.store(samples, std::memory_order_relaxed);
        if (end >= samples) {
            deck.visualRebuilding = false;
            deck.lastVisualRebuildSamples = samples;
        }
        visualGeneration_.fetch_add(1, std::memory_order_relaxed);
    }

    void resetVisualBins(Deck& deck) noexcept
    {
        for (int i = 0; i < kVisualBins; ++i) {
            deck.visualMin[(size_t) i].store(0.0f, std::memory_order_relaxed);
            deck.visualMax[(size_t) i].store(0.0f, std::memory_order_relaxed);
        }
        deck.lastVisualRebuildSamples = 0;
        deck.visualRebuildCursor = 0;
        deck.visualRebuilding = false;
    }

    uint32_t advanceChunkVisualGeneration(Deck& deck) noexcept
    {
        uint32_t next = deck.chunkVisualGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (next == 0) {
            deck.chunkVisualGeneration.store(1, std::memory_order_release);
            next = 1;
        }
        return next;
    }

    void resetChunkResidency(Deck& deck) noexcept
    {
        const uint32_t generation = advanceChunkVisualGeneration(deck);
        for (int i = 0; i < kMaxChunks; ++i)
            if (auto* chunk = chunkAt(deck, i)) {
                chunk->resident.store(false, std::memory_order_release);
                resetChunkVisuals(*chunk, generation);
            }
    }

    void clearDeckRuntime(Deck& deck, bool shouldMarkDirty) noexcept
    {
        deck.capturedSamples.store(0, std::memory_order_release);
        deck.writePosition.store(0, std::memory_order_relaxed);
        deck.recordOverrun.store(false, std::memory_order_relaxed);
        deck.visualCapturedSamples.store(0, std::memory_order_relaxed);
        deck.visualRebuildRequested.store(false, std::memory_order_relaxed);
        deck.assetBacked.store(false, std::memory_order_release);
        deck.assetChannels.store(0, std::memory_order_relaxed);
        deck.requestedWarmStartChunk.store(0, std::memory_order_relaxed);
        deck.requestedWarmEndChunk.store(0, std::memory_order_relaxed);
        deck.requestedWarm2StartChunk.store(0, std::memory_order_relaxed);
        deck.requestedWarm2EndChunk.store(0, std::memory_order_relaxed);
        deck.residentStartFrame.store(0, std::memory_order_relaxed);
        deck.residentEndFrame.store(0, std::memory_order_relaxed);
        deck.requestedResidentStartFrame.store(0, std::memory_order_relaxed);
        deck.requestedResidentEndFrame.store(0, std::memory_order_relaxed);
        deck.assetGeneration.fetch_add(1, std::memory_order_acq_rel);
        advanceChunkVisualGeneration(deck);
        deck.lastComposedRecordingSamples = -1;
        resetVisualBins(deck);
        if (shouldMarkDirty)
            markDirty();
        visualGeneration_.fetch_add(1, std::memory_order_relaxed);
    }

    void clearDeckForAssetLoad(Deck& deck, bool shouldMarkDirty) noexcept
    {
        clearDeckRuntime(deck, shouldMarkDirty);
        deck.assetFile = juce::File();
        deck.assetFingerprint = {};
        resetChunkResidency(deck);
    }

    void retireDeckAfterPromotion(int index) noexcept
    {
        if (!validDeckIndex(index))
            return;
        if (decks_[(size_t) index].readers.load(std::memory_order_acquire) == 0) {
            retired_[(size_t) index].store(false, std::memory_order_release);
            return;
        }
        retired_[(size_t) index].store(true, std::memory_order_release);
    }

    void reclaimRetiredDecks() noexcept
    {
        for (int i = 0; i < kDeckCount; ++i)
            if (retired_[(size_t) i].load(std::memory_order_acquire)
                && decks_[(size_t) i].readers.load(std::memory_order_acquire) == 0)
                retired_[(size_t) i].store(false, std::memory_order_release);
    }

    bool deckIsWritable(int index, bool allowAssetBacked) const noexcept
    {
        const int stagingIndex = stagingDeckIndex_.load(std::memory_order_acquire);
        return validDeckIndex(index)
            && index != committedDeckIndex_.load(std::memory_order_acquire)
            && index != stagingIndex
            && !retired_[(size_t) index].load(std::memory_order_acquire)
            && decks_[(size_t) index].readers.load(std::memory_order_acquire) == 0
            && (allowAssetBacked || !decks_[(size_t) index].assetBacked.load(std::memory_order_acquire));
    }

    bool deckIsUsableForStaging(int index) const noexcept
    {
        return validDeckIndex(index)
            && index != committedDeckIndex_.load(std::memory_order_acquire)
            && !retired_[(size_t) index].load(std::memory_order_acquire)
            && decks_[(size_t) index].readers.load(std::memory_order_acquire) == 0
            && !decks_[(size_t) index].assetBacked.load(std::memory_order_acquire);
    }

    int findWritableDeck(bool allowAssetBacked = false) const noexcept
    {
        for (int i = 0; i < kDeckCount; ++i)
            if (deckIsWritable(i, allowAssetBacked))
                return i;
        return -1;
    }

    bool ensureWritableStagingDeck() noexcept
    {
        reclaimRetiredDecks();
        const int stagingIndex = stagingDeckIndex_.load(std::memory_order_acquire);
        if (deckIsUsableForStaging(stagingIndex))
            return true;

        const int writableIndex = findWritableDeck();
        if (validDeckIndex(writableIndex)) {
            stagingDeckIndex_.store(writableIndex, std::memory_order_release);
            return true;
        }
        stagingDeckIndex_.store(-1, std::memory_order_release);
        return false;
    }

    static int chunksForSamples(int samples) noexcept
    {
        if (samples <= 0)
            return 1;
        const int64_t chunks = ((int64_t) samples + kChunkSamples - 1) / kChunkSamples;
        return juce::jlimit(1, kMaxChunks, (int) chunks);
    }

    static FrameRange normalizedFrameRange(int samples, int startFrame, int endFrame) noexcept
    {
        if (samples <= 0 || endFrame <= startFrame)
            return {};
        startFrame = juce::jlimit(0, samples - 1, startFrame);
        endFrame = juce::jlimit(startFrame + 1, samples, endFrame);
        return { startFrame, endFrame, true };
    }

    static ChunkRange chunkRangeForFrames(FrameRange frames, int budget) noexcept
    {
        if (!frames.valid || budget <= 0)
            return {};

        int startChunk = frames.start / kChunkSamples;
        int endChunk = chunksForSamples(frames.end) - 1;
        const int requestedChunks = endChunk - startChunk + 1;
        if (requestedChunks > budget) {
            const int centerChunk = (startChunk + endChunk) / 2;
            const int half = budget / 2;
            startChunk = juce::jlimit(startChunk, endChunk, centerChunk - half);
            endChunk = juce::jlimit(startChunk, endChunk, startChunk + budget - 1);
        }
        return { startChunk, endChunk + 1 };
    }

    Chunk* chunkAt(const Deck& deck, int index) const noexcept
    {
        if (index < 0 || index >= kMaxChunks)
            return nullptr;
        return deck.bank->chunks[(size_t) index].load(std::memory_order_acquire);
    }

    bool ensureChunksPreparedBlocking(Deck& deck, int targetChunks)
    {
        targetChunks = juce::jlimit(1, kMaxChunks, targetChunks);
        for (int i = 0; i < targetChunks; ++i)
            if (!allocateChunk(deck, i))
                return false;
        publishPreparedCount(deck);
        return true;
    }

    bool allocateChunk(Deck& deck, int index)
    {
        if (chunkAt(deck, index) != nullptr)
            return true;

        std::unique_ptr<Chunk> fresh;
        try {
            fresh = std::make_unique<Chunk>();
        } catch (...) {
            return false;
        }

        Chunk* expected = nullptr;
        if (deck.bank->chunks[(size_t) index].compare_exchange_strong(expected,
                                                                       fresh.get(),
                                                                       std::memory_order_release,
                                                                       std::memory_order_acquire)) {
            fresh.release();
            return true;
        }
        return true;
    }

    void publishPreparedCount(Deck& deck) noexcept
    {
        int count = deck.bank->preparedChunks.load(std::memory_order_acquire);
        while (count < kMaxChunks && chunkAt(deck, count) != nullptr)
            ++count;
        deck.bank->preparedChunks.store(count, std::memory_order_release);
    }

    void growLoop()
    {
        while (!stopGrowThread_.load(std::memory_order_acquire)) {
            for (auto& deck : decks_)
                growDeck(deck);

            std::unique_lock<std::mutex> lock(growMutex_);
            growCv_.wait_for(lock,
                             std::chrono::milliseconds(10),
                             [this] {
                                 if (stopGrowThread_.load(std::memory_order_acquire))
                                     return true;
                                 for (const auto& deck : decks_)
                                     if (deckNeedsGrowth(deck))
                                         return true;
                                 return false;
                             });
        }
    }

    void growDeck(Deck& deck)
    {
        const int target = deck.bank->requestedChunks.load(std::memory_order_acquire);
        int current = deck.bank->preparedChunks.load(std::memory_order_acquire);
        while (current < target && current < kMaxChunks) {
            if (!allocateChunk(deck, current))
                break;
            publishPreparedCount(deck);
            current = deck.bank->preparedChunks.load(std::memory_order_acquire);
        }
        warmAssetChunks(deck);
    }

    bool deckNeedsGrowth(const Deck& deck) const noexcept
    {
        return deck.bank->requestedChunks.load(std::memory_order_acquire)
             > deck.bank->preparedChunks.load(std::memory_order_acquire)
            || (deck.assetBacked.load(std::memory_order_acquire)
                && ((deck.requestedWarmEndChunk.load(std::memory_order_acquire)
                        > deck.requestedWarmStartChunk.load(std::memory_order_acquire))
                    || (deck.requestedWarm2EndChunk.load(std::memory_order_acquire)
                        > deck.requestedWarm2StartChunk.load(std::memory_order_acquire)))
                && !requestedRangeResident(deck));
    }

    int capacitySamples(const Deck& deck) const noexcept
    {
        const int chunks = deck.bank->preparedChunks.load(std::memory_order_acquire);
        return chunks >= kMaxChunks ? std::numeric_limits<int>::max()
                                    : chunks * kChunkSamples;
    }

    bool requestedRangeResident(const Deck& deck) const noexcept
    {
        return chunkRangeResident(deck,
                                  deck.requestedWarmStartChunk.load(std::memory_order_acquire),
                                  deck.requestedWarmEndChunk.load(std::memory_order_acquire))
            && chunkRangeResident(deck,
                                  deck.requestedWarm2StartChunk.load(std::memory_order_acquire),
                                  deck.requestedWarm2EndChunk.load(std::memory_order_acquire));
    }

    bool chunkRangeResident(const Deck& deck, int start, int end) const noexcept
    {
        if (end <= start)
            return true;
        for (int i = start; i < end; ++i) {
            const auto* chunk = chunkAt(deck, i);
            if (chunk == nullptr || !chunk->resident.load(std::memory_order_acquire))
                return false;
        }
        return true;
    }

    void requestPreparedSamples(Deck& deck, int requiredSamples) noexcept
    {
        if (requiredSamples <= capacitySamples(deck))
            return;
        const int requiredChunks = chunksForSamples(requiredSamples);
        int current = deck.bank->requestedChunks.load(std::memory_order_relaxed);
        while (requiredChunks > current
               && !deck.bank->requestedChunks.compare_exchange_weak(current,
                                                                    requiredChunks,
                                                                    std::memory_order_release,
                                                                    std::memory_order_relaxed)) {}
    }

    bool writeStereoFrame(Deck& deck, int frame, float left, float right) noexcept
    {
        if (frame < 0 || frame >= capacitySamples(deck))
            return false;

        const int chunkIndex = frame / kChunkSamples;
        const int offset = frame - chunkIndex * kChunkSamples;
        auto* chunk = chunkAt(deck, chunkIndex);
        if (chunk == nullptr)
            return false;

        chunk->audio.setSample(0, offset, left);
        chunk->audio.setSample(1, offset, right);
        updateChunkVisualBin(deck, *chunk, offset, left, right);
        return true;
    }

    float sampleAt(const Deck& deck, int channel, int frame) const noexcept
    {
        const int samples = deck.capturedSamples.load(std::memory_order_acquire);
        if (frame < 0 || frame >= samples)
            return 0.0f;

        const int chunkIndex = frame / kChunkSamples;
        const int offset = frame - chunkIndex * kChunkSamples;
        const auto* chunk = chunkAt(deck, chunkIndex);
        if (chunk == nullptr)
            return 0.0f;
        if (deck.assetBacked.load(std::memory_order_acquire)
            && !chunk->resident.load(std::memory_order_acquire))
            return 0.0f;

        channel = juce::jlimit(0, 1, channel);
        return chunk->audio.getSample(channel, offset);
    }

    void buildVisualBinsFromReader(Deck& deck, juce::AudioFormatReader& reader, int samples) noexcept
    {
        resetVisualBins(deck);
        if (samples <= 0)
            return;

        constexpr int block = 16384;
        juce::AudioBuffer<float> temp(juce::jmax(1, (int) reader.numChannels), block);
        for (int pos = 0; pos < samples; pos += block) {
            const int n = juce::jmin(block, samples - pos);
            temp.clear();
            if (!reader.read(&temp, 0, n, pos, true, true))
                break;
            for (int i = 0; i < n; ++i) {
                const float left = temp.getSample(0, i);
                const float right = temp.getNumChannels() > 1 ? temp.getSample(1, i) : left;
                updateVisualBin(deck, pos + i, left, right, samples);
            }
        }
        deck.lastVisualRebuildSamples = samples;
        deck.visualRebuildCursor = samples;
        deck.visualRebuilding = false;
        deck.visualCapturedSamples.store(samples, std::memory_order_relaxed);
    }

    void warmAssetChunks(Deck& deck)
    {
        if (!deck.assetBacked.load(std::memory_order_acquire))
            return;

        const int startA = deck.requestedWarmStartChunk.load(std::memory_order_acquire);
        const int endA = deck.requestedWarmEndChunk.load(std::memory_order_acquire);
        const int startB = deck.requestedWarm2StartChunk.load(std::memory_order_acquire);
        const int endB = deck.requestedWarm2EndChunk.load(std::memory_order_acquire);
        if (endA <= startA && endB <= startB)
            return;

        std::lock_guard<std::mutex> ioLock(assetIoMutex_);
        const juce::File file = deck.assetFile;
        const uint32_t generation = deck.assetGeneration.load(std::memory_order_acquire);
        if (!file.existsAsFile())
            return;

        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader(fm.createReaderFor(file));
        if (!reader)
            return;

        const int samples = deck.capturedSamples.load(std::memory_order_acquire);
        const bool mono = reader->numChannels == 1;
        if (!warmAssetChunkRange(deck, *reader, generation, samples, mono, startA, endA))
            return;
        warmAssetChunkRange(deck, *reader, generation, samples, mono, startB, endB);
    }

    bool warmAssetChunkRange(Deck& deck,
                             juce::AudioFormatReader& reader,
                             uint32_t generation,
                             int samples,
                             bool mono,
                             int start,
                             int end)
    {
        for (int chunkIndex = start; chunkIndex < end; ++chunkIndex) {
            if (deck.assetGeneration.load(std::memory_order_acquire) != generation)
                return false;

            auto* chunk = chunkAt(deck, chunkIndex);
            if (chunk == nullptr && !allocateChunk(deck, chunkIndex))
                return false;
            chunk = chunkAt(deck, chunkIndex);
            if (chunk == nullptr)
                return false;
            if (chunk->resident.load(std::memory_order_acquire))
                continue;

            const int frame = chunkIndex * kChunkSamples;
            const int n = juce::jlimit(0, kChunkSamples, samples - frame);
            if (n <= 0)
                continue;

            chunk->audio.clear();
            if (!reader.read(&chunk->audio, 0, n, frame, true, true))
                return false;
            if (mono)
                chunk->audio.copyFrom(1, 0, chunk->audio, 0, 0, n);
            chunk->resident.store(true, std::memory_order_release);
            const int currentResidentEnd = deck.residentEndFrame.load(std::memory_order_acquire);
            deck.residentStartFrame.store(currentResidentEnd <= 0
                                              ? frame
                                              : juce::jmin(deck.residentStartFrame.load(std::memory_order_acquire),
                                                          frame),
                                          std::memory_order_release);
            deck.residentEndFrame.store(juce::jmax(deck.residentEndFrame.load(std::memory_order_acquire),
                                                   frame + n),
                                        std::memory_order_release);
            visualGeneration_.fetch_add(1, std::memory_order_relaxed);
        }
        return true;
    }

    void populateResidencyBins(const Deck& deck, VisualSnapshot& snapshot) const noexcept
    {
        if (!snapshot.assetBacked || snapshot.capturedSamples <= 0)
            return;

        const int samples = snapshot.capturedSamples;
        const int requestedStart = deck.requestedWarmStartChunk.load(std::memory_order_acquire);
        const int requestedEnd = deck.requestedWarmEndChunk.load(std::memory_order_acquire);
        const int requested2Start = deck.requestedWarm2StartChunk.load(std::memory_order_acquire);
        const int requested2End = deck.requestedWarm2EndChunk.load(std::memory_order_acquire);
        for (int bin = 0; bin < kVisualBins; ++bin) {
            const int frameStart = (int) (((int64_t) bin * samples) / kVisualBins);
            const int frameEnd = (int) ((((int64_t) bin + 1) * samples + kVisualBins - 1) / kVisualBins);
            const int startChunk = juce::jlimit(0, kMaxChunks - 1, frameStart / kChunkSamples);
            const int endChunk = juce::jlimit(startChunk, kMaxChunks - 1,
                                              juce::jmax(frameStart, frameEnd - 1) / kChunkSamples);
            bool resident = false;
            bool requested = false;
            for (int chunkIndex = startChunk; chunkIndex <= endChunk; ++chunkIndex) {
                const auto* chunk = chunkAt(deck, chunkIndex);
                resident = resident || (chunk != nullptr && chunk->resident.load(std::memory_order_acquire));
                requested = requested
                    || (chunkIndex >= requestedStart && chunkIndex < requestedEnd)
                    || (chunkIndex >= requested2Start && chunkIndex < requested2End);
            }
            snapshot.resident[(size_t) bin] = resident ? 1.0f : 0.0f;
            snapshot.requested[(size_t) bin] = requested ? 1.0f : 0.0f;
        }
    }

    void retainDeck(int index) const noexcept
    {
        if (validDeckIndex(index))
            decks_[(size_t) index].readers.fetch_add(1, std::memory_order_acq_rel);
    }

    void releaseDeck(int index) const noexcept
    {
        if (validDeckIndex(index))
            decks_[(size_t) index].readers.fetch_sub(1, std::memory_order_acq_rel);
    }

    static float readSampleThunk(const void* owner, int deckIndex, int channel, int frame) noexcept
    {
        const auto* storage = static_cast<const BufferStorage*>(owner);
        if (storage == nullptr || !validDeckIndex(deckIndex))
            return 0.0f;
        return storage->sampleAt(storage->decks_[(size_t) deckIndex], channel, frame);
    }

    static void retainDeckThunk(const void* owner, int deckIndex) noexcept
    {
        if (const auto* storage = static_cast<const BufferStorage*>(owner))
            storage->retainDeck(deckIndex);
    }

    static void releaseDeckThunk(const void* owner, int deckIndex) noexcept
    {
        if (const auto* storage = static_cast<const BufferStorage*>(owner))
            storage->releaseDeck(deckIndex);
    }

    std::array<Deck, kDeckCount> decks_;
    std::array<std::atomic<bool>, kDeckCount> retired_ {};
    std::atomic<int> committedDeckIndex_ { 0 };
    std::atomic<int> stagingDeckIndex_ { 1 };
    std::atomic<bool> stopGrowThread_ { false };
    std::mutex growMutex_;
    std::condition_variable growCv_;
    std::thread growThread_;

    int preparedSampleRate_ = 0;
    std::mutex assetIoMutex_;
    std::atomic<bool> visualRecording_ { false };
    std::atomic<bool> visualPlaying_ { false };
    std::atomic<float> visualPlayhead_ { 0.0f };
    std::atomic<uint32_t> visualGeneration_ { 0 };
    std::atomic<uint32_t> recordingVisualComposeCount_ { 0 };
    std::atomic<uint32_t> promotedGeneration_ { 0 };
    std::atomic<uint32_t> dirtyGeneration_ { 0 };
    std::atomic<bool> dirty_ { false };
};

} // namespace buffer
} // namespace curlop
