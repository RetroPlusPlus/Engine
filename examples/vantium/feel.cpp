#include "feel.h"

#include <chrono>

#include "retropp/timing.h"

namespace vant {

using namespace retropp;

namespace {
using namespace std::chrono_literals;

constexpr float kJoltAmp   = 3.5f;  // the death jolt — under the ≤4px cap
constexpr float kRumbleAmp = 2.5f;  // the destruct rumble — long but gentle

const Tween<float> kJoltTween   = Tween<float>::of(kJoltAmp, 0.0f, 200ms, Easing::OutQuad);
const Tween<float> kRumbleTween =
    Tween<float>::of(kRumbleAmp, kRumbleAmp, 1800ms, Easing::Linear).then(0.0f, 700ms, Easing::OutQuad);
const Tween<float> kPopupTween =
    Tween<float>::of(0.0f, 1.0f, std::chrono::milliseconds(VantFeel::kPopupLifeMs), Easing::OutQuad);
// The destruct dim: two slow Multiply dips and back — a groan, never a strobe.
const Tween<float> kDimTween = Tween<float>::of(1.0f, 0.72f, 700ms, Easing::InOutSine)
                                   .then(1.0f, 600ms, Easing::InOutSine)
                                   .then(0.72f, 700ms, Easing::InOutSine)
                                   .then(1.0f, 500ms, Easing::InOutSine);

const TimingProfile kProfile{TickPeriodNs::Hz60};
constexpr std::uint64_t kRested = 1'000'000;
}  // namespace

VantFeel::VantFeel(const VantAssets& assets) : boomClip_(&assets.boomClip) {
    shakeAmp_.tween = &kJoltTween;  shakeAmp_.profile = kProfile;  shakeAmp_.elapsedTicks = kRested;
    shakeAmp_.advance(PlaybackMode::single());
    dim_.tween = &kDimTween;        dim_.profile = kProfile;       dim_.elapsedTicks = kRested;
    dim_.advance(PlaybackMode::single());
    pod_.animation  = &assets.podPulse;
    pod_.profile    = kProfile;
    star_.animation = &assets.starTwinkle;
    star_.profile   = kProfile;
}

void VantFeel::onEvent(const GameEvent& e) {
    switch (e.kind) {
        case GameEventKind::EnemyDown:
        case GameEventKind::MineDown:
        case GameEventKind::PlayerDeath: {
            Boom b;
            b.x = e.x;
            b.y = e.y;
            b.id = nextId_++;
            b.player.animation = boomClip_;
            b.player.profile   = kProfile;
            booms_.push_back(b);
            if (e.kind == GameEventKind::PlayerDeath) {
                shakeAmp_.tween = &kJoltTween;
                shakeAmp_.restart();
                shakeActive_ = true;
                shakePhase_  = 0.0f;
            }
            if (e.points > 0) {
                ScorePopup p;
                p.x = e.x;
                p.y = e.y;
                p.points = e.points;
                p.id = nextId_++;
                p.progress.tween   = &kPopupTween;
                p.progress.profile = kProfile;
                p.progress.restart();
                popups_.push_back(p);
            }
            break;
        }
        case GameEventKind::PodHit:
        case GameEventKind::WaveBonus:
        case GameEventKind::Touchdown: {
            ScorePopup p;
            p.x = e.x;
            p.y = e.y;
            p.points = e.points;
            p.id = nextId_++;
            p.progress.tween   = &kPopupTween;
            p.progress.profile = kProfile;
            p.progress.restart();
            popups_.push_back(p);
            break;
        }
        case GameEventKind::ShipDestroyed:
            shakeAmp_.tween = &kRumbleTween;   // the long, gentle scuttling rumble
            shakeAmp_.restart();
            shakeActive_ = true;
            shakePhase_  = 0.0f;
            dim_.restart();
            break;
        default:
            break;  // Fire / LandNow carry no feel beyond their SFX / HUD pulse
    }
}

void VantFeel::tick() {
    shakeAmp_.advance(PlaybackMode::single());
    dim_.advance(PlaybackMode::single());
    pod_.advance();   // loops forever — the glow breathes
    star_.advance();  // loops forever — the field shimmers
    if (shakeActive_) {
        shakePhase_ += 0.8f;
        if (shakeAmp_.finished()) shakeActive_ = false;
    }
    for (Boom& b : booms_) b.player.advance(PlaybackMode::single());
    std::erase_if(booms_, [](const Boom& b) { return b.player.finished(); });
    for (ScorePopup& p : popups_) p.progress.advance(PlaybackMode::single());
    std::erase_if(popups_, [](const ScorePopup& p) { return p.progress.finished(); });
}

std::optional<ScreenSpaceEffect> VantFeel::screenShake() const {
    if (!shakeActive_) return std::nullopt;
    ScreenSpaceEffect e{};
    e.kind      = ScreenSpaceEffectKind::RowDisplacement;
    e.amplitude = shakeAmp_.value();
    e.frequency = 3.0f;
    e.phase     = shakePhase_;
    e.axis      = Axis::Horizontal;
    e.edge      = DisplacementEdge::Blank;
    return e;
}

float VantFeel::destructDim() const { return dim_.value(); }

}  // namespace vant
