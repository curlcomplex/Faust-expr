// #108 finite-order sine-driven nonlinear fixture: x + drive*x^3.
// Its known third harmonic folds above Nyquist. No limiter or output normalization.
drive = hslider("drive", 1, 0, 4, 0.01);
process(x) = x + drive * x*x*x;
