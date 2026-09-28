#include "Pipeline.h"
#include "../dsp/Dsp.h"
#include <algorithm>
#include <cmath>

namespace dali::pipeline {
using namespace dali::dsp;

static const double kBandEdges[kNumBands + 1] = {20, 60, 150, 500, 2000, 5000, 11025};

static int bandOf(double hz) {
    for (int b = 0; b < kNumBands; ++b) if (hz < kBandEdges[b + 1]) return b;
    return kNumBands - 1;
}

// Rhythm pass (A-rate): log-mel flux in three regions + low/high energy.
// The log floor is RELATIVE to the track (loudest band level - 60 dB) so
// that digital silence / fades do not create huge fake onsets.
static void rhythmPass(const std::vector<float>& x, std::vector<float>& flux, std::vector<float>& fLow,
                       std::vector<float>& fHigh, std::vector<float>* lowDb, std::vector<float>* highDb) {
    const int nMel = 64;
    MelBank mel(nMel, 2048, kAnalysisRate, 30.0, 11000.0);
    std::vector<int> region;   // 0 low, 1 mid, 2 high
    for (double c : mel.centers) region.push_back(c < 150 ? 0 : (c > 5000 ? 2 : 1));
    StftConfig cfg{2048, 512, kAnalysisRate};
    const int n = frameCount(x.size(), cfg.hop);
    std::vector<float> P((size_t) n * nMel);
    forEachFrame(x, cfg, [&](int f, const float* mag) { mel.apply(mag, &P[(size_t) f * nMel]); });

    float maxP = 0; for (float v : P) maxP = std::max(maxP, v);
    const double floorDb = toDb(maxP + 1e-20, -200) - 60.0;
    flux.assign((size_t) n, 0); fLow.assign((size_t) n, 0); fHigh.assign((size_t) n, 0);
    if (lowDb) lowDb->assign((size_t) n, 0);
    if (highDb) highDb->assign((size_t) n, 0);
    std::vector<double> prev((size_t) nMel, floorDb), cur((size_t) nMel);
    for (int f = 0; f < n; ++f) {
        const float* row = &P[(size_t) f * nMel];
        double fa = 0, fl = 0, fh = 0, pl = 0, ph = 0;
        for (int b = 0; b < nMel; ++b) {
            cur[(size_t) b] = std::max(floorDb, toDb(row[b] + 1e-20, -200));
            const double d = f > 0 ? std::max(0.0, cur[(size_t) b] - prev[(size_t) b]) : 0.0;
            fa += d;
            if (region[(size_t) b] == 0) { fl += d; pl += row[b]; }
            if (region[(size_t) b] == 2) { fh += d; ph += row[b]; }
        }
        prev = cur;
        flux[(size_t) f] = (float) fa; fLow[(size_t) f] = (float) fl; fHigh[(size_t) f] = (float) fh;
        if (lowDb) (*lowDb)[(size_t) f] = (float) std::max(floorDb, toDb(pl + 1e-20, -200));
        if (highDb) (*highDb)[(size_t) f] = (float) std::max(floorDb, toDb(ph + 1e-20, -200));
    }
}

FrameFeatures extractFrameFeatures(const std::vector<float>& x, ProgressSink* p, float p0, float p1) {
    FrameFeatures ff;
    auto report = [&](float f, const char* s) { if (p) p->onProgress(p0 + (p1 - p0) * f, s); };

    // ---------------- A: rhythm
    report(0.0f, "Extracting rhythm features");
    ff.fpsA = kAnalysisRate / 512.0;
    rhythmPass(x, ff.flux, ff.fluxLow, ff.fluxHigh, &ff.lowDb, &ff.highDb);

    // ---------------- B: timbre + HPSS on a 128-band mel power spectrogram
    report(0.35f, "Extracting timbre features");
    ff.fpsB = kAnalysisRate / 1024.0;
    const int nMel = 128;
    MelBank mel(nMel, 2048, kAnalysisRate, 20.0, 11025.0);
    std::vector<int> melBand; for (double c : mel.centers) melBand.push_back(bandOf(c));
    StftConfig cfgB{2048, 1024, kAnalysisRate};
    const int nB = frameCount(x.size(), cfgB.hop);
    std::vector<float> S((size_t) nB * nMel);
    ff.centroid.resize((size_t) nB); ff.flatness.resize((size_t) nB);
    ff.mfcc.resize((size_t) nB); ff.band.resize((size_t) nB);
    const double binHz = kAnalysisRate / 2048.0;
    std::vector<float> logMel((size_t) nMel);
    forEachFrame(x, cfgB, [&](int f, const float* mag) {
        float* row = &S[(size_t) f * nMel];
        mel.apply(mag, row);
        double num = 0, den = 0, logSum = 0, pSum = 0; int cnt = 0;
        for (int k = 1; k <= 1024; ++k) {
            const double m = mag[k], pw = m * m + 1e-12;
            num += k * binHz * m; den += m;
            logSum += std::log(pw); pSum += pw; ++cnt;
        }
        ff.centroid[(size_t) f] = (float) (den > 1e-9 ? num / den : 0);
        ff.flatness[(size_t) f] = (float) (std::exp(logSum / cnt) / (pSum / cnt));
        for (int b = 0; b < nMel; ++b) logMel[(size_t) b] = (float) toDb(row[b] + 1e-10, -100);
        dct(logMel.data(), nMel, ff.mfcc[(size_t) f].data(), 13);
        auto& bd = ff.band[(size_t) f]; bd.fill(0);
        for (int b = 0; b < nMel; ++b) bd[(size_t) melBand[(size_t) b]] += row[b];
    });

    report(0.6f, "Separating harmonic / percussive");
    ff.bandH.assign((size_t) nB, {}); ff.bandP.assign((size_t) nB, {});
    const int tR = 8, fR = 4;
    std::vector<float> tmp;
    for (int f = 0; f < nB; ++f) {
        auto& h = ff.bandH[(size_t) f]; auto& pp = ff.bandP[(size_t) f];
        h.fill(0); pp.fill(0);
        for (int b = 0; b < nMel; ++b) {
            tmp.clear();
            for (int t = std::max(0, f - tR); t <= std::min(nB - 1, f + tR); ++t) tmp.push_back(S[(size_t) t * nMel + b]);
            std::nth_element(tmp.begin(), tmp.begin() + (long) tmp.size() / 2, tmp.end());
            const double hm = tmp[tmp.size() / 2];
            tmp.clear();
            for (int k = std::max(0, b - fR); k <= std::min(nMel - 1, b + fR); ++k) tmp.push_back(S[(size_t) f * nMel + k]);
            std::nth_element(tmp.begin(), tmp.begin() + (long) tmp.size() / 2, tmp.end());
            const double pm = tmp[tmp.size() / 2];
            const double s = S[(size_t) f * nMel + b];
            const double hw = hm * hm, pw = pm * pm, tot = hw + pw;
            const double mh = tot > 1e-30 ? hw / tot : 0.5;
            h[(size_t) melBand[(size_t) b]] += (float) (s * mh);
            pp[(size_t) melBand[(size_t) b]] += (float) (s * (1 - mh));
        }
    }

    // ---------------- C: chroma
    report(0.8f, "Extracting harmonic content");
    ff.fpsC = kAnalysisRate / 2048.0;
    StftConfig cfgC{8192, 2048, kAnalysisRate};
    const double binHzC = kAnalysisRate / 8192.0;
    std::vector<int> pc(4097, -1);
    std::vector<bool> isBass(4097, false);
    for (int k = 1; k <= 4096; ++k) {
        const double hz = k * binHzC;
        if (hz < 40 || hz > 5000) continue;
        const double midi = 69.0 + 12.0 * std::log2(hz / 440.0);
        pc[(size_t) k] = ((int) std::lround(midi) % 12 + 12) % 12;
        isBass[(size_t) k] = hz <= 250;
    }
    const int nC = frameCount(x.size(), cfgC.hop);
    ff.chroma.assign((size_t) nC, {}); ff.bassChroma.assign((size_t) nC, {});
    forEachFrame(x, cfgC, [&](int f, const float* mag) {
        auto& c = ff.chroma[(size_t) f]; auto& bc = ff.bassChroma[(size_t) f];
        c.fill(0); bc.fill(0);
        for (int k = 1; k <= 4096; ++k) {
            const int q = pc[(size_t) k];
            if (q < 0) continue;
            const float e = mag[k] * mag[k];
            if (isBass[(size_t) k]) bc[(size_t) q] += e;
            if (k * binHzC >= 80) c[(size_t) q] += e;
        }
    });
    report(1.0f, "Features ready");
    return ff;
}

StemFeatures extractStemFeatures(const std::vector<float>& x) {
    StemFeatures sf;
    if (x.empty()) return sf;
    sf.fpsA = kAnalysisRate / 512.0;
    sf.fpsB = kAnalysisRate / 1024.0;
    std::vector<float> flux;
    rhythmPass(x, flux, sf.fluxLow, sf.fluxHigh, &sf.lowDb, nullptr);
    MelBank mel(64, 2048, kAnalysisRate, 20.0, 11025.0);
    std::vector<int> melBand; for (double c : mel.centers) melBand.push_back(bandOf(c));
    StftConfig cfgB{2048, 1024, kAnalysisRate};
    sf.band.assign((size_t) frameCount(x.size(), cfgB.hop), {});
    std::vector<float> row(64);
    forEachFrame(x, cfgB, [&](int f, const float* mag) {
        mel.apply(mag, row.data());
        auto& bd = sf.band[(size_t) f]; bd.fill(0);
        for (int b = 0; b < 64; ++b) bd[(size_t) melBand[(size_t) b]] += row[(size_t) b];
    });
    return sf;
}

} // namespace dali::pipeline
