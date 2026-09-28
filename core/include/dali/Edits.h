// Edits.h — minimal manual corrections (Section 24). All edits operate on
// StructureDocument::sections/events; autoSections/autoEvents stay intact so
// the user can always reset to the automatic result.
#pragma once
#include "dali/Model.h"
#include <string>

namespace dali::edits {

bool renameSection(StructureDocument&, const std::string& sectionId, const std::string& label);
bool setSectionType(StructureDocument&, const std::string& sectionId, SectionType type);
// Moves the boundary at the START of `sectionId` to `newStartBar`
// (the previous section's end moves with it). Both sections keep >= 1 bar.
bool moveSectionStart(StructureDocument&, const std::string& sectionId, int newStartBar);
bool deleteEvent(StructureDocument&, const std::string& eventId);
std::string addMarker(StructureDocument&, double bar, const std::string& label);   // returns id
void resetToAutomatic(StructureDocument&);

} // namespace dali::edits
