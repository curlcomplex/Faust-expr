declare name "clap-single-burst-diagnostic";
e=library("engine.lib"); process=(e.sqrt(max(0.0,1.0-e.bal))*e.singleNoise + e.sqrt(max(0.0,e.bal))*e.bodyVoice):e.finish;
