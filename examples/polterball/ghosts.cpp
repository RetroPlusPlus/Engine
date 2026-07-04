#include "ghosts.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <span>

#include "retropp/timing.h"  // TimingProfile / TickPeriodNs — durations → ticks

namespace polter {

using namespace retropp;

namespace {
using namespace std::chrono_literals;

const TimingProfile kProfile{TickPeriodNs::Hz60};

// The scatter/chase breathing: hunt for a while, retreat to the corners for a beat, repeat.
const std::uint64_t kScatterTicks = kProfile.ticksForDuration(7s);
const std::uint64_t kChaseTicks   = kProfile.ticksForDuration(20s);

// Staggered pen releases at reset, and the respawn hold after the eyes arrive home.
const std::uint64_t kRelease1  = kProfile.ticksForDuration(2s);
const std::uint64_t kRelease2  = kProfile.ticksForDuration(4s);
const std::uint64_t kRespawnIn = kProfile.ticksForDuration(2s);

// Junction probe order — also the classic tie-break preference: Up, Left, Down, Right.
constexpr std::array<CellRC, 4> kDirs{{{0, -1}, {-1, 0}, {0, 1}, {1, 0}}};

// Scatter home corners, one per role (the Wanderer's is unused — it is random in every mode).
constexpr std::array<CellRC, kGhostCount> kScatterHome{{{18, 1}, {1, 1}, {1, 9}}};

std::uint32_t nextRand(std::uint32_t& s) { s = s * 1664525u + 1013904223u; return s; }

// Reverse a mid-edge walker in place: it keeps its exact position but heads back the way it came.
// (from, dir, t) → (the cell it was heading to, -dir, kCell - t): both describe the same point.
void reverse(Ghost& g) {
    g.from = CellRC{g.from.c + g.dirC, g.from.r + g.dirR};
    g.dirC = -g.dirC;
    g.dirR = -g.dirR;
    g.t    = static_cast<float>(kCell) - g.t;
}

}  // namespace

void GhostSquad::reset() {
    ghosts_ = {};  // wipe every walk/eyes/frightened field
    // Ghost 0 — the Chaser — starts hunting immediately, at the pen's exit row, heading left.
    ghosts_[0].role  = GhostRole::Chaser;
    ghosts_[0].state = GhostState::Active;
    ghosts_[0].from  = CellRC{kPenLeftCol, kPenExitRow};
    ghosts_[0].dirC  = -1;
    ghosts_[0].dirR  = 0;
    // Ghosts 1 + 2 — the Ambusher and the Wanderer — wait in the pen on staggered timers.
    ghosts_[1].role     = GhostRole::Ambusher;
    ghosts_[1].from     = CellRC{kPenLeftCol, kPenFloorRow};
    ghosts_[1].penTimer = static_cast<int>(kRelease1);
    ghosts_[2].role     = GhostRole::Wanderer;
    ghosts_[2].from     = CellRC{kPenRightCol, kPenFloorRow};
    ghosts_[2].penTimer = static_cast<int>(kRelease2);
    // A fresh wave opens in scatter, like the original cadence.
    chaseMode_ = false;
    modeTicks_ = static_cast<int>(kScatterTicks);
}

Vec2 GhostSquad::center(int i) const {
    const Ghost& g = ghosts_[static_cast<std::size_t>(i)];
    if (g.state == GhostState::Eyes && g.eyesPath) {
        return g.eyesPath->atDistance(g.eyesS);
    }
    return Vec2{cellPxX(g.from.c) + kCell / 2.0f + static_cast<float>(g.dirC) * g.t,
                cellPxY(g.from.r) + kCell / 2.0f + static_cast<float>(g.dirR) * g.t};
}

CellRC GhostSquad::targetFor(const Ghost& g, Vec2 ballCenter, Vec2 ballVel) const {
    // The ball's cell, clamped into the grid (a ball down in the court clamps to the bottom row, so
    // hunters gather at the gates — exactly the pressure you feel while lining up a serve).
    const int bc = std::clamp(static_cast<int>(ballCenter.x) / kCell, 0, kMazeCols - 1);
    const int br = std::clamp((static_cast<int>(ballCenter.y) - kMazeTop) / kCell, 0, kMazeRows - 1);

    if (!chaseMode_) return kScatterHome[static_cast<std::size_t>(g.role)];

    if (g.role == GhostRole::Ambusher) {
        // Aim a few cells AHEAD of the ball's travel — the cut-off artist. Dominant-axis steps so a
        // diagonal flight still yields a sensible corridor-space lead.
        const float ax = std::abs(ballVel.x), ay = std::abs(ballVel.y);
        int lc = bc, lr = br;
        if (ax > 0.01f || ay > 0.01f) {
            if (ax >= ay) lc += (ballVel.x > 0 ? kAmbushCells : -kAmbushCells);
            else          lr += (ballVel.y > 0 ? kAmbushCells : -kAmbushCells);
        }
        return CellRC{std::clamp(lc, 0, kMazeCols - 1), std::clamp(lr, 0, kMazeRows - 1)};
    }
    return CellRC{bc, br};  // the Chaser (the Wanderer never consults its target)
}

void GhostSquad::chooseDir(Ghost& g, const Board& board, CellRC target, bool randomPick,
                           std::uint32_t& rng) const {
    // Candidates: open, non-reverse directions out of the CURRENT cell, probed in the classic
    // tie-break order. The boards are authored dead-end-free, so at least one always exists; the
    // reverse fallback below is belt-and-braces, never the design.
    std::array<CellRC, 4> cand{};
    int n = 0;
    for (const CellRC d : kDirs) {
        if (d.c == -g.dirC && d.r == -g.dirR) continue;  // no reversing at a junction
        if (board.ghostOpen(g.from.c + d.c, g.from.r + d.r)) cand[static_cast<std::size_t>(n++)] = d;
    }
    if (n == 0) {  // trapped (a layout bug, not a game state) — reverse rather than freeze
        g.dirC = -g.dirC;
        g.dirR = -g.dirR;
        return;
    }
    if (randomPick) {
        const CellRC d = cand[nextRand(rng) % static_cast<std::uint32_t>(n)];
        g.dirC = d.c;
        g.dirR = d.r;
        return;
    }
    int best = 0, bestDist = INT32_MAX;
    for (int i = 0; i < n; ++i) {
        const CellRC d  = cand[static_cast<std::size_t>(i)];
        const int    dc = (g.from.c + d.c) - target.c;
        const int    dr = (g.from.r + d.r) - target.r;
        const int    dist = dc * dc + dr * dr;
        if (dist < bestDist) { bestDist = dist; best = i; }  // ties keep the earlier (U,L,D,R) probe
    }
    g.dirC = cand[static_cast<std::size_t>(best)].c;
    g.dirR = cand[static_cast<std::size_t>(best)].r;
}

void GhostSquad::tick(const Board& board, Vec2 ballCenter, Vec2 ballVel, float ghostSpeed,
                      bool ignited, std::uint32_t& rng) {
    // The scatter/chase clock breathes only in normal play — frightened time doesn't burn it.
    if (!ignited && --modeTicks_ <= 0) {
        chaseMode_ = !chaseMode_;
        modeTicks_ = static_cast<int>(chaseMode_ ? kChaseTicks : kScatterTicks);
        for (Ghost& g : ghosts_) {
            if (g.state == GhostState::Active) reverse(g);  // the classic mode-change tell
        }
    }

    for (Ghost& g : ghosts_) {
        switch (g.state) {
            case GhostState::InPen:
                if (--g.penTimer <= 0) {
                    g.state = GhostState::Exiting;
                    g.dirC  = 0;
                    g.dirR  = -1;
                    g.t     = 0.0f;
                }
                break;

            case GhostState::Exiting:
                // A steady crawl straight up through the gate; on surfacing into corridor, hunt.
                g.t += ghostSpeed * kExitSpeedMul;
                while (g.t >= static_cast<float>(kCell)) {
                    g.t -= static_cast<float>(kCell);
                    g.from.r -= 1;
                    if (board.ghostOpen(g.from.c, g.from.r)) {
                        g.state = GhostState::Active;
                        chooseDir(g, board, targetFor(g, ballCenter, ballVel),
                                  g.frightened || g.role == GhostRole::Wanderer, rng);
                        break;
                    }
                }
                break;

            case GhostState::Active: {
                const float speed = ghostSpeed * (g.frightened ? kFrightSpeedMul : 1.0f);
                g.t += speed;
                while (g.t >= static_cast<float>(kCell)) {
                    g.t -= static_cast<float>(kCell);
                    g.from = CellRC{g.from.c + g.dirC, g.from.r + g.dirR};
                    chooseDir(g, board, targetFor(g, ballCenter, ballVel),
                              g.frightened || g.role == GhostRole::Wanderer, rng);
                }
                break;
            }

            case GhostState::Eyes:
                g.eyesS += kEyesSpeed;
                if (g.eyesS >= g.eyesLen) {  // home — rest in the pen, then queue the exit
                    g.eyesPath.reset();
                    g.state    = GhostState::InPen;
                    g.from     = CellRC{kPenLeftCol, kPenFloorRow};
                    g.dirC     = 0;
                    g.dirR     = -1;
                    g.t        = 0.0f;
                    g.penTimer = static_cast<int>(kRespawnIn);
                }
                break;
        }
    }
}

void GhostSquad::frightenAll() {
    // Only ghosts already out (or on their way out) turn: a ghost released LATER during the same
    // ignite emerges normal, so a respawn can never be farmed into an endless chain.
    for (Ghost& g : ghosts_) {
        if (g.state == GhostState::Active) {
            g.frightened = true;
            reverse(g);  // the classic about-face the instant the tables turn
        } else if (g.state == GhostState::Exiting) {
            g.frightened = true;  // it will surface scared (no reverse mid-gate)
        }
    }
}

void GhostSquad::calmAll() {
    for (Ghost& g : ghosts_) g.frightened = false;
}

void GhostSquad::down(int i) {
    Ghost& g = ghosts_[static_cast<std::size_t>(i)];
    const Vec2 start = center(i);
    // The eyes fly home along a smooth curve: rise to the upper corridor, run across to the pen
    // column, drop into the house. throughPoints smooths the waypoints; the baked ArcLengthTable
    // makes the flight constant-speed (atDistance), sampled once here — not per frame.
    const float corridorY = cellPxY(3) + kCell / 2.0f;
    const float penX      = (cellPxX(kPenLeftCol) + cellPxX(kPenRightCol)) / 2.0f + kCell / 2.0f;
    const float penY      = cellPxY(kPenFloorRow) + kCell / 2.0f;
    const std::array<Vec2, 4> waypoints{{start,
                                         Vec2{start.x, corridorY},
                                         Vec2{penX, corridorY},
                                         Vec2{penX, penY}}};
    const Curve path = Curve::throughPoints(std::span<const Vec2>(waypoints));
    g.eyesPath   = path.arcTable();
    g.eyesLen    = g.eyesPath->length();
    g.eyesS      = 0.0f;
    g.frightened = false;
    g.state      = GhostState::Eyes;
}

}  // namespace polter
