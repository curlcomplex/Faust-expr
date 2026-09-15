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

// The loudness APIs take a compile-time channel count. This kernel is mono;
// multichannel files are deliberately analyzed as independent channel kernels
// by the host so this value is always 1.
process = input <: (
    _,
    truePeak,
    truePeakHold,
    an.loudness_momentary(1),
    an.loudness_shortterm(1),
    an.loudness_integrated(1)
);
