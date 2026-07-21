#pragma once

#include <cstddef>
#include <span>
#include <stdexcept>
#include <vector>

#include "retropp/image.h"  // AtlasId, AssetSlot

namespace retropp {

// The result of loading + slicing an atlas image — the SHEET: the uploaded atlas handle beside the
// carved sub-asset slots in read order. `manifest[i]` is the i-th carved asset's slot (its top-left
// atlas cell + dimensions) — feed slot.tile to a TileCell::tile / Sprite::tile and slot.dimensions to
// a Sprite::size. Where only the handle is wanted (Sprite::atlas, an AnimationFrame's `.sheet`), the
// projection is written explicitly — `sheet.atlasId` — so dropping the slots is always visible at the
// call site; nothing converts implicitly.
//
// A pure data type: it depends only on the atlas handle + slot geometry, so a consumer that just needs a
// sheet's layout includes this header alone, without the GPU renderer. Renderer::uploadAtlas / loadAtlas
// (renderer.h) produce one; renderer.h re-exports this type by including this header.
struct AtlasManifest {
    AtlasId                atlasId{};
    std::vector<AssetSlot> slots;
    // >0 only for an AnimationSeries load (the grid holds MULTIPLE animations, this many frames each);
    // 0 = ungrouped (Single / Tileset / SpriteSeries / SingleAnimation). The flat carve is unchanged;
    // this just records how the contiguous slots divide into per-animation runs (animationCount/animation).
    int framesPerAnimation = 0;

    [[nodiscard]] std::size_t      tileCount() const noexcept { return slots.size(); }
    [[nodiscard]] const AssetSlot& operator[](std::size_t i) const { return slots[i]; }

    // AnimationSeries navigation. animationCount() = how many whole per-animation runs the slots hold
    // (slots / framesPerAnimation; 0 when ungrouped or fewer slots than one run). animation(g) = the g-th
    // animation's contiguous run of framesPerAnimation slots, in read order — feed it (with a palette
    // + a duration) straight into an Animation. Throws std::out_of_range if g >= animationCount().
    [[nodiscard]] std::size_t animationCount() const noexcept {
        return framesPerAnimation > 0
                   ? slots.size() / static_cast<std::size_t>(framesPerAnimation)
                   : 0;
    }
    [[nodiscard]] std::span<const AssetSlot> animation(std::size_t g) const {
        if (g >= animationCount()) {
            throw std::out_of_range("AtlasManifest::animation: animation index out of range");
        }
        const std::size_t per = static_cast<std::size_t>(framesPerAnimation);
        return std::span<const AssetSlot>(slots.data() + g * per, per);
    }
};

}  // namespace retropp
