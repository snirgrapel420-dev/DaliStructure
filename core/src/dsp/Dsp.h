// Internal DSP building blocks (not part of the public API).
#pragma once
#include <complex>
#include <cstddef>
#include <vector>

namespace dali::dsp {

constexpr double kPi = 3.14159265358979323846;   // portable (no reliance on M_PI / _USE_MATH_DEFINES)

// DALI_DEBUG=1 enables diagnostic output on stderr.
bool debugEnabled();

// --------------------------------------------------------------- FFT
class RealFft {
public:
    explicit RealFft(int size);           // size must be a power of two
    int size() const { return n_; }
    // in: n real samples. out: n/2+1 magnitudes.
    void magnitude(const float* in, float* outMag);
private:
    void transform(std::vector<std::complex<double>>& a) const;
    int n_;
    std::vector<std::complex<double>> tw_;
    std::vector<int> rev_;
    std::vector<std::complex<double>> buf_;
};

// --------------------------------------------------------------- resampling
std::vector<float> resample(const std::vector<float>& in, double srcRate, double dstRate);

// --------------------------------------------------------------- STFT
struct StftConfig {
    int fftSize = 2048;
    int hop = 512;
    double sampleRate = 22050;
};

// Calls fn(frameIndex, magnitudes[fftSize/2+1]) for every centered frame.
// Frame f is centered at time f*hop/sampleRate. Returns number of frames.
template <typename Fn>
int forEachFrame(const std::vector<float>& x, const StftConfig& cfg, Fn&& fn);

int frameCount(size_t numSamples, int hop);

// --------------------------------------------------------------- filterbanks
struct MelBank {
    // Triangular mel filters over magnitude bins.
    MelBank(int numBands, int fftSize, double sampleRate, double fMin, double fMax);
    void apply(const float* mag, float* out) const;   // power-domain sum
    int numBands() const { return (int) centers.size(); }
    std::vector<double> centers;                      // Hz
private:
    struct Filter { int lo, hi; std::vector<float> w; };
    std::vector<Filter> filters_;
};

double hzToMel(double hz);
double melToHz(double mel);

// Discrete cosine transform (type II, orthonormal) of `in` -> first `k` coeffs.
void dct(const float* in, int n, float* out, int k);

// --------------------------------------------------------------- stats
double percentile(std::vector<double> v, double p);   // p in [0,100]
double median(std::vector<double> v);
double mean(const std::vector<double>& v);
double stddev(const std::vector<double>& v);
double pearson(const std::vector<double>& a, const std::vector<double>& b);
std::vector<double> movingAverage(const std::vector<double>& v, int radius);
inline double clamp01(double x) { return x < 0 ? 0 : (x > 1 ? 1 : x); }
inline double ramp(double x, double a, double b) { return clamp01((x - a) / (b - a)); }
double toDb(double power, double floorDb = -120);

} // namespace dali::dsp

#include "Dsp.inl"
