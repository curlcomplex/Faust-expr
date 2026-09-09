// Controlled architecture alternative: same controls/envelopes, sparse readout.
declare name "metal-pm-sparse";
e=library("engine.lib");
process=e.finish(e.sparse,e.ampFast);
