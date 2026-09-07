#!/usr/bin/env python3
"""Controlled follow-up to the EXISTING scheduler/bitcode probe.

No Curlop code is imported. No performance claims are made. Each child must
return normally with exit 0; a marker printed before aborting is not a pass.
Intentional retain controls are recorded separately from acceptance checks.
"""
from array import array
from pathlib import Path
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

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / 'build/lifecycle'
EVIDENCE = ROOT / 'evidence-lifecycle'
BUILD.mkdir(parents=True, exist_ok=True)
EVIDENCE.mkdir(parents=True, exist_ok=True)
REPEATS = 2
results = []
comparisons = []
commands = []
roundtrips = []


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def checked(cmd, name, timeout=90, env=None):
    cmd = [str(x) for x in cmd]
    commands.append({'name': name, 'argv': cmd})
    r = subprocess.run(cmd, cwd=ROOT, env=env, text=True, errors='replace',
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout)
    (EVIDENCE / (name + '.txt')).write_text(r.stdout)
    if r.returncode:
        print(r.stdout[-10000:], flush=True)
        raise RuntimeError(f'{name}: exit {r.returncode}')
    return r.stdout.strip()


def base_env():
    e = os.environ.copy()
    for key in ('FAUST_SCHEDULER_MODULE', 'FAUST_JIT_TARGET', 'OMP_NUM_THREADS',
                'OMP_REALTIME', 'OMP_DYN_THREAD'):
        e.pop(key, None)
    e['OMP_NUM_THREADS'] = '2'
    e['OMP_DYN_THREAD'] = '0'
    return e


def run_child(name, cmd, child_env, debug=True, timeout=15):
    cmd = [str(x) for x in cmd]
    commands.append({'name': name, 'argv': cmd,
                     'env': {k: child_env[k] for k in ('OMP_NUM_THREADS', 'OMP_DYN_THREAD',
                             'FAUST_SCHEDULER_MODULE') if k in child_env}})
    path = EVIDENCE / (name + '.txt')
    timed_out = False
    started = time.monotonic()
    with path.open('w') as f:
        p = subprocess.Popen(cmd, cwd=ROOT, env=child_env, stdout=f,
                             stderr=subprocess.STDOUT, start_new_session=True)
        try:
            p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            timed_out = True
            # Collect the LIVE blocked stacks before killing the hung child.
            if sys.platform == 'darwin':
                try:
                    subprocess.run(['/usr/bin/sample', str(p.pid), '1', '-file',
                                    str(EVIDENCE / (name + '-sample.txt'))],
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=8)
                except (OSError, subprocess.TimeoutExpired):
                    pass
            try:
                os.killpg(p.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            p.wait()
    text = path.read_text(errors='replace')
    stages = re.findall(r'^stage=(.*)$', text, re.M)
    completed = p.returncode == 0 and not timed_out and 'main_return' in stages
    row = {'name': name, 'exit_code': p.returncode, 'timeout': timed_out,
           'clean_exit': completed, 'last_stage': stages[-1] if stages else None,
           'wall_seconds_diagnostic_only': time.monotonic() - started}
    results.append(row)
    print(json.dumps(row), flush=True)
    if not completed:
        print('\n'.join(text.splitlines()[-28:]), flush=True)
    if debug and not completed and not timed_out:
        # Replays use DIFFERENT output files, so they cannot overwrite evidence
        # produced by the original child or silently change a reader's input.
        lldb = shutil.which('lldb')
        if lldb:
            replay = cmd.copy()
            replay[-1] += '.lldb'
            if replay[2] == 'source' and replay[-2] != '-': replay[-2] += '.lldb'
            dcmd = [lldb, '--batch', '--no-lldbinit',
                    '-o', 'settings set target.disable-aslr false', '-o', 'run',
                    '-k', 'thread backtrace all', '-k', 'image list', '--'] + replay
            commands.append({'name': name + '-lldb', 'argv': dcmd})
            dp = EVIDENCE / (name + '-lldb.txt')
            with dp.open('w') as df:
                d = subprocess.Popen(dcmd, env=child_env, cwd=ROOT, stdout=df,
                                     stderr=subprocess.STDOUT, start_new_session=True)
                try:
                    d.wait(timeout=25)
                except subprocess.TimeoutExpired:
                    try: os.killpg(d.pid, signal.SIGKILL)
                    except ProcessLookupError: pass
                    d.wait()
            print('debug_replay=' + dp.name, flush=True)
    return row


def compare(a, b, label, atol):
    row = {'name': label, 'absolute_tolerance': atol, 'pass': False}
    if not a.exists() or not b.exists():
        row['error'] = 'missing audio capture'
    else:
        aa = array('f'); aa.frombytes(a.read_bytes())
        bb = array('f'); bb.frombytes(b.read_bytes())
        if len(aa) != 102400 or len(aa) != len(bb):
            row['error'] = 'unexpected capture size'
        elif not all(math.isfinite(x) for x in aa) or not all(math.isfinite(x) for x in bb):
            row['error'] = 'nonfinite samples'
        else:
            row['samples'] = len(aa)
            row['max_abs_error'] = max(abs(x-y) for x, y in zip(aa, bb))
            row['rms_error'] = math.sqrt(sum((x-y)**2 for x, y in zip(aa, bb))/len(aa))
            row['bit_identical'] = a.read_bytes() == b.read_bytes()
            row['pass'] = row['max_abs_error'] <= atol
    comparisons.append(row)
    print(json.dumps(row), flush=True)
    return row


def main():
    inc = checked(['faust', '--includedir'], 'includedir')
    lib = checked(['faust', '--libdir'], 'libdir')
    prefix = checked(['brew', '--prefix', 'faust'], 'faust-prefix')
    llvm = checked(['brew', '--prefix', 'llvm@22'], 'llvm-prefix')
    clang = Path(llvm) / 'bin/clang++'
    flags = [str(clang), '-std=c++17', '-O2', '-g', '-fno-omit-frame-pointer', '-I' + inc]
    libs = ['-L' + lib, '-Wl,-rpath,' + lib, '-lfaust', '-lpthread', '-lz']
    version = checked(['faust', '-v'], 'faust-version')
    if 'FAUST Version 2.85.9' not in version:
        raise RuntimeError('expected the previously tested Faust 2.85.9; record/version migration first')
    clang_version = checked([clang, '--version'], 'clang-version')
    original = Path(prefix) / 'share/faust/scheduler.cpp'
    shutil.copy2(original, EVIDENCE / 'upstream-scheduler.cpp')
    checked(['otool', '-L', Path(lib) / 'libfaust.dylib'], 'libfaust-dependencies')
    checked(['uname', '-a'], 'uname')
    checked(['sysctl', 'hw.physicalcpu', 'hw.logicalcpu', 'machdep.cpu.brand_string'], 'cpu')
    original_probe = ROOT / 'scripts/scheduler_bitcode_probe.cpp'
    match = re.search(r'kSource\s*=\s*R"FAUST\((.*?)\)FAUST"', original_probe.read_text(), re.S)
    if not match:
        raise RuntimeError('existing probe DSP could not be extracted')
    source = BUILD / 'probe.dsp'
    source.write_text(match.group(1))
    shutil.copy2(source, EVIDENCE / 'probe.dsp')
    (EVIDENCE / 'provenance.json').write_text(json.dumps({
        'research_sha': checked(['git', 'rev-parse', 'HEAD'], 'research-sha'),
        'original_probe_sha256': sha(original_probe), 'dsp_sha256': sha(source),
        'upstream_scheduler_sha256': sha(original), 'faust': version,
        'clang': clang_version, 'scope': 'existing standalone ABI/lifecycle probe, not production Curlop',
        'threads_requested': 2, 'sample_rate': 48000, 'block_size': 256,
        'blocks': 200, 'repetitions': REPEATS}, indent=2))
    host = BUILD / 'jit_diag'
    checked(flags + [ROOT / 'scripts/lifecycle_diag.cpp'] + libs + ['-o', host], 'build-jit')
    target_src = BUILD / 'target.cpp'
    target_src.write_text('#include <faust/dsp/llvm-dsp.h>\n#include <iostream>\nint main(){std::cout<<getDSPMachineTarget();}\n')
    checked(flags + [target_src] + libs + ['-o', BUILD / 'target'], 'build-target')
    target = checked([BUILD / 'target'], 'jit-target')
    triple = target.split(':')[0]
    env = base_env()
    env['FAUST_JIT_TARGET'] = target
    baseline_audio = None

    def jit_case(variant, shape, rep, cleanup='explicit', scheduler=None):
        nonlocal baseline_audio
        stem = f'jit-{variant}-{cleanup}-{rep}'
        bc = EVIDENCE / (stem + '.bc.txt')
        audio = EVIDENCE / (stem + '.f32')
        e = env.copy()
        if scheduler: e['FAUST_SCHEDULER_MODULE'] = str(scheduler)
        write = run_child(stem, [host, shape, 'source', cleanup, source, bc, audio], e,
                          debug=(rep == 0))
        if variant == 'scalar' and cleanup == 'explicit' and rep == 0:
            baseline_audio = audio
        if cleanup != 'explicit': return
        reader = stem + '-read'
        read_audio = EVIDENCE / (reader + '.f32')
        # The original scheduler IR is physically absent during reload, not
        # merely omitted from the environment. Restore it for the next case.
        hidden = scheduler.with_suffix('.unavailable') if scheduler else None
        if scheduler: scheduler.rename(hidden)
        try:
            reloaded = run_child(reader, [host, shape, 'read', 'explicit', source, bc, read_audio], env,
                                 debug=(rep == 0))
        finally:
            if scheduler: hidden.rename(scheduler)
        match = compare(audio, read_audio, stem + '-bitcode-audio', 1e-7)
        gate = {'variant': variant, 'rep': rep, 'scheduler_ir_absent_during_reload': bool(scheduler),
                'pass': write['clean_exit'] and reloaded['clean_exit'] and match['pass']}
        roundtrips.append(gate)
        print('roundtrip_gate=' + json.dumps(gate), flush=True)
        if baseline_audio and variant != 'scalar':
            compare(baseline_audio, audio, stem + '-vs-scalar', 5e-5)

    for rep in range(REPEATS): jit_case('scalar', 'scalar', rep)
    jit_case('scalar', 'scalar', 0, 'retain')

    # Faust's bare-class output does not embed scheduler.cpp. An architecture
    # wrapper triggers that embedding. -A selects the very same runtime file
    # compiled to IR above; the marker below guards against selecting upstream.
    arch = BUILD / 'diag_arch.cpp'
    arch.write_text('<<includeIntrinsic>>\n<<includeclass>>\n')

    def aot_case(variant, shape, directory, repeat):
        output = directory / 'diag_dsp.h'
        args = ['faust', '-lang', 'cpp', '-cn', 'ProbeDSP', '-a', arch]
        if shape == 'sch': args += ['-sch', '-A', directory]
        checked(args + [source, '-o', output], f'generate-cpp-{variant}')
        # Preserve generated code even when the runtime selection guard fails.
        shutil.copy2(output, EVIDENCE / f'generated-{variant}.h')
        if shape == 'sch' and f'CURLOP_DIAG_RUNTIME_{variant}' not in output.read_text():
            raise RuntimeError('Faust C++ did not embed the selected scheduler copy')
        exe = directory / 'aot_diag'
        checked(flags + ['-DDIAG_AOT', '-I' + str(directory), ROOT / 'scripts/lifecycle_diag.cpp',
                         '-lpthread', '-o', exe], f'build-cpp-{variant}')
        for rep in range(repeat):
            stem = f'cpp-{variant}-{rep}'
            audio = EVIDENCE / (stem + '.f32')
            run_child(stem, [exe, shape, 'source', 'explicit', source, '-', audio], env,
                      debug=(rep == 0))
            compare(baseline_audio, audio, stem + '-vs-jit-scalar', 5e-5)

    scalar_dir = BUILD / 'scalar'; scalar_dir.mkdir(exist_ok=True)
    try: aot_case('scalar', 'scalar', scalar_dir, REPEATS)
    except Exception as e:
        results.append({'name': 'build-cpp-scalar', 'clean_exit': False, 'error': str(e)})
        print(str(e), flush=True)

    patch = ROOT / 'scripts/patch_scheduler_arm64.py'
    portability = BUILD / 'portability_only.py'
    text = patch.read_text()
    if '# Upstream DSPThread' not in text: raise RuntimeError('unexpected existing patch layout')
    portability.write_text(text.split('# Upstream DSPThread')[0] + '\np.write_text(s)\n')

    for variant in ('arm_portable', 'graceful_prior', 'graceful_atomic'):
        directory = BUILD / variant; directory.mkdir(exist_ok=True)
        runtime = directory / 'scheduler.cpp'
        shutil.copy2(original, runtime)
        try:
            checked([sys.executable, portability if variant == 'arm_portable' else patch, runtime],
                    f'patch-{variant}')
            s = runtime.read_text()
            if variant == 'graceful_atomic':
                if s.count('volatile bool fRunning;') != 1:
                    raise RuntimeError('unexpected graceful flag')
                # Only this variable differs from the previous graceful patch.
                s = '#include <atomic>\n' + s.replace('volatile bool fRunning;', 'std::atomic<bool> fRunning;')
            runtime.write_text(f'// CURLOP_DIAG_RUNTIME_{variant}\n' + s)
            shutil.copy2(runtime, EVIDENCE / f'scheduler-{variant}.cpp')
            ll = directory / 'scheduler.ll'
            checked(flags + ['-target', triple, '-S', '-emit-llvm', runtime, '-o', ll],
                    f'build-ir-{variant}')
            shutil.copy2(ll, EVIDENCE / f'scheduler-{variant}.ll')
            print(f'runtime={variant} sha256={sha(runtime)}', flush=True)
            count = 1 if variant == 'arm_portable' else REPEATS
            for rep in range(count): jit_case(variant, 'sch', rep, scheduler=ll)
            if variant == 'graceful_atomic': jit_case(variant, 'sch', 0, 'retain', ll)
            aot_case(variant, 'sch', directory, count)
        except Exception as e:
            results.append({'name': 'build-' + variant, 'clean_exit': False, 'error': str(e)})
            print('build_error=' + str(e), flush=True)


try:
    main()
except Exception as exc:
    results.append({'name': 'harness', 'clean_exit': False, 'error': repr(exc)})
    print('harness_error=' + repr(exc), flush=True)
finally:
    summary = {'results': results, 'audio_comparisons': comparisons, 'roundtrip_gates': roundtrips,
               'warning': 'Compatibility diagnostics only. No realtime or multicore speedup acceptance.'}
    (EVIDENCE / 'summary.json').write_text(json.dumps(summary, indent=2))
    (EVIDENCE / 'commands.json').write_text(json.dumps(commands, indent=2))
    print('LIFECYCLE_SUMMARY_BEGIN\n' + json.dumps(summary, indent=2) + '\nLIFECYCLE_SUMMARY_END', flush=True)
    report = ['## Lifecycle diagnostic observations', '',
              '| Case | Clean exit | Last stage / build error |', '|---|---|---|']
    for r in results:
        report.append(f"| {r['name']} | {r['clean_exit']} | {r.get('last_stage', r.get('error', ''))} |")
    report += ['', 'A retained-factory negative control is not an acceptance check.',
               'Strict scalar roundtrip tolerance is retained even if it fails; see exact errors.',
               'Full sample comparisons and backtraces are in the evidence artifact.']
    if os.environ.get('GITHUB_STEP_SUMMARY'):
        with open(os.environ['GITHUB_STEP_SUMMARY'], 'a') as f: f.write('\n'.join(report) + '\n')
# A red diagnostic workflow is useful evidence; never hide failures with || true.
raise SystemExit(0 if results and all(r['clean_exit'] for r in results) and
                 all(c['pass'] for c in comparisons) else 1)
