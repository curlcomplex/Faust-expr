#!/usr/bin/env python3
"""Bounded A/B repair of the pinned lightweightSemHybrid startup only.
Does not replace the scheduler, DSP, JUCE, or workgroup implementation.
"""
import hashlib,json,sys
from pathlib import Path
PIN='489942b7960de68411fdee3cb17fdaea668848b9'
def blob(text):
    data=text.encode();return hashlib.sha1(b'blob '+str(len(data)).encode()+b'\0'+data).hexdigest()
OLD='''        for (size_t i = 0; i < numThreads; ++i)
        {
            threads.emplace_back ([this] { runThread(); });
            setThreadPriority (threads.back(), 10);
            tryToUpgradeCurrentThreadToRealtime (rtOpts);
        }'''
PREVIOUS='''        // PR45_WORKER_CONTEXT: original arm retained in the same binary.
        const auto* context = std::getenv ("PS_RT_CONTEXT");
        const bool workerContext = context != nullptr && std::string (context) == "workers";
        for (size_t i = 0; i < numThreads; ++i)
        {
            if (workerContext)
            {
                // Synchronize startup BEFORE realtime work. The parent must
                // finish setting pthread priority before the worker installs
                // its Mach time-constraint policy, or it could overwrite it.
                std::promise<void> priorityInstalled;
                auto ready = priorityInstalled.get_future();
                threads.emplace_back ([this, rtOpts, ready = std::move (ready)] () mutable
                {
                    ready.wait();
                    tryToUpgradeCurrentThreadToRealtime (rtOpts);
                    runThread();
                });
                setThreadPriority (threads.back(), 10);
                // Preserve the original caller policy in BOTH comparison arms.
                tryToUpgradeCurrentThreadToRealtime (rtOpts);
                priorityInstalled.set_value();
            }
            else
            {
                threads.emplace_back ([this] { runThread(); });
                setThreadPriority (threads.back(), 10);
                tryToUpgradeCurrentThreadToRealtime (rtOpts);
            }
        }'''
NEW=PREVIOUS.replace(
    '        for (size_t i = 0; i < numThreads; ++i)',
    '''        // Worker budget is a separate experimental axis. Keep creator policy
        // fixed to the previous control to isolate worker-only effects.
        const auto* budget = std::getenv ("PS_RT_BUDGET");
        const bool matched = budget != nullptr && std::string (budget) != "startup";
        auto workerOptions = rtOpts;
        if (budget != nullptr && std::string (budget) == "periodic")
        {
            const double periodMs = 1000.0 * player.getBlockSize() / player.getSampleRate();
            workerOptions = workerOptions.withPeriodMs (periodMs)
                                         .withProcessingTimeMs (periodMs * 0.5)
                                         .withMaximumProcessingTimeMs (periodMs);
        }
        const auto creatorOptions = matched
            ? juce::Thread::RealtimeOptions().withPriority (10)
                    .withApproximateAudioProcessingTime (512, 44100.0)
            : rtOpts;
        for (size_t i = 0; i < numThreads; ++i)''', 1)
NEW=NEW.replace('[this, rtOpts, ready', '[this, workerOptions, ready')
NEW=NEW.replace('                    tryToUpgradeCurrentThreadToRealtime (rtOpts);',
                '                    tryToUpgradeCurrentThreadToRealtime (workerOptions);')
NEW=NEW.replace('tryToUpgradeCurrentThreadToRealtime (rtOpts);',
                'tryToUpgradeCurrentThreadToRealtime (creatorOptions);')
INCLUDE='#include <future> // PR45_WORKER_CONTEXT\n#include <cstdlib> // PR45_WORKER_CONTEXT\n'
def main(path):
    text=path.read_text()
    if 'PR45_WORKER_CONTEXT' in text:
        known=NEW if text.count(NEW)==1 else PREVIOUS
        assert text.count(known)==1 and text.count(INCLUDE)==1,'unrecognized prior pool patch'
        text=text.replace(known,OLD).replace(INCLUDE,'')
    assert blob(text)==PIN,'pinned Tracktion source changed; refusing to patch'
    index=text.index('struct ThreadPoolSemHybrid')
    prefix,suffix=text[:index],text[index:]
    assert suffix.count(OLD)==1
    patched=INCLUDE+prefix+suffix.replace(OLD,NEW)
    path.write_text(patched)
    record={'pin':'4536d8a21664fe6ec2aa34b25abc87fa2a0d3b86','original_blob':PIN,'patched_blob':blob(patched),'patched_sha256':hashlib.sha256(patched.encode()).hexdigest()}
    path.with_suffix('.pr45.json').write_text(json.dumps(record,indent=2)+'\n')
    print('WORKER_CONTEXT_PATCH',json.dumps(record),flush=True)
if __name__=='__main__':main(Path(sys.argv[1]))
