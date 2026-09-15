// Offline measurement DSP for issue #105.
// Keep this separate from instrument DSP so analysis-toolchain changes cannot
// silently change accepted instrument renders.
import("stdfaust.lib");

// One-channel measurement kernel. The host runs one instance per channel.
// `an.true_peak` is an oversampled true-peak estimate. We hold its maximum
// explicitly because the raw analyzer output is instantaneous.
input = _;
truePeak = an.true_peak(input);
truePeakHold = max(truePeak) ~ _;

// Loudness analyzers are intentionally exposed as separate signals. In
// particular, Faust's integrated loudness is a streaming approximation and
// must not be reported as an exact offline two-pass result.
process = input <: (
    _,
    truePeak,
    truePeakHold,
    an.loudness_momentary,
    an.loudness_shortterm,
    an.loudness_integrated
);
