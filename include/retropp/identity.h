#pragma once

#include <cstdint>

namespace retropp {

// LayerId / SpriteId / RegionId are the typed identity handles for a placed layer / sprite / region.
// Their full definitions live in draw_state.h; these opaque-enum declarations let the mint functions name
// them without a circular include — draw_state.h includes THIS header to seed the id default member
// initializers, and identity.cpp includes draw_state.h for the complete types.
enum class LayerId : std::uint32_t;
enum class SpriteId : std::uint32_t;
enum class RegionId : std::uint32_t;

// Assign a fresh identity to a newly constructed DrawLayer / Sprite / Region. Each draws the next value
// from a single monotonic counter (starts at 1; 0 is the reserved "none"), so a fresh construction gets a
// unique id and a copy preserves it (the implicit copy just copies the field — it does not re-mint). The
// counter is the one piece of engine-global state identity needs: an id faucet, not per-object draw state.
// The renderer matches an object to its previous tick state by this id to interpolate between sim states.
[[nodiscard]] LayerId  mintLayerId() noexcept;
[[nodiscard]] SpriteId mintSpriteId() noexcept;
[[nodiscard]] RegionId mintRegionId() noexcept;

}  // namespace retropp
