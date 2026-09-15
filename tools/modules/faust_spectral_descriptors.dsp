// Issue #107: offline filter-bank descriptors, intentionally separate from FFT metrics.
import("stdfaust.lib");

// Pinned qualification configuration. Keep names/config explicit in reports.
O = 3;       // filter split order
M = 3;       // bands per octave
FTOP = 10000;
N = 24;      // includes dc/top bands
T = 0.02;    // centroid/spread power smoothing seconds
HOP = 0.02;  // flux comparison/smoothing seconds

process(x) = x,
             (x : an.spectral_centroid(O,M,FTOP,N,T)),
             (x : an.spectral_spread(O,M,FTOP,N,T)),
             (x : an.spectral_flux(O,M,FTOP,N,HOP));
