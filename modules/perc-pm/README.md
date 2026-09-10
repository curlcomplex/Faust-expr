# perc-pm 0.1

Full-service tuned/inharmonic percussion experiment for #18. Primary reference family is Syntakt PC Carbon; Model:Cycles Perc supplies the compact FM-machine philosophy. Elektron's Syntakt OS 1.30 manual describes PC Carbon's SYN controls as Tune, Sweep (depth/time), Punch, Decay, Inharmonicity, Modulation, Modulation Envelope and Overdrive. This implementation independently authors the hidden curves and oscillator network; no firmware, tables or proprietary code are used.

The engine is deliberately distinct from kick and snare. A three-partial inharmonic PM body moves continuously from tom/wood/block territory to bell, glass and abrasive percussion. `sweep` coordinates depth and time, `inharmonicity` stretches operator ratios, `modulation` controls PM/feedback, and `mod_envelope` controls harmonic evolution. `punch` combines a short impact with early nonlinear emphasis. All musical controls latch at onset so locks cannot rewrite an already-ringing hit.

Reference previews are fixed before descriptor analysis. They are CC BY 4.0 PC Carbon one-shots from Winston Edwards / Particles Into Waves, *Syntakt Designer Drums*. They are lossy public previews with unknown firmware/settings/gain and therefore establish broad timbral/decay coverage only, not exact macro recovery or clone fidelity.

Qualification must render actual Faust at 44.1/48/96 kHz, varied block segmentation, repeated/persistent hits, same-sample locks, endpoint stress and high-register diagnostics. Audition files retain fixed kernel gain; reference collages use separately documented level matching. Musical approval, device realtime qualification and Tracker integration are separate gates.
