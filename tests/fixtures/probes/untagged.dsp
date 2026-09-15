declare name "LegacyMeter";
gain=hslider("gain",0.5,0,1,0.01);
process=*(gain):hbargraph("Level[unit:linear]",-1,1);
