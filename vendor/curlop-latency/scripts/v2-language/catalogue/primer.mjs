// Reader-facing routes are catalogue input: their construct ids must remain valid.
export default [];

export const primerRoutes = [
  {
    id: "variables", title: "Name material and reuse it",
    summary: "A definition gives a sequence a name. A function gives a reusable signal expression parameters, then returns that expression.",
    lesson: "Use a definition when the musical material itself is reused; use a function when the values feeding a receiver vary.",
    constructs: ["statement.definition", "statement.function", "material.sequence"],
    examples: ["statement.definition", "statement.function"],
  },
  {
    id: "routes", title: "Route material and address sockets",
    summary: "Send material to a module with <. Export >outputs, read <inputs, and name socket fields where the route needs them.",
    lesson: "The left side names where material goes. < reads from an input socket; > creates an output socket.",
    constructs: ["statement.route", "statement.socket_export", "statement.socket_route", "material.input"],
    examples: ["statement.route", "statement.socket_export", "statement.socket_route"],
  },
  {
    id: "pitch", title: "Write musical material directly",
    summary: "Notes such as c2 are pitch sugar; 440hz is an explicit frequency. / makes a sequence, while stack makes simultaneous voices.",
    lesson: "Bracketed sequences advance over time. A stack is polyphonic: each voice receives the enclosed per-voice receivers.",
    constructs: ["material.note", "material.sequence", "material.stack"],
    examples: ["material.note", "material.sequence", "material.stack"],
  },
  {
    id: "module_control", title: "Target modules and parameters",
    summary: "A receiver applies a value to a module parameter; use the qualified form when direct module addressing matters.",
    lesson: "Put a receiver after material to shape that material at its destination. Grouped receivers set several parameters together.",
    constructs: ["material.receiver", "material.grouped_receiver", "material.preset"],
    examples: ["material.receiver", "material.grouped_receiver", "material.preset"],
  },
  {
    id: "state", title: "Choose scope deliberately",
    summary: "Ordinary definitions describe local material. Session commands address the shared session state.",
    lesson: "There is no separate general-purpose global-variable spelling in this surface: shared state is explicit through session commands.",
    constructs: ["statement.definition", "statement.session", "material.markov"],
    examples: ["statement.session", "material.markov"],
  },
];
