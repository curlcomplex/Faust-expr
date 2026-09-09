// These spellings are recognised by the V2 parser, but the current VM-first
// compiler intentionally does not lower them. They are catalogue truth, not
// completion candidates or promises of runtime support.
export default ["noise", "sample_hold"].map((name) => ({
  id: `generator.${name}`, name, family: "parsed_only_generators", syntax: [`${name}(...)`],
  contexts: ["receiver_expression"], support: "parsed_not_lowerable",
  binding: { parser: "isSignalGenerator", compiler: "compileV2" },
  example: `bass < c2 cutoff(${name}())`, completion: null,
  arguments: [],
}));
