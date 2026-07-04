#include "waves.h"

#include <algorithm>
#include <cmath>
#include <span>

namespace vant {

using namespace retropp;

namespace {

std::uint32_t nextRand(std::uint32_t& s) { s = s * 1664525u + 1013904223u; return s; }

float frand(std::uint32_t& s, float lo, float hi) {
    return lo + (hi - lo) * (static_cast<float>(nextRand(s) % 10000u) / 10000.0f);
}

}  // namespace

void WaveSystem::resetQuota(int shipNum) {
    wavesCleared_ = 0;
    quota_        = std::min(kQuotaBase + (shipNum - 1), kQuotaCap);
    waveSpeed_    = kWaveSpeedBase +
                    kWaveSpeedGrow * static_cast<float>(std::min(shipNum, kWaveSpeedCapShip) - 1);
    landNowSent_  = false;
    reset(shipNum);
}

void WaveSystem::reset(int /*shipNum*/) {
    wave_ = Wave{};
    mines_.clear();
    eshots_.clear();
    lull_ = kWaveLull;
}

void WaveSystem::spawnWave(Vec2 ship, float camX, int facing, std::uint32_t& rng) {
    wave_            = Wave{};
    wave_.id         = nextWaveId_++;
    wave_.running    = true;
    wave_.livery     = (wave_.id - 1) % kFighterLiveries;
    wave_.headsRight = facing < 0;  // squadrons cross AGAINST the player's facing — head-on
    for (Fighter& f : wave_.members) {
        f.alive        = true;
        f.fireCooldown = 90 + static_cast<int>(nextRand(rng) % 60u);
    }
    // The path: enter ahead of the player's view, swoop through 5 waypoints spread across the
    // deck's altitude band, exit behind. Catmull-Rom through the points, baked once — the whole
    // squadron then walks ONE ArcLengthTable at constant speed. (`ship` biases a mid waypoint so
    // the swoop leans toward the Manta without aiming at it.)
    const float entryX = wave_.headsRight ? camX - 80.0f : camX + kViewW + 80.0f;
    const float exitX  = wave_.headsRight ? camX + kViewW + 160.0f : camX - 160.0f;
    std::array<Vec2, 5> pts{};
    for (int i = 0; i < 5; ++i) {
        const float t = static_cast<float>(i) / 4.0f;
        pts[static_cast<std::size_t>(i)] =
            Vec2{entryX + (exitX - entryX) * t, frand(rng, 72.0f, 408.0f)};
    }
    pts[2].y = std::clamp(ship.y + frand(rng, -60.0f, 60.0f), 72.0f, 408.0f);
    const Curve path = Curve::throughPoints(std::span<const Vec2>(pts));
    wave_.path    = path.arcTable();
    wave_.pathLen = wave_.path->length();
    wave_.headS   = 0.0f;
}

Vec2 WaveSystem::fighterPos(int i) const {
    const float s = wave_.headS - kWaveSpacing * static_cast<float>(i);
    return wave_.path->atDistance(std::clamp(s, 0.0f, wave_.pathLen));
}

void WaveSystem::killFighter(int i, std::vector<GameEvent>& events) {
    Fighter& f = wave_.members[static_cast<std::size_t>(i)];
    if (!f.alive) return;
    f.alive = false;
    ++wave_.killed;
    const Vec2 p = fighterPos(i);
    events.push_back(GameEvent{GameEventKind::EnemyDown, p.x, p.y, kScoreFighter});
    if (wave_.killed == kWaveSize) {
        events.push_back(GameEvent{GameEventKind::WaveBonus, p.x, p.y, kScoreWave});
    }
}

void WaveSystem::tick(const Deck& deck, Vec2 ship, bool shipAlive, float camX, int facing,
                      std::uint32_t& rng, std::vector<GameEvent>& events) {
    // ── The squadron. ──────────────────────────────────────────────────────────────────────────
    if (wave_.running) {
        wave_.headS += waveSpeed_;
        const float tailS = wave_.headS - kWaveSpacing * static_cast<float>(kWaveSize - 1);
        if (tailS > wave_.pathLen) {  // the whole file has crossed — the wave is over
            wave_.running = false;
            ++wavesCleared_;          // survived or died, the assault counts toward the quota
            lull_ = kWaveLull;
        } else if (shipAlive) {
            for (int i = 0; i < kWaveSize; ++i) {
                Fighter& f = wave_.members[static_cast<std::size_t>(i)];
                if (!f.alive) continue;
                const Vec2 p = fighterPos(i);
                if (p.x < camX - 16 || p.x > camX + kViewW + 16) continue;  // holds fire off screen
                if (--f.fireCooldown > 0) continue;
                f.fireCooldown = 90 + static_cast<int>(nextRand(rng) % 60u);
                const float dx  = ship.x - p.x, dy = ship.y - p.y;
                const float len = std::max(1.0f, std::sqrt(dx * dx + dy * dy));
                eshots_.push_back(EnemyShot{nextShotId_++, true, p.x, p.y,
                                            dx / len * kEShotSpeed, dy / len * kEShotSpeed});
            }
        }
    } else if (!quotaMet()) {
        if (--lull_ <= 0) spawnWave(ship, camX, facing, rng);
    } else if (!landNowSent_) {
        landNowSent_ = true;
        events.push_back(GameEvent{GameEventKind::LandNow});
    }

    // ── Enemy shots: straight flight; gone off the world or into raised structure. ────────────
    for (EnemyShot& s : eshots_) {
        if (!s.alive) continue;
        s.x += s.vx;
        s.y += s.vy;
        if (s.x < -16 || s.x > kWorldW + 16 || s.y < -16 || s.y > kViewH + 16 ||
            deck.rectHitsSolid(s.x - 4, s.y - 4, 8, 8)) {
            s.alive = false;
        }
    }
    std::erase_if(eshots_, [](const EnemyShot& s) { return !s.alive; });

    // ── Mines: rise from any spawn point that scrolls into view (max 2 alive), then home. ─────
    int alive = static_cast<int>(mines_.size());
    if (alive < kMaxMines) {
        for (const VCell& sp : deck.mineSpawns()) {
            if (alive >= kMaxMines) break;
            const float sx = deckPxX(sp.c) + 8.0f, sy = deckPxY(sp.r) + 8.0f;
            if (sx < camX + 32 || sx > camX + kViewW - 32) continue;
            bool near = false;  // one mine per spawn point at a time
            for (const Mine& m : mines_) {
                if (std::abs(m.x - sx) < static_cast<float>(kCell) * 2) near = true;
            }
            if (near || (nextRand(rng) % 120u) != 0) continue;  // a lazy, irregular trickle
            mines_.push_back(Mine{nextMineId_++, true, sx, sy, 0});
            ++alive;
        }
    }
    for (Mine& m : mines_) {
        if (!shipAlive) break;
        const float dx = ship.x - m.x, dy = ship.y - m.y;
        const float len = std::max(1.0f, std::sqrt(dx * dx + dy * dy));
        m.x += dx / len * kMineSpeed;
        m.y += dy / len * kMineSpeed;
    }
}

}  // namespace vant
