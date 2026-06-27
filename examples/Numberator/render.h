// Numberator — the view: turns the assets + the calculator's display string + which key is held into a
// FrameDrawState. Three layers by z: the chrome tilemap, the key sprites (a held key flips to sunken),
// and the glyph sprites (key labels + the right-aligned display). Rebuilt each frame; its sprite vectors
// back the spans the renderer reads, so they live as long as the View.
#pragma once

#include <string>
#include <vector>

#include "retropp/draw_state.h"  // FrameDrawState, Sprite

#include "assets.h"

namespace numberator {

class View {
public:
    void build(retropp::FrameDrawState& frame, const Assets& assets,
               const std::string& display, int pressedKey);

private:
    std::vector<retropp::Sprite> keys_;    // backs the key-layer span
    std::vector<retropp::Sprite> glyphs_;  // backs the glyph-layer span (labels + display)
};

}  // namespace numberator
