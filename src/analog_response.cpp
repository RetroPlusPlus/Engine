#include "retropp/analog_response.h"

#include <algorithm>
#include <cmath>

namespace retropp {

namespace {

// The 1-D dead-zone with edge rescale: zero under `size`, otherwise remap [size, 1] back onto [0, 1] so
// motion begins right at the edge. `v` is a magnitude in [0, 1]; the caller reapplies the sign.
float deadZoneScalar(float v, float size) noexcept {
    const float denom = 1.0f - size;
    if (v <= size || denom <= 0.0f) return 0.0f;
    return (v - size) / denom;
}

// The stretch factor toward the square gate at a point, in [1, m/mx]. Round leaves it at 1 (identity);
// Square takes it to the full m/mx; Scaled interpolates by where gateScale sits between 1 and the
// diagonal limit sqrt(2). Cardinal points (m == mx) are unaffected at every setting.
float gateFactor(float x, float y, const StickResponse& r) noexcept {
    if (r.gate == GateShape::Round) return 1.0f;
    const float mx = std::max(std::abs(x), std::abs(y));
    if (mx <= 0.0f) return 1.0f;
    const float full = std::hypot(x, y) / mx;  // 1 on a cardinal, up to sqrt(2) on a diagonal
    constexpr float kSqrt2 = 1.41421356f;
    const float k = (r.gate == GateShape::Square) ? kSqrt2 : r.gateScale;
    const float t = std::clamp((k - 1.0f) / (kSqrt2 - 1.0f), 0.0f, 1.0f);
    return 1.0f + (full - 1.0f) * t;
}

}  // namespace

Vec2 applyStickResponse(float rawX, float rawY, const StickResponse& response) noexcept {
    float x = std::clamp(rawX, -1.0f, 1.0f);
    float y = std::clamp(rawY, -1.0f, 1.0f);

    // Dead-zone.
    const float size = response.deadZone.size;
    if (response.deadZone.shape == DeadZoneShape::Radial) {
        const float m = std::hypot(x, y);
        if (m <= size || (1.0f - size) <= 0.0f) return {0.0f, 0.0f};
        const float scale = deadZoneScalar(m, size) / m;  // rescaled magnitude / original magnitude
        x *= scale;
        y *= scale;
    } else {  // PerAxis
        x = (x < 0.0f ? -1.0f : 1.0f) * deadZoneScalar(std::abs(x), size);
        y = (y < 0.0f ? -1.0f : 1.0f) * deadZoneScalar(std::abs(y), size);
    }

    // Gate remap, then clamp back into the unit box.
    const float factor = gateFactor(x, y, response);
    x = std::clamp(x * factor, -1.0f, 1.0f);
    y = std::clamp(y * factor, -1.0f, 1.0f);
    return {x, y};
}

float applyTriggerResponse(float raw, const TriggerResponse& response) noexcept {
    const float v = std::clamp(raw, 0.0f, 1.0f);
    return deadZoneScalar(v, response.deadZone);
}

}  // namespace retropp
