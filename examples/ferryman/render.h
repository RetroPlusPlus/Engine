#pragma once

// Ferryman — the DRAW step, its own translation unit. Reads the game model + the loaded assets +
// the feel layer and composes the frame:
//
//   z=0   drift  — the deep-water tile layer, hash-varied across the FOUR water tiles, scrolling
//         slowly; its palette breathes via the shimmer clip (palette animation on the tile path).
//   z=2   swell  — the sparse sparkle/foam overlay scrolling at a DIFFERENT rate: the second
//         parallax plane. Mostly hole tiles; the sea shows through everything above it.
//   z=5   terrain — the sanctuary island band (pads + beacon + trim + a deck-plate quay row) and
//         the fixed islets (shore/median 32×32 macro-tiles + props), stamped as 4×4 cell groups.
//         Everything else is the hole tile — open water.
//   z=8   shadows — the flying craft's cast shadows on the sea (their own art, the flat shadow
//         palette, squashed + softened): the tell that a craft flies OVER the field.
//   z=10  ground — sprites: waiting / stashed / stunned colonists (idle-bob clips, stun dim).
//   z=20  bolts  — every bullet in flight, gold and magenta liveries of one art.
//   z=30  actors — sprites: the ferry + its deck passengers (thruster clip, invulnerability alpha
//         breath), the MUTANT (a water hunter — it alone touches the boat), and the pooled booms.
//   z=35  flyers — sprites ABOVE the boat: the enemy craft (running-light palette phases,
//         hit-flash alpha) and the abductor (wing-light clip). They fly over the ferry and never
//         collide with it — only their bullets bite.
//   z=40  popups — the floating "+N" numbers and the WAVE N round card (rich glyphs).
//   z=100 hud    — the band: SCORE/WAVE/LIVES, CREW/PAYS/SAVED, the alert slot, the rule.
//
// The abductor's tractor beam rides in as an Add-blended ColorFill capsule region; the sanctuary
// glow as an Add region scaled by the deck's load; the death shake as a frame post-effect. Every
// sprite carries a STABLE identity key; teleports (respawns, abductor visits, enemy re-entries)
// re-key, so the automatic interpolation eases real motion and mount-snaps the jumps. Holds the
// reused per-frame buffers so the render loop allocates nothing steady-state.

#include <vector>

#include "retropp/draw_state.h"  // TileCell / Sprite / FrameDrawState
#include "retropp/renderer.h"    // Renderer

#include "assets.h"
#include "feel.h"
#include "game.h"

namespace ferryman {

class FerrymanRenderer {
public:
    FerrymanRenderer();
    void render(retropp::Renderer& renderer, const FerrymanGame& game,
                const FerrymanAssets& assets, const FerrymanFeel& feel);

private:
    // The sea maps are BIGGER than the viewport — a margin of 8 macro-blocks in each direction —
    // so the scrolling planes never run out of grain at an edge, and each dimension is rounded UP
    // to a multiple of the 4-block water field so the toroidal wrap always lands on a field
    // boundary (an off-field size would jump the pattern mid-cycle: a seam wherever the layer wraps
    // as it scrolls — the A11 lesson). Sizes derive from the viewport, so a resize just works.
    static constexpr int kSeaMarginBlocks = 8;
    static constexpr int kSeaBlockCols =
        (((kViewW / kBlock) + kSeaMarginBlocks + kWaterField - 1) / kWaterField) * kWaterField;
    static constexpr int kSeaBlockRows =
        ((((kViewH + kBlock - 1) / kBlock) + kSeaMarginBlocks + kWaterField - 1) / kWaterField) *
        kWaterField;
    static constexpr int kSeaCellCols = kSeaBlockCols * 4;  // 8px-cell columns
    static constexpr int kSeaCellRows = kSeaBlockRows * 4;  // 8px-cell rows
    std::vector<retropp::TileCell> driftCells_;   // the sea (kSeaCellCols × kSeaCellRows)
    std::vector<retropp::TileCell> swellCells_;   // the sparkle plane (kSeaCellCols × kSeaCellRows)
    std::vector<retropp::TileCell> terrainCells_; // sanctuary + islets (kMapW × field rows)
    std::vector<retropp::TileCell> hudCells_;     // the HUD band (kMapW × kHudBandRows)
    std::vector<retropp::TileCell> titleCells_;   // the title-text layer (kMapW × kMapH)
    std::vector<retropp::TileCell> pauseCells_;   // the pause-menu overlay (kMapW × kMapH)
    std::vector<retropp::Sprite>   groundSprites_; // grounded colonists
    std::vector<retropp::Sprite>   wakeSprites_;   // the boat's trailing foam wake (on the sea)
    std::vector<retropp::Sprite>   shadowSprites_; // flying craft's cast shadows (low, on the sea)
    std::vector<retropp::Sprite>   boltSprites_;
    std::vector<retropp::Sprite>   actorSprites_;  // ferry / deck / mutant / booms (boat level)
    std::vector<retropp::Sprite>   flyerSprites_;  // flying craft + abductor (ABOVE the boat)
    std::vector<retropp::Sprite>   popupSprites_;  // "+N" numbers + the round card
    std::vector<retropp::Sprite>   titleSprites_;  // the FERRYMAN marquee (waving glyphs)
    std::vector<retropp::Region>   warpRegions_;   // the mutant wake's trailing distortion regions

    float driftScrollX_ = 0.0f, driftScrollY_ = 0.0f;  // advanced on the SIM tick via tickScroll()
    float swellScrollX_ = 0.0f, swellScrollY_ = 0.0f;

public:
    // The two water planes' scroll advances on the sim tick (state changes never live in the
    // render callback), at different rates AND on a gentle diagonal — the sea's continuous
    // travel lives HERE (smooth), while the tile frames only twinkle details.
    void tickScroll() {
        driftScrollX_ += 0.18f;
        driftScrollY_ += 0.05f;
        swellScrollX_ += 0.55f;
        swellScrollY_ += 0.11f;
    }
};

}  // namespace ferryman
