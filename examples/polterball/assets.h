#pragma once

// Polterball — the ASSET LOADER, its own translation unit. Slices the three committed indexed PNGs
// into addressable atlas slots, uploads every palette, and builds the two shared Animations (the
// power pellet's palette pulse and the ghosts' skirt wave). Everything is Embed — the built binary
// is self-contained; see assets.cpp for the policy, decided per call.

#include <array>
#include <cstdint>
#include <vector>

#include "retropp/animation.h"  // Animation — the pulse + skirt clips live beside the art they use
#include "retropp/image.h"      // AtlasManifest / AtlasId / ContentKind / ReadOrder
#include "retropp/palette.h"    // PaletteId
#include "retropp/renderer.h"   // Renderer (loadAtlas + uploadPalette)

#include "layout.h"

namespace polter {

// Everything the renderer needs to draw: the three sliced sheets + their palette sets + the shared
// animation clips, plus the glyph / slot / tile lookups over them.
struct PolterAssets {
    retropp::AtlasManifest font;    // 8×8 Tileset: digits 0–9, A–Z, space, border rule — HUD / title
    retropp::AtlasManifest tiles;   // 32×32 Tileset: hard / soft / pellet / floor / gate maze blocks
    retropp::AtlasManifest sheet;   // 80×24 SpriteSeries: paddle / ball / ghost A / ghost B / eyes / power

    std::vector<retropp::PaletteId>   spritePals;  // indexed by the Pal enum (layout.h)
    std::array<retropp::PaletteId, 4> tilePals{};  // indexed by TilePal
    std::array<retropp::PaletteId, 3> textPals{};  // indexed by TextPal

    // The power pellet's pulse — two frames of the SAME art under alternating palettes — and the
    // ghosts' two-frame skirt wave. Both are pure data; the feel layer owns the players that walk
    // them, per the game-owns-the-cursor contract.
    retropp::Animation powerPulse;
    retropp::Animation ghostWalk;

    [[nodiscard]] retropp::AtlasId fontAtlas()   const { return font.atlas; }
    [[nodiscard]] retropp::AtlasId tileAtlas()   const { return tiles.atlas; }
    [[nodiscard]] retropp::AtlasId spriteAtlas() const { return sheet.atlas; }

    // The sprite sheet is a single-row SpriteSeries, so slot s's atlas cell is sheet[s].tile.
    [[nodiscard]] std::uint16_t slotTile(Slot s) const {
        return sheet[static_cast<std::size_t>(s)].tile;
    }
    // A maze block's top-left 8px tile; the block's 4×4 tiles are offset from it on the sheet's
    // 8px-tile grid (stride kTileSheetCols8) — the stamping helper in render.cpp does that walk.
    [[nodiscard]] std::uint16_t blockTile(TileBlock b) const {
        return tiles[static_cast<std::size_t>(b)].tile;
    }
    // The font is a single-row Tileset; map a character to its glyph slot (digits, then A–Z, then a
    // space cell for anything unmapped).
    [[nodiscard]] std::uint16_t glyphTile(char ch) const {
        std::size_t k = 36;  // space (the 37th cell) for anything unmapped
        if (ch >= '0' && ch <= '9')      k = static_cast<std::size_t>(ch - '0');
        else if (ch >= 'A' && ch <= 'Z') k = static_cast<std::size_t>(10 + (ch - 'A'));
        return font[k].tile;
    }
    // The full-width horizontal rule (the generator appends it as the LAST font cell) — a row of
    // these forms the HUD's bottom border.
    [[nodiscard]] std::uint16_t borderTile() const { return font[font.tileCount() - 1].tile; }
};

// Load + slice all three sheets, upload every palette, build the shared clips. May throw (loadAtlas
// throws on a missing / non-indexed sheet); the caller wraps it.
[[nodiscard]] PolterAssets loadPolterAssets(retropp::Renderer& renderer);

}  // namespace polter
