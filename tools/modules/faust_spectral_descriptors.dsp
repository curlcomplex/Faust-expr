// Issue #107: OFFLINE descriptors; not an instrument or an FFT metric.
import("stdfaust.lib");

// Single source of the fixed analysis profile: the driver reads these literals.
O = 3;
M = 3;
FTOP = 10000;
N = 24;
T = 0.02;    // Rectangular power averaging window, NOT an exponential tau.
HOP = 0.02;  // Rectangular amplitude window AND flux comparison delay.

process(x) = x,
             (x : an.spectral_centroid(O,M,FTOP,N,T)),
             (x : an.spectral_spread(O,M,FTOP,N,T)),
             (x : an.spectral_flux(O,M,FTOP,N,HOP)),
             (x : an.band_powers(O,M,FTOP,N,T) :> _);
