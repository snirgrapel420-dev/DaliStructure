// dali_analyze — run the DaliStructure engine on real tracks (no DAW needed)
// and optionally score it against your own annotations.
//
//   dali_analyze <file.wav | folder> [--out <dir>] [--bpm <x>] [--downbeat <sec>]
//                [--truth <annotations.csv>] [--quiet]
//
// Reads WAV (PCM 16/24/32-bit int, 32/64-bit float, incl. WAVE_FORMAT_EXTENSIBLE).
// Other formats: convert to WAV first (the plugin itself reads MP3/AIFF/FLAC via JUCE).
//
// Annotation CSV (one row per fact; times in seconds from the start of the file):
//   file,kind,value,label
//   track1.wav,bpm,138,
//   track1.wav,section,0.0,INTRO
//   track1.wav,section,27.83,GROOVE
//   track1.wav,section,55.65,BREAKDOWN
// Section labels use the DaliStructure names (INTRO, GROOVE, BUILD, PRE-DROP,
// DROP, MAIN, BREAKDOWN, BREAK, TRANSITION, OUTRO).
#include "dali/Engine.h"
#include "dali/Serialization.h"
#include <algorithm>
#include <cctype>
#include <iterator>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace dali;

// ---------------------------------------------------------------- WAV reader
static bool readWav(const fs::path& path, AudioData& out, std::string& err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { err = "cannot open file"; return false; }
    std::vector<unsigned char> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    auto u16 = [&](size_t p) { return (unsigned) d[p] | ((unsigned) d[p + 1] << 8); };
    auto u32 = [&](size_t p) { return (unsigned) d[p] | ((unsigned) d[p + 1] << 8) | ((unsigned) d[p + 2] << 16) | ((unsigned) d[p + 3] << 24); };
    if (d.size() < 12 || std::memcmp(d.data(), "RIFF", 4) || std::memcmp(d.data() + 8, "WAVE", 4)) { err = "not a RIFF/WAVE file"; return false; }
    unsigned fmt = 0, ch = 0, sr = 0, bits = 0; size_t dataPos = 0, dataLen = 0;
    for (size_t p = 12; p + 8 <= d.size();) {
        const unsigned len = u32(p + 4);
        if (!std::memcmp(d.data() + p, "fmt ", 4) && p + 24 <= d.size()) {
            fmt = u16(p + 8); ch = u16(p + 10); sr = u32(p + 12); bits = u16(p + 22);
            if (fmt == 0xFFFE && len >= 40 && p + 34 <= d.size()) fmt = u16(p + 32);   // EXTENSIBLE sub-format
        } else if (!std::memcmp(d.data() + p, "data", 4)) {
            dataPos = p + 8; dataLen = std::min<size_t>(len, d.size() - dataPos);
        }
        p += 8 + len + (len & 1);
    }
    if (!dataPos || !ch || !sr) { err = "missing fmt/data chunk"; return false; }
    if (!((fmt == 1 && (bits == 16 || bits == 24 || bits == 32)) || (fmt == 3 && (bits == 32 || bits == 64)))) {
        err = "unsupported WAV encoding (format " + std::to_string(fmt) + ", " + std::to_string(bits) + " bit)"; return false;
    }
    const size_t bps = bits / 8, frames = dataLen / (bps * ch);
    out.sampleRate = sr;
    out.channels.assign(ch, std::vector<float>(frames));
    for (size_t i = 0; i < frames; ++i)
        for (unsigned c = 0; c < ch; ++c) {
            const unsigned char* s = d.data() + dataPos + (i * ch + c) * bps;
            float v = 0;
            if (fmt == 1 && bits == 16) v = (int16_t) (s[0] | (s[1] << 8)) / 32768.0f;
            else if (fmt == 1 && bits == 24) { int32_t x = (s[0] << 8) | (s[1] << 16) | (s[2] << 24); v = (float) (x / 2147483648.0); }
            else if (fmt == 1 && bits == 32) { int32_t x; std::memcpy(&x, s, 4); v = (float) (x / 2147483648.0); }
            else if (fmt == 3 && bits == 32) std::memcpy(&v, s, 4);
            else { double x; std::memcpy(&x, s, 8); v = (float) x; }
            out.channels[c][i] = v;
        }
    return true;
}

// ---------------------------------------------------------------- annotations
struct Truth { double bpm = 0; std::vector<std::pair<double, SectionType>> sections; };

static std::map<std::string, Truth> readTruth(const std::string& path) {
    std::map<std::string, Truth> m;
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::vector<std::string> c; std::stringstream ss(line); std::string cell;
        while (std::getline(ss, cell, ',')) c.push_back(cell);
        if (c.size() < 3 || c[0] == "file") continue;
        auto& t = m[c[0]];
        if (c[1] == "bpm") t.bpm = std::atof(c[2].c_str());
        else if (c[1] == "section" && c.size() >= 4) {
            SectionType st;
            if (parse(c[3], st)) t.sections.push_back({std::atof(c[2].c_str()), st});
            else std::fprintf(stderr, "warning: unknown label '%s' in %s\n", c[3].c_str(), path.c_str());
        }
    }
    for (auto& kv : m) std::sort(kv.second.sections.begin(), kv.second.sections.end(), [](auto& a, auto& b) { return a.first < b.first; });
    return m;
}

static int group(SectionType t) {
    switch (t) {
        case SectionType::Groove: case SectionType::Main: return 1;
        case SectionType::Break: case SectionType::Breakdown: return 2;
        case SectionType::Build: case SectionType::PreDrop: return 3;
        default: return 10 + (int) t;
    }
}

struct Totals { int files = 0; double bpmOk = 0, bpmN = 0, bHit = 0, bDet = 0, bRec = 0, bTrue = 0, lStrict = 0, lLen = 0, lUnsure = 0, lN = 0; };

static void evaluate(const StructureDocument& d, const Truth& t, Totals& tot) {
    ++tot.files;
    if (t.bpm > 0) {
        const double e = std::fabs(d.reference.bpm - t.bpm) / t.bpm;
        const bool ok = e < 0.01;
        tot.bpmN++; tot.bpmOk += ok;
        std::printf("    BPM: detected %.2f, truth %.2f %s\n", d.reference.bpm, t.bpm, ok ? "OK" :
                    (std::fabs(d.reference.bpm * 2 - t.bpm) / t.bpm < 0.01 || std::fabs(d.reference.bpm / 2 - t.bpm) / t.bpm < 0.01 ? "OCTAVE ERROR" : "WRONG"));
    }
    if (t.sections.empty()) return;
    const double barSec = 4 * 60.0 / std::max(1.0, d.reference.bpm);
    std::vector<double> det, tru;
    for (auto& s : d.sections) if (s.startBar > 1) det.push_back(s.startTime);
    for (size_t i = 1; i < t.sections.size(); ++i) tru.push_back(t.sections[i].first);
    int h = 0, r = 0;
    for (double x : det) h += std::any_of(tru.begin(), tru.end(), [&](double y) { return std::fabs(x - y) <= barSec * 1.01; });
    for (double y : tru) r += std::any_of(det.begin(), det.end(), [&](double x) { return std::fabs(x - y) <= barSec * 1.01; });
    tot.bHit += h; tot.bDet += det.size(); tot.bRec += r; tot.bTrue += tru.size();
    double strict = 0, len = 0, unsure = 0, n = 0;
    for (double tt = t.sections.front().first; tt < d.reference.durationSec; tt += 0.5) {
        SectionType truthType = t.sections.front().second;
        for (auto& s : t.sections) if (s.first <= tt) truthType = s.second;
        const Section* s = d.sectionAtBar(d.tempo.barAtTime(tt));
        if (!s) continue;
        ++n;
        if (s->type == SectionType::Transition || s->type == SectionType::Section) { ++unsure; continue; }
        strict += s->type == truthType; len += group(s->type) == group(truthType);
    }
    tot.lStrict += strict; tot.lLen += len; tot.lUnsure += unsure; tot.lN += n;
    std::printf("    boundaries (+-1 bar): precision %.2f recall %.2f | labels: strict %.2f lenient %.2f unsure %.2f\n",
                det.empty() ? 1.0 : (double) h / det.size(), tru.empty() ? 1.0 : (double) r / tru.size(),
                n ? strict / n : 0, n ? len / n : 0, n ? unsure / n : 0);
}

struct Printer : ProgressSink {
    bool quiet; int last = -1;
    explicit Printer(bool q) : quiet(q) {}
    void onProgress(float f, const char* stage) override {
        const int p = (int) (f * 100);
        if (!quiet && p / 10 != last / 10) { std::fprintf(stderr, "    %3d%% %s\n", p, stage); last = p; }
    }
};

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: dali_analyze <file.wav|folder> [--out dir] [--bpm x] [--downbeat sec] [--truth csv] [--quiet]\n");
        return 2;
    }
    std::string input = argv[1], outDir, truthPath;
    EngineOptions base; bool quiet = false;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--out" && i + 1 < argc) outDir = argv[++i];
        else if (a == "--bpm" && i + 1 < argc) base.manualBpm = std::atof(argv[++i]);
        else if (a == "--downbeat" && i + 1 < argc) base.manualFirstDownbeatSec = std::atof(argv[++i]);
        else if (a == "--truth" && i + 1 < argc) truthPath = argv[++i];
        else if (a == "--quiet") quiet = true;
    }
    std::vector<fs::path> files;
    if (fs::is_directory(input)) {
        for (auto& e : fs::directory_iterator(input)) {
            auto ext = e.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (ext == ".wav") files.push_back(e.path());
        }
        std::sort(files.begin(), files.end());
    } else files.push_back(input);
    if (!outDir.empty()) fs::create_directories(outDir);
    const auto truth = truthPath.empty() ? std::map<std::string, Truth>{} : readTruth(truthPath);

    Totals tot; int failures = 0;
    for (auto& p : files) {
        std::printf("\n== %s\n", p.filename().u8string().c_str());
        AudioData audio; std::string err;
        if (!readWav(p, audio, err)) { std::printf("    ERROR: %s\n", err.c_str()); ++failures; continue; }
        EngineOptions opt = base; opt.filePath = p.u8string();
        Printer pr(quiet);
        const auto res = analyze(audio, opt, &pr);
        if (res.status != AnalysisStatus::Ok) { std::printf("    ERROR: %s\n", res.message.c_str()); ++failures; continue; }
        const auto& d = res.document;
        std::printf("    BPM %.2f%s | Key %s (%.0f%%) | %d bars | beat conf %.0f%% | downbeat conf %.0f%%\n",
                    d.reference.bpm, d.tempo.stable ? "" : " (variable)", d.reference.key.name.empty() ? "-" : d.reference.key.name.c_str(),
                    d.reference.key.confidence * 100, d.reference.barCount, d.tempo.beatConfidence * 100, d.tempo.downbeatConfidence * 100);
        for (auto& w : d.diagnostics.warnings) std::printf("    warning: %s\n", w.c_str());
        for (auto& s : d.sections) {
            std::printf("    %-11s bars %3d-%-3d  %6.1fs  energy %3.0f  conf %3.0f%%  [", s.label.c_str(), s.startBar, s.endBar, s.startTime, s.energy, s.confidence * 100);
            for (size_t i = 0; i < s.elements.size(); ++i) std::printf("%s%s", i ? ", " : "", s.elements[i].c_str());
            std::printf("]\n");
        }
        int shown = 0;
        std::printf("    events:");
        for (auto& e : d.events) if (e.importance != Importance::Minor) { std::printf("%s %s@%.0f", shown++ % 6 ? "," : "\n     ", e.label.c_str(), e.bar); }
        std::printf("\n");
        if (!outDir.empty()) {
            fs::path name = p.stem(); name += ".dalistructure.json";
            std::ofstream o(fs::path(outDir) / name);
            o << serialize(d, true);
        }
        auto it = truth.find(p.filename().u8string());
        if (it != truth.end()) evaluate(d, it->second, tot);
    }
    if (tot.files) {
        std::printf("\n==== EVALUATION over %d annotated files ====\n", tot.files);
        if (tot.bpmN) std::printf("BPM correct (1%%): %.0f / %.0f\n", tot.bpmOk, tot.bpmN);
        if (tot.bTrue) std::printf("Boundaries: precision %.2f recall %.2f\n", tot.bDet ? tot.bHit / tot.bDet : 1.0, tot.bRec / tot.bTrue);
        if (tot.lN) std::printf("Labels (time-weighted): strict %.2f lenient %.2f unsure %.2f\n", tot.lStrict / tot.lN, tot.lLen / tot.lN, tot.lUnsure / tot.lN);
    }
    return failures ? 1 : 0;
}
