#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <cmath>
#include <utility>

namespace curlop::buffer {

enum class InterpolationMode {
    Hold = 0,
    Linear,
    CatmullRom,
    Lagrange4,
    WindowedSinc8,
};

struct BufferView {
    const float* const* channels = nullptr;
    const void* owner = nullptr;
    float (*readSample)(const void*, int, int, int) noexcept = nullptr;
    void (*retainToken)(const void*, int) noexcept = nullptr;
    void (*releaseToken)(const void*, int) noexcept = nullptr;
    int ownerToken = -1;
    int numChannels = 0;
    int numSamples = 0;

    BufferView() = default;

    BufferView(const BufferView& other) noexcept
        : channels(other.channels),
          owner(other.owner),
          readSample(other.readSample),
          retainToken(other.retainToken),
          releaseToken(other.releaseToken),
          ownerToken(other.ownerToken),
          numChannels(other.numChannels),
          numSamples(other.numSamples)
    {
        retain();
    }

    BufferView& operator=(const BufferView& other) noexcept
    {
        if (this == &other)
            return *this;
        release();
        channels = other.channels;
        owner = other.owner;
        readSample = other.readSample;
        retainToken = other.retainToken;
        releaseToken = other.releaseToken;
        ownerToken = other.ownerToken;
        numChannels = other.numChannels;
        numSamples = other.numSamples;
        retain();
        return *this;
    }

    BufferView(BufferView&& other) noexcept
        : channels(std::exchange(other.channels, nullptr)),
          owner(std::exchange(other.owner, nullptr)),
          readSample(std::exchange(other.readSample, nullptr)),
          retainToken(std::exchange(other.retainToken, nullptr)),
          releaseToken(std::exchange(other.releaseToken, nullptr)),
          ownerToken(std::exchange(other.ownerToken, -1)),
          numChannels(std::exchange(other.numChannels, 0)),
          numSamples(std::exchange(other.numSamples, 0))
    {}

    BufferView& operator=(BufferView&& other) noexcept
    {
        if (this == &other)
            return *this;
        release();
        channels = std::exchange(other.channels, nullptr);
        owner = std::exchange(other.owner, nullptr);
        readSample = std::exchange(other.readSample, nullptr);
        retainToken = std::exchange(other.retainToken, nullptr);
        releaseToken = std::exchange(other.releaseToken, nullptr);
        ownerToken = std::exchange(other.ownerToken, -1);
        numChannels = std::exchange(other.numChannels, 0);
        numSamples = std::exchange(other.numSamples, 0);
        return *this;
    }

    ~BufferView()
    {
        release();
    }

    static BufferView from(const juce::AudioBuffer<float>& buffer) noexcept
    {
        BufferView view;
        view.channels = buffer.getArrayOfReadPointers();
        view.numChannels = buffer.getNumChannels();
        view.numSamples = buffer.getNumSamples();
        return view;
    }

    bool valid() const noexcept
    {
        return (channels != nullptr
                || (owner != nullptr && readSample != nullptr && ownerToken >= 0))
            && numChannels > 0
            && numSamples > 0;
    }

    float sample(int channel, int index) const noexcept
    {
        if (readSample != nullptr && owner != nullptr)
            return readSample(owner, ownerToken, channel, index);
        return channels[channel][index];
    }

private:
    void retain() const noexcept
    {
        if (owner != nullptr && retainToken != nullptr && ownerToken >= 0)
            retainToken(owner, ownerToken);
    }

    void release() noexcept
    {
        if (owner != nullptr && releaseToken != nullptr && ownerToken >= 0)
            releaseToken(owner, ownerToken);
        owner = nullptr;
        releaseToken = nullptr;
        retainToken = nullptr;
        readSample = nullptr;
        ownerToken = -1;
    }
};

struct ReadParams {
    float playGate = 0.0f;
    float position = 0.0f; // Normalized inside start..end.
    float rate = 1.0f;     // Source frames per output frame.
    float start = 0.0f;    // Normalized across the whole buffer.
    float end = 1.0f;      // Normalized across the whole buffer.
    bool loop = false;
    InterpolationMode interpolation = InterpolationMode::Linear;
};

struct ReadState {
    double playheadFrame = 0.0;
    bool active = false;
    bool previousGateHigh = false;
    int renderedSamples = 0;
    bool endedThisBlock = false;

    void reset() noexcept
    {
        playheadFrame = 0.0;
        active = false;
        previousGateHigh = false;
        renderedSamples = 0;
        endedThisBlock = false;
    }
};

class BufferReadCursor {
public:
    static void render(const BufferView& source,
                       juce::AudioBuffer<float>& output,
                       ReadState& state,
                       const ReadParams& params,
                       int outputStartSample,
                       int numSamples)
    {
        const auto range = makeRange(source.numSamples, params.start, params.end);
        const bool gateHigh = params.playGate > 0.0f;
        const bool scrubMode = std::abs(params.rate) <= 1.0e-6f;

        if (outputStartSample < 0 || numSamples <= 0 || outputStartSample >= output.getNumSamples())
            return;

        numSamples = juce::jmin(numSamples, output.getNumSamples() - outputStartSample);
        output.clear(outputStartSample, numSamples);
        state.renderedSamples = 0;
        state.endedThisBlock = false;

        if (!source.valid() || output.getNumChannels() <= 0 || !range.valid || !gateHigh) {
            state.active = false;
            state.previousGateHigh = gateHigh;
            return;
        }

        const double targetFrame = positionToFrame(range, params.position);
        if (!state.previousGateHigh) {
            state.playheadFrame = targetFrame;
            state.active = true;
        } else if (!state.active) {
            if (!scrubMode) {
                state.previousGateHigh = gateHigh;
                return;
            }
            state.playheadFrame = targetFrame;
            state.active = true;
        }

        const double scrubStartFrame = state.playheadFrame;

        for (int sample = 0; sample < numSamples; ++sample) {
            if (!state.active)
                break;

            const double frame = scrubMode
                ? scrubFrame(scrubStartFrame, targetFrame, sample, numSamples)
                : state.playheadFrame;

            if (!params.loop && !contains(range, frame)) {
                state.active = false;
                break;
            }

            const double readableFrame = params.loop ? wrap(range, frame) : frame;

            for (int ch = 0; ch < output.getNumChannels(); ++ch) {
                const int sourceChannel = juce::jmin(ch, source.numChannels - 1);
                output.setSample(ch,
                                 outputStartSample + sample,
                                 sampleAt(source, sourceChannel, readableFrame, range, params.loop, params.interpolation));
            }
            state.renderedSamples = sample + 1;

            if (!scrubMode) {
                state.playheadFrame += (double) params.rate;
                if (params.loop)
                    state.playheadFrame = wrap(range, state.playheadFrame);
                else if (!contains(range, state.playheadFrame)) {
                    state.active = false;
                    state.endedThisBlock = true;
                }
            }
        }

        if (scrubMode && state.active)
            state.playheadFrame = targetFrame;

        state.previousGateHigh = gateHigh;
    }

    static float sampleAt(const BufferView& source,
                          int channel,
                          double frame,
                          InterpolationMode interpolation) noexcept
    {
        const auto range = Range { 0, source.numSamples, source.numSamples > 0 };
        return sampleAt(source, channel, frame, range, false, interpolation);
    }

private:
    struct Range {
        int start = 0;
        int end = 0; // Exclusive.
        bool valid = false;

        int length() const noexcept { return end - start; }
    };

    static Range makeRange(int totalSamples, float normalizedStart, float normalizedEnd) noexcept
    {
        if (totalSamples <= 0)
            return {};

        const float start = juce::jlimit(0.0f, 1.0f, normalizedStart);
        const float end = juce::jlimit(0.0f, 1.0f, normalizedEnd);

        const int startFrame = juce::jlimit(0, totalSamples, (int) std::floor(start * (float) totalSamples));
        const int endFrame = juce::jlimit(0, totalSamples, (int) std::ceil(end * (float) totalSamples));
        return { startFrame, endFrame, endFrame > startFrame };
    }

    static double positionToFrame(const Range& range, float normalizedPosition) noexcept
    {
        const double pos = (double) juce::jlimit(0.0f, 1.0f, normalizedPosition);
        if (range.length() <= 1)
            return (double) range.start;
        return (double) range.start + pos * (double) (range.length() - 1);
    }

    static double scrubFrame(double fromFrame, double toFrame, int sample, int numSamples) noexcept
    {
        if (numSamples <= 1)
            return toFrame;
        const double t = (double) sample / (double) (numSamples - 1);
        return fromFrame + (toFrame - fromFrame) * t;
    }

    static bool contains(const Range& range, double frame) noexcept
    {
        return frame >= (double) range.start && frame < (double) range.end;
    }

    static double wrap(const Range& range, double frame) noexcept
    {
        const double length = (double) range.length();
        if (length <= 0.0)
            return (double) range.start;

        while (frame < (double) range.start)
            frame += length;
        while (frame >= (double) range.end)
            frame -= length;
        return frame;
    }

    static float readIndex(const BufferView& source, int channel, int index, const Range& range, bool loop) noexcept
    {
        if (!source.valid())
            return 0.0f;

        channel = juce::jlimit(0, source.numChannels - 1, channel);

        if (loop)
            index = (int) wrap(range, (double) index);
        else
            index = juce::jlimit(range.start, range.end - 1, index);

        return source.sample(channel, index);
    }

    static float sampleAt(const BufferView& source,
                          int channel,
                          double frame,
                          const Range& range,
                          bool loop,
                          InterpolationMode interpolation) noexcept
    {
        if (!source.valid() || !range.valid)
            return 0.0f;

        if (loop)
            frame = wrap(range, frame);

        const int base = (int) std::floor(frame);
        const double frac = frame - (double) base;

        switch (interpolation) {
            case InterpolationMode::Hold:
                return readIndex(source, channel, base, range, loop);

            case InterpolationMode::Linear: {
                const float a = readIndex(source, channel, base, range, loop);
                const float b = readIndex(source, channel, base + 1, range, loop);
                return a + (float) frac * (b - a);
            }

            case InterpolationMode::CatmullRom: {
                const float y0 = readIndex(source, channel, base - 1, range, loop);
                const float y1 = readIndex(source, channel, base, range, loop);
                const float y2 = readIndex(source, channel, base + 1, range, loop);
                const float y3 = readIndex(source, channel, base + 2, range, loop);
                const double x = frac;
                return (float) (y1 + 0.5 * x * ((double) y2 - y0
                    + x * (2.0 * y0 - 5.0 * y1 + 4.0 * y2 - y3
                    + x * (3.0 * ((double) y1 - y2) + y3 - y0))));
            }

            case InterpolationMode::Lagrange4: {
                const double x = frac;
                const float s0 = readIndex(source, channel, base - 1, range, loop);
                const float s1 = readIndex(source, channel, base, range, loop);
                const float s2 = readIndex(source, channel, base + 1, range, loop);
                const float s3 = readIndex(source, channel, base + 2, range, loop);
                const double w0 = (-x * (x - 1.0) * (x - 2.0)) / 6.0;
                const double w1 = ((x + 1.0) * (x - 1.0) * (x - 2.0)) / 2.0;
                const double w2 = (-(x + 1.0) * x * (x - 2.0)) / 2.0;
                const double w3 = ((x + 1.0) * x * (x - 1.0)) / 6.0;
                return (float) (w0 * s0 + w1 * s1 + w2 * s2 + w3 * s3);
            }

            case InterpolationMode::WindowedSinc8:
                return windowedSinc8(source, channel, base, frac, range, loop);
        }

        return 0.0f;
    }

    static double sinc(double x) noexcept
    {
        constexpr double pi = 3.14159265358979323846264338327950288;
        if (std::abs(x) < 1.0e-8)
            return 1.0;
        const double pix = pi * x;
        return std::sin(pix) / pix;
    }

    static float windowedSinc8(const BufferView& source,
                               int channel,
                               int base,
                               double frac,
                               const Range& range,
                               bool loop) noexcept
    {
        constexpr double pi = 3.14159265358979323846264338327950288;
        constexpr double radius = 4.0;
        double sum = 0.0;
        double weightSum = 0.0;

        for (int tap = -3; tap <= 4; ++tap) {
            const double distance = (double) tap - frac;
            const double absDistance = std::abs(distance);
            if (absDistance >= radius)
                continue;

            const double window = 0.5 + 0.5 * std::cos(pi * distance / radius);
            const double weight = sinc(distance) * window;
            sum += (double) readIndex(source, channel, base + tap, range, loop) * weight;
            weightSum += weight;
        }

        return weightSum != 0.0 ? (float) (sum / weightSum) : 0.0f;
    }
};

} // namespace curlop::buffer
