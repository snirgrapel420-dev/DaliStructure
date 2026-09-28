#include "dali/Engine.h"
#include "analysis/Pipeline.h"
#include "dsp/Dsp.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace dali {
using namespace dali::pipeline;

const char* describe(AnalysisStatus s) {
    switch (s) {
        case AnalysisStatus::Ok: return "Analysis complete.";
        case AnalysisStatus::InvalidAudio: return "The audio could not be read (unsupported or corrupted).";
        case AnalysisStatus::TooShort: return "The audio is too short for arrangement analysis.";
        case AnalysisStatus::Silent: return "The audio is silent.";
        case AnalysisStatus::NoTempo: return "Tempo could not be detected. You can enter the BPM manually.";
        case AnalysisStatus::Cancelled: return "Analysis cancelled.";
    }
    return "";
}

std::string contentHash(const AudioData& a) {
    uint64_t h = 1469598103934665603ULL;
    auto mix = [&](uint64_t v) { for (int i = 0; i < 8; ++i) { h ^= (v >> (i * 8)) & 0xFF; h *= 1099511628211ULL; } };
    mix((uint64_t) std::llround(a.sampleRate));
    mix((uint64_t) a.numFrames());
    mix((uint64_t) a.channels.size());
    for (auto& ch : a.channels)
        for (float s : ch) mix((uint64_t) (uint16_t) (int16_t) std::lround(std::clamp(s, -1.0f, 1.0f) * 32767.0f));
    char buf[20]; std::snprintf(buf, sizeof buf, "%016llx", (unsigned long long) h);
    return buf;
}

namespace {

struct ScopedProgress : ProgressSink {
    ProgressSink* outer; float a, b;
    ScopedProgress(ProgressSink* o, float a_, float b_) : outer(o), a(a_), b(b_) {}
    void onProgress(float f, const char* s) override { if (outer) outer->onProgress(a + (b - a) * std::clamp(f, 0.0f, 1.0f), s); }
    bool shouldCancel() override { return outer && outer->shouldCancel(); }
};

std::vector<float> to22k(const std::vector<float>& x, double sr) { return dsp::resample(x, sr, kAnalysisRate); }

} // namespace

AnalysisResult analyze(const AudioData& audio, const EngineOptions& opt, ProgressSink* progress) {
    AnalysisResult res;
    auto fail = [&](AnalysisStatus s, std::string msg = {}) {
        res.status = s; res.message = msg.empty() ? describe(s) : msg; return res;
    };
    auto report = [&](float f, const char* s) { if (progress) progress->onProgress(f, s); };
    auto cancelled = [&] { return progress && progress->shouldCancel(); };

    // ---------------- validation
    if (audio.channels.empty() || audio.sampleRate < 8000 || audio.numFrames() == 0) return fail(AnalysisStatus::InvalidAudio);
    for (auto& ch : audio.channels) {
        if (ch.size() != audio.numFrames()) return fail(AnalysisStatus::InvalidAudio);
        for (float s : ch) if (!std::isfinite(s)) return fail(AnalysisStatus::InvalidAudio, "The audio contains invalid samples (corrupted file).");
    }
    const double duration = (double) audio.numFrames() / audio.sampleRate;
    if (duration < opt.minDurationSec) {
        char buf[160]; std::snprintf(buf, sizeof buf, "The audio is %.1f s long; at least %.0f s are needed.", duration, opt.minDurationSec);
        return fail(AnalysisStatus::TooShort, buf);
    }

    // ---------------- preprocessing
    report(0.01f, "Preparing audio");
    std::vector<float> mono(audio.numFrames(), 0.0f);
    for (auto& ch : audio.channels) for (size_t i = 0; i < mono.size(); ++i) mono[i] += ch[i] / (float) audio.channels.size();
    const auto x = to22k(mono, audio.sampleRate);
    float peak = 0; for (float s : x) peak = std::max(peak, std::fabs(s));
    if (peak < 1e-4f) return fail(AnalysisStatus::Silent);
    double audioStart = 0;
    {
        const size_t blk = 1024; const double thr = peak * 0.01;
        for (size_t i = 0; i + blk <= x.size(); i += blk) {
            double e = 0; for (size_t j = i; j < i + blk; ++j) e += (double) x[j] * x[j];
            if (std::sqrt(e / blk) > thr) { audioStart = (double) i / kAnalysisRate; break; }
        }
    }
    double audioEnd = (double) x.size() / kAnalysisRate;
    {
        const size_t blk = 1024; const double thr = peak * 0.01;
        for (size_t i = x.size() >= blk ? x.size() - blk : 0; i > 0; i = i >= blk ? i - blk : 0) {
            double e = 0; for (size_t j = i; j < i + blk && j < x.size(); ++j) e += (double) x[j] * x[j];
            if (std::sqrt(e / blk) > thr) { audioEnd = (double) (i + blk) / kAnalysisRate; break; }
            if (i < blk) break;
        }
    }
    if (cancelled()) return fail(AnalysisStatus::Cancelled);

    StructureDocument doc;
    // ---------------- optional ML stages
    StemFeatureSet stems;
    if (opt.stemSeparator) {
        report(0.03f, "Separating stems");
        ScopedProgress sp(progress, 0.03f, 0.50f);
        StemSet s;
        if (opt.stemSeparator->separate(audio, s, &sp) && s.valid()) {
            if (cancelled()) return fail(AnalysisStatus::Cancelled);
            stems.drums = extractStemFeatures(to22k(s.drums, s.sampleRate));
            stems.bass = extractStemFeatures(to22k(s.bass, s.sampleRate));
            stems.vocals = extractStemFeatures(to22k(s.vocals, s.sampleRate));
            stems.other = extractStemFeatures(to22k(s.other, s.sampleRate));
            stems.valid = stems.drums.valid() && stems.bass.valid() && stems.vocals.valid() && stems.other.valid();
        }
        if (cancelled()) return fail(AnalysisStatus::Cancelled);
        if (!stems.valid) doc.diagnostics.warnings.push_back("Stem separation unavailable; element detection used the full mix (vocal events disabled).");
    }
    BeatActivations neural;
    bool haveNeural = false;
    if (opt.beatModel) {
        report(0.50f, "Tracking beats (neural)");
        ScopedProgress sp(progress, 0.50f, 0.58f);
        haveNeural = opt.beatModel->compute(audio, neural, &sp) && neural.fps > 0 && !neural.beat.empty();
        if (!haveNeural) doc.diagnostics.warnings.push_back("Neural beat tracker unavailable; DSP beat tracking was used.");
        if (cancelled()) return fail(AnalysisStatus::Cancelled);
    }

    // ---------------- features + rhythm
    const float fStart = opt.stemSeparator ? 0.58f : 0.05f;
    ScopedProgress fp(progress, fStart, fStart + (0.90f - fStart) * 0.7f);
    const FrameFeatures ff = extractFrameFeatures(x, &fp, 0.0f, 1.0f);
    if (cancelled()) return fail(AnalysisStatus::Cancelled);

    report(0.80f, "Detecting tempo and bars");
    RhythmOptions ro;
    ro.manualBpm = opt.manualBpm; ro.manualFirstDownbeatSec = opt.manualFirstDownbeatSec;
    ro.neural = haveNeural ? &neural : nullptr; ro.audioStartSec = audioStart; ro.audioEndSec = audioEnd; ro.durationSec = duration;
    RhythmResult rr = analyzeRhythm(ff, ro);
    if (!rr.ok) return fail(AnalysisStatus::NoTempo, rr.failure + " You can enter the BPM manually.");
    for (auto& w : rr.warnings) doc.diagnostics.warnings.push_back(w);
    doc.tempo = rr.tempo;
    if (doc.tempo.barCount() < 8) return fail(AnalysisStatus::TooShort, "Fewer than 8 bars were found.");
    if (cancelled()) return fail(AnalysisStatus::Cancelled);

    // ---------------- structure
    report(0.85f, "Analyzing arrangement");
    const BarFeatures bf = computeBarFeatures(ff, stems, doc.tempo);
    const auto nov = computeNovelty(bf);
    const auto segs = segmentBars(bf, nov);
    const auto fx = detectFxEvents(bf, doc.tempo, segs);
    report(0.92f, "Classifying sections");
    const auto classified = classifySections(bf, segs, nov, &fx);
    std::vector<Segment> finalSegs; for (auto& c : classified) finalSegs.push_back(c.seg);
    const auto fx2 = detectFxEvents(bf, doc.tempo, finalSegs);
    auto events = detectArrangementEvents(bf, classified, nov, fx2);
    if (cancelled()) return fail(AnalysisStatus::Cancelled);

    // ---------------- document
    report(0.97f, "Building structure map");
    doc.reference.filePath = opt.filePath;
    {
        const auto p = opt.filePath.find_last_of("/\\");
        doc.reference.fileName = p == std::string::npos ? opt.filePath : opt.filePath.substr(p + 1);
    }
    doc.reference.contentHash = contentHash(audio);
    doc.reference.sampleRate = audio.sampleRate;
    doc.reference.channels = (int) audio.channels.size();
    doc.reference.durationSec = duration;
    doc.reference.bpm = std::round(doc.tempo.nominalBpm * 100.0) / 100.0;
    doc.reference.key = detectKey(ff);
    doc.reference.barCount = doc.tempo.barCount();
    doc.reference.firstDownbeatSec = doc.tempo.beatTimes.front();
    doc.diagnostics.usedStems = stems.valid;
    doc.diagnostics.usedNeuralBeats = haveNeural;

    int n = 1;
    for (auto& c : classified) {
        Section s;
        s.id = "sec-" + std::to_string(n++);
        s.type = c.type; s.label = toString(c.type);
        s.startBar = c.seg.start + 1; s.endBar = c.seg.end;
        s.energy = std::round(c.energy); s.confidence = c.confidence;
        auto avg = [&](Element e) { double m = 0; for (int b = c.seg.start; b < c.seg.end; ++b) m += bf.activity[(size_t) e][(size_t) b]; return m / std::max(1, c.seg.end - c.seg.start); };
        const bool kick = avg(Kick) >= 0.5, perc = avg(Percussion) >= 0.5;
        if (kick) s.elements.push_back("Kick");
        if (avg(Bass) >= 0.5) s.elements.push_back("Bass");
        if (kick && perc) s.elements.push_back("Drums");
        if (perc) s.elements.push_back("Percussion");
        if (avg(Lead) >= 0.5) s.elements.push_back("Lead / Melody");
        if (avg(Pad) >= 0.5) s.elements.push_back("Chords / Pad");
        if (bf.elementSupported[Vocal] && avg(Vocal) >= 0.5) s.elements.push_back("Vocal");
        for (auto& e : events)
            if (categoryOf(e.type) == EventCategory::FX && e.bar >= s.startBar && e.bar < s.endBar + 1) { s.elements.push_back("FX"); break; }
        doc.sections.push_back(s);
    }
    n = 1;
    for (auto& e : events) { e.id = "ev-" + std::to_string(n++); doc.events.push_back(e); }
    const int bpb = doc.tempo.beatsPerBar;
    for (size_t k = 0; k < bf.beatEnergy.size(); ++k)
        doc.energy.push_back({1.0 + (double) k / bpb, std::round(bf.beatEnergy[k] * 10.0) / 10.0});
    for (int e = 0; e < NumElements; ++e) {
        if (!bf.elementSupported[(size_t) e]) continue;
        ElementTrack t; t.name = elementName((Element) e);
        for (double v : bf.activity[(size_t) e]) t.activity.push_back((float) v);
        doc.elements.push_back(std::move(t));
    }
    doc.autoSections = doc.sections;
    doc.autoEvents = doc.events;
    doc.recomputeTimes();
    doc.autoSections = doc.sections;
    doc.autoEvents = doc.events;

    report(1.0f, "Done");
    res.status = AnalysisStatus::Ok;
    res.message = describe(AnalysisStatus::Ok);
    res.document = std::move(doc);
    return res;
}

} // namespace dali
