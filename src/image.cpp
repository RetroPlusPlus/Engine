#include "retropp/image.h"

#include <stdexcept>
#include <string>

#include <SDL3/SDL.h>

#include "lodepng.h"
#include "retropp/asset_policy.h"    // resolveAssetPolicy
#include "retropp/asset_registry.h"  // detail::configDefaultAssetPolicy / findEmbeddedAsset

namespace retropp {

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

// Read one `bits`-wide sample (bits ∈ {1,2,4,8,16}) from a PNG scanline as a uint16 — the map path's
// widened readSample. For 16 bits PNG stores the sample big-endian (high byte first); sub-byte and
// 8-bit samples match readSample exactly. `bitPos` is the sample's bit offset from the row start.
[[nodiscard]] std::uint16_t readSample16(const unsigned char* row, std::size_t bitPos, unsigned bits) {
    const std::size_t byte = bitPos / 8;
    if (bits == 16) return static_cast<std::uint16_t>((row[byte] << 8) | row[byte + 1]);  // PNG big-endian
    if (bits == 8) return row[byte];
    const unsigned shift = 8u - bits - static_cast<unsigned>(bitPos % 8);
    const unsigned mask  = (1u << bits) - 1u;
    return static_cast<std::uint16_t>((row[byte] >> shift) & mask);
}

// Unpack a raw (unconverted) PNG scanline buffer into one uint16 index per pixel — the map path's
// widened unpackIndices. `channels` is the per-pixel sample count (1 for palette/grayscale, 2 for
// grayscale+alpha — the grey sample is the first channel, the only one kept); `bitdepth` the per-
// sample bit width. Faithful: the sample value IS the index, never scaled or reverse-derived.
[[nodiscard]] std::vector<std::uint16_t> unpackIndices16(const std::vector<unsigned char>& raw,
                                                         unsigned width, unsigned height,
                                                         unsigned channels, unsigned bitdepth) {
    const std::size_t bitsPerPixel = static_cast<std::size_t>(bitdepth) * channels;
    const std::size_t rowBytes     = (static_cast<std::size_t>(width) * bitsPerPixel + 7) / 8;
    std::vector<std::uint16_t> values(static_cast<std::size_t>(width) * height);
    for (unsigned y = 0; y < height; ++y) {
        const unsigned char* row = raw.data() + static_cast<std::size_t>(y) * rowBytes;
        for (unsigned x = 0; x < width; ++x) {
            const std::size_t bitPos = static_cast<std::size_t>(x) * bitsPerPixel;
            values[static_cast<std::size_t>(y) * width + x] = readSample16(row, bitPos, bitdepth);
        }
    }
    return values;
}

}  // namespace

IndexGrid loadMapPngFromMemory(std::span<const std::uint8_t> bytes) {
    const unsigned char* data = bytes.data();
    const std::size_t    size = bytes.size();

    // Inspect IHDR first so a truecolour source is rejected before any pixel decode, and so the
    // colour type / bit depth route the unpack. lodepng::State IS-A LodePNGState.
    lodepng::State state;
    unsigned width = 0, height = 0;
    if (const unsigned err = lodepng_inspect(&width, &height, &state, data, size)) {
        throw std::runtime_error(std::string{"loadMapPng: "} + lodepng_error_text(err));
    }

    const LodePNGColorType colortype = state.info_png.color.colortype;
    const unsigned         bitdepth  = state.info_png.color.bitdepth;

    // A map carries INDICES, not colour — truecolour is meaningless here and rejected.
    if (colortype == LCT_RGB || colortype == LCT_RGBA) {
        throw std::runtime_error("loadMapPng: a map PNG must be grayscale or palette, not truecolour");
    }

    // Decode WITHOUT colour conversion: palette indices and grey sample values come through unscaled;
    // we unpack the (possibly sub-byte, possibly 16-bit) samples to one uint16 index per pixel.
    state.decoder.color_convert = 0;
    std::vector<unsigned char> raw;
    if (const unsigned err = lodepng::decode(raw, width, height, state, data, size)) {
        throw std::runtime_error(std::string{"loadMapPng: "} + lodepng_error_text(err));
    }

    IndexGrid grid;
    grid.width  = static_cast<int>(width);
    grid.height = static_cast<int>(height);
    const unsigned channels = (colortype == LCT_GREY_ALPHA) ? 2u : 1u;
    grid.values = unpackIndices16(raw, width, height, channels, bitdepth);
    return grid;
}

IndexGrid loadMapPng(LiteralPath path, std::optional<AssetPolicy> policy) {
    // Resolve embed-vs-load: per-call > EngineConfig::defaultAssetPolicy > loadMapPng's per-type
    // default (Embed). An Embed asset decodes from the bytes the build baked into the binary, keyed by
    // its logical path; if none were baked (the target was not run through the asset scan) we fall
    // through to the disk read.
    if (resolveAssetPolicy(policy, detail::configDefaultAssetPolicy(), AssetPolicy::Embed) ==
        AssetPolicy::Embed) {
        if (const std::span<const std::uint8_t> bytes = detail::findEmbeddedAsset(path.view());
            !bytes.empty()) {
            return loadMapPngFromMemory(bytes);
        }
    }
    // LoadFromPath (or an un-baked Embed): resolve the logical path against the runtime asset root.
    const std::filesystem::path full = assetRoot() / path.c_str();
    std::vector<unsigned char> file;
    if (const unsigned err = lodepng::load_file(file, full.string())) {
        throw std::runtime_error(std::string{"loadMapPng: "} + lodepng_error_text(err));
    }
    return loadMapPngFromMemory(std::span<const std::uint8_t>(file.data(), file.size()));
}

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

    // Truecolour / truecolour-alpha is the RGBA branch — a declared seam, not yet implemented.
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

std::vector<AssetSlot> sliceLayout(PixelSize imageSize, AssetDimensions assetSize,
                                   ContentKind kind, ReadOrder order, int count) {
    // Degenerate image → nothing to carve (also guards the divisions below).
    if (imageSize.width <= 0 || imageSize.height <= 0) {
        return {};
    }

    // Single: the whole image is one asset, regardless of assetSize/order/count — slot 0 spans the image.
    if (kind == ContentKind::Single) {
        return {AssetSlot{0, AssetDimensions{imageSize.width, imageSize.height}}};
    }

    // Grid (Tileset / SpriteSeries — identical carving): the asset must be a POSITIVE multiple of the
    // 8px cell grid and fit within the image on both axes, else the grid is undefined → empty.
    if (assetSize.width <= 0 || assetSize.height <= 0 ||
        assetSize.width % kAtlasCellPx != 0 || assetSize.height % kAtlasCellPx != 0 ||
        assetSize.width > imageSize.width || assetSize.height > imageSize.height) {
        return {};
    }

    const int gridCols = imageSize.width  / assetSize.width;   // floor: trailing partial column dropped
    const int gridRows = imageSize.height / assetSize.height;  // floor: trailing partial row dropped
    const int cellsAcross = imageSize.width / kAtlasCellPx;    // atlas cell stride (matches the shader)

    // Full cells only — a trailing remainder is dropped and logged, never silently claimed as covered.
    const int remainderX = imageSize.width  - gridCols * assetSize.width;
    const int remainderY = imageSize.height - gridRows * assetSize.height;
    if (remainderX != 0 || remainderY != 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_RENDER,
                    "retropp: sliceLayout dropped a %dx%d-px trailing remainder of a %dx%d image not "
                    "covered by whole %dx%d cells (%d cols x %d rows carved)",
                    remainderX, remainderY, imageSize.width, imageSize.height,
                    assetSize.width, assetSize.height, gridCols, gridRows);
    }

    // The two axis sequences, each forward or reversed per the order's direction; `fill` picks which
    // is the inner loop. emit() converts a (col, row) cell to its top-left 8px atlas cell index.
    std::vector<int> colSeq(static_cast<std::size_t>(gridCols));
    for (int i = 0; i < gridCols; ++i) {
        colSeq[static_cast<std::size_t>(i)] =
            order.horizontal == ReadOrder::HorizontalDir::LeftToRight ? i : gridCols - 1 - i;
    }
    std::vector<int> rowSeq(static_cast<std::size_t>(gridRows));
    for (int i = 0; i < gridRows; ++i) {
        rowSeq[static_cast<std::size_t>(i)] =
            order.vertical == ReadOrder::VerticalDir::TopToBottom ? i : gridRows - 1 - i;
    }

    // How many slots to emit: 0 = the whole grid; a positive count caps to the first `count` in read
    // order (for a sheet with room for more cells than the art uses). A count past the grid capacity
    // is clamped to capacity and logged — never invents empty slots beyond the real cells.
    const int capacity = gridCols * gridRows;
    int limit = capacity;
    if (count > 0) {
        if (count > capacity) {
            SDL_LogWarn(SDL_LOG_CATEGORY_RENDER,
                        "retropp: sliceLayout asked for %d assets but the %dx%d-px image holds only %d "
                        "whole %dx%d cells; carving all %d",
                        count, imageSize.width, imageSize.height, capacity,
                        assetSize.width, assetSize.height, capacity);
        } else {
            limit = count;
        }
    }

    std::vector<AssetSlot> slots;
    slots.reserve(static_cast<std::size_t>(limit));
    auto emit = [&](int c, int r) {
        if (static_cast<int>(slots.size()) >= limit) return;  // stop once `count` assets are carved
        const int pxTop = c * assetSize.width;
        const int pyTop = r * assetSize.height;
        const int tile  = (pyTop / kAtlasCellPx) * cellsAcross + (pxTop / kAtlasCellPx);
        slots.push_back(AssetSlot{static_cast<std::uint16_t>(tile), assetSize});
    };

    if (order.fill == ReadOrder::Fill::Rows) {
        for (const int r : rowSeq) {
            for (const int c : colSeq) emit(c, r);
        }
    } else {  // Fill::Columns
        for (const int c : colSeq) {
            for (const int r : rowSeq) emit(c, r);
        }
    }
    return slots;
}

}  // namespace retropp
