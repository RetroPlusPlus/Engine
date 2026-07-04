#include "game.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

namespace vant {

using namespace retropp;

VantGame::VantGame()
    : rng(static_cast<std::uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count())) {}

void VantGame::newGame(const VantAssets& assets) {
    shipNum = 1;
    score   = 0;
    lives   = kLives;
    startShip(assets);
}

void VantGame::startShip(const VantAssets& assets) {
    deck.build(shipNum, assets);
    waves.resetQuota(shipNum);
    phase = FlightPhase::Flying;
    ++lifeEpoch;
    shipX = 64;
    shipY = 48;
    shipVx = 0;
    shipVy = 0;
    facing = 1;
    rolled = false;
    turnTicks = 0;
    invulnTicks = kInvulnTicks;
    camX = 0;
    for (PlayerShot& s : shots) s.alive = false;
}

void VantGame::respawn() {
    ++lifeEpoch;  // the Manta teleports — a fresh key mount-snaps it, no streak
    shipX = camX + 96;
    shipY = 48;   // above the deck band: always a safe altitude
    shipVx = 0;
    shipVy = 0;
    facing = 1;
    rolled = false;
    turnTicks = 0;
    invulnTicks = kInvulnTicks;
    waves.reset(shipNum);  // the assault regroups; quota PROGRESS is kept (the classic mercy)
}

void VantGame::die() {
    emit(GameEventKind::PlayerDeath, shipX + kMantaW / 2, shipY + kMantaH / 2);
    if (--lives <= 0) {
        std::printf("game over — score %d — back to title\n", score);
        state = GameState::Title;
        return;
    }
    respawn();
}

void VantGame::fire() {
    for (PlayerShot& s : shots) {
        if (s.alive) continue;
        s.alive = true;
        s.id    = nextShotId_++;
        s.x     = facing > 0 ? shipX + kMantaW - 4 : shipX - 12;
        s.y     = shipY + kMantaH / 2 - 4;
        s.vx    = kShotSpeed * static_cast<float>(facing);
        emit(GameEventKind::Fire, s.x, s.y);
        return;  // one bolt per press; kMaxShots caps what's in the air
    }
}

void VantGame::shipHitbox(float& x, float& y, float& w, float& h) const {
    x = shipX + kHitInsetX;
    w = kMantaW - 2 * kHitInsetX;
    if (rolled) {
        y = shipY + kHitRollY;
        h = kHitRollH;
    } else {
        y = shipY + kHitLevelY;
        h = kHitLevelH;
    }
}

void VantGame::tick(const InputState& in, const VantAssets& assets) {
    events_.clear();

    if (state == GameState::Title) {
        if (in.justPressed(Button::Start)) {
            state = GameState::Playing;
            newGame(assets);
        }
        return;
    }

    // ── The scripted phases: touchdown roll, then the destruct sequence. ──────────────────────
    if (phase == FlightPhase::Landing) {
        shipVx *= 0.94f;
        shipX += shipVx;
        shipY += (deck.stripY() - 22.0f - shipY) * 0.2f;  // settle onto the strip surface
        if (--landingTicks <= 0) {
            score += kScoreLanding;
            emit(GameEventKind::Touchdown, shipX, shipY, kScoreLanding);
            emit(GameEventKind::ShipDestroyed, shipX, shipY);
            phase         = FlightPhase::Destruct;
            destructTicks = kDestructTicks;
        }
        camX = std::clamp(shipX + static_cast<float>(facing * kCamLookahead) - kViewW / 2.0f,
                          0.0f, static_cast<float>(kWorldW - kViewW));
        return;
    }
    if (phase == FlightPhase::Destruct) {
        if (--destructTicks <= 0) {
            ++shipNum;
            std::printf("dreadnought %d scuttled — score %d — next ship\n", shipNum - 1, score);
            startShip(assets);
        }
        return;
    }

    // ── Flight input: inertia both axes, facing turn, roll, fire. ─────────────────────────────
    thrustDir = 0;
    if (in.isHeld(Button::Left)) {
        shipVx -= kAccel;
        thrustDir = -1;
        if (facing > 0 && shipVx < 0) { facing = -1; turnTicks = kTurnTicks; }
    }
    if (in.isHeld(Button::Right)) {
        shipVx += kAccel;
        thrustDir = 1;
        if (facing < 0 && shipVx > 0) { facing = 1; turnTicks = kTurnTicks; }
    }
    bank = 0;
    if (in.isHeld(Button::Up))   { shipVy -= kAccel; bank = shipVy < -1.8f ? -2 : -1; }
    if (in.isHeld(Button::Down)) { shipVy += kAccel; bank = shipVy > 1.8f ? 2 : 1; }
    shipVx = std::clamp(shipVx * kDrag, -kVMaxX, kVMaxX);
    shipVy = std::clamp(shipVy * kDrag, -kVMaxY, kVMaxY);
    shipX += shipVx;
    shipY += shipVy;
    shipX = std::clamp(shipX, 0.0f, static_cast<float>(kWorldW) - kMantaW);
    shipY = std::clamp(shipY, kShipMinY, kShipMaxY);
    if (turnTicks > 0) --turnTicks;
    if (invulnTicks > 0) --invulnTicks;

    rolled = in.isHeld(Button::B);                        // guns cold, hitbox thin
    if (in.justPressed(Button::A) && !rolled) fire();

    // The camera leads the facing, eased — a turn pans the view ahead smoothly.
    const float camTarget = std::clamp(
        shipX + kMantaW / 2 + static_cast<float>(facing * kCamLookahead) - kViewW / 2.0f,
        0.0f, static_cast<float>(kWorldW - kViewW));
    camX += (camTarget - camX) * kCamEase;

    // ── The Manta vs the superstructure (the roll is the whole trade). ────────────────────────
    float hx, hy, hw, hh;
    shipHitbox(hx, hy, hw, hh);
    if (invulnTicks == 0 && deck.rectHitsSolid(hx, hy, hw, hh)) {
        die();
        return;
    }

    // ── Player bolts: fly, stop on raised structure, hit fighters / mines / pods. ─────────────
    for (PlayerShot& s : shots) {
        if (!s.alive) continue;
        s.x += s.vx;
        if (s.x < camX - 64 || s.x > camX + kViewW + 64 ||
            deck.rectHitsSolid(s.x, s.y, 16, 8)) {
            s.alive = false;
            continue;
        }
        const int pod = deck.podAt(s.x + (s.vx > 0 ? 14.0f : 2.0f), s.y + 4);
        if (pod >= 0) {
            deck.scorchPod(pod, assets);
            score += kScorePod;
            emit(GameEventKind::PodHit, s.x, s.y, kScorePod);
            s.alive = false;
            continue;
        }
        Wave& w = waves.wave();
        if (w.running && w.path) {
            for (int i = 0; i < kWaveSize && s.alive; ++i) {
                if (!w.members[static_cast<std::size_t>(i)].alive) continue;
                const Vec2 p = waves.fighterPos(i);
                if (s.x < p.x + 16 && s.x + 16 > p.x - 16 && s.y < p.y + 8 && s.y + 8 > p.y - 8) {
                    score += kScoreFighter;
                    waves.killFighter(i, events_);   // EnemyDown (+ WaveBonus on the fifth)
                    if (w.killed == kWaveSize) score += kScoreWave;
                    s.alive = false;
                }
            }
        }
        for (Mine& m : waves.mines()) {
            if (!s.alive || !m.alive) continue;
            if (s.x < m.x + 8 && s.x + 16 > m.x - 8 && s.y < m.y + 8 && s.y + 8 > m.y - 8) {
                m.alive = false;
                score += kScoreMine;
                emit(GameEventKind::MineDown, m.x, m.y, kScoreMine);
                s.alive = false;
            }
        }
    }
    std::erase_if(waves.mines(), [](const Mine& m) { return !m.alive; });

    // ── The enemy layer moves (squadron / shots / mines), then bites. ─────────────────────────
    waves.tick(deck, Vec2{shipX + kMantaW / 2, shipY + kMantaH / 2}, invulnTicks == 0, camX,
               facing, rng, events_);

    if (invulnTicks == 0) {
        shipHitbox(hx, hy, hw, hh);
        for (const EnemyShot& s : waves.shots()) {
            if (s.x + 4 > hx && s.x - 4 < hx + hw && s.y + 4 > hy && s.y - 4 < hy + hh) {
                die();
                return;
            }
        }
        const Wave& w = waves.wave();
        if (w.running && w.path) {
            for (int i = 0; i < kWaveSize; ++i) {
                if (!w.members[static_cast<std::size_t>(i)].alive) continue;
                const Vec2 p = waves.fighterPos(i);
                if (p.x + 14 > hx && p.x - 14 < hx + hw && p.y + 7 > hy && p.y - 7 < hy + hh) {
                    die();
                    return;
                }
            }
        }
        for (const Mine& m : waves.mines()) {
            if (m.x + 7 > hx && m.x - 7 < hx + hw && m.y + 7 > hy && m.y - 7 < hy + hh) {
                die();
                return;
            }
        }
    }

    // ── Landing: quota met + over the strip + inside the speed envelope + Down. ───────────────
    if (waves.quotaMet() && in.justPressed(Button::Down) &&
        shipX > deck.stripX0() && shipX + kMantaW < deck.stripX1() &&
        shipY + kMantaH > deck.stripY() - 40.0f && shipY < deck.stripY() + 24.0f &&
        std::abs(shipVx) <= kLandMaxVx && std::abs(shipVy) <= kLandMaxVy) {
        phase        = FlightPhase::Landing;
        landingTicks = kLandingTicks;
        rolled       = false;
    }
}

}  // namespace vant
