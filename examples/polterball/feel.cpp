#include "feel.h"

#include <chrono>

#include "retropp/timing.h"  // TimingProfile / TickPeriodNs

namespace polter {

using namespace retropp;

namespace {
using namespace std::chrono_literals;

constexpr float kShakeAmp = 3.5f;  // px — a gentle, brief nudge

// Shared tween tracks — static storage, so every TweenPlayer that points at one outlives nothing it
// shouldn't. The game runs at 60 Hz; durations resolve to ticks via kProfile.
const Tween<float> kSquashTween =
    Tween<float>::of(1.0f, 0.80f, 80ms, Easing::OutQuad).then(1.0f, 90ms, Easing::InOutQuad);
const Tween<float> kShakeTween =
    Tween<float>::of(kShakeAmp, 0.0f, 180ms, Easing::OutQuad);
const Tween<float> kPopupTween =
    Tween<float>::of(0.0f, 1.0f, std::chrono::milliseconds(PolterFeel::kPopupLifeMs), Easing::OutQuad);

const TimingProfile kProfile{TickPeriodNs::Hz60};
constexpr std::uint64_t kRested = 1'000'000;  // an elapsed count safely past any track's end
}  // namespace

PolterFeel::PolterFeel(const PolterAssets& assets) {
    // Start the squash + shake cursors RESTED (past their end), so neither plays at startup — they
    // only run when restart()ed by an event. The squash rests at 1.0 (full height); the shake at 0.
    paddle_.tween = &kSquashTween;  paddle_.profile = kProfile;  paddle_.elapsedTicks = kRested;
    paddle_.advance(PlaybackMode::single());
    shakeAmp_.tween = &kShakeTween; shakeAmp_.profile = kProfile; shakeAmp_.elapsedTicks = kRested;
    shakeAmp_.advance(PlaybackMode::single());
    // The two looping clips run from tick 0 for the whole program (bare advance() loops forever).
    power_.animation = &assets.powerPulse;
    power_.profile   = kProfile;
    walk_.animation  = &assets.ghostWalk;
    walk_.profile    = kProfile;
}

void PolterFeel::onEvent(const GameEvent& e) {
    switch (e.kind) {
        case GameEventKind::PaddleBounce:
            paddle_.restart();  // kick the squash
            break;
        case GameEventKind::GhostDown: {
            ScorePopup p;
            p.x = e.x;
            p.y = e.y;
            p.points = e.points;
            p.id = nextPopupId_++;
            p.progress.tween   = &kPopupTween;
            p.progress.profile = kProfile;
            p.progress.restart();
            popups_.push_back(p);
            break;
        }
        case GameEventKind::BallSwallowed:
        case GameEventKind::BallLost:
            shakeAmp_.restart();
            shakeActive_ = true;
            shakePhase_  = 0.0f;
            break;
        default:
            break;  // serve / wall / pellet / break / ignite / clear carry no feel beyond their SFX
    }
}

void PolterFeel::tick() {
    paddle_.advance(PlaybackMode::single());
    shakeAmp_.advance(PlaybackMode::single());
    power_.advance();  // loops by default
    walk_.advance();
    if (shakeActive_) {
        shakePhase_ += 0.85f;  // advance the wobble while the shake runs
        if (shakeAmp_.finished()) shakeActive_ = false;
    }
    for (ScorePopup& p : popups_) p.progress.advance(PlaybackMode::single());
    std::erase_if(popups_, [](const ScorePopup& p) { return p.progress.finished(); });
}

std::optional<ScreenSpaceEffect> PolterFeel::screenShake() const {
    if (!shakeActive_) return std::nullopt;
    ScreenSpaceEffect e{};
    e.kind      = ScreenSpaceEffectKind::RowDisplacement;
    e.amplitude = shakeAmp_.value();        // tween-decayed magnitude
    e.frequency = 3.0f;
    e.phase     = shakePhase_;
    e.axis      = Axis::Horizontal;         // rows slide horizontally → a brief side-to-side judder
    e.edge      = DisplacementEdge::Blank;  // exposed strip shows the backdrop, not a smear
    return e;
}

}  // namespace polter
