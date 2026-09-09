const examples = {
  "material.note": "bass < c3", "material.rest": "bass < [c3 / -]", "material.hold": "bass < [c3 / _]",
  "material.sequence": "bass < [c3 / e3]", "material.tuplet": "bass < (c3 / e3)",
  "material.receiver": "bass < c3 cutoff(0.5)", "material.grouped_receiver": "bass < c3 [cutoff & drive](0.5)",
  "material.stack": "synth < [c3 stack(2){pan(0)}]", "material.preset": "bass < c3 preset(1){cutoff(0.5)}",
  "material.select": "bass < select(1){c3, e3}", "material.markov": "phrase = [c2 / e2]\nbass < markov(phrase, 1)",
  "material.input": "bass < c3 cutoff(<env)",
};
const argumentsById = {
  "material.note": [{ name: "pitch", type: "note or frequency", example: "c3" }],
  "material.rest": [], "material.hold": [],
  "material.sequence": [{ name: "item", type: "material", example: "c3" }],
  "material.tuplet": [{ name: "item", type: "material", example: "c3" }],
  "material.receiver": [{ name: "receiver", type: "module parameter", example: "cutoff" }, { name: "expression", type: "signal expression", example: 0.5 }],
  "material.grouped_receiver": [{ name: "receiver", type: "module parameter", example: "cutoff" }, { name: "expression", type: "signal expression", example: 0.5 }],
  "material.stack": [{ name: "count", type: "voice count", example: 2 }, { name: "receiver", type: "per-voice receiver", optional: true, example: "pan(0)" }],
  "material.preset": [{ name: "selector", type: "preset selector", example: 1 }, { name: "receiver", type: "receiver assignment", optional: true, example: "cutoff(0.5)" }],
  "material.select": [{ name: "selector", type: "numeric selector", example: 1 }, { name: "material", type: "option", example: "c3" }],
  "material.markov": [{ name: "material", type: "sequence or pitch source", example: "phrase" }, { name: "order", type: "numeric order", example: 1 }],
  "material.input": [{ name: "input", type: "input socket path", example: "env" }],
};

export default [
  ["material.note", "note", ["c3", "bb2", "440hz"], ["step", "route", "socket_export"]],
  ["material.rest", "rest", ["-"], ["step"]],
  ["material.hold", "hold", ["_"], ["step"]],
  ["material.sequence", "sequence", ["[item / item]", "item / item"], ["definition", "route", "socket_export"]],
  ["material.tuplet", "tuplet", ["(item / item)"], ["step", "socket_export"]],
  ["material.receiver", "receiver", ["cutoff(expression)", "module.cutoff(expression)"], ["step", "route"]],
  ["material.grouped_receiver", "grouped receiver", ["[cutoff & drive](expression)"], ["step", "route"]],
  ["material.stack", "stack", ["stack(count){ receiver(expression) }"], ["step", "polyphony"], "partial"],
  ["material.preset", "preset", ["preset(selector) { receiver(...), receiver(...) }"], ["step", "receiver_state"], "partial"],
  ["material.select", "select", ["select(selector) { material, material }"], ["step"], "partial"],
  ["material.markov", "markov", ["markov(material, order)", "markov(part.pitch, order)"], ["step", "stateful"], "partial"],
  ["material.input", "input read", ["<input", "<midi.pitch"], ["receiver_expression", "socket_route"]],
].map(([id, name, syntax, contexts, support = "implemented"]) => ({
  id, name, family: "material", syntax, contexts, support,
  binding: { parser: "parseMaterial", compiler: "compileV2" }, example: examples[id], arguments: argumentsById[id], completion: null,
}));
