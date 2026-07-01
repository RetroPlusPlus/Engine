#pragma once

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "retropp/draw_state.h"
#include "retropp/transform.h"

namespace retropp {

// ── Pure interpolation math (headlessly unit-tested) ─────────────────────────────────────────

// Round-to-nearest integer interpolation. Motion is eased in float and quantized at the write into an
// integer draw-state sink (Sprite::x/y, LayerScroll). There is deliberately no integer overload — easing
// in integers would truncate mid-track and stair-step.
[[nodiscard]] constexpr int lerpRound(int a, int b, float t) noexcept {
    const float v = static_cast<float>(a) + (static_cast<float>(b) - static_cast<float>(a)) * t;
    return static_cast<int>(v + (v >= 0.0f ? 0.5f : -0.5f));
}

[[nodiscard]] constexpr float lerpF(float a, float b, float t) noexcept { return a + (b - a) * t; }

[[nodiscard]] constexpr LayerScroll lerpScroll(LayerScroll a, LayerScroll b, float t) noexcept {
    return LayerScroll{lerpRound(a.x, b.x, t), lerpRound(a.y, b.y, t)};
}

// Component-wise interpolation of the nine homography coefficients. Eased per coefficient, so a layer or
// sprite transform animates smoothly between two tick states (a rotating quad's coefficients move together).
[[nodiscard]] constexpr Transform lerpTransform(const Transform& a, const Transform& b, float t) noexcept {
    return Transform{lerpF(a.m00, b.m00, t), lerpF(a.m01, b.m01, t), lerpF(a.m02, b.m02, t),
                     lerpF(a.m10, b.m10, t), lerpF(a.m11, b.m11, t), lerpF(a.m12, b.m12, t),
                     lerpF(a.m20, b.m20, t), lerpF(a.m21, b.m21, t), lerpF(a.m22, b.m22, t)};
}

// ── Per-object continuous fields (the interpolated subset) ───────────────────────────────────

// A layer's continuous fields — the ones that ease between ticks. The discrete fields (content, z, label,
// flips, blend, effects, regions) snap to the current submission and are not mirrored.
struct LayerMotion {
    LayerScroll scroll{};
    float       alpha = 1.0f;
    Transform   transform{};
    [[nodiscard]] bool operator==(const LayerMotion&) const noexcept = default;
};

// A sprite's continuous fields.
struct SpriteMotion {
    int       x = 0;
    int       y = 0;
    Transform transform{};
    [[nodiscard]] bool operator==(const SpriteMotion&) const noexcept = default;
};

// ── The per-id retained mirror ───────────────────────────────────────────────────────────────

// Holds, per layer id and per sprite id, the object's previous and current committed tick state, and
// produces the interpolated frame the renderer composites. This is the legitimate "last two ticks"
// history — per-object motion keyed by id, not a stored frame and not state the game reads or writes.
//
// Per render frame the renderer calls reconcile() once when a tick committed, then interpolate() to get the
// frame to draw. Between ticks reconcile() is not called and the mirror is untouched; interpolate() keeps
// easing toward the same submission as the blend factor grows, so several display frames per tick render
// smoothly.
class Interpolator {
public:
    // Commit one simulation tick: for each object in `submission`, by id, prev <- cur and cur <- the
    // submission's motion. An id seen for the first time mounts with prev == cur == its submission (so it
    // snaps until its next tick gives it history); an id absent from `submission` unmounts (a despawn —
    // dropped, no cross-fade). An id that is 0 (the none sentinel) is never mirrored. Call exactly once per
    // committed tick.
    void reconcile(const FrameDrawState& submission);

    // The interpolated render frame for `submission` at sub-tick `alpha`: a frame matching `submission`
    // whose each layer's and each sprite's continuous fields are replaced by lerp(prev, submission, alpha)
    // looked up by id. A matched id eases; an unmatched id (no history, or id 0) snaps to the submission.
    // Discrete fields and tile content come from `submission` unchanged. Returns a reference to reused
    // internal storage, valid until the next interpolate() call. Does not reconcile — call reconcile() on a
    // tick first.
    [[nodiscard]] const FrameDrawState& interpolate(const FrameDrawState& submission, float alpha);

    void clear();

    // Inspection seam (the per-object upload-skip path consumes the same change flag; tests read all of it).
    [[nodiscard]] std::optional<LayerMotion>  layerPrev(LayerId id) const;
    [[nodiscard]] std::optional<LayerMotion>  layerCur(LayerId id) const;
    [[nodiscard]] std::optional<SpriteMotion> spritePrev(SpriteId id) const;
    [[nodiscard]] std::optional<SpriteMotion> spriteCur(SpriteId id) const;
    // Whether the id's most recent tick commit changed its motion (a newly mounted id counts as changed —
    // it has no prior upload). Unknown ids report false.
    [[nodiscard]] bool        layerChanged(LayerId id) const;
    [[nodiscard]] bool        spriteChanged(SpriteId id) const;
    [[nodiscard]] std::size_t layerCount() const noexcept { return layers_.size(); }
    [[nodiscard]] std::size_t spriteCount() const noexcept { return sprites_.size(); }

private:
    template <class Motion>
    struct Slot {
        Motion prev{};
        Motion cur{};
        bool   changed = false;  // the last commit changed cur (mounts count as changed)
    };

    std::unordered_map<std::uint32_t, Slot<LayerMotion>>  layers_;
    std::unordered_map<std::uint32_t, Slot<SpriteMotion>> sprites_;

    // Reused scratch so steady-state interpolation allocates nothing: the interpolated frame, the per
    // sprite-layer interpolated sprite arrays its spans point into, and the seen-id sets reconcile rebuilds.
    FrameDrawState                   scratch_;
    std::vector<std::vector<Sprite>> spriteScratch_;
    std::unordered_set<std::uint32_t> seenLayers_;
    std::unordered_set<std::uint32_t> seenSprites_;
};

}  // namespace retropp
