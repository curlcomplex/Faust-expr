"""Complete Metal verification plus an independent closed-form envelope oracle.
The rejected recurrence is compiled/rendered only as a negative control.
"""
from __future__ import annotations
import argparse, json, wave
from pathlib import Path
import numpy as np
from metal_batch import Study, command, sha


def expected_envelope(frames, rate, onset, decay, punch):
    """Float64 oracle, independent of the generated Faust signal graph."""
    if frames < 1 or rate < 1 or not 0 <= onset < frames:
        raise ValueError('invalid dimensions')
    if not 0 <= decay <= 1 or not 0 <= punch <= 1:
        raise ValueError('invalid controls')
    t = np.maximum(0, np.arange(frames, dtype=np.float64) - onset) / rate
    attack = .00018 + .0007 * (1 - punch) ** 2
    body = .015 * 160.0 ** decay
    x = -np.expm1(-t / attack) * np.exp(-t / body)
    x[:onset + 1] = 0
    return x


def relative_error(actual, expected):
    actual, expected = np.asarray(actual, dtype=float), np.asarray(expected, dtype=float)
    if actual.shape != expected.shape or not np.isfinite(actual).all():
        raise ValueError('invalid measured envelope')
    active = expected > .001
    if not active.any():
        raise ValueError('no audible-region samples')
    return float(np.max(np.abs(actual[active] - expected[active]) / expected[active]))


class AcceptanceStudy(Study):
    def execute(self):
        super().execute()
        analytic = []
        for record in list(self.report['renders']):
            if record['build'] != 'envelopes':
                continue
            rate = record['rate']
            decay = float(record['label'].split('-')[-1])
            x = np.fromfile(self.out / (record['label'] + '.f32'), dtype='<f4').reshape(-1, 2)
            expected = expected_envelope(len(x), rate, round(.05 * rate), decay, decay)
            error = relative_error(x[:, 1], expected)
            self.check('independent-envelope:' + record['label'], error < 1e-5, max_relative=error)
            analytic.append(dict(label=record['label'], rate=rate, max_relative=error,
                                 max_abs=float(np.max(abs(x[:, 1] - expected)))))
        self.report['independent_envelope_oracle'] = analytic

        # Actual Faust negative control: the old optimization must STILL fail
        # the unchanged 1.5% tolerance. Product audio never uses this entry.
        exe = self.build('rejected-envelope', 'rejected-envelope.dsp', diagnostic=True)
        rate = 44100; onset = round(.05 * rate); frames = round(rate * 21.65)
        label = 'negative-control-recurrence-44100'
        score = self.out / (label + '.tsv'); raw = self.out / (label + '.f32')
        score.write_text(f'0 decay 1\n0 punch 1\n0 gate 0\n{onset} gate 1\n{onset+1} gate 0\n')
        diag = json.loads(command([exe, score, raw, rate, 127, frames, 0]))
        x = np.fromfile(raw, dtype='<f4').reshape(frames, 2)
        self.check('negative-control:finite', np.isfinite(x).all())
        expected = expected_envelope(frames, rate, onset, 1., 1.)
        direct_error, rejected_error = relative_error(x[:, 0], expected), relative_error(x[:, 1], expected)
        self.check('negative-control:direct-accurate', direct_error < 1e-5, max_relative=direct_error)
        self.check('negative-control:old-error-detected', rejected_error > self.limits['envelope_relative_above_minus60'],
                   max_relative=rejected_error, unchanged_limit=self.limits['envelope_relative_above_minus60'])
        self.report['rejected_envelope_negative_control'] = dict(direct_max_relative=direct_error,
            rejected_max_relative=rejected_error, product_uses_rejected_envelope=False)
        self.report['renders'].append(dict(label=label, build='rejected-envelope', rate=rate,
            block=127, frames=frames, channels=2, score_sha256=sha(score), raw_sha256=sha(raw),
            peak=float(np.max(x)), rms=float(np.sqrt(np.mean(x.astype(float)**2))), mean=float(np.mean(x)),
            max_jump=float(np.max(abs(np.diff(x, axis=0)))), diag=diag))
        listening = {}
        for path in sorted(self.out.glob('*.wav')):
            with wave.open(str(path), 'rb') as handle:
                frames = handle.getnframes()
                count = frames * handle.getnchannels() * handle.getsampwidth()
                self.check('wav-container:' + path.name, frames > 0 and len(handle.readframes(frames)) == count)
                listening[path.name] = dict(frames=frames, rate=handle.getframerate(),
                                           channels=handle.getnchannels(), sha256=sha(path))
        self.report['validated_listening_files'] = listening
        self.report['product_envelope'] = 'direct exponential; recurrence is diagnostic-only'
        self.report['legacy_build_label'] = 'fast is the accepted scalar build, NOT a speedup claim'
        self.report['passed'] = True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--references', type=Path, required=True)
    parser.add_argument('--replay', type=Path)
    args = parser.parse_args()
    study = AcceptanceStudy(args.out.resolve(), args.references.resolve(),
                            args.replay.resolve() if args.replay else None)
    error = None
    try:
        study.execute()
    except Exception as exc:
        error = str(exc)
    finally:
        study.save(error)
    if error:
        raise SystemExit(error)


if __name__ == '__main__':
    main()
