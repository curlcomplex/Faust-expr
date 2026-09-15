// #108 deterministic qualification fixture, not a released instrument.
import("stdfaust.lib");
freq = hslider("frequency_hz", 1000, 20, 24000, 0.0001);
shape = hslider("shape", 0, 0, 2, 1);
phase = os.phasor(1, freq) * (2*ma.PI);
partial(h) = 0.2 / h * sin(h * phase);
limited = sum(i, 8, partial(i+1) * ((i+1)*freq < ma.SR/2));
folded = sum(i, 8, partial(i+1));
process = select3(int(shape), partial(1), limited, folded);
