#!/usr/bin/env python3
"""Validate the controlled probe render; save evidence without normalising away errors."""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import wave
import numpy as np

RATE = 48000


def measure(samples: np.ndarray) -> dict:
    x = np.asarray(samples, dtype=np.float64)
    if x.ndim != 1 or len(x) != RATE:
        raise ValueError("Expected exactly 48000 mono frames")
    if not np.isfinite(x).all():
        raise ValueError("Non-finite samples")
    peak = float(np.abs(x).max())
    start_peak = float(np.abs(x[:2000]).max())
    tail_peak = float(np.abs(x[45600:]).max())
    tones = []
    for start, end, expected in ((4800, 12000, 220), (26400, 33600, 880)):
        segment = x[start:end]
        spectrum = np.abs(np.fft.rfft(segment * np.hanning(len(segment))))
        frequency = float(np.fft.rfftfreq(len(segment), 1 / RATE)[int(spectrum.argmax())])
        rms = float(np.sqrt(np.mean(segment**2)))
        tones.append({"expected_hz": expected, "measured_hz": frequency, "rms": rms,
                      "pass": bool(abs(frequency - expected) < 8 and 0.085 < rms < 0.12)})
    ok = bool(0.14 < peak < 0.16 and start_peak < 1e-7 and tail_peak < 1e-4
              and all(t["pass"] for t in tones))
    return {"pass": ok, "sample_rate": RATE, "frames": len(x), "peak": peak,
            "initial_silence_peak": start_peak, "tail_peak": tail_peak, "tones": tones}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("raw", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    raw = args.raw.read_bytes()
    x = np.frombuffer(raw, dtype="<f4")
    report = measure(x)
    report["raw_sha256"] = hashlib.sha256(raw).hexdigest()
    report["scope"] = "Standalone Faust compiler probe, not cymbal or host acceptance"
    commit = args.output / "commit.txt"
    report["commit"] = commit.read_text().strip() if commit.exists() else "uncommitted-local-test"
    (args.output / "results.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))
    if not report["pass"]:
        return 1
    with wave.open(str(args.output / "probe.wav"), "wb") as wav:
        wav.setparams((1, 2, RATE, 0, "NONE", "not compressed"))
        wav.writeframes(np.rint(x * 32767).astype("<i2").tobytes())
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    fig, ax = plt.subplots(figsize=(10, 3.5))
    ax.plot(np.arange(len(x))[::16] / RATE, x[::16])
    ax.set(xlabel="Time (seconds)", ylabel="Amplitude (full scale)",
           title="Actual Faust probe render — not a cymbal or application screenshot")
    ax.set_ylim(-0.2, 0.2)
    fig.tight_layout()
    fig.savefig(args.output / "waveform.png", dpi=150)
    plt.close(fig)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
