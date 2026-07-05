#include "retropp/sprite_path.h"

#include <cmath>     // std::atan2, std::lround
#include <span>      // std::span — throughPoints input
#include <utility>   // std::move

#include "retropp/draw_state.h"  // Sprite — applyTo writes into one
#include "retropp/transform.h"   // Transform — the composed scale/rotation write

namespace retropp {

// ── SpritePathMove named constructors ───────────────────────────────────────────────────────────────

SpritePathMove SpritePathMove::to(Vec2 destination) {
    return SpritePathMove{.kind = Kind::Line, .destination = destination};
}

SpritePathMove SpritePathMove::to(Vec2 origin, Vec2 destination) {
    return SpritePathMove{.kind = Kind::Line, .origin = origin, .destination = destination};
}

SpritePathMove SpritePathMove::through(std::vector<Vec2> points) {
    return SpritePathMove{.kind = Kind::ThroughPoints, .points = std::move(points)};
}

SpritePathMove SpritePathMove::through(Vec2 origin, std::vector<Vec2> points) {
    return SpritePathMove{.kind = Kind::ThroughPoints, .origin = origin, .points = std::move(points)};
}

SpritePathMove SpritePathMove::hermite(Vec2 destination, Vec2 originTangent, Vec2 destinationTangent) {
    return SpritePathMove{.kind               = Kind::Hermite,
                          .destination        = destination,
                          .originTangent      = originTangent,
                          .destinationTangent = destinationTangent};
}

SpritePathMove SpritePathMove::hermite(Vec2 origin, Vec2 destination, Vec2 originTangent,
                                       Vec2 destinationTangent) {
    return SpritePathMove{.kind               = Kind::Hermite,
                          .origin             = origin,
                          .destination        = destination,
                          .originTangent      = originTangent,
                          .destinationTangent = destinationTangent};
}

SpritePathMove SpritePathMove::onCurve(retropp::Curve c) {
    return SpritePathMove{.kind = Kind::Curve, .curve = std::move(c)};
}

// ── Compose + bake helpers ──────────────────────────────────────────────────────────────────────────

namespace {

// The tangent facing as a clockwise degree angle in the engine's top-left-origin pixel space — exactly what
// Transform::rotation takes. A zero facing yields 0°.
float facingDegrees(Vec2 facing) noexcept {
    return std::atan2(facing.y, facing.x) * 57.29577951308232f;  // 180 / π
}

// Resolve a move spec to a curve. origin defaults to `start` for the point/line/Hermite forms; the raw
// Curve form is exempt (it starts where its own geometry starts). ThroughPoints prepends the origin so the
// path travels FROM the start THROUGH the listed points.
Curve buildCurve(const SpritePathMove& m, Vec2 start) {
    const Vec2 origin = m.origin.value_or(start);
    switch (m.kind) {
        case SpritePathMove::Kind::Line:
            return Curve::line(origin, m.destination);
        case SpritePathMove::Kind::ThroughPoints: {
            std::vector<Vec2> pts;
            pts.reserve(m.points.size() + 1);
            pts.push_back(origin);
            pts.insert(pts.end(), m.points.begin(), m.points.end());
            return Curve::throughPoints(std::span<const Vec2>(pts), false);
        }
        case SpritePathMove::Kind::Hermite:
            return Curve::hermite(origin, m.originTangent, m.destination, m.destinationTangent);
        case SpritePathMove::Kind::Curve:
            return m.curve;
    }
    return Curve{};  // unreachable — every kind handled
}

// (Re)bake the node's movement geometry into the cursor's arc table.
void bakeNode(SpritePath& p) {
    p.arc   = buildCurve(p.node.move, p.start).arcTable();
    p.baked = true;
}

// Bake once if the current geometry is not yet current for the node.
void ensureBaked(SpritePath& p) {
    if (!p.baked) {
        bakeNode(p);
    }
}

// Resolve the composed sample at `elapsed` under `mode`: the movement (position + held facing) via walkAt,
// then the rotation / scale / facing / animation tracks off the SAME clock. `p.sample` supplies the
// hold-last state (facing across a dead spot, flipX while the horizontal component is 0). Precondition: the
// geometry is baked.
SpritePathSample resolveComposed(const SpritePath& p, std::uint64_t elapsed, PlaybackMode mode) {
    const SpritePathNode& node = p.node;

    // Movement — position, arc-length, finished, and the facing with hold-last carried from the cache.
    const WalkSample walk = walkAt(p.arc, node.pacing, elapsed, p.profile, mode);
    Vec2             heading = walk.facing;
    if (heading == Vec2{}) {
        heading = p.sample.facing;  // hold the last real heading across a directionless spot
    }

    SpritePathSample next{};
    next.position = walk.position;
    next.facing   = heading;
    next.distance = walk.distance;
    next.finished = walk.finished;

    // Rotation track + RotateToFacing, summed.
    float rotation = 0.0f;
    if (node.rotationDegrees.has_value()) {
        rotation += tweenAt(*node.rotationDegrees, elapsed, p.profile, node.rotationMode).value;
    }
    if (node.facing == FacingPolicy::RotateToFacing) {
        rotation += facingDegrees(heading);
    }
    next.rotationDegrees = rotation;

    // Scale track (identity when absent).
    if (node.scale.has_value()) {
        next.scale = tweenAt(*node.scale, elapsed, p.profile, node.scaleMode).value;
    }

    // flipX under FacingPolicy::FlipX: mirror while travelling toward -x, hold the previous value while the
    // horizontal component is 0 (vertical travel does not flip-flop). Otherwise carry the cached value.
    bool flip = p.sample.flipX;
    if (node.facing == FacingPolicy::FlipX) {
        if (heading.x < 0.0f) {
            flip = true;
        } else if (heading.x > 0.0f) {
            flip = false;
        }
    }
    next.flipX = flip;

    // Frame track (nullptr when absent or empty).
    if (node.animation != nullptr && node.animation->count() > 0) {
        next.frame = &frameAt(*node.animation, elapsed, p.profile, node.animationMode);
    }

    return next;
}

}  // namespace

// ── SpritePath ───────────────────────────────────────────────────────────────────────────────────────

void SpritePath::advance(PlaybackMode mode, std::uint64_t deltaTicks) {
    ensureBaked(*this);
    if (playing) {
        elapsedTicks += deltaTicks;
    }
    sample = resolveComposed(*this, elapsedTicks, mode);
}

void SpritePath::applyTo(Sprite& s) const {
    // Position → the sprite's top-left, quantized — the one quantize point.
    s.x = static_cast<int>(std::lround(sample.position.x));
    s.y = static_cast<int>(std::lround(sample.position.y));

    // Frame art fields (only when an animation track is present).
    if (sample.frame != nullptr) {
        s.atlas   = sample.frame->atlas;
        s.tile    = sample.frame->slot.tile;
        s.size    = sample.frame->slot.dimensions;
        s.palette = sample.frame->palette;
    }

    // A geometric transform when a rotation/scale track or RotateToFacing is declared. Pivot defaults to the
    // sprite's centre using the size AFTER the frame write; scale composes first, then rotation.
    const bool wantsTransform = node.rotationDegrees.has_value() || node.scale.has_value()
                             || node.facing == FacingPolicy::RotateToFacing;
    if (wantsTransform) {
        const float px = node.pivot ? node.pivot->x : static_cast<float>(s.size.width) * 0.5f;
        const float py = node.pivot ? node.pivot->y : static_cast<float>(s.size.height) * 0.5f;
        s.transform = Transform::scale(sample.scale.x, sample.scale.y, px, py)
                          .then(Transform::rotation(sample.rotationDegrees, px, py));
    }

    // flipX only under the FlipX policy.
    if (node.facing == FacingPolicy::FlipX) {
        s.flipX = sample.flipX;
    }
}

void SpritePath::play() { playing = true; }

void SpritePath::pause() { playing = false; }

void SpritePath::stop() {
    bakeNode(*this);  // pick up the current node (supports re-path via a fresh node + stop/restart)
    playing      = false;
    elapsedTicks = 0;
    sample       = resolveComposed(*this, 0, PlaybackMode::loopIndefinitely());
}

void SpritePath::restart() {
    bakeNode(*this);
    playing      = true;
    elapsedTicks = 0;
    sample       = resolveComposed(*this, 0, PlaybackMode::loopIndefinitely());
}

void SpritePath::seek(std::chrono::nanoseconds at) {
    ensureBaked(*this);
    elapsedTicks = profile.ticksForDuration(at);
    sample       = resolveComposed(*this, elapsedTicks, PlaybackMode::loopIndefinitely());
}

}  // namespace retropp
