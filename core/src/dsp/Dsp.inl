#pragma once
#include <cmath>

namespace dali::dsp {

inline int frameCount(size_t numSamples, int hop) {
    return (int) (numSamples / (size_t) hop) + 1;
}

template <typename Fn>
int forEachFrame(const std::vector<float>& x, const StftConfig& cfg, Fn&& fn) {
    const int n = cfg.fftSize;
    const int half = n / 2;
    const int frames = frameCount(x.size(), cfg.hop);
    std::vector<float> win((size_t) n), frame((size_t) n), mag((size_t) half + 1);
    for (int i = 0; i < n; ++i) win[(size_t) i] = (float) (0.5 - 0.5 * std::cos(2.0 * kPi * i / n));
    RealFft fft(n);
    const long len = (long) x.size();
    for (int f = 0; f < frames; ++f) {
        const long start = (long) f * cfg.hop - half;   // centered frames, zero padded
        for (int i = 0; i < n; ++i) {
            const long idx = start + i;
            frame[(size_t) i] = (idx >= 0 && idx < len) ? x[(size_t) idx] * win[(size_t) i] : 0.0f;
        }
        fft.magnitude(frame.data(), mag.data());
        fn(f, mag.data());
    }
    return frames;
}

} // namespace dali::dsp
