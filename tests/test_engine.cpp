// DaliStructure core tests: unit tests + end-to-end accuracy on synthetic tracks.
#include "Synth.h"
#include "dali/Edits.h"
#include "dali/Engine.h"
#include "dali/Export.h"
#include "dali/Serialization.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>

using namespace dali;
static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...) do { if (cond) ++g_pass; else { ++g_fail; std::printf("  FAIL: " __VA_ARGS__); std::printf("   [%s:%d]\n", __FILE__, __LINE__); } } while (0)

// ------------------------------------------------------------------ unit tests
static void testTempoMapAndModel() {
    TempoMap tm; tm.beatsPerBar = 4;
    for (int i = 0; i < 64; ++i) tm.beatTimes.push_back(1.0 + i * 0.5);
    CHECK(std::fabs(tm.timeAtBar(1) - 1.0) < 1e-9, "bar1 time");
    CHECK(std::fabs(tm.timeAtBar(3) - 5.0) < 1e-9, "bar3 time");
    CHECK(std::fabs(tm.barAtTime(5.0) - 3.0) < 1e-9, "barAtTime");
    CHECK(std::fabs(tm.timeAtBeat(-2) - 0.0) < 1e-9, "extrapolate before");
    CHECK(std::fabs(tm.timeAtBeat(70) - (1.0 + 70 * 0.5)) < 1e-9, "extrapolate after");
    CHECK(tm.barCount() == 16, "barCount");
}

static StructureDocument sampleDoc() {
    StructureDocument d;
    d.tempo.beatsPerBar = 4;
    for (int i = 0; i < 64 * 4; ++i) d.tempo.beatTimes.push_back(i * 60.0 / 140.0);
    d.tempo.nominalBpm = 140; d.reference.bpm = 140; d.reference.barCount = 64;
    d.reference.fileName = "Test \"quote\" / עברית.wav";
    auto sec = [&](const char* id, SectionType t, int a, int b) { Section s; s.id = id; s.type = t; s.label = toString(t); s.startBar = a; s.endBar = b; s.confidence = 0.8; s.elements = {"Kick", "Bass"}; return s; };
    d.sections = {sec("sec-1", SectionType::Intro, 1, 16), sec("sec-2", SectionType::Build, 17, 24), sec("sec-3", SectionType::Drop, 25, 56), sec("sec-4", SectionType::Outro, 57, 64)};
    Event e; e.id = "ev-1"; e.type = EventType::KickIn; e.bar = 25; e.importance = Importance::Major; e.confidence = 0.9; e.label = "Kick In";
    d.events = {e};
    d.energy = {{1, 10}, {25, 90}};
    d.autoSections = d.sections; d.autoEvents = d.events;
    d.recomputeTimes();
    return d;
}

static void testSerialization() {
    auto d = sampleDoc();
    const std::string s = serialize(d, true);
    StructureDocument r; std::string err;
    CHECK(deserialize(s, r, &err), "deserialize: %s", err.c_str());
    CHECK(r.sections.size() == 4 && r.sections[2].type == SectionType::Drop && r.sections[2].startBar == 25, "sections roundtrip");
    CHECK(r.events.size() == 1 && r.events[0].type == EventType::KickIn, "events roundtrip");
    CHECK(r.reference.fileName == d.reference.fileName, "unicode/escape roundtrip");
    CHECK(serialize(r) == serialize(d), "byte-identical re-serialization");
    StructureDocument bad;
    CHECK(!deserialize("{\"schemaVersion\": 99}", bad), "reject newer schema");
    CHECK(!deserialize("{broken", bad), "reject malformed");
}

static void testEdits() {
    auto d = sampleDoc();
    CHECK(edits::renameSection(d, "sec-3", "MAIN DROP") && d.sections[2].label == "MAIN DROP", "rename");
    CHECK(edits::setSectionType(d, "sec-2", SectionType::PreDrop) && d.sections[1].label == "PRE-DROP", "change type");
    CHECK(edits::moveSectionStart(d, "sec-3", 21) && d.sections[1].endBar == 20 && d.sections[2].startBar == 21, "move boundary");
    CHECK(std::fabs(d.sections[2].startTime - d.tempo.timeAtBar(21)) < 1e-9, "times recomputed after move");
    CHECK(edits::moveSectionStart(d, "sec-3", 5) && d.sections[1].startBar == 17 && d.sections[1].endBar == 17, "boundary clamped");
    const auto id = edits::addMarker(d, 33, "Vocal chop");
    CHECK(!id.empty() && d.events.size() == 2, "add marker");
    CHECK(edits::deleteEvent(d, "ev-1") && d.events.size() == 1, "delete event");
    CHECK(d.userEdited, "userEdited flag");
    edits::resetToAutomatic(d);
    CHECK(d.sections[2].startBar == 25 && d.events.size() == 1 && d.events[0].id == "ev-1" && !d.userEdited, "reset to auto");
}

static void testExportPlan() {
    auto d = sampleDoc();
    ExportSettings st; st.projectStartBar = 9; st.projectBpm = 128;
    auto plan = buildExportPlan(d, st);
    CHECK(plan.markers.size() == 4, "marker count");
    CHECK(plan.markers[2].name == "DROP" && plan.markers[2].projectBar == 33 && std::fabs(plan.markers[2].projectBeat - 128) < 1e-9, "drop at project bar 33 / beat 128");
    CHECK(std::fabs(plan.markers[2].projectTimeSec - 128 * 60.0 / 128) < 1e-9, "seconds at project bpm");
    st.includeEvents = true;
    plan = buildExportPlan(d, st);
    CHECK(plan.markers.size() == 5, "events included");
}

// ------------------------------------------------------------------ end to end
struct Score {
    double bpmErrPct = 0, beatOffsetMs = 0, beatF = 0, downbeatAcc = 0, boundP = 0, boundR = 0, labelStrict = 0, labelLenient = 0, unsure = 0;
};

static const char* nm(SectionType t) { return toString(t); }

static bool lenientEq(SectionType a, SectionType b) {
    auto g = [](SectionType t) {
        switch (t) {
            case SectionType::Groove: case SectionType::Main: return 1;
            case SectionType::Break: case SectionType::Breakdown: return 2;
            case SectionType::Build: case SectionType::PreDrop: return 3;
            default: return 10 + (int) t;
        }
    };
    return g(a) == g(b);
}

static Score evaluate(const synth::Track& tr, const StructureDocument& d, bool verbose) {
    Score sc;
    const auto& truth = tr.truth;
    const double trueBpm = 60.0 / (truth.beats[1] - truth.beats[0]);
    sc.bpmErrPct = 100.0 * std::fabs(d.reference.bpm - trueBpm) / trueBpm;
    // beat F-measure (70 ms)
    int hit = 0, det = 0; double off = 0;
    for (double b : d.tempo.beatTimes) {
        if (b < truth.beats.front() - 0.1 || b > truth.beats.back() + 0.1) continue;
        ++det;
        for (double t : truth.beats) if (std::fabs(t - b) < 0.07) { ++hit; off += b - t; break; }
    }
    sc.beatOffsetMs = hit ? 1000.0 * off / hit : 0;
    const double P = det ? (double) hit / det : 0, R = (double) hit / truth.beats.size();
    sc.beatF = P + R > 0 ? 2 * P * R / (P + R) : 0;
    // downbeats
    int dbHit = 0, dbN = 0;
    for (int bar = 1; bar <= d.tempo.barCount(); ++bar) {
        const double t = d.tempo.timeAtBar(bar);
        if (t < truth.beats.front() - 0.1 || t > truth.beats.back()) continue;
        ++dbN;
        for (size_t k = 0; k < truth.beats.size(); k += 4) if (std::fabs(truth.beats[k] - t) < 0.07) { ++dbHit; break; }
    }
    sc.downbeatAcc = dbN ? (double) dbHit / dbN : 0;
    // map a detected bar to a true bar via time
    auto trueBarAt = [&](double t) {
        for (size_t k = 0; k + 1 < truth.beats.size(); ++k)
            if (t < truth.beats[k + 1]) return (double) k / 4.0 + 1.0 + (t - truth.beats[k]) / (truth.beats[k + 1] - truth.beats[k]) / 4.0;
        return (double) truth.bars + 1;
    };
    std::vector<double> detB, trueB;
    for (auto& s : d.sections) if (s.startBar > 1) detB.push_back(trueBarAt(d.tempo.timeAtBar(s.startBar)));
    for (auto& s : truth.sections) if (s.second.first > 1) trueB.push_back(s.second.first);
    int bh = 0; for (double b : detB) for (double t : trueB) if (std::fabs(b - t) <= 1.01) { ++bh; break; }
    int rh = 0; for (double t : trueB) for (double b : detB) if (std::fabs(b - t) <= 1.01) { ++rh; break; }
    sc.boundP = detB.empty() ? 1 : (double) bh / detB.size();
    sc.boundR = trueB.empty() ? 1 : (double) rh / trueB.size();
    // labels per true bar
    int strict = 0, lenient = 0, unsure = 0;
    for (auto& ts : truth.sections)
        for (int bar = ts.second.first; bar <= ts.second.second; ++bar) {
            const double t = truth.beats[(size_t) (bar - 1) * 4] + 0.01;
            const auto* s = d.sectionAtBar(d.tempo.barAtTime(t));
            if (!s) continue;
            if (s->type == SectionType::Transition || s->type == SectionType::Section) { ++unsure; continue; }
            if (s->type == ts.first) ++strict;
            if (lenientEq(s->type, ts.first)) ++lenient;
        }
    sc.labelStrict = (double) strict / truth.bars;
    sc.labelLenient = (double) lenient / truth.bars;
    sc.unsure = (double) unsure / truth.bars;
    if (verbose) {
        std::printf("    truth : ");
        for (auto& s : truth.sections) std::printf("%s %d-%d | ", nm(s.first), s.second.first, s.second.second);
        std::printf("\n    result: ");
        for (auto& s : d.sections) std::printf("%s %d-%d (%.0f%%) | ", s.label.c_str(), s.startBar, s.endBar, s.confidence * 100);
        std::printf("\n    events (MAJOR+MEDIUM): ");
        for (auto& e : d.events) if (e.importance != Importance::Minor) std::printf("%s@%.0f(%.0f%%) ", e.label.c_str(), e.bar, e.confidence * 100);
        std::printf("\n    minor events: %d, key: %s (%.0f%%), warnings: %zu\n",
                    (int) std::count_if(d.events.begin(), d.events.end(), [](const Event& e) { return e.importance == Importance::Minor; }),
                    d.reference.key.name.c_str(), d.reference.key.confidence * 100, d.diagnostics.warnings.size());
        for (auto& w : d.diagnostics.warnings) std::printf("    warning: %s\n", w.c_str());
    }
    return sc;
}

struct Expect { double minBoundR = 0.8, minLenient = 0.75; };

static std::string g_filter;
static void runTrack(const synth::Spec& spec, Expect ex) {
    if (!g_filter.empty() && spec.name.find(g_filter) == std::string::npos) return;
    auto tr = synth::generate(spec);
    for (int mode = 0; mode < 2; ++mode) {
        synth::OracleSeparator oracle(tr.stems);
        EngineOptions opt; opt.filePath = spec.name + ".wav";
        if (mode == 1) opt.stemSeparator = &oracle;
        const auto t0 = std::chrono::steady_clock::now();
        auto res = analyze(tr.mix, opt);
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("\n[%s | %s] status=%s  (%.1fs audio analyzed in %.1fs)\n", spec.name.c_str(), mode ? "oracle stems" : "mix only",
                    describe(res.status), tr.mix.numFrames() / tr.mix.sampleRate, secs);
        CHECK(res.status == AnalysisStatus::Ok, "%s analysis failed: %s", spec.name.c_str(), res.message.c_str());
        if (res.status != AnalysisStatus::Ok) continue;
        const auto sc = evaluate(tr, res.document, true);
        std::printf("    BPM %.2f (err %.3f%%, offset %+.1fms)  beatF %.3f  downbeats %.3f  bounds P %.2f R %.2f  labels strict %.2f lenient %.2f unsure %.2f  bars %d/%d\n",
                    res.document.reference.bpm, sc.bpmErrPct, sc.beatOffsetMs, sc.beatF, sc.downbeatAcc, sc.boundP, sc.boundR,
                    sc.labelStrict, sc.labelLenient, sc.unsure, res.document.reference.barCount, tr.truth.bars);
        const bool variable = spec.bpm2 > 0;
        if (!variable) CHECK(sc.bpmErrPct < 0.3, "%s BPM error %.3f%%", spec.name.c_str(), sc.bpmErrPct);
        CHECK(sc.beatF > (variable ? 0.85 : 0.97), "%s beat F %.3f", spec.name.c_str(), sc.beatF);
        CHECK(sc.downbeatAcc > (variable ? 0.8 : 0.97), "%s downbeat acc %.3f", spec.name.c_str(), sc.downbeatAcc);
        CHECK(sc.boundR >= ex.minBoundR, "%s boundary recall %.2f", spec.name.c_str(), sc.boundR);
        CHECK(sc.boundP >= 0.7, "%s boundary precision %.2f", spec.name.c_str(), sc.boundP);
        CHECK(sc.labelLenient + sc.unsure * 0.5 >= ex.minLenient, "%s label accuracy %.2f", spec.name.c_str(), sc.labelLenient);
        // Arrangement events at known positions (exact bar)
        auto has = [&](EventType t, double bar, double tol = 0.01) {
            return std::any_of(res.document.events.begin(), res.document.events.end(),
                               [&](const Event& e) { return e.type == t && std::fabs(e.bar - bar) <= tol; });
        };
        int partStart = 1;
        for (size_t i = 0; i < spec.parts.size(); ++i) {
            const auto& part = spec.parts[i];
            if (part.type == SectionType::Drop) {
                CHECK(has(EventType::Drop, partStart), "%s: Drop event at bar %d", spec.name.c_str(), partStart);
                if (i > 0 && (spec.parts[i - 1].f & synth::RISER)) {
                    const bool riser = std::any_of(res.document.events.begin(), res.document.events.end(), [&](const Event& e) {
                        return e.type == EventType::Riser && std::fabs(e.bar + e.lengthBars - partStart) <= 1.01; });
                    CHECK(riser, "%s: riser ending at drop bar %d", spec.name.c_str(), partStart);
                }
            }
            if (part.type == SectionType::Breakdown && i > 0)
                CHECK(has(EventType::Breakdown, partStart), "%s: Breakdown event at bar %d", spec.name.c_str(), partStart);
            if (i > 0 && (part.f & synth::KICK) && !(spec.parts[i - 1].f & synth::KICK))
                CHECK(has(EventType::KickIn, partStart) || has(EventType::FullDrumsIn, partStart),
                      "%s: Kick In at bar %d", spec.name.c_str(), partStart);
            if (i > 0 && !(part.f & synth::KICK) && (spec.parts[i - 1].f & synth::KICK))
                CHECK(has(EventType::KickOut, partStart), "%s: Kick Out at bar %d", spec.name.c_str(), partStart);
            partStart += part.bars;
        }
        // Serialization of a real result must roundtrip
        StructureDocument r; CHECK(deserialize(serialize(res.document), r), "roundtrip real doc");
    }
}

static void testErrors() {
    std::printf("\n[error handling]\n");
    AudioData empty;
    CHECK(analyze(empty, {}).status == AnalysisStatus::InvalidAudio, "empty -> InvalidAudio");
    AudioData shortA; shortA.sampleRate = 44100; shortA.channels = {std::vector<float>(44100 * 10, 0.1f)};
    CHECK(analyze(shortA, {}).status == AnalysisStatus::TooShort, "10s -> TooShort");
    AudioData silent; silent.sampleRate = 44100; silent.channels = {std::vector<float>(44100 * 60, 0.0f)};
    CHECK(analyze(silent, {}).status == AnalysisStatus::Silent, "silence -> Silent");
    AudioData nan = silent; nan.channels[0][1000] = NAN;
    CHECK(analyze(nan, {}).status == AnalysisStatus::InvalidAudio, "NaN -> InvalidAudio");
    AudioData noise; noise.sampleRate = 44100; noise.channels = {std::vector<float>(44100 * 60)};
    std::mt19937 rng(3); std::normal_distribution<float> g(0, 0.1f);
    float lp = 0; for (auto& s : noise.channels[0]) { lp += 0.05f * (g(rng) - lp); s = lp; }
    auto r = analyze(noise, {});
    std::printf("  pink-ish noise -> %s : %s\n", describe(r.status), r.message.c_str());
    CHECK(r.status == AnalysisStatus::NoTempo, "noise -> NoTempo (got %s)", describe(r.status));
    EngineOptions manual; manual.manualBpm = 120;
    r = analyze(noise, manual);
    CHECK(r.status == AnalysisStatus::Ok && std::fabs(r.document.reference.bpm - 120) < 0.01, "manual BPM fallback works");
    struct Cancel : ProgressSink { bool shouldCancel() override { return true; } } cancel;
    auto tr = synth::generate({"c", 128, 0, 0, false, {{SectionType::Main, 32, synth::KICK | synth::BASS}}});
    CHECK(analyze(tr.mix, {}, &cancel).status == AnalysisStatus::Cancelled, "cancel");
}

int main(int argc, char** argv) {
    const bool quick = argc > 1 && std::string(argv[1]) == "--unit";
    if (argc > 1 && !quick) g_filter = argv[1];
    using S = SectionType; using namespace synth;
    testTempoMapAndModel(); testSerialization(); testEdits(); testExportPlan();
    std::printf("unit tests: %d passed, %d failed\n", g_pass, g_fail);
    if (!quick) {
        if (g_filter.empty()) testErrors();
        const unsigned DR = KICK | HATS | CLAP;
        runTrack({"Trance 138", 138, 0, 0, false, {
            {S::Intro, 16, KICK | HATS}, {S::Groove, 16, DR | BASS | PERC}, {S::Breakdown, 16, PAD | LEAD},
            {S::Build, 8, PAD | ROLL | RISER}, {S::Drop, 32, DR | BASS | PAD | LEAD | PERC | IMPACT},
            {S::Breakdown, 16, PAD | VOCAL}, {S::Build, 8, PAD | LEAD | ROLL | RISER},
            {S::Drop, 32, DR | BASS | PAD | LEAD | PERC | IMPACT}, {S::Outro, 16, KICK | HATS}}}, {});
        runTrack({"Goa 145 (rolling bass, 0.37s lead-in)", 145, 0, 0.37, true, {
            {S::Intro, 32, KICK | HATS | PERC}, {S::Main, 32, DR | BASS | LEAD}, {S::Main, 32, DR | BASS | PAD | PERC},
            {S::Breakdown, 16, PAD | LEAD}, {S::Build, 8, ROLL | RISER | PAD}, {S::Drop, 32, DR | BASS | LEAD | IMPACT},
            {S::Outro, 16, KICK | HATS | BASS}}}, {});
        runTrack({"Progressive 124 (long intro, subtle)", 124, 0, 0, false, {
            {S::Intro, 32, KICK | HATS}, {S::Groove, 32, DR | BASS | PERC}, {S::Main, 32, DR | BASS | PERC | PAD},
            {S::Breakdown, 16, PAD | LEAD}, {S::Build, 8, PAD | LEAD | RISER},
            {S::Drop, 32, DR | BASS | PERC | PAD | LEAD}, {S::Outro, 32, KICK | HATS | PERC}}}, {0.7, 0.7});
        runTrack({"Tempo change 130->134", 130, 134, 0, false, {
            {S::Intro, 16, KICK | HATS}, {S::Main, 32, DR | BASS | PAD}, {S::Breakdown, 16, PAD | LEAD},
            {S::Drop, 32, DR | BASS | LEAD}, {S::Outro, 16, KICK | HATS}}}, {0.6, 0.6});
    }
    std::printf("\n==== TOTAL: %d passed, %d failed ====\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
