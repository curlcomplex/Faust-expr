#pragma once

#include "control/vm/params/ParamTypes.h"

#include <vector>
// T-453 (F-062): choc resolves from the native/choc submodule. The retired
// alternate SDK and its bundled choc are no longer linked, so the old one-copy-only ODR
// constraint is satisfied by the submodule being the only choc in the binary.
#include "choc/containers/choc_SingleReaderSingleWriterFIFO.h"
#include <cstdio>

namespace curlop {

class ParamFIFO {
public:
    explicit ParamFIFO(int capacity = 4096) {
        queue_.reset(static_cast<size_t>(capacity));
    }

    bool push(const FIFOEvent& event) {
        FIFOEvent copy = event;
        bool ok = queue_.push(std::move(copy));
        if (!ok) {
            fprintf(stderr, "[ParamFIFO] FIFO full - dropped event type=%d\n",
                    static_cast<int>(event.type));
        }
        return ok;
    }

    template<typename Callback>
    int drain(Callback&& cb) {
        int count = 0;
        FIFOEvent event;
        while (queue_.pop(event)) {
            cb(event);
            ++count;
        }
        return count;
    }

private:
    choc::fifo::SingleReaderSingleWriterFIFO<FIFOEvent> queue_;
};

} // namespace curlop
