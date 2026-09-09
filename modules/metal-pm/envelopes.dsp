// Two-channel arithmetic envelope diagnostic; not a musical entry point.
// Include the same onset activation used by finish(), so the arithmetic
// age-based reference is not falsely treated as active before the first note.
declare name "metal-pm-envelope-diagnostic";
e=library("engine.lib");
process=e.ampReference*e.seen,e.ampFast*e.seen;
