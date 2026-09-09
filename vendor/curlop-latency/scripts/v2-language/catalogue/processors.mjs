const transforms = ["invert", "scale", "clip", "offset", "gain", "abs", "smooth", "transpose", "octave", "quantize"];
const processors = ["repeat", "transpose", "octave", "reverse", "rotate", "shuffle", "groove", "invert", "timescale", "sort", "grid", "scale", "arp", "ratchet", "flam", "buzz", "bounce", "geiger", "len", "deviate", "vel", "velocity", "prob", "probability", "cond", "bernoulli", "strum", "bpm", "fit", "chord"];
const structuralProcessors = new Set(["repeat", "transpose", "octave", "reverse", "rotate", "shuffle", "groove", "invert", "timescale", "sort", "grid", "scale", "arp", "ratchet", "flam", "buzz", "bounce", "geiger", "len", "deviate", "vel", "cond", "bpm", "fit", "chord"]);
const transformSnippets = {
  invert: "invert()", scale: "scale(0, 1)", clip: "clip(0, 1)", offset: "offset(0)",
  gain: "gain(1)", abs: "abs()", smooth: "smooth(20ms)", transpose: "transpose(12)",
  octave: "octave(-1)", quantize: "quantize(0.1)",
};
const processorSnippets = {
  repeat: "repeat(2)", transpose: "transpose(12)", octave: "octave(-1)", reverse: "reverse()",
  rotate: "rotate(1)", shuffle: "shuffle()", groove: "groove(swing, 50)", invert: "invert()",
  timescale: "timescale(1)", sort: "sort(up)", grid: "grid(1/16)", scale: "scale(c, minor)",
  arp: "arp(up, 1/8)", ratchet: "ratchet(2)", flam: "flam(1/16)", buzz: "buzz(2)",
  bounce: "bounce(0.5)", geiger: "geiger(0.5)", len: "len(1/4)", deviate: "deviate(0.1)",
  vel: "vel(0.5)", velocity: "velocity(0.5)", prob: "prob(0.5)", probability: "probability(0.5)",
  cond: "cond(first)", bernoulli: "bernoulli(0.5)", strum: "strum(0)", bpm: "bpm(120)",
  fit: "fit(4)", chord: "chord(major)",
};
const processorExamples = {
  prob: "bass < c2 prob(0.5)", probability: "bass < c2 probability(0.5)",
  bernoulli: "bass < c2 bernoulli(0.5, c3, e3)", strum: "bass < c2 strum(50ms)",
  chord: "bass < c3 chord(min7)",
};
const transformArguments = {
  invert: [], scale: [{ name: "low", type: "value", example: 0 }, { name: "high", type: "value", example: 1 }],
  clip: [{ name: "low", type: "value", example: 0 }, { name: "high", type: "value", example: 1 }],
  offset: [{ name: "amount", type: "value", example: 0 }], gain: [{ name: "amount", type: "value", example: 1 }],
  abs: [], smooth: [{ name: "time", type: "time", unit: "ms", example: "20ms" }],
  transpose: [{ name: "semitones", type: "value", example: 12 }], octave: [{ name: "octaves", type: "value", example: -1 }],
  quantize: [{ name: "step", type: "value", example: 0.1 }],
};
const numeric = (name, example) => [{ name, type: "numeric, unit, or dynamic expression", example }];
const processorArguments = {
  repeat: numeric("count", 2), transpose: numeric("semitones", 12), octave: numeric("octaves", -1), reverse: [],
  rotate: numeric("steps", 1), shuffle: [], groove: [{ name: "style", type: "groove name", example: "swing" }, { name: "amount", type: "numeric, unit, or dynamic expression", example: 50 }],
  invert: [], timescale: numeric("factor", 1), sort: [{ name: "direction", type: "sort direction", example: "up" }], grid: numeric("duration", "1/16"),
  scale: [{ name: "root", type: "named root", example: "c" }, { name: "mode", type: "named scale or scale generator", example: "minor" }],
  arp: [{ name: "direction", type: "arpeggio direction", example: "up" }, { name: "rate", type: "numeric, unit, or dynamic expression", example: "1/8" }],
  ratchet: numeric("count", 2), flam: numeric("offset", "1/16"), buzz: numeric("count", 2), bounce: numeric("amount", 0.5), geiger: numeric("amount", 0.5), len: numeric("duration", "1/4"),
  deviate: numeric("amount", 0.1), vel: numeric("value", 0.5), velocity: numeric("value", 0.5), prob: numeric("value", 0.5), probability: numeric("value", 0.5),
  cond: [{ name: "condition", type: "condition keyword, loop number, or supported expression", example: "first" }, { name: "cycle", type: "whole VM u16 number", optional: true, example: 2 }],
  bernoulli: [{ name: "weight", type: "numeric, unit, or dynamic expression", example: 0.5 }, { name: "when true", type: "material", example: "c3" }, { name: "when false", type: "material", example: "e3" }], strum: numeric("offset", "50ms"), bpm: [{ name: "value", type: "BPM value", example: 120 }], fit: numeric("length", 4), chord: [{ name: "name", type: "static known chord name", example: "min7" }],
};

export default [
  ...transforms.map((name) => ({
    id: `transform.${name}`, name, family: "transforms", syntax: [`${name}(...)`],
    contexts: ["signal_chain", "receiver_expression"],
    binding: { parser: "parseSignalChainTransformOp", compiler: "compileV2" },
    example: `bass < c2 cutoff(lfo(sine, 2hz, 0, 1) : ${transformSnippets[name]})`,
    arguments: transformArguments[name],
    completion: { label: `${name}()`, insert: transformSnippets[name], summary: "signal transform", kind: "function" },
  })),
  {
    id: "transform.slew", name: "slew", family: "transforms", syntax: ["slew(time)"],
    contexts: ["signal_chain", "socket_route"], aliases: ["smooth"],
    binding: { parser: "parseSignalChainTransformOp", compiler: "compileV2" },
    example: "bass < c2 cutoff(lfo(sine, 2hz, 0, 1) : slew(20ms))",
    arguments: [{ name: "time", type: "time", unit: "ms", example: "20ms" }],
    completion: { label: "slew()", insert: "slew(20ms)", summary: "signal transform alias", kind: "function" },
  },
  ...processors.map((name) => ({
    id: `processor.${name}`, name, family: structuralProcessors.has(name) ? "structural_processors" : "step_processors", syntax: [`${name}(...)`],
    contexts: ["step", "material_scope"],
    binding: { parser: "parsePostfixArgs", compiler: "compileV2" },
    example: processorExamples[name] ?? `bass < [c2 / e2] ${processorSnippets[name]}`,
    arguments: processorArguments[name],
    completion: { label: `${name}()`, insert: processorSnippets[name], summary: "sequence processor", kind: "function" },
  })),
];
