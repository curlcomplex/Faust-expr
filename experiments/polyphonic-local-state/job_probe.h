#pragma once
#include <time.h>
#include <atomic>
#include <thread>
#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#endif
namespace job_probe {
// Startup/teardown counters only; the render callback just reads them.
inline std::atomic<int> joinedWorkgroupWorkers{0};
inline std::atomic<int> failedWorkgroupJoins{0};
struct WorkgroupJoinObservation {
    bool joined;
    WorkgroupJoinObservation(bool requested,bool success) noexcept : joined(success) {
        if(joined) joinedWorkgroupWorkers.fetch_add(1,std::memory_order_relaxed);
        else if(requested) failedWorkgroupJoins.fetch_add(1,std::memory_order_relaxed);
    }
    ~WorkgroupJoinObservation(){if(joined)joinedWorkgroupWorkers.fetch_sub(1,std::memory_order_relaxed);}
};
inline bool enabled=false;
inline std::thread::id owner;
inline double cpuUs() noexcept {
    timespec value{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID,&value)!=0) return -1;
    return double(value.tv_sec)*1e6+double(value.tv_nsec)*1e-3;
}
struct Policy {
    int result=-1,defaultPolicy=1;
    unsigned period=0,computation=0,constraint=0;
    bool realtime()const noexcept{return result==0&&defaultPolicy==0;}
};
inline Policy policy() noexcept {
    Policy result;
#if defined(__APPLE__)
    thread_time_constraint_policy_data_t values{};
    mach_msg_type_number_t count=THREAD_TIME_CONSTRAINT_POLICY_COUNT;
    boolean_t defaults=false;
    result.result=thread_policy_get(pthread_mach_thread_np(pthread_self()),THREAD_TIME_CONSTRAINT_POLICY,reinterpret_cast<thread_policy_t>(&values),&count,&defaults);
    result.defaultPolicy=defaults;result.period=values.period;result.computation=values.computation;result.constraint=values.constraint;
#endif
    return result;
}
// Policy reads happen once per thread in the diagnostic arm's initial block,
// not throughout the timed edit. Both A/B arms use the identical observer.
struct Timing {double begin=0,end=0,cpu=0;Policy scheduling;bool caller=false;};
}
