"""Create the pinned, portable Analog Classics internal-review set.

The source registry is deliberately small and explicit: it is a review decision,
not a discovery rule that silently promotes a newer file.  ``faust -e`` expands
all Faust libraries; metadata normalisation is verified not to alter an
expression line. The resulting scripts are self-contained.
"""
from pathlib import Path
import argparse, hashlib, json, subprocess, sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).parent))
from faust_export_metadata import normalize_expanded

FREEZE = ROOT / "modules/analog-classics/synth-finish/REVIEW_FREEZE.json"
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
)
REJECTED = {
    "modules/juno-60/candidates/pr89-0b98748d/voice.dsp": "owner sonic rejection recorded in PR89/PR90 reconciliation",
    "modules/minimoog/v2/voice.dsp": "high-cutoff held-note collapse; engineering fixture only",
    "modules/minimoog/v3/voice.dsp": "numerically repaired experiment without demonstrated whole-instrument advantage",
    "modules/analog-classics/drums-v1": "exploratory v1 drum studies retained, not selected",
    "modules/hats-analog/v1": "superseded paired-hat baseline retained as negative control",
    "modules/clap/v1": "superseded clap identity",
    "modules/clap/v2": "rejected pitched-body clap identity",
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
    expanded = destination.read_text()
    normalized = normalize_expanded(expanded, source.read_text())
    destination.write_text(normalized)
    # The normalizer's invariant is intentionally checked again at the call site.
    code = lambda text: [x for x in text.splitlines() if not x.lstrip().startswith("declare ")]
    if code(normalized) != code(expanded):
        raise AssertionError("metadata normalization changed DSP expressions")
    subprocess.run(["faust", "-lang", "cpp", "-single", str(destination), "-o", str(destination.with_suffix(".hpp"))], check=True, capture_output=True, text=True)

def run(out):
    out = out.resolve(); scripts = out / "scripts"; scripts.mkdir(parents=True, exist_ok=True)
    freeze = json.loads(FREEZE.read_text())
    entries = []
    for identity, category, relative, rationale in SUBJECTS:
        source = ROOT / relative; destination = scripts / f"{identity}.dsp"
        export_one(source, destination)
        meta = declarations(source.read_text())
        commit = subprocess.check_output(["git", "log", "-1", "--format=%H", "--", relative], cwd=ROOT, text=True).strip()
        entries.append({"id": f"analog-classics:{identity}", "identity": identity, "displayName": meta.get("name", identity), "category": category, "version": 1, "soundVersion": meta.get("version", "not-declared"), "source": {"path": f"scripts/{destination.name}", "sha256": digest(destination)}, "upstream": {"commit": commit, "path": relative, "sha256": digest(source)}, "source_commit": commit, "source_path": relative, "source_sha256": digest(source), "export_path": f"scripts/{destination.name}", "export_sha256": digest(destination), "license": meta.get("license", "see source/provenance; no undeclared license inferred"), "dependencies": [], "status": "internal-review", "selectionRationale": rationale, "selection_rationale": rationale, "metadataAdaptation": "faust -e library expansion and metadata normalization only; expression lines are invariant and standalone compile succeeds"})
    manifest = {"schema": 1, "identity": "analog-classics-internal-review-2026-09-15.1", "status": "internal-review-only", "base_review_freeze": {"path": str(FREEZE.relative_to(ROOT)), "sha256": digest(FREEZE), "identity": freeze["id"]}, "modules": entries, "selected": entries, "rejected_or_unselected": REJECTED, "contract": {"one_note": "lowercase gate, freq in Hz, velocity; host owns allocation", "distinct_events": "accent, slide, choke, clock, reset and run are never aliases", "outputs": "audio, CV and observations are separately declared", "identity": "stable identity is manifest identity plus version and source digest, never display text or geometry", "sound_change": "exports are metadata/library expansion only; a sonic change requires a new version and review freeze"}, "consumer_limits": ["No CURLOP runtime, UI, project-state, voice-allocation or device acceptance is claimed.", "Effect/modulation entries retain their declared I/O rather than being mislabeled one-note instruments."]}
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")

if __name__ == "__main__":
    p = argparse.ArgumentParser(); p.add_argument("--out", type=Path, required=True)
    run(p.parse_args().out)
