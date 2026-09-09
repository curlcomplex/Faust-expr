#pragma once

#include <juce_core/juce_core.h>

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace curlop::payload {

inline bool readFiniteDouble(const juce::var& value, double& out)
{
    if (value.isVoid() || value.isUndefined() || value.isBool())
        return false;

    if (value.isInt() || value.isInt64() || value.isDouble())
    {
        const double parsed = static_cast<double>(value);
        if (! std::isfinite(parsed))
            return false;
        out = parsed;
        return true;
    }

    if (value.isString())
    {
        const auto text = value.toString().trim().toStdString();
        if (text.empty())
            return false;

        errno = 0;
        char* end = nullptr;
        const double parsed = std::strtod(text.c_str(), &end);
        if (end == text.c_str() || *end != '\0' || errno == ERANGE || ! std::isfinite(parsed))
            return false;

        out = parsed;
        return true;
    }

    return false;
}

inline bool readInt(const juce::var& value, int& out)
{
    if (value.isVoid() || value.isUndefined() || value.isBool())
        return false;

    if (value.isInt() || value.isInt64())
    {
        const auto parsed = static_cast<juce::int64>(value);
        if (parsed < std::numeric_limits<int>::min()
            || parsed > std::numeric_limits<int>::max())
            return false;
        out = static_cast<int>(parsed);
        return true;
    }

    if (value.isDouble())
    {
        const double parsed = static_cast<double>(value);
        if (! std::isfinite(parsed)
            || std::floor(parsed) != parsed
            || parsed < static_cast<double>(std::numeric_limits<int>::min())
            || parsed > static_cast<double>(std::numeric_limits<int>::max()))
            return false;
        out = static_cast<int>(parsed);
        return true;
    }

    if (value.isString())
    {
        const auto text = value.toString().trim().toStdString();
        if (text.empty())
            return false;

        errno = 0;
        char* end = nullptr;
        const long parsed = std::strtol(text.c_str(), &end, 10);
        if (end == text.c_str() || *end != '\0' || errno == ERANGE
            || parsed < std::numeric_limits<int>::min()
            || parsed > std::numeric_limits<int>::max())
            return false;

        out = static_cast<int>(parsed);
        return true;
    }

    return false;
}

inline bool readInt64(const juce::var& value, juce::int64& out)
{
    if (value.isVoid() || value.isUndefined() || value.isBool())
        return false;

    if (value.isInt() || value.isInt64())
    {
        out = static_cast<juce::int64>(value);
        return true;
    }

    if (value.isDouble())
    {
        const double parsed = static_cast<double>(value);
        constexpr double int64LowerInclusive = -9223372036854775808.0;
        constexpr double int64UpperExclusive = 9223372036854775808.0;
        if (! std::isfinite(parsed)
            || std::floor(parsed) != parsed
            || parsed < int64LowerInclusive
            || parsed >= int64UpperExclusive)
            return false;
        out = static_cast<juce::int64>(parsed);
        return true;
    }

    if (value.isString())
    {
        const auto text = value.toString().trim().toStdString();
        if (text.empty())
            return false;

        errno = 0;
        char* end = nullptr;
        const long long parsed = std::strtoll(text.c_str(), &end, 10);
        if (end == text.c_str() || *end != '\0' || errno == ERANGE)
            return false;

        out = static_cast<juce::int64>(parsed);
        return true;
    }

    return false;
}

inline int intProperty(const juce::var& object, const char* name, int fallback)
{
    int parsed = fallback;
    return readInt(object.getProperty(name, juce::var()), parsed) ? parsed : fallback;
}

inline bool readIntProperty(const juce::var& object, const char* name, int& out)
{
    return readInt(object.getProperty(name, juce::var()), out);
}

inline double doubleProperty(const juce::var& object, const char* name, double fallback)
{
    double parsed = fallback;
    return readFiniteDouble(object.getProperty(name, juce::var()), parsed) ? parsed : fallback;
}

inline bool readDoubleProperty(const juce::var& object, const char* name, double& out)
{
    return readFiniteDouble(object.getProperty(name, juce::var()), out);
}

inline float floatProperty(const juce::var& object, const char* name, float fallback)
{
    double parsed = fallback;
    return readFiniteDouble(object.getProperty(name, juce::var()), parsed)
            && std::abs(parsed) <= static_cast<double>(std::numeric_limits<float>::max())
        ? static_cast<float>(parsed)
        : fallback;
}

inline bool readFloatProperty(const juce::var& object, const char* name, float& out)
{
    double parsed = 0.0;
    if (! readFiniteDouble(object.getProperty(name, juce::var()), parsed)
        || std::abs(parsed) > static_cast<double>(std::numeric_limits<float>::max()))
        return false;
    out = static_cast<float>(parsed);
    return true;
}

} // namespace curlop::payload
