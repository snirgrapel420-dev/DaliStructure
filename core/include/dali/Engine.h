// Engine.h — public entry point of the Analysis Engine.
// Pure C++17, no UI / DAW / JUCE dependency. Safe to run on any background
// thread; never call from an audio callback.
#pragma once
#include "dali/Model.h"
#include <string>
#include <vector>

namespace dali {

struct AudioData {
    std::vector<std::vector<float>> channels;   // de-interleaved, any count >= 1
    double sampleRate = 0;
    size_t numFrames() const { return channels.empty() ? 0 : channels[0].size(); }
};

class ProgressSink {
public:
    virtual ~ProgressSink() = default;
    virtual void onProgress(float /*0..1*/, const char* /*stage*/) {}
    virtual bool shouldCancel() { return false; }
};

// ---------------------------------------------------------------- ML hooks
// Implemented in the plugin (ONNX Runtime). The engine works without them
// (DSP fallback) and records in diagnostics which path was used.
struct StemSet {
    double sampleRate = 0;                        // mono stems
    std::vector<float> drums, bass, vocals, other;
    bool valid() const { return sampleRate > 0 && !drums.empty() && drums.size() == bass.size()
                                && bass.size() == vocals.size() && vocals.size() == other.size(); }
};

class IStemSeparator {
public:
    virtual ~IStemSeparator() = default;
    virtual const char* name() const = 0;
    virtual bool separate(const AudioData& audio, StemSet& out, ProgressSink* progress) = 0;
};

struct BeatActivations {
    double fps = 0;
    std::vector<float> beat;       // 0..1 beat probability per frame
    std::vector<float> downbeat;   // 0..1 downbeat probability per frame
};

class IBeatActivationModel {
public:
    virtual ~IBeatActivationModel() = default;
    virtual const char* name() const = 0;
    virtual bool compute(const AudioData& audio, BeatActivations& out, ProgressSink* progress) = 0;
};

// ---------------------------------------------------------------- analysis
enum class AnalysisStatus { Ok, InvalidAudio, TooShort, Silent, NoTempo, Cancelled };
const char* describe(AnalysisStatus);

struct EngineOptions {
    std::string filePath;
    IStemSeparator* stemSeparator = nullptr;
    IBeatActivationModel* beatModel = nullptr;
    double minDurationSec = 30.0;
    // Manual override when automatic tempo detection fails (Section 22):
    double manualBpm = 0;                // > 0 forces a constant grid
    double manualFirstDownbeatSec = -1;  // >= 0 forces bar 1 position
};

struct AnalysisResult {
    AnalysisStatus status = AnalysisStatus::InvalidAudio;
    std::string message;                 // human readable, for the UI
    StructureDocument document;          // valid only when status == Ok
};

AnalysisResult analyze(const AudioData& audio, const EngineOptions& options, ProgressSink* progress = nullptr);

// Stable hash of decoded audio content (used as the cache key).
std::string contentHash(const AudioData& audio);

} // namespace dali
