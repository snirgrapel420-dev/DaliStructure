// Export.h — DAW-independent export layer (Sections 17–18).
//   Analysis Engine -> StructureDocument -> ExportPlan -> DAW Adapter
// The ExportPlan maps reference BARS onto the user's project grid. Because
// every marker is placed in bars/beats, the result is independent of the
// project's BPM; seconds are only computed for adapters that need them.
#pragma once
#include "dali/Model.h"
#include <string>
#include <vector>

namespace dali {

struct ExportSettings {
    int projectStartBar = 1;          // project bar where reference bar 1 lands
    double projectBpm = 0;            // 0 = use the reference BPM
    int projectBeatsPerBar = 4;
    bool includeEvents = false;       // add event markers besides sections
    Importance minEventImportance = Importance::Major;
    bool includeSectionBarRange = false;  // "DROP" vs "DROP 49-80"
};

struct ExportMarker {
    std::string name;
    int projectBar = 1;               // 1-based, may be fractional for events -> see beat
    double projectBeat = 0;           // 0-based absolute beat in the project
    double projectTimeSec = 0;        // at projectBpm (constant tempo)
    bool isSection = true;
};

struct ExportPlan {
    double projectBpm = 120;
    int beatsPerBar = 4;
    std::vector<ExportMarker> markers;   // sorted by projectBeat
};

ExportPlan buildExportPlan(const StructureDocument& doc, const ExportSettings& settings);

// Adapter interface: one implementation per DAW / format.
class IStructureExporter {
public:
    virtual ~IStructureExporter() = default;
    virtual const char* id() const = 0;             // "ableton-als", "ableton-link", ...
    virtual const char* displayName() const = 0;
    virtual const char* fileExtension() const = 0;  // "" for non-file adapters
    virtual bool write(const ExportPlan& plan, const StructureDocument& doc,
                       const std::string& destination, std::string& error) = 0;
};

} // namespace dali
