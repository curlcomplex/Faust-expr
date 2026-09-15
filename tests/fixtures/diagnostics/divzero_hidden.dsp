// Deliberate runtime divide-by-zero whose final output is clamped finite.
// interp-tracer must surface the internal fault rather than relying on output NaN/Inf.
x = 1.0 / 0.0;
process = min(x, 1.0);
