// Device-free coverage for the pure analog resolvers (analog_response.h): dead-zone (radial + per-axis),
// the stick gate (Round / Square / Scaled), and the trigger dead-zone. No device, no platform — the whole
// remap is a pure function of raw values and the AnalogResponse config.
#include <gtest/gtest.h>

#include <cmath>

#include "retropp/analog_response.h"
#include "retropp/geometry.h"

namespace retropp {
namespace {

constexpr float kTol = 1e-4f;

// The 1-D per-axis dead-zone with edge rescale — the reference the PerAxis stick shape and the trigger
// dead-zone both compute against.
float perAxisReference(float v, float dz) {
    v = std::clamp(v, -1.0f, 1.0f);
    const float mag = std::abs(v);
    if (mag < dz) return 0.0f;
    return (v < 0.0f ? -1.0f : 1.0f) * (mag - dz) / (1.0f - dz);
}

// ── Triggers ──────────────────────────────────────────────────────────────────────────────────────

TEST(AnalogResponse, TriggerDefaultMatchesPerAxisReference) {
    const TriggerResponse def{};  // size 0.06
    for (float v = 0.0f; v <= 1.0f; v += 0.05f) {
        EXPECT_NEAR(applyTriggerResponse(v, def), perAxisReference(v, 0.06f), kTol) << "v=" << v;
    }
    EXPECT_FLOAT_EQ(applyTriggerResponse(0.03f, def), 0.0f);  // under the dead-zone
}

TEST(AnalogResponse, TriggerSizeZeroIsPassthrough) {
    const TriggerResponse none{.deadZone = 0.0f};
    EXPECT_NEAR(applyTriggerResponse(0.42f, none), 0.42f, kTol);
    EXPECT_NEAR(applyTriggerResponse(0.0f, none), 0.0f, kTol);
}

// ── Sticks: dead-zone shape ─────────────────────────────────────────────────────────────────────

TEST(AnalogResponse, StickDefaultIsRadialDirectionPreserving) {
    const StickResponse def{};  // Radial, size 0.15, Round
    // A small off-axis push on x with a live y: radial keeps BOTH (magnitude is past the dead-zone),
    // preserving direction — where per-axis would have dropped x.
    const Vec2 out = applyStickResponse(0.1f, 0.5f, def);
    const float m = std::hypot(0.1f, 0.5f);
    const float expectedMag = (m - 0.15f) / (1.0f - 0.15f);
    EXPECT_GT(out.x, 0.0f);  // x survived
    EXPECT_NEAR(out.x / out.y, 0.1f / 0.5f, kTol);            // direction preserved
    EXPECT_NEAR(std::hypot(out.x, out.y), expectedMag, kTol);  // magnitude rescaled off the edge
}

TEST(AnalogResponse, RadialDiffersFromPerAxisOnOffAxisPush) {
    const Vec2 radial  = applyStickResponse(0.1f, 0.5f, {.deadZone = {.size = 0.15f, .shape = DeadZoneShape::Radial}});
    const Vec2 perAxis = applyStickResponse(0.1f, 0.5f, {.deadZone = {.size = 0.15f, .shape = DeadZoneShape::PerAxis}});
    EXPECT_GT(radial.x, 0.0f);
    EXPECT_FLOAT_EQ(perAxis.x, 0.0f);  // x (0.1 < 0.15) dropped independently
}

TEST(AnalogResponse, PerAxisShapeMatchesPerAxisReference) {
    const StickResponse perAxis{.deadZone = {.size = 0.15f, .shape = DeadZoneShape::PerAxis}};
    for (float x = -1.0f; x <= 1.0f; x += 0.25f) {
        for (float y = -1.0f; y <= 1.0f; y += 0.25f) {
            const Vec2 out = applyStickResponse(x, y, perAxis);
            EXPECT_NEAR(out.x, perAxisReference(x, 0.15f), kTol) << "x=" << x;
            EXPECT_NEAR(out.y, perAxisReference(y, 0.15f), kTol) << "y=" << y;
        }
    }
}

TEST(AnalogResponse, StickDeadZoneZeroesCentre) {
    // Inside the radial dead-zone → exactly zero, no NaN even at the origin with size 0.
    EXPECT_EQ(applyStickResponse(0.05f, 0.05f, {}), (Vec2{0.0f, 0.0f}));
    const Vec2 originNoDz = applyStickResponse(0.0f, 0.0f, {.deadZone = {.size = 0.0f}});
    EXPECT_FALSE(std::isnan(originNoDz.x));
    EXPECT_EQ(originNoDz, (Vec2{0.0f, 0.0f}));
}

// ── Sticks: gate ────────────────────────────────────────────────────────────────────────────────

TEST(AnalogResponse, GateRoundIsIdentity) {
    // With no dead-zone, Round returns the raw value verbatim.
    const StickResponse round{.deadZone = {.size = 0.0f}, .gate = GateShape::Round};
    const Vec2 out = applyStickResponse(0.6f, -0.3f, round);
    EXPECT_NEAR(out.x, 0.6f, kTol);
    EXPECT_NEAR(out.y, -0.3f, kTol);
}

TEST(AnalogResponse, GateScaledOneEqualsRound) {
    const StickResponse round{.deadZone = {.size = 0.0f}, .gate = GateShape::Round};
    const StickResponse scaled1{.deadZone = {.size = 0.0f}, .gate = GateShape::Scaled, .gateScale = 1.0f};
    for (float a = -0.7f; a <= 0.7f; a += 0.35f) {
        const Vec2 r = applyStickResponse(a, 0.5f, round);
        const Vec2 s = applyStickResponse(a, 0.5f, scaled1);
        EXPECT_NEAR(r.x, s.x, kTol);
        EXPECT_NEAR(r.y, s.y, kTol);
    }
}

TEST(AnalogResponse, GateSquareReachesCornerCardinalsUnchanged) {
    const StickResponse square{.deadZone = {.size = 0.0f}, .gate = GateShape::Square};
    const Vec2 diag = applyStickResponse(0.70710678f, 0.70710678f, square);
    EXPECT_NEAR(diag.x, 1.0f, kTol);  // corner reaches (1, 1)
    EXPECT_NEAR(diag.y, 1.0f, kTol);
    const Vec2 card = applyStickResponse(1.0f, 0.0f, square);
    EXPECT_NEAR(card.x, 1.0f, kTol);  // cardinal unchanged
    EXPECT_NEAR(card.y, 0.0f, kTol);
    const Vec2 half = applyStickResponse(0.35355339f, 0.35355339f, square);
    EXPECT_NEAR(half.x, 0.5f, kTol);  // half-throw diagonal stays proportional
}

TEST(AnalogResponse, GateScaledIsBetweenRoundAndSquare) {
    const StickResponse scaled{.deadZone = {.size = 0.0f}, .gate = GateShape::Scaled, .gateScale = 1.2f};
    const Vec2 out = applyStickResponse(0.70710678f, 0.70710678f, scaled);
    EXPECT_GT(out.x, 0.70710678f);  // boosted past the round throw
    EXPECT_LT(out.x, 1.0f);          // but short of the square corner
}

TEST(AnalogResponse, PipelineDeadZoneBeforeGate) {
    // A push inside the dead-zone is zero regardless of the gate (dead-zone runs first).
    const StickResponse square{.deadZone = {.size = 0.15f}, .gate = GateShape::Square};
    EXPECT_EQ(applyStickResponse(0.05f, 0.05f, square), (Vec2{0.0f, 0.0f}));
}

}  // namespace
}  // namespace retropp
