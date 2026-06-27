#include "render.h"

#include <cstddef>
#include <span>

#include "layout.h"

namespace numberator {

using namespace retropp;

namespace {
constexpr int kGlyphW = 24;  // a font cell is 24 wide
}

void View::build(FrameDrawState& frame, const Assets& a, const std::string& display, int pressedKey) {
    frame.layers.clear();

    // z=0 — the chrome: window body, title bar, and the sunken display well, assembled from the map PNG.
    DrawLayer chrome{};
    chrome.id      = "chrome";
    chrome.z       = 0;
    chrome.size    = PixelSize{kViewW, kViewH};
    chrome.content = a.chromeMap.asTileContent(TileWrap::Blank);
    frame.layers.push_back(std::move(chrome));

    // z=10 — the key sprites. A held key flips X and Y, so its raised bevel inverts to a sunken one.
    keys_.clear();
    for (int k = 0; k < static_cast<int>(kKeys.size()); ++k) {
        const Key&       key  = kKeys[static_cast<std::size_t>(k)];
        const AssetSlot& slot = a.buttonSlots[key.isOp ? 1 : 0];
        const bool       down = (k == pressedKey);
        keys_.push_back(Sprite{.x = keyX(k % kCols), .y = keyY(k / kCols),
                               .size = slot.dimensions, .tile = slot.tile,
                               .atlas = a.buttons, .palette = a.palette, .flipX = down, .flipY = down});
    }
    DrawLayer keyLayer{};
    keyLayer.id      = "keys";
    keyLayer.z       = 10;
    keyLayer.size    = PixelSize{kViewW, kViewH};
    keyLayer.content = SpriteContent{std::span<const Sprite>(keys_)};
    frame.layers.push_back(std::move(keyLayer));

    // z=20 — the glyphs: each key's label centred on it, plus the right-aligned display digits. The font's
    // background index is the palette's alpha-0 entry, so the black ink composites over keys and the well.
    glyphs_.clear();
    for (int k = 0; k < static_cast<int>(kKeys.size()); ++k) {
        const Key&       key   = kKeys[static_cast<std::size_t>(k)];
        const AssetSlot& glyph = a.glyphSlots[static_cast<std::size_t>(glyphSlot(key.glyph))];
        const int        nudge = (k == pressedKey) ? 2 : 0;  // the label sinks with the held key
        glyphs_.push_back(Sprite{.x = keyX(k % kCols) + (kBtnW - glyph.dimensions.width) / 2 + nudge,
                                 .y = keyY(k / kCols) + (kBtnH - glyph.dimensions.height) / 2 + nudge,
                                 .size = glyph.dimensions, .tile = glyph.tile,
                                 .atlas = a.font, .palette = a.palette});
    }
    // The display string, right-aligned; a glyph that would spill past the well's left edge is dropped.
    const int rightEdge = kDispX + kDispW - 8;
    const int n         = static_cast<int>(display.size());
    const int startX    = rightEdge - n * kGlyphW;
    const int dispY     = kDispY + (kDispH - 32) / 2;
    for (int i = 0; i < n; ++i) {
        const int x = startX + i * kGlyphW;
        if (x < kDispX + 2) continue;
        const int slotIndex = glyphSlot(display[static_cast<std::size_t>(i)]);
        if (slotIndex < 0 || slotIndex >= static_cast<int>(a.glyphSlots.size())) continue;
        const AssetSlot& glyph = a.glyphSlots[static_cast<std::size_t>(slotIndex)];
        glyphs_.push_back(Sprite{.x = x, .y = dispY, .size = glyph.dimensions, .tile = glyph.tile,
                                 .atlas = a.font, .palette = a.palette});
    }
    DrawLayer glyphLayer{};
    glyphLayer.id      = "glyphs";
    glyphLayer.z       = 20;
    glyphLayer.size    = PixelSize{kViewW, kViewH};
    glyphLayer.content = SpriteContent{std::span<const Sprite>(glyphs_)};
    frame.layers.push_back(std::move(glyphLayer));
}

}  // namespace numberator
