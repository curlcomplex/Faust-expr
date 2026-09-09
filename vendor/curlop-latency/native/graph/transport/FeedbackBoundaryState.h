#pragma once

#include "graph/transport/SignalDescriptor.h"

#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <vector>

namespace curlop::transport {

// Off-thread-prepared state for one exact z^-1 graph boundary. It is deliberately
// frame-based: an eventual local SCC executor reads the complete prior frame,
// runs its causal processors, then commits the complete current frame. No
// callback allocation, graph mutation, or channel reinterpretation occurs here.
class FeedbackBoundaryState final {
public:
    explicit FeedbackBoundaryState(SignalDescriptor descriptor)
        : descriptor_(std::move(descriptor))
        , priorFrame_(descriptor_.width(), 0.0f)
    {
        if (descriptor_.width() == 0u)
            throw std::invalid_argument("feedback boundary requires channels");
        if (descriptor_.rate() != SignalRate::Audio
            && descriptor_.rate() != SignalRate::FullRateControl)
            throw std::invalid_argument(
                "feedback boundary requires audio or full-rate control");
    }

    FeedbackBoundaryState(const FeedbackBoundaryState&) = delete;
    FeedbackBoundaryState& operator=(const FeedbackBoundaryState&) = delete;

    const SignalDescriptor& descriptor() const noexcept { return descriptor_; }
    SignalWidth width() const noexcept { return descriptor_.width(); }

    void reset() noexcept
    {
        std::fill(priorFrame_.begin(), priorFrame_.end(), 0.0f);
    }

    bool readFrame(float* destination, SignalWidth channels) const noexcept
    {
        if (destination == nullptr || channels != width())
            return false;
        std::copy(priorFrame_.begin(), priorFrame_.end(), destination);
        return true;
    }

    bool commitFrame(const float* source, SignalWidth channels) noexcept
    {
        if (source == nullptr || channels != width())
            return false;
        std::copy(source, source + priorFrame_.size(), priorFrame_.begin());
        return true;
    }

private:
    const SignalDescriptor descriptor_;
    std::vector<float> priorFrame_;
};

} // namespace curlop::transport
