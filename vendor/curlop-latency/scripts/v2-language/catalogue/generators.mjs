const generators = ["lfo", "ad", "adsr", "auto", "random", "deviate", "bernoulli", "keytrack", "accum"];
const signalGenerators = new Set(["lfo", "ad", "adsr", "auto", "random", "keytrack"]);
const snippets = {
  lfo: "lfo(sine, 2hz, 0, 1)", ad: "ad(20ms, 80ms, 0, 1)", adsr: "adsr(20ms, 80ms, 0.7, 120ms, 0, 1)",
  auto: "auto(lin, 0, 1)", random: "random(0, 1, 20ms)", deviate: "deviate(0.1)",
  bernoulli: "bernoulli(0.5, 0, 1)", keytrack: "keytrack(0, 1)", accum: "accum(0, 1, 1)",
};
const argumentsByName = {
  lfo: [
    { name: "shape", type: "waveform", choices: ["sine", "triangle", "tri", "saw", "square", "random", "sh"] },
    { name: "rate", type: "time or frequency", unit: "hz", example: "2hz" },
    { name: "low", type: "value", example: 0 }, { name: "high", type: "value", example: 1 },
    { name: "phase", type: "value", optional: true, example: 0 },
  ],
  ad: [{ name: "attack", type: "time", unit: "ms", example: "20ms" }, { name: "decay", type: "time", unit: "ms", example: "80ms" }, { name: "low", type: "value", example: 0 }, { name: "high", type: "value", example: 1 }],
  adsr: [{ name: "attack", type: "time", unit: "ms", example: "20ms" }, { name: "decay", type: "time", unit: "ms", example: "80ms" }, { name: "sustain", type: "value", example: 0.7 }, { name: "release", type: "time", unit: "ms", example: "120ms" }, { name: "low", type: "value", example: 0 }, { name: "high", type: "value", example: 1 }],
  auto: [{ name: "curve", type: "curve or value", choices: ["lin", "exp", "log", "eqpow"], example: "lin" }, { name: "low", type: "value", example: 0 }, { name: "high", type: "value", example: 1 }],
  random: [{ name: "low", type: "value", example: 0 }, { name: "high", type: "value", example: 1 }, { name: "slew", type: "time", unit: "ms", example: "20ms" }],
  deviate: [{ name: "amount", type: "value", example: 0.1 }],
  bernoulli: [{ name: "weight", type: "value", example: 0.5 }, { name: "low", type: "value", example: 0 }, { name: "high", type: "value", example: 1 }],
  keytrack: [{ name: "low", type: "value", example: 0 }, { name: "high", type: "value", example: 1 }],
  accum: [{ name: "start", type: "value", example: 0 }, { name: "increment", type: "value", example: 1 }, { name: "ceiling", type: "value", example: 1 }],
};

export default generators.map((name) => ({
  id: `generator.${name}`, name, family: signalGenerators.has(name) ? "signal_generators" : "generators", syntax: [`${name}(...)`],
  contexts: ["receiver_expression", "nested_expression"],
  binding: { parser: "parseGeneratorSignalValue", compiler: "compileFireExprGeneratorV2" },
  example: `bass < c2 cutoff(${snippets[name]})`,
  arguments: argumentsByName[name],
  completion: { label: `${name}()`, insert: snippets[name], summary: "control generator", kind: "function" },
}));
