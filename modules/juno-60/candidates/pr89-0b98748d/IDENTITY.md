# Preserved PR89 candidate

Review identity: **juno-60/pr89-0b98748d**. It is not PR83's `juno-60/reference-v1`.

`voice.dsp`, `vcf.lib` and `ORIGINAL_README.md` retain the exact three Git blobs
from PR89 at `0b98748db3f21bb19cf0808b401dca8399645ee7`. The original README filename
was changed only to distinguish its historical claims from current evidence.
The original voice metadata still says 0.1.0; use the explicit candidate identity,
source path and digest rather than that ambiguous historical label.

PR83's v1/v2 are unchanged. `../../CANDIDATES.json` enumerates all three sources.
Do not merge PR89's old colliding v1 path on top of PR83 as a second definition;
this review path is the non-destructive integration route. A future changed
candidate or release needs a deliberate new sound identity and metadata.

The first dedicated recovery qualification at `6524c5da...` compiled and rendered
this exact source under scalar/vector and 44.1/48/96 kHz settings. It is no longer
relying on the old probe/kick-only smoke badge. No earlier hardware score for
PR83 v1/v2 transfers to this candidate, and none is selected for promotion.

The three-way audition is eight seconds per source: PR83 v1, PR83 v2, then this
PR89 candidate. Each uses its own recorded defaults under the same note phrase,
raw relative levels, no effects and no loudness fitting. This is a candidate
comparison, not an assertion that each successive segment is an improvement.
