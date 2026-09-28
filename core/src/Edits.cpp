#include "dali/Edits.h"
#include "dali/Export.h"
#include <algorithm>
#include <cmath>

namespace dali::edits {

static Section* findSection(StructureDocument& d, const std::string& id, size_t* index = nullptr) {
    for (size_t i = 0; i < d.sections.size(); ++i)
        if (d.sections[i].id == id) { if (index) *index = i; return &d.sections[i]; }
    return nullptr;
}

bool renameSection(StructureDocument& d, const std::string& id, const std::string& label) {
    auto* s = findSection(d, id);
    if (!s || label.empty()) return false;
    s->label = label; s->origin = Origin::User; d.userEdited = true;
    return true;
}

bool setSectionType(StructureDocument& d, const std::string& id, SectionType type) {
    auto* s = findSection(d, id);
    if (!s) return false;
    const bool labelWasDefault = s->label == toString(s->type);
    s->type = type;
    if (labelWasDefault) s->label = toString(type);
    s->confidence = 1.0;            // user-confirmed
    s->origin = Origin::User; d.userEdited = true;
    return true;
}

bool moveSectionStart(StructureDocument& d, const std::string& id, int newStartBar) {
    size_t i = 0;
    auto* s = findSection(d, id, &i);
    if (!s || i == 0) return false;               // first section always starts at bar 1
    Section& prev = d.sections[i - 1];
    newStartBar = std::clamp(newStartBar, prev.startBar + 1, s->endBar);
    if (newStartBar == s->startBar) return true;
    prev.endBar = newStartBar - 1;
    s->startBar = newStartBar;
    prev.origin = s->origin = Origin::User;
    d.userEdited = true;
    d.recomputeTimes();
    return true;
}

bool deleteEvent(StructureDocument& d, const std::string& id) {
    auto it = std::find_if(d.events.begin(), d.events.end(), [&](const Event& e) { return e.id == id; });
    if (it == d.events.end()) return false;
    d.events.erase(it); d.userEdited = true;
    return true;
}

std::string addMarker(StructureDocument& d, double bar, const std::string& label) {
    int n = 1;
    auto exists = [&](const std::string& id) {
        return std::any_of(d.events.begin(), d.events.end(), [&](const Event& e) { return e.id == id; });
    };
    std::string id;
    do { id = "user-" + std::to_string(n++); } while (exists(id));
    Event e;
    e.id = id; e.type = EventType::UserMarker; e.label = label.empty() ? "Marker" : label;
    e.bar = std::max(1.0, bar); e.time = d.tempo.timeAtBar(e.bar);
    e.importance = Importance::Major; e.confidence = 1.0; e.origin = Origin::User;
    d.events.insert(std::upper_bound(d.events.begin(), d.events.end(), e,
                                     [](const Event& a, const Event& b) { return a.bar < b.bar; }), e);
    d.userEdited = true;
    return id;
}

void resetToAutomatic(StructureDocument& d) {
    d.sections = d.autoSections;
    d.events = d.autoEvents;
    d.userEdited = false;
}

} // namespace dali::edits

namespace dali {

ExportPlan buildExportPlan(const StructureDocument& doc, const ExportSettings& st) {
    ExportPlan plan;
    plan.projectBpm = st.projectBpm > 0 ? st.projectBpm : (doc.reference.bpm > 0 ? doc.reference.bpm : 120.0);
    plan.beatsPerBar = std::max(1, st.projectBeatsPerBar);

    auto place = [&](double refBar, ExportMarker& m) {
        // Bars map 1:1; beats within a bar are rescaled if the meters differ.
        const double barIndex = std::floor(refBar - 1.0);
        const double frac = (refBar - 1.0) - barIndex;
        m.projectBeat = (barIndex + st.projectStartBar - 1) * plan.beatsPerBar + frac * plan.beatsPerBar;
        m.projectBar = (int) std::floor(m.projectBeat / plan.beatsPerBar) + 1;
        m.projectTimeSec = m.projectBeat * 60.0 / plan.projectBpm;
    };

    for (auto& s : doc.sections) {
        ExportMarker m;
        m.name = s.label.empty() ? toString(s.type) : s.label;
        if (st.includeSectionBarRange)
            m.name += " " + std::to_string(s.startBar) + "-" + std::to_string(s.endBar);
        m.isSection = true;
        place(s.startBar, m);
        plan.markers.push_back(m);
    }
    if (st.includeEvents) {
        for (auto& e : doc.events) {
            if ((int) e.importance > (int) st.minEventImportance && e.origin != Origin::User) continue;
            ExportMarker m;
            m.name = e.label.empty() ? displayName(e.type) : e.label;
            m.isSection = false;
            place(e.bar, m);
            plan.markers.push_back(m);
        }
    }
    std::stable_sort(plan.markers.begin(), plan.markers.end(),
                     [](const ExportMarker& a, const ExportMarker& b) { return a.projectBeat < b.projectBeat; });
    return plan;
}

} // namespace dali
