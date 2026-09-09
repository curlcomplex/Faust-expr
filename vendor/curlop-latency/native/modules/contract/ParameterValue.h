#pragma once

#include <algorithm>
#include <cmath>
#include <cctype>
#include <string>

namespace curlop::param {

enum class Scale { Linear, Logarithmic, Exponential };

// The authored meaning is retained independently of the target's storage unit.
enum class AuthoredBasis {
    TargetUnit,
    WallClockSeconds,
    TempoBeats,
    ScriptSteps,
    FrequencyHz,
};

inline const char* authoredBasisToString(AuthoredBasis basis)
{
    switch (basis) {
        case AuthoredBasis::TargetUnit:       return "target-unit";
        case AuthoredBasis::WallClockSeconds: return "wall-clock-seconds";
        case AuthoredBasis::TempoBeats:       return "tempo-beats";
        case AuthoredBasis::ScriptSteps:      return "script-steps";
        case AuthoredBasis::FrequencyHz:      return "frequency-hz";
    }
    return "target-unit";
}

inline bool authoredBasisFromString(const std::string& text, AuthoredBasis& basis)
{
    if (text == "target-unit")        basis = AuthoredBasis::TargetUnit;
    else if (text == "wall-clock-seconds") basis = AuthoredBasis::WallClockSeconds;
    else if (text == "tempo-beats")  basis = AuthoredBasis::TempoBeats;
    else if (text == "script-steps") basis = AuthoredBasis::ScriptSteps;
    else if (text == "frequency-hz") basis = AuthoredBasis::FrequencyHz;
    else return false;
    return true;
}

struct AuthoredValue {
    double value = 0.0;
    AuthoredBasis basis = AuthoredBasis::TargetUnit;
};

struct TimingContext {
    double bpm = 120.0;
    double stepDurationBeats = 1.0;
};

struct ParameterDeclaration {
    double min = 0.0;
    double max = 1.0;
    std::string unit;
    Scale scale = Scale::Linear;
};

struct ConvertedSet {
    bool ok = false;
    double implementationValue = 0.0;
    double normalized = 0.0;
    const char* error = "";
};

// A modulation depth of 1 traverses the full declared range. The VM's
// bipolar wire encoding is derived from this position as 2 * position - 1.
inline double encodePosition(const ParameterDeclaration& target, double value)
{
    if (target.max <= target.min)
        return 0.5;
    if (target.scale == Scale::Logarithmic && target.min > 0.0)
        return std::log(std::max(value, target.min) / target.min)
            / std::log(target.max / target.min);
    return (value - target.min) / (target.max - target.min);
}

inline double decodePosition(const ParameterDeclaration& target, double position)
{
    const double bounded = std::clamp(position, 0.0, 1.0);
    if (target.scale == Scale::Logarithmic && target.min > 0.0
        && target.max > target.min)
        return target.min * std::pow(target.max / target.min, bounded);
    return target.min + bounded * (target.max - target.min);
}

inline double composeMappedValue(const ParameterDeclaration& target,
                                 double base, double offset)
{
    return decodePosition(target, encodePosition(target, base) + offset);
}

namespace detail {

inline std::string canonicalUnit(std::string unit)
{
    std::transform(unit.begin(), unit.end(), unit.begin(),
                   [] (unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return unit;
}

inline bool authoredSeconds(AuthoredValue value, TimingContext timing,
                            double& seconds, const char*& error)
{
    switch (value.basis) {
        case AuthoredBasis::WallClockSeconds:
            seconds = value.value;
            break;
        case AuthoredBasis::TempoBeats:
            if (! std::isfinite(timing.bpm) || timing.bpm <= 0.0) {
                error = "tempo-relative value requires positive finite BPM";
                return false;
            }
            seconds = value.value * 60.0 / timing.bpm;
            break;
        case AuthoredBasis::ScriptSteps:
            if (! std::isfinite(timing.bpm) || timing.bpm <= 0.0
                || ! std::isfinite(timing.stepDurationBeats)
                || timing.stepDurationBeats <= 0.0) {
                error = "Script-step value requires positive finite BPM and step duration";
                return false;
            }
            seconds = value.value * timing.stepDurationBeats * 60.0 / timing.bpm;
            break;
        case AuthoredBasis::FrequencyHz:
            if (value.value <= 0.0) {
                error = "frequency value must be positive";
                return false;
            }
            seconds = 1.0 / value.value;
            break;
        case AuthoredBasis::TargetUnit:
            return false;
    }
    if (! std::isfinite(seconds) || seconds < 0.0) {
        error = "time value must be finite and non-negative";
        return false;
    }
    return true;
}

inline double normalize(const ParameterDeclaration& target, double value)
{
    return encodePosition(target, value) * 2.0 - 1.0;
}

} // namespace detail

inline ConvertedSet convertSet(const ParameterDeclaration& target,
                               AuthoredValue authored,
                               TimingContext timing)
{
    if (! std::isfinite(target.min) || ! std::isfinite(target.max)
        || target.min > target.max)
        return { false, 0.0, 0.0,
                 "target range must be finite with min not exceeding max" };

    const auto unit = detail::canonicalUnit(target.unit);
    const bool targetIsTime = unit == "ms" || unit == "s";
    const bool targetIsRate = unit == "hz";
    double implementationValue = authored.value;
    const char* error = "";

    if (authored.basis != AuthoredBasis::TargetUnit) {
        if (! targetIsTime && ! targetIsRate)
            return { false, 0.0, 0.0,
                     "time or frequency value is incompatible with target quantity" };

        double seconds = 0.0;
        if (! detail::authoredSeconds(authored, timing, seconds, error))
            return { false, 0.0, 0.0, error };
        if (targetIsTime)
            implementationValue = unit == "ms" ? seconds * 1000.0 : seconds;
        else {
            if (seconds <= 0.0)
                return { false, 0.0, 0.0, "frequency period must be positive" };
            implementationValue = 1.0 / seconds;
        }
    }

    if (! std::isfinite(implementationValue))
        return { false, 0.0, 0.0, "parameter value must be finite" };

    return { true, implementationValue,
             detail::normalize(target, implementationValue), "" };
}

} // namespace curlop::param
