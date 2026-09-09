// Actual-Faust instrumentation of the SAME control smoothing expression.
// This is not an instrument and not a shipping auxiliary output.
cp = library("controls.lib");
process = cp.colorValue,cp.rawColor;
