// Synthetic EDM generator with exact ground truth (beats, bars, sections, stems).
// Used to verify the pipeline end-to-end where the correct answer is known.
// It does NOT replace testing on real music (see tools/evaluate).
#pragma once
#include "dali/Engine.h"
#include "dali/Model.h"
#include <algorithm>
#include <cmath>
#include <random>
#include <string>
#include <vector>

namespace synth {

constexpr double kPi = 3.14159265358979323846;

enum Flags : unsigned {
    KICK = 1, HATS = 2, CLAP = 4, BASS = 8, PAD = 16, LEAD = 32, VOCAL = 64,
    RISER = 128, ROLL = 256, IMPACT = 512, PERC = 1024
};

struct Part { dali::SectionType type; int bars; unsigned f; };

struct Spec {
    std::string name;
    double bpm = 138;
    double bpm2 = 0;          // if > 0: tempo switches to bpm2 at the middle bar
    double leadIn = 0;        // seconds of silence before bar 1
    bool rollingBass = false; // psy / goa 16th bass
    std::vector<Part> parts;
};

struct Truth {
    std::vector<double> beats;                     // seconds
    std::vector<std::pair<dali::SectionType, std::pair<int, int>>> sections;   // type, [start,end] 1-based bars
    int bars = 0;
};

struct Track {
    dali::AudioData mix;
    dali::StemSet stems;
    Truth truth;
};

inline Track generate(const Spec& spec, double sr = 44100.0, unsigned seed = 7) {
    Track tr;
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> uni(-1.f, 1.f);
    int totalBars = 0; for (auto& p : spec.parts) totalBars += p.bars;
    const int midBar = totalBars / 2;
    // beat times
    std::vector<double> beats;
    double t = spec.leadIn;
    for (int b = 0; b < totalBars * 4 + 1; ++b) {
        beats.push_back(t);
        const double bpm = (spec.bpm2 > 0 && b >= midBar * 4) ? spec.bpm2 : spec.bpm;
        t += 60.0 / bpm;
    }
    const double dur = beats.back() + 1.5;
    const size_t n = (size_t) (dur * sr);
    std::vector<float> drums(n, 0), bass(n, 0), vocals(n, 0), other(n, 0);
    auto add = [&](std::vector<float>& buf, double at, size_t len, auto&& gen) {
        const size_t s0 = (size_t) std::max(0.0, at * sr);
        const size_t fade = std::min(len / 4, (size_t) (0.006 * sr));   // no clicks at note ends
        for (size_t i = 0; i < len && s0 + i < n; ++i) {
            const float g = (fade > 0 && i + fade > len) ? (float) (len - i) / (float) fade : 1.0f;
            buf[s0 + i] += g * gen((double) i / sr);
        }
    };
    const int prog[4] = {45, 41, 48, 43};          // Am F C G (roots, MIDI)
    const bool minorChord[4] = {true, false, false, false};
    auto hz = [](double midi) { return 440.0 * std::pow(2.0, (midi - 69) / 12.0); };

    int bar0 = 0;
    for (auto& part : spec.parts) {
        tr.truth.sections.push_back({part.type, {bar0 + 1, bar0 + part.bars}});
        for (int pb = 0; pb < part.bars; ++pb) {
            const int bar = bar0 + pb;
            const int ci = bar % 4;
            const double root = prog[ci];
            const double third = root + (minorChord[ci] ? 3 : 4), fifth = root + 7;
            const double barT = beats[(size_t) bar * 4];
            const double T = beats[(size_t) bar * 4 + 1] - beats[(size_t) bar * 4];
            for (int bt = 0; bt < 4; ++bt) {
                const double tb = beats[(size_t) bar * 4 + bt];
                if (part.f & KICK)
                    add(drums, tb, (size_t) (0.35 * sr), [&](double x) {
                        const double ph = 2 * kPi * (45 * x + 105 * 0.03 * (1 - std::exp(-x / 0.03)));
                        return (float) (0.9 * std::sin(ph) * std::exp(-x / 0.22));
                    });
                if (part.f & HATS) {
                    float prev = 0;
                    add(drums, tb + T / 2, (size_t) (0.05 * sr), [&](double x) {
                        const float w = uni(rng); const float hp = w - prev; prev = w;
                        return (float) (0.22 * hp * std::exp(-x / 0.02));
                    });
                }
                if ((part.f & PERC) && (bt % 2 == 1)) {
                    for (int s : {1, 3}) {
                        float prev = 0;
                        add(drums, tb + s * T / 4, (size_t) (0.04 * sr), [&](double x) {
                            const float w = uni(rng); const float hp = w - prev; prev = w;
                            return (float) (0.12 * hp * std::exp(-x / 0.015));
                        });
                    }
                }
                if ((part.f & CLAP) && (bt == 1 || bt == 3)) {
                    float lp = 0;
                    add(drums, tb, (size_t) (0.2 * sr), [&](double x) {
                        lp += 0.3f * (uni(rng) - lp);
                        return (float) (0.35 * lp * std::exp(-x / 0.08));
                    });
                }
                if (part.f & ROLL) {
                    const int div = pb >= part.bars - 1 ? 8 : 4;
                    const double amp = 0.08 + 0.25 * (pb * 4 + bt) / (double) (part.bars * 4);
                    for (int s = 0; s < div; ++s) {
                        float lp = 0;
                        add(drums, tb + s * T / div, (size_t) (0.06 * sr), [&](double x) {
                            lp += 0.4f * (uni(rng) - lp);
                            return (float) (amp * lp * std::exp(-x / 0.03));
                        });
                    }
                }
                if (part.f & BASS) {
                    const double f0 = hz(root - 12);
                    std::vector<double> starts;
                    if (spec.rollingBass) for (int s : {1, 2, 3}) starts.push_back(tb + s * T / 4);
                    else starts.push_back(tb + T / 2);
                    const double len = spec.rollingBass ? T / 4 * 0.9 : T / 2 * 0.9;
                    for (double st : starts)
                        add(bass, st, (size_t) (len * sr), [&](double x) {
                            double s = 0; for (int h = 1; h <= 6; ++h) s += std::sin(2 * kPi * f0 * h * x) / h;
                            const double env = std::min(1.0, x / 0.004) * std::exp(-x / (spec.rollingBass ? 0.08 : 0.2));
                            return (float) (0.35 * s * env);
                        });
                }
                if (part.f & LEAD)
                    for (int s = 0; s < 4; ++s) {
                        const double notes[4] = {root + 24, third + 24, fifth + 24, third + 24};
                        const double f0 = hz(notes[(bt * 4 + s) % 4]);
                        add(other, tb + s * T / 4, (size_t) (T / 4 * sr), [&](double x) {
                            double v = 0; for (int h = 1; h <= 7; h += 2) v += std::sin(2 * kPi * f0 * h * x) / h;
                            return (float) (0.16 * v * std::exp(-x / 0.07));
                        });
                    }
                if ((part.f & VOCAL) && bt % 2 == 0) {
                    const double f0 = hz((bt == 0 ? root : fifth) + 12 + 12);
                    add(vocals, tb, (size_t) (2 * T * sr * 0.95), [&](double x) {
                        const double vib = 1.0 + 0.012 * std::sin(2 * kPi * 5.5 * x);
                        double v = 0; const double formant[5] = {1.0, 0.8, 0.5, 0.6, 0.25};
                        for (int h = 1; h <= 5; ++h) v += formant[h - 1] * std::sin(2 * kPi * f0 * vib * h * x);
                        return (float) (0.12 * v * std::min(1.0, x / 0.05));
                    });
                }
            }
            if (part.f & PAD) {
                const double barLen = beats[(size_t) bar * 4 + 4] - barT;
                for (double m : {root + 12, third + 12, fifth + 12})
                    add(other, barT, (size_t) (barLen * sr), [&](double x) {
                        const double f0 = hz(m);
                        double v = 0; for (int h = 1; h <= 4; ++h) v += std::sin(2 * kPi * f0 * h * x + h) / (h * h);
                        return (float) (0.10 * v * std::min(1.0, x / 0.15) * std::min(1.0, (barLen - x) / 0.02));
                    });
            }
            if (part.f & RISER) {
                const double barLen = beats[(size_t) bar * 4 + 4] - barT;
                // band-pass noise (state-variable filter) with rising center + rising gain
                static double lowS = 0, bandS = 0;
                add(other, barT, (size_t) (barLen * sr), [&](double x) {
                    const double prog01 = (pb + x / barLen) / part.bars;
                    const double fc = 300.0 * std::pow(30.0, prog01);
                    const double f = 2 * std::sin(kPi * std::min(fc, sr / 6) / sr);
                    const double in = uni(rng);
                    lowS += f * bandS; const double high = in - lowS - 0.5 * bandS; bandS += f * high;
                    return (float) ((0.03 + 0.5 * prog01 * prog01) * (bandS + 0.3 * high));
                });
            }
            if ((part.f & IMPACT) && pb == 0)
                add(other, barT, (size_t) (1.5 * sr), [&](double x) {
                    return (float) (0.6 * uni(rng) * std::exp(-x / 0.25) + 0.7 * std::sin(2 * kPi * 48 * x) * std::exp(-x / 0.5));
                });
        }
        bar0 += part.bars;
    }
    tr.truth.bars = totalBars;
    tr.truth.beats.assign(beats.begin(), beats.end() - 1);
    std::vector<float> mix(n);
    float peak = 0;
    for (size_t i = 0; i < n; ++i) { mix[i] = drums[i] + bass[i] + vocals[i] + other[i]; peak = std::max(peak, std::fabs(mix[i])); }
    const float g = peak > 0 ? 0.9f / peak : 1.0f;
    for (size_t i = 0; i < n; ++i) { mix[i] *= g; drums[i] *= g; bass[i] *= g; vocals[i] *= g; other[i] *= g; }
    tr.mix.sampleRate = sr;
    tr.mix.channels = {mix, mix};
    tr.stems.sampleRate = sr;
    tr.stems.drums = std::move(drums); tr.stems.bass = std::move(bass);
    tr.stems.vocals = std::move(vocals); tr.stems.other = std::move(other);
    return tr;
}

// "Oracle" separator returning the true stems: tests the stem-based code
// path with perfect separation (upper bound of the ML path).
class OracleSeparator : public dali::IStemSeparator {
public:
    explicit OracleSeparator(const dali::StemSet& s) : stems_(s) {}
    const char* name() const override { return "oracle"; }
    bool separate(const dali::AudioData&, dali::StemSet& out, dali::ProgressSink*) override { out = stems_; return true; }
private:
    const dali::StemSet& stems_;
};

} // namespace synth
