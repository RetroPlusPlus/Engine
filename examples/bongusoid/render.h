#pragma once

// Bongusoid — the DRAW step, its own translation unit. Reads the game model + the loaded assets + the
// feel layer and composes the frame: a text/backdrop tile layer (HUD or title) under the sprite play
// layer (bricks, paddle, ball) under a popup layer (the floating "+N" score numbers). The paddle squash
// and ball spin ride in as per-sprite transforms; the screen shake rides in as a frame post-effect.
// Holds the reused per-frame buffers so the render loop allocates nothing steady-state.

#include <vector>

#include "retropp/draw_state.h"  // TileCell / Sprite
#include "retropp/renderer.h"    // Renderer

#include "assets.h"
#include "feel.h"
#include "game.h"

namespace bong {

class BongRenderer {
public:
    BongRenderer();
    void render(retropp::Renderer& renderer, const BongGame& game, const BongAssets& assets,
                const BongFeel& feel, float alpha);

private:
    std::vector<retropp::TileCell> cells_;         // text/backdrop layer (kMapW × kMapH)
    std::vector<retropp::Sprite>   sprites_;       // play layer (bricks / paddle / ball)
    std::vector<retropp::Sprite>   popupSprites_;  // floating "+N" score numbers (font glyphs)
};

}  // namespace bong
