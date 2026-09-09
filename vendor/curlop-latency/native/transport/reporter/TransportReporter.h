// CURLOP Transport Reporter
//
// Reads VMState after each processBlock and stores transport info
// in atomics for the main thread to read and send to WebView.
//
// The main thread timer reads getState() at ~30Hz and sends
// TRANSPORT_UPDATE messages to the WebView for UI display
// (beat position indicator, loop counter, etc.).
//
// Also detects loop boundaries by comparing loop_count across blocks.
#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>

namespace curlop {

class TransportReporter {
public:
    struct TransportState {
        double   beatPosition;
        uint32_t loopCount;
        double   bpm;
        bool     playing;
        int64_t  samplePosition;
        int      activeStep;     // SF-059 T-421: top-level sounding step (-1 = none)
    };

    // Called from audio thread after each block (F-066 Phase 5: scalar
    // transport state from the slot clock — the VMState type is gone).
    void update(double beatPosition, uint32_t loopCountNow, int activeStep,
                double bpm, bool playing, int64_t samplePositionNow)
    {
        const double safeBeatPosition = std::isfinite(beatPosition) ? beatPosition : 0.0;
        const double safeBpm = (std::isfinite(bpm) && bpm > 0.0) ? bpm : 120.0;
        beatPosition_.store(safeBeatPosition, std::memory_order_relaxed);
        bpm_.store(safeBpm, std::memory_order_relaxed);
        playing_.store(playing, std::memory_order_relaxed);
        activeStep_.store(activeStep, std::memory_order_relaxed);
        samplePosition_.store(samplePositionNow, std::memory_order_relaxed);

        // Detect loop boundary: loop_count increased since last block
        uint32_t prevLoopCount = loopCount_.load(std::memory_order_relaxed);
        if (loopCountNow > prevLoopCount && playing)
        {
            loopWrapped_.store(true, std::memory_order_release);
        }
        loopCount_.store(loopCountNow, std::memory_order_relaxed);
    }

    // Called from main thread to get current transport state.
    TransportState getState() const
    {
        return {
            beatPosition_.load(std::memory_order_relaxed),
            loopCount_.load(std::memory_order_relaxed),
            bpm_.load(std::memory_order_relaxed),
            playing_.load(std::memory_order_relaxed),
            samplePosition_.load(std::memory_order_relaxed),
            activeStep_.load(std::memory_order_relaxed)
        };
    }

    // Check and clear the loop-wrapped flag. Called from main thread.
    // Returns true if a loop boundary was crossed since last check.
    bool checkAndClearLoopWrap()
    {
        return loopWrapped_.exchange(false, std::memory_order_acq_rel);
    }

    // Reset all state (on transport stop or new program load).
    void reset()
    {
        beatPosition_.store(0.0, std::memory_order_relaxed);
        loopCount_.store(0, std::memory_order_relaxed);
        bpm_.store(120.0, std::memory_order_relaxed);
        playing_.store(false, std::memory_order_relaxed);
        samplePosition_.store(0, std::memory_order_relaxed);
        loopWrapped_.store(false, std::memory_order_relaxed);
        activeStep_.store(-1, std::memory_order_relaxed);
    }

private:
    std::atomic<double>   beatPosition_{0.0};
    std::atomic<uint32_t> loopCount_{0};
    std::atomic<double>   bpm_{120.0};
    std::atomic<bool>     playing_{false};
    std::atomic<int64_t>  samplePosition_{0};
    std::atomic<bool>     loopWrapped_{false};
    std::atomic<int>      activeStep_{-1};   // SF-059 T-421 Follow-Playhead
};

} // namespace curlop
