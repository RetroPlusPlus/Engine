#include "retropp/tween.h"

#include <algorithm>
#include <cmath>

namespace retropp {

// ── Easing curves ───────────────────────────────────────────────────────────────────────────────────
//
// The standard easing equations (Robert Penner's set), each normalized to map [0,1] → [0,1]. The
// transcendentals (std::sin / std::pow / std::sqrt) keep this out of the constexpr header — exactly the
// playbackAt-in-animation.cpp split. ENDPOINTS ARE PINNED: after clamping t, t == 0 → 0 and t == 1 → 1
// for every non-Linear preset, so float rounding in sin/pow never leaks a 0.9999998 out of an endpoint
// (the Tween resolver relies on ease(e,1) being exactly 1 so a Single tween settles precisely on `to`).
// The Back family overshoots only in the open interval (0,1); its endpoints are exact polynomials, so
// pinning them changes nothing there.

namespace {

constexpr float kPi = 3.14159265358979323846f;

// Back-curve overshoot constants (the standard values).
constexpr float kBackC1 = 1.70158f;
constexpr float kBackC3 = kBackC1 + 1.0f;          // In/Out back
constexpr float kBackC2 = kBackC1 * 1.525f;        // InOut back

}  // namespace

float ease(Easing e, float t) noexcept {
    t = std::clamp(t, 0.0f, 1.0f);
    if (e == Easing::Linear) return t;
    // Pin the endpoints exactly for every shaped curve (see header contract).
    if (t == 0.0f) return 0.0f;
    if (t == 1.0f) return 1.0f;

    switch (e) {
        case Easing::Linear: return t;  // handled above; keeps the switch exhaustive

        // Quad — power 2
        case Easing::InQuad:    return t * t;
        case Easing::OutQuad:   return 1.0f - (1.0f - t) * (1.0f - t);
        case Easing::InOutQuad: return t < 0.5f ? 2.0f * t * t
                                                : 1.0f - std::pow(-2.0f * t + 2.0f, 2.0f) / 2.0f;

        // Cubic — power 3
        case Easing::InCubic:    return t * t * t;
        case Easing::OutCubic:   return 1.0f - std::pow(1.0f - t, 3.0f);
        case Easing::InOutCubic: return t < 0.5f ? 4.0f * t * t * t
                                                 : 1.0f - std::pow(-2.0f * t + 2.0f, 3.0f) / 2.0f;

        // Quart — power 4
        case Easing::InQuart:    return t * t * t * t;
        case Easing::OutQuart:   return 1.0f - std::pow(1.0f - t, 4.0f);
        case Easing::InOutQuart: return t < 0.5f ? 8.0f * t * t * t * t
                                                 : 1.0f - std::pow(-2.0f * t + 2.0f, 4.0f) / 2.0f;

        // Quint — power 5
        case Easing::InQuint:    return t * t * t * t * t;
        case Easing::OutQuint:   return 1.0f - std::pow(1.0f - t, 5.0f);
        case Easing::InOutQuint: return t < 0.5f ? 16.0f * t * t * t * t * t
                                                 : 1.0f - std::pow(-2.0f * t + 2.0f, 5.0f) / 2.0f;

        // Sine — quarter-cosine
        case Easing::InSine:    return 1.0f - std::cos((t * kPi) / 2.0f);
        case Easing::OutSine:   return std::sin((t * kPi) / 2.0f);
        case Easing::InOutSine: return -(std::cos(kPi * t) - 1.0f) / 2.0f;

        // Expo — base 2
        case Easing::InExpo:  return std::pow(2.0f, 10.0f * t - 10.0f);
        case Easing::OutExpo: return 1.0f - std::pow(2.0f, -10.0f * t);
        case Easing::InOutExpo:
            return t < 0.5f ? std::pow(2.0f, 20.0f * t - 10.0f) / 2.0f
                            : (2.0f - std::pow(2.0f, -20.0f * t + 10.0f)) / 2.0f;

        // Circ — circular arc
        case Easing::InCirc:  return 1.0f - std::sqrt(1.0f - t * t);
        case Easing::OutCirc: return std::sqrt(1.0f - (t - 1.0f) * (t - 1.0f));
        case Easing::InOutCirc:
            return t < 0.5f
                       ? (1.0f - std::sqrt(1.0f - std::pow(2.0f * t, 2.0f))) / 2.0f
                       : (std::sqrt(1.0f - std::pow(-2.0f * t + 2.0f, 2.0f)) + 1.0f) / 2.0f;

        // Back — overshoots its target (returns > 1 / < 0 in the interior; pinned at the endpoints)
        case Easing::InBack:  return kBackC3 * t * t * t - kBackC1 * t * t;
        case Easing::OutBack:
            return 1.0f + kBackC3 * std::pow(t - 1.0f, 3.0f) + kBackC1 * std::pow(t - 1.0f, 2.0f);
        case Easing::InOutBack:
            return t < 0.5f
                       ? (std::pow(2.0f * t, 2.0f) * ((kBackC2 + 1.0f) * 2.0f * t - kBackC2)) / 2.0f
                       : (std::pow(2.0f * t - 2.0f, 2.0f) *
                              ((kBackC2 + 1.0f) * (t * 2.0f - 2.0f) + kBackC2) +
                          2.0f) /
                             2.0f;
    }
    return t;  // unreachable — all enumerators handled
}

}  // namespace retropp
