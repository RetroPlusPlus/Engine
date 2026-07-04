#pragma once

// Polterball — the DRAW step, its own translation unit. Reads the game model + the loaded assets +
// the feel layer and composes the frame:
//
//   z=0   backdrop — ONE tile layer that is the HUD text, the maze, and the court at once. Its cells
//         mix TWO sheets freely (font glyphs up top, 32×32 maze blocks stamped as 4×4 tile groups
//         below) because every TileCell names its own atlas + palette — the engine's multi-sheet
//         layer, used for real.
//   z=10  maze actors — the ghosts (or their flying eyes), and the pulsing power pellets.
//   z=20  movers — the ball and the paddle (squash rides in as a per-sprite transform).
//   z=30  popups — the floating "+N" score numbers.
//
// The ignite glow rides in as an Add-blended ColorFill Region on the frame; the screen shake as a
// frame post-effect. Every sprite carries a STABLE identity key (the ghost's index+epoch, the
// ball's serve number, a popup's spawn id) so the engine's automatic interpolation eases real
// motion and mount-snaps real teleports — never a streak across the screen. Holds the reused
// per-frame buffers so the render loop allocates nothing steady-state.

#include <vector>

#include "retropp/draw_state.h"  // TileCell / Sprite / FrameDrawState
#include "retropp/renderer.h"    // Renderer

#include "assets.h"
#include "feel.h"
#include "game.h"

namespace polter {

class PolterRenderer {
public:
    PolterRenderer();
    void render(retropp::Renderer& renderer, const PolterGame& game, const PolterAssets& assets,
                const PolterFeel& feel);

private:
    std::vector<retropp::TileCell> cells_;         // the backdrop layer (kMapW × kMapH)
    std::vector<retropp::Sprite>   actorSprites_;  // ghosts / eyes / power pellets
    std::vector<retropp::Sprite>   moverSprites_;  // ball + paddle
    std::vector<retropp::Sprite>   popupSprites_;  // floating "+N" score numbers (font glyphs)
};

}  // namespace polter
