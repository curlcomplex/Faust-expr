// Issue #105: analysis only; never linked into an instrument export.
import("stdfaust.lib");

// Reset each file/channel. True peak has 12 taps/phase: flush 11 zero frames.
// The peak hold follows the interpolator; do not feed held peaks into it.
process(x) = x, peak, (peak : (max ~ _)),
             (x : an.loudness_momentary(1)), (x : an.loudness_shortterm(1)),
             (x : an.loudness_integrated(1))
with { peak = an.true_peak(x); };
