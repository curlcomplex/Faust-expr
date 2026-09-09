import("stdfaust.lib");
gate=button("gate");
frequency=hslider("frequency_hz",110,20,200,.001);
process=gate*os.osc(frequency)*0.25;
