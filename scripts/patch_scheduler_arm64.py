#!/usr/bin/env python3
from pathlib import Path
import sys

if len(sys.argv) != 2:
    raise SystemExit("usage: patch_scheduler_arm64.py <scheduler.cpp>")

p = Path(sys.argv[1])
s = p.read_text()

def replace_once(old: str, new: str, label: str):
    global s
    if old not in s:
        raise SystemExit(f"scheduler source did not match expected {label}")
    s = s.replace(old, new, 1)

replace_once(
    '#define EXPORT inline __attribute__ ((visibility("default"))) __attribute__((always_inline))',
    '#define EXPORT __attribute__ ((visibility("default")))',
    'EXPORT macro')

replace_once(
'''static INLINE UInt64 DSP_rdtsc(void)
{
\tunion {
\t\tUInt32 i32[2];
\t\tUInt64 i64;
\t} count;
\t
\t__asm__ __volatile__("rdtsc" : "=a" (count.i32[0]), "=d" (count.i32[1]));
    return count.i64;
}''',
'''static INLINE UInt64 DSP_rdtsc(void)
{
#if defined(__aarch64__) || defined(__arm64__)
    UInt64 count = 0;
    __asm__ __volatile__("mrs %0, cntvct_el0" : "=r" (count));
    return count;
#else
    union {
        UInt32 i32[2];
        UInt64 i64;
    } count;
    __asm__ __volatile__("rdtsc" : "=a" (count.i32[0]), "=d" (count.i32[1]));
    return count.i64;
#endif
}''',
    'DSP_rdtsc body')

replace_once(
'''#endif

/*
static INLINE int INC_ATOMIC''',
'''#endif

#if defined(__aarch64__) || defined(__arm64__)
static INLINE void NOP(void) {}
static INLINE char CAS1(volatile void* addr, volatile int value, int newvalue)
{
    return __sync_bool_compare_and_swap((int*)addr, value, newvalue);
}
static INLINE int atomic_xadd(volatile int* atomic, int val)
{
    return __sync_add_and_fetch(atomic, val);
}
#endif

/*
static INLINE int INC_ATOMIC''',
    'ARM atomic helper insertion point')

# Upstream DSPThread has no graceful lifetime flag. On macOS it kills the Mach
# thread and immediately frees the DSPThread/semaphore object. Add a normal
# wake/exit/join path so DSP destruction has a real happens-before boundary.
replace_once(
'''        int fNumThread;
        void* fDSP;
        
        static void* ThreadHandler(void* arg)''',
'''        int fNumThread;
        void* fDSP;
        volatile bool fRunning;
        
        static void* ThreadHandler(void* arg)''',
    'DSPThread fields')

replace_once(
'''            while (true) {
                thread->Run();
            }''',
'''            while (thread->fRunning) {
                thread->Run();
            }''',
    'worker loop')

replace_once(
'''        DSPThread(int num_thread, DSPThreadPool* pool, void* dsp)
            :fThreadPool(pool), fSemaphore(0), fRealTime(false), fNumThread(num_thread), fDSP(dsp) 
        {}''',
'''        DSPThread(int num_thread, DSPThreadPool* pool, void* dsp)
            :fThreadPool(pool), fSemaphore(0), fRealTime(false), fNumThread(num_thread), fDSP(dsp), fRunning(true)
        {}''',
    'DSPThread constructor')

replace_once(
'''        void Run()
        {
            fSemaphore.wait();
            computeThreadExternal(fDSP, fNumThread + 1);
            //fThreadPool->SignalOne();
        }''',
'''        void Run()
        {
            fSemaphore.wait();
            if (fRunning) {
                computeThreadExternal(fDSP, fNumThread + 1);
            }
            //fThreadPool->SignalOne();
        }''',
    'DSPThread Run')

replace_once(
'''        void Stop()
        {
            CancelThread(fThread);
        }''',
'''        void Stop()
        {
            fRunning = false;
            fSemaphore.post();
            pthread_join(fThread, NULL);
        }''',
    'DSPThread Stop')

p.write_text(s)
