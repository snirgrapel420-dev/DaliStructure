#include "Pipeline.h"
#include "../dsp/Dsp.h"
#include <algorithm>
#include <cmath>
#include <numeric>
#include <cstdio>
#include <cstdlib>

namespace dali::pipeline {
using namespace dali::dsp;

const char* elementName(Element e) {
    switch (e) {
        case Kick: return "Kick"; case Bass: return "Bass"; case Percussion: return "Percussion";
        case Lead: return "Lead"; case Pad: return "Chords / Pad"; case Vocal: return "Vocal";
        default: return "?";
    }
}

namespace {

struct Range { int a, b; };
Range frames(double t0, double t1, double fps, int n) {
    int a = (int) std::floor(t0 * fps + 0.5), b = (int) std::floor(t1 * fps + 0.5);
    a = std::clamp(a, 0, n); b = std::clamp(b, 0, n);
    if (b <= a && a < n) b = a + 1;
    return {a, b};
}

double sumBands(const std::array<float, kNumBands>& x, int from, int to) {
    double s = 0; for (int b = from; b <= to; ++b) s += x[(size_t) b]; return s;
}

std::vector<double> normalizedFlux(const std::vector<float>& raw, double fps) {
    std::vector<double> x(raw.begin(), raw.end());
    const auto avg = movingAverage(x, std::max(1, (int) std::lround(0.3 * fps)));
    for (size_t i = 0; i < x.size(); ++i) x[i] = std::max(0.0, x[i] - avg[i]);
    const double sd = stddev(x);
    if (sd > 1e-9) for (auto& v : x) v /= sd;
    return x;
}

std::vector<int> pickPeaks(const std::vector<double>& o, double fps) {
    std::vector<int> peaks;
    const int n = (int) o.size(), w = 3, lw = (int) fps;
    std::vector<double> prefix((size_t) n + 1, 0);
    for (int i = 0; i < n; ++i) prefix[(size_t) i + 1] = prefix[(size_t) i] + o[(size_t) i];
    for (int i = 1; i < n - 1; ++i) {
        bool isMax = true;
        for (int j = std::max(0, i - w); j <= std::min(n - 1, i + w) && isMax; ++j) if (o[(size_t) j] > o[(size_t) i]) isMax = false;
        if (!isMax) continue;
        const int a = std::max(0, i - lw), b = std::min(n, i + lw);
        const double localMean = (prefix[(size_t) b] - prefix[(size_t) a]) / std::max(1, b - a);
        if (o[(size_t) i] > localMean + 0.5 && o[(size_t) i] > 0.7) peaks.push_back(i);
    }
    return peaks;
}

double maxAround(const std::vector<float>& x, double fps, double t, int radius) {
    const int c = (int) std::lround(t * fps);
    double m = 0;
    for (int i = c - radius; i <= c + radius; ++i) if (i >= 0 && i < (int) x.size()) m = std::max(m, (double) x[(size_t) i]);
    return m;
}

// Relative activity 0..1 from a per-bar dB series.
std::vector<double> activityFromDb(const std::vector<double>& db, double mixRefDb, double absentBelowDb,
                                   const std::vector<bool>& silent) {
    std::vector<double> act(db.size(), 0.0);
    if (db.empty()) return act;
    const double ref = percentile(db, 95);
    if (ref < mixRefDb - absentBelowDb) return act;
    for (size_t i = 0; i < db.size(); ++i) act[i] = silent[i] ? 0.0 : ramp(db[i] - ref, -18.0, -6.0);
    return act;
}

} // namespace

BarFeatures computeBarFeatures(const FrameFeatures& ff, const StemFeatureSet& stems, const TempoMap& tm) {
    BarFeatures bf;
    const int N = tm.barCount(), bpb = tm.beatsPerBar;
    bf.numBars = N;
    const int nA = ff.framesA(), nB = ff.framesB(), nC = ff.framesC();
    const bool useStems = stems.valid;

    bf.rmsDb.resize((size_t) N); bf.bandDb.resize((size_t) N); bf.centroid.resize((size_t) N);
    bf.flatness.resize((size_t) N); bf.onsetDensity.resize((size_t) N); bf.percRatio.resize((size_t) N);
    bf.mfcc.resize((size_t) N); bf.chroma.resize((size_t) N); bf.silent.resize((size_t) N);
    bf.bassPattern.resize((size_t) N); bf.percPattern.resize((size_t) N); bf.barBassChroma.resize((size_t) N);

    const auto onset = normalizedFlux(ff.flux, ff.fpsA);
    const auto peaks = pickPeaks(onset, ff.fpsA);

    // ---- mix descriptors per bar
    for (int b = 0; b < N; ++b) {
        const double t0 = tm.timeAtBar(b + 1), t1 = tm.timeAtBar(b + 2);
        const Range rb = frames(t0, t1, ff.fpsB, nB);
        std::array<double, kNumBands> bp{}; double c = 0, fl = 0, h = 0, p = 0; std::array<double, 13> mf{};
        for (int f = rb.a; f < rb.b; ++f) {
            for (int k = 0; k < kNumBands; ++k) { bp[(size_t) k] += ff.band[(size_t) f][(size_t) k]; h += ff.bandH[(size_t) f][(size_t) k]; p += ff.bandP[(size_t) f][(size_t) k]; }
            c += ff.centroid[(size_t) f]; fl += ff.flatness[(size_t) f];
            for (int k = 0; k < 13; ++k) mf[(size_t) k] += ff.mfcc[(size_t) f][(size_t) k];
        }
        const double cnt = std::max(1, rb.b - rb.a);
        double tot = 0;
        for (int k = 0; k < kNumBands; ++k) { bf.bandDb[(size_t) b][(size_t) k] = toDb(bp[(size_t) k] / cnt + 1e-12, -100); tot += bp[(size_t) k] / cnt; }
        bf.rmsDb[(size_t) b] = toDb(tot + 1e-12, -100);
        bf.centroid[(size_t) b] = c / cnt; bf.flatness[(size_t) b] = fl / cnt;
        bf.percRatio[(size_t) b] = (h + p) > 1e-12 ? p / (h + p) : 0;
        for (int k = 0; k < 13; ++k) bf.mfcc[(size_t) b][(size_t) k] = mf[(size_t) k] / cnt;

        const Range rc = frames(t0, t1, ff.fpsC, nC);
        std::array<double, 12> ch{}, bc{};
        for (int f = rc.a; f < rc.b; ++f) for (int k = 0; k < 12; ++k) { ch[(size_t) k] += ff.chroma[(size_t) f][(size_t) k]; bc[(size_t) k] += ff.bassChroma[(size_t) f][(size_t) k]; }
        auto l2 = [](std::array<double, 12>& v) { double s = 0; for (double x : v) s += x * x; s = std::sqrt(s); if (s > 1e-12) for (auto& x : v) x /= s; };
        l2(ch); l2(bc);
        bf.chroma[(size_t) b] = ch; bf.barBassChroma[(size_t) b] = bc;

        const int fa = (int) std::floor(t0 * ff.fpsA), fb = (int) std::floor(t1 * ff.fpsA);
        const long np = std::count_if(peaks.begin(), peaks.end(), [&](int q) { return q >= fa && q < fb; });
        bf.onsetDensity[(size_t) b] = (double) np / bpb;
    }
    const double mixRef = percentile(bf.rmsDb, 95);
    for (int b = 0; b < N; ++b) bf.silent[(size_t) b] = bf.rmsDb[(size_t) b] < std::max(-80.0, mixRef - 40.0);

    // ---- kick presence (beat-synchronous low-band transients)
    // LINEAR low-band power rise (log flux is level-independent and turns
    // tiny residues in quiet parts into large "onsets").
    auto linearRise = [](const std::vector<float>& db) {
        std::vector<float> r(db.size(), 0.0f);
        for (size_t f = 1; f < db.size(); ++f)
            r[f] = (float) std::max(0.0, std::pow(10.0, db[f] / 10.0) - std::pow(10.0, db[f - 1] / 10.0));
        return r;
    };
    const std::vector<float> lowRise = linearRise(ff.lowDb);
    const std::vector<float> drumsLowRise = useStems ? linearRise(stems.drums.lowDb) : std::vector<float>{};
    const std::vector<float>& kickEnv = useStems ? drumsLowRise : lowRise;
    const double kickFps = useStems ? stems.drums.fpsA : ff.fpsA;
    std::vector<double> onBeat;
    for (int k = 0; k < tm.beatCount(); ++k) onBeat.push_back(maxAround(kickEnv, kickFps, tm.beatTimes[(size_t) k], 2));
    const double kickAbs = 0.25 * percentile(onBeat, 90);
    bf.activity[Kick].assign((size_t) N, 0.0);
    for (int b = 0; b < N; ++b) {
        int hits = 0;
        for (int j = 0; j < bpb; ++j) {
            const int k = b * bpb + j;
            const double tb = tm.timeAtBeat(k), T = tm.timeAtBeat(k + 1) - tb;
            const double at = maxAround(kickEnv, kickFps, tb, 2);
            std::vector<double> off;
            for (int q = 1; q < 4; ++q) off.push_back(maxAround(kickEnv, kickFps, tb + q * T / 4.0, 1));
            const double base = median(off);
            if (at > kickAbs && at > 1.4 * base + 1e-6) ++hits;
        }
        bf.activity[Kick][(size_t) b] = bf.silent[(size_t) b] ? 0.0 : ramp((double) hits / bpb, 0.2, 0.75);
    }

    // ---- other elements: per-bar dB series
    std::vector<double> bassDb((size_t) N), percDb((size_t) N), leadDb((size_t) N), padDb((size_t) N), vocalDb((size_t) N, -100);
    auto barSeries = [&](const std::vector<std::array<float, kNumBands>>& src, double fps, int b, int lo, int hi,
                         double phaseLo, double phaseHi, double* varDb) {
        const double t0 = tm.timeAtBar(b + 1), t1 = tm.timeAtBar(b + 2);
        const Range r = frames(t0, t1, fps, (int) src.size());
        double s = 0; int c = 0; std::vector<double> dbs;
        for (int f = r.a; f < r.b; ++f) {
            const double ph = tm.beatAtTime(f / fps);
            const double frac = ph - std::floor(ph);
            if (frac < phaseLo || frac > phaseHi) continue;
            const double v = sumBands(src[(size_t) f], lo, hi);
            s += v; ++c; dbs.push_back(toDb(v + 1e-12, -100));
        }
        if (varDb) *varDb = dbs.size() > 2 ? stddev(dbs) : 0;
        return c ? toDb(s / c + 1e-12, -100) : -100.0;
    };
    std::vector<double> leadVar((size_t) N), padVar((size_t) N);
    const auto& melodic = useStems ? stems.other.band : ff.bandH;
    const double melodicFps = useStems ? stems.other.fpsB : ff.fpsB;
    for (int b = 0; b < N; ++b) {
        if (useStems) {
            bassDb[(size_t) b] = barSeries(stems.bass.band, stems.bass.fpsB, b, Sub, LowMid, 0, 1, nullptr);
            percDb[(size_t) b] = barSeries(stems.drums.band, stems.drums.fpsB, b, HighMid, High, 0, 1, nullptr);
            vocalDb[(size_t) b] = barSeries(stems.vocals.band, stems.vocals.fpsB, b, LowMid, HighMid, 0, 1, nullptr);
        } else {
            bassDb[(size_t) b] = -100;   // computed below (needs kick-tail model)
            percDb[(size_t) b] = barSeries(ff.bandP, ff.fpsB, b, High, High, 0, 1, nullptr);
        }
        leadDb[(size_t) b] = barSeries(melodic, melodicFps, b, Mid, HighMid, 0, 1, &leadVar[(size_t) b]);
        padDb[(size_t) b] = barSeries(melodic, melodicFps, b, LowMid, Mid, 0, 1, &padVar[(size_t) b]);
    }
    std::vector<double> bassOnsetAct((size_t) N, -1.0);
    if (!useStems) {
        // Mix-only bass. With a kick present, low ENERGY is dominated by the
        // kick's tonal tail, so we use low-band ONSETS between the beats
        // (16ths 2-4): a decaying kick tail produces none, bass notes do.
        // Without a kick, plain low harmonic energy is reliable.
        const double kickLevel = std::max(1e-12, percentile(onBeat, 90));
        for (int b = 0; b < N; ++b) {
            bassDb[(size_t) b] = barSeries(ff.bandH, ff.fpsB, b, Sub, Low, 0, 1, nullptr);
            if (bf.activity[Kick][(size_t) b] < 0.5) continue;
            std::vector<double> perBeat;
            for (int j = 0; j < bpb; ++j) {
                const int k = b * bpb + j;
                const double tb = tm.timeAtBeat(k), T = tm.timeAtBeat(k + 1) - tb;
                double m = 0;
                for (int q = 1; q < 4; ++q) m = std::max(m, maxAround(lowRise, ff.fpsA, tb + q * T / 4.0, 1));
                perBeat.push_back(m / kickLevel);
            }
            bassOnsetAct[(size_t) b] = ramp(median(perBeat), 0.03, 0.12);
            if (debugEnabled() && b % 8 == 0) std::fprintf(stderr, "bar %d offLow %.3f\n", b + 1, median(perBeat));
        }
    }
    for (int b = 0; b < N; ++b) {
        leadDb[(size_t) b] -= 2.0 * std::max(0.0, 2.5 - leadVar[(size_t) b]);
        padDb[(size_t) b] -= 1.5 * std::max(0.0, padVar[(size_t) b] - 3.0);
    }
    const double absent = useStems ? 30.0 : 35.0;
    bf.activity[Bass] = activityFromDb(bassDb, mixRef, absent, bf.silent);
    for (int b = 0; b < N; ++b) if (bassOnsetAct[(size_t) b] >= 0) bf.activity[Bass][(size_t) b] = bassOnsetAct[(size_t) b];
    bf.activity[Percussion] = activityFromDb(percDb, mixRef, absent, bf.silent);
    bf.activity[Lead] = activityFromDb(leadDb, mixRef, absent, bf.silent);
    bf.activity[Pad] = activityFromDb(padDb, mixRef, absent, bf.silent);
    bf.activity[Vocal] = useStems ? activityFromDb(vocalDb, mixRef, 30.0, bf.silent) : std::vector<double>((size_t) N, 0.0);
    bf.elementSupported.fill(true);
    bf.elementSupported[Vocal] = useStems;

    // ---- 16th-note patterns for pattern-change events
    const std::vector<float>& bassEnv = useStems ? stems.bass.fluxLow : ff.fluxLow;
    const std::vector<float>& percEnv = useStems ? stems.drums.fluxHigh : ff.fluxHigh;
    for (int b = 0; b < N; ++b)
        for (int s = 0; s < 16; ++s) {
            const double t = tm.timeAtBeat(b * bpb + s * bpb / 16.0);
            bf.bassPattern[(size_t) b][(size_t) s] = maxAround(bassEnv, ff.fpsA, t, 1);
            bf.percPattern[(size_t) b][(size_t) s] = maxAround(percEnv, ff.fpsA, t, 1);
        }

    {
        std::vector<double> atBeat;
        for (int k = 0; k < N * bpb; ++k) atBeat.push_back(maxAround(lowRise, ff.fpsA, tm.timeAtBeat(k), 2));
        const double ref = std::max(1e-12, percentile(atBeat, 90));
        for (double v : atBeat) bf.beatLowRise.push_back(v / ref);
    }
    // ---- beat-level descriptors + energy
    const int nBeats = N * bpb;
    bf.beatHighDb.resize((size_t) nBeats); bf.beatCentroid.resize((size_t) nBeats);
    bf.beatFlatness.resize((size_t) nBeats); bf.beatOnset.resize((size_t) nBeats);
    std::vector<double> eL((size_t) nBeats), eLo((size_t) nBeats), eOn((size_t) nBeats), eSd((size_t) nBeats), eP((size_t) nBeats), eH((size_t) nBeats);
    for (int k = 0; k < nBeats; ++k) {
        const double t0 = tm.timeAtBeat(k), t1 = tm.timeAtBeat(k + 1);
        const Range ra = frames(t0, t1, ff.fpsA, nA);
        double hi = 0, on = 0;
        for (int f = ra.a; f < ra.b; ++f) { hi += std::pow(10.0, ff.highDb[(size_t) f] / 10.0); on += onset[(size_t) f]; }
        const double ca = std::max(1, ra.b - ra.a);
        bf.beatHighDb[(size_t) k] = toDb(hi / ca + 1e-12, -100);
        bf.beatOnset[(size_t) k] = on / ca;
        const Range rb = frames(t0, t1, ff.fpsB, nB);
        std::array<double, kNumBands> bp{}; double h = 0, p = 0, c = 0, fl = 0;
        for (int f = rb.a; f < rb.b; ++f) {
            for (int q = 0; q < kNumBands; ++q) { bp[(size_t) q] += ff.band[(size_t) f][(size_t) q]; h += ff.bandH[(size_t) f][(size_t) q]; p += ff.bandP[(size_t) f][(size_t) q]; }
            c += ff.centroid[(size_t) f]; fl += ff.flatness[(size_t) f];
        }
        const double cb = std::max(1, rb.b - rb.a);
        bf.beatCentroid[(size_t) k] = c / cb; bf.beatFlatness[(size_t) k] = fl / cb;
        double tot = 0; for (double v : bp) tot += v;
        eL[(size_t) k] = toDb(tot / cb + 1e-12, -100);
        eLo[(size_t) k] = toDb((bp[Sub] + bp[Low]) / cb + 1e-12, -100);
        eP[(size_t) k] = toDb(p / cb + 1e-12, -100);
        eH[(size_t) k] = toDb(h / cb + 1e-12, -100);
        double ent = 0;
        for (double v : bp) if (tot > 1e-12 && v > 0) { const double q = v / tot; ent -= q * std::log(q); }
        eSd[(size_t) k] = ent / std::log((double) kNumBands);
        const double w0 = tm.timeAtBeat(k - 2), w1 = tm.timeAtBeat(k + 2);
        const int fa = (int) std::floor(w0 * ff.fpsA), fb = (int) std::floor(w1 * ff.fpsA);
        eOn[(size_t) k] = (double) std::count_if(peaks.begin(), peaks.end(), [&](int q) { return q >= fa && q < fb; }) / 4.0;
    }
    auto norm01 = [](std::vector<double> v) {
        const double lo = percentile(v, 5), hi = percentile(v, 95);
        for (auto& x : v) x = hi - lo > 1e-9 ? clamp01((x - lo) / (hi - lo)) : 0.0;
        return v;
    };
    eL = norm01(eL); eLo = norm01(eLo); eOn = norm01(eOn); eSd = norm01(eSd); eP = norm01(eP); eH = norm01(eH);
    std::vector<double> e((size_t) nBeats);
    for (int k = 0; k < nBeats; ++k)
        e[(size_t) k] = 0.30 * eL[(size_t) k] + 0.20 * eLo[(size_t) k] + 0.15 * eOn[(size_t) k]
                      + 0.10 * eSd[(size_t) k] + 0.15 * eP[(size_t) k] + 0.10 * eH[(size_t) k];
    e = movingAverage(e, 2);
    const double lo = percentile(e, 2), hi = percentile(e, 98);
    bf.beatEnergy.resize((size_t) nBeats);
    for (int k = 0; k < nBeats; ++k) bf.beatEnergy[(size_t) k] = hi - lo > 1e-9 ? 100.0 * clamp01((e[(size_t) k] - lo) / (hi - lo)) : 0.0;
    bf.barEnergy.assign((size_t) N, 0.0);
    for (int b = 0; b < N; ++b) {
        double s = 0; for (int j = 0; j < bpb; ++j) s += bf.beatEnergy[(size_t) (b * bpb + j)];
        bf.barEnergy[(size_t) b] = s / bpb;
    }
    return bf;
}

KeyInfo detectKey(const FrameFeatures& ff) {
    static const double major[12] = {6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88};
    static const double minor[12] = {6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17};
    static const char* names[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    KeyInfo k;
    if (ff.chroma.empty()) return k;
    std::vector<double> energy;
    for (auto& c : ff.chroma) energy.push_back(std::accumulate(c.begin(), c.end(), 0.0));
    const double thr = percentile(energy, 40);
    std::vector<double> prof(12, 0.0);
    for (size_t f = 0; f < ff.chroma.size(); ++f) {
        if (energy[f] <= thr || energy[f] <= 1e-12) continue;
        for (int q = 0; q < 12; ++q) prof[(size_t) q] += std::sqrt(ff.chroma[f][(size_t) q] / energy[f]);
    }
    struct Cand { double r; int tonic; bool minor; };
    std::vector<Cand> cands;
    for (int t = 0; t < 12; ++t)
        for (int m = 0; m < 2; ++m) {
            std::vector<double> ref(12);
            for (int q = 0; q < 12; ++q) ref[(size_t) q] = (m ? minor : major)[(q - t + 12) % 12];
            cands.push_back({pearson(prof, ref), t, m == 1});
        }
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.r > b.r; });
    const Cand& best = cands[0];
    const int relTonic = best.minor ? (best.tonic + 3) % 12 : (best.tonic + 9) % 12;
    double second = -1, relative = -1;
    for (size_t i = 1; i < cands.size(); ++i) {
        const bool isRel = cands[i].tonic == relTonic && cands[i].minor != best.minor;
        if (isRel) relative = std::max(relative, cands[i].r);
        else second = std::max(second, cands[i].r);
    }
    k.name = std::string(names[best.tonic]) + (best.minor ? " minor" : " major");
    k.confidence = ramp(best.r, 0.4, 0.8) * ramp(best.r - second, 0.02, 0.15) * (0.6 + 0.4 * ramp(best.r - relative, 0.0, 0.08));
    return k;
}

} // namespace dali::pipeline
