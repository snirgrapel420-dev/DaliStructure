// DaliStructure — Reference Arrangement Mapper by Dali Audio
// PluginProcessor: owns the reference audio, the background analysis job,
// the analysis cache, reference playback and plugin state.
// No analysis ever runs on the audio thread.
#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include "dali/Engine.h"
#include "dali/Model.h"
#include <atomic>
#include <memory>
#include <mutex>

class DaliStructureProcessor : public juce::AudioProcessor,
                               public juce::ChangeBroadcaster,
                               private juce::AsyncUpdater
{
public:
    enum class Status { Empty, Loading, Analyzing, Ready, Error };

    DaliStructureProcessor();
    ~DaliStructureProcessor() override;

    // ---------------------------------------------------------------- AudioProcessor
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return "DaliStructure"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    // ---------------------------------------------------------------- message-thread API
    static bool isSupportedFile (const juce::File&);
    void loadReference (const juce::File& file, double manualBpm = 0.0);
    void reanalyze (double manualBpm = 0.0);          // ignores the cache
    void cancelAnalysis();

    Status getStatus() const noexcept           { return status.load(); }
    float getProgress() const noexcept          { return progress.load(); }
    juce::String getStage() const;
    juce::String getErrorMessage() const;
    bool errorAllowsManualBpm() const noexcept  { return manualBpmSuggested.load(); }
    std::shared_ptr<const dali::StructureDocument> getDocument() const;
    juce::File getReferenceFile() const;
    bool loadedFromCache() const noexcept       { return fromCache.load(); }

    // ---------------------------------------------------------------- transport (reference playback)
    void setPlaying (bool shouldPlay);
    bool isPlaying() const noexcept             { return playing.load(); }
    void seekSeconds (double seconds);
    double getPositionSeconds() const noexcept;
    double getDurationSeconds() const noexcept  { return durationSec.load(); }
    bool hasAudio() const noexcept              { return durationSec.load() > 0.0; }

    juce::AudioProcessorValueTreeState parameters;

    static juce::File getCacheDirectory();

private:
    struct Job
    {
        juce::File file;
        double manualBpm = 0.0;
        bool useCache = true;
        std::shared_ptr<dali::StructureDocument> restoredDocument;   // from saved state
    };
    class Worker;
    friend class Worker;

    void startJob (Job job);
    void stopWorker();
    void handleAsyncUpdate() override;

    // called from the worker thread
    void setAudioForPlayback (std::shared_ptr<const dali::AudioData> audio);
    void publishResult (std::shared_ptr<dali::StructureDocument> doc, Status st, const juce::String& error, bool manualBpmHint, bool cached);
    void setStage (const juce::String&);

    std::unique_ptr<Worker> worker;
    juce::AudioFormatManager formatManager;

    // state shared with the worker / UI
    mutable std::mutex stateMutex;
    std::shared_ptr<const dali::StructureDocument> document;
    std::shared_ptr<dali::StructureDocument> pendingDocument;
    juce::File referenceFile;
    juce::String stage, errorMessage;
    Status pendingStatus = Status::Empty;
    juce::String pendingError;
    std::atomic<Status> status { Status::Empty };
    std::atomic<float> progress { 0.0f };
    std::atomic<bool> manualBpmSuggested { false }, fromCache { false }, pendingManualHint { false }, pendingCached { false };

    // playback (audio thread reads under a try-lock; never blocks)
    juce::SpinLock audioLock;
    std::shared_ptr<const dali::AudioData> playAudio;
    std::atomic<bool> playing { false };
    std::atomic<double> readPos { 0.0 };         // in source samples
    std::atomic<double> sourceRate { 44100.0 };
    std::atomic<double> durationSec { 0.0 };
    double hostRate = 44100.0;
    std::atomic<float>* volumeParam = nullptr;
    juce::SmoothedValue<float> gainSmoothed;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DaliStructureProcessor)
};
