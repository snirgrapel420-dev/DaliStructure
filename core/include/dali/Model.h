// DaliStructure — Reference Arrangement Mapper
// (c) Dali Audio
//
// Model.h — the internal Structure Data model.
// This header is UI-independent and DAW-independent. Everything else
// (UI, cache, exporters, future Project Alignment) consumes this model.
//
// TIMING CONVENTIONS (single source of truth = TempoMap):
//   * Bars are 1-based, as in a DAW. Bar 1 starts at global beat 0.
//   * Beats are 0-based global beat indices; beat b = bar (b / beatsPerBar) + 1.
//   * Section [startBar, endBar] is INCLUSIVE (display "1–32" = 32 bars).
//     startBeat = (startBar-1)*bpb, endBeat = endBar*bpb (exclusive).
//   * Seconds are always DERIVED from the TempoMap, never authored.
//   * Events use a fractional 1-based bar position (bar 17.0 = downbeat of bar 17).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace dali {

constexpr int kSchemaVersion = 1;
constexpr const char* kEngineVersion = "1.0.0";

// ---------------------------------------------------------------- enums
enum class SectionType { Intro, Groove, Build, PreDrop, Drop, Main, Breakdown, Break, Transition, Outro, Section };
enum class EventCategory { Drums, Bass, Musical, Vocal, FX, Structure, User };
enum class EventType {
    KickIn, KickOut, FullDrumsIn, PercussionIn, PercussionChange,
    BassIn, BassOut, BassPatternChange,
    LeadIn, LeadOut, MelodyIn, ChordPadIn,
    VocalIn, VocalOut,
    Riser, Downlifter, Sweep, Impact, MajorFx,
    Drop, Breakdown, MajorTransition,
    UserMarker
};
enum class Importance { Major = 0, Medium = 1, Minor = 2 };
enum class Origin { Auto, User };

const char* toString(SectionType);
const char* toString(EventType);
const char* toString(EventCategory);
const char* toString(Importance);
const char* toString(Origin);
const char* displayName(EventType);      // "Kick In", "Lead / Melody In", ...
bool parse(const std::string&, SectionType&);
bool parse(const std::string&, EventType&);
bool parse(const std::string&, Importance&);
bool parse(const std::string&, Origin&);
EventCategory categoryOf(EventType);

// ---------------------------------------------------------------- tempo
struct TempoSegment {
    int startBeat = 0;   // global beat index where this tempo begins
    double bpm = 0;
};

struct TempoMap {
    std::vector<double> beatTimes;   // seconds, index = global beat; beat 0 = downbeat of bar 1
    int beatsPerBar = 4;
    double nominalBpm = 0;           // representative BPM (median)
    bool stable = true;              // true = constant tempo grid
    double beatConfidence = 0;       // 0..1
    double downbeatConfidence = 0;   // 0..1
    std::vector<TempoSegment> segments;

    bool empty() const { return beatTimes.size() < 2; }
    int beatCount() const { return (int) beatTimes.size(); }
    int barCount() const { return beatTimes.empty() ? 0 : (int) beatTimes.size() / beatsPerBar; }
    double timeAtBeat(double beat) const;   // interpolates / extrapolates
    double beatAtTime(double seconds) const;
    double timeAtBar(double bar1) const { return timeAtBeat((bar1 - 1.0) * beatsPerBar); }
    double barAtTime(double seconds) const { return beatAtTime(seconds) / beatsPerBar + 1.0; }
};

// ---------------------------------------------------------------- reference
struct KeyInfo {
    std::string name;        // "A minor", empty if unknown
    double confidence = 0;
};

struct ReferenceInfo {
    std::string filePath;
    std::string fileName;
    std::string contentHash;     // hash of decoded audio, used for the analysis cache
    double sampleRate = 0;
    int channels = 0;
    double durationSec = 0;
    double bpm = 0;
    KeyInfo key;
    int barCount = 0;
    double firstDownbeatSec = 0; // time of bar 1 in the audio file
};

// ---------------------------------------------------------------- structure
struct Section {
    std::string id;
    SectionType type = SectionType::Section;
    std::string label;           // display name, defaults to type name; user can rename
    int startBar = 1;            // inclusive, 1-based
    int endBar = 1;              // inclusive
    double startTime = 0;        // derived
    double endTime = 0;          // derived
    double energy = 0;           // 0..100 relative
    double confidence = 0;       // 0..1
    std::vector<std::string> elements;   // "Kick", "Bass", ... detected in this section
    Origin origin = Origin::Auto;

    int durationBars() const { return endBar - startBar + 1; }
    int startBeat(int bpb = 4) const { return (startBar - 1) * bpb; }
    int endBeat(int bpb = 4) const { return endBar * bpb; }
};

struct Event {
    std::string id;
    EventType type = EventType::UserMarker;
    std::string label;           // display text; for user markers the user's text
    double bar = 1;              // fractional 1-based bar position
    double time = 0;             // derived
    double lengthBars = 0;       // e.g. riser length, 0 = point event
    Importance importance = Importance::Medium;
    double confidence = 0;
    Origin origin = Origin::Auto;
};

struct EnergyPoint {
    double bar = 1;              // fractional 1-based position
    double value = 0;            // 0..100 relative
};

// Per-bar activity (0..1) of a musical element. Used by the Details panel;
// not shown on the main timeline (Structure First).
struct ElementTrack {
    std::string name;
    std::vector<float> activity;   // index = bar-1
};

struct AnalysisDiagnostics {
    std::string engineVersion = kEngineVersion;
    bool usedStems = false;
    bool usedNeuralBeats = false;
    std::vector<std::string> warnings;
};

// Ready for Reference-vs-Project comparison (Section 19). V1 stores the
// user's project mapping; project audio analysis can fill projectSections later.
struct ProjectAlignment {
    double projectBpm = 0;         // 0 = unknown / same as reference
    int projectStartBar = 1;       // where reference bar 1 maps in the project
    std::vector<Section> projectSections;
};

struct StructureDocument {
    int schemaVersion = kSchemaVersion;
    ReferenceInfo reference;
    TempoMap tempo;

    // Automatic result, never modified by edits (allows "Reset to automatic").
    std::vector<Section> autoSections;
    std::vector<Event> autoEvents;

    // Current (possibly user-edited) state shown in the UI and exported.
    std::vector<Section> sections;
    std::vector<Event> events;
    bool userEdited = false;

    std::vector<EnergyPoint> energy;        // beat resolution
    std::vector<ElementTrack> elements;
    AnalysisDiagnostics diagnostics;
    ProjectAlignment alignment;

    // Re-derive every seconds field from bars via the tempo map.
    void recomputeTimes();
    const Section* sectionAtBar(double bar) const;
    double energyAtBar(double bar) const;
};

} // namespace dali
