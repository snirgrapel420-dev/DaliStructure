// Pipeline.h — internal stage interfaces of the Analysis Engine.
//
//   Audio -> Preprocess -> FrameFeatures -> Rhythm (tempo/beats/downbeats)
//         -> BarFeatures + ElementActivity + Energy + Key
//         -> Novelty -> Segmentation -> Classification
//         -> Arrangement Events -> StructureDocument
//
// Each stage is a pure function of its inputs so it can be unit-tested alone.
#pragma once
#include "dali/Engine.h"
#include "dali/Model.h"
#include <array>
#include <string>
#include <vector>

namespace dali::pipeline {

constexpr double kAnalysisRate = 22050.0;
constexpr int kNumBands = 6;   // sub, low, low-mid, mid, high-mid, high
// Band edges in Hz: 20 | 60 | 150 | 500 | 2000 | 5000 | 11025
enum Band { Sub = 0, Low, LowMid, Mid, HighMid, High };

// ------------------------------------------------------------ frame features
struct FrameFeatures {
    // A-rate: fft 2048 / hop 512 (~43 fps) — rhythm detail
    double fpsA = 0;
    std::vector<float> flux;        // broadband log-mel spectral flux
    std::vector<float> fluxLow;     // < 150 Hz
    std::vector<float> fluxHigh;    // > 5 kHz
    std::vector<float> lowDb;       // energy < 150 Hz (dB)
    std::vector<float> highDb;      // energy > 5 kHz (dB)

    // B-rate: fft 2048 / hop 1024 (~21.5 fps) — timbre / texture
    double fpsB = 0;
    std::vector<std::array<float, kNumBands>> band;   // mix power per band
    std::vector<std::array<float, kNumBands>> bandH;  // harmonic (HPSS)
    std::vector<std::array<float, kNumBands>> bandP;  // percussive (HPSS)
    std::vector<float> centroid;    // Hz
    std::vector<float> flatness;    // 0..1
    std::vector<std::array<float, 13>> mfcc;

    // C-rate: fft 8192 / hop 2048 (~10.8 fps) — pitch
    double fpsC = 0;
    std::vector<std::array<float, 12>> chroma;
    std::vector<std::array<float, 12>> bassChroma;

    int framesA() const { return (int) flux.size(); }
    int framesB() const { return (int) band.size(); }
    int framesC() const { return (int) chroma.size(); }
};

FrameFeatures extractFrameFeatures(const std::vector<float>& mono22k, ProgressSink* p, float p0, float p1);

// Reduced feature set for a separated stem.
struct StemFeatures {
    double fpsA = 0, fpsB = 0;
    std::vector<float> fluxLow, fluxHigh, lowDb;
    std::vector<std::array<float, kNumBands>> band;
    bool valid() const { return !band.empty(); }
};
StemFeatures extractStemFeatures(const std::vector<float>& mono22k);

struct StemFeatureSet {
    StemFeatures drums, bass, vocals, other;
    bool valid = false;
};

// ------------------------------------------------------------ rhythm
struct RhythmResult {
    TempoMap tempo;
    bool ok = false;
    std::string failure;
    std::vector<std::string> warnings;
};

struct RhythmOptions {
    double manualBpm = 0;
    double manualFirstDownbeatSec = -1;
    const BeatActivations* neural = nullptr;
    double audioStartSec = 0;    // first non-silent time
    double audioEndSec = 0;      // last non-silent time (0 = duration)
    double durationSec = 0;
};

RhythmResult analyzeRhythm(const FrameFeatures& ff, const RhythmOptions& opt);

// exposed for tests
double estimateTempoBpm(const std::vector<float>& env, double fps, double* strength = nullptr);

// ------------------------------------------------------------ bars
enum Element { Kick = 0, Bass, Percussion, Lead, Pad, Vocal, NumElements };
const char* elementName(Element e);

struct BarFeatures {
    int numBars = 0;
    std::vector<double> rmsDb;
    std::vector<std::array<double, kNumBands>> bandDb;
    std::vector<double> centroid, flatness, onsetDensity, percRatio;
    std::vector<std::array<double, 13>> mfcc;
    std::vector<std::array<double, 12>> chroma;
    std::vector<bool> silent;
    std::array<std::vector<double>, NumElements> activity;   // 0..1 per bar
    std::array<bool, NumElements> elementSupported{};        // Vocal needs stems
    // beat-resolution helpers for events & energy
    std::vector<double> beatEnergy;       // 0..100
    std::vector<double> barEnergy;        // 0..100
    std::vector<double> beatHighDb, beatCentroid, beatFlatness, beatOnset;
    std::vector<double> beatLowRise;      // linear low-band attack at the beat / typical kick attack
    std::vector<std::array<double, 16>> bassPattern, percPattern;   // per bar, 16ths
    std::vector<std::array<double, 12>> barBassChroma;
};

BarFeatures computeBarFeatures(const FrameFeatures& ff, const StemFeatureSet& stems, const TempoMap& tm);
KeyInfo detectKey(const FrameFeatures& ff);

// ------------------------------------------------------------ structure
struct Segment { int start = 0; int end = 0; };   // bar indices, 0-based, end exclusive

std::vector<double> computeNovelty(const BarFeatures& bf);                        // size numBars+1
std::vector<Segment> segmentBars(const BarFeatures& bf, const std::vector<double>& novelty);

struct Classified {
    Segment seg;
    SectionType type = SectionType::Section;
    double confidence = 0;
    double energy = 0;
};
std::vector<Classified> classifySections(const BarFeatures& bf, std::vector<Segment> segs,
                                         const std::vector<double>& novelty,
                                         const std::vector<Event>* fxEvents);

// ------------------------------------------------------------ events
std::vector<Event> detectFxEvents(const BarFeatures& bf, const TempoMap& tm, const std::vector<Segment>& segs);
std::vector<Event> detectArrangementEvents(const BarFeatures& bf, const std::vector<Classified>& sections,
                                           const std::vector<double>& novelty, std::vector<Event> fx);

} // namespace dali::pipeline
