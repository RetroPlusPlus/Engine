#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "retropp/draw_state.h"
#include "retropp/frame_timing.h"
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

// ── The declared cadence ─────────────────────────────────────────────────────────────────────

// The cadence in force for a layer: its own `advancesEvery` when it declares one, otherwise the frame's.
// A declared 0 reads as 1 — zero would mean a world that never advances, which is not a cadence.
[[nodiscard]] constexpr std::uint32_t resolveCadence(std::optional<std::uint32_t> layer,
                                                     std::uint32_t frame) noexcept {
    const std::uint32_t declared = layer.value_or(frame);
    return declared == 0 ? 1u : declared;
}

// The fraction of the way from a slot's previous value to its current one to draw at.
//
//   advancesEvery    — ticks between one advance of this object's world and the next
//   spanAtChange     — ticks the commit that produced the current value ran
//   ticksSinceChange — ticks elapsed since that commit
//   subTick          — fraction of the current tick period elapsed, in [0, 1)
//
// The move is spread over whichever is longer — the declared cadence, or the ticks that commit actually
// covered — and the drawn position runs `advancesEvery` ticks behind the simulation. A shorter lag would
// land past the current value for a slot whose last change is several ticks old, and the eased position
// is only ever between two values the object has held.
//
// At `advancesEvery == 1` with a change this commit the expression is `(span - 1 + subTick) / span`: the
// sub-tick fraction mapped across the commit's own span, which is what the run loop publishes as `alpha`.
// The result stays in [0, 1] for any input; it saturates at 1 for a slot whose world has stopped, which
// draws its current value.
[[nodiscard]] constexpr float easeFactor(std::uint32_t advancesEvery, std::uint32_t spanAtChange,
                                         std::uint64_t ticksSinceChange, float subTick) noexcept {
    const std::uint32_t cadence = advancesEvery == 0 ? 1u : advancesEvery;
    const std::uint32_t width   = spanAtChange > cadence ? spanAtChange : cadence;
    const float         u       = static_cast<float>(ticksSinceChange) + subTick;
    const float         f       = (static_cast<float>(width - cadence) + u) / static_cast<float>(width);
    return f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
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

// A sprite's continuous fields. The pivot and origin each ease beside the transform — lerped as their own
// points rather than baked into the matrix, so a pivot animating between ticks eases as a moving hinge and
// a moving origin eases as a sliding placement handle. Sprite::z is discrete (a stacking key, like the
// flips) and is not mirrored.
struct SpriteMotion {
    int       x = 0;
    int       y = 0;
    float     alpha = 1.0f;
    Transform transform{};
    Point     pivot{};
    Point     origin{};
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
    // Commit the ticks `timing.commitSpan` covers: for each object in `submission`, keyed by its KEY (the
    // developer-supplied stable identity that survives the game rebuilding the frame each render), a motion
    // that DIFFERS from the one held rotates prev <- cur and cur <- the submission's motion, and records the
    // tick and span it changed on. A motion that matches the one held leaves the pair alone until the object's
    // declared cadence has elapsed, then settles prev <- cur; holding it is what lets a world that advances
    // every few ticks ease across all of them. A key seen for the first time mounts with
    // prev == cur == its submission (so it snaps until its next change gives it history); a key absent from
    // `submission` unmounts (a despawn — dropped, no cross-fade). The key is required and non-empty
    // (validateSpriteKeys flags an empty one as a bug); the loop skips an empty-keyed object as a defensive
    // guard so a mis-keyed release build degrades to a snap instead of crashing — not a mode to rely on.
    // Call exactly once per iteration that committed at least one tick.
    void reconcile(const FrameDrawState& submission, const FrameTiming& timing);

    // The interpolated render frame for `submission` at `timing`: a frame matching `submission` whose each
    // layer's and each sprite's continuous fields are replaced by lerp(prev, submission, f) looked up by key,
    // where f is that object's own easeFactor — the frame's one blend factor is not shared, because objects
    // may advance on different cadences. A matched key eases; an unmatched key (a spawn with no history)
    // snaps to the submission. Discrete fields and tile content come from `submission` unchanged. Returns a
    // reference to reused internal storage, valid until the next interpolate() call. Does not reconcile —
    // call reconcile() on a tick first.
    [[nodiscard]] const FrameDrawState& interpolate(const FrameDrawState& submission,
                                                    const FrameTiming&    timing);

    void clear();

    // The eased CONTINUOUS position for placement at output resolution: lerp(prev, cur, f) kept in float
    // (no round), at the same per-object factor interpolate() uses, so the two paths cannot place an object
    // differently. interpolate() rounds the same ease into the frame's integer LayerScroll / Sprite::x/y
    // for the discrete draw state; these give the renderer the un-rounded value so a sub-pixel position can
    // place between whole viewport pixels on the output-resolution compose path. The tick endpoints are
    // always integer (submissions carry integer positions); the timing supplies the fraction. Returns
    // nullopt for a key with no history (a spawn); the caller then places at the submission's whole-pixel
    // position (a snap), matching interpolate().
    [[nodiscard]] std::optional<Vec2> interpolatedLayerScroll(std::string_view   key,
                                                              const FrameTiming& timing) const;
    [[nodiscard]] std::optional<Vec2> interpolatedSpritePos(std::string_view   key,
                                                            const FrameTiming& timing) const;

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

    // Whether every mounted layer and sprite has prev == cur — no object is mid-ease. With all slots settled
    // the sub-tick alpha is output-irrelevant (lerp(a, a, t) == a), so a frame composed here is independent of
    // frame timing. The frame-level compose skip gates on this: only a settled frame can be bit-identical to
    // the last composed one. An empty mirror (nothing keyed) is settled. An object whose world stops settles once
    // its declared cadence has elapsed — the ticks it spends finishing its last move are ticks it is still easing
    // through. Cheap — one pass over the slots.
    [[nodiscard]] bool allSettled() const noexcept;

private:
    template <class Motion>
    struct Slot {
        Motion        prev{};
        Motion        cur{};
        bool          changed       = false;  // the last commit changed cur (mounts count as changed)
        std::uint64_t changedAtTick = 0;      // the mirror's tick count when cur last took a new value
        std::uint32_t spanAtChange  = 1;      // ticks that commit ran — the distance prev and cur cover
        std::uint32_t cadence       = 1;      // ticks per advance, as of the last reconcile
    };

    // The factor to draw `slot` at, from the timing of the frame being rendered.
    template <class Motion>
    [[nodiscard]] float factorFor(const Slot<Motion>& slot, const FrameTiming& timing) const noexcept {
        return easeFactor(slot.cadence, slot.spanAtChange, tick_ - slot.changedAtTick, timing.subTick);
    }

    std::unordered_map<std::string, Slot<LayerMotion>>  layers_;
    std::unordered_map<std::string, Slot<SpriteMotion>> sprites_;

    // The mirror's own tick clock, advanced by each reconcile's commit span. A slot's age is measured
    // against this rather than against the loop's count, so the class needs no reference to the loop.
    std::uint64_t tick_ = 0;

    // Reused scratch so steady-state interpolation allocates nothing: the interpolated frame, the per
    // sprite-layer interpolated sprite arrays its spans point into, and the seen-label sets reconcile rebuilds.
    FrameDrawState                   scratch_;
    std::vector<std::vector<Sprite>> spriteScratch_;
    std::unordered_set<std::string> seenLayers_;
    std::unordered_set<std::string> seenSprites_;
};

}  // namespace retropp
