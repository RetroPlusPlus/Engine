#pragma once

// Bongusoid — the SIMULATION, its own translation unit. Owns the board + ball + paddle + score/lives and
// advances them one 60 Hz tick from the input. It draws nothing and makes no sound: it NAMES what happened
// each tick as a GameEvent stream (S2), which the audio + feel layers consume. Pure game model.

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "retropp/input.h"  // InputState (digital + analog) + Stick

#include "layout.h"

namespace bong {

// The game's input vocabulary — the semantic actions the sim reads. main.cpp binds each action to
// its physical sources in one ActionMap; the stick and the mouse are read raw beside these.
enum class Action : std::uint8_t {
    MoveLeft,
    MoveRight,
    Serve,       // launches the parked ball
    Start,       // starts a game at the title
    Fullscreen,  // host-level toggle, read in main.cpp
};

// What happened this tick — the sim's output channel beyond the board state. Audio cues off `kind`; the
// feel layer reads `x`/`y` (a brick's top-left, for a score popup) and `points` (the popup's number).
enum class GameEventKind : std::uint8_t {
    Serve, PaddleBounce, WallBounce, BrickHit, BrickBreak, BallLost, LevelClear,
};
struct GameEvent {
    GameEventKind kind;
    float         x      = 0.0f;
    float         y      = 0.0f;
    int           points = 0;
};

// The game model. State is public so the sim-free renderer reads it directly (a demo, not a library
// surface — clarity over encapsulation); the sim mutates it through tick() and the private helpers.
struct BongGame {
    BongGame();

    GameState state = GameState::Title;

    std::array<std::array<Cell, kBrickCols>, kBrickRows> grid{};
    int   bricksLeft = 0;

    float paddleX = static_cast<float>(kViewW) / 2.0f - kPaddleW / 2.0f;
    float ballX = 0, ballY = 0, ballVx = 0, ballVy = 0;
    int   score = 0, lives = kLives;
    bool  serving = true;

    // Advance one sim tick; refills the per-tick event list (cleared at entry).
    void tick(const retropp::InputState& in);

    // The events emitted THIS tick (valid until the next tick()).
    [[nodiscard]] std::span<const GameEvent> events() const { return events_; }

private:
    std::uint32_t rng;
    std::vector<GameEvent> events_;

    void emit(GameEventKind kind, float x = 0.0f, float y = 0.0f, int points = 0) {
        events_.push_back(GameEvent{kind, x, y, points});
    }

    void buildBoard();
    void parkBall();
    void aimBallUp(float desiredVx);
    void serve();
    void newGame();
    void nextBoard();
    void loseLife();
};

}  // namespace bong
