#pragma once
#include <time.h>
// Diagnostic only; no changes to the existing Tracktion scheduler or DSP.
namespace job_probe {
inline bool enabled=false; // set before preparing the player; immutable during playback
inline double cpuUs() noexcept {
    timespec value{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID,&value)!=0) return -1;
    return double(value.tv_sec)*1e6+double(value.tv_nsec)*1e-3;
}
struct Timing {double begin=0,end=0,cpu=0;};
}
