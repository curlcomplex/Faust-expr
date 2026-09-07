#!/usr/bin/env python3
"""Processing-liveness follow-up to the existing lifecycle probe, not a benchmark.
The DSP and host driver are read from the research branch, not replaced.
Runtime changes and generated source are archived. Historical failures remain.
"""
from pathlib import Path
from array import array
import hashlib
import json
import math
import os
import re
import shutil
import signal
import subprocess
import sys
import time


def once(s, old, new):
    if s.count(old) != 1:
        raise RuntimeError(f'expected one occurrence ({s.count(old)}): {old[:90]!r}')
    return s.replace(old, new, 1)


DIAG_FIELDS = r'''
        // Diagnostic only: one independent trace/watchdog per scheduler instance.
        enum { DIAG_MAX_THREADS = 64 };
        std::atomic<unsigned long long> dGeneration{0}, dBlock{0};
        std::atomic<unsigned long long> dCalls[DIAG_MAX_THREADS]{};
        std::atomic<unsigned long long> dEmpty[DIAG_MAX_THREADS]{};
        std::atomic<unsigned long long> dIssued[DIAG_MAX_THREADS]{};
        std::atomic<int> dTask[DIAG_MAX_THREADS]{}, dApi[DIAG_MAX_THREADS]{};
        std::atomic<bool> dStop{false}, dActive{false};
        pthread_t dWatch{};
        bool dWatchStarted = false;
        int dTaskLimit = 0;

        void DResult(int thread, int task, int api)
        {
            if (thread < 0 || thread >= DIAG_MAX_THREADS) abort();
            dApi[thread].store(api, std::memory_order_relaxed);
            dTask[thread].store(task, std::memory_order_relaxed);
            if (task < 0 || task >= dTaskLimit) {
                fprintf(stderr, "diag_invalid_task thread=%d task=%d api=%d\n", thread, task, api);
                DDump("invalid_task");
                abort();
            }
            if (task) dIssued[thread].fetch_add(1, std::memory_order_relaxed);
        }
        void DDump(const char* why)
        {
            fprintf(stderr, "diag_snapshot reason=%s scheduler=%p block=%llu generation=%llu active=%d participants=%d\n",
                    why, (void*)this, dBlock.load(), dGeneration.load(), int(dActive.load()), fDynamicNumThreads);
            for (int i = 0; i < fDynamicNumThreads && i < DIAG_MAX_THREADS; ++i) {
                fprintf(stderr, "diag_worker id=%d calls=%llu empty=%llu issued=%llu task=%d api=%d\n", i,
                        dCalls[i].load(), dEmpty[i].load(), dIssued[i].load(), dTask[i].load(), dApi[i].load());
                fTaskQueueList[i].DDump(i);
            }
            fTaskGraph->DDump();
            fflush(stderr);
        }
        static void* DWatchMain(void* opaque)
        {
            auto* self = static_cast<WorkStealingScheduler*>(opaque);
            unsigned long long last = 0;
            int stationary = 0;
            while (!self->dStop.load()) {
                usleep(100000);
                auto now = self->dGeneration.load();
                if (self->dActive.load() && now == last) ++stationary;
                else stationary = 0;
                last = now;
                if (stationary == 20 || stationary == 40) self->DDump("no_generation_progress");
            }
            return nullptr;
        }
'''


def instrument_runtime(s, barrier=False, trace=True):
    # The caller first applies the EXISTING ARM/graceful patch.
    s = '#include <atomic>\n' + once(s, 'volatile bool fRunning;', 'std::atomic<bool> fRunning;')
    if barrier:
        s = once(s, 'volatile int fCurThreadCount;', 'std::atomic<int> fCurThreadCount;')
        s = once(s, 'DEC_ATOMIC(&fCurThreadCount);', 'fCurThreadCount.fetch_sub(1);')
        s = once(s, '//fThreadPool->SignalOne();', 'if (fRunning) fThreadPool->SignalOne();')
        s = once(s, '//while (!fThreadPool->IsFinished()) {}', 'while (!fThreadPool->IsFinished()) {}')
    # Explicitly validate creation instead of treating a requested thread as alive.
    s = once(s, 'fThreadPool[i]->Start(realtime);', '''int rc = fThreadPool[i]->Start(realtime);
            fprintf(stderr, "diag_thread_create id=%d rc=%d\\n", i + 1, rc);
            if (rc != 0) abort();''')
    if not trace:
        return s
    s = once(s, 'class TaskQueue \n{', '''class TaskQueue
{''')
    marker = '        INLINE void InitOne()'
    s = once(s, marker, '''        // Best-effort volatile queue snapshot, NOT a coherent concurrent proof.
        void DDump(int id) {
            AtomicCounter c; c = fCounter;
            fprintf(stderr, "diag_queue id=%d head=%d tail=%d", id, int(Head(c)), int(Tail(c)));
            for (int i = 0; i < fTaskQueueSize; ++i)
                if (fTaskList[i] != -1) fprintf(stderr, " %d:%d", i, fTaskList[i]);
            fprintf(stderr, "\\n");
        }
''' + marker)
    s = once(s, '        INLINE void InitTask(int task, int val)', '''        void DDump() {
            fprintf(stderr, "diag_dependencies");
            for (int i = 0; i < fTaskQueueSize; ++i)
                if (fTaskList[i] != 0) fprintf(stderr, " %d:%d", i, int(fTaskList[i]));
            fprintf(stderr, "\\n");
        }
        INLINE void InitTask(int task, int val)''')
    s = once(s, '        int fReadyTaskListIndex;', '        int fReadyTaskListIndex;\n' + DIAG_FIELDS)
    s = once(s, '            fStaticNumThreads = get_max_cpu();', '''            dTaskLimit = task_queue_size;
            fStaticNumThreads = get_max_cpu();''')
    s = once(s, '            fReadyTaskListIndex = 0;', '''            fReadyTaskListIndex = 0;
            if (fDynamicNumThreads < 1 || fDynamicNumThreads > fStaticNumThreads || fDynamicNumThreads > DIAG_MAX_THREADS) abort();''')
    s = once(s, '            delete fThreadPool;', '''            dActive = false;
            dStop = true;
            if (dWatchStarted && pthread_join(dWatch, nullptr) != 0) abort();
            DDump("destroy");
            delete fThreadPool;''')
    s = once(s, '            fThreadPool->StartAll(fStaticNumThreads - 1, true, dsp);', '''            fThreadPool->StartAll(fStaticNumThreads - 1, true, dsp);
            int rc = pthread_create(&dWatch, nullptr, DWatchMain, this);
            fprintf(stderr, "diag_watchdog_create rc=%d computeThreadExternal=%p\\n", rc, (void*)&computeThreadExternal);
            if (rc != 0) abort();
            dWatchStarted = true;''')
    s = once(s, '            GetRealTime();', '''            dBlock.fetch_add(1, std::memory_order_relaxed);
            dActive.store(true, std::memory_order_relaxed);
            GetRealTime();''')
    s = once(s, '            fDynThreadAdapter.StopMeasure(fStaticNumThreads, fDynamicNumThreads);', '''            fDynThreadAdapter.StopMeasure(fStaticNumThreads, fDynamicNumThreads);
            dActive.store(false, std::memory_order_relaxed);''')
    s = once(s, '            fTaskQueueList[cur_thread].PushHead(task_num);', '''            DResult(cur_thread, task_num, 1);
            fTaskQueueList[cur_thread].PushHead(task_num);''')
    s = once(s, '            return TaskQueue::GetNextTask(fTaskQueueList, cur_thread, fDynamicNumThreads);', '''            dApi[cur_thread].store(2, std::memory_order_relaxed);
            dCalls[cur_thread].fetch_add(1, std::memory_order_relaxed);
            int task = TaskQueue::GetNextTask(fTaskQueueList, cur_thread, fDynamicNumThreads);
            if (task == 0) dEmpty[cur_thread].fetch_add(1, std::memory_order_relaxed);
            DResult(cur_thread, task, 3);
            return task;''')
    s = once(s, '            fTaskGraph->ActivateOutputTask(fTaskQueueList[cur_thread], task, task_num);', '''            dApi[cur_thread].store(4, std::memory_order_relaxed);
            fTaskGraph->ActivateOutputTask(fTaskQueueList[cur_thread], task, task_num);
            DResult(cur_thread, *task_num, 5);''')
    s = once(s, '            fTaskGraph->ActivateOneOutputTask(fTaskQueueList[cur_thread], task, task_num);', '''            dApi[cur_thread].store(6, std::memory_order_relaxed);
            fTaskGraph->ActivateOneOutputTask(fTaskQueueList[cur_thread], task, task_num);
            DResult(cur_thread, *task_num, 7);''')
    s = once(s, '            fTaskGraph->GetReadyTask(fTaskQueueList[cur_thread], task_num);', '''            fTaskGraph->GetReadyTask(fTaskQueueList[cur_thread], task_num);
            DResult(cur_thread, *task_num, 8);''')
    s = once(s, '            TaskQueue::InitAll(fTaskQueueList, fDynamicNumThreads);', '''            dGeneration.fetch_add(1, std::memory_order_relaxed);
            TaskQueue::InitAll(fTaskQueueList, fDynamicNumThreads);''')
    # Keep an observable boundary and print its address for JIT-PC attribution.
    s = once(s, 'EXPORT int getNextTask(void* scheduler, int cur_thread)',
             'EXPORT __attribute__((noinline)) int getNextTask(void* scheduler, int cur_thread)')
    s += '\n// End of diagnostic instrumentation; no task partition changes.\n'
    return s


def main():
    root = Path(__file__).resolve().parents[1]
    build = root / 'build/liveness'; build.mkdir(parents=True, exist_ok=True)
    evidence = root / 'evidence-liveness'; evidence.mkdir(parents=True, exist_ok=True)
    commands, results, comparisons = [], [], []
    def sha(p): return hashlib.sha256(Path(p).read_bytes()).hexdigest()
    def checked(cmd, name, timeout=120):
        cmd = list(map(str, cmd)); commands.append({'name': name, 'argv': cmd})
        r = subprocess.run(cmd, cwd=root, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           text=True, errors='replace', timeout=timeout)
        (evidence / (name + '.txt')).write_text(r.stdout)
        if r.returncode: raise RuntimeError(f'{name} exit={r.returncode}: {r.stdout[-6000:]}')
        return r.stdout.strip()
    def run(cmd, name, env, timeout=20):
        cmd = list(map(str, cmd)); commands.append({'name': name, 'argv': cmd,
            'env': {k: v for k, v in env.items() if k.startswith(('OMP_', 'FAUST_', 'DIAG_'))}})
        path = evidence / (name + '.txt'); timedout = False
        with path.open('w') as f:
            p = subprocess.Popen(cmd, cwd=root, env=env, stdout=f, stderr=subprocess.STDOUT, start_new_session=True)
            try: p.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                timedout = True
                # Inspect the ORIGINAL hung process; a replay can miss a race.
                for tool, suffix in [(['/usr/bin/sample', str(p.pid), '1'], '-sample'),
                    (['lldb', '--batch', '--no-lldbinit', '-p', str(p.pid),
                      '-o', 'thread backtrace all', '-o', 'register read',
                      '-o', 'disassemble --start-address `$pc-32` --count 32',
                      '-o', 'image list', '-o', 'process detach'], '-lldb-attach')]:
                    try:
                        with (evidence / (name + suffix + '.txt')).open('w') as d:
                            subprocess.run(tool, stdout=d, stderr=subprocess.STDOUT, timeout=12)
                    except (OSError, subprocess.TimeoutExpired): pass
                try: os.killpg(p.pid, signal.SIGKILL)
                except ProcessLookupError: pass
                p.wait()
        text = path.read_text(errors='replace')
        row = {'name': name, 'exit_code': p.returncode, 'timeout': timedout,
               'clean_exit': p.returncode == 0 and 'stage=main_return' in text,
               'invalid_task': 'diag_invalid_task' in text,
               'created_workers': len(re.findall(r'diag_thread_create id=\d+ rc=0', text)),
               'last_block': (re.findall(r'diag_host_block=(\d+)', text) or [None])[-1]}
        results.append(row); print(json.dumps(row), flush=True)
        if not row['clean_exit']: print(text[-14000:], flush=True)
        return row
    def compare(a, b, name):
        row = {'name': name, 'pass': False, 'tolerance': 1e-7}
        if a.exists() and b.exists():
            x, y = array('f'), array('f'); x.frombytes(a.read_bytes()); y.frombytes(b.read_bytes())
            if len(x) == len(y) and x and all(map(math.isfinite, x)) and all(map(math.isfinite, y)):
                row.update(samples=len(x), max_abs_error=max(abs(i-j) for i,j in zip(x,y)),
                           bit_identical=a.read_bytes() == b.read_bytes())
                row['pass'] = row['max_abs_error'] <= row['tolerance']
        comparisons.append(row); print(json.dumps(row), flush=True)
    try:
        inc = checked(['faust', '--includedir'], 'include')
        lib = checked(['faust', '--libdir'], 'lib')
        prefix = checked(['brew', '--prefix', 'faust'], 'faust-prefix')
        llvm = checked(['brew', '--prefix', 'llvm@22'], 'llvm-prefix')
        version = checked(['faust', '-v'], 'faust-version')
        if 'FAUST Version 2.85.9' not in version: raise RuntimeError('unexpected Faust version')
        clang = Path(llvm) / 'bin/clang++'
        checked([clang, '--version'], 'clang-version')
        checked(['sysctl', 'hw.physicalcpu', 'hw.logicalcpu', 'machdep.cpu.brand_string'], 'cpu')
        checked(['uname', '-a'], 'os')
        checked(['otool', '-L', Path(lib)/'libfaust.dylib'], 'dependencies')
        upstream = Path(prefix) / 'share/faust/scheduler.cpp'
        shutil.copy2(upstream, evidence / 'upstream-scheduler.cpp')
        old_probe = root / 'scripts/scheduler_bitcode_probe.cpp'
        dsp = re.search(r'kSource\s*=\s*R"FAUST\((.*?)\)FAUST"', old_probe.read_text(), re.S).group(1)
        source = build/'probe.dsp'; source.write_text(dsp); shutil.copy2(source, evidence/source.name)
        flags = [str(clang), '-std=c++17', '-O2', '-g', '-fno-omit-frame-pointer', '-I'+inc]
        libs = ['-L'+lib, '-Wl,-rpath,'+lib, '-lfaust', '-lpthread', '-lz']
        target_src = build/'target.cpp'
        target_src.write_text('#include <faust/dsp/llvm-dsp.h>\n#include <iostream>\nint main(){std::cout << getDSPMachineTarget();}\n')
        checked(flags+[target_src]+libs+['-o', build/'target'], 'build-target')
        target = checked([build/'target'], 'target'); triple = target.split(':')[0]
        driver = (root/'scripts/lifecycle_diag.cpp').read_text()
        driver = once(driver, 'constexpr int frames = 256, blocks = 200;', '''constexpr int frames = 256;
    const int blocks = std::getenv("DIAG_BLOCKS") ? std::atoi(std::getenv("DIAG_BLOCKS")) : 200;
    if (blocks < 1 || blocks > 20000) throw std::runtime_error("invalid DIAG_BLOCKS");''')
        driver = once(driver, '        instance->compute(frames, nullptr, outputs);', '''        if ((b % 128) == 0) std::cout << "diag_host_block=" << b << '\\n';
        instance->compute(frames, nullptr, outputs);''')
        driver_path = build/'driver.cpp'; driver_path.write_text(driver)
        shutil.copy2(driver_path, evidence/'driver.cpp')
        host = build/'jit'; checked(flags+[driver_path]+libs+['-o',host], 'build-jit')
        env = os.environ.copy()
        for k in list(env):
            if k.startswith(('OMP_', 'FAUST_')): env.pop(k)
        env.update(OMP_NUM_THREADS='2', OMP_DYN_THREAD='0', FAUST_JIT_TARGET=target, DIAG_BLOCKS='1000')
        arch = build/'arch.cpp'; arch.write_text('<<includeIntrinsic>>\n<<includeclass>>\n')
        (evidence/'provenance.json').write_text(json.dumps({
            'sha': checked(['git','rev-parse','HEAD'],'research-sha'), 'dsp_sha256':sha(source),
            'upstream_sha256':sha(upstream), 'original_driver_sha256':sha(root/'scripts/lifecycle_diag.cpp'),
            'scope':'single-instance liveness diagnostic of existing probe, not performance or production'},indent=2))
        for variant, barrier, trace in [('trace',False,True), ('trace_barrier',True,True), ('untraced',False,False)]:
            folder = build/variant; folder.mkdir(exist_ok=True)
            rt = folder/'scheduler.cpp'; shutil.copy2(upstream,rt)
            checked([sys.executable, root/'scripts/patch_scheduler_arm64.py',rt], 'old-patch-'+variant)
            rt.write_text(instrument_runtime(rt.read_text(), barrier, trace))
            shutil.copy2(rt,evidence/('scheduler-'+variant+'.cpp'))
            ll=folder/'scheduler.ll'
            checked(flags+['-target',triple,'-S','-emit-llvm',rt,'-o',ll], 'build-ir-'+variant)
            shutil.copy2(ll,evidence/('scheduler-'+variant+'.ll'))
            header=folder/'diag_dsp.h'
            checked(['faust','-lang','cpp','-cn','ProbeDSP','-a',arch,'-sch',source,'-o',header], 'generate-native-'+variant)
            raw=header.read_text(); shutil.copy2(header,evidence/('native-original-'+variant+'.h'))
            native=once(raw,upstream.read_text(),rt.read_text())
            native=once(native,'virtual ~ProbeDSP() = default;','// Prior diagnostic: remove only duplicate default destructor.')
            header.write_text(native); shutil.copy2(header,evidence/('native-'+variant+'.h'))
            exe=folder/'native'; checked(flags+['-DDIAG_AOT','-I'+str(folder),driver_path,'-lpthread','-o',exe], 'build-native-'+variant)
            bc=evidence/(variant+'.bc.txt'); ref=evidence/(variant+'-source.f32')
            write_env=env.copy(); write_env['FAUST_SCHEDULER_MODULE']=str(ll)
            run([host,'sch','source','explicit',source,bc,ref],variant+'-write',write_env)
            # Preserve full linked IR for generated-code inspection without rerunning Faust.
            import base64
            bc_raw=folder/'factory.bc'; bc_raw.write_bytes(base64.b64decode(bc.read_text()))
            checked([Path(llvm)/'bin/llvm-dis',bc_raw,'-o',evidence/(variant+'-factory.ll')], 'disassemble-bitcode-'+variant)
            hidden=ll.with_suffix('.hidden'); ll.rename(hidden)
            try:
                failures=0
                for rep in range(30 if trace else 10):
                    audio=evidence/(variant+f'-read-{rep}.f32')
                    r=run([host,'sch','read','explicit',source,bc,audio], variant+f'-read-{rep}', env)
                    compare(ref,audio,variant+f'-read-{rep}-audio')
                    native_audio=evidence/(variant+f'-native-{rep}.f32')
                    n=run([exe,'sch','source','explicit',source,'-',native_audio],variant+f'-native-{rep}',env)
                    if rep: compare(evidence/(variant+'-native-0.f32'),native_audio,variant+f'-native-{rep}-audio')
                    if not r['clean_exit'] or not n['clean_exit']: failures+=1
                    # Retain multiple examples without wasting CI on endless hangs.
                    if failures>=2: break
            finally: hidden.rename(ll)
    except Exception as e:
        results.append({'name':'harness','clean_exit':False,'error':repr(e)})
        print('harness_error='+repr(e),flush=True)
    finally:
        summary={'results':results,'audio_comparisons':comparisons,
            'warning':'Liveness diagnostics. Trace/watchdog alter scheduling; no performance claim or production acceptance.'}
        (evidence/'summary.json').write_text(json.dumps(summary,indent=2))
        (evidence/'commands.json').write_text(json.dumps(commands,indent=2))
        print('LIVENESS_SUMMARY='+json.dumps(summary),flush=True)
    return 0 if results and all(r['clean_exit'] for r in results) and all(c['pass'] for c in comparisons) else 1


if __name__ == '__main__':
    if len(sys.argv)>1 and sys.argv[1]=='--patch-only':
        inp,out=map(Path,sys.argv[2:4]); out.write_text(instrument_runtime(inp.read_text(),False,True))
    else: raise SystemExit(main())
