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
// layerKeysAreUnique / findLayerKeyCollision stay constexpr. Every layer carries a required key, so a
// fixed layer set is constexpr-constructible and the compile-time seam holds for a fixed stack:
constexpr std::array<DrawLayer, 3> kFixedLayers{{
    DrawLayer{.key = "a", .z = 0},
    DrawLayer{.key = "b", .z = 10},
    DrawLayer{.key = "c", .z = 20},
}};
static_assert(layerKeysAreUnique(std::span<const DrawLayer>{kFixedLayers}),
              "distinct z + key is unique at compile time");

}  // namespace

// ── Layer-key collision detection (runtime — auto-minted default ids) ──────────────────

TEST(DrawState, LayerKeyUniquenessDetectedForDistinctZAndLabel) {
    const std::array<DrawLayer, 3> distinct{{
        DrawLayer{.key = "a", .z = 0},
        DrawLayer{.key = "b", .z = 10},
        DrawLayer{.key = "c", .z = 20},
    }};
    EXPECT_TRUE(layerKeysAreUnique(std::span<const DrawLayer>{distinct}));
}

TEST(DrawState, DuplicateZAndKeyDetectedWithKind) {
    const std::array<DrawLayer, 2> zCollision{{
        DrawLayer{.key = "a", .z = 5},
        DrawLayer{.key = "b", .z = 5},
    }};
    EXPECT_FALSE(layerKeysAreUnique(std::span<const DrawLayer>{zCollision}));
    EXPECT_EQ(findLayerKeyCollision(std::span<const DrawLayer>{zCollision})->kind,
              LayerKeyCollision::Kind::DuplicateZ);

    const std::array<DrawLayer, 2> keyCollision{{
        DrawLayer{.key = "dup", .z = 0},
        DrawLayer{.key = "dup", .z = 9},
    }};
    EXPECT_FALSE(layerKeysAreUnique(std::span<const DrawLayer>{keyCollision}));
    EXPECT_EQ(findLayerKeyCollision(std::span<const DrawLayer>{keyCollision})->kind,
              LayerKeyCollision::Kind::DuplicateKey);
}

// ── Envelope defaults ─────────────────────────────────────────────────────────────────

TEST(DrawState, LayerDefaultsAreFaithful) {
    const DrawLayer layer{.key = "layer"};
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

// ── Reconciliation key (ObjectKey) ─────────────────────────────────────────────────────

TEST(DrawState, ObjectKeyConstructsFromStringsAndComparesByValue) {
    constexpr ObjectKey fromLiteral = "ball";
    const ObjectKey     fromView    = std::string_view{"ball"};
    const std::string   owned       = "ball";
    const ObjectKey     fromString  = owned;                     // runtime std::string ctor
    EXPECT_EQ(std::string_view{fromLiteral}, "ball");            // implicit ObjectKey -> string_view
    EXPECT_EQ(fromLiteral, fromView);                            // value equality
    EXPECT_EQ(fromLiteral, fromString);
    EXPECT_NE(fromLiteral, ObjectKey{"paddle"});
    // ObjectKey has NO default constructor: a DrawLayer / Sprite / Region that omits `.key` is a COMPILE
    // error (a required identity) — every construction in this suite names a key by necessity.
    static_assert(!std::is_default_constructible_v<ObjectKey>);
}

// ── Sprite-key uniqueness (frame-wide — one interpolation slot per key) ─────────────────

TEST(DrawState, SpriteKeyCollisionDetectsDuplicateAcrossLayersAndEmpty) {
    const std::array<Sprite, 1> layerA{{Sprite{.key = "dup"}}};
    const std::array<Sprite, 1> layerB{{Sprite{.key = "dup"}}};   // same key on a DIFFERENT layer
    const std::vector<DrawLayer> frame{
        DrawLayer{.key = "la", .z = 0, .content = SpriteContent{std::span<const Sprite>(layerA)}},
        DrawLayer{.key = "lb", .z = 1, .content = SpriteContent{std::span<const Sprite>(layerB)}},
    };
    const auto dup = findSpriteKeyCollision(frame);
    ASSERT_TRUE(dup.has_value());
    EXPECT_EQ(dup->kind, SpriteKeyCollision::Kind::DuplicateKey);

    const std::array<Sprite, 1> empties{{Sprite{.key = ""}}};
    const std::vector<DrawLayer> emptyFrame{
        DrawLayer{.key = "le", .z = 0, .content = SpriteContent{std::span<const Sprite>(empties)}},
    };
    const auto empty = findSpriteKeyCollision(emptyFrame);
    ASSERT_TRUE(empty.has_value());
    EXPECT_EQ(empty->kind, SpriteKeyCollision::Kind::EmptyKey);
}

TEST(DrawState, UniqueSpriteKeysReturnNullopt) {
    const std::array<Sprite, 2> sprites{{Sprite{.key = "ball"}, Sprite{.key = "paddle"}}};
    const std::vector<DrawLayer> frame{
        DrawLayer{.key = "l", .z = 0, .content = SpriteContent{std::span<const Sprite>(sprites)}},
    };
    EXPECT_FALSE(findSpriteKeyCollision(frame).has_value());
}

TEST(DrawState, ValidateSpriteKeysThrowsOnDuplicateUnderThrowPolicy) {
    const std::array<Sprite, 2> sprites{{Sprite{.key = "dup"}, Sprite{.key = "dup"}}};
    const std::vector<DrawLayer> frame{
        DrawLayer{.key = "l", .z = 0, .content = SpriteContent{std::span<const Sprite>(sprites)}},
    };
    EXPECT_THROW(validateSpriteKeys(frame, LayerKeyCollisionPolicy::Throw), std::invalid_argument);
}

TEST(DrawState, FindCollisionReturnsNulloptForUniqueKeysAndZ) {
    const std::array<DrawLayer, 3> layers{{
        DrawLayer{.key = "a", .z = 0},
        DrawLayer{.key = "b", .z = 1},
        DrawLayer{.key = "c", .z = 2},
    }};
    EXPECT_FALSE(findLayerKeyCollision(std::span<const DrawLayer>{layers}).has_value());
}

TEST(DrawState, DuplicateKeyMessageNamesTheCollidingKey) {
    const std::vector<DrawLayer> layers{
        DrawLayer{.key = "duplicatedName", .z = 0},
        DrawLayer{.key = "duplicatedName", .z = 9},
    };
    try {
        (void)layerDrawOrder(layers, LayerKeyCollisionPolicy::Throw);
        FAIL() << "expected a collision throw";
    } catch (const std::invalid_argument& e) {
        const std::string_view msg{e.what()};
        EXPECT_NE(msg.find("duplicatedName"), std::string_view::npos);  // the colliding key
        EXPECT_NE(msg.find("key"), std::string_view::npos);             // names the violated invariant
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
        DrawLayer{.key = "a", .z = 20},
        DrawLayer{.key = "b", .z = 0},
        DrawLayer{.key = "c", .z = 10},
    };
    const auto order = layerDrawOrder(layers, LayerKeyCollisionPolicy::Throw);
    ASSERT_EQ(order.size(), 3u);
    EXPECT_EQ(order[0], 1u);  // z=0
    EXPECT_EQ(order[1], 2u);  // z=10
    EXPECT_EQ(order[2], 0u);  // z=20
}

TEST(DrawState, DrawOrderAlreadySortedAndReversed) {
    const std::vector<DrawLayer> sorted{
        DrawLayer{.key = "a", .z = 0},
        DrawLayer{.key = "b", .z = 1},
        DrawLayer{.key = "c", .z = 2},
    };
    EXPECT_EQ(layerDrawOrder(sorted, LayerKeyCollisionPolicy::Throw),
              (std::vector<std::size_t>{0, 1, 2}));

    const std::vector<DrawLayer> reversed{
        DrawLayer{.key = "a", .z = 2},
        DrawLayer{.key = "b", .z = 1},
        DrawLayer{.key = "c", .z = 0},
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
        DrawLayer{.key = "a", .z = 5},
        DrawLayer{.key = "b", .z = 5},
    };
    EXPECT_THROW((void)layerDrawOrder(layers, LayerKeyCollisionPolicy::Throw), std::invalid_argument);
}

TEST(DrawState, DuplicateKeyThrowsUnderThrowPolicy) {
    const std::vector<DrawLayer> layers{
        DrawLayer{.key = "dup", .z = 0},
        DrawLayer{.key = "dup", .z = 9},
    };
    EXPECT_THROW((void)layerDrawOrder(layers, LayerKeyCollisionPolicy::Throw), std::invalid_argument);
}

// ── Collision policy: WarnAndResolve (a shipped game stays up, order stays deterministic) ─

TEST(DrawState, DuplicateZResolvesDeterministicallyUnderWarnPolicy) {
    // Suppress the expected warning so it doesn't clutter test output.
    SDL_SetLogPriority(SDL_LOG_CATEGORY_RENDER, SDL_LOG_PRIORITY_CRITICAL);
    const std::vector<DrawLayer> layers{
        DrawLayer{.key = "spriteA",    .z = 5},
        DrawLayer{.key = "spriteB",    .z = 5},
        DrawLayer{.key = "background", .z = 0},
    };
    std::vector<std::size_t> order;
    EXPECT_NO_THROW(order = layerDrawOrder(layers, LayerKeyCollisionPolicy::WarnAndResolve));
    ASSERT_EQ(order.size(), 3u);
    EXPECT_EQ(order[0], 2u);  // z=0 background → back
    EXPECT_EQ(order[1], 0u);  // z=5 tie resolved by SUBMISSION order → spriteA (index 0)
    EXPECT_EQ(order[2], 1u);  // z=5 → spriteB (index 1)
}

TEST(DrawState, DuplicateKeyDoesNotThrowUnderWarnPolicy) {
    SDL_SetLogPriority(SDL_LOG_CATEGORY_RENDER, SDL_LOG_PRIORITY_CRITICAL);
    const std::vector<DrawLayer> layers{
        DrawLayer{.key = "dup", .z = 0},
        DrawLayer{.key = "dup", .z = 9},
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
