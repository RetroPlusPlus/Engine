#include "gbcpp/draw_state.h"

#include <SDL3/SDL.h>
#include <gtest/gtest.h>

#include <array>
#include <span>
#include <stdexcept>
#include <vector>

namespace gbcpp {
namespace {

// ── Compile-time collision detection (the static_assert seam) ─────────────────────────
// These ARE the build-time proof: a fixed layer stack with a z or id collision fails to
// compile. Negating them (removing a !, or colliding the "distinct" set) turns the build red.

constexpr std::array<DrawLayer, 3> kDistinctLayers{{
    DrawLayer{.id = LayerId{0}, .z = 0},
    DrawLayer{.id = LayerId{1}, .z = 10},
    DrawLayer{.id = LayerId{2}, .z = 20},
}};
static_assert(layerKeysAreUnique(std::span<const DrawLayer>{kDistinctLayers}),
              "distinct z + id must be reported unique at compile time");

constexpr std::array<DrawLayer, 2> kZCollision{{
    DrawLayer{.id = LayerId{0}, .z = 5},
    DrawLayer{.id = LayerId{1}, .z = 5},
}};
static_assert(!layerKeysAreUnique(std::span<const DrawLayer>{kZCollision}),
              "duplicate z must be detected at compile time");

constexpr std::array<DrawLayer, 2> kIdCollision{{
    DrawLayer{.id = LayerId{7}, .z = 0},
    DrawLayer{.id = LayerId{7}, .z = 9},
}};
static_assert(!layerKeysAreUnique(std::span<const DrawLayer>{kIdCollision}),
              "duplicate id must be detected at compile time");

// findLayerKeyCollision reports the kind, constexpr.
static_assert(findLayerKeyCollision(std::span<const DrawLayer>{kZCollision})->kind ==
              LayerKeyCollision::Kind::DuplicateZ);
static_assert(findLayerKeyCollision(std::span<const DrawLayer>{kIdCollision})->kind ==
              LayerKeyCollision::Kind::DuplicateId);

}  // namespace

// ── Envelope defaults (faithful baseline) ─────────────────────────────────────────────

TEST(DrawState, LayerDefaultsAreFaithful) {
    const DrawLayer layer{};
    EXPECT_FLOAT_EQ(layer.alpha, 1.0f);                         // default opaque
    EXPECT_EQ(layer.z, 0);
    EXPECT_EQ(contentKind(layer.content), LayerContentKind::Tiles);  // default content = Tiles
    EXPECT_EQ(layer.effect.kind, ScreenSpaceEffectKind::None);
}

TEST(DrawState, FrameModifierDefaultsAreIdentity) {
    const FrameDrawState frame{};
    EXPECT_EQ(frame.globalModifier.kind, ColorModifierKind::None);
    EXPECT_FLOAT_EQ(frame.globalModifier.mulR, 1.0f);
    EXPECT_FLOAT_EQ(frame.globalModifier.addR, 0.0f);
    EXPECT_EQ(frame.blend.kind, BlendKind::None);
    EXPECT_FLOAT_EQ(frame.blend.strength, 0.0f);
    EXPECT_TRUE(frame.layers.empty());
    EXPECT_TRUE(frame.postEffects.empty());
}

TEST(DrawState, ContentKindMirrorsVariant) {
    EXPECT_EQ(contentKind(LayerContent{TileContent{}}), LayerContentKind::Tiles);
    EXPECT_EQ(contentKind(LayerContent{SpriteContent{}}), LayerContentKind::Sprites);
}

// ── clampAlpha ────────────────────────────────────────────────────────────────────────

TEST(DrawState, ClampAlpha) {
    EXPECT_FLOAT_EQ(clampAlpha(-0.5f), 0.0f);
    EXPECT_FLOAT_EQ(clampAlpha(0.0f), 0.0f);
    EXPECT_FLOAT_EQ(clampAlpha(0.25f), 0.25f);
    EXPECT_FLOAT_EQ(clampAlpha(1.0f), 1.0f);
    EXPECT_FLOAT_EQ(clampAlpha(2.0f), 1.0f);
    static_assert(clampAlpha(-1.0f) == 0.0f && clampAlpha(5.0f) == 1.0f);
}

// ── Draw order (distinct z — no collision) ────────────────────────────────────────────

TEST(DrawState, DrawOrderAscendingByZ) {
    const std::vector<DrawLayer> layers{
        DrawLayer{.id = LayerId{0}, .z = 20},
        DrawLayer{.id = LayerId{1}, .z = 0},
        DrawLayer{.id = LayerId{2}, .z = 10},
    };
    const auto order = layerDrawOrder(layers, LayerKeyCollisionPolicy::Throw);
    ASSERT_EQ(order.size(), 3u);
    EXPECT_EQ(order[0], 1u);  // z=0
    EXPECT_EQ(order[1], 2u);  // z=10
    EXPECT_EQ(order[2], 0u);  // z=20
}

TEST(DrawState, DrawOrderAlreadySortedAndReversed) {
    const std::vector<DrawLayer> sorted{
        DrawLayer{.id = LayerId{0}, .z = 0},
        DrawLayer{.id = LayerId{1}, .z = 1},
        DrawLayer{.id = LayerId{2}, .z = 2},
    };
    EXPECT_EQ(layerDrawOrder(sorted, LayerKeyCollisionPolicy::Throw),
              (std::vector<std::size_t>{0, 1, 2}));

    const std::vector<DrawLayer> reversed{
        DrawLayer{.id = LayerId{0}, .z = 2},
        DrawLayer{.id = LayerId{1}, .z = 1},
        DrawLayer{.id = LayerId{2}, .z = 0},
    };
    EXPECT_EQ(layerDrawOrder(reversed, LayerKeyCollisionPolicy::Throw),
              (std::vector<std::size_t>{2, 1, 0}));
}

TEST(DrawState, DrawOrderEmptyIsEmpty) {
    const std::vector<DrawLayer> none;
    EXPECT_TRUE(layerDrawOrder(none, LayerKeyCollisionPolicy::Throw).empty());
}

// ── Collision policy: Throw ───────────────────────────────────────────────────────────

TEST(DrawState, DuplicateZThrowsUnderThrowPolicy) {
    const std::vector<DrawLayer> layers{
        DrawLayer{.id = LayerId{0}, .z = 5},
        DrawLayer{.id = LayerId{1}, .z = 5},
    };
    EXPECT_THROW((void)layerDrawOrder(layers, LayerKeyCollisionPolicy::Throw), std::invalid_argument);
}

TEST(DrawState, DuplicateIdThrowsUnderThrowPolicy) {
    const std::vector<DrawLayer> layers{
        DrawLayer{.id = LayerId{7}, .z = 0},
        DrawLayer{.id = LayerId{7}, .z = 9},
    };
    EXPECT_THROW((void)layerDrawOrder(layers, LayerKeyCollisionPolicy::Throw), std::invalid_argument);
}

// ── Collision policy: WarnAndResolve (a shipped game stays up, order stays deterministic) ─

TEST(DrawState, DuplicateZResolvesDeterministicallyUnderWarnPolicy) {
    // Suppress the expected warning so it doesn't clutter test output.
    SDL_SetLogPriority(SDL_LOG_CATEGORY_RENDER, SDL_LOG_PRIORITY_CRITICAL);
    const std::vector<DrawLayer> layers{
        DrawLayer{.id = LayerId{2}, .z = 5},
        DrawLayer{.id = LayerId{1}, .z = 5},
        DrawLayer{.id = LayerId{0}, .z = 0},
    };
    std::vector<std::size_t> order;
    EXPECT_NO_THROW(order = layerDrawOrder(layers, LayerKeyCollisionPolicy::WarnAndResolve));
    ASSERT_EQ(order.size(), 3u);
    EXPECT_EQ(order[0], 2u);  // z=0  (id 0)
    EXPECT_EQ(order[1], 1u);  // z=5, tie resolved by ascending id → id 1 (index 1)
    EXPECT_EQ(order[2], 0u);  // z=5, id 2 (index 0)
}

TEST(DrawState, DuplicateIdDoesNotThrowUnderWarnPolicy) {
    SDL_SetLogPriority(SDL_LOG_CATEGORY_RENDER, SDL_LOG_PRIORITY_CRITICAL);
    const std::vector<DrawLayer> layers{
        DrawLayer{.id = LayerId{7}, .z = 0},
        DrawLayer{.id = LayerId{7}, .z = 9},
    };
    EXPECT_NO_THROW((void)layerDrawOrder(layers, LayerKeyCollisionPolicy::WarnAndResolve));
}

// ── Default policy is build-config-derived (the toggle's sane default) ─────────────────

TEST(DrawState, DefaultCollisionPolicyMatchesBuildConfig) {
#ifdef NDEBUG
    EXPECT_EQ(kDefaultCollisionPolicy, LayerKeyCollisionPolicy::WarnAndResolve);
#else
    EXPECT_EQ(kDefaultCollisionPolicy, LayerKeyCollisionPolicy::Throw);
#endif
}

}  // namespace gbcpp
