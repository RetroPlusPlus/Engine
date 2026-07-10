#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
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

[[nodiscard]] constexpr Point lerpPoint(Point a, Point b, float t) noexcept {
    return Point{lerpF(a.x, b.x, t), lerpF(a.y, b.y, t)};
}

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

// A layer's continuous fields — the ones that ease between ticks. The discrete fields (content, z, key,
// flips, blend, effects, regions) snap to the current submission and are not mirrored.
struct LayerMotion {
    LayerScroll scroll{};
    float       alpha = 1.0f;
    Transform   transform{};
    [[nodiscard]] bool operator==(const LayerMotion&) const noexcept = default;
};

// A sprite's continuous fields. The pivot eases beside the transform — deliberately lerped as its own
// point rather than baked into the matrix, so a pivot animating between ticks eases as a moving hinge.
// Sprite::z is discrete (a stacking key, like the flips) and is not mirrored.
struct SpriteMotion {
    int       x = 0;
    int       y = 0;
    float     alpha = 1.0f;
    Transform transform{};
    Point     pivot{};
    [[nodiscard]] bool operator==(const SpriteMotion&) const noexcept = default;
};

// ── The per-id retained mirror ───────────────────────────────────────────────────────────────

// Holds, per layer key and per sprite key, the object's previous and current committed tick state, and
// produces the interpolated frame the renderer composites. This is the legitimate "last two ticks"
// history — per-object motion keyed by the developer key, not a stored frame and not state the game
// reads or writes.
//
// Per render frame the renderer calls reconcile() once when a tick committed, then interpolate() to get the
// frame to draw. Between ticks reconcile() is not called and the mirror is untouched; interpolate() keeps
// easing toward the same submission as the blend factor grows, so several display frames per tick render
// smoothly.
class Interpolator {
public:
    // Commit one simulation tick: for each object in `submission`, keyed by its KEY (the developer-supplied
    // stable identity that survives the game rebuilding the frame each render), prev <- cur and cur <- the
    // submission's motion. A key seen for the first time mounts with
    // prev == cur == its submission (so it snaps until its next tick gives it history); a key absent from
    // `submission` unmounts (a despawn — dropped, no cross-fade). An object with an EMPTY key is never
    // mirrored (a required key should never be empty, but a shipped game under WarnAndResolve stays up).
    // Call exactly once per committed tick.
    void reconcile(const FrameDrawState& submission);

    // The interpolated render frame for `submission` at sub-tick `alpha`: a frame matching `submission`
    // whose each layer's and each sprite's continuous fields are replaced by lerp(prev, submission, alpha)
    // looked up by key. A matched key eases; an unmatched key (no history, or empty) snaps to the
    // submission. Discrete fields and tile content come from `submission` unchanged. Returns a reference to
    // reused internal storage, valid until the next interpolate() call. Does not reconcile — call
    // reconcile() on a tick first.
    [[nodiscard]] const FrameDrawState& interpolate(const FrameDrawState& submission, float alpha);

    void clear();

    // The eased CONTINUOUS position for placement at output resolution: lerp(prev, cur, alpha) kept in
    // float (no round). interpolate() rounds the same ease into the frame's integer LayerScroll /
    // Sprite::x/y for the discrete draw state; these give the renderer the un-rounded value so a
    // sub-pixel position can place between whole viewport pixels on the output-resolution compose path.
    // The tick endpoints are always integer (submissions carry integer positions); the fraction comes
    // from alpha. Returns nullopt for a key with no history (spawn) or an empty key — the caller then
    // places at the submission's whole-pixel position (a snap), matching interpolate().
    [[nodiscard]] std::optional<Vec2> interpolatedLayerScroll(std::string_view key, float alpha) const;
    [[nodiscard]] std::optional<Vec2> interpolatedSpritePos(std::string_view key, float alpha) const;

    // Inspection seam (the per-object upload-skip path consumes the same change flag; tests read all of it).
    [[nodiscard]] std::optional<LayerMotion>  layerPrev(std::string_view key) const;
    [[nodiscard]] std::optional<LayerMotion>  layerCur(std::string_view key) const;
    [[nodiscard]] std::optional<SpriteMotion> spritePrev(std::string_view key) const;
    [[nodiscard]] std::optional<SpriteMotion> spriteCur(std::string_view key) const;
    // Whether the key's most recent tick commit changed its motion (a newly mounted key counts as
    // changed — it has no prior upload). Unknown keys report false.
    [[nodiscard]] bool        layerChanged(std::string_view key) const;
    [[nodiscard]] bool        spriteChanged(std::string_view key) const;
    [[nodiscard]] std::size_t layerCount() const noexcept { return layers_.size(); }
    [[nodiscard]] std::size_t spriteCount() const noexcept { return sprites_.size(); }

private:
    template <class Motion>
    struct Slot {
        Motion prev{};
        Motion cur{};
        bool   changed = false;  // the last commit changed cur (mounts count as changed)
    };

    std::unordered_map<std::string, Slot<LayerMotion>>  layers_;
    std::unordered_map<std::string, Slot<SpriteMotion>> sprites_;

    // Reused scratch so steady-state interpolation allocates nothing: the interpolated frame, the per
    // sprite-layer interpolated sprite arrays its spans point into, and the seen-label sets reconcile rebuilds.
    FrameDrawState                   scratch_;
    std::vector<std::vector<Sprite>> spriteScratch_;
    std::unordered_set<std::string> seenLayers_;
    std::unordered_set<std::string> seenSprites_;
};

}  // namespace retropp
