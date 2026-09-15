// Runtime division survives code generation; final native output stays finite.
// Unlike 1/0, this is not a compile-time error masquerading as a runtime test.
process(x) = min(abs(1.0 / x), 1.0);
