# TX81Z v6 — panel fixed-frequency semantics

Owning issue: #97. Strategy: #111. Implementation PR: #127.

This version makes one deliberate instrument-facing correction over the retained v5 native-law reference: **fixed mode now means the audible Hz documented by Yamaha**, rather than reproducing the pinned `ymfm` substep interpretation that produced roughly 8 Hz from a value described there as 8192 Hz.

The v5 source and its counterexample remain unchanged for regression/reference evidence.

## Mapping

Yamaha documents fixed mode as eight ranges: 8–255, 16–510, 32–1020, 64–2040, 128–4080, 256–8160, 512–16320 and 1024–32640 Hz, with fine steps 1,2,4,8,16,32,64,128 Hz respectively. VCED `FREQ/CRS` is 0–63; ACED supplies `FIX`, `FIX RANGE`, and `FINE`.

The firmware/chip mapping retained by independent TX81Z reverse engineering uses the upper four bits of the six-bit VCED frequency value as OPZ fixed-frequency coarse, plus ACED fine. The resulting base value is the same register interpretation used by `ymfm` before its disputed phase-substep scaling:

```
coarse = CRS >> 2
base = ((coarse == 0) ? 8 : coarse * 16) | FINE
fixed_hz = base * (2 ^ FIX_RANGE)
```

`opNFixedCRS` therefore accepts the TX81Z's 0–63 VCED value. Its lower two bits do not affect fixed mode. `opNFine` and `opNRange` are the ACED fine/range controls already represented by the candidate. Ratio mode still uses the qualified v5 raw ratio controls; full VCED ratio/preset translation remains subsequent instrument work.

For the Faust host-rate voice, fixed Hz is converted directly to the existing 20-bit phase accumulator (`Hz * 2^20 / SR`). This is intentionally **not** native-`ymfm` parity for fixed mode. The old v5 mode exists specifically to preserve that software-reference discrepancy.

## Sources and limits

- Yamaha TX81Z owner manual: fixed-frequency range and fine-step table, ratio/fixed semantics.
- Yamaha TX81Z System Exclusive tables: VCED FREQ 0–63 and ACED FIX/FIX RANGE/FINE.
- `ymfm` pinned in v5: the register assembly and native substep implementation retained as the counterexample.
- `iflyhigh/ax81z` VCED/ACED reverse-engineering notes: independent documentation of the FREQ-upper-bits / FIX RANGE / FINE register mapping. It is used here as behavioral documentation, not copied source.

This does not yet add TX81Z LFO/AM, velocity sensitivity, level scaling, full VCED/ACED patch loading, hardware acceptance, GUI or polyphony. High fixed frequencies above host Nyquist can alias, just as Yamaha's manual notes hardware limits at the top of the original range. No anti-aliasing behavior is invented in this correction.
