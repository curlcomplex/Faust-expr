#include "control/surfaces/script/music/MusicData.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>

namespace curlop::music {

const std::vector<ChordEntry>& chordTable()
{
    // Built once on first call. Float intervals preserved exactly per JS
    // musicData.js — microtonal chords (neutral, harm7, just_maj, just_min,
    // slendro_tri, qcluster) must NOT round to integer semitones.
    static const std::vector<ChordEntry> kTable = {
        // ── Standard triads ────────────────────────────────────────────────
        {"maj",         {0, 4, 7}},
        {"min",         {0, 3, 7}},
        {"dim",         {0, 3, 6}},
        {"aug",         {0, 4, 8}},
        {"sus2",        {0, 2, 7}},
        {"sus4",        {0, 5, 7}},

        // ── Seventh chords ─────────────────────────────────────────────────
        {"maj7",        {0, 4, 7, 11}},
        {"min7",        {0, 3, 7, 10}},
        {"dom7",        {0, 4, 7, 10}},
        {"dom",         {0, 4, 7, 10}},
        {"dim7",        {0, 3, 6, 9}},
        {"min7b5",      {0, 3, 6, 10}},
        {"aug7",        {0, 4, 8, 10}},
        {"minmaj7",     {0, 3, 7, 11}},

        // ── Ninth chords ───────────────────────────────────────────────────
        {"maj9",        {0, 4, 7, 11, 14}},
        {"min9",        {0, 3, 7, 10, 14}},
        {"dom9",        {0, 4, 7, 10, 14}},

        // ── Eleventh / thirteenth chords ──────────────────────────────────
        {"maj11",       {0, 4, 7, 11, 14, 17}},
        {"min11",       {0, 3, 7, 10, 14, 17}},
        {"dom11",       {0, 4, 7, 10, 14, 17}},
        {"maj13",       {0, 4, 7, 11, 14, 17, 21}},
        {"min13",       {0, 3, 7, 10, 14, 17, 21}},
        {"dom13",       {0, 4, 7, 10, 14, 17, 21}},

        // ── Added tone chords ─────────────────────────────────────────────
        {"add9",        {0, 4, 7, 14}},
        {"madd9",       {0, 3, 7, 14}},
        {"add11",       {0, 4, 7, 17}},

        // ── Sixth chords ──────────────────────────────────────────────────
        {"maj6",        {0, 4, 7, 9}},
        {"min6",        {0, 3, 7, 9}},

        // ── Altered dominants ─────────────────────────────────────────────
        {"dom7b9",      {0, 4, 7, 10, 13}},
        {"dom7s9",      {0, 4, 7, 10, 15}},
        {"dom7b5",      {0, 4, 6, 10}},
        {"dom7s5",      {0, 4, 8, 10}},
        {"dom7s11",     {0, 4, 7, 10, 18}},
        {"maj7s11",     {0, 4, 7, 11, 18}},

        // ── 7sus ──────────────────────────────────────────────────────────
        {"sus7",        {0, 5, 7, 10}},

        // ── Named / exotic chords ─────────────────────────────────────────
        {"hendrix",     {0, 4, 7, 10, 15}},
        {"sowhat",      {0, 5, 10, 15, 19}},
        {"prometheus",  {0, 6, 10, 16, 21, 26}},
        {"mystic",      {0, 6, 10, 16, 21, 26}},
        {"tristan",     {0, 6, 10, 15}},
        {"mu",          {0, 2, 4, 7}},
        {"power",       {0, 7}},
        {"cluster",     {0, 1, 2}},

        // ── Microtonal / Just Intonation ──────────────────────────────────
        {"neutral",     {0, 3.5, 7}},
        {"harm7",       {0, 4, 7, 9.69}},
        {"just_maj",    {0, 3.86, 7.02}},
        {"just_min",    {0, 3.16, 7.02}},
        {"slendro_tri", {0, 2.4, 4.8}},
        {"qcluster",    {0, 0.5, 1, 1.5}},
    };
    return kTable;
}

std::vector<double> chordToIntervals(std::string_view chordName)
{
    for (const auto& e : chordTable()) if (e.name == chordName) return e.intervals;
    return {};
}

// ─── Scales (Cut 0.D-3b) ──────────────────────────────────────────────────
// Byte-for-byte port of src/sequencer/musicData.js SCALES. Order matters for
// nothing (name lookup only), but kept identical to JS for reviewability.

const std::vector<ScaleEntry>& scaleTable()
{
    static const std::vector<ScaleEntry> kTable = {
        {"chromatic",      {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}},
        {"major",          {0, 2, 4, 5, 7, 9, 11}},
        {"minor",          {0, 2, 3, 5, 7, 8, 10}},
        {"dorian",         {0, 2, 3, 5, 7, 9, 10}},
        {"phrygian",       {0, 1, 3, 5, 7, 8, 10}},
        {"lydian",         {0, 2, 4, 6, 7, 9, 11}},
        {"mixolydian",     {0, 2, 4, 5, 7, 9, 10}},
        {"locrian",        {0, 1, 3, 5, 6, 8, 10}},
        {"harmonic_minor", {0, 2, 3, 5, 7, 8, 11}},
        {"melodic_minor",  {0, 2, 3, 5, 7, 9, 11}},
        {"major_pent",     {0, 2, 4, 7, 9}},
        {"minor_pent",     {0, 3, 5, 7, 10}},
        {"blues",          {0, 3, 5, 6, 7, 10}},
        {"blues_major",    {0, 2, 3, 4, 7, 9}},
        {"whole",          {0, 2, 4, 6, 8, 10}},
        {"tizita",         {0, 2, 4, 7, 9}},
        {"tizita_min",     {0, 2, 3, 7, 8}},
        {"bati",           {0, 4, 5, 7, 11}},
        {"anchihoye",      {0, 1, 5, 6, 9}},
        {"hirajoshi",      {0, 2, 3, 7, 8}},
        {"iwato",          {0, 1, 5, 6, 10}},
        {"pelog",          {0, 1, 3, 7, 8}},
        {"slendro",        {0, 2.4, 4.8, 7.2, 9.6}},
        {"maqam_rast",     {0, 2, 3.5, 5, 7, 9, 10.5}},
        {"maqam_bayati",   {0, 1.5, 3, 5, 7, 8, 10}},
        {"maqam_sikah",    {0, 1.5, 3.5, 5.5, 7, 8.5, 10.5}},
        {"quarter_tone",   {0, 0.5, 1, 1.5, 2, 2.5, 3, 3.5, 4, 4.5, 5, 5.5,
                            6, 6.5, 7, 7.5, 8, 8.5, 9, 9.5, 10, 10.5, 11, 11.5}},
    };
    return kTable;
}

namespace {
static std::string toLower(std::string_view s)
{
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return out;
}
} // namespace

const std::vector<double>& scaleIntervals(std::string_view scaleName)
{
    static const std::vector<double> kEmpty{};
    const auto& tbl = scaleTable();
    for (const auto& e : tbl) if (e.name == scaleName) return e.intervals;
    // Case-insensitive second pass.
    const std::string lower = toLower(scaleName);
    for (const auto& e : tbl) if (std::string(e.name) == lower) return e.intervals;
    // Fallback: chromatic.
    for (const auto& e : tbl) if (e.name == "chromatic") return e.intervals;
    return kEmpty;
}

int noteChroma(std::string_view rootNote)
{
    // Resolve root to 0-11 chromatic index (enharmonic normalization).
    std::string r = toLower(rootNote);
    // Enharmonic flats → sharps.
    if (r == "db") r = "c#";
    else if (r == "eb") r = "d#";
    else if (r == "gb") r = "f#";
    else if (r == "ab") r = "g#";
    else if (r == "bb") r = "a#";

    // Uppercase first char (C, C#, etc. per kNoteNames).
    if (!r.empty()) r[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(r[0])));

    for (size_t i = 0; i < kNoteNames.size(); ++i)
        if (kNoteNames[i] == r) return static_cast<int>(i);
    return 0;
}

double quantizeToScale(double pitchHz, std::string_view scaleName,
                       std::string_view rootNote)
{
    const auto& intervals = scaleIntervals(scaleName);
    if (intervals.empty()) return pitchHz;

    const int rootOffset = noteChroma(rootNote);

    // Convert Hz → MIDI float (A4 = 69, 440Hz).
    const double midi = 69.0 + 12.0 * std::log2(pitchHz / 440.0);
    double currentChroma = std::fmod(midi, 12.0);
    if (currentChroma < 0) currentChroma += 12.0;

    // Find nearest scale degree (circular distance).
    double closestDiff = std::numeric_limits<double>::infinity();
    double bestTargetChroma = currentChroma;
    for (double interval : intervals) {
        double targetChroma = std::fmod(interval + rootOffset, 12.0);
        if (targetChroma < 0) targetChroma += 12.0;
        double diff = std::abs(currentChroma - targetChroma);
        if (diff > 6.0) diff = 12.0 - diff;
        if (diff < closestDiff) {
            closestDiff = diff;
            bestTargetChroma = targetChroma;
        }
    }

    double shift = bestTargetChroma - currentChroma;
    if (shift > 6.0)  shift -= 12.0;
    if (shift < -6.0) shift += 12.0;
    return pitchHz * std::pow(2.0, shift / 12.0);
}

} // namespace curlop::music
