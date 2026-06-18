// ============================================================================================
//  Bongusoid — an Arkanoid-style brick-breaker on Retro++, and the engine's broad reference example.
//  This is BONGUSOID-S1, the FOUNDATION: a complete, shippable little game built off the breakout demo
//  but on a 640×480 viewport, drawing from REAL PNG sprite art, in its OWN build output folder, with a
//  minimal Title → Playing state scaffold that the later sessions (audio, powerups, full shell) slot into.
//
//  "Bongusoid" is an ORIGINAL homage — original art, layouts, and (later) SFX; no Taito assets or names.
// ============================================================================================
//
//  WHAT THIS DEMONSTRATES (engine features, roughly in order):
//    • The one-line retropp_add_example(bongusoid) build helper → this demo's OWN output dir
//      (build/bongusoid_demo) with ONLY its LoadFromPath asset riding along (the auto-finalizer places it).
//    • EngineConfig with a RAW viewport + 60 Hz: ViewportResolution{640, 480} (an arbitrary size, not a
//      console preset) + TimingProfile{TickPeriodNs::Hz60}.
//    • Renderer::loadAtlas on a committed INDEXED PNG sheet, sliced into addressable slots (ENG-2.G):
//      bongusoid_sprites.png as an 80×24 SpriteSeries (Vaus / ball / brick / silver / gold), each game
//      sprite drawn at its OWN AssetDimensions from its slot's top-left (the breakout solid-atlas pattern
//      over real art); bongusoid_font.png as an 8×8 Tileset for the HUD + title text.
//    • Per-asset AssetPolicy (ENG-2.M.b): the font is Embed (baked into the binary), the sprite sheet is
//      LoadFromPath (copied beside the binary) — proven live, decided by the loadAtlas call alone.
//    • Indexed-palette COLOUR by SELECTION (ENG-2.K): one brick shape recolours to each row's palette;
//      silver shows a "cracked" palette after one hit; index 0 is transparent (the sprite path discards it).
//    • Digital + ANALOG input (ENG-2.A + follow-on): keyboard/gamepad d-pad + stick AND the mouse — both
//      absolute cursor-to-paddle and relative spinner delta, whichever moved last this tick winning.
//
//  THE GAME:
//    • Title screen: PRESS ENTER (Return / gamepad Start) to play.
//    • Left / Right (or the mouse / a gamepad stick) move the Vaus paddle; A (keyboard X / gamepad South)
//      serves the ball. The ball bounces off walls, the paddle (taking english from the hit offset), and
//      bricks. Colour bricks break in one hit; SILVER bricks take two (cracking after the first); GOLD
//      bricks are indestructible — they bounce the ball forever and never count toward clearing the board.
//    • Miss the ball → lose a life and re-serve. Out of lives → back to the title. Clear every destructible
//      brick → a fresh board (score + lives carry over).
//
//  PHOTOSENSITIVITY: motion is smooth and moderate; nothing flashes or strobes (the silver "crack" is a
//  one-time steady palette change). The demo never auto-launches a window; you run it yourself.
//
//  QUIT: close the window. (The PLAN's "Esc quits the title" is deferred to ENG work — the engine has no
//  game-facing quit API yet; the future home is RunLoop::requestStop(). See the S1 PLAN amendment.)
//
//  CI: like the other example hosts it instantiates SdlPlatform + Renderer for real, so the live GPU +
//  image-load + analog-input path keeps compiling/linking on every CI platform — but CI never opens it.
// ============================================================================================

// Take ownership of main(): SDL's header would otherwise #define main → SDL_main and expect SDL's
// entry shim. We init SDL ourselves (inside SdlPlatform), so we opt out of that redirect.
#define SDL_MAIN_HANDLED
#include <SDL3/SDL_main.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <span>
#include <string_view>
#include <vector>

#include "retropp/asset_policy.h"    // AssetPolicy (Embed / LoadFromPath)
#include "retropp/clock.h"           // SteadyClock
#include "retropp/draw_state.h"      // FrameDrawState / DrawLayer / TileContent / SpriteContent / Sprite
#include "retropp/engine_config.h"   // EngineConfig
#include "retropp/geometry.h"        // AssetDimensions / PixelSize / Vec2i
#include "retropp/image.h"           // ContentKind / ReadOrder / AtlasManifest (the slicer surface)
#include "retropp/input.h"           // InputState (digital + analog: cursor / rawDeltaX / stick) + Button
#include "retropp/palette.h"         // Rgba8 / PaletteId
#include "retropp/renderer.h"        // Renderer — loadAtlas + uploadPalette + renderFrame
#include "retropp/run_loop.h"        // RunLoop — setTick / setRender
#include "retropp/sdl_platform.h"    // SdlPlatform
#include "retropp/timing.h"          // TimingProfile, TickPeriodNs (Hz60)
#include "retropp/viewport.h"        // ViewportResolution
#include "retropp/windowed_host.h"   // WindowedHost — pumps OS events + drives the loop

namespace {

using namespace retropp;

constexpr int kTile = 8;

// ── Layout / tuning for the 640×480 playfield (absolute px / px-per-tick at 60 Hz) ────────────────────
constexpr int   kViewW = 640, kViewH = 480;
constexpr int   kMapW  = kViewW / kTile;   // 80 backdrop/text tiles wide
constexpr int   kMapH  = kViewH / kTile;   // 60 tall

constexpr float kPaddleW = 80.0f, kPaddleH = 16.0f;
constexpr float kPaddleY = 448.0f;          // paddle top-y
constexpr float kBallSz  = 12.0f;
constexpr float kPaddleSpeed = 6.0f;        // keyboard / gamepad px per tick
constexpr float kSpinnerSens = 1.5f;        // raw mouse delta → paddle px (the spinner feel)
constexpr float kBallSpeed   = 4.4f;        // constant ball speed magnitude
constexpr float kMaxEnglish  = 3.6f;        // max horizontal speed imparted by a paddle-edge hit
constexpr float kMinEnglish  = 1.3f;        // the ball can never go perfectly vertical (anti-trap)
constexpr float kPlayTop     = 40.0f;       // ball ceiling (room for the HUD)
constexpr int   kLives       = 3;
constexpr int   kScoreColour = 10;
constexpr int   kScoreSilver = 30;

// Brick grid: 13 cols × 8 rows; 44×24 cell (incl. gap), 40×18 drawn brick, 34px side margin.
// 13·44 + 2·34 = 640; field spans y = 80 … 272.
constexpr int   kBrickCols = 13, kBrickRows = 8;
constexpr float kBrickAreaTop = 80.0f;
constexpr float kBrickCellW = 44.0f, kBrickCellH = 24.0f;
constexpr float kBrickW = 40.0f, kBrickH = 18.0f;
constexpr float kBrickMarginX = 34.0f;

// Sprite-sheet slot order (matches gen_bongusoid_assets.py → the manifest's slot order).
enum Slot { S_VAUS = 0, S_BALL, S_BRICK, S_SILVER, S_GOLD };

// Sprite-layer palette-set indices. Brick rows 0..5 are palettes 0..5; the rest follow.
constexpr int kBrickRowPals = 6;
enum Pal {
    PAL_SILVER       = kBrickRowPals,  // 6  — silver, undamaged
    PAL_SILVER_CRACK,                  // 7  — silver, one hit taken
    PAL_GOLD,                          // 8
    PAL_PADDLE,                        // 9
    PAL_BALL,                          // 10
};

// One brick cell: its kind, its remaining hits, and (for a colour brick) which row palette colours it.
enum class Brick : std::uint8_t { None, Colour, Silver, Gold };
struct Cell {
    Brick type   = Brick::None;
    int   hp     = 0;
    int   colour = 0;  // palette index 0..5 for a Colour brick
};

// A tiny clock-seeded LCG (variety per run; native game, not a GB ROM).
std::uint32_t nextRand(std::uint32_t& s) { s = s * 1664525u + 1013904223u; return s; }

// Build a {transparent, main, light, dark} sprite palette from a base colour.
Rgba8 scale(Rgba8 c, float f) {
    auto ch = [&](std::uint8_t v) {
        return static_cast<std::uint8_t>(std::clamp(static_cast<int>(v * f), 0, 255));
    };
    return Rgba8{ch(c.r), ch(c.g), ch(c.b)};
}
std::array<Rgba8, 4> brickPal(Rgba8 base) {
    return std::array<Rgba8, 4>{{ {0, 0, 0}, base, scale(base, 1.30f), scale(base, 0.62f) }};
}

}  // namespace

int main() {
    SDL_SetMainReady();

    // ── 1. Startup configuration — a raw 640×480 viewport at 60 Hz ──────────────────────────────────
    const EngineConfig config{
        .window   = {.title = "Retro++ — Bongusoid (640×480, 60 Hz)"},
        .viewport = ViewportResolution{640, 480},
        .timing   = TimingProfile{TickPeriodNs::Hz60},
    };
    EngineConfig::setActive(config);

    // ── 2. Core engine objects ──────────────────────────────────────────────────────────────────────
    SteadyClock clock;
    RunLoop     loop{clock};
    SdlPlatform platform;
    Renderer    renderer{platform.device(), platform.window()};
    renderer.setSamplingMode(config.enhancements.sampling);

    // ── 3. Load the committed indexed PNGs, each with its OWN asset policy ────────────────────────────
    //   • FONT  → Embed: baked into this binary by the build scan (the .png never ships).
    //   • SPRITE → LoadFromPath: copied beside the binary into build/bongusoid_demo/ by the auto-finalizer.
    // The policy is decided HERE, by the loadAtlas call; no build rule, no copy rule.
    AtlasManifest font, sheet;
    try {
        font  = renderer.loadAtlas("examples/bongusoid/assets/bongusoid_font.png", AssetDimensions{kTile, kTile},
                                   ContentKind::Tileset, ReadOrder::LeftRightThenDown,
                                   /*count=*/0, /*transparentIndex=*/-1, /*framesPerAnimation=*/0,
                                   AssetPolicy::Embed);
        sheet = renderer.loadAtlas("examples/bongusoid/assets/bongusoid_sprites.png", AssetDimensions{80, 24},
                                   ContentKind::SpriteSeries, ReadOrder::LeftRightThenDown,
                                   /*count=*/0, /*transparentIndex=*/-1, /*framesPerAnimation=*/0,
                                   AssetPolicy::LoadFromPath);
    } catch (const std::exception& e) {
        std::printf("bongusoid: could not load assets: %s\n", e.what());
        return 1;
    }
    const AtlasId fontAtlas   = font.atlas;
    const AtlasId spriteAtlas = sheet.atlas;
    auto slotTile = [&](Slot s) { return sheet[static_cast<std::size_t>(s)].tile; };
    // The font is a single-row Tileset, so glyph k's tile == k; map a character to its glyph slot.
    auto glyphTile = [&](char ch) -> std::uint16_t {
        std::size_t k = 36;  // space (the 37th cell) for anything unmapped
        if (ch >= '0' && ch <= '9')      k = static_cast<std::size_t>(ch - '0');
        else if (ch >= 'A' && ch <= 'Z') k = static_cast<std::size_t>(10 + (ch - 'A'));
        return font[k].tile;
    };

    // ── 4. Palettes ───────────────────────────────────────────────────────────────────────────────────
    // 4a. Sprite-layer set: 6 brick-row colours, then silver(full/cracked), gold, paddle, ball.
    const std::array<Rgba8, kBrickRowPals> rowColours{{
        {220, 70, 70}, {230, 140, 55}, {228, 210, 80}, {95, 200, 100}, {75, 185, 215}, {130, 130, 235},
    }};
    std::vector<PaletteId> spritePals;
    auto upPal = [&](const std::array<Rgba8, 4>& p) {
        spritePals.push_back(renderer.uploadPalette(std::span<const Rgba8>(p)));
    };
    for (Rgba8 c : rowColours) upPal(brickPal(c));     // 0..5
    upPal(brickPal({185, 190, 200}));                  // 6  silver
    upPal(brickPal({130, 134, 146}));                  // 7  silver cracked (dimmer)
    upPal(brickPal({226, 188, 70}));                   // 8  gold
    upPal(brickPal({90, 150, 235}));                   // 9  paddle
    upPal(brickPal({245, 230, 160}));                  // 10 ball

    // 4b. Text-layer set: every font cell shares a dark background (palette entry 0); entry 1 is the lit
    //     glyph colour. Blank cells render as the dark background, so this one layer is the backdrop too.
    const Rgba8 kBg{16, 18, 28};
    const std::array<Rgba8, 2> textWhite{{ kBg, {235, 238, 248} }};
    const std::array<Rgba8, 2> textGold {{ kBg, {236, 196, 96} }};
    const std::array<Rgba8, 2> textCyan {{ kBg, {130, 220, 230} }};
    const std::array<PaletteId, 3> textPals{
        renderer.uploadPalette(std::span<const Rgba8>(textWhite)),
        renderer.uploadPalette(std::span<const Rgba8>(textGold)),
        renderer.uploadPalette(std::span<const Rgba8>(textCyan)),
    };
    enum TextPal { TXT_WHITE = 0, TXT_GOLD, TXT_CYAN };

    // ── 5. Game state ────────────────────────────────────────────────────────────────────────────────
    enum class GameState { Title, Playing };
    GameState state = GameState::Title;

    std::array<std::array<Cell, kBrickCols>, kBrickRows> grid{};
    int bricksLeft = 0;

    float paddleX = kViewW / 2.0f - kPaddleW / 2.0f;
    float ballX = 0, ballY = 0, ballVx = 0, ballVy = 0;
    int   score = 0, lives = kLives;
    bool  serving = true;
    std::uint32_t rng = static_cast<std::uint32_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());

    auto brickX = [&](int col) { return kBrickMarginX + col * kBrickCellW; };
    auto brickY = [&](int row) { return kBrickAreaTop + row * kBrickCellH; };

    // Lay out a board: rows 0..5 colour (one hue each), row 6 silver (2 hits), row 7 colour with three
    // indestructible GOLD bricks (ends + centre) — isolated, so they never wall off the board.
    auto buildBoard = [&] {
        bricksLeft = 0;
        for (int r = 0; r < kBrickRows; ++r) {
            for (int c = 0; c < kBrickCols; ++c) {
                Cell& cell = grid[static_cast<std::size_t>(r)][static_cast<std::size_t>(c)];
                if (r <= 5) {
                    cell = Cell{Brick::Colour, 1, r};
                    ++bricksLeft;
                } else if (r == 6) {
                    cell = Cell{Brick::Silver, 2, 0};
                    ++bricksLeft;
                } else {  // r == 7
                    if (c == 0 || c == kBrickCols / 2 || c == kBrickCols - 1) {
                        cell = Cell{Brick::Gold, 0, 0};  // indestructible, not counted
                    } else {
                        cell = Cell{Brick::Colour, 1, 2};
                        ++bricksLeft;
                    }
                }
            }
        }
    };

    auto parkBall = [&] {
        ballX = paddleX + kPaddleW / 2 - kBallSz / 2;
        ballY = kPaddleY - kBallSz - 1;
        ballVx = 0; ballVy = 0;
    };

    // Aim the ball UPWARD with a desired horizontal velocity plus a tiny random jitter, renormalised so
    // |v| ≈ kBallSpeed. The jitter keeps the ball off any perfectly-periodic orbit, so it can't trap
    // itself bouncing forever in an already-cleared column — it always drifts to reach every brick.
    auto aimBallUp = [&](float desiredVx) {
        desiredVx += (static_cast<float>(nextRand(rng) % 1000) / 1000.0f - 0.5f) * 0.8f;
        desiredVx = std::clamp(desiredVx, -kMaxEnglish, kMaxEnglish);
        if (std::abs(desiredVx) < kMinEnglish) desiredVx = (desiredVx < 0 ? -kMinEnglish : kMinEnglish);
        ballVx = desiredVx;
        ballVy = -std::sqrt(std::max(0.5f, kBallSpeed * kBallSpeed - ballVx * ballVx));
    };
    auto serve = [&] {
        aimBallUp((static_cast<float>(nextRand(rng) % 200) / 100.0f - 1.0f) * 1.4f);
        serving = false;
    };

    auto newGame = [&] {
        score = 0; lives = kLives; serving = true;
        buildBoard(); parkBall();
    };
    auto nextBoard = [&] {  // cleared the board → fresh bricks, keep score + lives
        buildBoard(); serving = true; parkBall();
        std::printf("board cleared — score %d — next board\n", score);
    };
    auto loseLife = [&] {
        if (--lives <= 0) {
            std::printf("game over — score %d — back to title\n", score);
            state = GameState::Title;
            return;
        }
        serving = true; parkBall();
    };

    // ── 6. Simulation step (60 Hz) ───────────────────────────────────────────────────────────────────
    loop.setTick([&](const InputState& in) {
        // 6a. Title: ENTER (Return / gamepad Start) starts a fresh game.
        if (state == GameState::Title) {
            if (in.justPressed(Button::Start)) { state = GameState::Playing; newGame(); }
            return;
        }

        // 6b. Paddle control — digital, gamepad stick, and the mouse (absolute + relative). The keyboard
        //     and stick move it incrementally; then, if the mouse moved THIS tick, the most-recent intent
        //     wins: an on-screen cursor positions the paddle absolutely, else the raw delta drives it as
        //     a spinner.
        if (in.isHeld(Button::Left))  paddleX -= kPaddleSpeed;
        if (in.isHeld(Button::Right)) paddleX += kPaddleSpeed;
        const float stickX = in.stick(Stick::Left).x;
        if (std::abs(stickX) > 0.25f) paddleX += stickX * kPaddleSpeed;
        if (in.cursorOnScreen() && in.cursorDelta().x != 0) {
            paddleX = static_cast<float>(in.cursor().x) - kPaddleW / 2.0f;   // absolute cursor → paddle
        } else if (in.rawDeltaX() != 0.0f) {
            paddleX += in.rawDeltaX() * kSpinnerSens;                        // relative spinner feel
        }
        paddleX = std::clamp(paddleX, 0.0f, kViewW - kPaddleW);

        // 6c. While serving the ball rides the paddle; A (keyboard X / gamepad South) launches it.
        if (serving) {
            parkBall();
            if (in.justPressed(Button::A)) serve();
            return;
        }

        // 6d. Integrate the ball.
        ballX += ballVx;
        ballY += ballVy;

        // 6e. Walls: bounce off left / right / top; falling past the bottom loses a life.
        if (ballX < 0)                { ballX = 0;                ballVx = std::abs(ballVx); }
        if (ballX > kViewW - kBallSz) { ballX = kViewW - kBallSz; ballVx = -std::abs(ballVx); }
        if (ballY < kPlayTop)         { ballY = kPlayTop;         ballVy = std::abs(ballVy); }
        if (ballY > kViewH)           { loseLife(); return; }

        // 6f. Paddle: bounce up with english from where on the paddle the ball hit (centre = straight up,
        //     edges = a steep angle). Speed is renormalised so |v| stays ≈ kBallSpeed.
        if (ballVy > 0 &&
            ballX < paddleX + kPaddleW && ballX + kBallSz > paddleX &&
            ballY + kBallSz >= kPaddleY && ballY + kBallSz <= kPaddleY + kPaddleH + 6) {
            const float hit = ((ballX + kBallSz / 2) - (paddleX + kPaddleW / 2)) / (kPaddleW / 2);  // [-1,1]
            aimBallUp(hit * kMaxEnglish);
            ballY = kPaddleY - kBallSz - 1;
        }

        // 6g. Bricks: reflect off the nearer face (min-translation axis) and EJECT the ball out of the
        //     brick (so an indestructible gold brick can't trap it). Colour breaks in one hit; silver
        //     cracks then breaks on the second; gold only bounces. One brick per tick.
        for (int r = 0; r < kBrickRows && (ballVx != 0 || ballVy != 0); ++r) {
            for (int c = 0; c < kBrickCols; ++c) {
                Cell& cell = grid[static_cast<std::size_t>(r)][static_cast<std::size_t>(c)];
                if (cell.type == Brick::None) continue;
                const float bx = brickX(c), by = brickY(r);
                if (ballX < bx + kBrickW && ballX + kBallSz > bx &&
                    ballY < by + kBrickH && ballY + kBallSz > by) {
                    const float overlapL = (ballX + kBallSz) - bx;   // penetration from the left face
                    const float overlapR = (bx + kBrickW) - ballX;   // from the right
                    const float overlapT = (ballY + kBallSz) - by;   // from the top
                    const float overlapB = (by + kBrickH) - ballY;   // from the bottom
                    if (std::min(overlapL, overlapR) < std::min(overlapT, overlapB)) {
                        ballVx = -ballVx;
                        ballX += (overlapL < overlapR ? -overlapL : overlapR);
                    } else {
                        ballVy = -ballVy;
                        ballY += (overlapT < overlapB ? -overlapT : overlapB);
                    }
                    if (cell.type == Brick::Gold) {
                        // indestructible — bounce only.
                    } else if (cell.type == Brick::Silver && --cell.hp > 0) {
                        // cracked — a hit SFX hook will go here in S2; for now just the palette change.
                    } else {
                        score += (cell.type == Brick::Silver) ? kScoreSilver : kScoreColour;
                        cell.type = Brick::None;
                        if (--bricksLeft == 0) nextBoard();
                    }
                    return;  // one brick per tick
                }
            }
        }
    });

    // Reused per-frame buffers.
    std::vector<TileCell> cells(static_cast<std::size_t>(kMapW) * kMapH);
    std::vector<Sprite>   sprites;

    // Stamp a string into the text grid at (col, row) with a palette; clipped to the row.
    auto stampText = [&](int col, int row, std::string_view s, std::uint8_t pal) {
        for (std::size_t i = 0; i < s.size() && col + static_cast<int>(i) < kMapW; ++i) {
            TileCell& cell = cells[static_cast<std::size_t>(row) * kMapW + col + static_cast<int>(i)];
            cell.tile    = glyphTile(s[i]);
            cell.palette = pal;
        }
    };
    auto centred = [&](int row, std::string_view s, std::uint8_t pal) {
        stampText((kMapW - static_cast<int>(s.size())) / 2, row, s, pal);
    };

    auto placeSprite = [&](float x, float y, AssetDimensions size, Slot s, int pal) {
        sprites.push_back(Sprite{
            .x = static_cast<int>(x), .y = static_cast<int>(y), .size = size,
            .tile = slotTile(s), .palette = static_cast<std::uint8_t>(pal)});
    };

    // ── 7. Render step ───────────────────────────────────────────────────────────────────────────────
    loop.setRender([&](float alpha) {
        // 7a. Text layer: clear to the dark background, then draw the title or the HUD.
        const std::uint16_t blank = glyphTile(' ');
        for (TileCell& c : cells) { c.tile = blank; c.palette = TXT_WHITE; }
        if (state == GameState::Title) {
            centred(22, "BONGUSOID", TXT_GOLD);
            centred(30, "PRESS ENTER TO PLAY", TXT_CYAN);
            centred(40, "MOVE PADDLE   A SERVES", TXT_WHITE);
        } else {
            char buf[16];
            std::snprintf(buf, sizeof buf, "%06d", score);
            stampText(2, 1, "SCORE", TXT_WHITE);
            stampText(8, 1, buf, TXT_WHITE);
            std::snprintf(buf, sizeof buf, "%d", lives);
            stampText(70, 1, "LIVES", TXT_WHITE);
            stampText(76, 1, buf, TXT_WHITE);
        }

        // 7b. Sprite layer (only while playing): standing bricks, paddle, ball.
        sprites.clear();
        if (state == GameState::Playing) {
            for (int r = 0; r < kBrickRows; ++r) {
                for (int c = 0; c < kBrickCols; ++c) {
                    const Cell& cell = grid[static_cast<std::size_t>(r)][static_cast<std::size_t>(c)];
                    Slot slot = S_BRICK; int pal = cell.colour;
                    if (cell.type == Brick::None) continue;
                    if (cell.type == Brick::Silver) { slot = S_SILVER; pal = cell.hp >= 2 ? PAL_SILVER : PAL_SILVER_CRACK; }
                    else if (cell.type == Brick::Gold) { slot = S_GOLD; pal = PAL_GOLD; }
                    placeSprite(brickX(c), brickY(r), AssetDimensions{static_cast<int>(kBrickW), static_cast<int>(kBrickH)}, slot, pal);
                }
            }
            placeSprite(paddleX, kPaddleY, AssetDimensions{static_cast<int>(kPaddleW), static_cast<int>(kPaddleH)}, S_VAUS, PAL_PADDLE);
            placeSprite(ballX, ballY, AssetDimensions{static_cast<int>(kBallSz), static_cast<int>(kBallSz)}, S_BALL, PAL_BALL);
        }

        // 7c. Assemble the frame: text/backdrop (z=0) → play sprites (z=10).
        FrameDrawState frame;
        DrawLayer bg{};
        bg.id = "backdrop"; bg.z = 0; bg.size = PixelSize{kViewW, kViewH};
        bg.content = TileContent{fontAtlas, std::span<const PaletteId>(textPals),
                                 kMapW, kMapH, std::span<const TileCell>(cells)};
        frame.layers.push_back(bg);

        DrawLayer play{};
        play.id = "play"; play.z = 10; play.size = PixelSize{kViewW, kViewH};
        play.content = SpriteContent{spriteAtlas, std::span<const PaletteId>(spritePals),
                                     std::span<const Sprite>(sprites)};
        frame.layers.push_back(play);

        renderer.renderFrame(frame, alpha);
    });

    std::printf("Bongusoid (640×480, 60 Hz) — ENTER to start; Left/Right or the mouse move the paddle, "
                "A serves. Silver takes two hits, gold never breaks. Close the window to quit.\n");
    WindowedHost{loop, platform}.run();
    return 0;
}
