#include "Pipeline.h"
#include "../dsp/Dsp.h"
#include <algorithm>
#include <cmath>
#include <set>
#include <cstdio>
#include <cstdlib>

namespace dali::pipeline {
using namespace dali::dsp;

namespace {

Event makeEvent(EventType t, double bar1, Importance imp, double conf, double lengthBars = 0, std::string label = {}) {
    Event e; e.type = t; e.bar = bar1; e.importance = imp; e.confidence = clamp01(conf);
    e.lengthBars = lengthBars; e.label = label.empty() ? displayName(t) : label;
    return e;
}

std::vector<double> slice(const std::vector<double>& v, int a, int b) {
    a = std::max(0, a); b = std::min((int) v.size(), b);
    return a < b ? std::vector<double>(v.begin() + a, v.begin() + b) : std::vector<double>{};
}

std::vector<double> ramp01(size_t n) { std::vector<double> t(n); for (size_t i = 0; i < n; ++i) t[i] = (double) i; return t; }

double deltaByRegression(const std::vector<double>& y) {
    const size_t n = y.size(); if (n < 2) return 0;
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (size_t i = 0; i < n; ++i) { sx += i; sy += y[i]; sxx += (double) i * i; sxy += i * y[i]; }
    const double den = n * sxx - sx * sx;
    return den > 1e-12 ? (n * sxy - sx * sy) / den * (double) (n - 1) : 0;
}

// Hysteresis + minimum run length (2 bars) -> stable on/off states.
std::vector<bool> stableStates(const std::vector<double>& act) {
    const size_t n = act.size();
    std::vector<bool> st(n, false);
    bool cur = n && act[0] >= 0.45;
    for (size_t i = 0; i < n; ++i) {
        if (act[i] >= 0.55) cur = true;
        else if (act[i] <= 0.3) cur = false;
        st[i] = cur;
    }
    // remove runs shorter than 2 bars (not at the very start/end)
    for (size_t i = 0; i < n;) {
        size_t j = i; while (j < n && st[j] == st[i]) ++j;
        if (j - i < 2 && i > 0 && j < n) for (size_t k = i; k < j; ++k) st[k] = !st[k];
        i = j;
    }
    return st;
}

double cosDistance(const double* a, const double* b, int n) {
    double ab = 0, aa = 0, bb = 0;
    for (int i = 0; i < n; ++i) { ab += a[i] * b[i]; aa += a[i] * a[i]; bb += b[i] * b[i]; }
    return (aa > 1e-12 && bb > 1e-12) ? 1.0 - ab / std::sqrt(aa * bb) : 0.0;
}

} // namespace

std::vector<Event> detectFxEvents(const BarFeatures& bf, const TempoMap& tm, const std::vector<Segment>& segs) {
    std::vector<Event> ev;
    const int N = bf.numBars, bpb = tm.beatsPerBar;
    if (N < 4) return ev;
    const double medHigh = median(bf.beatHighDb);
    std::set<int> boundaries;
    for (auto& s : segs) if (s.start > 0) boundaries.insert(s.start);
    // energy jumps also qualify as riser targets
    {
        std::set<int> segB = boundaries;
        for (int b = 2; b < N; ++b) {
            if (bf.barEnergy[(size_t) b] - bf.barEnergy[(size_t) b - 1] <= 20) continue;
            // a section boundary nearby is the real target; do not add a competing one
            bool near = false; for (int d = -2; d <= 2; ++d) near = near || segB.count(b + d);
            if (!near) boundaries.insert(b);
        }
    }

    std::vector<std::pair<int, int>> riserSpans;
    for (int e : boundaries) {
        // --- riser ending at bar e
        for (int L : {16, 8, 4, 2}) {
            if (e - L < 0) continue;
            const auto hi = slice(bf.beatHighDb, (e - L) * bpb, e * bpb);
            const auto cen = slice(bf.beatCentroid, (e - L) * bpb, e * bpb);
            if (hi.size() < 6) continue;
            const auto t = ramp01(hi.size());
            const double cHi = pearson(t, hi), cCen = pearson(t, cen), rise = deltaByRegression(hi);
            const auto firstHalf = slice(hi, 0, (int) hi.size() / 2);
            const double cFirst = pearson(ramp01(firstHalf.size()), firstHalf);
            if (debugEnabled()) std::fprintf(stderr, "riser? end %d L %d cHi %.2f cCen %.2f rise %.1f cFirst %.2f\n", e + 1, L, cHi, cCen, rise, cFirst);
            if (cHi > 0.7 && cCen > 0.4 && rise > 6.0 && cFirst > 0.3) {
                const double conf = 0.5 + 0.5 * ramp(cHi, 0.7, 0.95) * ramp(rise, 6, 18);
                ev.push_back(makeEvent(EventType::Riser, e - L + 1, L >= 8 ? Importance::Medium : Importance::Minor, conf, L,
                                       "Riser (" + std::to_string(L) + " bars)"));
                riserSpans.push_back({e - L, e});
                break;
            }
        }
        // --- downlifter starting at bar e
        for (int L : {4, 2, 1}) {
            if (e + L > N) continue;
            const auto hi = slice(bf.beatHighDb, e * bpb, (e + L) * bpb);
            const auto cen = slice(bf.beatCentroid, e * bpb, (e + L) * bpb);
            if (hi.size() < 4) continue;
            const auto t = ramp01(hi.size());
            if (pearson(t, cen) < -0.7 && pearson(t, hi) < -0.6 && -deltaByRegression(hi) > 6.0 && hi.front() > medHigh) {
                ev.push_back(makeEvent(EventType::Downlifter, e + 1, Importance::Medium, 0.5 + 0.4 * ramp(-deltaByRegression(hi), 6, 15), L));
                break;
            }
        }
    }
    // Overlapping risers (the same riser seen from two nearby boundaries): keep the most confident.
    {
        std::vector<Event> kept;
        for (auto& e : ev) {
            if (e.type != EventType::Riser) { kept.push_back(e); continue; }
            auto ov = std::find_if(kept.begin(), kept.end(), [&](const Event& k) {
                if (k.type != EventType::Riser) return false;
                const double a = std::max(k.bar, e.bar), b = std::min(k.bar + k.lengthBars, e.bar + e.lengthBars);
                return b - a > 0.5 * std::min(k.lengthBars, e.lengthBars);
            });
            if (ov == kept.end()) kept.push_back(e);
            else if (e.confidence > ov->confidence) *ov = e;
        }
        ev = kept;
        riserSpans.clear();
        for (auto& e : ev) if (e.type == EventType::Riser) riserSpans.push_back({(int) e.bar - 1, (int) (e.bar - 1 + e.lengthBars)});
        // A riser that simply stops is not a downlifter.
        ev.erase(std::remove_if(ev.begin(), ev.end(), [&](const Event& e) {
            if (e.type != EventType::Downlifter) return false;
            const int b0 = (int) e.bar - 1;
            return std::any_of(riserSpans.begin(), riserSpans.end(), [&](auto& r) { return b0 >= r.first && b0 <= r.second + 2; });
        }), ev.end());
    }
    // --- impacts: extreme onset on a downbeat with an energy jump
    const double onP99 = percentile(bf.beatOnset, 99);
    auto bounds_dbg = [&](int b) { return boundaries.count(b) > 0; };
    for (int b = 1; b < N; ++b) {
        const int k = b * bpb;
        if (k >= (int) bf.beatOnset.size()) break;
        // energy is smoothed over +/-2 beats, so measure the jump across that window
        if (k < 3 || k + 2 >= (int) bf.beatEnergy.size()) continue;
        const double jump = bf.beatEnergy[(size_t) k + 2] - bf.beatEnergy[(size_t) k - 3];
        // Impact = sudden attack on a downbeat with an energy jump: a low "boom"
        // (linear low-band attack well above a normal kick) or a crash (high band +10 dB).
        // Log-flux is not used: after a loud riser an impact adds little RELATIVE change.
        // ...compared with the SAME section's following beats (a returning kick is not an impact)
        std::vector<double> rest;
        for (int j = 1; j < bpb && k + j < (int) bf.beatLowRise.size(); ++j) rest.push_back(bf.beatLowRise[(size_t) (k + j)]);
        const bool boom = bf.beatLowRise[(size_t) k] >= 1.2 * std::max(0.3, median(rest));
        std::vector<double> restHigh;
        for (int j = 1; j < bpb && k + j < (int) bf.beatHighDb.size(); ++j) restHigh.push_back(bf.beatHighDb[(size_t) (k + j)]);
        const bool crash = bf.beatHighDb[(size_t) k] - bf.beatHighDb[(size_t) k - 1] >= 10.0
                        && bf.beatHighDb[(size_t) k] - median(restHigh) >= 4.0;   // returning hats are not a crash
        if (debugEnabled() && bounds_dbg(b)) std::fprintf(stderr, "impact? bar %d lowRise %.2f rest %.2f boom %d crash %d onsetP99 %d jump %.1f\n", b + 1, bf.beatLowRise[(size_t) k], median(rest), (int) boom, (int) crash, (int) (bf.beatOnset[(size_t) k] >= onP99), jump);
        if ((boom || crash || bf.beatOnset[(size_t) k] >= onP99) && jump >= 12)
            ev.push_back(makeEvent(EventType::Impact, b + 1, Importance::Medium, 0.5 + 0.5 * ramp(jump, 12, 35)));
    }
    // --- sweeps: strong monotonic brightness change without a noise riser
    for (auto& s : segs) {
        if (s.end - s.start < 8) continue;
        for (int a = s.start; a + 8 <= s.end; a += 4) {
            const bool inRiser = std::any_of(riserSpans.begin(), riserSpans.end(), [&](auto& r) { return a < r.second && a + 8 > r.first; });
            if (inRiser) continue;
            const auto cen = slice(bf.beatCentroid, a * bpb, (a + 8) * bpb);
            const auto hi = slice(bf.beatHighDb, a * bpb, (a + 8) * bpb);
            const double c = pearson(ramp01(cen.size()), cen);
            const double rel = std::fabs(deltaByRegression(cen)) / std::max(1.0, mean(cen));
            if (std::fabs(c) > 0.85 && rel > 0.4 && std::fabs(deltaByRegression(hi)) < 4.0) {
                ev.push_back(makeEvent(EventType::Sweep, a + 1, Importance::Minor, 0.5 + 0.4 * ramp(std::fabs(c), 0.85, 0.97), 8));
                break;   // at most one sweep per section
            }
        }
    }
    // --- major FX: noise-like, loud, not explained by risers
    const double flatP97 = percentile(bf.flatness, 97);
    int lastFx = -100;
    for (int b = 0; b < N; ++b) {
        const bool inRiser = std::any_of(riserSpans.begin(), riserSpans.end(), [&](auto& r) { return b >= r.first && b < r.second + 1; });
        if (inRiser || bf.silent[(size_t) b] || b - lastFx < 8) continue;
        if (bf.flatness[(size_t) b] >= flatP97 && bf.flatness[(size_t) b] > 0.25 && bf.barEnergy[(size_t) b] > 30) {
            ev.push_back(makeEvent(EventType::MajorFx, b + 1, Importance::Minor, 0.5));
            lastFx = b;
        }
    }
    return ev;
}

std::vector<Event> detectArrangementEvents(const BarFeatures& bf, const std::vector<Classified>& sections,
                                           const std::vector<double>& nov, std::vector<Event> fx) {
    std::vector<Event> ev;
    const int N = bf.numBars;
    std::set<int> bounds;
    for (auto& s : sections) if (s.seg.start > 0) bounds.insert(s.seg.start);
    auto snap = [&](int b) {
        for (int d : {0, -1, 1}) if (bounds.count(b + d)) return b + d;
        return b;
    };
    auto sectionAt = [&](int bar0) -> const Classified* {
        for (auto& s : sections) if (bar0 >= s.seg.start && bar0 < s.seg.end) return &s;
        return nullptr;
    };

    // ---------------- element in/out
    struct Map { Element el; EventType in, out; bool major; double capDsp; };
    const Map maps[] = {
        {Kick, EventType::KickIn, EventType::KickOut, true, 1.0},
        {Bass, EventType::BassIn, EventType::BassOut, true, 0.85},
        {Percussion, EventType::PercussionIn, EventType::PercussionIn, false, 0.85},
        {Lead, EventType::LeadIn, EventType::LeadOut, false, 0.7},
        {Pad, EventType::ChordPadIn, EventType::ChordPadIn, false, 0.7},
        {Vocal, EventType::VocalIn, EventType::VocalOut, false, 1.0},
    };
    const bool stems = bf.elementSupported[Vocal];
    std::array<std::vector<bool>, NumElements> states;
    for (auto& m : maps) {
        if (!bf.elementSupported[(size_t) m.el]) continue;
        const auto& act = bf.activity[(size_t) m.el];
        states[(size_t) m.el] = stableStates(act);
        const auto& st = states[(size_t) m.el];
        for (int b = 1; b < N; ++b) {
            if (st[(size_t) b] == st[(size_t) b - 1]) continue;
            const bool on = st[(size_t) b];
            if (!on && m.in == m.out) continue;   // no "out" event type for this element
            const int at = snap(b);
            const double before = mean(slice(act, b - 2, b)), after = mean(slice(act, b, b + 2));
            double conf = ramp(std::fabs(after - before), 0.3, 0.8);
            if (!stems) conf = std::min(conf, m.capDsp);
            if (conf < 0.35) continue;
            const bool atBoundary = bounds.count(at) > 0;
            Importance imp = m.major ? (atBoundary ? Importance::Major : Importance::Medium)
                                     : (atBoundary ? Importance::Medium : Importance::Minor);
            ev.push_back(makeEvent(on ? m.in : m.out, at + 1, imp, conf));
        }
    }
    // Full Drums In replaces separate Kick In + Percussion In on the same bar.
    if (!states[Kick].empty() && !states[Percussion].empty()) {
        for (int b = 1; b < N; ++b) {
            const bool full = states[Kick][(size_t) b] && states[Percussion][(size_t) b];
            const bool wasFull = states[Kick][(size_t) b - 1] && states[Percussion][(size_t) b - 1];
            if (!full || wasFull) continue;
            const int at = snap(b);
            const bool kickNew = !states[Kick][(size_t) b - 1], percNew = !states[Percussion][(size_t) b - 1];
            if (!(kickNew && percNew)) continue;
            ev.erase(std::remove_if(ev.begin(), ev.end(), [&](const Event& e) {
                         return (e.type == EventType::KickIn || e.type == EventType::PercussionIn) && std::fabs(e.bar - (at + 1)) < 0.5; }),
                     ev.end());
            ev.push_back(makeEvent(EventType::FullDrumsIn, at + 1, Importance::Major, 0.8));
        }
    }

    // ---------------- pattern changes (bass / percussion), compared per 4-bar block
    auto patternChanges = [&](const std::vector<std::array<double, 16>>& pat, Element el, EventType type) {
        if (states[(size_t) el].empty()) return;
        for (int b = 4; b + 4 <= N; b += 4) {
            bool active = true;
            for (int q = b - 4; q < b + 4; ++q) active = active && states[(size_t) el][(size_t) q];
            if (!active) continue;
            double A[16] = {0}, B[16] = {0};
            for (int q = 0; q < 4; ++q) for (int s = 0; s < 16; ++s) { A[s] += pat[(size_t) (b - 4 + q)][(size_t) s]; B[s] += pat[(size_t) (b + q)][(size_t) s]; }
            double d = cosDistance(A, B, 16);
            if (type == EventType::BassPatternChange) {
                double ca[12], cb[12];
                for (int k = 0; k < 12; ++k) { ca[k] = cb[k] = 0; for (int q = 0; q < 4; ++q) { ca[k] += bf.barBassChroma[(size_t) (b - 4 + q)][(size_t) k]; cb[k] += bf.barBassChroma[(size_t) (b + q)][(size_t) k]; } }
                d = std::max(d, 0.7 * cosDistance(ca, cb, 12));
            }
            if (d < 0.3) continue;
            const bool atBoundary = bounds.count(b) > 0;
            ev.push_back(makeEvent(type, b + 1, atBoundary ? Importance::Medium : Importance::Minor, 0.4 + 0.5 * ramp(d, 0.3, 0.6)));
        }
    };
    patternChanges(bf.bassPattern, Bass, EventType::BassPatternChange);
    patternChanges(bf.percPattern, Percussion, EventType::PercussionChange);

    // ---------------- structure events
    for (auto& s : sections) {
        if (s.seg.start == 0) continue;
        if (s.type == SectionType::Drop) ev.push_back(makeEvent(EventType::Drop, s.seg.start + 1, Importance::Major, s.confidence));
        else if (s.type == SectionType::Breakdown) ev.push_back(makeEvent(EventType::Breakdown, s.seg.start + 1, Importance::Major, s.confidence));
        else if (s.type == SectionType::Transition)
            ev.push_back(makeEvent(EventType::MajorTransition, s.seg.start + 1,
                                   nov[(size_t) s.seg.start] > 2.5 ? Importance::Major : Importance::Medium, 0.5));
    }

    // ---------------- FX importance in context
    for (auto& e : fx) {
        const int b0 = (int) std::lround(e.bar) - 1;
        if (e.type == EventType::Riser) {
            const auto* target = sectionAt(b0 + (int) e.lengthBars);
            if (target && target->type == SectionType::Drop && e.lengthBars >= 4) e.importance = Importance::Major;
        } else if (e.type == EventType::Impact) {
            const auto* s = sectionAt(b0);
            if (s && s->seg.start == b0 && s->type == SectionType::Drop) e.importance = Importance::Major;
            else if (bounds.count(b0)) e.importance = Importance::Medium;
            else e.importance = Importance::Minor;
        } else if (e.type == EventType::MajorFx && bounds.count(b0)) {
            e.importance = Importance::Medium;
        }
        ev.push_back(e);
    }

    // ---------------- de-duplicate: same type within 2 bars -> keep the more confident
    std::sort(ev.begin(), ev.end(), [](const Event& a, const Event& b) { return a.bar < b.bar; });
    std::vector<Event> out;
    for (auto& e : ev) {
        auto dup = std::find_if(out.begin(), out.end(), [&](const Event& o) { return o.type == e.type && std::fabs(o.bar - e.bar) < 2.0; });
        if (dup == out.end()) out.push_back(e);
        else if (e.confidence > dup->confidence) *dup = e;
    }
    return out;
}

} // namespace dali::pipeline
