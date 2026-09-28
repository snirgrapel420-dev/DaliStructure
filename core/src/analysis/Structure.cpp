#include "Pipeline.h"
#include "../dsp/Dsp.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <cstdio>
#include <cstdlib>

namespace dali::pipeline {
using namespace dali::dsp;

namespace {

// ---------------------------------------------------------------- feature matrix
// Each group is z-scored per dimension (with a floor on the std so that
// near-constant features do not explode) and weighted so that no single
// group dominates: weight / sqrt(dims).
std::vector<std::vector<double>> buildMatrix(const BarFeatures& bf) {
    const int N = bf.numBars;
    std::vector<std::vector<double>> X((size_t) N);
    struct Col { std::vector<double> v; double floorStd; double weight; };
    std::vector<Col> cols;
    const double dbFloor = percentile(bf.rmsDb, 95) - 50.0;
    auto clampDb = [&](double x) { return std::max(dbFloor, x); };

    auto addGroup = [&](std::vector<std::vector<double>> dims, double floorStd, double w) {
        const double gw = w / std::sqrt((double) std::max<size_t>(1, dims.size()));
        for (auto& d : dims) cols.push_back({std::move(d), floorStd, gw});
    };
    { std::vector<double> v; for (double x : bf.rmsDb) v.push_back(clampDb(x)); addGroup({v}, 2.0, 1.0); }
    { std::vector<std::vector<double>> g(kNumBands);
      for (int b = 0; b < N; ++b) for (int k = 0; k < kNumBands; ++k) g[(size_t) k].push_back(clampDb(bf.bandDb[(size_t) b][(size_t) k]));
      addGroup(g, 2.0, 1.0); }
    { std::vector<std::vector<double>> g;
      for (int e = 0; e < NumElements; ++e) if (bf.elementSupported[(size_t) e]) g.push_back(bf.activity[(size_t) e]);
      addGroup(g, 0.2, 1.5); }
    { std::vector<std::vector<double>> g(12);
      for (int b = 0; b < N; ++b) for (int k = 1; k < 13; ++k) g[(size_t) k - 1].push_back(bf.mfcc[(size_t) b][(size_t) k]);
      addGroup(g, 1.0, 0.7); }
    { std::vector<std::vector<double>> g(12);
      for (int b = 0; b < N; ++b) for (int k = 0; k < 12; ++k) g[(size_t) k].push_back(bf.chroma[(size_t) b][(size_t) k]);
      addGroup(g, 0.05, 0.5); }
    addGroup({bf.onsetDensity}, 0.2, 0.4);
    addGroup({bf.percRatio}, 0.05, 0.4);
    addGroup({bf.flatness}, 0.02, 0.3);
    addGroup({bf.centroid}, 100.0, 0.3);

    for (auto& c : cols) {
        const double m = mean(c.v), s = std::max(stddev(c.v), c.floorStd);
        for (int b = 0; b < N; ++b) X[(size_t) b].push_back((c.v[(size_t) b] - m) / s * c.weight);
    }
    return X;
}

double distMeans(const std::vector<std::vector<double>>& X, int a0, int a1, int b0, int b1) {
    const size_t D = X.empty() ? 0 : X[0].size();
    double d = 0;
    for (size_t k = 0; k < D; ++k) {
        double ma = 0, mb = 0;
        for (int i = a0; i < a1; ++i) ma += X[(size_t) i][k];
        for (int i = b0; i < b1; ++i) mb += X[(size_t) i][k];
        ma /= std::max(1, a1 - a0); mb /= std::max(1, b1 - b0);
        d += (ma - mb) * (ma - mb);
    }
    return std::sqrt(d);
}

struct Prefix {
    std::vector<std::vector<double>> s1;   // [N+1][D]
    std::vector<double> s2;                // [N+1]
    explicit Prefix(const std::vector<std::vector<double>>& X) {
        const size_t N = X.size(), D = N ? X[0].size() : 0;
        s1.assign(N + 1, std::vector<double>(D, 0.0)); s2.assign(N + 1, 0.0);
        for (size_t i = 0; i < N; ++i) {
            double q = 0;
            for (size_t k = 0; k < D; ++k) { s1[i + 1][k] = s1[i][k] + X[i][k]; q += X[i][k] * X[i][k]; }
            s2[i + 1] = s2[i] + q;
        }
    }
    double sse(int a, int b) const {
        const int n = b - a; if (n <= 0) return 0;
        double m = 0;
        for (size_t k = 0; k < s1[0].size(); ++k) { const double d = s1[(size_t) b][k] - s1[(size_t) a][k]; m += d * d; }
        return std::max(0.0, (s2[(size_t) b] - s2[(size_t) a]) - m / n);
    }
};

} // namespace

std::vector<double> computeNovelty(const BarFeatures& bf) {
    const int N = bf.numBars;
    std::vector<double> nov((size_t) N + 1, 0.0);
    if (N < 4) return nov;
    const auto X = buildMatrix(bf);
    const int scales[3] = {2, 4, 8};
    const double w[3] = {0.5, 1.0, 1.0};
    for (int b = 1; b < N; ++b) {
        double s = 0, ws = 0;
        for (int i = 0; i < 3; ++i) {
            const int L = scales[i];
            const int a0 = std::max(0, b - L), b1 = std::min(N, b + L);
            if (b - a0 < 1 || b1 - b < 1) continue;
            // shrink toward zero when a side is truncated (edge effects)
            const double shrink = std::sqrt(std::min(b - a0, b1 - b) / (double) L);
            s += w[i] * distMeans(X, a0, b, b, b1) * shrink; ws += w[i];
        }
        nov[(size_t) b] = ws > 0 ? s / ws : 0;
    }
    return nov;
}

std::vector<Segment> segmentBars(const BarFeatures& bf, const std::vector<double>& nov) {
    const int N = bf.numBars;
    if (N < 8) return {{0, N}};
    const auto X = buildMatrix(bf);
    const Prefix pre(X);

    // Phrase grid offset: which bar positions (mod 16) carry the most novelty.
    int off = 0;
    {
        double best = -1;
        for (int o = 0; o < 4; ++o) { double s = 0; for (int p = o; p < N; p += 4) s += nov[(size_t) p] * nov[(size_t) p]; if (s > best) { best = s; off = o; } }
        for (int lvl : {8, 16}) {
            int bestO = off; best = -1;
            for (int o = off; o < lvl; o += lvl / 2 >= 4 ? (lvl == 8 ? 4 : 8) : 4) {
                double s = 0; for (int p = o; p < N; p += lvl) s += nov[(size_t) p] * nov[(size_t) p];
                if (s > best) { best = s; bestO = o; }
            }
            off = bestO;
        }
    }
    const double C = 8.0;       // cost of one extra section
    const double wNov = 2.0;
    auto gridBonus = [&](int p) {
        const int r = ((p - off) % 16 + 16) % 16;
        if (r == 0) return 0.4;
        if (r % 8 == 0) return 0.3;
        if (r % 4 == 0) return 0.15;
        return -0.5;
    };
    auto lenPrior = [&](int len, bool edge) -> double {
        if (!edge && len < 4) return -1e9;
        if (edge && len < 2) return -1e9;
        if (len == 8 || len == 16 || len == 32 || len == 64) return 0.5;
        if (len == 4 || len == 24 || len == 48) return 0.2;
        if (len % 4 == 0) return 0.0;
        return edge ? -0.2 : -0.8;
    };

    const double NEG = -std::numeric_limits<double>::infinity();
    std::vector<double> best((size_t) N + 1, NEG);
    std::vector<int> prev((size_t) N + 1, -1);
    best[0] = 0;
    for (int b = 1; b <= N; ++b) {
        for (int a = 0; a < b; ++a) {
            if (best[(size_t) a] == NEG) continue;
            const bool edge = (a == 0) || (b == N);
            const double lp = lenPrior(b - a, edge);
            if (lp < -1e8) continue;
            double s = best[(size_t) a] - pre.sse(a, b) + C * lp;
            if (b < N) s += wNov * nov[(size_t) b] + C * gridBonus(b) - C;
            if (s > best[(size_t) b]) { best[(size_t) b] = s; prev[(size_t) b] = a; }
        }
    }
    std::vector<Segment> segs;
    for (int b = N; b > 0; b = prev[(size_t) b]) {
        if (prev[(size_t) b] < 0) { segs.push_back({0, b}); break; }
        segs.push_back({prev[(size_t) b], b});
    }
    std::reverse(segs.begin(), segs.end());
    return segs;
}

// ---------------------------------------------------------------- classification
namespace {

struct Desc {
    int len = 0; bool first = false, last = false;
    double E = 0, kick = 0, bass = 0, perc = 0, lead = 0, pad = 0, vocal = 0, silentFrac = 0;
    double slope = 0;       // energy change across the section (regression), 0..100 scale
    bool riserEnd = false;
};

Desc describe(const BarFeatures& bf, const Segment& s, const std::vector<Event>* fx, bool first, bool last) {
    Desc d; d.len = s.end - s.start; d.first = first; d.last = last;
    auto avg = [&](const std::vector<double>& v) { double m = 0; for (int b = s.start; b < s.end; ++b) m += v[(size_t) b]; return m / std::max(1, d.len); };
    d.E = avg(bf.barEnergy);
    d.kick = avg(bf.activity[Kick]); d.bass = avg(bf.activity[Bass]); d.perc = avg(bf.activity[Percussion]);
    d.lead = avg(bf.activity[Lead]); d.pad = avg(bf.activity[Pad]); d.vocal = avg(bf.activity[Vocal]);
    int sil = 0; for (int b = s.start; b < s.end; ++b) sil += bf.silent[(size_t) b];
    d.silentFrac = (double) sil / std::max(1, d.len);
    if (d.len >= 2) {
        double sx = 0, sy = 0, sxx = 0, sxy = 0;
        for (int b = s.start; b < s.end; ++b) { const double x = b - s.start, y = bf.barEnergy[(size_t) b]; sx += x; sy += y; sxx += x * x; sxy += x * y; }
        const double n = d.len, den = n * sxx - sx * sx;
        d.slope = den > 1e-9 ? (n * sxy - sx * sy) / den * (n - 1) : 0;
    }
    if (fx) for (auto& e : *fx)
        if (e.type == EventType::Riser && std::fabs((e.bar + e.lengthBars) - (s.end + 1)) <= 1.01) d.riserEnd = true;
    return d;
}

struct Scored { SectionType type; double score; };

std::vector<Scored> scoreSection(const Desc& d, const Desc* prev, const Desc* next, double minE, double maxE) {
    const double relE = maxE - minE > 1e-9 ? (d.E - minE) / (maxE - minE) : 0.5;
    const double hiE = ramp(relE, 0.45, 0.8);
    const double kickOn = ramp(d.kick, 0.35, 0.7), kickOff = 1.0 - ramp(d.kick, 0.15, 0.45);
    const double bassOn = ramp(d.bass, 0.3, 0.65);
    // An energy jump right after the INTRO is the track "starting", not a drop:
    // drops follow tension (breakdown / build), so the jump counts less there.
    const double jumpIn = prev ? ramp(d.E - prev->E, 6, 20) * (prev->first ? 0.3 : 1.0) : 0.0;
    const double prevCalm = prev ? 1.0 - ramp(prev->kick, 0.15, 0.45) : 0.0;
    const double nextJump = next ? ramp(next->E - d.E, 6, 20) : 0.0;
    const double rise = std::max(ramp(d.slope, 6, 20), d.riserEnd ? 0.8 : 0.0);
    const double mid = (!d.first && !d.last) ? 1.0 : 0.0;
    const double shortS = 1.0 - ramp(d.len, 8, 16), longish = ramp(d.len, 6, 12);

    std::vector<Scored> s;
    s.push_back({SectionType::Drop, kickOn * bassOn * hiE * std::max(jumpIn, 0.9 * prevCalm) * (mid ? 1.0 : 0.3)});
    s.push_back({SectionType::Main, kickOn * bassOn * ramp(relE, 0.35, 0.7) * (1.0 - 0.8 * std::max(jumpIn, prevCalm)) * (mid ? 1.0 : 0.4)});
    s.push_back({SectionType::Groove, kickOn * (1.0 - std::min(bassOn, hiE)) * (d.last ? 0.3 : 1.0) * (d.first ? 0.6 : 1.0)});
    // Position is the strongest evidence for INTRO/OUTRO (DJ intros are often full-energy drums).
    s.push_back({SectionType::Intro, d.first ? 0.75 + 0.25 * (1.0 - hiE) : 0.0});
    s.push_back({SectionType::Outro, d.last ? std::min(1.0, (0.75 + 0.25 * (1.0 - hiE)) * (1.0 + 0.2 * ramp(-d.slope, 5, 15))) : 0.0});
    s.push_back({SectionType::Breakdown, kickOff * longish * mid * (0.6 + 0.4 * (1.0 - hiE)) * (1.0 - 0.5 * rise * nextJump)});
    s.push_back({SectionType::Break, mid * std::max(kickOff * shortS * 0.9 * (1.0 - 0.5 * nextJump), 0.9 * ramp(d.silentFrac, 0.3, 0.6))});
    s.push_back({SectionType::Build, rise * (0.35 + 0.65 * nextJump) * (1.0 - ramp(d.len, 32, 48)) * (d.last ? 0.0 : 1.0)});
    // PRE-DROP = the short tension moment IMMEDIATELY before the drop
    const double nextIsDrop = next ? ramp(next->kick, 0.35, 0.7) * ramp(next->bass, 0.3, 0.65) : 0.0;
    s.push_back({SectionType::PreDrop, (1.0 - ramp(d.len, 2, 6)) * nextJump * nextIsDrop * std::max(kickOff, d.riserEnd ? 1.0 : 0.0) * mid});
    std::sort(s.begin(), s.end(), [](const Scored& a, const Scored& b) { return a.score > b.score; });
    return s;
}

} // namespace

std::vector<Classified> classifySections(const BarFeatures& bf, std::vector<Segment> segs,
                                         const std::vector<double>& nov, const std::vector<Event>* fx) {
    std::vector<Classified> out;
    for (int pass = 0; pass < 4; ++pass) {
        std::vector<Desc> D;
        for (size_t i = 0; i < segs.size(); ++i) D.push_back(describe(bf, segs[i], fx, i == 0, i + 1 == segs.size()));
        double minE = 1e9, maxE = -1e9;
        for (auto& d : D) { minE = std::min(minE, d.E); maxE = std::max(maxE, d.E); }
        out.clear();
        for (size_t i = 0; i < segs.size(); ++i) {
            const auto sc = scoreSection(D[i], i ? &D[i - 1] : nullptr, i + 1 < D.size() ? &D[i + 1] : nullptr, minE, maxE);
            if (debugEnabled() && pass == 0) {
                const auto& d = D[i];
                std::fprintf(stderr, "seg %3d-%3d E=%5.1f kick=%.2f bass=%.2f perc=%.2f lead=%.2f pad=%.2f slope=%5.1f riser=%d | %s %.2f, %s %.2f\n",
                             segs[i].start + 1, segs[i].end, d.E, d.kick, d.bass, d.perc, d.lead, d.pad, d.slope, (int) d.riserEnd,
                             toString(sc[0].type), sc[0].score, toString(sc[1].type), sc[1].score);
            }
            Classified c; c.seg = segs[i]; c.energy = D[i].E;
            const double s1 = sc[0].score, s2 = sc.size() > 1 ? sc[1].score : 0.0;
            c.confidence = s1 * (0.55 + 0.45 * clamp01((s1 - s2) / 0.35));
            if (c.confidence >= 0.5) c.type = sc[0].type;
            else c.type = D[i].len <= 8 ? SectionType::Transition : SectionType::Section;
            if (c.type == SectionType::Transition || c.type == SectionType::Section)
                c.confidence = std::max(0.3, c.confidence);   // honest: "we know it's a section, not which kind"
            out.push_back(c);
        }
        // Merge neighbours with the same label unless the boundary is strong.
        bool merged = false;
        std::vector<Segment> next;
        for (size_t i = 0; i < out.size(); ++i) {
            // A build often starts quietly: a short kick-less piece right before a BUILD belongs to it.
            const bool quietLeadIn = i > 0 && out[i].type == SectionType::Build
                && (out[i - 1].type == SectionType::Transition || out[i - 1].type == SectionType::Break)
                && out[i - 1].seg.end - out[i - 1].seg.start <= 8 && (out[i].seg.end - next.back().start) <= 16;
            if (!next.empty() && quietLeadIn) { next.back().end = out[i].seg.end; merged = true; continue; }
            const bool bothBuild = out[i].type == SectionType::Build && out[i - (i ? 1 : 0)].type == SectionType::Build;
            if (!next.empty() && out[i].type == out[i - 1].type && out[i].type != SectionType::Transition
                && (nov[(size_t) out[i].seg.start] < 2.0 || (bothBuild && out[i].seg.end - next.back().start <= 16)) && (out[i].seg.end - next.back().start) <= 64) {
                next.back().end = out[i].seg.end; merged = true;
            } else next.push_back(out[i].seg);
        }
        if (!merged) break;
        segs = next;
    }
    return out;
}

} // namespace dali::pipeline
