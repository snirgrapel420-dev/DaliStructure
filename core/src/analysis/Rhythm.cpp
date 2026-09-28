#include "Pipeline.h"
#include "../dsp/Dsp.h"
#include <algorithm>
#include <cmath>
#include <numeric>
#include <cstdio>
#include <cstdlib>

namespace dali::pipeline {
using namespace dali::dsp;

// Measured with synthetic transients (tests/test_rhythm): the log-mel flux
// peak of a centered 2048-sample frame lags the true onset by this amount.
static constexpr double kFluxLatencySec = -0.020;

namespace {

std::vector<double> normalizeEnvelope(const std::vector<float>& raw, double fps) {
    std::vector<double> x(raw.begin(), raw.end());
    const auto avg = movingAverage(x, std::max(1, (int) std::lround(0.3 * fps)));
    for (size_t i = 0; i < x.size(); ++i) x[i] = std::max(0.0, x[i] - avg[i]);
    const double sd = stddev(x);
    if (sd > 1e-9) for (auto& v : x) v /= sd;
    return x;
}

std::vector<double> blur(const std::vector<double>& x) {
    static const double k[5] = {0.054, 0.244, 0.403, 0.244, 0.054};
    std::vector<double> y(x.size(), 0.0);
    const int n = (int) x.size();
    for (int i = 0; i < n; ++i)
        for (int j = -2; j <= 2; ++j) { const int t = std::clamp(i + j, 0, n - 1); y[(size_t) i] += k[j + 2] * x[(size_t) t]; }
    return y;
}

double sampleAt(const std::vector<double>& env, double fps, double t) {
    const double f = t * fps;
    if (f < 0 || f >= (double) env.size() - 1) return 0;
    const size_t i = (size_t) f; const double a = f - (double) i;
    return env[i] * (1 - a) + env[i + 1] * a;
}

double autocorr(const std::vector<double>& o, int lag, int from, int to) {
    double s = 0; int c = 0;
    for (int t = from; t + lag < to; ++t) { s += o[(size_t) t] * o[(size_t) (t + lag)]; ++c; }
    return c ? s / c : 0;
}

double tempoPrior(double bpm) {
    const double z = std::log2(bpm / 128.0) / 0.9;
    return std::exp(-0.5 * z * z);
}

// Returns period in frames (fractional); strength = normalized peak salience.
// Lags are searched on a fractional grid (0.05 frame) with interpolated
// autocorrelation: integer-only lags bias the comb (2L, 4L) toward tempos
// whose period happens to be close to an integer number of frames.
double estimatePeriod(const std::vector<double>& oRaw, double fps, int from, int to,
                      double minBpm, double maxBpm, double* strength) {
    const std::vector<double> o = blur(oRaw);
    const double lagMin = std::max(2.0, 60.0 * fps / maxBpm);
    const double lagMax = 60.0 * fps / minBpm;
    const int maxNeeded = (int) std::ceil(lagMax * 4) + 2;
    std::vector<double> ac((size_t) maxNeeded + 2, 0.0);
    for (int l = 0; l <= maxNeeded + 1; ++l) ac[(size_t) l] = autocorr(o, l, from, to);
    const double ac0 = ac[0] > 1e-12 ? ac[0] : 1.0;
    auto acAt = [&](double l) {
        const size_t i = (size_t) l; const double f = l - (double) i;
        return i + 1 < ac.size() ? ac[i] * (1 - f) + ac[i + 1] * f : 0.0;
    };
    auto sal = [&](double l) { return acAt(l) + 0.5 * acAt(2 * l) + 0.25 * acAt(4 * l); };
    double best = 0, bestScore = -1e300;
    for (double l = lagMin; l <= lagMax; l += 0.05) {
        const double s = sal(l) * tempoPrior(60.0 * fps / l);
        if (s > bestScore) { bestScore = s; best = l; }
    }
    if (best <= 0) { if (strength) *strength = 0; return 0; }
    if (strength) {
        double meanAc = 0; int c = 0;
        for (int l = (int) std::ceil(lagMin); l <= (int) lagMax; ++l) { meanAc += ac[(size_t) l]; ++c; }
        meanAc /= std::max(1, c);
        *strength = (acAt(best) - meanAc) / ac0;
    }
    return best;
}

// Ellis (2007) dynamic-programming beat tracker with a per-frame period.
std::vector<int> trackBeatsDP(const std::vector<double>& o, const std::vector<double>& period, double tightness) {
    const int n = (int) o.size();
    std::vector<double> score((size_t) n, 0.0);
    std::vector<int> back((size_t) n, -1);
    for (int t = 0; t < n; ++t) {
        const double P = period[(size_t) t];
        const int lo = t - (int) std::lround(2 * P), hi = t - (int) std::lround(P / 2);
        double best = -1e300; int arg = -1;
        for (int tau = std::max(0, lo); tau <= hi && tau < t; ++tau) {
            const double d = std::log((t - tau) / P);
            const double s = score[(size_t) tau] - tightness * d * d;
            if (s > best) { best = s; arg = tau; }
        }
        score[(size_t) t] = o[(size_t) t] + (arg >= 0 ? best : 0.0);
        back[(size_t) t] = arg;
    }
    const int P = (int) std::lround(period.empty() ? 1 : period.back());
    int t = n - 1; double bestS = -1e300;
    for (int i = std::max(0, n - P); i < n; ++i) if (score[(size_t) i] > bestS) { bestS = score[(size_t) i]; t = i; }
    std::vector<int> beats;
    while (t >= 0) { beats.push_back(t); t = back[(size_t) t]; }
    std::reverse(beats.begin(), beats.end());
    return beats;
}

struct Grid { double t0 = 0; double T = 0; };
// First grid time >= -T/2 (a downbeat at t = -0.01 s must not be lost).
inline double gridStart(double t0, double T) { return t0 - std::floor((t0 + 0.5 * T) / T) * T; }

// Robust straight-line fit of DP beats, then a fine period/phase search
// maximizing envelope energy on the grid.
Grid fitGrid(const std::vector<double>& beatSec, const std::vector<double>& env, double fps, double tStart, double duration, double Tinit) {
    Grid g;
    if (beatSec.size() < 8) return g;
    double T = Tinit, a = beatSec[0];
    // Beat indices from LOCAL intervals (robust to drift and to missed beats).
    std::vector<double> idx(beatSec.size(), 0.0);
    for (size_t i = 1; i < beatSec.size(); ++i)
        idx[i] = idx[i - 1] + std::max(1.0, std::round((beatSec[i] - beatSec[i - 1]) / T));
    std::vector<bool> use(beatSec.size(), true);
    for (int iter = 0; iter < 4; ++iter) {
        double sx = 0, sy = 0, sxx = 0, sxy = 0; int c = 0;
        for (size_t i = 0; i < beatSec.size(); ++i) {
            if (!use[i]) continue;
            const double x = idx[i], y = beatSec[i];
            sx += x; sy += y; sxx += x * x; sxy += x * y; ++c;
        }
        const double den = c * sxx - sx * sx;
        if (c < 4 || std::fabs(den) < 1e-12) break;
        T = (c * sxy - sx * sy) / den;
        a = (sy - T * sx) / c;
        for (size_t i = 0; i < beatSec.size(); ++i) use[i] = std::fabs(beatSec[i] - (a + T * idx[i])) < 0.12 * T;
    }
    if (debugEnabled()) std::fprintf(stderr, "lsq a=%.4f T=%.5f\n", a, T);
    // bring a to the first grid point >= 0
    a = a - std::floor(a / T) * T;
    // Coarse-to-fine search of (period, phase) maximizing onset energy on the
    // grid. The coarse pass scans the FULL phase range: the regression
    // intercept can be biased by DP beats placed inside kick-less breakdowns.
    // The period step must be << (peak width / number of beats) or the true
    // period falls between steps.
    double bestS = -1, bestT = T, bestA = a;
    auto score = [&](double Tc, double ph) {
        double s = 0;
        for (double t = ph + std::ceil((tStart - ph) / Tc) * Tc; t < duration; t += Tc) s += sampleAt(env, fps, t);
        return s;
    };
    const double T0 = T;
    for (int di = -120; di <= 120; ++di) {
        const double Tc = T0 * (1.0 + di * 0.0001);          // +/-1.2 %
        for (int pi = 0; pi < 100; ++pi) {
            const double ph = Tc * pi / 100.0;
            const double s = score(Tc, ph);
            if (s > bestS) { bestS = s; bestT = Tc; bestA = ph; }
        }
    }
    const double T1 = bestT, A1 = bestA;
    for (int di = -20; di <= 20; ++di)
        for (int pi = -10; pi <= 10; ++pi) {
            const double Tc = T1 * (1.0 + di * 0.00001), ph = A1 + T1 * pi * 0.001;
            const double s = score(Tc, ph - std::floor(ph / Tc) * Tc);
            if (s > bestS) { bestS = s; bestT = Tc; bestA = ph; }
        }
    g.T = bestT; g.t0 = bestA - std::floor(bestA / bestT) * bestT;
    return g;
}

std::array<double, 12> meanChroma(const std::vector<std::array<float, 12>>& ch, double fps, double t0, double t1) {
    std::array<double, 12> m{};
    const int a = std::max(0, (int) std::floor(t0 * fps)), b = std::min((int) ch.size(), (int) std::ceil(t1 * fps));
    for (int f = a; f < b; ++f) for (int k = 0; k < 12; ++k) m[(size_t) k] += ch[(size_t) f][(size_t) k];
    double n = 0; for (double v : m) n += v * v;
    n = std::sqrt(n);
    if (n > 1e-12) for (auto& v : m) v /= n;
    return m;
}

double cosDist(const std::array<double, 12>& a, const std::array<double, 12>& b) {
    double s = 0; for (int k = 0; k < 12; ++k) s += a[(size_t) k] * b[(size_t) k];
    return 1.0 - s;
}

std::vector<double> zscore(std::vector<double> v) {
    const double m = mean(v), s = stddev(v);
    for (auto& x : v) x = s > 1e-9 ? (x - m) / s : 0;
    return v;
}

// Chooses which beat (mod beatsPerBar) is the downbeat.
int chooseDownbeatPhase(const std::vector<double>& beats, const FrameFeatures& ff, const std::vector<double>& onset,
                        double onsetFps, const BeatActivations* neural, double* confidence) {
    const int n = (int) beats.size();
    const int bpb = 4;
    if (n < 16) { if (confidence) *confidence = 0; return 0; }
    std::vector<double> cChroma((size_t) n, 0), cBass((size_t) n, 0), cTimbre((size_t) n, 0), cAccent((size_t) n, 0), cNeural((size_t) n, 0);
    auto timbreMean = [&](double t0, double t1) {
        std::array<double, 13> m{}; int c = 0;
        const int a = std::max(0, (int) std::floor(t0 * ff.fpsB)), b = std::min(ff.framesB(), (int) std::ceil(t1 * ff.fpsB));
        for (int f = a; f < b; ++f) { for (int k = 1; k < 13; ++k) m[(size_t) k] += ff.mfcc[(size_t) f][(size_t) k]; ++c; }
        if (c) for (auto& v : m) v /= c;
        return m;
    };
    for (int k = 2; k + 2 < n; ++k) {
        cChroma[(size_t) k] = cosDist(meanChroma(ff.chroma, ff.fpsC, beats[(size_t) k - 2], beats[(size_t) k]),
                                      meanChroma(ff.chroma, ff.fpsC, beats[(size_t) k], beats[(size_t) k + 2]));
        cBass[(size_t) k] = cosDist(meanChroma(ff.bassChroma, ff.fpsC, beats[(size_t) k - 2], beats[(size_t) k]),
                                    meanChroma(ff.bassChroma, ff.fpsC, beats[(size_t) k], beats[(size_t) k + 2]));
        cAccent[(size_t) k] = sampleAt(onset, onsetFps, beats[(size_t) k]);
        if (k >= 4 && k + 4 < n) {
            auto a = timbreMean(beats[(size_t) k - 4], beats[(size_t) k]);
            auto b = timbreMean(beats[(size_t) k], beats[(size_t) k + 4]);
            double d = 0; for (int q = 1; q < 13; ++q) d += (a[(size_t) q] - b[(size_t) q]) * (a[(size_t) q] - b[(size_t) q]);
            cTimbre[(size_t) k] = std::sqrt(d);
        }
        if (neural && !neural->downbeat.empty()) {
            const size_t f = (size_t) std::lround(beats[(size_t) k] * neural->fps);
            double m = 0;
            for (size_t j = f > 2 ? f - 2 : 0; j <= f + 2 && j < neural->downbeat.size(); ++j) m = std::max(m, (double) neural->downbeat[j]);
            cNeural[(size_t) k] = m;
        }
    }
    cChroma = zscore(cChroma); cBass = zscore(cBass); cTimbre = zscore(cTimbre); cAccent = zscore(cAccent);
    if (neural) cNeural = zscore(cNeural);
    std::vector<double> cue((size_t) n);
    for (int k = 0; k < n; ++k)
        cue[(size_t) k] = 1.0 * cChroma[(size_t) k] + 1.0 * cBass[(size_t) k] + 1.0 * cTimbre[(size_t) k]
                        + 0.3 * cAccent[(size_t) k] + 3.0 * cNeural[(size_t) k];
    std::array<double, 4> S{}; std::array<int, 4> C{};
    for (int k = 2; k + 2 < n; ++k) { S[(size_t) (k % bpb)] += cue[(size_t) k]; C[(size_t) (k % bpb)]++; }
    for (int p = 0; p < bpb; ++p) S[(size_t) p] /= std::max(1, C[(size_t) p]);
    std::array<int, 4> order{0, 1, 2, 3};
    std::sort(order.begin(), order.end(), [&](int a, int b) { return S[(size_t) a] > S[(size_t) b]; });
    const double se = stddev(cue) / std::sqrt(std::max(1.0, n / 4.0));
    if (confidence) *confidence = ramp((S[(size_t) order[0]] - S[(size_t) order[1]]) / std::max(1e-9, se), 1.0, 6.0);
    return order[0];
}

// Piecewise-constant tempo (tempo changes in DJ edits / live-played intros):
// group local tempo windows into constant-tempo regions, fit an exact grid in
// each region (away from the change), then switch grids at the beat where
// both grids agree best.
std::vector<double> piecewiseGrid(const std::vector<double>& dpSec, const std::vector<double>& env, double fps,
                                  double duration, const std::vector<double>& localBpm, const std::vector<double>& centers) {
    struct Region { double t0, t1, bpm; };
    std::vector<Region> regions;
    for (size_t i = 0; i < localBpm.size(); ++i) {
        if (!regions.empty() && std::fabs(localBpm[i] - regions.back().bpm) / regions.back().bpm < 0.008) {
            regions.back().t1 = centers[i];
        } else regions.push_back({centers[i], centers[i], localBpm[i]});
    }
    // drop tiny regions (single noisy windows) by merging into neighbours
    std::vector<Region> clean;
    for (auto& r : regions) {
        if (!clean.empty() && r.t1 - r.t0 < 10.0) { clean.back().t1 = r.t1; continue; }
        clean.push_back(r);
    }
    if (clean.size() < 2) return {};
    std::vector<double> out;
    Grid prev{};
    double prevEnd = 0;
    for (size_t k = 0; k < clean.size(); ++k) {
        const double a = k == 0 ? 0.0 : clean[k].t0, b = k + 1 == clean.size() ? duration : clean[k].t1;
        std::vector<double> sub;
        for (double t : dpSec) if (t >= a && t <= b) sub.push_back(t);
        const Grid g = fitGrid(sub, env, fps, a, b, 60.0 / clean[k].bpm);
        if (g.T <= 0) return {};
        if (k == 0) {
            for (double t = gridStart(g.t0, g.T); t < (clean.size() > 1 ? clean[1].t0 : duration); t += g.T) out.push_back(t);
        } else {
            // transition zone between previous region end and this region start
            const double z0 = prevEnd - 10.0, z1 = clean[k].t0 + 10.0;
            double bestD = 1e9, switchA = 0, switchB = 0;
            for (double ta = prev.t0 + std::ceil((z0 - prev.t0) / prev.T) * prev.T; ta < z1; ta += prev.T) {
                const double tb = g.t0 + std::round((ta - g.t0) / g.T) * g.T;
                if (std::fabs(ta - tb) < bestD) { bestD = std::fabs(ta - tb); switchA = ta; switchB = tb; }
            }
            while (!out.empty() && out.back() > switchA - 0.25 * prev.T) out.pop_back();
            for (double t = prev.t0 + std::ceil((out.empty() ? 0.0 : out.back() + 0.5 * prev.T - prev.t0) / prev.T) * prev.T;
                 t < switchA - 0.25 * prev.T; t += prev.T) out.push_back(t);
            const double end = k + 1 < clean.size() ? clean[k + 1].t0 : duration + g.T;
            for (double t = switchB; t < end; t += g.T) out.push_back(t);
        }
        prev = g; prevEnd = b;
    }
    return out;
}

} // namespace

double estimateTempoBpm(const std::vector<float>& env, double fps, double* strength) {
    const auto o = normalizeEnvelope(env, fps);
    const double P = estimatePeriod(o, fps, 0, (int) o.size(), 60, 200, strength);
    return P > 0 ? 60.0 * fps / P : 0;
}

RhythmResult analyzeRhythm(const FrameFeatures& ff, const RhythmOptions& opt) {
    RhythmResult r;
    const bool useNeural = opt.neural && opt.neural->fps > 0 && !opt.neural->beat.empty();
    const double fps = useNeural ? opt.neural->fps : ff.fpsA;
    std::vector<double> env;
    if (useNeural) env.assign(opt.neural->beat.begin(), opt.neural->beat.end());
    else {
        // Broadband flux + low-band flux: the low band anchors the grid on the
        // kick instead of off-beat hats (typical EDM failure mode).
        const auto a = normalizeEnvelope(ff.flux, ff.fpsA), b = normalizeEnvelope(ff.fluxLow, ff.fpsA);
        env.resize(a.size());
        for (size_t i = 0; i < a.size(); ++i) env[i] = 0.25 * a[i] + b[i];
    }
    const auto envBlur = blur(env);
    const auto onsetForCues = normalizeEnvelope(ff.flux, ff.fpsA);
    const double duration = opt.durationSec;
    const int n = (int) env.size();

    std::vector<double> beatSec;
    bool stable = true;
    std::vector<double> localBpm;

    if (opt.manualBpm > 0) {
        const double T = 60.0 / opt.manualBpm;
        double t0;
        if (opt.manualFirstDownbeatSec >= 0) t0 = opt.manualFirstDownbeatSec - std::floor(opt.manualFirstDownbeatSec / T) * T;
        else {
            double best = -1; t0 = 0;
            for (int i = 0; i < 200; ++i) {
                const double a = T * i / 200.0; double s = 0;
                for (double t = a; t < duration; t += T) s += sampleAt(envBlur, fps, t);
                if (s > best) { best = s; t0 = a; }
            }
        }
        for (double t = gridStart(t0, T); t < duration + T; t += T) beatSec.push_back(t);
    } else {
        double strength = 0;
        const double P = estimatePeriod(env, fps, 0, n, 60, 200, &strength);
        if (P <= 0 || strength < 0.02) {
            r.failure = "No stable pulse was found in this audio.";
            return r;
        }
        // local tempo in 20 s windows (hop 5 s), restricted to +/-8 % of global
        const double gBpm = 60.0 * fps / P;
        const int win = (int) (20 * fps), hop = (int) (5 * fps);
        std::vector<double> centers;
        for (int s = 0; s + win / 2 < n; s += hop) {
            double st = 0;
            const double p = estimatePeriod(env, fps, s, std::min(n, s + win), gBpm * 0.92, gBpm * 1.08, &st);
            if (p > 0 && st > 0.05) { localBpm.push_back(60.0 * fps / p); centers.push_back((s + win / 2.0) / fps); }
        }
        if (localBpm.size() >= 3) {
            const double spread = (percentile(localBpm, 90) - percentile(localBpm, 10)) / median(localBpm);
            stable = spread < 0.015;
        }
        std::vector<double> period((size_t) n, P);
        if (!stable) {
            for (int t = 0; t < n; ++t) {
                const double ts = t / fps;
                size_t j = 0; while (j + 1 < centers.size() && centers[j + 1] < ts) ++j;
                double bpm = localBpm[j];
                if (j + 1 < centers.size() && ts > centers[j]) {
                    const double f = (ts - centers[j]) / (centers[j + 1] - centers[j]);
                    bpm = localBpm[j] * (1 - f) + localBpm[j + 1] * f;
                }
                period[(size_t) t] = 60.0 * fps / bpm;
            }
            r.warnings.push_back("Tempo is not constant; a variable beat grid was used.");
        }
        const auto dpBeats = trackBeatsDP(env, period, 100.0);
        std::vector<double> dpSec; for (int f : dpBeats) dpSec.push_back(f / fps);
        if (stable) {
            const Grid g = fitGrid(dpSec, envBlur, fps, 0.0, duration, P / fps);
            if (debugEnabled()) {
                std::fprintf(stderr, "P=%.3f dp=%zu first:", P, dpSec.size());
                for (size_t i = 0; i < std::min<size_t>(8, dpSec.size()); ++i) std::fprintf(stderr, " %.3f", dpSec[i]);
                std::fprintf(stderr, "\ngrid t0=%.4f T=%.5f (bpm %.3f)\n", g.t0, g.T, 60 / g.T);
            }
            if (g.T <= 0) { r.failure = "Beat grid could not be established."; return r; }
            double t0 = g.t0;
            // On-beat vs off-beat: in EDM the off-beat often carries hats AND bass,
            // so broadband flux can prefer it. The beat is where the strongest
            // low-frequency transients (kick) are.
            if (!useNeural) {
                const auto low = blur(normalizeEnvelope(ff.fluxLow, ff.fpsA));
                double s0 = 0, s1 = 0;
                for (double t = g.t0; t < duration; t += g.T) { s0 += sampleAt(low, ff.fpsA, t); s1 += sampleAt(low, ff.fpsA, t + g.T / 2); }
                if (s1 > 1.1 * s0) t0 = g.t0 + g.T / 2 - (g.t0 + g.T / 2 >= g.T ? g.T : 0);
            }
            for (double t = gridStart(t0, g.T); t < duration + g.T; t += g.T) beatSec.push_back(t);
        } else {
            beatSec = piecewiseGrid(dpSec, envBlur, fps, duration, localBpm, centers);
            if (beatSec.size() < 16) beatSec = dpSec;
        }
        if (!useNeural) for (auto& t : beatSec) t -= kFluxLatencySec;
    }
    if (beatSec.size() < 16) { r.failure = "Too few beats were detected."; return r; }

    // --- beat confidence: how much onset energy sits on the grid
    {
        std::vector<double> on;
        for (double t : beatSec) if (t >= opt.audioStartSec && t < duration) on.push_back(sampleAt(envBlur, fps, t));
        const double ratio = mean(on) / std::max(1e-9, mean(envBlur));
        r.tempo.beatConfidence = ramp(ratio, 1.2, 2.5) * (stable ? 1.0 : 0.8);
        if (debugEnabled()) std::fprintf(stderr, "beat ratio %.3f (n=%zu) stable=%d\n", ratio, on.size(), (int) stable);
        if (opt.manualBpm > 0) r.tempo.beatConfidence = std::max(r.tempo.beatConfidence, 0.5);
    }
    if (r.tempo.beatConfidence < 0.2 && opt.manualBpm <= 0) {
        r.failure = "The beat could not be detected reliably.";
        return r;
    }

    // --- downbeat phase
    double dbConf = 0;
    int phase = 0;
    if (opt.manualFirstDownbeatSec >= 0) {
        size_t best = 0;
        for (size_t i = 0; i < beatSec.size(); ++i)
            if (std::fabs(beatSec[i] - opt.manualFirstDownbeatSec) < std::fabs(beatSec[best] - opt.manualFirstDownbeatSec)) best = i;
        phase = (int) (best % 4); dbConf = 1.0;
    } else {
        phase = chooseDownbeatPhase(beatSec, ff, onsetForCues, ff.fpsA, useNeural ? opt.neural : nullptr, &dbConf);
    }
    r.tempo.downbeatConfidence = dbConf;
    if (dbConf < 0.3) r.warnings.push_back("Bar positions (downbeats) are uncertain.");

    // --- bar 1 = first downbeat at/after the start of the music
    const double T0 = beatSec.size() > 1 ? beatSec[1] - beatSec[0] : 0.5;
    size_t first = (size_t) phase;
    while (first + 4 < beatSec.size() && beatSec[first] < opt.audioStartSec - 0.5 * T0) first += 4;
    std::vector<double> beats(beatSec.begin() + (long) first, beatSec.end());
    // extend to cover the audio and complete the last bar
    while (beats.size() < 2 || beats.back() < duration || beats.size() % 4 != 0) {
        const double step = beats.size() >= 2 ? beats[beats.size() - 1] - beats[beats.size() - 2] : T0;
        beats.push_back(beats.back() + step);
    }
    // Trim bars that start after the music has ended (release tails, silence):
    // keep bars whose start lies before (musicEnd - 1/4 bar).
    {
        const double musicEnd = opt.audioEndSec > 0 ? opt.audioEndSec : duration;
        int bars = (int) beats.size() / 4;
        while (bars > 1) {
            const double start = beats[(size_t) (bars - 1) * 4];
            const double barLen = 4 * (beats[1] - beats[0]);
            if (start < musicEnd - 0.25 * barLen) break;
            --bars;
        }
        while (beats.size() < (size_t) bars * 4 + 1) beats.push_back(beats.back() + (beats.back() - beats[beats.size() - 2]));
        beats.resize((size_t) bars * 4 + 1);
    }
    r.tempo.beatTimes = beats;
    r.tempo.beatsPerBar = 4;
    r.tempo.stable = stable;

    // --- tempo segments (per bar median, merge within 1 %)
    std::vector<double> ibi;
    for (size_t i = 1; i < beats.size(); ++i) ibi.push_back(beats[i] - beats[i - 1]);
    r.tempo.nominalBpm = 60.0 / median(ibi);
    if (stable) r.tempo.segments.push_back({0, r.tempo.nominalBpm});
    else {
        for (size_t b = 0; b + 4 <= ibi.size(); b += 4) {
            std::vector<double> bar(ibi.begin() + (long) b, ibi.begin() + (long) b + 4);
            const double bpm = 60.0 / median(bar);
            if (r.tempo.segments.empty() || std::fabs(bpm - r.tempo.segments.back().bpm) / r.tempo.segments.back().bpm > 0.01)
                r.tempo.segments.push_back({(int) b, bpm});
        }
    }
    r.ok = true;
    return r;
}

} // namespace dali::pipeline
