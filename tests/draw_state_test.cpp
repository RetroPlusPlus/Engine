#include "retropp/draw_state.h"

#include <SDL3/SDL.h>
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <vector>

namespace retropp {
namespace {

// ── Compile-time collision detection (the static_assert seam) ─────────────────────────
// layerKeysAreUnique / findLayerKeyCollision stay constexpr. A fresh DrawLayer auto-mints its id (a
// runtime value), so a default-id layer set is no longer constexpr-constructible — but giving each layer
// an explicit id keeps the set constexpr, so the compile-time seam still holds for a fixed stack:
constexpr std::array<DrawLayer, 3> kExplicitIdLayers{{
    DrawLayer{.id = LayerId{1}, .label = "a", .z = 0},
    DrawLayer{.id = LayerId{2}, .label = "b", .z = 10},
    DrawLayer{.id = LayerId{3}, .label = "c", .z = 20},
}};
static_assert(layerKeysAreUnique(std::span<const DrawLayer>{kExplicitIdLayers}),
              "distinct z + label is unique at compile time when ids are explicit");

}  // namespace

// ── Layer-key collision detection (runtime — auto-minted default ids) ──────────────────

TEST(DrawState, LayerKeyUniquenessDetectedForDistinctZAndLabel) {
    const std::array<DrawLayer, 3> distinct{{
        DrawLayer{.label = "a", .z = 0},
        DrawLayer{.label = "b", .z = 10},
        DrawLayer{.label = "c", .z = 20},
    }};
    EXPECT_TRUE(layerKeysAreUnique(std::span<const DrawLayer>{distinct}));
}

TEST(DrawState, DuplicateZAndLabelDetectedWithKind) {
    const std::array<DrawLayer, 2> zCollision{{
        DrawLayer{.label = "a", .z = 5},
        DrawLayer{.label = "b", .z = 5},
    }};
    EXPECT_FALSE(layerKeysAreUnique(std::span<const DrawLayer>{zCollision}));
    EXPECT_EQ(findLayerKeyCollision(std::span<const DrawLayer>{zCollision})->kind,
              LayerKeyCollision::Kind::DuplicateZ);

    const std::array<DrawLayer, 2> labelCollision{{
        DrawLayer{.label = "dup", .z = 0},
        DrawLayer{.label = "dup", .z = 9},
    }};
    EXPECT_FALSE(layerKeysAreUnique(std::span<const DrawLayer>{labelCollision}));
    EXPECT_EQ(findLayerKeyCollision(std::span<const DrawLayer>{labelCollision})->kind,
              LayerKeyCollision::Kind::DuplicateLabel);
}

// ── Envelope defaults ─────────────────────────────────────────────────────────────────

TEST(DrawState, LayerDefaultsAreFaithful) {
    const DrawLayer layer{};
    EXPECT_FLOAT_EQ(layer.alpha, 1.0f);                         // default opaque
    EXPECT_EQ(layer.z, 0);
    EXPECT_EQ(contentKind(layer.content), LayerContentKind::Tiles);  // default content = Tiles
    EXPECT_TRUE(layer.effects.empty());                              // no per-layer effect by default
}

TEST(DrawState, FrameDefaultsAreIdentity) {
    const FrameDrawState frame{};
    EXPECT_EQ(frame.blend, BlendMode::Normal);  // container blend defaults to alpha-over
    EXPECT_TRUE(frame.layers.empty());
    EXPECT_TRUE(frame.postEffects.empty());
    EXPECT_TRUE(frame.regions.empty());
}

TEST(DrawState, ContentKindMirrorsVariant) {
    EXPECT_EQ(contentKind(LayerContent{TileContent{}}), LayerContentKind::Tiles);
    EXPECT_EQ(contentKind(LayerContent{SpriteContent{}}), LayerContentKind::Sprites);
}

// ── Identity handles (LayerId / SpriteId primary keys) ─────────────────────────────────

TEST(DrawState, IdentityHandlesAreDistinctUint32BackedAndDefaultUnstamped) {
    static_assert(std::is_same_v<std::underlying_type_t<LayerId>, std::uint32_t>);
    static_assert(std::is_same_v<std::underlying_type_t<SpriteId>, std::uint32_t>);
    static_assert(!std::is_same_v<LayerId, SpriteId>);                  // distinct handle types
    EXPECT_EQ(static_cast<std::uint32_t>(LayerId{}), 0u);               // 0 = unstamped / none
    EXPECT_EQ(static_cast<std::uint32_t>(SpriteId{}), 0u);
}

TEST(DrawState, FreshLayerAndSpriteAutoMintNonZeroIdsAndCopyPreservesThem) {
    const DrawLayer a{};
    const DrawLayer b{};
    EXPECT_NE(a.id, LayerId{});          // a fresh layer auto-mints a non-zero id
    EXPECT_NE(b.id, LayerId{});
    EXPECT_NE(a.id, b.id);               // two fresh layers get distinct ids
    EXPECT_TRUE(a.label.empty());        // no developer name by default

    const DrawLayer copy = a;            // copy preserves the id (the match key across frames)
    EXPECT_EQ(copy.id, a.id);

    const Sprite s1{};
    const Sprite s2{};
    EXPECT_NE(s1.id, SpriteId{});
    EXPECT_NE(s1.id, s2.id);
    const Sprite scopy = s1;
    EXPECT_EQ(scopy.id, s1.id);

    const Region r1{};
    const Region r2{};
    EXPECT_NE(r1.id, RegionId{});        // a region carries identity too (it can move / animate)
    EXPECT_NE(r1.id, r2.id);
    const Region rcopy = r1;
    EXPECT_EQ(rcopy.id, r1.id);

    // The ids come from one shared counter, so every object across the three types is globally unique.
    EXPECT_NE(static_cast<std::uint32_t>(a.id), static_cast<std::uint32_t>(s1.id));
    EXPECT_NE(static_cast<std::uint32_t>(s1.id), static_cast<std::uint32_t>(r1.id));
}

TEST(DrawState, FindCollisionReturnsNulloptForUniqueLabelsAndZ) {
    const std::array<DrawLayer, 3> layers{{
        DrawLayer{.label = "a", .z = 0},
        DrawLayer{.label = "b", .z = 1},
        DrawLayer{.label = "c", .z = 2},
    }};
    EXPECT_FALSE(findLayerKeyCollision(std::span<const DrawLayer>{layers}).has_value());
}

TEST(DrawState, DuplicateLabelMessageNamesTheCollidingLabel) {
    const std::vector<DrawLayer> layers{
        DrawLayer{.label = "duplicatedName", .z = 0},
        DrawLayer{.label = "duplicatedName", .z = 9},
    };
    try {
        (void)layerDrawOrder(layers, LayerKeyCollisionPolicy::Throw);
        FAIL() << "expected a collision throw";
    } catch (const std::invalid_argument& e) {
        const std::string_view msg{e.what()};
        EXPECT_NE(msg.find("duplicatedName"), std::string_view::npos);  // the colliding label
        EXPECT_NE(msg.find("label"), std::string_view::npos);           // names the violated key
    }
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
        DrawLayer{.label = "a", .z = 20},
        DrawLayer{.label = "b", .z = 0},
        DrawLayer{.label = "c", .z = 10},
    };
    const auto order = layerDrawOrder(layers, LayerKeyCollisionPolicy::Throw);
    ASSERT_EQ(order.size(), 3u);
    EXPECT_EQ(order[0], 1u);  // z=0
    EXPECT_EQ(order[1], 2u);  // z=10
    EXPECT_EQ(order[2], 0u);  // z=20
}

TEST(DrawState, DrawOrderAlreadySortedAndReversed) {
    const std::vector<DrawLayer> sorted{
        DrawLayer{.label = "a", .z = 0},
        DrawLayer{.label = "b", .z = 1},
        DrawLayer{.label = "c", .z = 2},
    };
    EXPECT_EQ(layerDrawOrder(sorted, LayerKeyCollisionPolicy::Throw),
              (std::vector<std::size_t>{0, 1, 2}));

    const std::vector<DrawLayer> reversed{
        DrawLayer{.label = "a", .z = 2},
        DrawLayer{.label = "b", .z = 1},
        DrawLayer{.label = "c", .z = 0},
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
        DrawLayer{.label = "a", .z = 5},
        DrawLayer{.label = "b", .z = 5},
    };
    EXPECT_THROW((void)layerDrawOrder(layers, LayerKeyCollisionPolicy::Throw), std::invalid_argument);
}

TEST(DrawState, DuplicateLabelThrowsUnderThrowPolicy) {
    const std::vector<DrawLayer> layers{
        DrawLayer{.label = "dup", .z = 0},
        DrawLayer{.label = "dup", .z = 9},
    };
    EXPECT_THROW((void)layerDrawOrder(layers, LayerKeyCollisionPolicy::Throw), std::invalid_argument);
}

// ── Collision policy: WarnAndResolve (a shipped game stays up, order stays deterministic) ─

TEST(DrawState, DuplicateZResolvesDeterministicallyUnderWarnPolicy) {
    // Suppress the expected warning so it doesn't clutter test output.
    SDL_SetLogPriority(SDL_LOG_CATEGORY_RENDER, SDL_LOG_PRIORITY_CRITICAL);
    const std::vector<DrawLayer> layers{
        DrawLayer{.label = "spriteA",    .z = 5},
        DrawLayer{.label = "spriteB",    .z = 5},
        DrawLayer{.label = "background", .z = 0},
    };
    std::vector<std::size_t> order;
    EXPECT_NO_THROW(order = layerDrawOrder(layers, LayerKeyCollisionPolicy::WarnAndResolve));
    ASSERT_EQ(order.size(), 3u);
    EXPECT_EQ(order[0], 2u);  // z=0 background → back
    EXPECT_EQ(order[1], 0u);  // z=5 tie resolved by SUBMISSION order → spriteA (index 0)
    EXPECT_EQ(order[2], 1u);  // z=5 → spriteB (index 1)
}

TEST(DrawState, DuplicateLabelDoesNotThrowUnderWarnPolicy) {
    SDL_SetLogPriority(SDL_LOG_CATEGORY_RENDER, SDL_LOG_PRIORITY_CRITICAL);
    const std::vector<DrawLayer> layers{
        DrawLayer{.label = "dup", .z = 0},
        DrawLayer{.label = "dup", .z = 9},
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

}  // namespace retropp
