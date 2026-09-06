// Compiler/render smoke test. This is NOT a physically modelled cymbal.
import("stdfaust.lib");
declare name "Cymbal Lab Toolchain Probe";
gate = button("gate");
freq = hslider("freq", 220, 20, 20000, 1);
gain = hslider("gain", 0.15, 0, 0.25, 0.001);
process = os.osc(freq) * gain * (gate : si.smoo);
