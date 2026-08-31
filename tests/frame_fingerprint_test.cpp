// Frame-level compose-skip fingerprint + settledness — device-free.
//
// hashFrameStructure(FrameDrawState) folds every compose-consumed field of a submission into one 64-bit
// value; renderFrame re-blits its retained output instead of recomposing when a settled frame's fingerprint
// matches the last composed one. These tests pin the two properties the skip's correctness rests on:
//   1. The fingerprint is sensitive to every enumerated input class — mutating any field the compose reads
//      changes the hash, so an equal fingerprint means an equal composed image. A missed class here would be
//      a wrong skip (a stale frame re-blitted over a changed one).
//   2. Interpolator::allSettled() reports whether any object is mid-ease — the gate that excludes frame timing
//      from the fingerprint (a settled frame composes independently of alpha).
// Both are pure CPU; no GPU device is constructed.

#include <cstdint>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "retropp/draw_state.h"
#include "retropp/interpolation.h"
#include "steady_timing.h"
#include "retropp/renderer.h"

namespace {
using namespace retropp;

// Backing storage the frame's spans point into — must outlive every hashFrameStructure call that reads it.
struct Backing {
    std::vector<TileCell>          cells;
    std::vector<Sprite>            sprites;
    std::vector<Vec4>              paramTable;
    std::vector<Point>            shapePoints;
};

// A representative frame exercising every hashed input class: a tile layer (with cells + a layer effect
// carrying a paramTable), a sprite layer (a sprite carrying its own effect + a region), and frame-level
// postEffects + a frame region. Rebuilt fresh so each test can mutate one field of a copy.
FrameDrawState makeFrame(Backing& b) {
    b.cells.assign(4 * 4, TileCell{.atlas = AtlasId{1}, .tile = 2, .palette = PaletteId{1}});
    b.cells[5] = TileCell{.atlas = AtlasId{1}, .tile = 7, .palette = PaletteId{2}, .flipX = true};
    b.paramTable = {Vec4{1, 2, 3, 4}, Vec4{5, 6, 7, 8}};
    b.shapePoints = {Point{2, 2}, Point{30, 4}, Point{16, 28}};

    DrawLayer tiles{.key = ObjectKey("bg")};
    tiles.z       = 0;
    tiles.size    = PixelSize{64, 64};
    tiles.scroll  = LayerScroll{3, 5};
    tiles.alpha   = 1.0f;
    tiles.blend   = BlendMode::Normal;
    tiles.content = TileContent{.widthInTiles = 4, .heightInTiles = 4,
                                .cells = std::span<const TileCell>(b.cells), .wrap = TileWrap::Repeat};
    tiles.effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::RowDisplacement,
                                       .amplitude = 2.0f, .frequency = 1.5f,
                                       .paramTable = std::span<const Vec4>(b.paramTable)}};

    b.sprites = {Sprite{.key = ObjectKey("hero"), .x = 12, .y = 20, .atlas = AtlasId{1}, .tile = 3,
                        .palette = PaletteId{1}}};
    b.sprites[0].effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::Gleam, .sweep = 0.4f, .gain = 0.5f}};
    b.sprites[0].regions = {Region{.key = ObjectKey("flash"),
                                   .shape = ShapePoints{.points = {Point{1, 1}, Point{6, 1}, Point{6, 6}},
                                                        .strokeWidth = 1.5f},
                                   .effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill,
                                                                 .fill = Rgba8{255, 0, 0}}}}};
    DrawLayer sprites{.key = ObjectKey("actors")};
    sprites.z       = 10;
    sprites.size    = PixelSize{64, 64};
    sprites.content = SpriteContent{.sprites = std::span<const Sprite>(b.sprites)};

    FrameDrawState frame;
    frame.layers      = {tiles, sprites};
    frame.blend       = BlendMode::Normal;
    frame.postEffects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorSaturation, .saturation = 128}};
    frame.regions     = {Region{.key = ObjectKey("vignette"),
                                .shape = ShapePoints{.points = std::vector<Point>(b.shapePoints)},
                                .effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill,
                                                              .fill = Rgba8{0, 0, 0}, .fillIntensity = 0.5f}},
                                .blend = BlendMode::Multiply}};
    return frame;
}

// ── The fingerprint is not degenerate and is deterministic ────────────────────────────────────────────

TEST(FrameFingerprint, IdenticalFramesHashEqual) {
    Backing a, b;
    EXPECT_EQ(hashFrameStructure(makeFrame(a)), hashFrameStructure(makeFrame(b)));
}

TEST(FrameFingerprint, EmptyFrameHashesStablyAndDiffersFromPopulated) {
    const FrameDrawState empty;
    EXPECT_EQ(hashFrameStructure(empty), hashFrameStructure(FrameDrawState{}));
    Backing b;
    EXPECT_NE(hashFrameStructure(empty), hashFrameStructure(makeFrame(b)));
}

// ── Sensitivity: mutating each enumerated input class changes the hash ─────────────────────────────────
// Each case rebuilds the baseline (its own backing) so the mutation is the only difference.

TEST(FrameFingerprint, LayerScalarFieldsAreHashed) {
    Backing base; const std::uint64_t h0 = hashFrameStructure(makeFrame(base));

    { Backing b; FrameDrawState f = makeFrame(b); f.layers[0].scroll = LayerScroll{4, 5};
      EXPECT_NE(h0, hashFrameStructure(f)) << "layer scroll"; }
    { Backing b; FrameDrawState f = makeFrame(b); f.layers[0].z = 1;
      EXPECT_NE(h0, hashFrameStructure(f)) << "layer z"; }
    { Backing b; FrameDrawState f = makeFrame(b); f.layers[0].alpha = 0.5f;
      EXPECT_NE(h0, hashFrameStructure(f)) << "layer alpha"; }
    { Backing b; FrameDrawState f = makeFrame(b); f.layers[0].blend = BlendMode::Add;
      EXPECT_NE(h0, hashFrameStructure(f)) << "layer blend"; }
    { Backing b; FrameDrawState f = makeFrame(b); f.layers[0].size = PixelSize{32, 64};
      EXPECT_NE(h0, hashFrameStructure(f)) << "layer size"; }
    { Backing b; FrameDrawState f = makeFrame(b); f.layers[0].transform = Transform::scale(2.0f, 2.0f);
      EXPECT_NE(h0, hashFrameStructure(f)) << "layer transform"; }
    { Backing b; FrameDrawState f = makeFrame(b); f.layers[0].transformEdge = DisplacementEdge::Stretch;
      EXPECT_NE(h0, hashFrameStructure(f)) << "layer transformEdge"; }
    { Backing b; FrameDrawState f = makeFrame(b); f.layers[0].key = ObjectKey("bg2");
      EXPECT_NE(h0, hashFrameStructure(f)) << "layer key"; }
    { Backing b; FrameDrawState f = makeFrame(b); f.layers[0].advancesEvery = 2;
      EXPECT_NE(h0, hashFrameStructure(f)) << "layer advancesEvery"; }
    { Backing b; FrameDrawState f = makeFrame(b); f.layers[0].advancesEvery = 1;
      EXPECT_NE(h0, hashFrameStructure(f)) << "layer advancesEvery declared 1 vs unset"; }
}

// A declaration changes how the frame's motion is eased, so a frame carrying a different one is not the
// frame the retained output was composed from.
TEST(FrameFingerprint, TheDeclaredCadenceIsHashed) {
    Backing base; const std::uint64_t h0 = hashFrameStructure(makeFrame(base));
    { Backing b; FrameDrawState f = makeFrame(b); f.advancesEvery = 2;
      EXPECT_NE(h0, hashFrameStructure(f)) << "frame advancesEvery"; }
}

TEST(FrameFingerprint, TileContentIsHashed) {
    Backing base; const std::uint64_t h0 = hashFrameStructure(makeFrame(base));

    { Backing b; FrameDrawState f = makeFrame(b); b.cells[0].tile = 9;
      EXPECT_NE(h0, hashFrameStructure(f)) << "tile cell"; }
    { Backing b; FrameDrawState f = makeFrame(b);
      std::get<TileContent>(f.layers[0].content).wrap = TileWrap::Blank;
      EXPECT_NE(h0, hashFrameStructure(f)) << "tile wrap"; }
}

// The huge-map opt-out: a declared layer contributes only its declaration, so unset / false / true each hash
// distinctly, and the cells are NOT read (a false-declared layer with mutated cells hashes the same as one
// with the original cells — the renderer's declared-dirty gate, not the fingerprint, catches a true).
TEST(FrameFingerprint, ContentChangedDeclarationIsHashedNotCells) {
    Backing base; const std::uint64_t hUnset = hashFrameStructure(makeFrame(base));

    Backing bf; FrameDrawState ff = makeFrame(bf);
    std::get<TileContent>(ff.layers[0].content).contentChanged = false;
    const std::uint64_t hFalse = hashFrameStructure(ff);

    Backing bt; FrameDrawState ft = makeFrame(bt);
    std::get<TileContent>(ft.layers[0].content).contentChanged = true;
    const std::uint64_t hTrue = hashFrameStructure(ft);

    EXPECT_NE(hUnset, hFalse) << "unset vs declared-unchanged";
    EXPECT_NE(hFalse, hTrue) << "declared-unchanged vs declared-changed";

    // A false-declared layer does not read cells — mutating them leaves the fingerprint unchanged.
    Backing bf2; FrameDrawState ff2 = makeFrame(bf2);
    std::get<TileContent>(ff2.layers[0].content).contentChanged = false;
    bf2.cells[0].tile = 99;
    EXPECT_EQ(hFalse, hashFrameStructure(ff2)) << "declared-false ignores cell mutation";
}

TEST(FrameFingerprint, SpriteFieldsAreHashed) {
    Backing base; const std::uint64_t h0 = hashFrameStructure(makeFrame(base));

    { Backing b; FrameDrawState f = makeFrame(b); b.sprites[0].x = 13;
      EXPECT_NE(h0, hashFrameStructure(f)) << "sprite x"; }
    { Backing b; FrameDrawState f = makeFrame(b); b.sprites[0].tile = 4;
      EXPECT_NE(h0, hashFrameStructure(f)) << "sprite tile"; }
    { Backing b; FrameDrawState f = makeFrame(b); b.sprites[0].palette = PaletteId{2};
      EXPECT_NE(h0, hashFrameStructure(f)) << "sprite palette"; }
    { Backing b; FrameDrawState f = makeFrame(b); b.sprites[0].alpha = 0.25f;
      EXPECT_NE(h0, hashFrameStructure(f)) << "sprite alpha"; }
    { Backing b; FrameDrawState f = makeFrame(b); b.sprites[0].blend = BlendMode::Screen;
      EXPECT_NE(h0, hashFrameStructure(f)) << "sprite blend"; }
    { Backing b; FrameDrawState f = makeFrame(b); b.sprites[0].pivot = Point{4, 4};
      EXPECT_NE(h0, hashFrameStructure(f)) << "sprite pivot"; }
    { Backing b; FrameDrawState f = makeFrame(b); b.sprites[0].key = ObjectKey("hero2");
      EXPECT_NE(h0, hashFrameStructure(f)) << "sprite key"; }
}

TEST(FrameFingerprint, EffectAndRegionParamsAreHashed) {
    Backing base; const std::uint64_t h0 = hashFrameStructure(makeFrame(base));

    { Backing b; FrameDrawState f = makeFrame(b); f.layers[0].effects[0].amplitude = 3.0f;
      EXPECT_NE(h0, hashFrameStructure(f)) << "layer effect param"; }
    { Backing b; FrameDrawState f = makeFrame(b); b.paramTable[1].z = 99.0f;
      EXPECT_NE(h0, hashFrameStructure(f)) << "paramTable cell"; }
    { Backing b; FrameDrawState f = makeFrame(b); b.sprites[0].effects[0].gain = 0.9f;
      EXPECT_NE(h0, hashFrameStructure(f)) << "sprite effect param"; }
    { Backing b; FrameDrawState f = makeFrame(b); b.sprites[0].regions[0].shape.strokeWidth = 3.0f;
      EXPECT_NE(h0, hashFrameStructure(f)) << "region strokeWidth"; }
    { Backing b; FrameDrawState f = makeFrame(b); b.sprites[0].regions[0].blend = BlendMode::Add;
      EXPECT_NE(h0, hashFrameStructure(f)) << "region blend"; }
    { Backing b; FrameDrawState f = makeFrame(b);
      b.sprites[0].regions[0].effects[0].fill = Rgba8{0, 255, 0};
      EXPECT_NE(h0, hashFrameStructure(f)) << "region effect fill"; }
    // The Bloom knobs are hashed individually — a glow that widens, re-floors, or brightens under a
    // settled frame must recompose, never re-blit stale.
    { Backing b; FrameDrawState f = makeFrame(b); f.postEffects[0].radius = 4.0f;
      EXPECT_NE(h0, hashFrameStructure(f)) << "bloom radius"; }
    { Backing b; FrameDrawState f = makeFrame(b); f.postEffects[0].threshold = 100;
      EXPECT_NE(h0, hashFrameStructure(f)) << "bloom threshold"; }
    { Backing b; FrameDrawState f = makeFrame(b); f.postEffects[0].intensity = 200;
      EXPECT_NE(h0, hashFrameStructure(f)) << "bloom intensity"; }
}

TEST(FrameFingerprint, FrameLevelFieldsAreHashed) {
    Backing base; const std::uint64_t h0 = hashFrameStructure(makeFrame(base));

    { Backing b; FrameDrawState f = makeFrame(b); f.blend = BlendMode::Multiply;
      EXPECT_NE(h0, hashFrameStructure(f)) << "frame blend"; }
    { Backing b; FrameDrawState f = makeFrame(b); f.postEffects[0].saturation = 64;
      EXPECT_NE(h0, hashFrameStructure(f)) << "postEffect param"; }
    { Backing b; FrameDrawState f = makeFrame(b); f.regions[0].shape.points[0].x = 5.0f;
      EXPECT_NE(h0, hashFrameStructure(f)) << "frame region point"; }
    { Backing b; FrameDrawState f = makeFrame(b); f.regions[0].alpha = 0.3f;
      EXPECT_NE(h0, hashFrameStructure(f)) << "frame region alpha"; }
}

// A Custom effect's reflected params live in the generated union — hashEffect must cover them too, or a
// custom-shader param change would be a silent wrong skip.
TEST(FrameFingerprint, CustomEffectGeneratedFieldsAreHashed) {
    Backing b0; FrameDrawState f0 = makeFrame(b0);
    f0.postEffects[0] = ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::Custom,
                                          .customShader = static_cast<PostProcessStageId>(1)};
    const std::uint64_t h0 = hashFrameStructure(f0);

    Backing b1; FrameDrawState f1 = makeFrame(b1);
    f1.postEffects[0] = ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::Custom,
                                          .customShader = static_cast<PostProcessStageId>(1)};
    f1.postEffects[0].strength = 0.75f;  // a generated custom-shader field
    EXPECT_NE(h0, hashFrameStructure(f1)) << "generated custom field";
}

// Layer order is significant — the compositor z-sorts, and two frames that differ only in submission order of
// same-z-distinct layers compose differently. Swapping the two layers changes the hash.
TEST(FrameFingerprint, LayerOrderIsSignificant) {
    Backing base; const std::uint64_t h0 = hashFrameStructure(makeFrame(base));
    Backing b; FrameDrawState f = makeFrame(b);
    std::swap(f.layers[0], f.layers[1]);
    EXPECT_NE(h0, hashFrameStructure(f));
}

// ── Interpolator::allSettled() — the skip's settledness gate ───────────────────────────────────────────

// A layer key'd frame at one position; reconciling it twice at different positions moves a slot off settled,
// and a third equal tick settles it again.
FrameDrawState spriteFrameAt(std::vector<Sprite>& backing, int x) {
    backing = {Sprite{.key = ObjectKey("s"), .x = x, .y = 0, .atlas = AtlasId{1}, .palette = PaletteId{1}}};
    DrawLayer layer{.key = ObjectKey("L")};
    layer.content = SpriteContent{.sprites = std::span<const Sprite>(backing)};
    FrameDrawState f;
    f.layers = {layer};
    return f;
}

TEST(InterpolatorSettled, EmptyMirrorIsSettled) {
    Interpolator interp;
    EXPECT_TRUE(interp.allSettled());
}

TEST(InterpolatorSettled, MountIsSettledMotionIsNotThenResettles) {
    Interpolator interp;
    std::vector<Sprite> backing;

    interp.reconcile(spriteFrameAt(backing, 0), tickAt(0.0f));   // mount: prev == cur
    EXPECT_TRUE(interp.allSettled()) << "a freshly mounted slot is settled";

    interp.reconcile(spriteFrameAt(backing, 10), tickAt(0.0f));  // moved: prev 0, cur 10
    EXPECT_FALSE(interp.allSettled()) << "a moving slot is not settled";

    interp.reconcile(spriteFrameAt(backing, 10), tickAt(0.0f));  // equal tick: prev 10, cur 10
    EXPECT_TRUE(interp.allSettled()) << "settles after an equal tick";
}

}  // namespace
