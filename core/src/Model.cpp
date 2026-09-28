#include "dali/Model.h"
#include <algorithm>
#include <cmath>

namespace dali {

namespace {
struct SectionName { SectionType t; const char* s; };
const SectionName kSectionNames[] = {
    {SectionType::Intro, "INTRO"},         {SectionType::Groove, "GROOVE"},
    {SectionType::Build, "BUILD"},         {SectionType::PreDrop, "PRE-DROP"},
    {SectionType::Drop, "DROP"},           {SectionType::Main, "MAIN"},
    {SectionType::Breakdown, "BREAKDOWN"}, {SectionType::Break, "BREAK"},
    {SectionType::Transition, "TRANSITION"}, {SectionType::Outro, "OUTRO"},
    {SectionType::Section, "SECTION"},
};

struct EventName { EventType t; const char* id; const char* display; EventCategory c; };
const EventName kEventNames[] = {
    {EventType::KickIn, "kick_in", "Kick In", EventCategory::Drums},
    {EventType::KickOut, "kick_out", "Kick Out", EventCategory::Drums},
    {EventType::FullDrumsIn, "full_drums_in", "Full Drums In", EventCategory::Drums},
    {EventType::PercussionIn, "percussion_in", "Percussion In", EventCategory::Drums},
    {EventType::PercussionChange, "percussion_change", "Percussion Change", EventCategory::Drums},
    {EventType::BassIn, "bass_in", "Bass In", EventCategory::Bass},
    {EventType::BassOut, "bass_out", "Bass Out", EventCategory::Bass},
    {EventType::BassPatternChange, "bass_pattern_change", "Bass Pattern Change", EventCategory::Bass},
    {EventType::LeadIn, "lead_in", "Lead / Melody In", EventCategory::Musical},
    {EventType::LeadOut, "lead_out", "Lead / Melody Out", EventCategory::Musical},
    {EventType::MelodyIn, "melody_in", "Melody In", EventCategory::Musical},
    {EventType::ChordPadIn, "chord_pad_in", "Chord / Pad In", EventCategory::Musical},
    {EventType::VocalIn, "vocal_in", "Vocal In", EventCategory::Vocal},
    {EventType::VocalOut, "vocal_out", "Vocal Out", EventCategory::Vocal},
    {EventType::Riser, "riser", "Riser", EventCategory::FX},
    {EventType::Downlifter, "downlifter", "Downlifter", EventCategory::FX},
    {EventType::Sweep, "sweep", "Sweep", EventCategory::FX},
    {EventType::Impact, "impact", "Impact", EventCategory::FX},
    {EventType::MajorFx, "major_fx", "Major FX", EventCategory::FX},
    {EventType::Drop, "drop", "Drop", EventCategory::Structure},
    {EventType::Breakdown, "breakdown", "Breakdown", EventCategory::Structure},
    {EventType::MajorTransition, "major_transition", "Major Transition", EventCategory::Structure},
    {EventType::UserMarker, "user_marker", "Marker", EventCategory::User},
};
} // namespace

const char* toString(SectionType t) {
    for (auto& n : kSectionNames) if (n.t == t) return n.s;
    return "SECTION";
}
bool parse(const std::string& s, SectionType& out) {
    for (auto& n : kSectionNames) if (s == n.s) { out = n.t; return true; }
    return false;
}
const char* toString(EventType t) {
    for (auto& n : kEventNames) if (n.t == t) return n.id;
    return "user_marker";
}
const char* displayName(EventType t) {
    for (auto& n : kEventNames) if (n.t == t) return n.display;
    return "Marker";
}
EventCategory categoryOf(EventType t) {
    for (auto& n : kEventNames) if (n.t == t) return n.c;
    return EventCategory::User;
}
bool parse(const std::string& s, EventType& out) {
    for (auto& n : kEventNames) if (s == n.id) { out = n.t; return true; }
    return false;
}
const char* toString(EventCategory c) {
    switch (c) {
        case EventCategory::Drums: return "DRUMS";
        case EventCategory::Bass: return "BASS";
        case EventCategory::Musical: return "MUSICAL";
        case EventCategory::Vocal: return "VOCAL";
        case EventCategory::FX: return "FX";
        case EventCategory::Structure: return "STRUCTURE";
        case EventCategory::User: return "USER";
    }
    return "USER";
}
const char* toString(Importance i) {
    switch (i) { case Importance::Major: return "MAJOR"; case Importance::Medium: return "MEDIUM"; default: return "MINOR"; }
}
bool parse(const std::string& s, Importance& out) {
    if (s == "MAJOR") { out = Importance::Major; return true; }
    if (s == "MEDIUM") { out = Importance::Medium; return true; }
    if (s == "MINOR") { out = Importance::Minor; return true; }
    return false;
}
const char* toString(Origin o) { return o == Origin::User ? "user" : "auto"; }
bool parse(const std::string& s, Origin& out) {
    if (s == "user") { out = Origin::User; return true; }
    if (s == "auto") { out = Origin::Auto; return true; }
    return false;
}

// ---------------------------------------------------------------- TempoMap
double TempoMap::timeAtBeat(double beat) const {
    const int n = (int) beatTimes.size();
    if (n == 0) return 0;
    if (n == 1) return beatTimes[0];
    if (beat <= 0) return beatTimes[0] + beat * (beatTimes[1] - beatTimes[0]);
    if (beat >= n - 1) return beatTimes[n - 1] + (beat - (n - 1)) * (beatTimes[n - 1] - beatTimes[n - 2]);
    const int i = (int) std::floor(beat);
    const double f = beat - i;
    return beatTimes[i] + f * (beatTimes[i + 1] - beatTimes[i]);
}

double TempoMap::beatAtTime(double t) const {
    const int n = (int) beatTimes.size();
    if (n < 2) return 0;
    if (t <= beatTimes[0]) return (t - beatTimes[0]) / (beatTimes[1] - beatTimes[0]);
    if (t >= beatTimes[n - 1]) return (n - 1) + (t - beatTimes[n - 1]) / (beatTimes[n - 1] - beatTimes[n - 2]);
    auto it = std::upper_bound(beatTimes.begin(), beatTimes.end(), t);
    const int i = (int) (it - beatTimes.begin()) - 1;
    return i + (t - beatTimes[i]) / (beatTimes[i + 1] - beatTimes[i]);
}

// ---------------------------------------------------------------- Document
void StructureDocument::recomputeTimes() {
    auto fixSections = [&](std::vector<Section>& v) {
        for (auto& s : v) {
            s.startTime = tempo.timeAtBar(s.startBar);
            s.endTime = tempo.timeAtBar(s.endBar + 1);
        }
    };
    auto fixEvents = [&](std::vector<Event>& v) {
        for (auto& e : v) e.time = tempo.timeAtBar(e.bar);
    };
    fixSections(autoSections); fixSections(sections);
    fixEvents(autoEvents); fixEvents(events);
}

const Section* StructureDocument::sectionAtBar(double bar) const {
    for (auto& s : sections)
        if (bar >= s.startBar && bar < s.endBar + 1) return &s;
    return nullptr;
}

double StructureDocument::energyAtBar(double bar) const {
    if (energy.empty()) return 0;
    if (bar <= energy.front().bar) return energy.front().value;
    if (bar >= energy.back().bar) return energy.back().value;
    auto it = std::lower_bound(energy.begin(), energy.end(), bar,
                               [](const EnergyPoint& p, double b) { return p.bar < b; });
    const auto& b = *it; const auto& a = *(it - 1);
    const double f = (bar - a.bar) / std::max(1e-9, b.bar - a.bar);
    return a.value + f * (b.value - a.value);
}

} // namespace dali
