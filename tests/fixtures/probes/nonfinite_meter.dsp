declare name "BadDiagnostic";
// Intentionally invalid diagnostic with finite passthrough: capture must fail.
process(x)=attach(x,log(x):hbargraph("bad[probe:999]",-1,1));
