#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "retropp/animation.h"    // Animation, AnimationFrame, PlaybackMode, frameAt
#include "retropp/curve.h"        // Curve, ArcLengthTable
#include "retropp/geometry.h"     // Vec2
#include "retropp/path_walker.h"  // PathPacing — the movement driver, resolved via walkAt
#include "retropp/timing.h"       // TimingProfile
#include "retropp/tween.h"        // Tween<float>, Tween<Vec2> — the rotation / scale tracks

namespace retropp {

struct Sprite;  // draw_state.h — applyTo() writes into one; only a reference is named here

// ── Sprite paths: SpritePath, the movement orchestrator ─────────────────────────────────────────────
//
// One game-owned cursor that drives a sprite along a curve while it rotates, scales, and animates — the
// player one level up from PathWalker. Where PathWalker resolves elapsed ticks → a position + facing and
// nothing else, SpritePath composes that movement with concurrent rotation and scale tracks, a frame
// animation, and a facing policy, all off ONE elapsed-tick clock, and writes the composed result into a
// Sprite. It is the movement analogue of the whole player family: pure-data content (SpritePathNode) plus a
// pure resolver internally, wrapped in a game-owned cursor whose state lives in the game's object, not the
// engine. The renderer never sees it.
//
// A path plays ONE node here: the node names where it travels, how fast, which way it faces, and the tracks
// that run alongside the move.

// ── SpritePathMove: where a node travels ────────────────────────────────────────────────────────────

// A declarative movement spec. It is resolved to a Curve when its node starts — origin is only known then,
// so it defaults to wherever the path begins. Named constructors are the authoring surface; aggregate
// initialization stays available and interchangeable. Identity is the kind, the first member.
struct SpritePathMove {
    enum class Kind : std::uint8_t { Line, ThroughPoints, Hermite, Curve };
    Kind                kind = Kind::Line;              // identity, first member
    std::optional<Vec2> origin{};             // absent → the path's start (start, or a previous node's end)
    Vec2                destination{};         // Line / Hermite: the point travelled to
    std::vector<Vec2>   points;               // ThroughPoints: travelled through in order, ending at back()
    Vec2                originTangent{};        // Hermite: the departure directional vector
    Vec2                destinationTangent{};   // Hermite: the arrival directional vector
    retropp::Curve      curve{};              // Curve: travel this exact curve — origin defaulting does NOT
                                              //   apply; it starts where the curve starts

    [[nodiscard]] bool operator==(const SpritePathMove&) const = default;

    static SpritePathMove to(Vec2 destination);                          // Line from the inherited origin
    static SpritePathMove to(Vec2 origin, Vec2 destination);             // Line from an explicit origin
    static SpritePathMove through(std::vector<Vec2> points);             // ThroughPoints from the origin
    static SpritePathMove through(Vec2 origin, std::vector<Vec2> points);
    static SpritePathMove hermite(Vec2 destination, Vec2 originTangent, Vec2 destinationTangent);
    static SpritePathMove hermite(Vec2 origin, Vec2 destination, Vec2 originTangent, Vec2 destinationTangent);
    static SpritePathMove onCurve(retropp::Curve c);                     // travel a pre-authored curve as-is
};

// How the movement facing drives the sprite's orientation. Applied during compose; the raw facing() stays
// exposed for a game to override afterward.
enum class FacingPolicy : std::uint8_t {
    None,            // facing does not touch the sprite (default)
    FlipX,           // flipX true while travelling toward -x (from the horizontal tangent sign; holds its
                     //   previous value while the horizontal component is 0, so vertical travel does not
                     //   flip-flop a mirrored sprite)
    RotateToFacing,  // rotation from the tangent angle (atan2), summed with the rotation track; art is
                     //   assumed authored facing +x
};

// One node: a movement spec + pacing + the concurrent tracks. PURE DATA — the SpritePath cursor owns all
// runtime state and resolves each track via the pure resolvers (tweenAt / playbackAt) against its own clock.
// A tween track is held BY VALUE (a node is self-contained, copyable, with no lifetime coupling); the
// Animation is BY POINTER (a shared asset the game owns, valid for the cursor's lifetime), as is the
// DistanceTween a pacing may carry.
struct SpritePathNode {
    std::string_view            label{};      // optional symbolic id (empty = unnamed); identity, first member
    SpritePathMove              move{};       // where it travels
    PathPacing                  pacing{};     // how fast (constant speed / eased / a distance tween; default parked)
    FacingPolicy                facing = FacingPolicy::None;
    std::optional<Tween<float>> rotationDegrees{};              // rotation track (absent = no track)
    PlaybackMode                rotationMode = PlaybackMode::single();
    std::optional<Tween<Vec2>>  scale{};                        // scale track (absent = no track)
    PlaybackMode                scaleMode = PlaybackMode::single();
    const Animation*            animation = nullptr;            // frames track (game-owned; nullptr = none)
    PlaybackMode                animationMode = PlaybackMode::loopIndefinitely();
    std::optional<Vec2>         pivot{};      // sprite-local pixels for rotation/scale; absent = the sprite's
                                              //   centre at the write
};

// ── The composed sample ────────────────────────────────────────────────────────────────────────────

// The composed per-tick result, as RAW VALUES — no pre-composed Transform, because the rotation/scale pivot
// defaults to the sprite's centre and the sprite's size is only known at the write (the frame track may
// change it). applyTo() composes for the common case; a consumer reading these composes its own.
struct SpritePathSample {
    Vec2                  position{};              // float — quantized at the write
    Vec2                  facing{};                // the movement facing (holds the last real heading across dead spots)
    float                 rotationDegrees = 0.0f;  // the rotation track + RotateToFacing, summed
    Vec2                  scale{1.0f, 1.0f};       // the scale track (identity when absent)
    bool                  flipX = false;           // meaningful under FacingPolicy::FlipX
    const AnimationFrame* frame = nullptr;         // the current frame (nullptr = no animation track)
    float                 distance = 0.0f;         // the movement arc-length resolved
    bool                  finished = false;        // MOVEMENT finished (the tracks never gate completion)

    [[nodiscard]] bool operator==(const SpritePathSample&) const noexcept = default;
};

// ── The game-owned cursor ────────────────────────────────────────────────────────────────────────────

// The "just play it" cursor over one node. STATE LIVES HERE, IN THE GAME'S OBJECT — not in the engine. The
// same shape as TweenPlayer / AnimationPlayer / PathWalker: elapsed-tick bookkeeping + play / pause / seek
// over the composed resolve. The game constructs it, calls advance() each tick, and either reads the getters
// or calls applyTo(sprite). The move spec is baked to an ArcLengthTable lazily on the first advance() (and
// re-baked by stop() / restart()), because designated initialization cannot run a bake and the start anchor
// is only known then. Re-path a mover by assigning a new node and calling restart().
struct SpritePath {
    // The cadence a bare-constructed path resolves pacing and track durations against. EngineConfig::setActive
    // fans the configured cadence into it at startup (alongside the other players' defaultTiming), or assign
    // it directly, or override one path via .profile. A single process-wide default — legitimate here: the
    // engine is single-threaded, and this is a config default, not retained state.
    static inline TimingProfile defaultTiming = TimingProfile::GameBoyColor;

    SpritePathNode   node{};               // the one node this path plays
    Vec2             start{};              // where the path begins when the node's move names no origin
    TimingProfile    profile = defaultTiming;
    std::uint64_t    elapsedTicks = 0;
    bool             playing = true;
    SpritePathSample sample{};             // cached by advance() so the getters need no arguments

    ArcLengthTable   arc{};                // private-by-convention runtime: the lazily-baked movement geometry
    bool             baked = false;        //   and whether it is current for the node

    // Each game tick: bake the node if needed, accrue elapsedTicks (ONLY while playing), and re-resolve the
    // composed sample under `mode`. Mode defaults to loopIndefinitely so a bare advance() loops the movement,
    // like the other players; the tracks resolve at the same clock under their own per-node modes.
    void advance(PlaybackMode mode = PlaybackMode::loopIndefinitely(), std::uint64_t deltaTicks = 1);

    // Write the composed sample into `s`: position → x/y (quantized), the frame's art fields when an
    // animation track is present, a scale-then-rotation transform when a rotation/scale track or
    // RotateToFacing is declared (about node.pivot or the sprite's centre), and flipX under FacingPolicy::FlipX.
    // key, alpha, flipY, and the 90° texture rotation are left untouched — the game's.
    void applyTo(Sprite& s) const;

    [[nodiscard]] Vec2                  position()        const noexcept { return sample.position; }
    [[nodiscard]] Vec2                  facing()          const noexcept { return sample.facing; }
    [[nodiscard]] float                 rotationDegrees() const noexcept { return sample.rotationDegrees; }
    [[nodiscard]] Vec2                  scaleValue()      const noexcept { return sample.scale; }
    [[nodiscard]] bool                  flipX()           const noexcept { return sample.flipX; }
    [[nodiscard]] const AnimationFrame* frame()           const noexcept { return sample.frame; }
    [[nodiscard]] float                 distance()        const noexcept { return sample.distance; }
    [[nodiscard]] bool                  finished()        const noexcept { return sample.finished; }

    void play();                             // resume
    void pause();                            // freeze at the current sample
    void stop();                             // pause + rewind to the node start (re-bakes the node)
    void restart();                          // rewind + play (re-bakes the node — the re-path entry point)
    void seek(std::chrono::nanoseconds at);  // jump to a wall-time offset; keeps the playing / paused state
};

}  // namespace retropp
