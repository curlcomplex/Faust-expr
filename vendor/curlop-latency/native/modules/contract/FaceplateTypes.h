#pragma once

// ═══════════════════════════════════════════════════════════════════════════
// FaceplateTypes.h — the normalised faceplate model (ADR-009, T-357).
//
// ADR-009 "Faceplate Standard": a module's interface is described by ONE
// normalised schema, populated by two extractors (Faust UI tree / native
// hand-declaration). The DSP source stays
// pristine; CURLOP owns the editable layout separately.
//
//   FaceplateSchema  — extracted, canonical: params, meters, group tree, and
//                      source-authored faceplate-only elements.
//                      The *what* — generalises the schema-half of
//                      ParamSchemaEntry (ModuleTypes.h).
//   FaceplateLayout  — sparse per-instance overrides where they exist. Faust
//                      modules author their default faceplate in the source.
//
//   runtime faceplate  =  FaceplateSchema  ⊕  FaceplateLayout
//
// T357-1 defines the structs. Extractors are T357-2 (Faust) / T357-4 (native);
// persistence is T357-5; the schema⊕layout merge +
// source/layout sync rules are T357-6. Designs: .planning/adrs/ADR-009 +
// OP ADR-008 (op-faust) + .planning/research/
// F-055-PLAN-module-identity-faceplate-s456.md.
// ═══════════════════════════════════════════════════════════════════════════

#include <cstdint>
#include <cctype>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include "modules/contract/ModuleTypes.h"   // ParamType, Scale — reused here, not redefined

namespace curlop {

inline std::string trimFaceplateMetadataToken (std::string value)
{
    while (! value.empty() && std::isspace ((unsigned char) value.front()))
        value.erase (value.begin());
    while (! value.empty() && std::isspace ((unsigned char) value.back()))
        value.pop_back();
    return value;
}

inline std::string lowerFaceplateMetadataToken (std::string value)
{
    value = trimFaceplateMetadataToken (std::move (value));
    for (auto& ch : value)
        ch = (char) std::tolower ((unsigned char) ch);
    return value;
}

inline std::string faceplateMetadataStyleToken (std::string value)
{
    value = trimFaceplateMetadataToken (std::move (value));
    const auto brace = value.find ('{');
    if (brace != std::string::npos)
        value = value.substr (0, brace);
    return lowerFaceplateMetadataToken (std::move (value));
}

inline std::string normaliseFaceplateOrientationMetadata (std::string value)
{
    value = faceplateMetadataStyleToken (std::move (value));
    return value == "horizontal" || value == "vertical" ? value : std::string();
}

inline std::string normaliseFaceplateSkinMetadata (std::string value)
{
    value = faceplateMetadataStyleToken (std::move (value));
    return value == "outline" ? value : std::string();
}

inline std::string normaliseFaceplateLabelPositionMetadata (std::string value)
{
    value = faceplateMetadataStyleToken (std::move (value));
    return value == "top" || value == "bottom" || value == "left"
        || value == "right" || value == "hidden" || value == "compact" ? value : std::string();
}

inline std::string normaliseFaceplateAccentMetadata (std::string value)
{
    value = trimFaceplateMetadataToken (std::move (value));
    if (value.size() != 7 || value.front() != '#')
        return {};

    for (size_t i = 1; i < value.size(); ++i)
    {
        const auto ch = (unsigned char) value[i];
        if (! std::isxdigit (ch))
            return {};
        value[i] = (char) std::toupper (ch);
    }
    return value;
}

// ── FaceplateSchema — the extracted, canonical interface description ─────────

// One control's value domain + identity — the *schema* half: what the control
// is, not where it sits. Layout (position / size / widget pick) is a separate
// FaceplateLayoutEntry, joined back by `id`.
struct FaceplateParam {
    std::string id;                              // stable identity — backend path
                                                 // (Faust "/Group/Cutoff").
                                                 // endpoint name, or the native
                                                 // author's chosen key. Survives a
                                                 // display-name change; the layout
                                                 // joins on it.
    std::string name;                            // display label
    ParamType   type         = ParamType::Continuous;  // value domain
    float       minValue     = 0.0f;
    float       maxValue     = 1.0f;
    float       defaultValue = 0.0f;
    float       step         = 0.0f;             // 0 = continuous; >0 = quantised
    std::string unit;                            // "Hz" | "ms" | "dB" | "st" | ...
    Scale       scale        = Scale::Linear;
    std::string widgetHint;                      // default widget from the source
                                                 // ([style:knob] etc.) — "knob" |
                                                 // "slider" | "menu" | … A layout
                                                 // entry may override it.
    bool        hidden       = false;            // declared + addressable, but
                                                 // excluded from the default
                                                 // faceplate ([hidden:1]).
    std::string tooltip;
    float       order        = -1.0f;
    std::vector<ParamOption> options;
};

// One level readout — an output display (Faust hbargraph / vbargraph, or a
// native output endpoint). Not user-editable; sampled audio-thread → GUI.
struct FaceplateMeter {
    std::string id;
    std::string name;
    float       minValue = 0.0f;
    float       maxValue = 1.0f;
    std::string unit;
    Scale       scale    = Scale::Linear;
    std::string widgetHint;
    std::string orientation;   // "horizontal" | "vertical"
    std::string tooltip;
    float       order    = -1.0f;
    float       column   = -1.0f;
    float       row      = -1.0f;
    float       width    = 1.0f;
    float       height   = 1.0f;
};

// A faceplate-only element. These are not DSP params and must not create
// ControlCores or graph sockets. Faust authors them with `declare curlop_gui`.
struct FaceplateElement {
    std::string id;
    std::string kind;        // label | divider | spacer | group | level | led | status | ...
    std::string label;       // display text/title
    std::string variant;     // section | caption | accent | ...
    std::string group;       // optional Faust/native group owner for closure
    std::string source;      // future displays: audio:meter, audio:fft, ...
    float       column = -1.0f;
    float       row    = -1.0f;
    float       width  = 1.0f;
    float       height = 1.0f;
    std::string orientation;
    std::string accent;      // "#RRGGBB" | ""
    float       fontSize = 0.0f; // label font size; 0 = renderer default
    std::string labelPosition;
    bool        hidden = false;
    float       minValue = 0.0f;  // levels/readouts
    float       maxValue = 1.0f;
    float       threshold = 0.5f;  // led/status threshold
};

// Source/project-authored socket presentation. This is GUI metadata only: it
// must not decide whether a socket exists or how audio/control is routed.
struct SocketPresentation {
    std::string name;        // socket/param/control-output name
    std::string direction;   // input | output
    std::string role;        // audio | gate | trigger | pitch | velocity | cv | modulation | clock | data | debug
    std::string label;
    std::string shape;       // circle | square | diamond | ...
    std::string color;       // theme token or explicit authored color
    std::string wireColor;   // theme token or explicit authored wire color
    std::string wireStyle;   // solid | dashed | pulse | ...
    std::string polarity;    // unipolar | bipolar
    float       minValue = 0.0f;
    float       maxValue = 1.0f;
    // Optional exact authored faceplate control/meter/element id. Presentation
    // only: it places the existing socket; it never creates or routes a port.
    std::string anchor;
};

// Edge-level wire presentation override. Empty fields mean "inherit from the
// source socket role/presentation".
struct WirePresentation {
    std::string role;
    std::string color;
    std::string style;
    std::string polarity;
    float       minValue = 0.0f;
    float       maxValue = 1.0f;
};

// A control group — Faust vgroup / hgroup / tgroup, or a native group.
enum class GroupKind : uint8_t { Vertical, Horizontal, Tab };

// Recursive: groups nest. Carried separately from params so the default
// faceplate layout can honour the source's grouping. (std::vector supports an
// incomplete value type, so the recursive `children` member is well-formed.)
struct FaceplateGroup {
    std::string                 name;            // "" for the implicit root
    GroupKind                   kind = GroupKind::Vertical;
    float                       order = -1.0f;
    float                       column = -1.0f;
    float                       row    = -1.0f;
    float                       width  = -1.0f;
    float                       height = -1.0f;
    std::string                 accent;          // "#RRGGBB" | ""
    float                       fontSize = 0.0f; // label font size; 0 = renderer default
    std::string                 labelPosition;
    std::vector<std::string>    paramIds;         // FaceplateParam.id members
    std::vector<std::string>    meterIds;         // FaceplateMeter.id members
    std::vector<FaceplateGroup>  children;        // nested sub-groups
};

// The whole extracted schema — backend-agnostic. The graph, the GUI, and
// persistence see only this; never a backend-specific form.
struct FaceplateSchema {
    std::vector<FaceplateParam> params;
    std::vector<FaceplateMeter> meters;
    std::vector<FaceplateElement> elements;
    FaceplateGroup              rootGroup;        // the group tree's root
};

// ── FaceplateLayout — the user-edited override layer ────────────────────────

// One control's layout override, joined to a FaceplateParam / FaceplateMeter
// by `controlId`. Unset numeric fields (column / row = -1) mean "use the
// default the merge derives from declaration order + group nesting".
struct FaceplateLayoutEntry {
    std::string controlId;                       // FaceplateParam.id or
                                                 // FaceplateMeter.id
    float       column   = -1.0f;                // grid position; -1 = unset
    float       row      = -1.0f;
    float       width    = 1.0f;                 // grid span
    float       height   = 1.0f;
    std::string widgetOverride;                  // user's widget pick;
                                                 // "" = use the schema hint
    std::string orientation;                     // "horizontal" | "vertical" | ""
    std::string mode;                            // "bipolar" | "" — knob centre-detent
    bool        interactive  = true;             // false = display / readout-only
    std::string accent;                          // "#RRGGBB" | "" — per-control
    float       size     = 1.0f;                 // relative scale
    std::string skin;                            // visual variant ("outline" | "")
    float       fontSize = 0.0f;                 // label font size; 0 = renderer default
    std::string labelPosition;                   // top | bottom | left | right | hidden | ""
    std::string xParam;                          // xy_pad: param on the X axis
    std::string yParam;                          // xy_pad: param on the Y axis
};

// Panel-level presentation.
struct FaceplatePanelStyle {
    std::string accent;                          // panel accent; "" = theme default
    int         gridColumns = 0;                 // fixed column count; 0 = auto-fit
};

// The editable layer — stored beside the DSP source in the module registry
// (T357-5). Empty = the merge uses pure schema defaults.
struct FaceplateLayout {
    std::vector<FaceplateLayoutEntry> controls;
    FaceplatePanelStyle               panel;
};

// ── native extraction — ModuleSchema → FaceplateSchema / FaceplateLayout ────
//
// A native C++ module has no DSP source to extract from (ADR-009): its author
// hand-declares the interface. In CURLOP that hand-declaration *is* the
// ModuleSchema (ModuleRegistryGen.h) — a ParamSchemaEntry list that conflates
// the schema half (type / range / unit / scale) with the layout half (grid
// cell / accent / size). These two functions are the native extractor: they
// split that single declaration into the ADR-009 schema/layout pair. No module
// rewrite is needed — the existing ModuleSchema is the author's declaration.
// (The Faust extractor is FaustUiCapture.h.)

inline FaceplateSchema faceplateSchemaFromModuleSchema(const ModuleSchema& m)
{
    FaceplateSchema s;
    s.params.reserve(m.params.size());
    for (const ParamSchemaEntry& p : m.params) {
        FaceplateParam fp;
        fp.id           = p.name;   // native modules key on the param name
        fp.name         = p.name;
        fp.type         = p.type;
        fp.minValue     = p.min;
        fp.maxValue     = p.max;
        fp.defaultValue = p.defaultValue;
        fp.step         = 0.0f;     // ParamSchemaEntry declares no step granularity
        fp.unit         = p.unit;
        fp.scale        = p.scale;
        fp.widgetHint   = p.widget;
        fp.hidden       = p.hidden;
        fp.tooltip      = p.tooltip;
        fp.order        = p.order;
        fp.options      = p.options;
        s.params.push_back(fp);
        s.rootGroup.paramIds.push_back(fp.id);
    }
    // ModuleSchema declares no meters and no group tree — the native faceplate
    // is a flat param list under the implicit root.
    return s;
}

inline FaceplateLayout faceplateLayoutFromModuleSchema(const ModuleSchema& m)
{
    FaceplateLayout layout;
    layout.controls.reserve(m.params.size());
    for (const ParamSchemaEntry& p : m.params) {
        FaceplateLayoutEntry e;
        e.controlId   = p.name;
        e.column      = p.column;
        e.row         = p.row;
        e.width       = p.width;
        e.height      = p.height;
        e.orientation = p.orientation;
        e.mode        = p.mode;
        e.interactive = p.interactive;
        e.accent      = p.accent;
        e.size        = p.size;
        e.skin        = p.skin;
        e.fontSize    = p.fontSize;
        e.labelPosition = p.labelPosition;
        e.xParam      = p.xParam;
        e.yParam      = p.yParam;
        // widgetOverride stays empty: ParamSchemaEntry.widget is the author's
        // *default* hint (it lands on FaceplateParam.widgetHint), not an
        // override of one.
        layout.controls.push_back(e);
    }
    return layout;
}

// ── T357-6 — schema ⊕ layout merge ──────────────────────────────────────────
//
// ADR-009: `runtime faceplate = FaceplateSchema ⊕ FaceplateLayout`. The schema
// (extracted, canonical) is the source of truth for *which* controls exist;
// the FaceplateLayout carries the user's *sparse* overrides. mergeFaceplate
// reconciles the two into the resolved layout the renderer consumes — exactly
// one entry per schema control, in schema declaration + group order.
//
// Source/layout sync rules (ADR-009, normative — the §"Two artefacts" para):
//   • a schema control with no override → a default entry is synthesised
//     (controlId set; position / widget / etc. left at their unset defaults —
//      the entry's index in the resolved list IS its declaration-order default
//      position, which the renderer flows);
//   • an override whose controlId matches no schema control → dropped (stale —
//      the source removed that control);
//   • result order follows the schema's group tree, depth-first.
// The schema is authority: mergeFaceplate never invents a control the schema
// lacks, nor drops one the schema declares. It computes no concrete grid cells
// — a panel-size-dependent render concern; "default position" is carried as
// the resolved list's declaration order, not baked (col,row) pairs.

namespace detail {

// Depth-first walk of the group tree → ordered control ids (each group's
// paramIds then meterIds, then its child groups). `seen` dedupes a control id
// that an extractor placed in two groups — first occurrence wins its order.
inline void collectGroupControlIds(const FaceplateGroup& g,
                                   std::vector<std::string>& out,
                                   std::unordered_set<std::string>& seen)
{
    for (const auto& id : g.paramIds)
        if (seen.insert(id).second) out.push_back(id);
    for (const auto& id : g.meterIds)
        if (seen.insert(id).second) out.push_back(id);
    for (const auto& child : g.children)
        collectGroupControlIds(child, out, seen);
}

} // namespace detail

inline FaceplateLayout mergeFaceplate(const FaceplateSchema& schema,
                                      const FaceplateLayout& overrides)
{
    // 1. Ordered control-id list: the group tree first (declaration + nesting
    //    order), then any schema param / meter the tree did not reach appended
    //    at the end — so a control the schema declares can never be silently
    //    dropped if an extractor leaves it out of the group tree.
    std::vector<std::string> order;
    std::unordered_set<std::string> seen;
    detail::collectGroupControlIds(schema.rootGroup, order, seen);
    for (const auto& p : schema.params)
        if (seen.insert(p.id).second) order.push_back(p.id);
    for (const auto& m : schema.meters)
        if (seen.insert(m.id).second) order.push_back(m.id);

    // 2. Index the sparse overrides by controlId (first entry wins on a dup).
    std::unordered_map<std::string, const FaceplateLayoutEntry*> overrideById;
    for (const auto& e : overrides.controls)
        overrideById.emplace(e.controlId, &e);

    // 3. Resolve — one entry per schema control, in order. A matching override
    //    is kept verbatim; otherwise a default entry is synthesised. An
    //    override with no matching schema control is never visited here, so it
    //    is dropped (the stale-entry rule).
    FaceplateLayout merged;
    merged.panel = overrides.panel;
    merged.controls.reserve(order.size());
    for (const auto& id : order) {
        auto it = overrideById.find(id);
        if (it != overrideById.end()) {
            merged.controls.push_back(*it->second);
        } else {
            FaceplateLayoutEntry def;
            def.controlId = id;          // all other fields at struct defaults
            merged.controls.push_back(def);
        }
    }
    return merged;
}

} // namespace curlop
