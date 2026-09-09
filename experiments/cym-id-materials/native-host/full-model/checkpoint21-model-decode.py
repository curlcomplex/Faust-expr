#!/usr/bin/env python3
"""Lossless transport decoder for one frozen full-size checkpoint-21 impact.

The predictor is ONLY a compression tool: XOR residuals restore original IEEE
754 values, and SHA-256 checks reject every inexact array. This does not alter
the synthesizer, reduce its modes, or recreate coefficient values approximately.
"""
from __future__ import annotations
import argparse
import hashlib
import io
import json
from pathlib import Path
import tarfile
import numpy as np

CAPSULE_SHA256 = '61b6a32078c2bb4eef3ec9533436a777fbc8949c5253169f8e53559996a042f8'
CAPSULE_BYTES = 449992
REQUIRED_NAMES = {'low-essential.bin', 'low-fluid.bin', 'proxy.bin',
    'low-indices.bin', 'low-thickness-xor.bin', 'high-essential.bin',
    'high-indices.bin', 'high-map-xor.bin', 'knot-scale.bin', 'envelope.bin'}


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def predict_log_ratio(a: float, b: float) -> float:
    z = (a - b) / (a + b)
    z2 = z * z
    power = z
    total = z
    for k in range(3, 32, 2):
        power = power * z2
        total = total + power / k
    return 2 * total


def prediction(sf, sv, tf, indices, normalise=False):
    output = np.empty((len(tf), sv.shape[1]), dtype='<f8')
    for i, frequency in enumerate(tf):
        weights = []
        for j in indices[i]:
            z = predict_log_ratio(float(sf[j]), float(frequency)) / .24
            x = -.5 * z * z
            term = value = 1.
            if x < -1:
                value = 1.
            else:
                for n in range(1, 25):
                    term = (term * x) / n
                    value = value + term
            weights.append(value)
        total = 0.
        for w in weights:
            total = total + w
        weights = [w / total for w in weights]
        row = []
        for column in range(sv.shape[1]):
            value = 0.
            for k, j in enumerate(indices[i]):
                value = value + weights[k] * float(sv[j, column])
            row.append(value)
        if normalise:
            total = 0.
            for k in range(6):
                total = total + row[k]
            for k in range(6):
                row[k] = row[k] / total
        output[i] = row
    return output


def decode(capsule: Path):
    compressed = capsule.read_bytes()
    if len(compressed) != CAPSULE_BYTES or digest(compressed) != CAPSULE_SHA256:
        raise ValueError('Frozen capsule is missing, incomplete or changed; refusing to render')
    with tarfile.open(fileobj=io.BytesIO(compressed), mode='r:xz') as tar:
        members = tar.getmembers()
        if len(members) != 11 or {m.name for m in members} != REQUIRED_NAMES | {'manifest.json'}:
            raise ValueError('Unexpected capsule members')
        if any(not m.isfile() or m.size > 2_000_000 for m in members):
            raise ValueError('Unsafe capsule entry')
        manifest = json.load(tar.extractfile('manifest.json'))
        arrays = {}
        for key, entry in manifest['arrays'].items():
            name = key + '.bin'
            if name not in REQUIRED_NAMES:
                raise ValueError('Unknown numerical array')
            dtype = np.dtype(entry['dtype'])
            if dtype.str not in ('<f8', '<u8', '<u2'):
                raise ValueError('Invalid numeric type')
            shape = tuple(entry['shape'])
            if len(shape) != 2 or any(int(s) <= 0 for s in shape):
                raise ValueError('Invalid numeric shape')
            encoded = tar.extractfile(name).read()
            if len(encoded) != int(np.prod(shape)) * dtype.itemsize or len(encoded) != entry['length']:
                raise ValueError('Unexpected encoded size')
            if digest(encoded) != entry['sha256']:
                raise ValueError('Transport array mismatch: ' + name)
            raw_bytes = np.frombuffer(encoded, dtype='u1').reshape(dtype.itemsize, -1).T.copy().tobytes()
            restored = np.frombuffer(raw_bytes, dtype=dtype).reshape(shape[1], shape[0]).T.copy()
            arrays[name] = restored
    low = np.zeros((4207, 21), dtype='<f8')
    le = arrays['low-essential.bin']
    low[:, :2], low[:, 15:19] = le[:, :2], le[:, 2:]
    proxy = arrays['proxy.bin']
    guessed = prediction(proxy[:, 0], proxy[:, 1:], low[:, 0], arrays['low-indices.bin'], True)
    low[:, 2:9] = np.bitwise_xor(guessed.view('<u8'), arrays['low-thickness-xor.bin']).view('<f8')
    low[:, 9:15] = arrays['low-fluid.bin']
    high = np.zeros((3053, 21), dtype='<f8')
    he = arrays['high-essential.bin']
    high[:, :2], high[:, 15:21] = he[:, :2], he[:, 2:]
    guessed = prediction(low[:, 0], low[:, 2:15], high[:, 0], arrays['high-indices.bin'])
    high[:, 2:15] = np.bitwise_xor(guessed.view('<u8'), arrays['high-map-xor.bin']).view('<f8')
    rng = np.random.default_rng(1609)
    rng.standard_normal(1853)
    raw = rng.standard_normal((1853, 57))
    scale = arrays['knot-scale.bin']
    knots = np.zeros((3053, 57), dtype='<f8')
    knots[:1853] = (raw / scale[:, 0, None]) * scale[:, 1, None]
    envelope = arrays['envelope.bin']
    et = envelope[:, 0].copy(); ew = envelope[:, 1].copy()
    result = {'low': low, 'high': high, 'knots': knots, 'envelope_time': et, 'envelope_weight': ew}
    report = {'capsule_sha256': CAPSULE_SHA256, 'transport_only': True, 'numpy': np.__version__,
              'mode_counts': {'ordinary': 4207, 'hard': 1853, 'bloom': 1200}, 'arrays': {}}
    checks = {'low': low, 'high': high, 'knots': knots, 'envelope': np.column_stack([et, ew])}
    for name, a in checks.items():
        sha = digest(np.ascontiguousarray(a).tobytes())
        expected = manifest['expected'][name]
        if sha != expected or not np.isfinite(a).all():
            raise ValueError(f'Full original array did not reproduce exactly: {name}: {sha} != {expected}')
        report['arrays'][name] = {'shape': list(a.shape), 'sha256': sha, 'exact_frozen_hash': True}
    if digest(raw.tobytes()) != manifest['rng']['raw_sha256']:
        raise ValueError('Frozen modulation RNG sequence did not reproduce exactly')
    return result, report


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('capsule', type=Path)
    ap.add_argument('output_directory', type=Path)
    args = ap.parse_args()
    arrays, report = decode(args.capsule)
    args.output_directory.mkdir(parents=True, exist_ok=True)
    for name, a in arrays.items():
        np.save(args.output_directory / (name + '.npy'), a, allow_pickle=False)
    (args.output_directory / 'model-identity.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))

if __name__ == '__main__':
    main()
