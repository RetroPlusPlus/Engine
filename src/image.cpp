#include "gbcpp/image.h"

#include <stdexcept>
#include <string>

#include "lodepng.h"

namespace gbcpp {

namespace {

// Read one `bits`-wide sample (bits ∈ {1,2,4,8}) from a PNG scanline, MSB-first within the byte —
// the PNG sub-byte packing order (the left-most pixel occupies the most-significant bits, each
// scanline starts on a byte boundary). `bitPos` is the sample's bit offset from the row start.
[[nodiscard]] std::uint8_t readSample(const unsigned char* row, std::size_t bitPos, unsigned bits) {
    const std::size_t byte = bitPos / 8;
    if (bits == 8) return row[byte];
    const unsigned shift = 8u - bits - static_cast<unsigned>(bitPos % 8);
    const unsigned mask  = (1u << bits) - 1u;
    return static_cast<std::uint8_t>((row[byte] >> shift) & mask);
}

// Unpack a raw (unconverted) PNG scanline buffer into one index byte per pixel. `channels` is the
// per-pixel sample count (1 for palette/grayscale, 2 for grayscale+alpha — the grey sample is the
// first channel and the only one we keep); `bitdepth` is the per-sample bit width. Faithful: the
// sample value IS the index, never scaled or reverse-derived from a colour.
[[nodiscard]] std::vector<std::uint8_t> unpackIndices(const std::vector<unsigned char>& raw,
                                                      unsigned width, unsigned height,
                                                      unsigned channels, unsigned bitdepth) {
    const std::size_t bitsPerPixel = static_cast<std::size_t>(bitdepth) * channels;
    const std::size_t rowBytes     = (static_cast<std::size_t>(width) * bitsPerPixel + 7) / 8;
    std::vector<std::uint8_t> indices(static_cast<std::size_t>(width) * height);
    for (unsigned y = 0; y < height; ++y) {
        const unsigned char* row = raw.data() + static_cast<std::size_t>(y) * rowBytes;
        for (unsigned x = 0; x < width; ++x) {
            // The grey/palette sample is the pixel's first channel (alpha, if any, follows).
            const std::size_t bitPos = static_cast<std::size_t>(x) * bitsPerPixel;
            indices[static_cast<std::size_t>(y) * width + x] = readSample(row, bitPos, bitdepth);
        }
    }
    return indices;
}

}  // namespace

LoadedImage loadPngFromMemory(std::span<const std::uint8_t> bytes) {
    const unsigned char* data = bytes.data();
    const std::size_t    size = bytes.size();

    // Inspect the IHDR first so a truecolour source is rejected before any pixel decode, and so the
    // colour type / bit depth route the unpack. lodepng::State IS-A LodePNGState.
    lodepng::State state;
    unsigned width = 0, height = 0;
    if (const unsigned err = lodepng_inspect(&width, &height, &state, data, size)) {
        throw std::runtime_error(std::string{"loadPng: "} + lodepng_error_text(err));
    }

    const LodePNGColorType colortype = state.info_png.color.colortype;
    const unsigned         bitdepth  = state.info_png.color.bitdepth;

    // Truecolour / truecolour-alpha is the RGBA branch — the seam exists, the consumer is B.3.b.
    if (colortype == LCT_RGB || colortype == LCT_RGBA) {
        throw std::runtime_error("loadPng: RGBA image sources are supported in ENG-2.B.3.b");
    }
    // The indexed path is 8-bit indices (the R8 atlas). 16-bit grayscale would need >8-bit indices.
    if (bitdepth == 16) {
        throw std::runtime_error("loadPng: 16-bit samples are not supported on the indexed path");
    }

    // Decode WITHOUT colour conversion: the output stays in the PNG's own format, so palette indices
    // and grey sample values come through unscaled. We unpack the (possibly sub-byte) samples to one
    // index per pixel ourselves — exact for any bit depth, never reverse-derived from a colour.
    state.decoder.color_convert = 0;
    std::vector<unsigned char> raw;
    if (const unsigned err = lodepng::decode(raw, width, height, state, data, size)) {
        throw std::runtime_error(std::string{"loadPng: "} + lodepng_error_text(err));
    }

    LoadedImage image;
    image.kind   = ImageColorKind::Indexed;
    image.width  = static_cast<int>(width);
    image.height = static_cast<int>(height);

    const unsigned channels = (colortype == LCT_GREY_ALPHA) ? 2u : 1u;
    image.indices = unpackIndices(raw, width, height, channels, bitdepth);

    // A PLTE PNG carries its palette; grayscale carries none (the consumer supplies colour).
    if (colortype == LCT_PALETTE) {
        const unsigned char* p   = state.info_png.color.palette;
        const std::size_t    n   = state.info_png.color.palettesize;
        image.palette.reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            image.palette.push_back(Rgba8{p[4 * i + 0], p[4 * i + 1], p[4 * i + 2], p[4 * i + 3]});
        }
    }

    return image;
}

LoadedImage loadPng(const std::filesystem::path& path) {
    std::vector<unsigned char> file;
    if (const unsigned err = lodepng::load_file(file, path.string())) {
        throw std::runtime_error(std::string{"loadPng: "} + lodepng_error_text(err));
    }
    return loadPngFromMemory(std::span<const std::uint8_t>(file.data(), file.size()));
}

}  // namespace gbcpp
