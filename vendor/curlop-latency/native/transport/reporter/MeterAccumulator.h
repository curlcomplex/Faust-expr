#pragma once
#include <juce_core/juce_core.h>
#include <array>
#include <atomic>
#include <string>
#include <unordered_map>

class MeterAccumulator {
public:
    static constexpr int kScopeTraceBins = 64;
    static constexpr int kSpectrumBins = 32;
    static constexpr int kSpectrogramFrames = 24;
    static constexpr int kGoniometerPoints = 64;
    enum PresentationStream : uint32_t
    {
        scopeStream = 1u << 0,
        spectrumStream = 1u << 1,
        spectrogramStream = 1u << 2,
        goniometerStream = 1u << 3,
        allPresentationStreams = scopeStream | spectrumStream
            | spectrogramStream | goniometerStream
    };

    // The mounted Web scene reports which analysis renderers are visible.
    // Default to every stream until that first report so startup never loses
    // presentation data. The audio thread reads this mask without blocking.
    void setPresentationStreamMask (uint32_t mask) noexcept
    {
        presentationStreamMask_.store (mask & allPresentationStreams,
                                       std::memory_order_release);
    }

    void exchangePeak(const float* readL, const float* readR, int numSamples);
    void feedMasterScope(int clipId, const float* readL, const float* readR, int numSamples);

    // clipId-scoped — callers pass the slot's clipId so JS can route meter events
    // by viewed clip. Same key in different clips does not clobber (view/play
    // separation at the meter layer). Negative clipId is legal (sentinel) and
    // keyed as its own bucket.
    void feedPeak(int clipId, const std::string& source, float peak);
    void feedRange(int clipId, const std::string& source, float min, float max);
    void feedRange(int clipId, const std::string& source, float min, float max, float value);
    void feedLevel(int clipId, const std::string& source, float min, float max, float rms);

    juce::String serializeAndClear();
    juce::String serializeScopeTraceAndClear();
    juce::String serializeSpectrumAndClear();
    juce::String serializeSpectrogramAndClear();
    juce::String serializeGoniometerAndClear();

    float exchangePeakL() { return outputPeakL_.exchange(0.0f, std::memory_order_relaxed); }
    float exchangePeakR() { return outputPeakR_.exchange(0.0f, std::memory_order_relaxed); }

    // FF-002 / s344 — non-destructive peek for additional readers (native
    // transport bar meters). Does NOT reset the atomic, so the existing
    // exchangePeakL/R consumer (CurlopProcessor JSON snapshot fan-out)
    // continues to drain at its own cadence without losing samples.
    float peekPeakL() const noexcept { return outputPeakL_.load(std::memory_order_relaxed); }
    float peekPeakR() const noexcept { return outputPeakR_.load(std::memory_order_relaxed); }

private:
    std::atomic<float> outputPeakL_{0.0f};
    std::atomic<float> outputPeakR_{0.0f};
    std::array<std::atomic<float>, kScopeTraceBins> masterScopeBins_{};
    std::array<std::atomic<float>, kGoniometerPoints> masterGoniometerX_{};
    std::array<std::atomic<float>, kGoniometerPoints> masterGoniometerY_{};
    std::atomic<int> masterScopeClipId_{-1};
    std::atomic<bool> masterScopeReady_{false};
    std::atomic<bool> masterSpectrumReady_{false};
    std::atomic<bool> masterSpectrogramReady_{false};
    std::atomic<bool> masterGoniometerReady_{false};
    std::atomic<uint32_t> presentationStreamMask_ { allPresentationStreams };
    std::atomic<uint64_t> masterSpectrogramSequence_ { 0 };
    std::atomic<uint64_t> masterSpectrogramDropped_ { 0 };

    struct MeterKey {
        int clipId;
        std::string source;
        bool operator==(const MeterKey& o) const noexcept {
            return clipId == o.clipId && source == o.source;
        }
    };
    struct MeterKeyHash {
        size_t operator()(const MeterKey& k) const noexcept {
            return std::hash<std::string>{}(k.source) ^ (static_cast<size_t>(k.clipId) * 0x9E3779B97F4A7C15ull);
        }
    };
    struct MeterPeak
    {
        double min = 0;
        double max = 0;
        double rms = 0;
        double value = 0;
        bool seen = false;
        bool rmsSeen = false;
        bool valueSeen = false;
    };
    void feedRangeInternal(int clipId,
                           const std::string& source,
                           float min,
                           float max,
                           bool hasValue,
                           float value);
    std::unordered_map<MeterKey, MeterPeak, MeterKeyHash> meterPeaks_;
};
