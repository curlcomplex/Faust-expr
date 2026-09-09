# Metal PM — full-batch candidate

Owner: #17. New electronic metallic-percussion module; not the physical cymbal project and not a rewrite of Tracker's rejected machine.

Model:Cycles Metal supplies the compact product philosophy. The laboratory panel keeps Pitch, Decay, Color, Shape, Sweep and Contour directly visible, plus Punch and Drive for architecture/consumer evaluation. Syntakt CY Alloy is a separate comparator whose documented Shimmer/Radio/Modulation dimensions support testing independently detuned interacting operators, not exact ratios.

The engine is a six-state sinusoidal PM network. Color moves authored frequency relationships from relatively ordered toward inharmonic; Shape moves from a sparse readout to a denser network and increases interaction; Sweep/Punch control the initial pitch gesture; Contour controls modulation decay; Decay controls amplitude independently; Drive is neutral at zero. Gate/velocity are events. Choke is an explicit articulation input and is not part of the eight-knob surface.

All musical values latch on onset in this first version. Note-off leaves an open tail unless Choke is enabled. Persistent oscillator state is retained between hits. No sampled noise, cymbal model, reverb, hidden EQ, limiter or output normalization.

This first commit deliberately precedes measurement/optimization. The batch must test sparse versus dense spectral coverage, persistent retriggers, same-sample locks, choke timing, sample rates/block sizes, high-register behavior, and same-sound optimization before product handoff. Human listening remains separate from green CI.
