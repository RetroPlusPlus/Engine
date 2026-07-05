#pragma once

// Ferryman — the ABDUCTOR, its own translation unit: the colonist-snatcher that turns waiting
// souls into a race, and — when it wins — into MUTANTS that hunt you. It never harms the ferry;
// the traffic and the mutants do that. Its cycle:
//
//   • Away        — off the field on a timer (first entry lands mid-wave; re-books after a
//                   departure or a foil).
//   • Entering    — a smooth swoop along a baked Catmull-Rom curve from off-screen through two
//                   interior waypoints (Curve::throughPoints → ArcLengthTable, walked at constant
//                   speed — sampled once at entry, not per frame).
//   • Positioning — cruises toward the hover point above whatever colonist the GAME says it wants
//                   (target selection is the sim's knowledge; the abductor only flies), with a
//                   lateral sway so it reads as flying, not sliding.
//   • Descending  — drops straight down the beam line toward the target. THE BEAM IS LIT from
//                   here until it leaves (the renderer draws it; the alarm voices it).
//   • Carrying    — grabbed (the sim decides the contact): rises straight off the top of the
//                   field with the colonist. Off-screen = the colonist is LOST (a mutant later).
//   • Fleeing     — body-blocked by the ferry: drops the colonist (the sim re-homes it) and runs
//                   for the nearest edge at speed, then re-books.
//
// The abductor only MOVES itself; every contact rule (grab, foil, lost) lives in the sim, which
// reads its position from here. Each field visit gets a fresh spawn number — its sprite key
// embeds it, so the interpolator mount-snaps every arrival instead of streaking it in from the
// last exit.

#include <cstdint>
#include <optional>

#include "retropp/curve.h"     // Curve / ArcLengthTable — the entry swoop
#include "retropp/geometry.h"  // Vec2

namespace ferryman {

enum class AbductorState : std::uint8_t {
    Away, Entering, Positioning, Descending, Carrying, Fleeing
};

class Abductor {
public:
    // Schedule the next field visit `entryTicks` from now (wave start / after a visit ends).
    void reset(int entryTicks);

    // Advance one tick. `target` is the colonist position the sim wants it over (nullopt = no
    // colonist to steal — it loiters along a slow patrol line instead). `rng` shapes the entry.
    void tick(std::optional<retropp::Vec2> target, std::uint32_t& rng);

    // The sim's contact verdicts:
    void grab()  { state_ = AbductorState::Carrying; }   // it reached the colonist — lift away
    void foil(int reentryTicks);                          // body-blocked — flee, then re-book
    void leftWithColonist(int reentryTicks) { reset(reentryTicks); }  // rose off the top

    [[nodiscard]] AbductorState state() const { return state_; }
    [[nodiscard]] retropp::Vec2 pos() const { return retropp::Vec2{x_, y_}; }
    [[nodiscard]] int           spawn() const { return spawn_; }  // key epoch, fresh per visit
    [[nodiscard]] bool beamLit() const {
        return state_ == AbductorState::Descending || state_ == AbductorState::Carrying;
    }
    // True on the one tick Descending began — the sim turns it into the BeamLock alarm event.
    [[nodiscard]] bool beamJustLit() const { return beamJustLit_; }

private:
    AbductorState state_ = AbductorState::Away;
    int           timer_ = 0;   // Away: ticks until entry
    int           spawn_ = 0;
    float         x_ = 0.0f, y_ = 0.0f;
    float         sway_ = 0.0f;        // the positioning flight's lateral bob phase
    float         patrol_ = 0.0f;      // the no-target loiter phase
    bool          beamJustLit_ = false;

    std::optional<retropp::ArcLengthTable> entry_;  // present only while Entering
    float pathS_ = 0.0f, pathLen_ = 0.0f;
};

}  // namespace ferryman
