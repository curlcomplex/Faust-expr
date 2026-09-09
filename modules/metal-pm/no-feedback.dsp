// Diagnostic only: removes feedback's host-rate-dependent one-sample delay.
declare name "metal-pm-no-feedback-diagnostic";
e=library("engine.lib")[feedbackScale=0.0;];
process=e.finish(e.dense,e.ampFast);
