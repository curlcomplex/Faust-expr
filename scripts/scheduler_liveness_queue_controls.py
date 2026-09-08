#!/usr/bin/env python3
"""Controlled queue-publication changes to the EXISTING liveness matrix.

fence: hardware-ordering diagnostic only, not a C++ memory-model repair.
atomic: keep the existing packed head/tail algorithm, use atomic accesses of
        one width for the counter and release/acquire task publication.
No generated Faust DSP/task partitioning is changed. Not production acceptance.
"""
from pathlib import Path
import json
import os
import sys
import run_scheduler_liveness as base


def queue_control(s, mode):
    if mode == 'baseline': return s
    if mode == 'fence':
        return base.once(s, '            IncHead(fCounter);', '''            // Diagnostic only: does ordering publication change the failure?
            __atomic_thread_fence(__ATOMIC_RELEASE);
            IncHead(fCounter);''')
    if mode != 'atomic': raise RuntimeError('unknown queue mode: '+mode)
    start = s.index('class TaskQueue')
    end = s.index('class TaskGraph', start)
    q = s[start:end]
    q = base.once(q, 'volatile AtomicCounter fCounter;', 'std::atomic<int> fCounter{0};')
    q = base.once(q, 'fCounter.info.fValue = 0;', 'fCounter.store(0, std::memory_order_release);')
    q = base.once(q, '''            fTaskList[Head(fCounter)] = item;
            IncHead(fCounter);''', '''            AtomicCounter before;
            before.info.fValue = fCounter.load(std::memory_order_relaxed);
            const int head = Head(before);
            if (head < 0 || head >= fTaskQueueSize || head >= 32767) abort();
            fTaskList[head] = item;
            // One producer per queue. The full-word RMW preserves a thief's
            // concurrent tail increment and publishes the slot contents.
            fCounter.fetch_add(1, std::memory_order_release);''')
    if q.count('old_val = fCounter;') != 2: raise RuntimeError('unexpected queue loads')
    q = q.replace('old_val = fCounter;', 'old_val.info.fValue = fCounter.load(std::memory_order_acquire);')
    old='} while (!CAS1(&fCounter, Value(old_val), Value(new_val)));'
    new='''} while (!fCounter.compare_exchange_strong(old_val.info.fValue,
                     new_val.info.fValue, std::memory_order_acq_rel,
                     std::memory_order_acquire));'''
    if q.count(old) != 2: raise RuntimeError('unexpected queue CAS count')
    q = q.replace(old,new)
    if 'AtomicCounter c; c = fCounter;' in q:
        q = base.once(q, 'AtomicCounter c; c = fCounter;',
                     'AtomicCounter c; c.info.fValue = fCounter.load(std::memory_order_relaxed);')
    return s[:start] + q + s[end:]


def main():
    mode=os.environ.get('QUEUE_CONTROL','baseline')
    if mode not in ('baseline','fence','atomic'): raise RuntimeError('invalid QUEUE_CONTROL')
    original=base.instrument_runtime
    def adapted(s, barrier=False, trace=True):
        return queue_control(original(s,barrier,trace),mode)
    base.instrument_runtime=adapted
    print('queue_control='+mode,flush=True)
    rc=base.main()
    folder=Path(base.__file__).resolve().parents[1]/'evidence-liveness'
    (folder/'queue-control.json').write_text(json.dumps({
        'mode':mode,'generated_dsp_modified':False,
        'warning':'fence is an ordering diagnostic; atomic is a candidate, not a validated RT queue',
        'basis':'same source, lifecycle driver, native adaptation and bitcode roundtrip as preceding liveness probe'
    },indent=2))
    return rc

if __name__=='__main__': raise SystemExit(main())
