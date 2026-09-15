"""Declarative first slice for #96/#112.

This file defines the three smallest DX7 diagnostic questions. It deliberately
contains no synth implementation and no reference audio. An oracle adapter must
record its exact engine/source identity before these cases can become evidence.
"""

CASES = (
    {
        "id": "DX7-C01-carrier",
        "question": "single carrier frequency/output-level law",
        "operators": {"carrier": 1, "modulators": 0},
        "events": ((0.010, "gate", 1.0), (0.510, "gate", 0.0)),
        "notes_hz": (110.0, 440.0, 1760.0),
        "preserve_absolute_level": True,
        "compare": ("pitch", "level", "harmonic_spectrum", "raw_waveform"),
    },
    {
        "id": "DX7-C02-pair",
        "question": "one modulator into one carrier; modulation-index law",
        "operators": {"carrier": 1, "modulators": 1},
        "ratio": (1.0, 1.0),
        "modulator_level_grid": (0, 24, 48, 72, 96, 99),
        "events": ((0.010, "gate", 1.0), (0.510, "gate", 0.0)),
        "notes_hz": (110.0, 440.0),
        "preserve_absolute_level": True,
        "compare": ("pitch", "level", "harmonic_spectrum", "alias_images"),
    },
    {
        "id": "DX7-C03-envelope",
        "question": "two-operator articulated note; envelope/release trajectory",
        "operators": {"carrier": 1, "modulators": 1},
        "ratio": (1.0, 1.0),
        "events": ((0.010, "gate", 1.0), (0.510, "gate", 0.0)),
        "notes_hz": (220.0,),
        "preserve_absolute_level": True,
        "compare": ("pitch", "level", "amplitude_envelope", "release", "spectrum_over_time"),
    },
)


def validate_cases(cases=CASES):
    ids = [case["id"] for case in cases]
    assert len(ids) == len(set(ids))
    assert ids == ["DX7-C01-carrier", "DX7-C02-pair", "DX7-C03-envelope"]
    for case in cases:
        assert case["events"][0][1:] == ("gate", 1.0)
        assert case["events"][1][1:] == ("gate", 0.0)
        assert case["events"][0][0] < case["events"][1][0]
        assert case["preserve_absolute_level"] is True
        assert case["compare"]
    return True


if __name__ == "__main__":
    validate_cases()
    for case in CASES:
        print(case["id"], case["question"])
