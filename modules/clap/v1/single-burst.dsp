// Diagnostic: canonical clap engine with the diffuse tail retained but only the first burst.
import("stdfaust.lib");
e = library("engine.lib");
process = e.render(1.0, 0.0);
