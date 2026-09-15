"""Create the pinned, portable Analog Classics internal-review set.

The source registry is deliberately small and explicit: it is a review decision,
not a discovery rule that silently promotes a newer file.  ``faust -e`` expands
all Faust libraries; metadata normalisation is verified not to alter an
expression line. The resulting scripts are self-contained.
"""
from pathlib import Path
import argparse, hashlib, json, subprocess, sys, uuid

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).parent))
from faust_export_metadata import normalize_expanded

FREEZE = ROOT / "modules/analog-classics/synth-finish/REVIEW_FREEZE.json"
PINNED_COMMITS = {
    "606": "02f195e64cf274d0e995325fe2fd526d404bbe95",
    "909": "7aa75de6394568410d5b114d62b405138e9c9e35",
    "808-aux": "07464333c5eed1e916820a3fec99b69147561cb0",
}
SUBJECTS = (
    # Exact synth freeze: do not promote PR89 or Mini v2/v3 by version number.
    ("juno-60", "instrument", "modules/juno-60/v3/voice.dsp", "selected by the hardware/listening freeze; PR89 remains rejected"),
    ("juno-106", "instrument", "modules/juno-106/v3/voice.dsp", "selected coherent-Roland freeze candidate"),
    ("sh-101", "instrument", "modules/mono-101/v4/voice.dsp", "selected coherent-Roland freeze candidate"),
    ("mini", "instrument", "modules/minimoog/v1/voice.dsp", "stable v1 retained; v2 collapse and v3 experiment remain unselected"),
    ("808-kick", "instrument", "modules/analog-classics/drums-v2/kick808.dsp", "v2 replaces preserved exploratory v1"),
    ("808-snare", "instrument", "modules/analog-classics/drums-v2/snare808.dsp", "v2 replaces preserved exploratory v1"),
    ("808-clap", "instrument", "modules/analog-classics/drums-v2/clap808.dsp", "v2 replaces preserved exploratory v1"),
    ("808-cymbal", "instrument", "modules/analog-classics/drums-v2/cymbal808.dsp", "v2 replaces preserved exploratory v1"),
    ("analog-hat-closed", "instrument", "modules/hats-analog/single-note-v1/closed.dsp", "explicit v2-derived single-note compatibility facade"),
    ("analog-hat-open", "instrument", "modules/hats-analog/single-note-v1/open.dsp", "explicit v2-derived single-note compatibility facade with local choke"),
    ("analog-kick-sharp", "instrument", "modules/kick-analog/sharp-02/optimized.dsp", "same-sound optimized candidate; reference path remains preserved"),
    ("analog-snare", "instrument", "modules/snare-analog/v2/snare.dsp", "v2 delivery supersedes the root prototype"),
    ("clap", "instrument", "modules/clap/v3/clap.dsp", "v3 rebuild selected; v1/v2 rejected identities remain explicit"),
    ("acid-voice", "instrument", "modules/acid-voice/v1/voice.dsp", "current single-note Acid Voice candidate"),
    ("trigger-seq", "modulation", "modules/trigger-seq/v1/trigger.dsp", "external-clock source; CV output and meter observation are distinct"),
    ("retro-mixer-eq", "effect", "modules/retro-mixer-eq/v1/eq.dsp", "current compact mixer-EQ candidate"),
    ("tape-echo", "effect", "modules/tape-echo/v1/echo.dsp", "current multi-head echo candidate"),
    ("vintage-rack-reverb", "effect", "modules/vintage-rack-reverb/v1/reverb.dsp", "current rack-reverb candidate"),
    ("vintage-tape", "effect", "modules/vintage-tape/v1/tape.dsp", "current tape-colour candidate"),
    ("retro-ensemble", "effect", "modules/retro-ensemble/v1/ensemble.dsp", "current ensemble candidate"),
    ("bassline-seq", "modulation", "modules/bassline-seq/v1/sequence.dsp", "current Acid-batch sequencer entry"),
    # These frozen sibling worktrees deliberately retain their upstream commit;
    # do not make a later stack position look like a selection decision.
    ("606-kick", "instrument", "modules/drums-606/v1/kick.dsp", "#79 frozen 606 kick selection", "work/source-606"),
    ("606-snare", "instrument", "modules/drums-606-reference/v1/snare.dsp", "#79 frozen 606 snare selection", "work/source-606"),
    ("606-low-tom", "instrument", "modules/drums-606/v1/tom.dsp", "#79 frozen 606 low-tom preset", "work/source-606"),
    ("606-high-tom", "instrument", "modules/drums-606/v1/tom.dsp", "#79 frozen 606 high-tom preset", "work/source-606"),
    ("606-closed-hat", "instrument", "modules/drums-606/v1/closed-hat.dsp", "#79 frozen 606 closed-hat selection", "work/source-606"),
    ("606-open-hat", "instrument", "modules/drums-606-reference/v1/open-hat.dsp", "#79 frozen 606 open-hat/choke selection", "work/source-606"),
    ("606-cymbal", "instrument", "modules/drums-606-reference/v1/cymbal.dsp", "#79 frozen 606 cymbal selection", "work/source-606"),
    ("909-kick", "instrument", "modules/drums-909/v1/kick.dsp", "#81 frozen recorded-anchor selection", "work/source-909"),
    ("909-snare", "instrument", "modules/drums-909/v1/snare.dsp", "#81 frozen recorded-anchor selection", "work/source-909"),
    ("909-low-tom", "instrument", "modules/drums-909/v1/tom.dsp", "#81 frozen recorded-anchor preset", "work/source-909"),
    ("909-mid-tom", "instrument", "modules/drums-909/v1/tom.dsp", "#81 frozen recorded-anchor preset", "work/source-909"),
    ("909-high-tom", "instrument", "modules/drums-909/v1/tom.dsp", "#81 frozen recorded-anchor preset", "work/source-909"),
    ("909-rim", "instrument", "modules/drums-909/v1/rim.dsp", "#81 frozen recorded-anchor selection", "work/source-909"),
    ("909-clap", "instrument", "modules/drums-909/v1/clap.dsp", "#81 frozen recorded-anchor selection", "work/source-909"),
    ("808-aux-tom-conga", "instrument", "modules/drums-808-aux/v1/tom-conga.dsp", "#76 experimental auxiliary source retained for review", "work/source-808-aux"),
    ("808-aux-rim-claves", "instrument", "modules/drums-808-aux/v2/rim-claves.dsp", "#76 selected v2 Claves pitch correction", "work/source-808-aux"),
    ("808-aux-maracas", "instrument", "modules/drums-808-aux/v1/maracas.dsp", "#76 experimental auxiliary source retained for review", "work/source-808-aux"),
    ("808-aux-cowbell", "instrument", "modules/drums-808-aux/v1/cowbell.dsp", "#76 experimental auxiliary source retained for review", "work/source-808-aux"),
)
REJECTED = {
    "modules/juno-60/candidates/pr89-0b98748d/voice.dsp": "owner sonic rejection recorded in PR89/PR90 reconciliation",
    "modules/minimoog/v2/voice.dsp": "high-cutoff held-note collapse; engineering fixture only",
    "modules/minimoog/v3/voice.dsp": "numerically repaired experiment without demonstrated whole-instrument advantage",
    "modules/analog-classics/drums-v1": "exploratory v1 drum studies retained, not selected",
    "modules/hats-analog/v1": "superseded paired-hat baseline retained as negative control",
    "modules/clap/v1": "superseded clap identity",
    "modules/clap/v2": "rejected pitched-body clap identity",
    "909 sample-backed voices": "asset/rights pending; diagnostic sample fixture is not a portable instrument",
}

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def declarations(text):
    result = {}
    for line in text.splitlines():
        if line.startswith("declare ") and ' "' in line:
            key, value = line[8:].split(' "', 1)
            result[key] = value.rsplit('";', 1)[0]
    return result

def export_one(source, destination):
    include_dirs = (source.parent, ROOT / "modules/analog-classics/synth-batch", ROOT / "modules/analog-classics/synth-finish")
    command = ["faust", "-e"] + [flag for directory in include_dirs for flag in ("-I", str(directory))] + [str(source), "-o", str(destination)]
    raw = subprocess.run(command, text=True, capture_output=True)
    if raw.returncode:
        raise RuntimeError(raw.stderr or raw.stdout)
    # A legacy source can request a compiler JSON sidecar through its metadata.
    # It is not part of the portable review script and must not perturb its hash.
    destination.with_suffix(destination.suffix + ".json").unlink(missing_ok=True)
    destination.with_suffix(destination.suffix + ".xml").unlink(missing_ok=True)
    expanded = destination.read_text()
    normalized = normalize_expanded(expanded, source.read_text())
    destination.write_text(normalized)
    # The normalizer's invariant is intentionally checked again at the call site.
    code = lambda text: [x for x in text.splitlines() if not x.lstrip().startswith("declare ")]
    if code(normalized) != code(expanded):
        raise AssertionError("metadata normalization changed DSP expressions")
    subprocess.run(["faust", "-lang", "cpp", "-single", str(destination), "-o", str(destination.with_suffix(".hpp"))], check=True, capture_output=True, text=True)

def provenance(identity, source_root):
    if identity.startswith("909-"):
        return "Authored assembly; retained MIT notice for two extracted Mutable Instruments Plaits arithmetic functions (LICENSES.md)."
    if identity.startswith("808-aux-"):
        return "Authored Faust source; TapTools MIT revision is an oracle only, not copied runtime DSP."
    return "Authored Faust-expr source; expanded standard Faust library provenance is retained in exported declarations."

def run(out):
    out = out.resolve(); scripts = out / "scripts"; scripts.mkdir(parents=True, exist_ok=True)
    freeze = json.loads(FREEZE.read_text())
    entries = []
    for subject in SUBJECTS:
        identity, category, relative, rationale = subject[:4]
        source_root = ROOT
        source = source_root / relative; destination = scripts / f"{identity}.dsp"
        export_one(source, destination)
        meta = declarations(source.read_text())
        commit = next((sha for prefix, sha in PINNED_COMMITS.items() if identity.startswith(prefix)), subprocess.check_output(["git", "log", "-1", "--format=%H", "--", relative], cwd=source_root, text=True).strip())
        entries.append({"id": f"analog-classics:{identity}", "identity": identity, "displayName": meta.get("name", identity), "category": category, "version": 1, "soundVersion": meta.get("version", "not-declared"), "source": {"path": f"scripts/{destination.name}", "sha256": digest(destination)}, "upstream": {"commit": commit, "path": relative, "sha256": digest(source)}, "source_commit": commit, "source_path": relative, "source_sha256": digest(source), "export_path": f"scripts/{destination.name}", "export_sha256": digest(destination), "license": meta.get("license", provenance(identity, source_root)), "dependencyProvenance": provenance(identity, source_root), "dependencies": [], "status": "internal-review", "selectionRationale": rationale, "selection_rationale": rationale, "metadataAdaptation": "faust -e library expansion and metadata normalization only; expression lines are invariant and standalone compile succeeds"})
    for entry in entries:
        entry["lineageUuid"] = str(uuid.uuid5(uuid.NAMESPACE_URL, f"curlcomplex/CURLOP/{entry['id']}"))
    manifest = {"schema": 1, "identity": "analog-classics-internal-review-2026-09-15.1", "status": "internal-review-only", "base_review_freeze": {"path": str(FREEZE.relative_to(ROOT)), "sha256": digest(FREEZE), "identity": freeze["id"]}, "modules": entries, "selected": entries, "rejected_or_unselected": REJECTED, "contract": {"one_note": "lowercase gate, freq in Hz, velocity; host owns allocation", "distinct_events": "accent, slide, choke, clock, reset and run are never aliases", "outputs": "audio, CV and observations are separately declared", "identity": "stable identity is manifest identity plus version and source digest, never display text or geometry", "sound_change": "exports are metadata/library expansion only; a sonic change requires a new version and review freeze"}, "consumer_limits": ["No CURLOP runtime, UI, project-state, voice-allocation or device acceptance is claimed.", "Effect/modulation entries retain their native I/O rather than being mislabeled one-note instruments."]}
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")

if __name__ == "__main__":
    p = argparse.ArgumentParser(); p.add_argument("--out", type=Path, required=True)
    run(p.parse_args().out)
