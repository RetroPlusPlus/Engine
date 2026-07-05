#include "game.h"

#include <chrono>
#include <cmath>
#include <cstdio>

#include "retropp/timing.h"  // TimingProfile / TickPeriodNs — durations → ticks

namespace ferryman {

using namespace retropp;

namespace {
using namespace std::chrono_literals;

const TimingProfile kProfile{TickPeriodNs::Hz60};
const std::uint64_t kInvulnTicks      = kProfile.ticksForDuration(2500ms);
const std::uint64_t kWaveLullTicks    = kProfile.ticksForDuration(2200ms);
const std::uint64_t kColonistSpawnGap = kProfile.ticksForDuration(4s);
const std::uint64_t kStunTicks        = kProfile.ticksForDuration(4s);
const std::uint64_t kAbductorFirst    = kProfile.ticksForDuration(8s);
const std::uint64_t kAbductorReentry  = kProfile.ticksForDuration(10s);
const std::uint64_t kMutantDelay      = kProfile.ticksForDuration(5s);

constexpr float kDegToRad = 3.14159265f / 180.0f;
constexpr float kFieldTopF = static_cast<float>(kFieldTop);

// A tiny clock-seeded LCG (variety per run; native game, no VM involved).
std::uint32_t nextRand(std::uint32_t& s) { s = s * 1664525u + 1013904223u; return s; }

float frand(std::uint32_t& s) { return static_cast<float>(nextRand(s) % 10000u) / 10000.0f; }

float distTo(float ax, float ay, float bx, float by) { return std::hypot(ax - bx, ay - by); }

// Centre-based AABB overlap with per-object box sizes (full widths/heights).
bool boxesOverlap(float ax, float ay, float aw, float ah,
                  float bx, float by, float bw, float bh) {
    return std::abs(ax - bx) < (aw + bw) / 2.0f && std::abs(ay - by) < (ah + bh) / 2.0f;
}

// The per-wave enemy roster — density ramps, geometry never randomizes.
int corsairTarget(int wave) { return std::min(2 + (wave - 1) / 2, 5); }
int wardenTarget(int wave) { return std::min(1 + (wave - 1) / 3, 3); }
int dreadTarget(int wave) {
    return wave >= kDreadFirstWave ? std::min(1 + (wave - kDreadFirstWave) / 3, 2) : 0;
}

}  // namespace

FerrymanGame::FerrymanGame()
    : rng(static_cast<std::uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count())) {}

const Colonist* FerrymanGame::colonistById(int id) const {
    for (const Colonist& c : colonists)
        if (c.id == id) return &c;
    return nullptr;
}

void FerrymanGame::newGame() {
    score   = 0;
    lives   = kLives;
    wave    = 0;
    rescued = 0;
    fireRateMul = 1.0f;
    waveLull = 0;
    deck.clear();
    colonists.clear();
    enemies.clear();
    bolts.clear();
    mutantQueue.clear();
    respawnQueue.clear();
    carried[0].reset();
    carried[1].reset();
    colonistSpawnTimer_ = 0;
    cargoFireTimer_     = 0;
    randomizeIslets();
    respawnFerry();
    // A couple of souls already wait on the islets, so the first run out has a purpose.
    for (int i = 0; i < 2; ++i) {
        const IsletSpec& islet = islets[static_cast<std::size_t>(nextRand(rng) % islets.size())];
        Colonist c;
        c.id   = nextId_++;
        c.look = c.id % kColonistLooks;
        c.x    = isletCenterX(islet) + (frand(rng) - 0.5f) * 20.0f;
        c.y    = isletCenterY(islet) + (frand(rng) - 0.5f) * 10.0f;
        colonists.push_back(c);
    }
    spawnWave();
}

void FerrymanGame::randomizeIslets() {
    // Roll this run's archipelago — the random-tilemap demo's ergonomic applied to geography:
    // sample within the valid vocabulary (field rows, 1- or 2-block widths, the prop set) and
    // reject anything violating the spacing constraint, so sea lanes always stay open. Rejection
    // sampling with a guard: a crowded roll just yields a slightly sparser sea, never a spin.
    islets.clear();
    const int want = kIsletCountMin +
                     static_cast<int>(nextRand(rng) %
                                      static_cast<unsigned>(kIsletCountMax - kIsletCountMin + 1));
    for (int guard = 0; static_cast<int>(islets.size()) < want && guard < 400; ++guard) {
        IsletSpec s{};
        s.tilesW = nextRand(rng) % 2u == 0u ? 2 : 1;
        s.blockX = static_cast<int>(nextRand(rng) %
                                    static_cast<unsigned>(kBlockCols - s.tilesW + 1));
        s.blockY = kIsletRowMin + static_cast<int>(nextRand(rng) %
                                                   static_cast<unsigned>(kIsletRowMax -
                                                                         kIsletRowMin + 1));
        bool clear = true;
        for (const IsletSpec& o : islets) {
            const int gapX = std::max({o.blockX - (s.blockX + s.tilesW - 1),
                                       s.blockX - (o.blockX + o.tilesW - 1), 0});
            const int gapY = std::abs(o.blockY - s.blockY);
            if (std::max(gapX, gapY) < kIsletSpacing) {
                clear = false;
                break;
            }
        }
        if (!clear) continue;
        if (s.tilesW == 2) {  // some pairs carry a prop: a buoy, a mooring post, a lamp — or none
            switch (nextRand(rng) % 4u) {
                case 0:  s.prop = T_BUOY; break;
                case 1:  s.prop = T_MOORING; break;
                case 2:  s.prop = T_LAMP; break;
                default: s.prop = 0; break;
            }
        }
        islets.push_back(s);
    }
    if (islets.empty())  // unreachable in practice; the guarantee costs one line
        islets.push_back(IsletSpec{9, 7, 2, 0});
}

void FerrymanGame::respawnFerry() {
    ++lifeNum;  // the ferry teleports home — a fresh key makes it mount-snap, not streak
    ferryX     = kViewW / 2.0f;
    ferryY     = kViewH - 80.0f;
    invulnLeft = static_cast<int>(kInvulnTicks);
    ferryFacingLeft = false;
}

void FerrymanGame::spawnEnemy(int kind) {
    Enemy e;
    e.id   = nextId_++;
    e.kind = kind;
    e.hp   = kEnemyHp[static_cast<std::size_t>(kind)];
    e.fireTimer =
        60 + static_cast<int>(frand(rng) * 120.0f);  // staggered first volleys, never a chorus
    switch (kind) {
        case EK_CORSAIR: {
            // A straight crossing run: enter on a side edge, fly a gentle diagonal.
            const bool fromLeft = nextRand(rng) % 2u == 0u;
            e.x  = fromLeft ? -kEnemyW[EK_CORSAIR] : kViewW + kEnemyW[EK_CORSAIR];
            e.y  = kFieldTopF + 60.0f + frand(rng) * (kViewH - kFieldTopF - 160.0f);
            e.vx = (fromLeft ? 1.0f : -1.0f) * kCorsairSpeed;
            e.vy = (frand(rng) - 0.5f) * 0.6f;
            break;
        }
        case EK_WARDEN: {
            // A patrol orbit around a mid-sea anchor; it drifts in from above its station.
            e.anchorX = 100.0f + frand(rng) * (kViewW - 200.0f);
            e.anchorY = kFieldTopF + 120.0f + frand(rng) * (kViewH - kFieldTopF - 220.0f);
            e.orbit   = frand(rng) * 6.28f;
            e.x       = e.anchorX;
            e.y       = kFieldTopF - 30.0f;
            break;
        }
        case EK_DREAD: {
            // The ponderous horizontal crossing, upper-mid sea.
            const bool fromLeft = nextRand(rng) % 2u == 0u;
            e.x  = fromLeft ? -kEnemyW[EK_DREAD] : kViewW + kEnemyW[EK_DREAD];
            e.y  = kFieldTopF + 90.0f + frand(rng) * 120.0f;
            e.vx = (fromLeft ? 1.0f : -1.0f) * kDreadSpeed;
            break;
        }
        default: {  // EK_MUTANT — arrives at the top, where its abductor left
            e.x = 40.0f + frand(rng) * (kViewW - 80.0f);
            e.y = kFieldTopF + 10.0f;
            break;
        }
    }
    enemies.push_back(e);
}

void FerrymanGame::spawnWave() {
    ++wave;
    rescued     = 0;
    fireRateMul = std::max(std::pow(kFireRateGrow, static_cast<float>(wave - 1)), kFireRateFloor);
    // Top the roster up to this wave's targets (survivors keep flying; newcomers enter from the
    // edges, so the field thickens without anything popping into view).
    int have[3] = {0, 0, 0};
    for (const Enemy& e : enemies)
        if (e.kind < 3) ++have[e.kind];
    for (int k = have[EK_CORSAIR]; k < corsairTarget(wave); ++k) spawnEnemy(EK_CORSAIR);
    for (int k = have[EK_WARDEN]; k < wardenTarget(wave); ++k) spawnEnemy(EK_WARDEN);
    for (int k = have[EK_DREAD]; k < dreadTarget(wave); ++k) spawnEnemy(EK_DREAD);
    const int base = static_cast<int>(kAbductorFirst);
    abductors[0].reset(base);
    abductors[1].reset(base + base / 2);  // the second (once active) arrives on its own beat
}

void FerrymanGame::moveInput(const InputState& in) {
    // 8-direction sailing: d-pad / arrows, WASD aliases (W→R, A→Y, D→L per the default keymap +
    // one rebind; S is polled raw by the host), and the analog stick.
    float dx = 0.0f, dy = 0.0f;
    if (in.isHeld(Button::Left) || in.isHeld(Button::Y))  dx -= 1.0f;
    if (in.isHeld(Button::Right) || in.isHeld(Button::L)) dx += 1.0f;
    if (in.isHeld(Button::Up) || in.isHeld(Button::R))    dy -= 1.0f;
    if (in.isHeld(Button::Down) || rawDownHeld)           dy += 1.0f;
    const Vec2 stick = in.stick(Stick::Left);
    if (std::abs(stick.x) > 0.25f) dx += stick.x;
    if (std::abs(stick.y) > 0.25f) dy += stick.y;
    const float mag = std::hypot(dx, dy);
    if (mag < 0.01f) return;
    // THE WEIGHT RULE: every soul aboard slows the ferry — cargo is the difficulty.
    const float speed =
        kFerrySpeedBase - kFerrySpeedPerPax * static_cast<float>(deck.size());
    ferryX += dx / mag * speed;
    ferryY += dy / mag * speed;
    if (dx != 0.0f) ferryFacingLeft = dx < 0.0f;
    // The field edges clamp — no wrap, the sea has a shore.
    ferryX = std::clamp(ferryX, kFerryW / 2.0f, kViewW - kFerryW / 2.0f);
    ferryY = std::clamp(ferryY, kFieldTopF + kFerryH / 2.0f, kViewH - kFerryH / 2.0f);
}

void FerrymanGame::colonistFlow() {
    // The islet trickle: a new soul every few seconds while fewer than the cap wait.
    int waiting = 0;
    for (const Colonist& c : colonists)
        if (c.state != ColonistState::Aboard) ++waiting;
    if (waiting < kMaxWaiting && --colonistSpawnTimer_ <= 0) {
        colonistSpawnTimer_ = static_cast<int>(kColonistSpawnGap);
        const IsletSpec& islet = islets[static_cast<std::size_t>(nextRand(rng) % islets.size())];
        Colonist c;
        c.id   = nextId_++;
        c.look = c.id % kColonistLooks;
        c.x    = isletCenterX(islet) + (frand(rng) - 0.5f) * 20.0f;
        c.y    = isletCenterY(islet) + (frand(rng) - 0.5f) * 10.0f;
        colonists.push_back(c);
    }
    // Stun recovery: a dropped soul dusts itself off and waits where it landed.
    for (Colonist& c : colonists)
        if (c.state == ColonistState::Stunned && --c.stunnedLeft <= 0)
            c.state = ColonistState::Waiting;
}

void FerrymanGame::tryDrop() {
    if (deck.empty() || ferryY < kSanctuaryBottom) return;  // at the sanctuary you BANK, not drop
    const int id = deck.back();
    deck.pop_back();
    for (Colonist& c : colonists) {
        if (c.id != id) continue;
        c.state = ColonistState::Waiting;  // stashed where you sail — recoverable, and stealable
        c.x     = ferryX;
        c.y     = ferryY + 14.0f;
        emit(GameEventKind::Drop, c.x, c.y);
        return;
    }
}

void FerrymanGame::placeGrounded(Colonist c, float px, float py, int stunTicks) {
    c.state       = ColonistState::Stunned;
    c.stunnedLeft = stunTicks;
    c.x = std::clamp(px, 10.0f, kViewW - 10.0f);
    c.y = std::clamp(py, kFieldTopF + 10.0f, kViewH - 10.0f);
    colonists.push_back(c);
}

void FerrymanGame::abductorPhase() {
    for (int i = 0; i < abductorCount(); ++i) {
        auto&      abd = abductors[static_cast<std::size_t>(i)];
        const Vec2 ap  = abd.pos();

        // The sim picks WHAT it wants (its knowledge); the abductor flies. Prey = the nearest
        // grounded colonist — never one aboard the ferry.
        std::optional<Vec2> target;
        float best = 1.0e9f;
        for (const Colonist& c : colonists) {
            if (c.state == ColonistState::Aboard) continue;
            const float d = distTo(ap.x, ap.y, c.x, c.y);
            if (d < best) {
                best   = d;
                target = Vec2{c.x, c.y};
            }
        }
        abd.tick(target, rng);

        if (abd.beamJustLit()) emit(GameEventKind::BeamLock, ap.x, ap.y);

        // The grab: a descending saucer within reach of a grounded colonist swallows it whole —
        // the colonist LEAVES the field vector and rides in the saucer's stomach (`carried`),
        // identity intact, until a foil returns it or the top of the field loses it.
        if (abd.state() == AbductorState::Descending) {
            for (std::size_t k = 0; k < colonists.size(); ++k) {
                const Colonist& c = colonists[k];
                if (c.state == ColonistState::Aboard) continue;
                if (distTo(ap.x, ap.y + kAbductorH / 2.0f, c.x, c.y) <= kGrabDist) {
                    carried[static_cast<std::size_t>(i)] = c;
                    colonists.erase(colonists.begin() + static_cast<std::ptrdiff_t>(k));
                    abd.grab();
                    break;
                }
            }
        }

        // The body-block foil: the ferry rams a lit beam — the colonist (if already grabbed)
        // lands stunned below, SAME identity, and the thief flees. Pays the bounty. (A cargo
        // bolt strike foils it too — boltPhase handles that path.)
        if (abd.beamLit() &&
            distTo(ferryX, ferryY, ap.x, ap.y) <= kFoilDist + kFerryBoxW / 2.0f) {
            if (auto& held = carried[static_cast<std::size_t>(i)]) {
                placeGrounded(*held, ap.x, ap.y + 20.0f, static_cast<int>(kStunTicks));
                held.reset();
            }
            abd.foil(static_cast<int>(kAbductorReentry));
            score += kFoilScore;
            emit(GameEventKind::Foil, ap.x, ap.y, kFoilScore);
        }

        // Carried off the top: the colonist is LOST — and something is coming back.
        if (abd.state() == AbductorState::Carrying && ap.y < kFieldTopF - 30.0f) {
            carried[static_cast<std::size_t>(i)].reset();
            abd.leftWithColonist(static_cast<int>(kAbductorReentry));
            emit(GameEventKind::ColonistLost, ap.x, kFieldTopF);
            mutantQueue.push_back(static_cast<int>(kMutantDelay));
        }
    }

    // The mutant queue: each loss matures into a hunter (capped; the rest wait their turn).
    int mutantsAlive = 0;
    for (const Enemy& e : enemies)
        if (e.kind == EK_MUTANT) ++mutantsAlive;
    for (std::size_t q = 0; q < mutantQueue.size();) {
        if (mutantQueue[q] > 0) --mutantQueue[q];
        if (mutantQueue[q] == 0 && mutantsAlive < kMaxMutants) {
            spawnEnemy(EK_MUTANT);
            ++mutantsAlive;
            emit(GameEventKind::MutantSpawn, enemies.back().x, enemies.back().y);
            mutantQueue.erase(mutantQueue.begin() + static_cast<std::ptrdiff_t>(q));
        } else {
            ++q;
        }
    }
}

void FerrymanGame::enemyPhase() {
    // The respawn queue: a downed craft re-enters from an edge after its delay (a fresh id —
    // the population holds, and the interpolator mount-snaps the arrival).
    for (std::size_t q = 0; q < respawnQueue.size();) {
        if (--respawnQueue[q].second <= 0) {
            spawnEnemy(respawnQueue[q].first);
            respawnQueue.erase(respawnQueue.begin() + static_cast<std::ptrdiff_t>(q));
        } else {
            ++q;
        }
    }

    for (Enemy& e : enemies) {
        if (e.hitFlash > 0) --e.hitFlash;
        switch (e.kind) {
            case EK_CORSAIR: {
                e.sway += 0.05f;
                e.x += e.vx;
                e.y += e.vy + std::sin(e.sway) * 0.5f;
                // Off either side: turn the run around (a new pass, same craft — continuous
                // motion, no teleport, so the key needs no epoch).
                if ((e.vx > 0 && e.x > kViewW + kEnemyW[EK_CORSAIR]) ||
                    (e.vx < 0 && e.x < -kEnemyW[EK_CORSAIR])) {
                    e.vx = -e.vx;
                    e.y  = kFieldTopF + 60.0f + frand(rng) * (kViewH - kFieldTopF - 160.0f);
                }
                e.y = std::clamp(e.y, kFieldTopF + 20.0f, kViewH - 20.0f);
                // The aimed bolt: one straight, readable line at where you ARE — dodge by moving.
                if (--e.fireTimer <= 0) {
                    e.fireTimer = static_cast<int>(kCorsairFireEvery * fireRateMul);
                    const float d = distTo(e.x, e.y, ferryX, ferryY);
                    if (d > 1.0f && e.x > 0 && e.x < kViewW) {
                        bolts.push_back(Bolt{.id = nextId_++, .x = e.x, .y = e.y,
                                             .vx = (ferryX - e.x) / d * kEnemyBoltSpeed,
                                             .vy = (ferryY - e.y) / d * kEnemyBoltSpeed});
                    }
                }
                break;
            }
            case EK_WARDEN: {
                e.orbit += kWardenOrbit;
                const float tx = e.anchorX + std::cos(e.orbit) * kWardenRadius;
                const float ty = e.anchorY + std::sin(e.orbit) * kWardenRadius;
                e.x += std::clamp(tx - e.x, -1.2f, 1.2f);
                e.y += std::clamp(ty - e.y, -1.2f, 1.2f);
                // The 6-bolt ring: pure geometry, phase-locked to its orbit — read it and slip
                // between the spokes.
                if (--e.fireTimer <= 0) {
                    e.fireTimer = static_cast<int>(kWardenFireEvery * fireRateMul);
                    for (int k = 0; k < 6; ++k) {
                        const float a = e.orbit + static_cast<float>(k) * 60.0f * kDegToRad;
                        bolts.push_back(Bolt{.id = nextId_++, .x = e.x, .y = e.y,
                                             .vx = std::cos(a) * kEnemyBoltSpeed * 0.9f,
                                             .vy = std::sin(a) * kEnemyBoltSpeed * 0.9f});
                    }
                }
                break;
            }
            case EK_DREAD: {
                e.x += e.vx;
                if ((e.vx > 0 && e.x > kViewW + kEnemyW[EK_DREAD]) ||
                    (e.vx < 0 && e.x < -kEnemyW[EK_DREAD])) {
                    e.vx = -e.vx;
                    e.y  = kFieldTopF + 90.0f + frand(rng) * 120.0f;
                }
                // The 3-bolt fan at the ferry: a centre aim flanked by fixed spreads.
                if (--e.fireTimer <= 0) {
                    e.fireTimer = static_cast<int>(kDreadFireEvery * fireRateMul);
                    const float d = distTo(e.x, e.y, ferryX, ferryY);
                    if (d > 1.0f && e.x > 0 && e.x < kViewW) {
                        const float base = std::atan2(ferryY - e.y, ferryX - e.x);
                        for (int k = -1; k <= 1; ++k) {
                            const float a = base + static_cast<float>(k) * kFanSpreadDeg * kDegToRad;
                            bolts.push_back(Bolt{.id = nextId_++, .x = e.x, .y = e.y,
                                                 .vx = std::cos(a) * kEnemyBoltSpeed,
                                                 .vy = std::sin(a) * kEnemyBoltSpeed});
                        }
                    }
                }
                break;
            }
            default: {  // EK_MUTANT: the contact hunter — no bullets, no mercy, no lanes
                const float dx = ferryX - e.x, dy = ferryY - e.y;
                const float d  = std::hypot(dx, dy);
                if (d > 0.5f) {
                    e.x += dx / d * kMutantSpeed;
                    e.y += dy / d * kMutantSpeed;
                }
                break;
            }
        }
    }
}

void FerrymanGame::cargoFire() {
    // THE SEED OF THE ARSENAL: one volley clock whose period divides by the souls aboard — each
    // passenger is another gunner on the rail. The bolt aims at the nearest enemy; the player
    // has no fire button. (The full game grows this into the whole build system — PLAN §4.)
    if (deck.empty()) {
        cargoFireTimer_ = kCargoFirePeriod;
        return;
    }
    if (--cargoFireTimer_ > 0) return;
    cargoFireTimer_ = kCargoFirePeriod / static_cast<int>(deck.size());

    float best = 1.0e9f;
    const Enemy* target = nullptr;
    for (const Enemy& e : enemies) {
        const float d = distTo(ferryX, ferryY, e.x, e.y);
        if (d < best) {
            best   = d;
            target = &e;
        }
    }
    if (target == nullptr) return;
    bolts.push_back(Bolt{.id = nextId_++, .x = ferryX, .y = ferryY - 8.0f,
                         .vx = (target->x - ferryX) / best * kCargoBoltSpeed,
                         .vy = (target->y - ferryY + 8.0f) / best * kCargoBoltSpeed,
                         .friendly = true});
    emit(GameEventKind::CargoFire, ferryX, ferryY);
}

void FerrymanGame::enemyDown(std::size_t idx) {
    const Enemy e = enemies[idx];
    enemies.erase(enemies.begin() + static_cast<std::ptrdiff_t>(idx));
    const int pts = kEnemyScore[static_cast<std::size_t>(e.kind)];
    score += pts;
    emit(GameEventKind::EnemyDown, e.x, e.y, pts);
    if (e.kind != EK_MUTANT)  // craft re-enter; a downed mutant is simply gone
        respawnQueue.emplace_back(e.kind, kEnemyRespawnTicks);
}

void FerrymanGame::boltPhase() {
    for (std::size_t b = 0; b < bolts.size();) {
        Bolt& bolt = bolts[b];
        bolt.x += bolt.vx;
        bolt.y += bolt.vy;
        bool spent = bolt.x < -20.0f || bolt.x > kViewW + 20.0f || bolt.y < kFieldTopF - 20.0f ||
                     bolt.y > kViewH + 20.0f;

        if (!spent && bolt.friendly) {
            // The crew's gold bolts: down a craft or mutant…
            for (std::size_t k = 0; k < enemies.size(); ++k) {
                Enemy& e = enemies[k];
                if (boxesOverlap(bolt.x, bolt.y, kBoltBox, kBoltBox, e.x, e.y,
                                 kEnemyW[static_cast<std::size_t>(e.kind)],
                                 kEnemyH[static_cast<std::size_t>(e.kind)])) {
                    e.hitFlash = 8;
                    if (--e.hp <= 0) enemyDown(k);
                    spent = true;
                    break;
                }
            }
            // …and a strike on a beam-lit abductor is a foil too (the crew defends its own).
            if (!spent) {
                for (int i = 0; i < abductorCount(); ++i) {
                    auto&      abd = abductors[static_cast<std::size_t>(i)];
                    const Vec2 ap  = abd.pos();
                    if (abd.state() == AbductorState::Away ||
                        abd.state() == AbductorState::Fleeing) {
                        continue;
                    }
                    if (boxesOverlap(bolt.x, bolt.y, kBoltBox, kBoltBox, ap.x, ap.y, kAbductorW,
                                     kAbductorH)) {
                        if (auto& held = carried[static_cast<std::size_t>(i)]) {
                            placeGrounded(*held, ap.x, ap.y + 20.0f, static_cast<int>(kStunTicks));
                            held.reset();
                        }
                        abd.foil(static_cast<int>(kAbductorReentry));
                        score += kFoilScore;
                        emit(GameEventKind::Foil, ap.x, ap.y, kFoilScore);
                        spent = true;
                        break;
                    }
                }
            }
        } else if (!spent && invulnLeft == 0 &&
                   boxesOverlap(bolt.x, bolt.y, kBoltBox, kBoltBox, ferryX, ferryY, kFerryBoxW,
                                kFerryBoxH)) {
            ferryDeath();
            return;  // the tick's remaining contacts are moot (respawn or title)
        }

        if (spent) {
            bolts.erase(bolts.begin() + static_cast<std::ptrdiff_t>(b));
        } else {
            ++b;
        }
    }
}

void FerrymanGame::ferryDeath() {
    emit(GameEventKind::FerryDeath, ferryX, ferryY);
    // The passengers spill where the ferry went down — stunned, treading water, recoverable,
    // and exposed. Identity (and so each sprite key) survives the swim.
    int spread = 0;
    for (int id : deck) {
        for (std::size_t k = 0; k < colonists.size(); ++k) {
            if (colonists[k].id != id) continue;
            Colonist c = colonists[k];
            colonists.erase(colonists.begin() + static_cast<std::ptrdiff_t>(k));
            const int sx = spread % 3 - 1, sy = spread / 3;
            placeGrounded(c, ferryX + static_cast<float>(sx) * 22.0f,
                          ferryY + static_cast<float>(sy) * 18.0f, static_cast<int>(kStunTicks));
            ++spread;
            break;
        }
    }
    deck.clear();
    bolts.clear();  // the moment of the sinking clears the air — a fair restart, not a trap
    if (--lives <= 0) {
        std::printf("game over — score %d — back to title\n", score);
        state = GameState::Title;
        return;
    }
    respawnFerry();
}

void FerrymanGame::resolveContacts() {
    // Enemy contact — craft and mutants alike are lethal to touch.
    if (invulnLeft == 0) {
        for (const Enemy& e : enemies) {
            if (boxesOverlap(ferryX, ferryY, kFerryBoxW, kFerryBoxH, e.x, e.y,
                             kEnemyW[static_cast<std::size_t>(e.kind)] * 0.85f,
                             kEnemyH[static_cast<std::size_t>(e.kind)] * 0.85f)) {
                ferryDeath();
                return;
            }
        }
    }

    // Pickup: sail over a grounded colonist with deck space and it climbs aboard — and takes
    // the rail immediately: the first soul fires within half a second, and every boarding pulls
    // the next volley forward to the heavier deck's cadence (never waits out the old timer).
    for (Colonist& c : colonists) {
        if (c.state != ColonistState::Waiting) continue;
        if (deck.size() >= static_cast<std::size_t>(kDeckCap)) break;
        if (distTo(ferryX, ferryY, c.x, c.y) <= kPickupDist) {
            c.state = ColonistState::Aboard;
            deck.push_back(c.id);
            const int cadence = kCargoFirePeriod / static_cast<int>(deck.size());
            cargoFireTimer_ =
                std::min(cargoFireTimer_, deck.size() == 1 ? kFirstVolleyTicks : cadence);
            emit(GameEventKind::Pickup, c.x, c.y);
        }
    }

    // Banking: sail into the sanctuary band with souls aboard and the whole deck delivers —
    // slot i pays i × 50, so the heavy crossing is the rich one.
    if (ferryY < kSanctuaryBottom + kFerryH / 2.0f && !deck.empty()) {
        const int pts = haulPays();
        const int n   = static_cast<int>(deck.size());
        score += pts;
        rescued += n;
        for (int id : deck)
            std::erase_if(colonists, [id](const Colonist& c) { return c.id == id; });
        deck.clear();
        emit(GameEventKind::Bank, ferryX, kSanctuaryBottom - 20.0f, pts);
    }
}

void FerrymanGame::tick(const InputState& in) {
    events_.clear();

    // Title: ENTER starts a fresh game — the main Return (gamepad Start) or the numpad Enter
    // (which rides the X slot; in play the same alias drops cargo).
    if (state == GameState::Title) {
        if (in.justPressed(Button::Start) || in.justPressed(Button::X)) {
            state = GameState::Playing;
            newGame();
        }
        return;
    }

    moveInput(in);
    if (in.justPressed(Button::B) || in.justPressed(Button::X)) tryDrop();

    colonistFlow();
    abductorPhase();
    enemyPhase();
    cargoFire();
    boltPhase();
    if (state == GameState::Title) return;  // a bolt on the last life ended the game
    resolveContacts();
    if (state == GameState::Title) return;

    // Quota met → a short lull (the round card holds the stage) → the next, denser crossing.
    // Waiting colonists persist — the sea's souls carry over.
    if (rescued >= quota() && waveLull == 0) {
        waveLull = static_cast<int>(kWaveLullTicks);
        emit(GameEventKind::WaveClear, kViewW / 2.0f, kSanctuaryBottom);
        std::printf("crossing %d cleared — score %d\n", wave, score);
    }
    if (waveLull > 0 && --waveLull == 0) spawnWave();

    if (invulnLeft > 0) --invulnLeft;
}

}  // namespace ferryman
