"""Create the pinned, portable Analog Classics internal-review set.

The source registry is deliberately small and explicit: it is a review decision,
not a discovery rule that silently promotes a newer file.  ``faust -e`` expands
all Faust libraries; metadata normalisation is verified not to alter an
expression line. The resulting scripts are self-contained.
"""
from pathlib import Path
import argparse, hashlib, json, os, re, subprocess, sys, tempfile, uuid

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).parent))
from faust_export_metadata import normalize_expanded

FREEZE = ROOT / "modules/analog-classics/synth-finish/REVIEW_FREEZE.json"
DRUMS_909_SELECTION = ROOT / "modules/drums-909-reference/v1/selection.json"
SOURCE_TREE_BRANCH = "issue-118-analog-classics-review"
SOURCE_TREE_COMMIT = "149d14b9902167d595f30df1321a2de15c51afd6"
DEPENDENCY_FIXES = (
    {"commit": "b34bd21b9213f2810232f7e1bb128c4594c5c568", "scope": "analog-snare engine dependency namespace fix"},
    {"commit": "fbef71bfcfefc1fe6f3f3d368c6e7faefc4e92d5", "scope": "Trigger Sequencer engine dependency output fix"},
)
PINNED_COMMITS = {
    "606": "02f195e64cf274d0e995325fe2fd526d404bbe95",
    "909": "7aa75de6394568410d5b114d62b405138e9c9e35",
    "808-aux": "07464333c5eed1e916820a3fec99b69147561cb0",
}
PINNED_BRANCHES = {"606": "606-reference-tuning", "909": "80-909-reference-tuning", "808-aux": "75-808-fischer-reference-pass"}
ADAPTATION_EVIDENCE = {}
EXPORT_EVIDENCE = {}
FROZEN_PRESETS = {
 "606-low-tom": {"gate":0,"freq":137.55581,"velocity":1,"accent":0,"decay":.29948,"tone":.38612,"noise":.021917,"level":.8},
 "606-high-tom": {"gate":0,"freq":207.57604,"velocity":1,"accent":0,"decay":.205787,"tone":.162234,"noise":.102346,"level":.8},
}
SUPERSEDED_IDENTITIES = {
    "analog-classics:909-low-tom": {"replacement": "analog-classics:909-tom", "preset": "low-tom", "status": "superseded-preset-wrapper"},
    "analog-classics:909-mid-tom": {"replacement": "analog-classics:909-tom", "preset": "mid-tom", "status": "superseded-preset-wrapper"},
    "analog-classics:909-high-tom": {"replacement": "analog-classics:909-tom", "preset": "high-tom", "status": "superseded-preset-wrapper"},
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
    ("909-tom", "instrument", "modules/drums-909/v1/tom.dsp", "one canonical 909 Tom instrument; low/mid/high remain named recorded-anchor presets", "work/source-909"),
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

def tree_blob_digest(commit, path):
    blob = subprocess.check_output(["git", "show", f"{commit}:{path}"], cwd=ROOT)
    return hashlib.sha256(blob).hexdigest()

def declarations(text):
    result = {}
    for line in text.splitlines():
        if line.startswith("declare ") and ' "' in line:
            key, value = line[8:].split(' "', 1)
            result[key] = value.rsplit('";', 1)[0]
    return result

def control_defaults(text):
    defaults = {label.split("[", 1)[0]: 0 for label in re.findall(r'(?:button|checkbox)\("([^"]+)"', text)}
    for label, value in re.findall(r'hslider\("([^"]+)",\s*([-+0-9.eEfF]+)', text):
        defaults[label.split("[", 1)[0]] = float(value.rstrip("fF"))
    return defaults

def build_runner(source, folder, include_dirs=()):
    folder.mkdir(parents=True, exist_ok=True)
    command = [os.getenv("FAUST", "faust"), "-lang", "cpp", "-single", "-cn", "ModuleDSP"]
    command += [flag for directory in include_dirs for flag in ("-I", str(directory))]
    subprocess.run(command + [str(source), "-o", str(folder / "generated.hpp")], check=True, capture_output=True, text=True)
    subprocess.run([os.getenv("CXX", "c++"), "-std=c++17", "-O2", "-ffp-contract=off", "-I" + str(folder), str(ROOT / "tools/modules/render.cpp"), "-o", str(folder / "render")], check=True, capture_output=True, text=True)
    return folder / "render", (folder / "generated.hpp").read_text()

def rendered_equivalence(identity, original_text, exported):
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        original = root / "original.dsp"
        original.write_text(original_text)
        old, _ = build_runner(original, root / "old")
        new, _ = build_runner(exported, root / "new")
        old_score = root / "old.tsv"; new_score = root / "new.tsv"
        common = "0\tvelocity\t0.7\n64\tgate\t1\n4800\tgate\t0\n"
        old_controls = subprocess.check_output([str(old), "--controls"], text=True)
        old_address = "pitch_hz" if any(line.startswith("pitch_hz\t") for line in old_controls.splitlines()) else "freq"
        old_score.write_text(f"0\t{old_address}\t120\n" + common)
        new_score.write_text("0\tfreq\t120\n" + common)
        old_raw = root / "old.f32"; new_raw = root / "new.f32"
        old_diag = subprocess.check_output([str(old), str(old_score), str(old_raw), "48000", "64", "12000", "0"], text=True)
        new_diag = subprocess.check_output([str(new), str(new_score), str(new_raw), "48000", "64", "12000", "0"], text=True)
        if old_raw.read_bytes() != new_raw.read_bytes():
            raise AssertionError(identity + ": adapted render differs")
        stable_diagnostics = lambda text: {key:value for key,value in json.loads(text).items() if key != "instrumented_compute_ns"}
        return {"sampleRate":48000,"blockSize":64,"frames":12000,"oldAddress":old_address,"newAddress":"freq","audioSha256":digest(old_raw),"byteIdentical":True,"oldDiagnostics":stable_diagnostics(old_diag),"newDiagnostics":stable_diagnostics(new_diag)}

def dependency_provenance(source, include_dirs, compiler_version):
    dependencies = {}
    visited = set()

    def visit(path):
        path = path.resolve()
        if path in visited:
            return
        visited.add(path)
        text = path.read_text()
        for kind, name in re.findall(r'\b(import|library)\("([^"]+)"\)', text):
            candidates = (path.parent / name, *(directory / name for directory in include_dirs))
            dependency = next((candidate.resolve() for candidate in candidates if candidate.exists()), None)
            if dependency is None:
                dependencies[("standard-library", name)] = {
                    "kind": "standard-library", "name": name,
                    "compilerVersion": compiler_version,
                }
                continue
            relative = str(dependency.relative_to(ROOT))
            dependencies[("repository-library", relative)] = {
                "kind": "repository-library", "path": relative,
                "sha256": digest(dependency), "referencedAs": kind,
            }
            visit(dependency)

    visit(source)
    return [dependencies[key] for key in sorted(dependencies)]

def export_one(source, destination, identity, compiler_version):
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
    adapted = []
    before_adaptation = None
    if identity in {"analog-kick-sharp", "analog-snare", "clap"}:
        before_adaptation = normalized
        def replace(pattern, replacement):
            nonlocal normalized
            originals = sorted(set(re.findall(pattern, normalized)))
            normalized = re.sub(pattern, replacement, normalized)
            adapted.extend(originals)
        replace(r'hslider\("pitch_hz[^"\\]*"', 'hslider("freq[unit:Hz][scale:log][curlop:input]"')
        replace(r'button\("gate(?!\[curlop:input\])[^"\\]*"', 'button("gate[curlop:input]"')
        replace(r'hslider\("velocity(?!\[curlop:input\])[^"\\]*"', 'hslider("velocity[curlop:input]"')
        expected = {'hslider("pitch_hz"', 'button("gate"', 'hslider("velocity"'}
        if set(adapted) != expected or 'process' not in normalized or 'process' not in before_adaptation:
            raise AssertionError("adaptation whitelist proof failed")
        ADAPTATION_EVIDENCE[identity] = {"kind": "ui-address-only", "originalLabels": sorted(set(adapted)), "finalLabels": ["freq[unit:Hz][scale:log][curlop:input]", "gate[curlop:input]", "velocity[curlop:input]"], "onlyUiLabelStringsChanged": True}
        destination.write_text(normalized)
    if identity in FROZEN_PRESETS:
        for control, value in FROZEN_PRESETS[identity].items():
            if control in {"gate", "velocity", "accent"}:
                continue
            pattern = r'(hslider\("' + re.escape(control) + r'[^"\\]*",\s*)[-+0-9.eEfF]+'
            normalized, count = re.subn(pattern, r'\g<1>' + repr(value) + 'f', normalized)
            if count == 0:
                raise AssertionError(f"missing frozen preset control {identity}:{control}")
        destination.write_text(normalized)
    with tempfile.TemporaryDirectory() as tmp:
        runner, header = build_runner(destination, Path(tmp) / "compiled")
    channels = [int(re.search(r'getNumInputs\(\).*?return\s+(\d+);', header, re.S).group(1)), int(re.search(r'getNumOutputs\(\).*?return\s+(\d+);', header, re.S).group(1))]
    bars = re.findall(r'(?:hbargraph|vbargraph)\("([^"]+)"', normalized)
    imports = dependency_provenance(source, include_dirs, compiler_version)
    output_roles = [{"label":label,"role":"cv" if "[curlop:cvout]" in label else "observation" if "[curlop:meterout]" in label else "bargraph-unclassified"} for label in bars]
    evidence = {"compiledIo":{"audioInputs":channels[0],"signalOutputs":channels[1]},"namedOutputs":output_roles,"imports":imports,"controlDefaults":control_defaults(normalized)}
    if identity in ADAPTATION_EVIDENCE:
        evidence["renderedEquivalence"] = rendered_equivalence(identity, before_adaptation, destination)
    EXPORT_EVIDENCE[identity] = evidence
    destination.with_suffix(".hpp").write_text(header)

def provenance(identity, source_root):
    if identity.startswith("909-"):
        return "Authored assembly; retained MIT notice for two extracted Mutable Instruments Plaits arithmetic functions (LICENSES.md)."
    if identity.startswith("808-aux-"):
        return "Authored Faust source; TapTools MIT revision is an oracle only, not copied runtime DSP."
    return "Authored Faust-expr source; expanded standard Faust library provenance is retained in exported declarations."

def run(out):
    out = out.resolve(); scripts = out / "scripts"; scripts.mkdir(parents=True, exist_ok=True)
    for superseded in SUPERSEDED_IDENTITIES:
        (scripts / f"{superseded.split(':', 1)[1]}.dsp").unlink(missing_ok=True)
    freeze = json.loads(FREEZE.read_text())
    drums_909 = json.loads(DRUMS_909_SELECTION.read_text())["selections"]
    compiler_version = subprocess.check_output(["faust", "--version"], text=True).splitlines()[0]
    cxx_version = subprocess.check_output([os.getenv("CXX", "c++"), "--version"], text=True).splitlines()[0]
    entries = []
    for subject in SUBJECTS:
        identity, category, relative, rationale = subject[:4]
        source_root = ROOT
        source = source_root / relative; destination = scripts / f"{identity}.dsp"
        export_one(source, destination, identity, compiler_version)
        meta = declarations(source.read_text())
        source_file_commit = subprocess.check_output(["git", "log", "-1", "--format=%H", "--", relative], cwd=source_root, text=True).strip()
        lineage_commit = next((sha for prefix, sha in PINNED_COMMITS.items() if identity.startswith(prefix)), source_file_commit)
        lineage_branch = next((name for prefix, name in PINNED_BRANCHES.items() if identity.startswith(prefix)), "82-synth-reference-finish")
        if tree_blob_digest(SOURCE_TREE_COMMIT, relative) != digest(source):
            raise AssertionError(f"{identity}: source does not match pinned source tree")
        for dependency in EXPORT_EVIDENCE[identity]["imports"]:
            if dependency["kind"] == "repository-library" and tree_blob_digest(SOURCE_TREE_COMMIT, dependency["path"]) != dependency["sha256"]:
                raise AssertionError(f"{identity}: dependency does not match pinned source tree: {dependency['path']}")
        license_identifier = meta.get("license", "NOASSERTION")
        license_status = "declared" if "license" in meta else "unresolved-internal-review"
        if identity.startswith("909-") and "license" not in meta:
            license_identifier = "NOASSERTION (module); MIT (identified Plaits formulas)"
            license_status = "mixed-reviewed"
        entries.append({"id": f"analog-classics:{identity}", "identity": identity, "displayName": meta.get("name", identity), "category": category, "version": 1, "soundVersion": meta.get("version", "not-declared"), "source": {"path": f"scripts/{destination.name}", "sha256": digest(destination)}, "upstream": {"branch": SOURCE_TREE_BRANCH, "commit": SOURCE_TREE_COMMIT, "path": relative, "sha256": digest(source), "sourceFileLastChangeCommit": source_file_commit, "lineage": {"branch": lineage_branch, "commit": lineage_commit}}, "source_commit": SOURCE_TREE_COMMIT, "source_file_last_change_commit": source_file_commit, "source_path": relative, "source_sha256": digest(source), "export_path": f"scripts/{destination.name}", "export_sha256": digest(destination), "license": license_identifier, "licenseIdentifier": license_identifier, "licenseStatus": license_status, "dependencyProvenance": provenance(identity, source_root), "dependencies": EXPORT_EVIDENCE[identity]["imports"], "compiler": {"faustVersion": compiler_version, "cxxVersion": cxx_version, "exportOptions": ["-e", "-I", "<source-parent>", "-I", "modules/analog-classics/synth-batch", "-I", "modules/analog-classics/synth-finish"], "compileOptions": ["-lang", "cpp", "-single", "-cn", "ModuleDSP"], "runnerCompileOptions": ["-std=c++17", "-O2", "-ffp-contract=off"]}, "status": "internal-review", "selectionRationale": rationale, "selection_rationale": rationale, "metadataAdaptation": "faust -e library expansion and metadata normalization only; expression lines are invariant and standalone compile succeeds"})
    for entry in entries:
        entry["lineageUuid"] = str(uuid.uuid5(uuid.NAMESPACE_URL, f"curlcomplex/CURLOP/{entry['id']}"))
        labels = re.findall(r'(?:hslider|button|checkbox)\("([^"]+)"', (out / entry["export_path"]).read_text())
        required = {"gate[curlop:input]", "velocity[curlop:input]"}
        has_freq = any(label.startswith("freq[") and "[unit:Hz]" in label and "[curlop:input]" in label for label in labels)
        gaps = [] if entry["category"] != "instrument" or (required <= set(labels) and has_freq) else ["missing canonical tagged one-note input"]
        special = sorted({label.split("[", 1)[0] for label in labels} & {"accent", "slide", "chokeGate", "clock", "reset", "run"})
        entry["metadataAdaptation"] = ADAPTATION_EVIDENCE.get(entry["identity"], "faust expansion and metadata normalization only")
        entry["dependencies"] = EXPORT_EVIDENCE[entry["identity"]]["imports"]
        entry["outputEvidence"] = EXPORT_EVIDENCE[entry["identity"]]["compiledIo"] | {"namedOutputs": EXPORT_EVIDENCE[entry["identity"]]["namedOutputs"]}
        if "renderedEquivalence" in EXPORT_EVIDENCE[entry["identity"]]:
            entry["metadataAdaptation"]["renderedAudioEquivalence"] = EXPORT_EVIDENCE[entry["identity"]]["renderedEquivalence"]
        if entry["identity"] in FROZEN_PRESETS:
            entry["frozenPresetSettings"] = FROZEN_PRESETS[entry["identity"]]
            entry["adaptedDefaults"] = {k:v for k,v in FROZEN_PRESETS[entry["identity"]].items() if k not in {"gate", "velocity", "accent"}}
            family, voice = entry["identity"].split("-", 1)
            entry["displayName"] = family + " " + voice.replace("-", " ").title()
            entry["displayMetadataAdaptation"] = {"onlyUiMetadataChanged": True, "displayName": entry["displayName"]}
        if entry["identity"] == "909-tom":
            entry["canonicalDefaultState"] = {"kind": "source-defaults", "settings": EXPORT_EVIDENCE[entry["identity"]]["controlDefaults"]}
            entry["namedPresets"] = [{"name": preset.replace("-", " ").title(), "presetId": preset, "status": "frozen-reference-anchor", "reference": drums_909[preset]["reference"], "settings": drums_909[preset]["settings"]} for preset in ("low-tom", "mid-tom", "high-tom")]
        entry["contractEvidence"] = {"capturedControls": sorted(set(labels)), "canonicalOneNote": entry["category"] != "instrument" or not gaps, "gaps": gaps, "specialEvents": special, "metadataAdapted": entry["identity"] in ADAPTATION_EVIDENCE}
    manifest = {"schema": 1, "identity": "analog-classics-internal-review-2026-09-15.2", "status": "internal-review-only", "sourceTree": {"branch": SOURCE_TREE_BRANCH, "commit": SOURCE_TREE_COMMIT, "contract": "Every recorded source and repository-library dependency hash is the blob at this exact commit; file-level lineage commits are informational only.", "dependencyFixes": DEPENDENCY_FIXES}, "base_review_freeze": {"path": str(FREEZE.relative_to(ROOT)), "sha256": digest(FREEZE), "identity": freeze["id"]}, "modules": entries, "selected": entries, "supersededIdentities": SUPERSEDED_IDENTITIES, "rejected_or_unselected": REJECTED, "contract": {"one_note": "lowercase gate, freq in Hz, velocity; host owns allocation", "distinct_events": "accent, slide, choke, clock, reset and run are never aliases", "outputs": "audio, CV and observations are separately declared", "identity": "stable identity is manifest identity plus version and source digest, never display text or geometry", "sound_change": "exports are metadata/library expansion only; a sonic change requires a new version and review freeze"}, "consumer_limits": ["No CURLOP runtime, UI, project-state, voice-allocation or device acceptance is claimed.", "Effect/modulation entries retain their native I/O rather than being mislabeled one-note instruments."]}
    manifest["schema"] = "curlop-analog-classics-review/v1"
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")

if __name__ == "__main__":
    p = argparse.ArgumentParser(); p.add_argument("--out", type=Path, required=True)
    run(p.parse_args().out)
