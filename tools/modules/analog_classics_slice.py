"""Actual-Faust single-note hats and external-clock sequencer qualification (#55).
Reuses the existing renderer/build helper. No substitute audio synthesizer.
"""
from pathlib import Path
import argparse
import json
import os
import subprocess
import traceback
import numpy as np
from scipy.io import wavfile
from hats_v2_delivery import Lab, command, digest, DEFAULT as OLD_DEFAULT

ROOT = Path(__file__).resolve().parents[2]
HATS = ROOT / 'modules/hats-analog/single-note-v1'
SEQ = ROOT / 'modules/trigger-seq/v1'
HAT = dict(metal=.96, tone=.60, decay=.46, shape=.30, drive=.12, freq=440., velocity=1., gate=0.)
OPEN = HAT | dict(choke=.78, chokeGate=0.)
STEPS = {f'step{i+1:02d}': 0. for i in range(32)}
SEQ_DEFAULT = dict(run=1., length=16.) | STEPS


def controls(exe):
    lines = command([exe, '--controls']).splitlines()
    io = tuple(map(int, lines[0].split('\t')[1:]))
    values = {a[0]: tuple(map(float, a[1:])) for a in (s.split('\t') for s in lines[1:])}
    return io, values


def trace(inputs, values, events):
    """Independent integer reference model; returns expected control signals only."""
    changes = {}
    for n, k, v in events:
        changes.setdefault(n, {})[k] = v
    p = SEQ_DEFAULT | values
    result = np.zeros((len(inputs), 2), dtype=np.float32)
    position = None
    previous_clock = previous_reset = False
    for n, (clock, reset) in enumerate(inputs):
        p.update(changes.get(n, {}))
        current_clock, current_reset = clock > 0, reset > 0
        tick = current_clock and not previous_clock and p['run'] > 0
        if current_reset and not previous_reset:
            position = None
        if tick:
            length = max(1, min(32, int(p['length'])))
            position = 0 if position is None else (position + 1) % length
            result[n, 0] = p[f'step{position+1:02d}']
        result[n, 1] = 0 if position is None else position + 1
        previous_clock, previous_reset = current_clock, current_reset
    return result


def run(out):
    lab = Lab(out)
    lab.report.update(version='analog-classics-0.1.0-experiment', human_approved=False,
                      host_integrated=False, test_kind='standalone actual Faust, not host acceptance')
    check = lab.check
    output = lab.out

    def render(name, exe, values, events=(), sr=48000, block=128, frames=48000, inputs=None):
        rows = {(0, k): float(v) for k, v in values.items()}
        seen = set()
        for n, k, v in events:
            if (n, k) in seen:
                raise ValueError('duplicate test event')
            seen.add((n, k))
            rows[n, k] = float(v)
        score, raw = output / (name + '.tsv'), output / (name + '.f32')
        score.write_text(''.join(f'{n}\t{k}\t{v:.9g}\n' for (n, k), v in sorted(rows.items())))
        args = [exe, score, raw, sr, block, frames, 0]
        record = dict(name=name, rate=sr, block=block, score_sha256=digest(score))
        if inputs is not None:
            data = np.asarray(inputs, dtype='<f4')
            if data.shape != (frames, 2):
                raise ValueError('clock/reset input shape')
            input_path = output / (name + '-input.f32')
            data.tofile(input_path)
            record['input_sha256'] = digest(input_path)
            args.append(input_path)
        diag = json.loads(command(args))
        x = np.fromfile(raw, '<f4').reshape(-1, diag['channels'])
        check(name + ':finite', len(x) == frames and np.isfinite(x).all(), peak=float(abs(x).max()))
        record.update(raw_sha256=digest(raw), diagnostics=diag)
        lab.report['renders'].append(record)
        return x

    def hat(name, exe, values=None, events=None, **kw):
        defaults = OPEN if exe in (oh, oh_vec) else HAT
        return render(name, exe, defaults | (values or {}),
                      [(480, 'gate', 1), (481, 'gate', 0)] if events is None else events, **kw)[:, 0]

    try:
        ch = lab.build('closed-scalar', HATS / 'closed.dsp')
        oh = lab.build('open-scalar', HATS / 'open.dsp')
        ch_vec = lab.build('closed-vector', HATS / 'closed.dsp', True)
        oh_vec = lab.build('open-vector', HATS / 'open.dsp', True)
        baseline = lab.build('baseline-separated', ROOT / 'modules/hats-analog/v2/voices.dsp')
        # Comparison-only baseline adapter: same Hz conversion, unchanged v2 kernel.
        # This avoids comparing two differently rounded oscillator frequencies.
        baseline_dir = ROOT / 'modules/hats-analog/v2'
        adapted = output / 'baseline-hz-source'
        adapted.mkdir(exist_ok=True)
        old_source = (baseline_dir/'engine.lib').read_text()
        anchor = 'pitch = hslider("pitch_ratio",1,.6,1.7,.001);'
        check('baseline-adapter-one-control-only', old_source.count(anchor) == 1)
        mapped_control = 'hslider("freq",440,264,748,.01)/440.0'
        (adapted/'engine.lib').write_text(old_source.replace(anchor, 'pitch = '+mapped_control+';'))
        (adapted/'voices.dsp').write_text((baseline_dir/'voices.dsp').read_text())
        baseline_hz = lab.build('baseline-hz', adapted/'voices.dsp')
        (adapted/'ratio.dsp').write_text('process = '+mapped_control+';\n')
        ratio_probe = lab.build('hz-ratio-probe', adapted/'ratio.dsp')
        lab.report['baseline_adapter'] = dict(original_engine_sha256=digest(baseline_dir/'engine.lib'),
            adapted_engine_sha256=digest(adapted/'engine.lib'), only_change='pitch input UI and Hz/440 conversion')
        seq = lab.build('seq-signal', SEQ / 'audio-clock-test.dsp')
        seq_vec = lab.build('seq-vector', SEQ / 'audio-clock-test.dsp', True)
        seq_ui = lab.build('seq-ui', SEQ / 'trigger.dsp')
        check('baseline-would-fail-new-note-label-contract', 'freq' not in controls(baseline)[1])
        for label, exe, expected in [('closed', ch, HAT), ('open', oh, OPEN)]:
            io, ui = controls(exe)
            check(label + ':one-output-no-internal-note-bank', io == (0, 1))
            check(label + ':canonical-controls', set(ui) == set(expected))
            check(label + ':freq-hz-bounds', ui['freq'][:3] == (264., 748., 440.))
            check(label + ':defaults', all(abs(ui[k][2] - v) < 1e-6 for k, v in expected.items()))
        check('seq-real-clock-io', controls(seq)[0] == (2, 2))
        check('seq-ui-clock-io', controls(seq_ui)[0] == (0, 2))
        check('seq-ui-control-set', set(controls(seq_ui)[1]) == set(SEQ_DEFAULT) | {'clock', 'reset', 'gate', 'step'})

        # Preserve original-literal comparisons as evidence, not bit-identity claims.
        # Unit accuracy and waveform preservation at matched actual inputs are distinct.
        deltas = []
        original_deltas = []
        lab.report['pitch_conversion'] = []
        for hz in (264., 440., 748.):
            actual = float(render('pitch-map-'+str(int(hz)), ratio_probe, {'freq': hz}, frames=1)[0, 0])
            ideal = hz/440.0
            relative_error = abs(actual/ideal - 1)
            check('pitch-map-float-accuracy-'+str(int(hz)), relative_error <= 2*np.finfo(np.float32).eps,
                  actual_ratio=actual, mathematical_ratio=ideal)
            lab.report['pitch_conversion'].append(dict(hz=hz, actual_ratio=actual,
                mathematical_ratio=ideal, cents_difference=float(1200*np.log2(actual/ideal))))
        aligned_values = {k: v for k, v in OLD_DEFAULT.items() if k != 'pitch_ratio'}
        for sr in (44100, 48000, 96000):
            for ratio in (.6, 1., 1.7):
                for art, exe in [(0, ch), (1, oh)]:
                    tag = f'parity-{sr}-{ratio}-{art}'
                    events = [(480, 'gate', 1), (481, 'gate', 0)]
                    old = render(tag+'-old', baseline, OLD_DEFAULT | dict(articulation=art, pitch_ratio=ratio), events, sr=sr, frames=sr)
                    new = hat(tag+'-new', exe, dict(freq=440*ratio), sr=sr, frames=sr)
                    original_difference = float(abs(new - old[:, art]).max())
                    original_deltas.append(dict(name=tag, max_abs_difference=original_difference))
                    aligned = render(tag+'-matched-hz', baseline_hz,
                        aligned_values | dict(articulation=art, freq=440*ratio), events, sr=sr, frames=sr)
                    difference = float(abs(new - aligned[:, art]).max())
                    deltas.append(difference)
                    check(tag+':matched-input-sound-preserved', difference < 3e-5, max_abs_difference=difference)
        lab.report['max_matched_input_sample_difference'] = max(deltas)
        lab.report['original_literal_ratio_comparisons'] = original_deltas
        lab.report['max_original_literal_sample_difference'] = max(d['max_abs_difference'] for d in original_deltas)
        dry = {}
        for label, exe, vector in [('closed', ch, ch_vec), ('open', oh, oh_vec)]:
            x = hat(label, exe, frames=96000)
            dry[label] = x
            check(label+':initial-silence', not np.any(x[:480]))
            check(label+':never-triggered', not np.any(hat(label+'-never', exe, events=[])))
            check(label+':zero-velocity', not np.any(hat(label+'-zero', exe, {'velocity': 0})))
            half = hat(label+'-half', exe, {'velocity': .5}, frames=96000)
            check(label+':linear-velocity', abs(half - .5*x).max() < 2e-7)
            held = hat(label+'-held', exe, events=[(480, 'gate', 1)], frames=96000)
            check(label+':noteoff-not-choke', np.array_equal(held, x))
            locks = [(480, 'gate', 1), (481, 'gate', 0), (8000, 'tone', .1), (8000, 'freq', 748), (8000, 'velocity', .2)]
            check(label+':onset-locks-retained', np.array_equal(hat(label+'-locks', exe, events=locks, frames=96000), x))
            for b in (1, 32, 64, 127, 256, 512):
                check(label+':block-'+str(b), np.array_equal(hat(label+'-block-'+str(b), exe, frames=96000, block=b), x))
            check(label+':vector-parity', abs(hat(label+'-vector', vector, frames=96000) - x).max() < 3e-5)
            long = hat(label+'-max-tail', exe, {'decay': 1}, frames=48000*20)
            check(label+':finite-ended-tail', abs(long[-48000:]).max() < 1e-8)
            rapid = [(480+i*211+j, 'gate', 1-j) for i in range(64) for j in (0, 1)]
            hat(label+'-rapid', exe, events=rapid)

        kill_at = 6240
        base_events = [(480, 'gate', 1), (481, 'gate', 0)]
        kill_events = base_events + [(kill_at, 'choke', 1), (kill_at, 'chokeGate', 1), (kill_at+1, 'chokeGate', 0)]
        killed = hat('external-choke', oh, events=kill_events, frames=96000)
        check('choke-silent-after-10ms', abs(killed[kill_at+480:]).max() < 1e-6)
        repeated = kill_events + [(kill_at+2400, 'chokeGate', 1), (kill_at+2401, 'chokeGate', 0)]
        check('repeated-choke-no-resurrection', np.array_equal(hat('repeated-choke', oh, events=repeated, frames=96000), killed))
        off_events = base_events + [(kill_at, 'chokeGate', 1), (kill_at+1, 'chokeGate', 0)]
        check('zero-choke-exact-off', np.array_equal(hat('zero-choke', oh, {'choke': 0}, events=off_events, frames=96000), dry['open']))
        simultaneous = [(480, 'gate', 1), (480, 'chokeGate', 1), (481, 'gate', 0), (481, 'chokeGate', 0)]
        check('simultaneous-choke-wins', not np.any(hat('simultaneous', oh, events=simultaneous)))
        fresh = hat('fresh-after-choke', oh, events=kill_events+[(12000, 'gate', 1), (12001, 'gate', 0)])
        check('new-hit-clears-choke', abs(fresh[12100:]).max() > .01)
        wrong = output / 'wrong-name.tsv'
        wrong.write_text('0\tpitch_ratio\t1\n')
        attempt = subprocess.run([str(ch), str(wrong), str(output/'invalid.f32'), '48000', '128', '1000', '0'], capture_output=True, text=True, timeout=20)
        check('misnamed-note-control-rejected', attempt.returncode != 0 and 'invalid control' in attempt.stderr)

        # Real signal input events occur inside compute blocks, not only at UI splits.
        count = 1024
        clocks = np.zeros((count, 2), dtype=np.float32)
        for n in range(5, count-4, 11):
            clocks[n:n+3, 0] = 1
        for length in (1, 2, 7, 16, 31, 32):
            values = SEQ_DEFAULT | {'length': length} | {k: float(i%3 != 1) for i, k in enumerate(STEPS)}
            observed = render('seq-length-'+str(length), seq, values, frames=count, inputs=clocks)
            expected = trace(clocks, values, [])
            check('seq-exact-length-'+str(length), np.array_equal(observed, expected))
        values = SEQ_DEFAULT | {k: 1 for k in STEPS}
        special = clocks.copy()
        special[5:8, 1] = 1  # simultaneous first clock/reset
        special[94:98, 1] = 1  # reset before an edge
        special[148:151, 1] = 1  # simultaneous reset/clock
        special[500:505, 0] = -1  # negative values do not open gates
        special[632:638, 0] = .3
        special[635:638, 0] = .8  # positive changes are not extra rising edges
        events = [(100, 'run', 0), (171, 'run', 1), (278, 'length', 7), (377, 'length', 31), (450, 'step01', 0)]
        expected = trace(special, values, events)
        for b in (1, 32, 64, 127, 128, 256, 512):
            observed = render('seq-edge-block-'+str(b), seq, values, events, frames=count, block=b, inputs=special)
            check('seq-edge-trace-'+str(b), np.array_equal(observed, expected))
        check('seq-vector-exact', np.array_equal(render('seq-edge-vector', seq_vec, values, events, frames=count, inputs=special), expected))
        check('seq-empty-pattern', not np.any(render('seq-empty', seq, SEQ_DEFAULT, frames=count, inputs=clocks)[:, 0]))
        only_last = SEQ_DEFAULT | {'length': 32, 'step32': 1}
        check('seq-last-step-addressable', np.array_equal(render('seq-last', seq, only_last, frames=count, inputs=clocks), trace(clocks, only_last, [])))
        ui_events = []
        for channel, key in [(0, 'clock'), (1, 'reset')]:
            previous = 0
            for n, v in enumerate(clocks[:, channel]):
                if v != previous:
                    ui_events.append((n, key, float(v)))
                    previous = v
        ui_result = render('seq-ui-events', seq_ui, values | {'clock': 0, 'reset': 0}, sorted(ui_events), frames=count)
        check('seq-ui-signal-parity', np.array_equal(ui_result, trace(clocks, values, [])))
        for sr in (44100, 96000):
            check('seq-rate-'+str(sr), np.array_equal(render('seq-rate-'+str(sr), seq, values, frames=count, inputs=clocks, sr=sr), trace(clocks, values, [])))
        mutant = output/'two-sample-mutant'
        mutant.mkdir(exist_ok=True)
        source = (SEQ/'engine.lib').read_text()
        anchor = 'pulse = tick * enabled;'
        check('mutant-anchor', source.count(anchor) == 1)
        (mutant/'engine.lib').write_text(source.replace(anchor, "pulse = (tick + tick') * enabled;"))
        (mutant/'audio-clock-test.dsp').write_text((SEQ/'audio-clock-test.dsp').read_text())
        bad = lab.build('seq-mutant', mutant/'audio-clock-test.dsp')
        bad_trace = render('seq-mutant-trace', bad, values, frames=count, inputs=clocks)
        check('trace-gate-rejects-two-sample-pulse-mutant', not np.array_equal(bad_trace, trace(clocks, values, [])))

        # Offline patch: Faust sequencer outputs drive new independent Faust voices.
        frames = 48000*10
        groove_clock = np.zeros((frames, 2), dtype=np.float32)
        for n in range(480, 480+64*6000, 6000):
            groove_clock[n:n+8, 0] = 1
        cp = SEQ_DEFAULT | {f'step{i+1:02d}': 1 for i in (0,2,4,6,8,10,12,14)}
        op = SEQ_DEFAULT | {f'step{i+1:02d}': 1 for i in (3,7,11,15)}
        closed_pulses = render('groove-closed-seq', seq, cp, frames=frames, inputs=groove_clock)[:,0]
        open_pulses = render('groove-open-seq', seq, op, frames=frames, inputs=groove_clock)[:,0]
        def edges(pulses, key):
            return [(int(n)+j, key, 1-j) for n in np.flatnonzero(pulses) for j in (0,1)]
        cx = hat('groove-closed', ch, events=edges(closed_pulses, 'gate'), frames=frames)
        ox = hat('groove-open', oh, events=sorted(edges(open_pulses,'gate')+edges(closed_pulses,'chokeGate')), frames=frames)
        comparison = np.concatenate([dry['closed'], dry['open'], killed])
        groove = cx+ox
        gain = min(1., .95/max(float(abs(comparison).max()), float(abs(groove).max()), 1e-20))
        audition = output/'audition'
        audition.mkdir(exist_ok=True)
        wavfile.write(audition/'01_closed_open_choked.wav', 48000, (comparison*gain).astype(np.float32))
        wavfile.write(audition/'02_sequenced_hats.wav', 48000, (groove*gain).astype(np.float32))
        wavfile.write(audition/'03_separate_hat_channels.wav', 48000, (np.column_stack([cx,ox])*gain).astype(np.float32))
        lab.report['audition'] = dict(global_gain=gain, processing='only this common gain; no mastering', routing='offline scores derived from actual Faust sequencer samples')
        lab.report['passed'] = True
    except Exception as exc:
        lab.report['passed'] = False
        lab.report['error'] = traceback.format_exc()
        if isinstance(exc, subprocess.CalledProcessError):
            lab.report['command_output'] = exc.output
        raise
    finally:
        lab.report['commit'] = command(['git','rev-parse','HEAD']).strip()
        lab.report['source_sha256'] = {str(p.relative_to(ROOT)): digest(p) for folder in (HATS,SEQ) for p in sorted(folder.iterdir()) if p.is_file()}
        lab.report['renderer_sha256'] = digest(ROOT/'tools/modules/render.cpp')
        lab.report['faust_version'] = command([os.getenv('FAUST','faust'),'--version'])
        lab.report['check_count'] = len(lab.report['checks'])
        lab.report['render_count'] = len(lab.report['renders'])
        (output/'results.json').write_text(json.dumps(lab.report, indent=2))
        print(json.dumps({k: lab.report.get(k) for k in ('commit','passed','check_count','render_count','max_matched_input_sample_difference','max_original_literal_sample_difference')}))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, default=ROOT/'build/hats-v2/analog-classics')
    run(parser.parse_args().out)
