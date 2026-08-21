#include "feel.h"

#include <chrono>
#include <cmath>

#include "retropp/timing.h"  // TimingProfile / TickPeriodNs

namespace bong {

using namespace retropp;

namespace {
using namespace std::chrono_literals;

constexpr float kShakeAmp  = 3.5f;   // px — a gentle, brief nudge
constexpr float kSpinPerVx = 7.0f;   // ball-spin degrees per unit of horizontal english, per tick

// Shared tween tracks — static storage, so every TweenPlayer that points at one outlives nothing it
// shouldn't. The game runs at 60 Hz; durations resolve to ticks via kProfile.
const Tween<float> kSquashTween =
    Tween<float>::of(1.0f, 0.80f, 80ms, Easing::OutQuad).then(1.0f, 90ms, Easing::InOutQuad);
const Tween<float> kShakeTween =
    Tween<float>::of(kShakeAmp, 0.0f, 180ms, Easing::OutQuad);
const Tween<float> kPopupTween =
    Tween<float>::of(0.0f, 1.0f, std::chrono::milliseconds(BongFeel::kPopupLifeMs), Easing::OutQuad);

const TimingProfile kProfile{TickPeriodNs::Hz60};
constexpr std::uint64_t kRested = 1'000'000;  // an elapsed count safely past any track's end (starts idle)
}  // namespace

BongFeel::BongFeel() {
    // Start the paddle + shake cursors RESTED (past their end), so neither plays at startup — they only
    // run when restart()ed by an event. Paddle rests at 1.0 (full height); shake rests at 0 (silent).
    paddle_.tween = &kSquashTween;  paddle_.profile = kProfile;  paddle_.elapsedTicks = kRested;
    paddle_.advance(PlaybackMode::single());
    shakeAmp_.tween = &kShakeTween; shakeAmp_.profile = kProfile; shakeAmp_.elapsedTicks = kRested;
    shakeAmp_.advance(PlaybackMode::single());
}

void BongFeel::onEvent(const GameEvent& e) {
    switch (e.kind) {
        case GameEventKind::PaddleBounce:
            paddle_.restart();  // kick the squash
            break;
        case GameEventKind::BrickBreak: {
            ScorePopup p;
            p.x = e.x; p.y = e.y; p.points = e.points;
            p.id = nextPopupId_++;
            p.progress.tween = &kPopupTween;
            p.progress.profile = kProfile;
            p.progress.restart();
            popups_.push_back(p);  // popup only — no screen shake on a brick break (it fires too often)
            break;
        }
        case GameEventKind::BallLost:
            shakeAmp_.restart(); shakeActive_ = true; shakePhase_ = 0.0f;  // a gentle jolt on the rare loss
            break;
        default:
            break;  // serve / wall / brick-hit / level-clear carry no feel beyond their SFX
    }
}

void BongFeel::update(const BongGame& game) {
    // The ball tumbles in the direction of its english; wrap to keep the angle bounded.
    spinDeg_ = std::fmod(spinDeg_ + game.ballVx * kSpinPerVx, 360.0f);
}

void BongFeel::tick() {
    paddle_.advance(PlaybackMode::single());
    shakeAmp_.advance(PlaybackMode::single());
    if (shakeActive_) {
        shakePhase_ += 0.85f;  // advance the wobble while the shake runs
        if (shakeAmp_.finished()) shakeActive_ = false;
    }
    for (ScorePopup& p : popups_) p.progress.advance(PlaybackMode::single());
    std::erase_if(popups_, [](const ScorePopup& p) { return p.progress.finished(); });
}

std::optional<ScreenSpaceEffect> BongFeel::screenShake() const {
    if (!shakeActive_) return std::nullopt;
    ScreenSpaceEffect e{};
    e.kind      = ScreenSpaceEffectKind::RowDisplacement;
    e.amplitude = shakeAmp_.value();           // tween-decayed magnitude
    e.frequency = 3.0f;
    e.phase     = shakePhase_;
    e.axis      = Axis::Horizontal;            // rows slide horizontally → a brief side-to-side judder
    e.edge      = DisplacementEdge::Blank;     // exposed strip shows the backdrop, not a smear
    return e;
}

}  // namespace bong
