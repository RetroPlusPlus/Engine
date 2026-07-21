#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "retropp/image.h"     // AtlasId (a frame's sheet) + AssetDimensions (via geometry.h)
#include "retropp/palette.h"   // PaletteId
#include "retropp/timing.h"    // TimingProfile, ticksForDuration

namespace retropp {

// ── Animation: a helper layer over the immediate-mode draw model ─────────────────────────────────────
//
// The engine is immediate-mode with NO retained per-layer state: FrameDrawState is recomputed whole
// every frame, so animation is ALREADY supported — a game animates by resubmitting a fresh frame each
// tick with a different Sprite::tile / TileCell::palette. What this layer adds is the ERGONOMICS: an
// Animation value type, a PURE STATELESS resolver (elapsed ticks → current frame), and a game-owned
// "just play it" wrapper. It introduces NO engine state and NO new render path — exactly the
// relationship the atlas slicer has to draw-time atlas addressing (a device-free convenience layer).
//
// Two animation modes fall out of ONE unit shape: vary the art across frames → sprite/tile animation;
// hold the art constant and vary the palette → palette-cycling animation; vary both → both. Palette is
// an independent per-frame field, so palette animation is not a separate mechanism.

// One unit of an animation: which sheet + which of its slots (the art), which palette, for how long,
// under an optional symbolic label. PURE DATA — it references its resources by handle and carries NO
// draw-state builder logic (the game threads a resolved frame into its Sprite / TileCell — see the
// application pattern in the guide). The art is named ONCE: `.sheet` is the sheet's AtlasId — "this
// sheet, this slot", two values, authored as the explicit projection `{ .sheet = sheet.atlasId }`;
// `.tileIndex` is a SLOT INDEX into that sheet (the i in sheet[i]).
// Nothing is retained or pointed at, so an Animation is a plain value with no lifetime tie to the
// manifest that named it. atlas()/tile()/size() resolve the slot's art in Sprite's own vocabulary —
// slot → cell/size is arithmetic over the slice geometry the renderer records at loadAtlas. Each
// frame's art comes wholly from its own named sheet. An OMITTED `.tileIndex` is a palette-only frame
// (hasArt() == false): the art carries over from the prior frame and only the palette changes —
// palette-cycling is the same mechanism, one field. Different frames may name different sheets, so
// multi-sheet animations still compose across frames.
struct AnimationFrame {
    std::string_view           label{};      // optional symbolic id (empty = unnamed); identity, first member
    AtlasId                    sheet{};       // the sheet this frame's art comes from (sheet.atlasId)
    std::optional<std::size_t> tileIndex{};   // slot index into sheet (sheet[*tileIndex]); omitted = palette-only
    PaletteId                  palette{};     // this frame's palette → enables palette-cycling animation
    std::chrono::nanoseconds   duration{};    // how long this frame shows (real time; resolved to ticks)

    // Does this frame carry art, or only a palette? An omitted `.tileIndex` = palette-only: the consumer
    // keeps the sprite's current art and updates only its palette (the art "carries over").
    [[nodiscard]] bool hasArt() const noexcept { return tileIndex.has_value(); }

    // The resolved art, read through the sheet — the three fields a Sprite / TileCell needs, in the SAME
    // vocabulary. Precondition for tile()/size(): hasArt(), and `.sheet` names a sheet the engine
    // renderer carved via loadAtlas (they resolve the slot arithmetically from that sheet's recorded
    // slice geometry — calling them on a palette-only frame is a precondition violation, like indexing
    // past a container). Feed them straight in: atlas() → Sprite::atlas, tile() → Sprite::tile (the
    // top-left atlas cell), size() → Sprite::size.
    [[nodiscard]] AtlasId         atlas() const noexcept { return sheet; }
    [[nodiscard]] std::uint16_t   tile()  const;  // defined in animation.cpp — resolves via the renderer
    [[nodiscard]] AssetDimensions size()  const;  //   (Renderer::atlasSlot, the recorded slice geometry)

    [[nodiscard]] bool operator==(const AnimationFrame&) const noexcept = default;
};

// An ordered list of frames. PURE DATA + pure access methods; NO playback STATE and NO loop POLICY live
// here — HOW an animation plays (once / N times / forever / for a duration) is a playback decision
// supplied at play time via PlaybackMode, NOT a property baked into the asset, so the same Animation
// plays once in one place and loops in another. The game owns the elapsed-tick clock. Both ways to
// obtain a frame resolve to the SAME AnimationFrame descriptor: time-driven playback (sampleAnimation /
// sampleAnimationFrame) and direct programmatic selection (operator[] / find / indexOf). "Play it" and "pin frame N"
// never conflict because the frame set is plain indexable data.
struct Animation {
    std::vector<AnimationFrame> frames;

    [[nodiscard]] std::size_t           count() const noexcept { return frames.size(); }
    [[nodiscard]] const AnimationFrame& operator[](std::size_t i) const { return frames[i]; }  // raw index

    // Programmatic symbolic access: the first frame whose label == name (labels should be unique within
    // an animation, like a DrawLayer label within a frame). indexOf → its index (nullopt if absent); find → a
    // pointer to it (nullptr if absent).
    [[nodiscard]] std::optional<std::size_t> indexOf(std::string_view name) const noexcept;
    [[nodiscard]] const AnimationFrame*      find(std::string_view name) const noexcept;
};

// How an animation plays — a playback decision supplied WHEN you play it, never baked into the asset
// (a policy on the asset would be a second source of truth). The two data-bearing modes carry their
// payload via the engine's named-constructor idiom (the ShapePoints / Transform / ViewportResolution
// preset shape). Identity is the kind, first member.
struct PlaybackMode {
    enum class Kind : std::uint8_t { Single, LoopNTimes, LoopIndefinitely, PlayForDuration };
    Kind                     kind      = Kind::LoopIndefinitely;  // identity, first member
    std::uint32_t            loopCount = 0;   // LoopNTimes: number of full passes before finishing
    std::chrono::nanoseconds duration{};       // PlayForDuration: total wall-time to play (→ ticks)

    [[nodiscard]] bool operator==(const PlaybackMode&) const noexcept = default;

    static PlaybackMode single();                                     // play once, hold the final frame
    static PlaybackMode loopNTimes(std::uint32_t n);                  // loop n passes, then hold final
    static PlaybackMode loopIndefinitely();                           // loop forever (never finishes) — default
    static PlaybackMode playForDuration(std::chrono::nanoseconds d);  // loop for d, then stop
};

// The output of the pure resolver: which frame to show now + whether playback has ended.
struct PlaybackState {
    std::size_t frameIndex = 0;      // the frame to show now
    bool        finished   = false;  // playback ended (Single / LoopNTimes / PlayForDuration past their end)
    [[nodiscard]] constexpr bool operator==(const PlaybackState&) const noexcept = default;
};

// Length of ONE pass in TICKS: the sum of ticksForDuration(frame.duration) over the frames. 0 frames
// → 0. Pure (TimingProfile is a pass-by-value host config, NOT state).
[[nodiscard]] std::uint64_t totalTicks(const Animation& anim, const TimingProfile& profile) noexcept;

// THE pure playback resolver — the single source of playback truth (AnimationPlayer is the stateful
// wrapper over it). Resolves elapsed ticks under `mode`:
//   LoopIndefinitely   → elapsed modulo totalTicks; never finished.
//   Single             → first pass, then clamp to the last frame; finished once elapsed ≥ totalTicks.
//   LoopNTimes(n)      → wrap for n passes, then hold the last frame; finished once elapsed ≥ n·totalTicks.
//   PlayForDuration(d) → wrap (like LoopIndefinitely) until elapsed ≥ ticksForDuration(d), then hold the
//                        frame shown at the cutoff; finished past d.
// Empty animation → { 0, true }. A zero-tick frame (duration rounds to 0 ticks) is skipped over (never
// the resting frame) so it cannot stall a loop. Durations resolve to ticks via the profile — the engine
// never stores ticks; tick-quantized playback is the honest granularity for a fixed-step sim.
[[nodiscard]] PlaybackState sampleAnimation(const Animation& anim, std::uint64_t elapsedTicks,
                                       const TimingProfile& profile, PlaybackMode mode) noexcept;

// Convenience: the frame itself (anim[sampleAnimation(...).frameIndex]). Precondition: anim.count() > 0.
[[nodiscard]] const AnimationFrame& sampleAnimationFrame(const Animation& anim, std::uint64_t elapsedTicks,
                                            const TimingProfile& profile, PlaybackMode mode) noexcept;

// A game-owned playback cursor over an Animation. STATE LIVES HERE, IN THE GAME'S OBJECT — not in the
// engine. Wraps elapsed-tick bookkeeping + play / pause / seek over the pure sampleAnimation resolver. The
// renderer never sees this; the game constructs it, calls advance() each tick, and threads current()
// into draw state. The engine PROVIDES the type and the game OWNS the instance, exactly like
// std::vector, so it adds no engine-held render state — the immediate-mode model stays intact.
struct AnimationPlayer {
    // The cadence a bare-constructed player resolves frame durations against. Two equally first-class
    // ways to set it: `EngineConfig::setActive` fans the configured cadence into it automatically (one
    // startup call, alongside RunLoop::defaultTiming / Renderer::defaultViewport), OR you assign it
    // directly anytime — `AnimationPlayer::defaultTiming = loop.timing();` (or any profile you like).
    // Either way every bare `AnimationPlayer{.animation = &a}` inherits it with no per-player profile to
    // type; a direct assignment after setActive simply overrides it. It is a single process-wide default
    // — legitimate here: the engine is single-threaded by design, and this is a config default, not
    // retained render state. Override one player by setting `.profile` directly.
    static inline TimingProfile defaultTiming = TimingProfile::GameBoyColor;

    const Animation* animation = nullptr;   // game-owned; must outlive the player (span-style lifetime)
    TimingProfile    profile   = defaultTiming;  // resolves durations→ticks; inherits the default above
    std::uint64_t    elapsedTicks = 0;
    bool             playing = true;
    PlaybackState    state{};               // cached by advance() so current()/finished() need no args

    // Each game tick: accrue elapsedTicks (ONLY while playing) and re-resolve via sampleAnimation under
    // `mode`. THIS IS THE "PLAY" — mode defaults to loopIndefinitely so a bare advance() just loops;
    // pass single() / loopNTimes(n) / playForDuration(d) for the other policies.
    void advance(PlaybackMode mode = PlaybackMode::loopIndefinitely(),
                 std::uint64_t deltaTicks = 1) noexcept;

    [[nodiscard]] const AnimationFrame& current() const;              // (*animation)[state.frameIndex]
    [[nodiscard]] std::size_t           currentIndex() const noexcept { return state.frameIndex; }
    [[nodiscard]] bool                  finished() const noexcept { return state.finished; }

    void play()    noexcept;  // resume
    void pause()   noexcept;  // freeze on the current frame
    void stop()    noexcept;  // pause + rewind to start (frame 0)
    void restart() noexcept;  // rewind + play
    void seek(std::size_t frameIndex) noexcept;  // jump to a frame's start (keeps playing/paused)
    void seek(std::string_view label) noexcept;  // jump by symbolic label (no-op if absent)
};

}  // namespace retropp
