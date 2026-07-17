#pragma once

// Bongusoid — the ASSET LOADER, its own translation unit. Slices the two committed indexed PNGs into
// addressable atlas slots and uploads the palettes, returning a BongAssets the sim-free render step reads.
// In S2 BOTH sheets are Embed (baked into the binary) — see assets.cpp for the policy, decided per call.

#include <array>
#include <cstdint>
#include <vector>

#include "retropp/image.h"     // AtlasManifest / AtlasId / ContentKind / ReadOrder
#include "retropp/palette.h"   // PaletteId
#include "retropp/renderer.h"  // Renderer (loadAtlas + uploadPalette)

#include "layout.h"

namespace bong {

// Everything the renderer needs to draw: the two sliced sheets + their palette sets, plus the glyph /
// slot lookups over them. The manifests are kept so a slot's `tile` is read directly (no separate table).
struct BongAssets {
    retropp::AtlasManifest font;    // 8×8 Tileset: digits 0–9, A–Z, space — the HUD / title text
    retropp::AtlasManifest sheet;   // 80×24 SpriteSeries: Vaus / ball / brick / silver / gold

    std::vector<retropp::PaletteId>     spritePals;  // 0..5 brick rows, 6/7 silver+crack, 8 gold, 9 paddle, 10 ball
    std::array<retropp::PaletteId, 3>   textPals{};  // TXT_WHITE / TXT_GOLD / TXT_CYAN

    [[nodiscard]] retropp::AtlasId fontAtlas()   const { return font.atlas; }
    [[nodiscard]] retropp::AtlasId spriteAtlas() const { return sheet.atlas; }

    // The sprite sheet is a single-row SpriteSeries, so slot s's atlas cell is sheet[s].tile.
    [[nodiscard]] std::uint16_t slotTile(Slot s) const {
        return sheet[static_cast<std::size_t>(s)].tile;
    }
    // The font is a single-row Tileset, so glyph k's tile == k; map a character to its glyph slot
    // (digits, then A–Z, then a space cell for anything unmapped).
    [[nodiscard]] std::uint16_t glyphTile(char ch) const {
        std::size_t k = 36;  // space (the 37th cell) for anything unmapped
        if (ch >= '0' && ch <= '9')      k = static_cast<std::size_t>(ch - '0');
        else if (ch >= 'A' && ch <= 'Z') k = static_cast<std::size_t>(10 + (ch - 'A'));
        return font[k].tile;
    }
    // The full-width horizontal rule (the gen script appends it as the LAST font cell) — a row of these
    // forms the HUD's bottom border.
    [[nodiscard]] std::uint16_t borderTile() const { return font[font.tileCount() - 1].tile; }
};

// Load + slice both sheets and upload every palette. May throw (loadAtlas throws on a missing/!indexed
// sheet); the caller wraps it. The asset POLICY is decided inside, per loadAtlas call (both Embed in S2).
[[nodiscard]] BongAssets loadBongAssets(retropp::Renderer& renderer);

}  // namespace bong
