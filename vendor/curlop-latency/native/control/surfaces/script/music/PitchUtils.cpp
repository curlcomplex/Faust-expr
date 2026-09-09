#include "control/surfaces/script/music/PitchUtils.h"
#include "control/surfaces/script/ScriptParser.h"
#include "control/surfaces/script/music/MusicData.h"
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <string>

namespace curlop::pitch {
namespace {

bool parseStrictFiniteDouble(const juce::String& raw, double& out)
{
    const auto trimmed = raw.trim();
    if (trimmed.isEmpty()) return false;

    const auto text = trimmed.toStdString();
    char* end = nullptr;
    errno = 0;
    const double value = std::strtod(text.c_str(), &end);
    if (end == text.c_str()
        || end == nullptr
        || *end != '\0'
        || errno == ERANGE
        || ! std::isfinite(value))
        return false;

    out = value;
    return true;
}

bool parseFrequencyStrict(const juce::String& freqStr, double& hzOut)
{
    const auto lower = freqStr.trim().toLowerCase();
    if (lower.endsWith("khz"))
    {
        double value = 0.0;
        if (! parseStrictFiniteDouble(lower.substring(0, lower.length() - 3), value))
            return false;
        hzOut = value * 1000.0;
        return std::isfinite(hzOut) && hzOut > 0.0;
    }
    if (lower.endsWith("hz"))
    {
        double value = 0.0;
        if (! parseStrictFiniteDouble(lower.substring(0, lower.length() - 2), value))
            return false;
        hzOut = value;
        return std::isfinite(hzOut) && hzOut > 0.0;
    }
    return parseStrictFiniteDouble(lower, hzOut)
           && std::isfinite(hzOut)
           && hzOut > 0.0;
}

} // namespace

// Matches JS regex /^([a-g])([#b]?)([0-9])$/i — letter + optional accidental + octave.
int noteToMidi(const juce::String& noteStr)
{
    if (noteStr.length() < 2 || noteStr.length() > 3) return -1;

    auto lower = noteStr.toLowerCase();
    const juce::juce_wchar letter = lower[0];
    if (letter < 'a' || letter > 'g') return -1;

    int pos = 1;
    int accidental = 0;
    if (lower[1] == '#') { accidental = 1;  pos = 2; }
    else if (lower[1] == 'b') { accidental = -1; pos = 2; }

    if (pos >= lower.length()) return -1;
    const juce::juce_wchar octCh = lower[pos];
    if (octCh < '0' || octCh > '9') return -1;
    if (pos + 1 != lower.length()) return -1;  // trailing garbage
    const int octave = (int)(octCh - '0');

    // Semitone offset from C: c=0 d=2 e=4 f=5 g=7 a=9 b=11
    static constexpr int SEMI[7] = { 9, 11, 0, 2, 4, 5, 7 }; // a,b,c,d,e,f,g
    int semitone = SEMI[letter - 'a'] + accidental;

    // Standard MIDI: C4 = 60. Formula: (octave+1)*12 + semitone.
    return (octave + 1) * 12 + semitone;
}

double semiToHz(double semitone)
{
    return 440.0 * std::pow(2.0, (semitone - 69.0) / 12.0);
}

// '+10c' → 10, '-5c' → -5. Strips trailing 'c' (or 'C').
double parseCents(const juce::String& centsStr)
{
    const auto lower = centsStr.trim().toLowerCase();
    if (! lower.endsWithChar('c') || lower.length() <= 1)
        return 0.0;

    double cents = 0.0;
    return parseStrictFiniteDouble(lower.substring(0, lower.length() - 1), cents)
           ? cents
           : 0.0;
}

// '440hz' → 440, '1khz' → 1000, '440.5hz' → 440.5.
double parseFreq(const juce::String& freqStr)
{
    double hz = -1.0;
    return parseFrequencyStrict(freqStr, hz) ? hz : -1.0;
}

// Cut 0.D-3a: PitchChain → ResolvedPitch (mirror of JS pitchUtils.js
// resolvePitch at line 110).
ResolvedPitch resolvePitch(const curlop::script::PitchChain& chain)
{
    ResolvedPitch r;

    if (chain.cents.isNotEmpty()) r.cents_offset = parseCents(chain.cents);

    if (chain.note.isNotEmpty()) {
        const int midi = noteToMidi(chain.note);
        if (midi >= 0) {
            r.root_semitone = midi;
            r.hasPitch = true;
        }
    }

    if (chain.freq.isNotEmpty()) {
        const double hz = parseFreq(chain.freq);
        if (hz > 0.0 && std::isfinite(hz)) {
            r.freq_hz = hz;
            r.hasFreq = true;
        }
    }

    // Voicings: root + chord intervals. Empty if no note or no chord.
    if (r.hasPitch) {
        if (chain.chord.isNotEmpty()) {
            auto intervals = curlop::music::chordToIntervals(
                std::string(chain.chord.toRawUTF8()));
            if (intervals.empty()) intervals = { 0.0 };
            r.voicings.reserve(intervals.size());
            for (double iv : intervals) r.voicings.push_back(r.root_semitone + iv);
        } else {
            r.voicings.push_back(static_cast<double>(r.root_semitone));
        }
    }

    // B-274 — explicit voicings (set by compile-time pitch transforms such
    // as quantizeStepPitches) override note/chord derivation verbatim, so a
    // per-voicing rewrite survives to NOTE_CHORD emission.
    if (! chain.explicitVoicings.empty()) {
        r.voicings = chain.explicitVoicings;
        r.root_semitone = static_cast<int>(std::lround(chain.explicitVoicings[0]));
        r.hasPitch = true;
    }

    // Unified float pitch: Hz passthrough if freq present, else semitone +
    // cents-as-fraction. Otherwise -1.0 sentinel.
    if (r.hasFreq)       r.final_pitch = r.freq_hz;
    else if (r.hasPitch) r.final_pitch = r.root_semitone + r.cents_offset / 100.0;
    else                 r.final_pitch = -1.0;

    return r;
}

double resolvePitchHz(const curlop::script::PitchChain& chain)
{
    auto r = resolvePitch(chain);
    if (r.hasFreq)  return r.freq_hz;
    if (r.hasPitch) return semiToHz(r.final_pitch);
    return 440.0;
}

} // namespace curlop::pitch
