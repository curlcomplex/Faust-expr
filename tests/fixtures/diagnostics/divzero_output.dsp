// Deliberate runtime divide-by-zero with non-finite output.
// interp-tracer's default zero input exercises the division at runtime.
process = 1.0 / _;
