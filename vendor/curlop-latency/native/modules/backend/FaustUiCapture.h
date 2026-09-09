#pragma once

// ═══════════════════════════════════════════════════════════════════════════
// FaustUiCapture.h — backend-agnostic capture of a Faust UI tree + schema
// synthesis (ADR-009 faceplate standard, T-357).
//
// FaustNode's `ParamCaptureUI` subclasses libfaust's global-namespace
// `::UI`. That libfaust header MUST stay confined to FaustNode.cpp — it
// collides with the local `UI` stubs in audio/faust/Faust{Djembe,Bell}Adapter.h
// (see FaustNode.cpp:3). So the two concerns are split:
//
//   ParamCaptureUI : ::UI   — the libfaust ABI adapter   (FaustNode.cpp)
//   FaustUiCapture          — the captured tree + synthesis  (here)
//
// ParamCaptureUI forwards each `::UI` callback into a FaustUiCapture member.
// This header is libfaust-free *and* JUCE-free — only ModuleTypes.h /
// FaceplateTypes.h (POD). A test TU can therefore drive FaustUiCapture
// directly, no libfaust link needed; that second consumer is what earns the
// header promotion (SPEC-008 AGENT-EFFECTIVE-CODEBASE).
//
// Faust UI primitive  →  faceplate model:
//   hslider/vslider/nentry      → FaceplateParam (Continuous)
//   button/checkbox             → FaceplateParam (Boolean)
//   hbargraph/vbargraph         → FaceplateMeter
//   vgroup/hgroup/tgroup        → FaceplateGroup tree
//   declare [unit:]/[style:]/   → FaceplateParam.unit / widgetHint / scale /
//     [scale:]/[hidden:]          hidden — Faust fires `declare` immediately
//                                 before the widget it annotates, so pending
//                                 metadata is buffered and attached to the
//                                 next captured widget.
// ═══════════════════════════════════════════════════════════════════════════

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

#include "modules/contract/ModuleTypes.h"     // ParamSchemaEntry, ParamType, Scale
#include "modules/contract/FaceplateTypes.h"  // FaceplateSchema, FaceplateParam, ...
#include "control/vm/machine/Delivery.h"      // vm::ModuleDeclaration (F-066 P5)

namespace curlop {

// Faust slider LABEL → canonical ALL_CAPS param name ("cutoffFreq" → "CUTOFF_FREQ").
// Pure string transform — lives here (next to faustControlInputOf, the other
// Faust-label classifier) so the libfaust-JIT TU (FaustNode.cpp) can reach it
// WITHOUT pulling in ModuleRegistryGen.h, whose transitive Faust-glue stubs
// (Meta/UI/dsp) collide with the real libfaust ::UI/::dsp headers. ModuleRegistryGen.h
// re-exports it for its existing callers.
inline std::string faustLabelToParamName(const std::string& label)
{
    std::string out;
    for (size_t i = 0; i < label.size(); ++i) {
        char ch = label[i];
        if (std::isupper((unsigned char)ch) && i > 0) out.push_back('_');
        out.push_back((char)std::toupper((unsigned char)ch));
    }
    return out;
}

// The production Box renderer must discard meterout channels before it has a
// compiled UI instance to walk. The marker is a Faust bargraph label metadata
// token, so counting the exact token keeps that preflight in lockstep with the
// UI capture without making the renderer duplicate Faust's UI ABI.
inline int faustMeterOutputMarkerCount(const std::string& source)
{
    static constexpr char marker[] = "[curlop:meterout]";
    int count = 0;
    std::size_t offset = 0;
    while ((offset = source.find(marker, offset)) != std::string::npos) {
        ++count;
        offset += sizeof(marker) - 1;
    }
    return count;
}

enum class FaustWidgetKind { HSlider, VSlider, NumEntry, Button, CheckBox };

// F-075: one declared control OUTPUT — a named, sample-rate signal channel a
// module emits (an LFO, an envelope, a derived value). Declared in the `.fdsp`
// by a `[curlop:cvout]`-tagged bargraph riding its output channel via `attach`
// (see the cv() helper). Order is Faust declaration order = the trailing
// output-channel order of `process`. `[curlop:meterout]` uses the same trailing
// convention for faceplate telemetry, after any cvout channels; it is never a
// patch socket or audio channel.
struct ControlOutputDecl {
    std::string name;
    float       min = 0.0f;
    float       max = 1.0f;
    vm::SignalType type = vm::SignalType::Value;
    // Presentation-only source metadata. It must not change this output's
    // identity, type or channel order.
    std::string role;
    std::string anchor;
};

// ── F-066 Phase 5 (T-462): declared-control classification. ────────────────
// One widget → one vm::ControlInput. gate/freq/velocity stop being reserved
// zones (adr-modules §"How modules receive control") — they become ordinary
// declared inputs the delivery layer routes onto BY TYPE, classified by the
// honest name convention:
//   gate → Gate, freq/frequency → Pitch (Hz), velocity/vel → Velocity;
//   buttons/checkboxes
//   convert as gates; [scale:log] maps to the Exp unit; else Linear Value.
// T-573/B-331 (s512): gain is NO LONGER velocity name-magic — it is an ordinary
// Value knob at its declared row, so set_param (keyToParamSlot, declaredStart+c)
// and delivery (rowMap) agree on it and it never lands on the VM-monopolised
// velIdx (the dead-effect-knob root cause). A synth's velocity slider must be
// named velocity (or vel) to receive note velocity.
// Future: explicit Faust metadata ([type:gate]) can override the name
// convention — not built until a module needs it.
inline vm::ControlInput faustControlInputOf(const std::string& label,
                                            bool buttonLike,
                                            float init, float mn, float mx,
                                            const std::string& scaleMeta)
{
    std::string lower;
    lower.reserve(label.size());
    for (char ch : label) lower.push_back((char) std::tolower((unsigned char) ch));

    vm::ControlInput in;
    in.name = label;
    in.min = mn; in.max = mx; in.def = init;
    if (lower == "gate") {
        in.type = vm::SignalType::Gate;
        in.unit = vm::InputUnit::BinaryGate;
        in.min = 0.0f; in.max = 1.0f;
    } else if (lower == "freq" || lower == "frequency") {
        in.type = vm::SignalType::Pitch;
        in.unit = vm::InputUnit::Hz;
    } else if (lower == "velocity" || lower == "vel") {
        in.type = vm::SignalType::Velocity;
        in.unit = vm::InputUnit::Linear;
    } else {
        in.type = vm::SignalType::Value;
        in.unit = buttonLike            ? vm::InputUnit::Gate
                : (scaleMeta == "log")  ? vm::InputUnit::Exp
                                        : vm::InputUnit::Linear;
        if (buttonLike) { in.min = 0.0f; in.max = 1.0f; }
    }
    return in;
}

// Captured Faust UI tree. Methods mirror the `::UI` callbacks one-for-one but
// with libfaust-free signatures. Build order = Faust declaration order, so the
// synthesised schema reads top-to-bottom from the `.dsp` source.
class FaustUiCapture
{
public:
    FaustUiCapture() { clear(); }

    // ── capture API (mirrors the ::UI callbacks) ───────────────────────────

    void openBox(GroupKind kind, const char* label)
    {
        const auto parsed = parseUiLabel(label ? label : "");
        Box b;
        b.label  = parsed.display;
        b.kind   = kind;
        b.parent = boxStack_.back();
        b.order  = parsed.hasOrder ? parsed.order : pending_.order;
        b.meta   = pending_;
        applyInlineLayoutMeta (b.meta, parsed.meta);
        b.seq    = nextSeq_++;
        const int idx = (int) boxes_.size();
        boxes_.push_back(b);
        boxes_[b.parent].childBoxes.push_back(idx);
        boxStack_.push_back(idx);
        pending_ = PendingMeta{};   // metadata never spans a box boundary
    }

    void closeBox()
    {
        if (boxStack_.size() > 1)   // never pop the implicit root
            boxStack_.pop_back();
        pending_ = PendingMeta{};
    }

    void addParam(FaustWidgetKind kind, const char* label,
                  double init, double lo, double hi, double step)
    {
        const auto parsed = parseUiLabel(label ? label : "");
        const int parentBox = ensurePathBoxes(boxStack_.back(), parsed.groups);
        Param p;
        p.label = parsed.display;
        p.path  = pathForBox(parentBox, p.label);
        p.kind  = kind;
        p.init  = init;
        p.min   = lo;
        p.max   = hi;
        p.step  = step;
        p.meta  = pending_;
        applyInlineLayoutMeta (p.meta, parsed.meta);
        p.order = parsed.hasOrder ? parsed.order : p.meta.order;
        p.seq   = nextSeq_++;
        pending_ = PendingMeta{};   // consume the buffered declare metadata
        const int idx = (int) params_.size();
        params_.push_back(p);
        boxes_[parentBox].childParams.push_back(idx);
    }

    void addMeter(bool horizontal, const char* label, double lo, double hi)
    {
        const auto parsed = parseUiLabel(label ? label : "");
        const int parentBox = ensurePathBoxes(boxStack_.back(), parsed.groups);
        Meter m;
        m.label      = parsed.display;
        m.path       = pathForBox(parentBox, m.label);
        m.min        = lo;
        m.max        = hi;
        m.horizontal = horizontal;
        m.meta       = pending_;
        applyInlineLayoutMeta (m.meta, parsed.meta);
        m.order      = parsed.hasOrder ? parsed.order : m.meta.order;
        m.seq        = nextSeq_++;
        // F-075: a `[curlop:cvout]`-tagged bargraph is a control-OUTPUT channel
        // marker, not an on-faceplate display meter. Read the buffered tag
        // BEFORE clearing pending_.
        m.isControlOut = pending_.cvout;
        m.isMeterOut = pending_.meterout;
        pending_ = PendingMeta{};   // consume the buffered declare metadata
        const int idx = (int) meters_.size();
        meters_.push_back(m);
        boxes_[parentBox].childMeters.push_back(idx);
    }

    bool nextWidgetIsControlOutput() const noexcept
    {
        return pending_.cvout;
    }

    bool nextWidgetIsMeterOutput() const noexcept
    {
        return pending_.meterout;
    }

    // Buffered for the next widget — Faust emits `declare` before the add* it
    // annotates. Unrecognised keys are ignored.
    void declareMeta(const char* key, const char* value)
    {
        if (key == nullptr || value == nullptr)
            return;
        const std::string k = key, v = value;
        if (k == "unit")        pending_.unit  = v;
        else if (k == "style") {
            pending_.style = styleToken(v);
            pending_.options = styleOptions(v);
        }
        else if (k == "skin")   pending_.skin  = normaliseFaceplateSkinMetadata(v);
        else if (k == "scale")  pending_.scale = v;
        else if (k == "tooltip") pending_.tooltip = v;
        else if (k == "role") pending_.role = v;
        else if (k == "anchor") pending_.anchor = v;
        else if (k == "curlop_id") pending_.sourceId = v;
        else if (k == "order")  pending_.order = parseFloatMeta(v, -1.0f);
        else if (k == "hidden") pending_.hidden = (v == "1" || v == "true");
        else if (k == "curlop") {
            if (v == "cvout")       pending_.cvout = true;   // F-075 socket namespace
            else if (v == "meterout") pending_.meterout = true;
            else if (v == "input")  pending_.input = true;
            else if (v == "hidden") pending_.hidden = true;
        }
        // SF-093: faceplate geometry declared in the .dsp — the size + cell of a
        // widget are author-controlled, not a registry hardcode. [w:N]/[h:N] =
        // grid-unit span (default 1×1); [col:N]/[row:N] = explicit cell (default
        // -1 = auto-flow). N may be fractional for source-authored polish.
        else if (k == "w")      { pending_.w   = parseFloatMeta(v, 1.0f);  pending_.hasW = true; }
        else if (k == "h")      { pending_.h   = parseFloatMeta(v, 1.0f);  pending_.hasH = true; }
        else if (k == "col")    { pending_.col = parseFloatMeta(v, -1.0f); pending_.hasCol = true; }
        else if (k == "row")    { pending_.row = parseFloatMeta(v, -1.0f); pending_.hasRow = true; }
    }

    void clear()
    {
        params_.clear();
        meters_.clear();
        boxes_.clear();
        boxes_.push_back(Box{});       // boxes_[0] — implicit root, label ""
        boxStack_.assign(1, 0);
        pending_ = PendingMeta{};
        nextSeq_ = 0;
    }

    // ── synthesis ──────────────────────────────────────────────────────────

    // ParamSchemaEntry list (params only) — what FaustNode's hot-swap
    // schema-diff + per-instance ModuleEntry::params consume, and what the
    // snapshot carries to the Web scene (GraphStateSnapshot emits
    // widget/scale/unit; the compositor renders by widget).
    //
    // T-558: enriched from the captured metadata so Faust widget kinds +
    // [style:]/[scale:log]/[unit:] reach the faceplate instead of collapsing to
    // a grid of identical knobs. Widget kinds map faithfully — hslider→slider,
    // vslider→slider, button→momentary, checkbox→toggle — so the .dsp's declared
    // controls are honoured; [style:knob] requests a knob, [style:led] an LED.
    // Only JIT modules use this path (the AOT factory modules are untouched), so
    // there's no restyle of existing instruments. menu/radio/numerical aren't
    // drawable yet → knob fallback. (`hidden` is set here for the unit layer; the
    // snapshot doesn't carry it yet — GUI-honouring of [hidden:1] is a follow-up.)
    std::vector<ParamSchemaEntry> synthesiseSchema() const
    {
        std::vector<ParamSchemaEntry> out;
        out.reserve(params_.size());
        for (const Param& p : params_) {
            ParamSchemaEntry e;
            e.name = p.label;
            e.sourceId = p.meta.sourceId.empty() ? p.path : p.meta.sourceId;
            if (isBoolean(p.kind)) {
                e.type = ParamType::Boolean;
                e.min = 0.0f; e.max = 1.0f; e.defaultValue = 0.0f;
            } else {
                e.type = ParamType::Continuous;
                e.min = (float) p.min;
                e.max = (float) p.max;
                e.defaultValue = (float) p.init;
            }
            e.type   = isMenuLike(p.meta.style) ? ParamType::Select : e.type;
            e.widget = rendererWidgetHint(p.kind, p.meta.style);
            e.scale  = scaleFromMeta(p.meta.scale);
            e.unit   = p.meta.unit;
            e.tooltip = p.meta.tooltip;
            e.order = p.order;
            e.options = p.meta.options;
            // Script-driven inputs (gate/pitch/velocity) are delivered by the VM
            // and surface as input sockets, not faceplate knobs — auto-hide them.
            e.hidden = p.meta.hidden || isScriptDrivenInput(p);
            e.graphInput = p.meta.input;
            e.skin = p.meta.skin;
            e.orientation = p.meta.orientation;
            e.accent = p.meta.accent;
            e.size = p.meta.hasSize ? p.meta.size : 1.0f;
            e.fontSize = p.meta.hasFontSize ? p.meta.fontSize : 0.0f;
            e.labelPosition = p.meta.labelPosition;
            // SF-093: faceplate grid geometry from the .dsp metadata. Sizeless
            // widgets keep ParamSchemaEntry's defaults (1×1, auto-flow -1/-1).
            e.width  = p.meta.w;
            e.height = p.meta.h;
            e.column = p.meta.col;
            e.row    = p.meta.row;
            out.push_back(std::move(e));
        }
        return out;
    }

    // F-066 Phase 5 (T-462): the module's declared control inputs — EVERY
    // widget, reserved-zone skipping gone, declaration order preserved
    // (order is the voice/type routing contract at the delivery seam).
    vm::ModuleDeclaration synthesiseModuleDeclaration() const
    {
        vm::ModuleDeclaration d;
        d.inputs.reserve(params_.size());
        for (const Param& p : params_)
            d.inputs.push_back(faustControlInputOf(
                p.label, isBoolean(p.kind),
                (float) p.init, (float) p.min, (float) p.max, p.meta.scale));
        return d;
    }

    // F-075: the module's declared control OUTPUTS, in Faust declaration order
    // (= the trailing output-channel order of `process`). One entry per
    // `[curlop:cvout]`-tagged bargraph. The engine pairs the i-th entry with the
    // i-th trailing output channel; the count also tells it how many of
    // `getNumOutputs()` are control vs real audio. Meterout channels follow the
    // cvout channels and are excluded from the retained audio/control prefix.
    std::vector<ControlOutputDecl> synthesiseControlOutputs() const
    {
        std::vector<ControlOutputDecl> out;
        for (const Meter& m : meters_) {
            if (! m.isControlOut) continue;
            ControlOutputDecl d;
            d.name = m.label;
            d.min  = (float) m.min;
            d.max  = (float) m.max;
            d.role = m.meta.role;
            d.anchor = m.meta.anchor;
            out.push_back(std::move(d));
        }
        return out;
    }

    int countMeterOutputs() const noexcept
    {
        return static_cast<int>(std::count_if(
            meters_.begin(), meters_.end(),
            [] (const Meter& meter) { return meter.isMeterOut; }));
    }

    // The normalised faceplate schema (ADR-009) — params + meters + group
    // tree, declaration order preserved.
    FaceplateSchema synthesiseFaceplateSchema() const
    {
        FaceplateSchema s;
        s.params.reserve(params_.size());
        for (const Param& p : params_) {
            FaceplateParam fp;
            fp.id   = p.path;
            fp.name = p.label;
            if (isBoolean(p.kind)) {
                fp.type         = ParamType::Boolean;
                fp.minValue     = 0.0f;
                fp.maxValue     = 1.0f;
                fp.defaultValue = 0.0f;
                fp.step         = 1.0f;
            } else {
                fp.type         = ParamType::Continuous;
                fp.minValue     = (float) p.min;
                fp.maxValue     = (float) p.max;
                fp.defaultValue = (float) p.init;
                fp.step         = (float) p.step;
            }
            fp.unit       = p.meta.unit;
            fp.scale      = scaleFromMeta(p.meta.scale);
            fp.widgetHint = p.meta.style.empty() ? defaultWidgetHint(p.kind)
                                                 : p.meta.style;
            fp.hidden     = p.meta.hidden || isScriptDrivenInput(p);
            fp.tooltip    = p.meta.tooltip;
            fp.order      = p.order;
            fp.options    = p.meta.options;
            s.params.push_back(std::move(fp));
        }
        s.meters.reserve(meters_.size());
        for (const Meter& m : meters_) {
            if (m.isControlOut) continue;   // F-075: channel marker, not a display meter
            FaceplateMeter fm;
            fm.id       = m.path;
            fm.name     = m.label;
            fm.minValue = (float) m.min;
            fm.maxValue = (float) m.max;
            fm.unit     = m.meta.unit;
            fm.scale    = scaleFromMeta(m.meta.scale);
            fm.widgetHint = meterWidgetHint(m.horizontal, m.meta.style);
            fm.orientation = m.horizontal ? "horizontal" : "vertical";
            fm.tooltip  = m.meta.tooltip;
            fm.order    = m.order;
            fm.column   = m.meta.col;
            fm.row      = m.meta.row;
            fm.width    = m.meta.w;
            fm.height   = m.meta.h;
            s.meters.push_back(std::move(fm));
        }
        s.rootGroup = buildGroup(0);
        return s;
    }

private:
    struct PendingMeta {
        std::string unit;
        std::string style;
        std::string scale;
        std::string skin;
        std::string orientation;
        std::string accent;
        float       fontSize = 0.0f;
        std::string labelPosition;
        std::string tooltip;
        std::string role;
        std::string anchor;
        float       order  = -1.0f;
        float       size   = 1.0f;
        std::vector<ParamOption> options;
        bool        hidden = false;
        bool        input  = false;  // [curlop:input]
        bool        cvout  = false;  // [curlop:cvout]
        bool        meterout = false; // [curlop:meterout]
        // SF-093: faceplate grid geometry declared in the .dsp. Defaults mirror
        // ParamSchemaEntry — single cell, auto-flow placement — so a sizeless
        // widget carries no hardcoded size.
        float       w   = 1.0f;      // [w:N] grid-unit span (columns)
        float       h   = 1.0f;      // [h:N] grid-unit span (rows)
        float       col = -1.0f;     // [col:N] explicit column (-1 = auto-flow)
        float       row = -1.0f;     // [row:N] explicit row    (-1 = auto-flow)
        bool        hasW = false;
        bool        hasH = false;
        bool        hasCol = false;
        bool        hasRow = false;
        bool        hasSize = false;
        bool        hasFontSize = false;
        // Optional authored public identity; empty preserves the generated path.
        std::string sourceId;
    };

    struct Param {
        std::string     label;
        std::string     path;         // "/Group/Sub/label" — stable id
        FaustWidgetKind kind = FaustWidgetKind::HSlider;
        double          init = 0.0;
        double          min  = 0.0;
        double          max  = 1.0;
        double          step = 0.0;
        PendingMeta     meta;
        float           order = -1.0f;
        int             seq   = 0;
    };

    struct Meter {
        std::string label;
        std::string path;
        double      min          = 0.0;
        double      max          = 1.0;
        bool        horizontal   = true;
        bool        isControlOut = false;   // F-075 [curlop:cvout] channel marker
        bool        isMeterOut   = false;   // [curlop:meterout] discardable telemetry channel
        PendingMeta meta;
        float       order        = -1.0f;
        int         seq          = 0;
    };

    struct Box {
        std::string      label;       // "" for the implicit root
        GroupKind        kind   = GroupKind::Vertical;
        int              parent = -1;
        float            order  = -1.0f;
        PendingMeta      meta;
        int              seq    = 0;
        std::vector<int> childParams;  // indices into params_
        std::vector<int> childMeters;  // indices into meters_
        std::vector<int> childBoxes;   // indices into boxes_
    };

    static bool isBoolean(FaustWidgetKind k)
    {
        return k == FaustWidgetKind::Button || k == FaustWidgetKind::CheckBox;
    }

    struct PathGroup {
        GroupKind kind = GroupKind::Vertical;
        std::string label;
        float order = -1.0f;
        bool hasOrder = false;
        PendingMeta meta;
    };

    struct ParsedLabel {
        std::string display;
        float order = -1.0f;
        bool hasOrder = false;
        PendingMeta meta;
        std::vector<PathGroup> groups;
    };

    static void applyInlineLayoutMeta(PendingMeta& dst, const PendingMeta& src)
    {
        if (src.hasCol) { dst.col = src.col; dst.hasCol = true; }
        if (src.hasRow) { dst.row = src.row; dst.hasRow = true; }
        if (src.hasW)   { dst.w = src.w;     dst.hasW = true; }
        if (src.hasH)   { dst.h = src.h;     dst.hasH = true; }
        if (src.order >= 0.0f) dst.order = src.order;
        if (! src.style.empty()) { dst.style = src.style; dst.options = src.options; }
        if (! src.scale.empty()) dst.scale = src.scale;
        if (! src.skin.empty()) dst.skin = src.skin;
        if (! src.orientation.empty()) dst.orientation = src.orientation;
        if (! src.accent.empty()) dst.accent = src.accent;
        if (src.hasFontSize) { dst.fontSize = src.fontSize; dst.hasFontSize = true; }
        if (! src.labelPosition.empty()) dst.labelPosition = src.labelPosition;
        if (! src.role.empty()) dst.role = src.role;
        if (! src.anchor.empty()) dst.anchor = src.anchor;
        if (! src.sourceId.empty()) dst.sourceId = src.sourceId;
        if (src.hasSize) { dst.size = src.size; dst.hasSize = true; }
        if (src.hidden) dst.hidden = true;
    }

    // A param the VM injects (gate/pitch/velocity) — classified by name via the
    // canonical faustControlInputOf convention. These are control INPUTS driven
    // by scripting (surfaced as input sockets), never user-facing faceplate knobs.
    static bool isScriptDrivenInput(const Param& p)
    {
        const auto in = faustControlInputOf(p.label, isBoolean(p.kind),
                                            (float) p.init, (float) p.min,
                                            (float) p.max, p.meta.scale);
        return in.type == vm::SignalType::Gate
            || in.type == vm::SignalType::Pitch
            || in.type == vm::SignalType::Velocity;
    }

    static const char* defaultWidgetHint(FaustWidgetKind k)
    {
        return isBoolean(k) ? "button" : "knob";
    }

    static bool isMenuLike(const std::string& style)
    {
        return style == "menu" || style == "radio";
    }

    // T-558: captured widget kind + [style:] token → a Web renderer hint
    // (the strings the faceplate compositor understands:
    // knob/hslider/vslider/led/button/toggle). Continuous controls default to
    // "knob"; an explicit style overrides where the renderer can draw it.
    // menu/radio/numerical aren't drawable yet, so they fall back to the kind
    // default rather than vanish.
    static std::string rendererWidgetHint(FaustWidgetKind kind,
                                          const std::string& style)
    {
        // Explicit [style:] override where the renderer can draw it. Faust's own
        // style vocabulary is knob/menu/radio/led/numerical; we also accept
        // hslider/vslider/slider. menu/radio/numerical aren't drawable yet, so
        // they fall through to the faithful kind default.
        if (isMenuLike(style))
            return "select";
        if (style == "knob" || style == "led" || style == "button" || style == "toggle"
         || style == "hslider" || style == "vslider")
            return style;
        if (style == "slider")
            return "hslider";
        switch (kind) {
            case FaustWidgetKind::HSlider:  return "hslider";
            case FaustWidgetKind::VSlider:  return "vslider";
            case FaustWidgetKind::NumEntry: return "knob";   // numeric-entry widget TODO
            case FaustWidgetKind::CheckBox: return "toggle";
            case FaustWidgetKind::Button:   return "button";
        }
        return "knob";
    }

    static std::string meterWidgetHint(bool horizontal, const std::string& style)
    {
        if (style == "led") return "status";
        if (style == "numerical") return "readout";
        if (style == "vbargraph") return "level";
        if (style == "hbargraph") return "level";
        return horizontal ? "level" : "level";
    }

    static Scale scaleFromMeta(const std::string& scale)
    {
        if (scale == "log" || scale == "logarithmic") return Scale::Logarithmic;
        if (scale == "exp" || scale == "exponential") return Scale::Exponential;
        return Scale::Linear;
    }

    // SF-093: parse a [w:N]/[h:N]/[col:N]/[row:N] metadata value to a float.
    // Tolerant of leading sign + surrounding whitespace; a non-numeric value
    // falls back to `dflt` (a malformed tag never crashes the JIT pipeline).
    static float parseFloatMeta(const std::string& v, float dflt)
    {
        const char* start = v.c_str();
        char* end = nullptr;
        const float parsed = std::strtof(start, &end);
        while (end != nullptr && *end != '\0' && std::isspace((unsigned char) *end))
            ++end;
        return end == start || end == nullptr || *end != '\0' || ! std::isfinite(parsed)
            ? dflt
            : parsed;
    }

    // "[style:menu{'a':0}]" → "menu"; "[style:knob]" → "knob".
    static std::string styleToken(const std::string& v)
    {
        const std::string::size_type brace = v.find('{');
        return brace == std::string::npos ? v : v.substr(0, brace);
    }

    static std::string trim(std::string s)
    {
        while (!s.empty() && std::isspace((unsigned char) s.front())) s.erase(s.begin());
        while (!s.empty() && std::isspace((unsigned char) s.back())) s.pop_back();
        return s;
    }

    static std::string stripQuotes(std::string s)
    {
        s = trim(std::move(s));
        if (s.size() >= 2 && ((s.front() == '\'' && s.back() == '\'')
                           || (s.front() == '"'  && s.back() == '"')))
            return s.substr(1, s.size() - 2);
        return s;
    }

    static std::vector<ParamOption> styleOptions(const std::string& v)
    {
        std::vector<ParamOption> out;
        const auto l = v.find('{');
        const auto r = v.rfind('}');
        if (l == std::string::npos || r == std::string::npos || r <= l)
            return out;

        std::string body = v.substr(l + 1, r - l - 1);
        size_t pos = 0;
        while (pos < body.size()) {
            const auto colon = body.find(':', pos);
            if (colon == std::string::npos) break;
            std::string key = stripQuotes(body.substr(pos, colon - pos));
            const auto comma = body.find(',', colon + 1);
            std::string val = body.substr(colon + 1,
                                          comma == std::string::npos
                                            ? std::string::npos
                                            : comma - colon - 1);
            ParamOption opt;
            opt.label = key;
            opt.value = parseFloatMeta(val, (float) out.size());
            if (! opt.label.empty()) out.push_back(std::move(opt));
            if (comma == std::string::npos) break;
            pos = comma + 1;
        }
        return out;
    }

    static std::vector<std::string> splitPath(const std::string& raw)
    {
        std::vector<std::string> out;
        size_t pos = 0;
        while (pos <= raw.size()) {
            const auto slash = raw.find('/', pos);
            std::string part = raw.substr(pos, slash == std::string::npos
                                               ? std::string::npos
                                               : slash - pos);
            part = trim(std::move(part));
            if (! part.empty()) out.push_back(std::move(part));
            if (slash == std::string::npos) break;
            pos = slash + 1;
        }
        return out;
    }

    static void parseOrderPrefix(std::string& s, float& order, bool& hasOrder)
    {
        if (s.size() < 3 || s.front() != '[') return;
        const auto close = s.find(']');
        if (close == std::string::npos) return;
        order = parseFloatMeta(s.substr(1, close - 1), -1.0f);
        hasOrder = true;
        s = trim(s.substr(close + 1));
    }

    static void parseInlineLayoutMeta(std::string& s, PendingMeta& meta)
    {
        size_t pos = 0;
        while ((pos = s.find('[', pos)) != std::string::npos) {
            const auto close = s.find(']', pos + 1);
            if (close == std::string::npos) break;
            const auto body = s.substr(pos + 1, close - pos - 1);
            const auto colon = body.find(':');
            if (colon == std::string::npos) {
                pos = close + 1;
                continue;
            }

            const auto key = trim(body.substr(0, colon));
            const auto val = trim(body.substr(colon + 1));
            if (key == "col")      { meta.col = parseFloatMeta(val, -1.0f); meta.hasCol = true; }
            else if (key == "row") { meta.row = parseFloatMeta(val, -1.0f); meta.hasRow = true; }
            else if (key == "w")   { meta.w   = parseFloatMeta(val, 1.0f);  meta.hasW = true; }
            else if (key == "h")   { meta.h   = parseFloatMeta(val, 1.0f);  meta.hasH = true; }
            else if (key == "style") { meta.style = styleToken(val); meta.options = styleOptions(val); }
            else if (key == "skin") meta.skin = normaliseFaceplateSkinMetadata(val);
            else if (key == "scale") meta.scale = val;
            else if (key == "orientation") meta.orientation = normaliseFaceplateOrientationMetadata(val);
            else if (key == "accent") meta.accent = normaliseFaceplateAccentMetadata(val);
            else if (key == "labelsize" || key == "fontSize")
            {
                meta.fontSize = parseFloatMeta(val, 0.0f);
                meta.hasFontSize = meta.fontSize > 0.0f;
            }
            else if (key == "labelpos" || key == "labelPosition") meta.labelPosition = normaliseFaceplateLabelPositionMetadata(val);
            else if (key == "size") { meta.size = parseFloatMeta(val, 1.0f); meta.hasSize = true; }
            else if (key == "hidden") meta.hidden = (val == "1" || val == "true");
            else if (key == "curlop_id") meta.sourceId = val;
            else {
                pos = close + 1;
                continue;
            }

            s.erase(pos, close - pos + 1);
        }
        s = trim(std::move(s));
    }

    static GroupKind parseGroupPrefix(std::string& s)
    {
        if (s.size() > 2 && s[1] == ':') {
            const char prefix = (char) std::tolower((unsigned char) s[0]);
            s = s.substr(2);
            if (prefix == 'h') return GroupKind::Horizontal;
            if (prefix == 't') return GroupKind::Tab;
        }
        return GroupKind::Vertical;
    }

    static ParsedLabel parseUiLabel(const std::string& raw)
    {
        ParsedLabel parsed;
        auto parts = splitPath(raw);
        if (parts.empty()) {
            parsed.display = raw;
            return parsed;
        }

        for (size_t i = 0; i + 1 < parts.size(); ++i) {
            std::string label = parts[i];
            PathGroup g;
            parseOrderPrefix(label, g.order, g.hasOrder);
            g.kind = parseGroupPrefix(label);
            parseOrderPrefix(label, g.order, g.hasOrder);
            parseInlineLayoutMeta(label, g.meta);
            g.label = trim(std::move(label));
            if (! g.label.empty()) parsed.groups.push_back(std::move(g));
        }

        std::string display = parts.back();
        parseOrderPrefix(display, parsed.order, parsed.hasOrder);
        parseGroupPrefix(display);
        parseOrderPrefix(display, parsed.order, parsed.hasOrder);
        parseInlineLayoutMeta(display, parsed.meta);
        parsed.display = trim(std::move(display));
        return parsed;
    }

    int ensurePathBoxes(int parent, const std::vector<PathGroup>& groups)
    {
        int cur = parent;
        for (const auto& g : groups) {
            int found = -1;
            for (int ci : boxes_[cur].childBoxes) {
                if (boxes_[ci].label == g.label && boxes_[ci].kind == g.kind) {
                    found = ci;
                    break;
                }
            }
            if (found >= 0) {
                cur = found;
                continue;
            }
            Box b;
            b.label = g.label;
            b.kind = g.kind;
            b.parent = cur;
            b.order = g.hasOrder ? g.order : -1.0f;
            b.meta = g.meta;
            b.seq = nextSeq_++;
            const int idx = (int) boxes_.size();
            boxes_.push_back(std::move(b));
            boxes_[cur].childBoxes.push_back(idx);
            cur = idx;
        }
        return cur;
    }

    // The Faust path for a widget under the current box stack — joins each
    // open box's (non-empty) label, then the widget label. Matches Faust's
    // own setParamValue path convention ("/Group/Cutoff").
    std::string pathForBox(int boxIdx, const std::string& label) const
    {
        std::vector<int> path;
        for (int idx = boxIdx; idx > 0; idx = boxes_[idx].parent)
            path.push_back(idx);
        std::reverse(path.begin(), path.end());

        std::string p;
        for (int idx : path) {
            if (! boxes_[idx].label.empty()) {
                p += '/';
                p += boxes_[idx].label;
            }
        }
        p += '/';
        p += label;
        return p;
    }

    FaceplateGroup buildGroup(int boxIdx) const
    {
        const Box& b = boxes_[boxIdx];
        FaceplateGroup g;
        g.name = b.label;
        g.kind = b.kind;
        g.order = b.order;
        g.column = b.meta.hasCol ? b.meta.col : -1.0f;
        g.row = b.meta.hasRow ? b.meta.row : -1.0f;
        g.width = b.meta.hasW ? b.meta.w : -1.0f;
        g.height = b.meta.hasH ? b.meta.h : -1.0f;
        g.accent = b.meta.accent;
        g.fontSize = b.meta.hasFontSize ? b.meta.fontSize : 0.0f;
        g.labelPosition = b.meta.labelPosition;

        auto sortedParams = b.childParams;
        std::sort(sortedParams.begin(), sortedParams.end(), [&] (int a, int c) {
            const auto& pa = params_[a];
            const auto& pc = params_[c];
            if (pa.order >= 0.0f || pc.order >= 0.0f)
                return (pa.order >= 0.0f ? pa.order : 1.0e9f)
                     < (pc.order >= 0.0f ? pc.order : 1.0e9f);
            return pa.seq < pc.seq;
        });
        auto sortedMeters = b.childMeters;
        std::sort(sortedMeters.begin(), sortedMeters.end(), [&] (int a, int c) {
            const auto& ma = meters_[a];
            const auto& mc = meters_[c];
            if (ma.order >= 0.0f || mc.order >= 0.0f)
                return (ma.order >= 0.0f ? ma.order : 1.0e9f)
                     < (mc.order >= 0.0f ? mc.order : 1.0e9f);
            return ma.seq < mc.seq;
        });
        auto sortedBoxes = b.childBoxes;
        std::sort(sortedBoxes.begin(), sortedBoxes.end(), [&] (int a, int c) {
            const auto& ba = boxes_[a];
            const auto& bc = boxes_[c];
            if (ba.order >= 0.0f || bc.order >= 0.0f)
                return (ba.order >= 0.0f ? ba.order : 1.0e9f)
                     < (bc.order >= 0.0f ? bc.order : 1.0e9f);
            return ba.seq < bc.seq;
        });

        for (int pi : sortedParams) g.paramIds.push_back(params_[pi].path);
        for (int mi : sortedMeters)
            if (! meters_[mi].isControlOut)   // F-075: cvout markers aren't faceplate controls
                g.meterIds.push_back(meters_[mi].path);
        for (int ci : sortedBoxes)  g.children.push_back(buildGroup(ci));
        return g;
    }

    std::vector<Param> params_;
    std::vector<Meter> meters_;
    std::vector<Box>   boxes_;       // boxes_[0] = implicit root
    std::vector<int>   boxStack_;    // index stack; back() = currently-open box
    PendingMeta        pending_;     // declare metadata awaiting the next widget
    int                nextSeq_ = 0;
};

} // namespace curlop
