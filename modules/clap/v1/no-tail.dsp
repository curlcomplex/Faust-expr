// Diagnostic: canonical clap engine with only the diffuse tail disabled.
import("stdfaust.lib");
e = library("engine.lib");
process = e.render(0.0, 1.0);
