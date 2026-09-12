declare name "Bassline Seq";
declare version "0.1.0-experiment";
declare category "Analog Classics";
declare curlop_role "control-source";
bs=library("engine.lib");
clock=button("clock[curlop:input]");reset=button("reset[curlop:input]");
process=bs.sequence(clock,reset);
