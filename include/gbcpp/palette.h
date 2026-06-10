#pragma once

#include <cstdint>

namespace gbcpp {

// Colour resources for the indexed-tile / runtime-palette pipeline (ENG-2.B.2.b).
//
// The faithful GB/C colour model: tile art is INDICES (0..N-1), and the colour comes from a
// palette selected at render time — never baked into the art. An indexed atlas holds one
// palette index per pixel; a palette maps each index to a final output colour; a tile picks
// which palette (within its layer's set) it draws from. Application happens per-pixel in the
// fragment shader, so palette swaps, day/night, and animation stay data/shader concerns —
// never a palette-RAM poke or scanline idiom.

// One palette entry: a final output colour. Identity is the named channels — never a
// positional quad. Default is opaque black. The engine stores Rgba8 output regardless of the
// source console; a palette's colours come from the game's assets at upload time.
struct Rgba8 {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
    std::uint8_t a = 255;
    [[nodiscard]] constexpr bool operator==(const Rgba8&) const noexcept = default;
};
static_assert(sizeof(Rgba8) == 4);

// A handle to uploaded palette colour data the renderer owns. Identity is the typed handle;
// the renderer maps it to a row in its palette store. Sibling to AtlasId.
enum class PaletteId : std::uint32_t {};

// Named entry-count presets. The enumerator VALUE is the entry count, so a caller passes a
// raw integer or a preset interchangeably (static_cast<std::uint32_t>(PaletteSize::GameBoy) == 4).
// These are count mnemonics, not per-console colour models — the engine stores Rgba8 output
// regardless; the preset sets only how many entries. The engine generalizes beyond GB, so
// baking 4 into the type would repeat the hardcoded-32x32-tilemap mistake.
enum class PaletteSize : std::uint32_t {
    GameBoy      = 4,
    GameBoyColor = 4,
    Nes          = 4,
    MasterSystem = 16,
    Genesis      = 16,
    Snes         = 16,
};

}  // namespace gbcpp
