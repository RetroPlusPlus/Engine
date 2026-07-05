#include "abductor.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <span>

#include "layout.h"

namespace ferryman {

using namespace retropp;

namespace {

std::uint32_t nextRand(std::uint32_t& s) { s = s * 1664525u + 1013904223u; return s; }

float frand(std::uint32_t& s) { return static_cast<float>(nextRand(s) % 10000u) / 10000.0f; }

// A point in the field's upper-lane band (the entry curve wanders through these — it arrives
// over the crossing, not over the shore where its prey waits).
Vec2 interiorPoint(std::uint32_t& s) {
    return Vec2{60.0f + frand(s) * (kViewW - 120.0f),
                static_cast<float>(kFieldTop) + 40.0f + frand(s) * 180.0f};
}

}  // namespace

void Abductor::reset(int entryTicks) {
    state_ = AbductorState::Away;
    timer_ = entryTicks;
    entry_.reset();
    beamJustLit_ = false;
}

void Abductor::foil(int reentryTicks) {
    state_ = AbductorState::Fleeing;
    timer_ = reentryTicks;   // consumed when the flee leaves the field
}

void Abductor::tick(std::optional<Vec2> target, std::uint32_t& rng) {
    beamJustLit_ = false;

    switch (state_) {
        case AbductorState::Away: {
            if (--timer_ > 0) return;
            // Bake the entry swoop: off-screen on a random side, two interior waypoints, and an
            // arrival point — smoothed by throughPoints, walked at constant speed via the arc
            // table (sampled ONCE here, not per frame).
            Vec2 start{};
            switch (nextRand(rng) % 3u) {
                case 0:  start = Vec2{-50.0f, kFieldTop + 30.0f + frand(rng) * 150.0f}; break;
                case 1:  start = Vec2{kViewW + 50.0f, kFieldTop + 30.0f + frand(rng) * 150.0f}; break;
                default: start = Vec2{frand(rng) * kViewW, kFieldTop - 50.0f}; break;
            }
            const std::array<Vec2, 4> waypoints{{start, interiorPoint(rng), interiorPoint(rng),
                                                 interiorPoint(rng)}};
            const Curve path = Curve::throughPoints(std::span<const Vec2>(waypoints));
            entry_   = path.arcTable();
            pathLen_ = entry_->length();
            pathS_   = 0.0f;
            x_       = start.x;
            y_       = start.y;
            ++spawn_;  // every visit is a NEW arrival — a fresh key mount-snaps it in
            state_ = AbductorState::Entering;
            break;
        }

        case AbductorState::Entering: {
            pathS_ += kAbductorEntry;
            const Vec2 p = entry_->atDistance(pathS_);
            x_ = p.x;
            y_ = p.y;
            if (pathS_ >= pathLen_) {
                entry_.reset();
                state_ = AbductorState::Positioning;
            }
            break;
        }

        case AbductorState::Positioning: {
            // Fly to the hover point above the sim's chosen colonist; with nothing to steal it
            // patrols a slow line over the upper lanes, waiting for prey.
            Vec2 want{};
            if (target) {
                want = Vec2{target->x, target->y - kHoverHeight};
            } else {
                patrol_ += 0.008f;
                want = Vec2{kViewW / 2.0f + std::sin(patrol_) * (kViewW / 2.0f - 80.0f),
                            static_cast<float>(kFieldTop) + 90.0f};
            }
            const float dx = want.x - x_, dy = want.y - y_;
            const float d  = std::hypot(dx, dy);
            sway_ += 0.07f;
            if (d > 0.5f) {
                const float ux = dx / d, uy = dy / d;
                const float bob = std::sin(sway_) * 0.5f;
                x_ += ux * kAbductorCruise - uy * bob;
                y_ += uy * kAbductorCruise + ux * bob;
            }
            // On station above real prey → drop down the beam line.
            if (target && std::abs(x_ - target->x) < 4.0f && std::abs(dy) < 6.0f) {
                state_       = AbductorState::Descending;
                beamJustLit_ = true;
            }
            break;
        }

        case AbductorState::Descending: {
            if (!target) {  // the prey vanished mid-drop (the ferry got there) — climb back off
                state_ = AbductorState::Positioning;
                break;
            }
            x_ += std::clamp(target->x - x_, -0.6f, 0.6f);  // tracks a drifting target gently
            y_ += kAbductorDescend;
            break;
        }

        case AbductorState::Carrying: {
            y_ -= kAbductorRise;
            // Off the top of the field: the sim notices (pos().y) and books the loss.
            break;
        }

        case AbductorState::Fleeing: {
            const float dir = x_ < kViewW / 2.0f ? -1.0f : 1.0f;
            x_ += dir * kAbductorFlee;
            y_ -= kAbductorFlee * 0.35f;
            if (x_ < -60.0f || x_ > kViewW + 60.0f) reset(timer_);
            break;
        }
    }
}

}  // namespace ferryman
