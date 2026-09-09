// Hand-maintained factory module registry. C++ is the source of truth.
//
// History: previously auto-generated from src/data/modules/*.js by the
// retired scripts/codegen/gen-module-registry.mjs codegen. JS deprecation
// (M-001 closure) makes this file the canonical schema source. Add new
// modules by editing this file directly.
//
// Schema reference: native/audio/ModuleTypes.h::ParamSchemaEntry. Wire
// parser parity is enforced by native/EventRouter.cpp::parseModuleSchema
// (consumed when a runtime user-authored USER_REGISTER_MODULE arrives — not
// at boot any more).

#pragma once

#include <string>
#include <unordered_map>
#include <cctype>
#include <cstdio>
#include "modules/contract/ModuleTypes.h"

namespace curlop {
namespace vm { struct ModuleDeclaration; }

inline ParamSchemaEntry sequencerParam(const std::string& name,
                                       ParamType type,
                                       float min,
                                       float max,
                                       float def,
                                       const std::string& unit,
                                       const std::string& widget,
                                       int column,
                                       int row)
{
    ParamSchemaEntry p;
    p.name = name;
    p.type = type;
    p.min = min;
    p.max = max;
    p.defaultValue = def;
    p.unit = unit;
    p.scale = Scale::Linear;
    p.widget = widget;
    p.column = column;
    p.row = row;
    p.width = 1;
    p.height = 1;
    p.orientation = "";
    p.hidden = false;
    p.mode = "";
    p.interactive = true;
    p.accent = "";
    p.size = 1.0f;
    p.xParam = "";
    p.yParam = "";
    p.acceptance = ParamAcceptance::ControlValue;
    return p;
}

inline void appendStepParams(ModuleSchema& s, int firstRow)
{
    for (int i = 0; i < 16; ++i) {
        char name[16];
        std::snprintf(name, sizeof(name), "STEP_%02d", i + 1);
        s.params.push_back(sequencerParam(
            name, ParamType::Continuous, 0.0f, 1.0f, (i % 4) == 0 ? 1.0f : 0.0f,
            "", "toggle", i % 8, firstRow + (i / 8)));
    }
}

inline void appendPitchStepParams(ModuleSchema& s, int firstRow)
{
    static constexpr float kSemi = 1.0f / 120.0f;
    static constexpr int kDefaultSemis[16] = {
        0, 2, 4, 7, 12, 7, 4, 2, 0, -3, -5, -8, -12, -8, -5, -3
    };
    for (int i = 0; i < 16; ++i) {
        char name[16];
        std::snprintf(name, sizeof(name), "PITCH_%02d", i + 1);
        s.params.push_back(sequencerParam(
            name, ParamType::Continuous, -1.0f, 1.0f, kDefaultSemis[i] * kSemi,
            "pitch", "knob", i % 8, firstRow + (i / 8)));
    }
}

inline void appendStepSequencerParams(ModuleSchema& s)
{
    s.params.push_back(sequencerParam(
        "RATE", curlop::ParamType::Continuous, -1.0f, 1.0f, 1.0f,
        "x", "knob", 0, 0));
    s.params.push_back(sequencerParam(
        "INC", curlop::ParamType::Boolean, 0.0f, 1.0f, 0.0f,
        "", "button", 1, 0));
    s.params.push_back(sequencerParam(
        "DEC", curlop::ParamType::Boolean, 0.0f, 1.0f, 0.0f,
        "", "button", 2, 0));
    s.params.push_back(sequencerParam(
        "RESET", curlop::ParamType::Boolean, 0.0f, 1.0f, 0.0f,
        "", "button", 3, 0));
    s.params.push_back(sequencerParam(
        "SELECT", curlop::ParamType::Continuous, 0.0f, 16.0f, 0.0f,
        "step", "knob", 4, 0));
    s.params.push_back(sequencerParam(
        "VELOCITY", curlop::ParamType::Continuous, 0.0f, 1.0f, 0.7f,
        "", "knob", 5, 0));
    appendStepParams(s, 1);
    appendPitchStepParams(s, 3);
}

// Factory modules. Each carries a stable, hard-coded UUID-v4 `lineageUuid`
// (the identity model's `lineage_id`) — generated once, never changes. The
// dotted `lineageId` is the registry key (the model's `id`). See
// .planning/specs/MODULE-IDENTITY-SPEC.md.
inline void populateFactoryModuleRegistry(
    std::unordered_map<std::string, curlop::ModuleSchema>& reg)
{
    // ── core.host_automation (Host Automation) ──
    {
        curlop::ModuleSchema s;
        s.lineageId   = "core.host_automation";
        s.lineageUuid = "29d1e3a1-c1e8-4d04-8adf-0da1c5f49a63";
        s.name        = "Host Automation";
        s.moduleType  = "sequencer";
        s.flavour     = "control";
        s.category    = "Plugin";
        s.tier        = "cobalt";
        s.description = "DAW automation bank source with eight selectable normalized control outputs.";
        s.voices      = 1;
        s.stealPolicy = curlop::StealPolicy::LastStolen;
        s.outputs.emplace_back("lane_a");
        s.outputs.emplace_back("lane_b");
        s.outputs.emplace_back("lane_c");
        s.outputs.emplace_back("lane_d");
        s.outputs.emplace_back("lane_e");
        s.outputs.emplace_back("lane_f");
        s.outputs.emplace_back("lane_g");
        s.outputs.emplace_back("lane_h");
        s.params.push_back(sequencerParam(
            "LANE_A", curlop::ParamType::Integer, 1.0f, 128.0f, 1.0f,
            "", "knob", 0, 0));
        s.params.push_back(sequencerParam(
            "LANE_B", curlop::ParamType::Integer, 1.0f, 128.0f, 2.0f,
            "", "knob", 1, 0));
        s.params.push_back(sequencerParam(
            "LANE_C", curlop::ParamType::Integer, 1.0f, 128.0f, 3.0f,
            "", "knob", 2, 0));
        s.params.push_back(sequencerParam(
            "LANE_D", curlop::ParamType::Integer, 1.0f, 128.0f, 4.0f,
            "", "knob", 3, 0));
        s.params.push_back(sequencerParam(
            "LANE_E", curlop::ParamType::Integer, 1.0f, 128.0f, 5.0f,
            "", "knob", 4, 0));
        s.params.push_back(sequencerParam(
            "LANE_F", curlop::ParamType::Integer, 1.0f, 128.0f, 6.0f,
            "", "knob", 5, 0));
        s.params.push_back(sequencerParam(
            "LANE_G", curlop::ParamType::Integer, 1.0f, 128.0f, 7.0f,
            "", "knob", 6, 0));
        s.params.push_back(sequencerParam(
            "LANE_H", curlop::ParamType::Integer, 1.0f, 128.0f, 8.0f,
            "", "knob", 7, 0));
        reg["core.host_automation"] = std::move(s);
    }

    // ── core.midi_in (MIDI In) ──
    {
        curlop::ModuleSchema s;
        s.lineageId   = "core.midi_in";
        s.lineageUuid = "dd15ee2b-67e6-4c4b-9e1b-c817dd60b701";
        s.name        = "MIDI In";
        s.moduleType  = "sequencer";
        s.flavour     = "control";
        s.category    = "Plugin";
        s.tier        = "cobalt";
        s.description = "MIDI input as routeable gate, pitch, velocity, and CC control streams.";
        s.voices      = 1;
        s.stealPolicy = curlop::StealPolicy::LastStolen;
        s.outputs.emplace_back("gate");
        s.outputs.emplace_back("pitch");
        s.outputs.emplace_back("velocity");
        s.outputs.emplace_back("cc");
        s.params.push_back(sequencerParam(
            "CHANNEL", curlop::ParamType::Integer, 1.0f, 16.0f, 1.0f,
            "", "knob", 0, 0));
        s.params.push_back(sequencerParam(
            "CC", curlop::ParamType::Integer, 0.0f, 127.0f, 1.0f,
            "", "knob", 1, 0));
        reg["core.midi_in"] = std::move(s);
    }

    // ── core.midi_out (MIDI Out) ──
    {
        curlop::ModuleSchema s;
        s.lineageId   = "core.midi_out";
        s.lineageUuid = "aab018a0-e016-439a-a092-8e5b47e209bd";
        s.name        = "MIDI Out";
        s.moduleType  = "effect";
        s.flavour     = "control";
        s.category    = "Plugin";
        s.tier        = "cobalt";
        s.description = "Explicit graph sink for outgoing MIDI note, pitch-bend, and CC events.";
        s.voices      = 1;
        s.stealPolicy = curlop::StealPolicy::LastStolen;
        s.params.push_back(sequencerParam(
            "GATE", curlop::ParamType::Continuous, 0.0f, 1.0f, 0.0f,
            "", "button", 0, 0));
        s.params.push_back(sequencerParam(
            "PITCH", curlop::ParamType::Continuous, -1.0f, 1.0f, 0.0f,
            "pitch", "knob", 1, 0));
        s.params.push_back(sequencerParam(
            "VELOCITY", curlop::ParamType::Continuous, 0.0f, 1.0f, 0.7f,
            "", "knob", 2, 0));
        s.params.push_back(sequencerParam(
            "CHANNEL", curlop::ParamType::Integer, 1.0f, 16.0f, 1.0f,
            "", "knob", 3, 0));
        s.params.push_back(sequencerParam(
            "CC", curlop::ParamType::Integer, 0.0f, 127.0f, 1.0f,
            "", "knob", 4, 0));
        s.params.push_back(sequencerParam(
            "CC_VALUE", curlop::ParamType::Continuous, -1.0f, 1.0f, -1.0f,
            "", "knob", 5, 0));
        reg["core.midi_out"] = std::move(s);
    }

    // ── core.transport_clock (Transport Clock) ──
    {
        curlop::ModuleSchema s;
        s.lineageId   = "core.transport_clock";
        s.lineageUuid = "f2be2a88-c1d3-4c57-a3e1-9c9ef7f1734b";
        s.name        = "Transport Clock";
        s.moduleType  = "sequencer";
        s.flavour     = "control";
        s.category    = "Script";
        s.tier        = "cobalt";
        s.description = "Patchable transport clock source with play, pulse, phase, bar, beat, and BPM outputs.";
        s.voices      = 1;
        s.stealPolicy = curlop::StealPolicy::LastStolen;
        s.outputs.emplace_back("play_gate");
        s.outputs.emplace_back("start");
        s.outputs.emplace_back("reset");
        s.outputs.emplace_back("clock");
        s.outputs.emplace_back("phase");
        s.outputs.emplace_back("beat_phase");
        s.outputs.emplace_back("bar_phase");
        s.outputs.emplace_back("bpm");
        s.params.push_back(sequencerParam(
            "STEPS_PER_BAR", curlop::ParamType::Integer, 1.0f, 64.0f, 16.0f,
            "steps/bar", "knob", 0, 0));
        s.params.push_back(sequencerParam(
            "PULSE_WIDTH", curlop::ParamType::Continuous, 0.001f, 1.0f, 0.5f,
            "", "knob", 1, 0));
        reg["core.transport_clock"] = std::move(s);
    }

    // ── core.trigger_seq (Trigger Sequencer) ──
    {
        curlop::ModuleSchema s;
        s.lineageId   = "core.trigger_seq";
        s.lineageUuid = "b49f7eed-b883-4e96-a910-48a0d16ef7e1";
        s.name        = "Trigger Seq";
        s.moduleType  = "sequencer";
        s.flavour     = "control";
        s.category    = "Script";
        s.tier        = "cobalt";
        s.description = "16-step trigger sequencer with a routeable gate control output.";
        s.voices      = 1;
        s.stealPolicy = curlop::StealPolicy::LastStolen;
        s.outputs.emplace_back("gate");
        s.params.push_back(sequencerParam(
            "RATE", curlop::ParamType::Continuous, -1.0f, 1.0f, 1.0f,
            "x", "knob", 0, 0));
        s.params.push_back(sequencerParam(
            "INC", curlop::ParamType::Boolean, 0.0f, 1.0f, 0.0f,
            "", "button", 1, 0));
        s.params.push_back(sequencerParam(
            "DEC", curlop::ParamType::Boolean, 0.0f, 1.0f, 0.0f,
            "", "button", 2, 0));
        s.params.push_back(sequencerParam(
            "RESET", curlop::ParamType::Boolean, 0.0f, 1.0f, 0.0f,
            "", "button", 3, 0));
        s.params.push_back(sequencerParam(
            "SELECT", curlop::ParamType::Continuous, 0.0f, 16.0f, 0.0f,
            "step", "knob", 4, 0));
        appendStepParams(s, 1);
        reg["core.trigger_seq"] = std::move(s);
    }

    // ── core.stepseq (Step Sequencer) ──
    {
        curlop::ModuleSchema s;
        s.lineageId   = "core.stepseq";
        s.lineageUuid = "50b4b5a6-d2b4-49b6-b81c-c9788392f142";
        s.name        = "Step Seq";
        s.moduleType  = "sequencer";
        s.flavour     = "control";
        s.category    = "Script";
        s.tier        = "cobalt";
        s.description = "16-step graph sequencer with gate, pitch, and velocity control outputs.";
        s.voices      = 1;
        s.stealPolicy = curlop::StealPolicy::LastStolen;
        s.outputs.emplace_back("gate");
        s.outputs.emplace_back("pitch");
        s.outputs.emplace_back("velocity");
        appendStepSequencerParams(s);
        reg["core.stepseq"] = std::move(s);
    }

    // ── core.pitch_seq (Pitch Sequencer) ──
    {
        curlop::ModuleSchema s;
        s.lineageId   = "core.pitch_seq";
        s.lineageUuid = "439795e1-f050-422a-bbe7-3f8041f6ba83";
        s.name        = "Pitch Seq";
        s.moduleType  = "sequencer";
        s.flavour     = "control";
        s.category    = "Script";
        s.tier        = "cobalt";
        s.description = "16-step pitch sequencer with gate, pitch, and velocity control outputs.";
        s.voices      = 1;
        s.stealPolicy = curlop::StealPolicy::LastStolen;
        s.outputs.emplace_back("gate");
        s.outputs.emplace_back("pitch");
        s.outputs.emplace_back("velocity");
        appendStepSequencerParams(s);
        reg["core.pitch_seq"] = std::move(s);
    }

    // ── core.script_v2 (Script V2) — CON-005 V2 syntax module ──
    {
        curlop::ModuleSchema s;
        s.lineageId   = "core.script_v2";
        s.lineageUuid = "9c46e7c6-62ec-4b93-9c42-17d142c5b8e7";
        s.name        = "Script V2";
        s.moduleType  = "script";
        s.flavour     = "control";
        s.category    = "Script";
        s.tier        = "cobalt";
        s.description = "V2 script sequencer with graph input and output sockets";
        s.voices      = 1;
        s.stealPolicy = curlop::StealPolicy::LastStolen;
        s.outputs.emplace_back("gate");
        s.outputs.emplace_back("pitch");
        s.outputs.emplace_back("velocity");
        reg["core.script_v2"] = std::move(s);
    }

    // ── core.buffer (Buffer) — native audio-as-addressable-data player/recorder ──
    {
        curlop::ModuleSchema s;
        s.lineageId   = "core.buffer";
        s.lineageUuid = "46d87426-5b9f-41f1-978f-d76fe887e1bc";
        s.name        = "Buffer";
        s.moduleType  = "effect";
        s.category    = "Buffer";
        s.tier        = "cobalt";
        s.description = "Native buffer player/recorder — patch audio in, capture with REC_GATE, play captured material with signed-rate scrub.";
        s.voices      = 1;
        s.stealPolicy = curlop::StealPolicy::LastStolen;
        s.inputs.emplace_back("IN");
        s.outputs.emplace_back("OUT");
        { curlop::ParamSchemaEntry p; p.name="PLAY_GATE"; p.type=curlop::ParamType::Boolean; p.min=0.0f; p.max=1.0f; p.defaultValue=0.0f; p.unit=""; p.scale=curlop::Scale::Linear; p.widget="button"; p.column=1; p.row=0; p.width=1; p.height=1; p.orientation=""; p.hidden=false; p.mode=""; p.interactive=true; p.accent=""; p.size=1.0f; p.xParam=""; p.yParam=""; p.acceptance=curlop::ParamAcceptance::ControlValue; s.params.push_back(std::move(p)); }
        { curlop::ParamSchemaEntry p; p.name="REC_GATE"; p.type=curlop::ParamType::Boolean; p.min=0.0f; p.max=1.0f; p.defaultValue=0.0f; p.unit=""; p.scale=curlop::Scale::Linear; p.widget="button"; p.column=0; p.row=0; p.width=1; p.height=1; p.orientation=""; p.hidden=false; p.mode=""; p.interactive=true; p.accent=""; p.size=1.0f; p.xParam=""; p.yParam=""; p.acceptance=curlop::ParamAcceptance::ControlValue; s.params.push_back(std::move(p)); }
        { curlop::ParamSchemaEntry p; p.name="POSITION"; p.type=curlop::ParamType::Continuous; p.min=0.0f; p.max=1.0f; p.defaultValue=0.0f; p.unit=""; p.scale=curlop::Scale::Linear; p.widget="knob"; p.column=0; p.row=1; p.width=1; p.height=1; p.orientation=""; p.hidden=false; p.mode=""; p.interactive=true; p.accent=""; p.size=1.0f; p.xParam=""; p.yParam=""; p.acceptance=curlop::ParamAcceptance::ControlValue; s.params.push_back(std::move(p)); }
        { curlop::ParamSchemaEntry p; p.name="RATE"; p.type=curlop::ParamType::Continuous; p.min=-4.0f; p.max=4.0f; p.defaultValue=1.0f; p.unit="x"; p.scale=curlop::Scale::Linear; p.widget="knob"; p.column=1; p.row=1; p.width=1; p.height=1; p.orientation=""; p.hidden=false; p.mode="bipolar"; p.interactive=true; p.accent=""; p.size=1.0f; p.xParam=""; p.yParam=""; p.acceptance=curlop::ParamAcceptance::ControlValue; s.params.push_back(std::move(p)); }
        { curlop::ParamSchemaEntry p; p.name="START"; p.type=curlop::ParamType::Continuous; p.min=0.0f; p.max=1.0f; p.defaultValue=0.0f; p.unit=""; p.scale=curlop::Scale::Linear; p.widget="knob"; p.column=0; p.row=2; p.width=1; p.height=1; p.orientation=""; p.hidden=false; p.mode=""; p.interactive=true; p.accent=""; p.size=1.0f; p.xParam=""; p.yParam=""; p.acceptance=curlop::ParamAcceptance::ControlValue; s.params.push_back(std::move(p)); }
        { curlop::ParamSchemaEntry p; p.name="END"; p.type=curlop::ParamType::Continuous; p.min=0.0f; p.max=1.0f; p.defaultValue=1.0f; p.unit=""; p.scale=curlop::Scale::Linear; p.widget="knob"; p.column=1; p.row=2; p.width=1; p.height=1; p.orientation=""; p.hidden=false; p.mode=""; p.interactive=true; p.accent=""; p.size=1.0f; p.xParam=""; p.yParam=""; p.acceptance=curlop::ParamAcceptance::ControlValue; s.params.push_back(std::move(p)); }
        { curlop::ParamSchemaEntry p; p.name="LOOP"; p.type=curlop::ParamType::Boolean; p.min=0.0f; p.max=1.0f; p.defaultValue=1.0f; p.unit=""; p.scale=curlop::Scale::Linear; p.widget="toggle"; p.column=2; p.row=0; p.width=1; p.height=1; p.orientation=""; p.hidden=false; p.mode=""; p.interactive=true; p.accent=""; p.size=1.0f; p.xParam=""; p.yParam=""; p.acceptance=curlop::ParamAcceptance::ControlValue; s.params.push_back(std::move(p)); }
        { curlop::ParamSchemaEntry p; p.name="INTERP"; p.type=curlop::ParamType::Integer; p.min=0.0f; p.max=4.0f; p.defaultValue=1.0f; p.unit=""; p.scale=curlop::Scale::Linear; p.widget="knob"; p.column=2; p.row=2; p.width=1; p.height=1; p.orientation=""; p.hidden=false; p.mode=""; p.interactive=true; p.accent=""; p.size=1.0f; p.xParam=""; p.yParam=""; p.acceptance=curlop::ParamAcceptance::ControlValue; s.params.push_back(std::move(p)); }
        { curlop::ParamSchemaEntry p; p.name="GAIN"; p.type=curlop::ParamType::Continuous; p.min=0.0f; p.max=1.0f; p.defaultValue=0.4f; p.unit=""; p.scale=curlop::Scale::Linear; p.widget="knob"; p.column=2; p.row=1; p.width=1; p.height=1; p.orientation=""; p.hidden=false; p.mode=""; p.interactive=true; p.accent=""; p.size=1.0f; p.xParam=""; p.yParam=""; p.acceptance=curlop::ParamAcceptance::ControlValue; s.params.push_back(std::move(p)); }
        { curlop::ParamSchemaEntry p; p.name="FADE_IN_MS"; p.type=curlop::ParamType::Continuous; p.min=0.0f; p.max=100.0f; p.defaultValue=2.0f; p.unit="ms"; p.scale=curlop::Scale::Linear; p.widget="knob"; p.column=3; p.row=1; p.width=1; p.height=1; p.orientation=""; p.hidden=false; p.mode=""; p.interactive=true; p.accent=""; p.size=1.0f; p.xParam=""; p.yParam=""; p.acceptance=curlop::ParamAcceptance::ControlValue; s.params.push_back(std::move(p)); }
        { curlop::ParamSchemaEntry p; p.name="FADE_OUT_MS"; p.type=curlop::ParamType::Continuous; p.min=0.0f; p.max=100.0f; p.defaultValue=2.0f; p.unit="ms"; p.scale=curlop::Scale::Linear; p.widget="knob"; p.column=3; p.row=2; p.width=1; p.height=1; p.orientation=""; p.hidden=false; p.mode=""; p.interactive=true; p.accent=""; p.size=1.0f; p.xParam=""; p.yParam=""; p.acceptance=curlop::ParamAcceptance::ControlValue; s.params.push_back(std::move(p)); }
        reg["core.buffer"] = std::move(s);
    }

    // ── core.visual_shader — render-domain authored ISF sink ──
    {
        curlop::ModuleSchema s;
        s.lineageId   = "core.visual_shader";
        s.lineageUuid = "50eddb63-1987-4a28-a6c9-896e1bdbf271";
        s.name        = "Visual Shader";
        s.moduleType  = "visual";
        s.flavour     = "control";
        s.category    = "Visual";
        s.tier        = "cobalt";
        s.description = "Live single-pass ISF shader rendered behind the graph.";
        s.voices      = 1;
        s.stealPolicy = curlop::StealPolicy::LastStolen;
        reg["core.visual_shader"] = std::move(s);
    }
    // ── core.visual_output — per-clip terminal feeding the visual master ──
    {
        curlop::ModuleSchema s;
        s.lineageId   = "core.visual_output";
        s.lineageUuid = "12df2701-4d3d-4de2-a3b8-87a9f48a1930";
        s.name        = "Visual Output";
        s.moduleType  = "visual";
        s.flavour     = "control";
        s.category    = "Visual";
        s.tier        = "cobalt";
        s.description = "Terminal texture input for the project visual master.";
        s.voices      = 1;
        s.stealPolicy = curlop::StealPolicy::LastStolen;
        reg["core.visual_output"] = std::move(s);
    }

    // F-056: core.faust_jit registry entry — desktop only (Faust JIT excised on iOS).
#if CURLOP_ENABLE_FAUST_RUNTIME
    // ── core.faust_jit (FaustJit) — Phase 1 (s438) ─────────────────────────
    // Faust LLVM-JIT-backed audio module. Ships with placeholder source
    // (continuous sine + GAIN slider) so the node is audible on add. Phase 2
    // wires per-instance source persistence + editor-driven hot-swap and
    // promotes this schema from placeholder-static to per-instance-derived.
    {
        curlop::ModuleSchema s;
        s.lineageId   = "core.faust_jit";
        s.lineageUuid = "42edaa67-2165-4609-a4fb-80079b3d7247";
        s.name        = "FaustJit";
        s.moduleType  = "synth";
        s.category    = "Synth";
        s.tier        = "cobalt";
        s.description = "Faust LLVM-JIT module — edit a .dsp script, hear it. Placeholder: sine + GAIN.";
        s.voices      = 1;
        s.stealPolicy = curlop::StealPolicy::LastStolen;   // T-385 ADR-008 explicit voice-policy declaration (every factory module is mono today; opt-out when SF-052 polyphony lands)
        s.outputs.emplace_back("OUT");
        // SF-093: NO hardcoded geometry. This static schema is only the pre-
        // hydration fallback (the real surface is per-instance, synthesised from
        // the .dsp by FaustUiCapture). It must MATCH what the sizeless
        // placeholderSource() produces — single cell, auto-flow (column/row -1) —
        // so the default module's knobs follow its Faust script, not a registry
        // constant. Author bigger knobs with [w:N][h:N] in the .dsp.
        { curlop::ParamSchemaEntry p; p.name="FREQ"; p.type=curlop::ParamType::Continuous; p.min=20.0f; p.max=8000.0f; p.defaultValue=440.0f; p.unit="hz"; p.scale=curlop::Scale::Logarithmic; p.widget="knob"; p.column=-1; p.row=-1; p.width=1; p.height=1; p.orientation=""; p.hidden=false; p.mode=""; p.interactive=true; p.accent=""; p.size=1.0f; p.xParam=""; p.yParam=""; p.acceptance=curlop::ParamAcceptance::ControlValue; s.params.push_back(std::move(p)); }
        { curlop::ParamSchemaEntry p; p.name="GAIN"; p.type=curlop::ParamType::Continuous; p.min=0.0f; p.max=1.0f; p.defaultValue=0.3f; p.unit=""; p.scale=curlop::Scale::Linear; p.widget="knob"; p.column=-1; p.row=-1; p.width=1; p.height=1; p.orientation=""; p.hidden=false; p.mode=""; p.interactive=true; p.accent=""; p.size=1.0f; p.xParam=""; p.yParam=""; p.acceptance=curlop::ParamAcceptance::ControlValue; s.params.push_back(std::move(p)); }
        reg["core.faust_jit"] = std::move(s);
    }
#endif  // CURLOP_ENABLE_FAUST_RUNTIME

    // ── core.audio_input (Audio Input) — T-836 / FF-032 Section 3E ──
    {
        curlop::ModuleSchema s;
        s.lineageId   = "core.audio_input";
        s.lineageUuid = "96f1e866-bcb0-4519-ab33-e4a3217b246e";
        s.name        = "Audio Input";
        s.moduleType  = "synth";
        s.category    = "Utility";
        s.tier        = "cobalt";
        s.description =
            "Mono source for one numbered channel of the current audio input bus.";
        s.voices      = 1;
        s.stealPolicy = curlop::StealPolicy::LastStolen;
        s.outputs.emplace_back("AUDIO");
        s.signalOutputs.push_back({
            "AUDIO",
            curlop::transport::SignalDescriptor::legacyMono()
        });
        reg["core.audio_input"] = std::move(s);
    }

    // ── output (Output) — from Output.js ──
    {
        curlop::ModuleSchema s;
        s.lineageId   = "output";
        s.lineageUuid = "161ecfda-7679-43db-a140-c1f9d9153134";
        s.name        = "Output";
        s.moduleType  = "effect";
        s.category    = "Utility";
        s.tier        = "cobalt";
        s.description = "Audio sink — final stereo output with master level. One cable in, audio out.";
        s.voices      = 1;
        s.stealPolicy = curlop::StealPolicy::LastStolen;   // T-385 ADR-008 explicit voice-policy declaration (every factory module is mono today; opt-out when SF-052 polyphony lands)
        s.inputs.emplace_back("IN");
        { curlop::ParamSchemaEntry p; p.name="LEVEL"; p.type=curlop::ParamType::Continuous; p.min=0.0f; p.max=1.5f; p.defaultValue=1.0f; p.unit=""; p.scale=curlop::Scale::Linear; p.widget="knob"; p.column=0; p.row=0; p.width=2; p.height=1; p.orientation=""; p.hidden=false; p.mode=""; p.interactive=true; p.accent=""; p.size=1.5f; p.skin="primary"; p.xParam=""; p.yParam=""; p.acceptance=curlop::ParamAcceptance::ControlValue; s.params.push_back(std::move(p)); }
        { curlop::ParamSchemaEntry p; p.name="CLIP"; p.type=curlop::ParamType::Boolean; p.min=0.0f; p.max=1.0f; p.defaultValue=0.0f; p.unit=""; p.scale=curlop::Scale::Linear; p.widget="led"; p.column=2; p.row=0; p.width=1; p.height=1; p.orientation=""; p.hidden=false; p.mode=""; p.interactive=false; p.accent="#FF3333"; p.size=0.75f; p.skin="lamp"; p.xParam=""; p.yParam=""; p.acceptance=curlop::ParamAcceptance::ControlValue; s.params.push_back(std::move(p)); }
        reg["output"] = std::move(s);
    }
}

// T-351 / D-MOD-1: the factory schema registry — built once, lazily, from
// populateFactoryModuleRegistry. The factory schema is the single source of
// truth for a module's param set — the APG sub-graph builders derive their
// ParamDef[] from it (see module_builders::schemaToParamDefs in
// graph/ApgClipBuilder_modules.h) instead of hand-duplicating name/min/max.
inline const std::unordered_map<std::string, ModuleSchema>& factoryRegistry()
{
    static const std::unordered_map<std::string, ModuleSchema> reg = []{
        std::unordered_map<std::string, ModuleSchema> r;
        populateFactoryModuleRegistry(r);
        return r;
    }();
    return reg;
}

// Single-module schema lookup by dotted registry key (the identity model's
// `id`). Returns nullptr if `lineageId` is not a factory module.
inline const ModuleSchema* factoryModuleSchema(const std::string& lineageId)
{
    const auto& reg = factoryRegistry();
    auto it = reg.find(lineageId);
    return it == reg.end() ? nullptr : &it->second;
}

// T-100: reverse identity lookup — factory module schema by stable
// lineageUuid (the model's `lineage_id`). Returns nullptr for an empty uuid,
// an unknown uuid, or a user-registered module (no factory UUID). .curlop
// load uses this to resolve a persisted module identity-first, so a project
// survives a registry dotted-id recategorize. See MODULE-IDENTITY-SPEC.md.
inline const ModuleSchema* factoryModuleByUuid(const std::string& uuid)
{
    if (uuid.empty()) return nullptr;
    for (const auto& kv : factoryRegistry())
        if (kv.second.lineageUuid == uuid) return &kv.second;
    return nullptr;
}

// T-382: DSL-name lookup — find the factory schema whose dotted lineageId's
// last segment equals `dsl`. The DSL name ("sine", "delay", "djembe") is the
// last segment of the model id ("core.synth.sine", "core.fx.delay",
// "core.synth.djembe") per CLAUDE.md / MODULE-TECHNICAL-SPEC §1.2.
//
// Used by the install builder (BridgeMessageHandler) to wire per-port
// ParamAcceptance into the new ModulationTree from each module's schema.
// Linear scan over 25 factory entries — message-thread only, O(N_modules)
// at install time, no audio-thread reads.
//
// Returns nullptr if no factory module's last segment matches (user-
// registered modules or unknown DSL names).
inline const ModuleSchema* factoryModuleByDslName(const std::string& dsl)
{
    if (dsl.empty()) return nullptr;
    for (const auto& kv : factoryRegistry()) {
        const auto& lid = kv.second.lineageId;
        const auto pos = lid.rfind('.');
        const std::string last = (pos == std::string::npos)
            ? lid : lid.substr(pos + 1);
        if (last == dsl) return &kv.second;
    }
    return nullptr;
}

// The precompiled factory Faust instruments were retired for TC1. All
// remaining modules either provide their own source declaration at runtime or
// use the synthesized declaration path.
inline const vm::ModuleDeclaration* factoryModuleDeclarationByDslName(
    const std::string&)
{
    return nullptr;
}

} // namespace curlop
