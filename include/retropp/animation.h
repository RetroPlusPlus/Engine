#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "retropp/image.h"     // AtlasId, AssetSlot (AssetSlot carries AssetDimensions)
#include "retropp/palette.h"   // PaletteId
#include "retropp/timing.h"    // TimingProfile, ticksForDuration

namespace retropp {

// ── Animation: a helper layer over the immediate-mode draw model (ENG-2.H) ──────────────────────────
//
// The engine is immediate-mode with NO retained per-layer state: FrameDrawState is recomputed whole
// every frame, so animation is ALREADY supported — a game animates by resubmitting a fresh frame each
// tick with a different Sprite::tile / TileCell::palette. What this layer adds is the ERGONOMICS: an
// Animation value type, a PURE STATELESS resolver (elapsed ticks → current frame), and a game-owned
// "just play it" wrapper. It introduces NO engine state and NO new render path — exactly the
// relationship ENG-2.G's slicer has to draw-time atlas addressing (a device-free convenience layer).
//
// Two animation modes fall out of ONE unit shape: vary the art across frames → sprite/tile animation;
// hold the art constant and vary the palette → palette-cycling animation; vary both → both. Palette is
// an independent per-frame field, so palette animation is not a separate mechanism.

// One unit of an animation: which art (any atlas + cell + size), which palette, for how long, under an
// optional symbolic label. PURE DATA — it references renderer resources by handle and carries NO
// draw-state builder logic (the same generalization discipline as AssetSlot / AtlasManifest: the game
// threads a resolved frame into its Sprite / TileCell — see the application pattern in the guide). The
// art reference is an AtlasId + an AssetSlot (the ENG-2.G { tile, dimensions } pair) so manifest[i]
// drops straight in, and DIFFERENT frames may carry DIFFERENT AtlasIds → frames compose freely from a
// sliced sheet AND arbitrary one-off images.
struct AnimationFrame {
    std::string_view         label{};     // optional symbolic id (empty = unnamed); identity, first member
    AtlasId                  atlas{};      // which uploaded atlas this frame's art lives in
    AssetSlot                slot{};       // { tile, dimensions } — feed to Sprite::tile/size or TileCell::tile
    PaletteId                palette{};    // this frame's palette → enables palette-cycling animation
    std::chrono::nanoseconds duration{};   // how long this frame shows (real time; resolved to ticks)
    [[nodiscard]] bool operator==(const AnimationFrame&) const noexcept = default;
};

// An ordered list of frames. PURE DATA + pure access methods; NO playback STATE and NO loop POLICY live
// here — HOW an animation plays (once / N times / forever / for a duration) is a playback decision
// supplied at play time via PlaybackMode, NOT a property baked into the asset, so the same Animation
// plays once in one place and loops in another. The game owns the elapsed-tick clock. Both ways to
// obtain a frame resolve to the SAME AnimationFrame descriptor: time-driven playback (playbackAt /
// frameAt) and direct programmatic selection (operator[] / find / indexOf). "Play it" and "pin frame N"
// never conflict because the frame set is plain indexable data.
struct Animation {
    std::vector<AnimationFrame> frames;

    [[nodiscard]] std::size_t           count() const noexcept { return frames.size(); }
    [[nodiscard]] const AnimationFrame& operator[](std::size_t i) const { return frames[i]; }  // raw index

    // Programmatic symbolic access: the first frame whose label == name (labels should be unique within
    // an animation, like LayerId within a frame). indexOf → its index (nullopt if absent); find → a
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

// THE pure playback resolver — the single source of playback truth (AnimationPlayer is stateful sugar
// over it). Resolves elapsed ticks under `mode`:
//   LoopIndefinitely   → elapsed modulo totalTicks; never finished.
//   Single             → first pass, then clamp to the last frame; finished once elapsed ≥ totalTicks.
//   LoopNTimes(n)      → wrap for n passes, then hold the last frame; finished once elapsed ≥ n·totalTicks.
//   PlayForDuration(d) → wrap (like LoopIndefinitely) until elapsed ≥ ticksForDuration(d), then hold the
//                        frame shown at the cutoff; finished past d.
// Empty animation → { 0, true }. A zero-tick frame (duration rounds to 0 ticks) is skipped over (never
// the resting frame) so it cannot stall a loop. Durations resolve to ticks via the profile — the engine
// never stores ticks; tick-quantized playback is the honest granularity for a fixed-step sim.
[[nodiscard]] PlaybackState playbackAt(const Animation& anim, std::uint64_t elapsedTicks,
                                       const TimingProfile& profile, PlaybackMode mode) noexcept;

// Convenience: the frame itself (anim[playbackAt(...).frameIndex]). Precondition: anim.count() > 0.
[[nodiscard]] const AnimationFrame& frameAt(const Animation& anim, std::uint64_t elapsedTicks,
                                            const TimingProfile& profile, PlaybackMode mode) noexcept;

// A game-owned playback cursor over an Animation. STATE LIVES HERE, IN THE GAME'S OBJECT — not in the
// engine. Wraps elapsed-tick bookkeeping + play / pause / seek over the pure playbackAt resolver. The
// renderer never sees this; the game constructs it, calls advance() each tick, and threads current()
// into draw state. It is the exact shape of the retained-mode resolution — the engine PROVIDES the
// type, the game OWNS the instance, like std::vector — so it does NOT reopen Issue 14 (an
// engine-tracked or LayerId-keyed player WOULD).
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

    // Each game tick: accrue elapsedTicks (ONLY while playing) and re-resolve via playbackAt under
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
