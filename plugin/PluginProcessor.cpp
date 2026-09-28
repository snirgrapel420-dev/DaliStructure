#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "dali/Serialization.h"

namespace
{
constexpr int kStateVersion = 1;
juce::String toJuce (const std::string& s)  { return juce::String::fromUTF8 (s.c_str(), (int) s.size()); }
std::string toStd (const juce::String& s)   { return s.toStdString(); }

juce::AudioProcessorValueTreeState::ParameterLayout createLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "volume", 1 }, "Reference Volume",
                                                             juce::NormalisableRange<float> (-60.0f, 6.0f, 0.1f), 0.0f));
    return layout;
}
} // namespace

// ============================================================================ Worker
// Background job: read file -> (restore | cache | analyze) -> publish.
class DaliStructureProcessor::Worker : public juce::Thread, public dali::ProgressSink
{
public:
    Worker (DaliStructureProcessor& o, Job j) : juce::Thread ("DaliStructure Analysis"), owner (o), job (std::move (j)) {}

    void onProgress (float f, const char* stageName) override
    {
        owner.progress.store (0.1f + 0.9f * juce::jlimit (0.0f, 1.0f, f));
        owner.setStage (stageName);
    }
    bool shouldCancel() override { return threadShouldExit(); }

    void run() override
    {
        owner.status.store (Status::Loading);
        owner.progress.store (0.0f);
        owner.setStage ("Loading audio");

        std::unique_ptr<juce::AudioFormatReader> reader (owner.formatManager.createReaderFor (job.file));
        if (reader == nullptr)
            return fail ("This file could not be opened. Supported formats: WAV, AIFF, MP3, FLAC.");
        if (reader->lengthInSamples <= 0 || reader->sampleRate <= 0)
            return fail ("The audio file is empty or damaged.");
        const double seconds = (double) reader->lengthInSamples / reader->sampleRate;
        if (seconds > 30.0 * 60.0)
            return fail ("The file is longer than 30 minutes. Please use a single track.");

        auto audio = std::make_shared<dali::AudioData>();
        audio->sampleRate = reader->sampleRate;
        const int numCh = juce::jlimit (1, 2, (int) reader->numChannels);
        const auto length = (size_t) reader->lengthInSamples;
        audio->channels.assign ((size_t) numCh, std::vector<float> (length));

        const int chunk = 1 << 16;
        juce::AudioBuffer<float> tmp (numCh, chunk);
        for (juce::int64 pos = 0; pos < reader->lengthInSamples; pos += chunk)
        {
            if (threadShouldExit()) return;
            const int n = (int) juce::jmin ((juce::int64) chunk, reader->lengthInSamples - pos);
            tmp.clear();
            reader->read (&tmp, 0, n, pos, true, numCh > 1);
            for (int c = 0; c < numCh; ++c)
                std::copy (tmp.getReadPointer (c), tmp.getReadPointer (c) + n, audio->channels[(size_t) c].begin() + (std::ptrdiff_t) pos);
            owner.progress.store (0.1f * (float) pos / (float) reader->lengthInSamples);
        }
        reader.reset();
        owner.setAudioForPlayback (audio);
        if (threadShouldExit()) return;

        owner.setStage ("Checking cache");
        const std::string hash = dali::contentHash (*audio);

        // 1) Restored project state (keeps the user's edits) if the audio is unchanged.
        if (job.restoredDocument != nullptr)
        {
            if (job.restoredDocument->reference.contentHash == hash)
            {
                job.restoredDocument->reference.filePath = toStd (job.file.getFullPathName());
                return owner.publishResult (job.restoredDocument, Status::Ready, {}, false, true);
            }
        }

        // 2) Global analysis cache (same audio analyzed before, same engine version).
        const auto cacheFile = getCacheDirectory().getChildFile (toJuce (hash) + "_" + dali::kEngineVersion + ".json");
        if (job.useCache && job.manualBpm <= 0.0 && cacheFile.existsAsFile())
        {
            auto doc = std::make_shared<dali::StructureDocument>();
            if (dali::deserialize (toStd (cacheFile.loadFileAsString()), *doc) && doc->reference.contentHash == hash)
            {
                doc->reference.filePath = toStd (job.file.getFullPathName());
                doc->reference.fileName = toStd (job.file.getFileName());
                return owner.publishResult (doc, Status::Ready, {}, false, true);
            }
        }

        // 3) Full analysis.
        owner.status.store (Status::Analyzing);
        dali::EngineOptions opt;
        opt.filePath = toStd (job.file.getFullPathName());
        opt.manualBpm = job.manualBpm;
        auto result = dali::analyze (*audio, opt, this);
        if (threadShouldExit() || result.status == dali::AnalysisStatus::Cancelled)
            return;
        if (result.status != dali::AnalysisStatus::Ok)
            return owner.publishResult (nullptr, Status::Error, toJuce (result.message),
                                        result.status == dali::AnalysisStatus::NoTempo, false);

        auto doc = std::make_shared<dali::StructureDocument> (std::move (result.document));
        if (job.restoredDocument != nullptr)
            doc->diagnostics.warnings.push_back ("The reference audio changed since this project was saved, so it was analyzed again (previous manual edits were not applied).");
        if (job.manualBpm <= 0.0 && getCacheDirectory().createDirectory())
            cacheFile.replaceWithText (toJuce (dali::serialize (*doc)));
        owner.publishResult (doc, Status::Ready, {}, false, false);
    }

private:
    void fail (const juce::String& message) { owner.publishResult (nullptr, Status::Error, message, false, false); }

    DaliStructureProcessor& owner;
    Job job;
};

// ============================================================================ Processor
DaliStructureProcessor::DaliStructureProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      parameters (*this, nullptr, "DaliStructureParams", createLayout())
{
    formatManager.registerBasicFormats();
    volumeParam = parameters.getRawParameterValue ("volume");
}

DaliStructureProcessor::~DaliStructureProcessor()
{
    cancelPendingUpdate();
    stopWorker();
}

juce::File DaliStructureProcessor::getCacheDirectory()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
        .getChildFile ("Dali Audio").getChildFile ("DaliStructure").getChildFile ("Cache");
}

bool DaliStructureProcessor::isSupportedFile (const juce::File& f)
{
    return f.hasFileExtension ("wav;wave;aif;aiff;mp3;flac");
}

void DaliStructureProcessor::prepareToPlay (double sampleRate, int)
{
    hostRate = sampleRate > 0 ? sampleRate : 44100.0;
    gainSmoothed.reset (hostRate, 0.05);
    gainSmoothed.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (volumeParam->load(), -60.0f));
}

bool DaliStructureProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    return (out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo())
        && layouts.getMainInputChannelSet().isDisabled();
}

void DaliStructureProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    midi.clear();
    buffer.clear();
    gainSmoothed.setTargetValue (juce::Decibels::decibelsToGain (volumeParam->load(), -60.0f));
    const int numSamples = buffer.getNumSamples();
    if (! playing.load (std::memory_order_relaxed))
    {
        gainSmoothed.skip (numSamples);
        return;
    }

    const juce::SpinLock::ScopedTryLockType lock (audioLock);
    if (! lock.isLocked() || playAudio == nullptr)
        return;

    const auto& a = *playAudio;
    const size_t len = a.numFrames();
    if (len < 2 || a.channels.empty())
        return;

    double pos = readPos.load (std::memory_order_relaxed);
    const double step = a.sampleRate / hostRate;
    const int outChannels = buffer.getNumChannels();
    const int srcChannels = (int) a.channels.size();
    for (int i = 0; i < numSamples; ++i)
    {
        const auto idx = (size_t) pos;
        if (idx + 1 >= len)
        {
            playing.store (false);
            break;
        }
        const float frac = (float) (pos - (double) idx);
        const float g = gainSmoothed.getNextValue();
        for (int ch = 0; ch < outChannels; ++ch)
        {
            const auto& src = a.channels[(size_t) juce::jmin (ch, srcChannels - 1)];
            buffer.setSample (ch, i, (src[idx] + frac * (src[idx + 1] - src[idx])) * g);
        }
        pos += step;
    }
    readPos.store (pos, std::memory_order_relaxed);
}

// ---------------------------------------------------------------- transport
void DaliStructureProcessor::setPlaying (bool shouldPlay)
{
    if (shouldPlay && getPositionSeconds() >= getDurationSeconds() - 0.05)
        seekSeconds (0.0);
    playing.store (shouldPlay && hasAudio());
}

void DaliStructureProcessor::seekSeconds (double seconds)
{
    readPos.store (juce::jlimit (0.0, getDurationSeconds(), seconds) * sourceRate.load());
}

double DaliStructureProcessor::getPositionSeconds() const noexcept
{
    return readPos.load() / juce::jmax (1.0, sourceRate.load());
}

void DaliStructureProcessor::setAudioForPlayback (std::shared_ptr<const dali::AudioData> audio)
{
    playing.store (false);
    {
        const juce::SpinLock::ScopedLockType lock (audioLock);
        std::swap (playAudio, audio);        // the previous buffer is released outside the lock
    }
    if (playAudio != nullptr)
    {
        sourceRate.store (playAudio->sampleRate);
        durationSec.store ((double) playAudio->numFrames() / playAudio->sampleRate);
    }
    else
    {
        durationSec.store (0.0);
    }
    readPos.store (0.0);
}

// ---------------------------------------------------------------- jobs
void DaliStructureProcessor::loadReference (const juce::File& file, double manualBpm)
{
    const bool sameFile = file == getReferenceFile();
    {
        const std::lock_guard<std::mutex> lock (stateMutex);
        referenceFile = file;
        if (! sameFile) document.reset();
    }
    startJob ({ file, manualBpm, manualBpm <= 0.0, nullptr });
}

void DaliStructureProcessor::reanalyze (double manualBpm)
{
    const auto f = getReferenceFile();
    if (f.existsAsFile())
        startJob ({ f, manualBpm, false, nullptr });
}

void DaliStructureProcessor::startJob (Job job)
{
    stopWorker();
    cancelPendingUpdate();
    {
        const std::lock_guard<std::mutex> lock (stateMutex);
        errorMessage.clear();
    }
    manualBpmSuggested.store (false);
    status.store (Status::Loading);
    progress.store (0.0f);
    worker = std::make_unique<Worker> (*this, std::move (job));
    worker->startThread();
    sendChangeMessage();
}

void DaliStructureProcessor::stopWorker()
{
    if (worker != nullptr)
    {
        worker->signalThreadShouldExit();
        worker->stopThread (15000);
        worker.reset();
    }
}

void DaliStructureProcessor::cancelAnalysis()
{
    stopWorker();
    cancelPendingUpdate();
    const std::lock_guard<std::mutex> lock (stateMutex);
    if (document != nullptr) status.store (Status::Ready);
    else
    {
        errorMessage = "Analysis cancelled.";
        status.store (Status::Error);
    }
    sendChangeMessage();
}

void DaliStructureProcessor::publishResult (std::shared_ptr<dali::StructureDocument> doc, Status st,
                                            const juce::String& error, bool manualBpmHint, bool cached)
{
    {
        const std::lock_guard<std::mutex> lock (stateMutex);
        pendingDocument = std::move (doc);
        pendingStatus = st;
        pendingError = error;
        pendingManualHint.store (manualBpmHint);
        pendingCached.store (cached);
    }
    triggerAsyncUpdate();
}

void DaliStructureProcessor::handleAsyncUpdate()
{
    {
        const std::lock_guard<std::mutex> lock (stateMutex);
        if (pendingStatus == Status::Ready && pendingDocument != nullptr)
            document = pendingDocument;
        pendingDocument.reset();
        errorMessage = pendingError;
        manualBpmSuggested.store (pendingManualHint.load());
        fromCache.store (pendingCached.load());
        status.store (pendingStatus);
        progress.store (pendingStatus == Status::Ready ? 1.0f : progress.load());
    }
    sendChangeMessage();
}

void DaliStructureProcessor::setStage (const juce::String& s)
{
    const std::lock_guard<std::mutex> lock (stateMutex);
    stage = s;
}

juce::String DaliStructureProcessor::getStage() const
{
    const std::lock_guard<std::mutex> lock (stateMutex);
    return stage;
}

juce::String DaliStructureProcessor::getErrorMessage() const
{
    const std::lock_guard<std::mutex> lock (stateMutex);
    return errorMessage;
}

std::shared_ptr<const dali::StructureDocument> DaliStructureProcessor::getDocument() const
{
    const std::lock_guard<std::mutex> lock (stateMutex);
    return document;
}

juce::File DaliStructureProcessor::getReferenceFile() const
{
    const std::lock_guard<std::mutex> lock (stateMutex);
    return referenceFile;
}

// ---------------------------------------------------------------- state
void DaliStructureProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    juce::XmlElement xml ("DaliStructure");
    xml.setAttribute ("stateVersion", kStateVersion);
    xml.setAttribute ("file", getReferenceFile().getFullPathName());
    if (auto params = parameters.copyState().createXml())
        xml.addChildElement (params.release());
    if (auto doc = getDocument())
    {
        auto* d = xml.createNewChildElement ("Document");
        d->addTextElement (toJuce (dali::serialize (*doc)));
    }
    copyXmlToBinary (xml, destData);
}

void DaliStructureProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    const auto xml = getXmlFromBinary (data, sizeInBytes);
    if (xml == nullptr || ! xml->hasTagName ("DaliStructure"))
        return;
    if (auto* params = xml->getChildByName (parameters.state.getType()))
        parameters.replaceState (juce::ValueTree::fromXml (*params));

    std::shared_ptr<dali::StructureDocument> restored;
    if (auto* d = xml->getChildByName ("Document"))
    {
        auto doc = std::make_shared<dali::StructureDocument>();
        std::string err;
        if (dali::deserialize (toStd (d->getAllSubText()), *doc, &err))
            restored = doc;
    }
    const juce::File file (xml->getStringAttribute ("file"));
    {
        const std::lock_guard<std::mutex> lock (stateMutex);
        referenceFile = file;
        document = restored;
    }
    if (file.existsAsFile())
    {
        startJob ({ file, 0.0, true, restored });
    }
    else if (file != juce::File())
    {
        const std::lock_guard<std::mutex> lock (stateMutex);
        errorMessage = "Reference file not found: " + file.getFullPathName()
                     + (restored != nullptr ? "\nThe saved structure is shown; load the file again to enable playback." : "");
        status.store (restored != nullptr ? Status::Ready : Status::Error);
    }
    sendChangeMessage();
}

juce::AudioProcessorEditor* DaliStructureProcessor::createEditor()
{
    return new DaliStructureEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new DaliStructureProcessor();
}
