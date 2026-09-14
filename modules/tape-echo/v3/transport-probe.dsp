import("stdfaust.lib");
u=library("transport.lib");
target=nentry("target",15840,0,44000,1);
process=(_,_) : (!,!,u.chase(int(target)),u.chase(int(target)));
