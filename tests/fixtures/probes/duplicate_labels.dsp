declare name "ProbeDuplicates";
// Same displayed labels in different groups; metadata is not identity by label.
left(x) = vgroup("Left[description:left stage]", x * hslider("gain",0.5,0,1,0.01)
    : hbargraph("Level[probe:101][hidden:1][unit:linear]", -1,1));
right(x) = vgroup("Right", x * hslider("gain",0.25,0,1,0.01)
    : hbargraph("Level[probe:102][hidden:0][unit:linear]", -1,1));
process(x) = left(x), right(x);
