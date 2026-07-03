#pragma once

// Bongusoid — the GAME-FEEL layer (S2): the "juice" over the plain sim. All of it is game-owned state the
// engine never ticks (the ENG-2.J / Issue-14 contract): the game advances the tween cursors here and the
// renderer reads the resolved values into draw state. Four effects, all photosensitivity-safe (smooth,
// brief, no flashing):
//   • score popups — a "+N" that rises and shrinks away where a brick broke (per-popup Tween cursor).
//   • paddle squash — the Vaus dips its height briefly when the ball bounces off it (Tween → scaleY).
//   • ball spin     — the ball tumbles in the direction of its english (a continuous accumulator → rotation).
//   • screen shake  — a gentle, brief RowDisplacement wobble on a brick break / a lost ball (Tween → amplitude).

#include <cstdint>
#include <optional>
#include <vector>

#include "retropp/draw_state.h"  // ScreenSpaceEffect
#include "retropp/tween.h"       // Tween / TweenPlayer / PlaybackMode

#include "game.h"  // GameEvent / GameEventKind / BongGame

namespace bong {

// A floating "+N" spawned where a brick broke. `progress` runs 0→1 over the popup's life; the renderer
// derives the rise and the shrink from it (so the popup needs no per-frame engine state).
struct ScorePopup {
    float                       x = 0.0f;  // spawn origin (brick top-left), viewport px
    float                       y = 0.0f;
    int                         points = 0;
    int                         id = 0;    // per-spawn identity — the stable key its glyph sprites use
    retropp::TweenPlayer<float> progress{};  // 0→1, Single; points at the shared kPopupTween
};

class BongFeel {
public:
    BongFeel();

    // React to a tick's game events: spawn a popup (BrickBreak), trigger the paddle squash (PaddleBounce),
    // kick the screen shake (BrickBreak / BallLost).
    void onEvent(const GameEvent& e);

    // Per tick, after the sim: accumulate ball spin from the ball's horizontal english.
    void update(const BongGame& game);

    // Advance every cursor one tick and reap finished popups. Call once per tick.
    void tick();

    // ── Read by the renderer ────────────────────────────────────────────────────────────────────────
    [[nodiscard]] float paddleScaleY() const { return paddle_.value(); }   // ~1.0 idle, dips on a bounce
    [[nodiscard]] float ballSpinDegrees() const { return spinDeg_; }       // accumulated tumble
    [[nodiscard]] const std::vector<ScorePopup>& popups() const { return popups_; }
    // The shake as a frame-level effect, or nullopt while idle (so the renderer adds no post-process pass).
    [[nodiscard]] std::optional<retropp::ScreenSpaceEffect> screenShake() const;

    static constexpr std::uint64_t kPopupLifeMs = 600;  // popup rise/shrink duration (renderer reads it too)

private:
    // Continuous ball spin: degrees accumulated from english each tick (not a tween — it has no endpoint).
    float spinDeg_ = 0.0f;

    // Paddle squash + screen shake: short Tween cursors restarted on the triggering event. The shake
    // amplitude tween decays to 0, so a finished shake is silent; `shakeActive_` gates the post-process.
    retropp::TweenPlayer<float> paddle_{};   // scaleY: 1 → dip → 1
    retropp::TweenPlayer<float> shakeAmp_{};  // amplitude: kShakeAmp → 0
    bool  shakeActive_ = false;
    float shakePhase_  = 0.0f;  // advances while a shake runs, for the wobble

    std::vector<ScorePopup> popups_;
    int                     nextPopupId_ = 1;  // monotonic id handed to each spawned popup
};

}  // namespace bong
