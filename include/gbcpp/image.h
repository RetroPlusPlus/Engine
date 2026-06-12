#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

#include "gbcpp/palette.h"  // Rgba8

namespace gbcpp {

// How a decoded image carries colour. The PNG's colour type routes the load: a palette or
// grayscale PNG is INDEXED (one palette index per pixel, faithful console model); a truecolour
// PNG is RGBA (one colour per pixel). Identity is this field — first member.
enum class ImageColorKind : std::uint8_t { Indexed, Rgba };

// A decoded image. For an INDEXED source, `indices` holds one palette index per pixel (row-major)
// and `palette` holds the embedded palette (PLTE) when the PNG carries one — empty for grayscale
// (grayscale carries no embedded colour; the consumer supplies a palette separately). For an RGBA
// source, `pixels` holds one colour per pixel (row-major). Only the field matching `kind` is
// populated. Dimensions are in pixels.
struct LoadedImage {
    ImageColorKind            kind   = ImageColorKind::Indexed;  // identity, first member
    int                       width  = 0;
    int                       height = 0;
    std::vector<std::uint8_t> indices;   // kind == Indexed: one index per pixel, row-major
    std::vector<Rgba8>        palette;   // kind == Indexed: embedded palette (may be empty)
    std::vector<Rgba8>        pixels;    // kind == Rgba: one colour per pixel (ENG-2.B.3.b)
};

// Decode a PNG file into a LoadedImage. Indexed/grayscale PNGs populate `indices` (+ `palette`
// for PLTE PNGs). A truecolour PNG throws std::runtime_error("...ENG-2.B.3.b...") for now (the
// RGBA consumer is B.3.b). Throws std::runtime_error on a missing file or a decode failure.
[[nodiscard]] LoadedImage loadPng(const std::filesystem::path& path);

// Same, from an in-memory PNG byte span (used by the headless tests and embeddable assets).
[[nodiscard]] LoadedImage loadPngFromMemory(std::span<const std::uint8_t> bytes);

}  // namespace gbcpp
