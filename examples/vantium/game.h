#pragma once

// Vantium — the SIMULATION, its own translation unit. Owns the Manta, the camera, the player
// shots, the deck, the wave system, and the score/lives/landing state, and advances them one
// 60 Hz tick from the input. It draws nothing and makes no sound: it NAMES what happened each
// tick as a GameEvent stream, which the audio + feel layers consume.
//
// THE LOOP: strafe the dreadnought in either direction (the camera leads your facing), gun down
// squadron after squadron while threading the superstructure — level over the open deck, ROLLED
// (hold B, guns cold) through the one-cell canyons — until the wave quota falls; then bleed off
// speed, line up the strip, and set her down (Down, inside the speed envelope) to scuttle the
// ship and fly on to the next, faster one.

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "retropp/input.h"  // InputState

#include "deck.h"
#include "layout.h"
#include "waves.h"

namespace vant {

// The game's input vocabulary — the semantic actions the sim reads. main.cpp binds each action to
// its physical sources in one ActionMap.
enum class Action : std::uint8_t {
    Up,          // thrust directions (Down also lands, over the strip with the quota met)
    Down,
    Left,
    Right,
    Fire,
    Roll,        // held: thin hitbox, guns cold
    Start,       // starts a game at the title; flips the evaluation grid in play (main.cpp)
    Fullscreen,  // host-level toggle, read in main.cpp
};

struct PlayerShot {
    int   id = 0;
    bool  alive = false;
    float x = 0, y = 0, vx = 0;
};

struct VantGame {
    VantGame();

    GameState   state = GameState::Title;
    FlightPhase phase = FlightPhase::Flying;

    int shipNum = 1;

    // The Manta.
    float shipX = 64, shipY = 48, shipVx = 0, shipVy = 0;
    int   facing = 1;            // +1 right / −1 left
    bool  rolled = false;        // held B: thin hitbox, guns cold
    int   turnTicks = 0;         // side-on frame while > 0
    int   bank = 0;              // −2..2, from vertical input (render picks the frame)
    int   invulnTicks = 0;
    int   thrustDir = 0;         // −1/0/+1: horizontal input this tick (drives the exhaust glow)
    int   lives = kLives;
    int   score = 0;

    float camX = 0;

    std::array<PlayerShot, kMaxShots> shots{};

    Deck       deck;
    WaveSystem waves;

    int landingTicks  = 0;   // Landing phase countdown
    int destructTicks = 0;   // Destruct phase countdown

    // Sprite-identity epochs: the Manta re-keys per life (a respawn is a teleport), shots and
    // enemies key by their monotonic ids.
    int lifeEpoch = 0;

    void tick(const retropp::InputState& in, const VantAssets& assets);

    [[nodiscard]] std::span<const GameEvent> events() const { return events_; }
    [[nodiscard]] bool quotaMet() const { return waves.quotaMet(); }

private:
    std::uint32_t rng;
    std::vector<GameEvent> events_;
    int nextShotId_ = 1;

    void emit(GameEventKind kind, float x = 0.0f, float y = 0.0f, int points = 0) {
        events_.push_back(GameEvent{kind, x, y, points});
    }

    void newGame(const VantAssets& assets);
    void startShip(const VantAssets& assets);
    void respawn();
    void die();
    void fire();
    void shipHitbox(float& x, float& y, float& w, float& h) const;
};

}  // namespace vant
