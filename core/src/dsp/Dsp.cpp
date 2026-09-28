#include "Dsp.h"
#include <algorithm>
#include <cassert>
#include <numeric>
#include <cstdlib>

namespace dali::dsp {

bool debugEnabled() {
    static const bool on = [] {
#ifdef _MSC_VER
        char* v = nullptr; size_t n = 0;
        const bool r = _dupenv_s(&v, &n, "DALI_DEBUG") == 0 && v != nullptr;
        std::free(v);
        return r;
#else
        return std::getenv("DALI_DEBUG") != nullptr;
#endif
    }();
    return on;
}

// --------------------------------------------------------------- FFT
RealFft::RealFft(int size) : n_(size), tw_((size_t) size / 2), rev_((size_t) size), buf_((size_t) size) {
    assert(size >= 2 && (size & (size - 1)) == 0);
    for (int i = 0; i < size / 2; ++i) tw_[(size_t) i] = std::polar(1.0, -2.0 * kPi * i / size);
    int bits = 0; while ((1 << bits) < size) ++bits;
    for (int i = 0; i < size; ++i) {
        int r = 0; for (int b = 0; b < bits; ++b) if (i & (1 << b)) r |= 1 << (bits - 1 - b);
        rev_[(size_t) i] = r;
    }
}

void RealFft::transform(std::vector<std::complex<double>>& a) const {
    const int n = n_;
    for (int i = 0; i < n; ++i) if (i < rev_[(size_t) i]) std::swap(a[(size_t) i], a[(size_t) rev_[(size_t) i]]);
    for (int len = 2; len <= n; len <<= 1) {
        const int step = n / len, halfLen = len / 2;
        for (int i = 0; i < n; i += len)
            for (int j = 0; j < halfLen; ++j) {
                auto u = a[(size_t) (i + j)];
                auto v = a[(size_t) (i + j + halfLen)] * tw_[(size_t) (j * step)];
                a[(size_t) (i + j)] = u + v;
                a[(size_t) (i + j + halfLen)] = u - v;
            }
    }
}

void RealFft::magnitude(const float* in, float* out) {
    for (int i = 0; i < n_; ++i) buf_[(size_t) i] = {in[i], 0.0};
    transform(buf_);
    for (int k = 0; k <= n_ / 2; ++k) out[k] = (float) std::abs(buf_[(size_t) k]);
}

// --------------------------------------------------------------- resampling
std::vector<float> resample(const std::vector<float>& in, double srcRate, double dstRate) {
    if (in.empty() || srcRate <= 0 || dstRate <= 0) return {};
    if (std::fabs(srcRate - dstRate) < 1e-6) return in;
    const double ratio = dstRate / srcRate;
    const double cutoff = std::min(1.0, ratio) * 0.95;      // relative to src Nyquist
    const int zeroCrossings = 16;
    const double halfWidth = zeroCrossings / cutoff;         // in src samples
    const size_t outLen = (size_t) std::floor((double) in.size() * ratio);
    std::vector<float> out(outLen);
    const long len = (long) in.size();
    // Pre-tabulated windowed-sinc kernel (512 points per source sample, linear interpolation).
    const int res = 512;
    const int tableLen = (int) std::ceil(halfWidth * res) + 2;
    std::vector<float> table((size_t) tableLen);
    for (int i = 0; i < tableLen; ++i) {
        const double d = (double) i / res;
        if (d >= halfWidth) { table[(size_t) i] = 0; continue; }
        const double xw = d / halfWidth;
        const double w = 0.42 + 0.5 * std::cos(kPi * xw) + 0.08 * std::cos(2 * kPi * xw);   // Blackman
        const double arg = kPi * cutoff * d;
        const double s = std::fabs(arg) < 1e-9 ? 1.0 : std::sin(arg) / arg;
        table[(size_t) i] = (float) (cutoff * s * w);
    }
    auto kernel = [&](double d) {
        const double p = std::fabs(d) * res;
        const size_t i = (size_t) p;
        if (i + 1 >= table.size()) return 0.0f;
        const float f = (float) (p - (double) i);
        return table[i] * (1 - f) + table[i + 1] * f;
    };
    for (size_t j = 0; j < outLen; ++j) {
        const double t = (double) j / ratio;
        const long k0 = (long) std::ceil(t - halfWidth), k1 = (long) std::floor(t + halfWidth);
        double acc = 0, norm = 0;
        for (long k = std::max(0L, k0); k <= std::min(len - 1, k1); ++k) {
            const double h = kernel(t - (double) k);
            acc += in[(size_t) k] * h; norm += h;
        }
        // Normalising by the local tap sum keeps DC gain exactly 1 at the edges.
        out[j] = (float) (norm > 1e-9 ? acc / norm : 0.0);
    }
    return out;
}

// --------------------------------------------------------------- mel
double hzToMel(double hz) { return 2595.0 * std::log10(1.0 + hz / 700.0); }
double melToHz(double mel) { return 700.0 * (std::pow(10.0, mel / 2595.0) - 1.0); }

MelBank::MelBank(int numBands, int fftSize, double sr, double fMin, double fMax) {
    const int bins = fftSize / 2 + 1;
    const double binHz = sr / fftSize;
    const double m0 = hzToMel(fMin), m1 = hzToMel(fMax);
    std::vector<double> pts((size_t) numBands + 2);
    for (int i = 0; i < numBands + 2; ++i) pts[(size_t) i] = melToHz(m0 + (m1 - m0) * i / (numBands + 1));
    for (int b = 0; b < numBands; ++b) {
        const double lo = pts[(size_t) b], c = pts[(size_t) b + 1], hi = pts[(size_t) b + 2];
        centers.push_back(c);
        Filter f;
        f.lo = std::max(0, (int) std::floor(lo / binHz));
        f.hi = std::min(bins - 1, (int) std::ceil(hi / binHz));
        for (int k = f.lo; k <= f.hi; ++k) {
            const double hz = k * binHz;
            double w = 0;
            if (hz >= lo && hz <= c) w = (hz - lo) / std::max(1e-9, c - lo);
            else if (hz > c && hz <= hi) w = (hi - hz) / std::max(1e-9, hi - c);
            f.w.push_back((float) w);
        }
        // Guarantee narrow low bands still receive at least their nearest bin.
        double sum = 0; for (float w : f.w) sum += w;
        if (sum < 1e-6) { f.lo = f.hi = std::min(bins - 1, (int) std::lround(c / binHz)); f.w.assign(1, 1.0f); }
        filters_.push_back(std::move(f));
    }
}

void MelBank::apply(const float* mag, float* out) const {
    for (size_t b = 0; b < filters_.size(); ++b) {
        const auto& f = filters_[b];
        double acc = 0;
        for (int k = f.lo; k <= f.hi; ++k) { const double m = mag[k]; acc += f.w[(size_t) (k - f.lo)] * m * m; }
        out[b] = (float) acc;
    }
}

void dct(const float* in, int n, float* out, int k) {
    for (int i = 0; i < k; ++i) {
        double acc = 0;
        for (int j = 0; j < n; ++j) acc += in[j] * std::cos(kPi * i * (j + 0.5) / n);
        out[i] = (float) (acc * std::sqrt((i == 0 ? 1.0 : 2.0) / n));
    }
}

// --------------------------------------------------------------- stats
double percentile(std::vector<double> v, double p) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    const double pos = std::clamp(p, 0.0, 100.0) / 100.0 * (double) (v.size() - 1);
    const size_t i = (size_t) std::floor(pos);
    const double f = pos - (double) i;
    return i + 1 < v.size() ? v[i] * (1 - f) + v[i + 1] * f : v[i];
}
double median(std::vector<double> v) { return percentile(std::move(v), 50); }
double mean(const std::vector<double>& v) {
    return v.empty() ? 0 : std::accumulate(v.begin(), v.end(), 0.0) / (double) v.size();
}
double stddev(const std::vector<double>& v) {
    if (v.size() < 2) return 0;
    const double m = mean(v); double s = 0;
    for (double x : v) s += (x - m) * (x - m);
    return std::sqrt(s / (double) (v.size() - 1));
}
double pearson(const std::vector<double>& a, const std::vector<double>& b) {
    const size_t n = std::min(a.size(), b.size());
    if (n < 2) return 0;
    double ma = 0, mb = 0;
    for (size_t i = 0; i < n; ++i) { ma += a[i]; mb += b[i]; }
    ma /= (double) n; mb /= (double) n;
    double sab = 0, saa = 0, sbb = 0;
    for (size_t i = 0; i < n; ++i) { sab += (a[i] - ma) * (b[i] - mb); saa += (a[i] - ma) * (a[i] - ma); sbb += (b[i] - mb) * (b[i] - mb); }
    return (saa > 1e-12 && sbb > 1e-12) ? sab / std::sqrt(saa * sbb) : 0;
}
std::vector<double> movingAverage(const std::vector<double>& v, int r) {
    std::vector<double> out(v.size());
    const int n = (int) v.size();
    for (int i = 0; i < n; ++i) {
        double s = 0; int c = 0;
        for (int j = std::max(0, i - r); j <= std::min(n - 1, i + r); ++j) { s += v[(size_t) j]; ++c; }
        out[(size_t) i] = c ? s / c : 0;
    }
    return out;
}
double toDb(double power, double floorDb) {
    return power > 0 ? std::max(floorDb, 10.0 * std::log10(power)) : floorDb;
}

} // namespace dali::dsp
