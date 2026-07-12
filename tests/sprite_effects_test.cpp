// Sprite::effects + Sprite::regions — the carrier surface (built-in colour kinds). A sprite carries an
// effects chain (whole-silhouette colour transforms in list order) and a regions list (confined effects
// graded over the sprite's own pixel by each region's alpha/blend, gated by a quad-space shape ∩ the
// silhouette). Two halves:
//
//   1. Packing (device-free). buildSpriteFxRecords flattens a sprite's effects then its regions into the
//      contiguous SpriteFxRecord run the fragment loops; the fields (kind, isRegion/invert flags, blend,
//      alpha, pointCount, resolved params, quad-space points) are checked directly.
//
//   2. Evaluation (device-backed, via captureViewport). A solid sprite over a known scene carries an
//      effect; each covered pixel is compared against the CPU oracle evalSpriteFxRecords (the exact mirror
//      the fragment reproduces), with the sprite's base colour read from a no-effect capture so
//      palette→float→8-bit rounding cancels. Off-sprite pixels stay byte-identical to the no-sprite scene.
//
// The effect MATH itself (applyColorFill / applyGleam / applyBlendMode / the region SDF) is the device-free
// authority of postprocess.h's own tests; this file verifies it reaches the SPRITE surface inline.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "retropp/draw_state.h"
#include "retropp/palette.h"
#include "retropp/postprocess.h"
#include "retropp/renderer.h"
#include "retropp/viewport.h"

namespace {
using namespace retropp;

// ── Part 1: packing (device-free) ────────────────────────────────────────────────────────────────

TEST(SpriteFxPacking, ChainColorFillIsOneWholeSilhouetteRecord) {
    Sprite s{.key = "s"};
    s.effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill, .fill = Rgba8{255, 128, 0, 255}}};
    const std::vector<SpriteFxRecord> r = buildSpriteFxRecords(s);
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(r[0].kind, static_cast<std::uint32_t>(ScreenSpaceEffectKind::ColorFill));
    EXPECT_EQ(r[0].flags & kSpriteFxIsRegion, 0u);   // a chain step
    EXPECT_EQ(r[0].pointCount, 0u);                  // whole silhouette
    EXPECT_FLOAT_EQ(r[0].alpha, 1.0f);
    EXPECT_NEAR(r[0].params[0], 1.0f, 1e-6f);        // 255/255
    EXPECT_NEAR(r[0].params[1], 128.0f / 255.0f, 1e-6f);
    EXPECT_NEAR(r[0].params[2], 0.0f, 1e-6f);
}

TEST(SpriteFxPacking, RegionCarriesShapeBlendAlphaAndPoints) {
    Sprite s{.key = "s"};
    Region rg{.key = "flash"};
    rg.shape   = ShapePoints::rectangle(Point{1, 2}, 4, 6);  // 4 quad-space vertices
    rg.alpha   = 0.5f;
    rg.blend   = BlendMode::Multiply;
    rg.effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill, .fill = Rgba8{0, 255, 0, 255}}};
    s.regions  = {rg};

    const std::vector<SpriteFxRecord> r = buildSpriteFxRecords(s);
    ASSERT_EQ(r.size(), 1u);
    EXPECT_NE(r[0].flags & kSpriteFxIsRegion, 0u);
    EXPECT_EQ(r[0].flags & kSpriteFxInvert, 0u);
    EXPECT_EQ(r[0].blend, static_cast<std::uint32_t>(BlendMode::Multiply));
    EXPECT_FLOAT_EQ(r[0].alpha, 0.5f);
    EXPECT_EQ(r[0].pointCount, 4u);
    EXPECT_FLOAT_EQ(r[0].points[0], 1.0f);   // p0.x
    EXPECT_FLOAT_EQ(r[0].points[1], 2.0f);   // p0.y
    EXPECT_FLOAT_EQ(r[0].points[2], 5.0f);   // p1.x = 1+4
    EXPECT_FLOAT_EQ(r[0].points[5], 8.0f);   // p2.y = 2+6
    EXPECT_FLOAT_EQ(r[0].radius, 0.0f);
    EXPECT_FLOAT_EQ(r[0].strokeWidth, 0.0f);
}

TEST(SpriteFxPacking, GleamAndTransparencyParamsResolve) {
    Sprite s{.key = "s"};
    s.effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::Gleam, .sweep = 0.3f, .width = 0.2f,
                                   .gain = 1.5f, .slant = 0.4f}};
    Region rg{.key = "hole"};
    rg.shape   = ShapePoints::circle(Point{4, 4}, 3);
    rg.effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::Transparency,
                                    .stencil = StencilMode::TransparentInside, .feather = 2.0f}};
    s.regions = {rg};

    const std::vector<SpriteFxRecord> r = buildSpriteFxRecords(s);
    ASSERT_EQ(r.size(), 2u);
    EXPECT_EQ(r[0].kind, static_cast<std::uint32_t>(ScreenSpaceEffectKind::Gleam));
    EXPECT_FLOAT_EQ(r[0].params[0], 0.3f);
    EXPECT_FLOAT_EQ(r[0].params[1], 0.2f);
    EXPECT_FLOAT_EQ(r[0].params[2], 1.5f);
    EXPECT_FLOAT_EQ(r[0].params[3], 0.4f);
    EXPECT_EQ(r[1].kind, static_cast<std::uint32_t>(ScreenSpaceEffectKind::Transparency));
    EXPECT_EQ(r[1].pointCount, 1u);                  // a circle is one point + radius
    EXPECT_FLOAT_EQ(r[1].radius, 3.0f);
    EXPECT_FLOAT_EQ(r[1].params[0], static_cast<float>(static_cast<int>(StencilMode::TransparentInside)));
    EXPECT_FLOAT_EQ(r[1].params[1], 2.0f);           // feather
}

TEST(SpriteFxPacking, FlattensEffectsThenRegionsInOrder) {
    Sprite s{.key = "s"};
    s.effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::None},                      // dropped
                 ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill, .fill = Rgba8{10, 10, 10, 255}}};
    Region rg{.key = "r"};
    rg.effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill, .fill = Rgba8{20, 20, 20, 255}},
                  ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::Gleam, .gain = 1.0f}};
    s.regions = {rg};

    const std::vector<SpriteFxRecord> r = buildSpriteFxRecords(s);
    ASSERT_EQ(r.size(), 3u);                                  // None dropped; 1 chain + 2 region effects
    EXPECT_EQ(r[0].flags & kSpriteFxIsRegion, 0u);           // the chain ColorFill
    EXPECT_NE(r[1].flags & kSpriteFxIsRegion, 0u);           // region effect 0
    EXPECT_NE(r[2].flags & kSpriteFxIsRegion, 0u);           // region effect 1
    EXPECT_EQ(r[2].kind, static_cast<std::uint32_t>(ScreenSpaceEffectKind::Gleam));
}

TEST(SpriteFxPacking, CurveRegionIsSkippedAndHasEffectsReflectsRealizedContent) {
    Sprite plain{.key = "s"};
    EXPECT_FALSE(spriteHasEffects(plain));

    Sprite s{.key = "s"};
    Region rg{.key = "curved"};
    rg.shape.curve = {CurveSegment{}};  // a curve boundary — unsupported inline on the sprite path
    rg.effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill, .fill = Rgba8{1, 2, 3, 255}}};
    s.regions = {rg};
    EXPECT_TRUE(spriteHasEffects(s));                         // it carries a non-None effect...
    EXPECT_TRUE(buildSpriteFxRecords(s).empty());            // ...but the curve region is skipped
}

TEST(SpriteFxPacking, InvertedRegionSetsTheInvertFlag) {
    Sprite s{.key = "s"};
    Region rg{.key = "r"};
    rg.shape   = ShapePoints::circle(Point{4, 4}, 2).inverted();
    rg.effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill, .fill = Rgba8{9, 9, 9, 255}}};
    s.regions  = {rg};
    const std::vector<SpriteFxRecord> r = buildSpriteFxRecords(s);
    ASSERT_EQ(r.size(), 1u);
    EXPECT_NE(r[0].flags & kSpriteFxInvert, 0u);
}

// ── Part 2: evaluation (device-backed) ─────────────────────────────────────────────────────────

constexpr int kW = 64;
constexpr int kH = 64;
constexpr int kSpriteX = 24;   // 8×8 sprite top-left
constexpr int kSpriteY = 20;

#if defined(_WIN32) && (defined(_M_ARM64) || defined(__aarch64__))
inline constexpr bool kDeviceOptional = true;
#else
inline constexpr bool kDeviceOptional = false;
#endif

Vec4 norm(Rgba8 c) {
    return Vec4{c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, c.a / 255.0f};
}
std::uint8_t quant(float v) { return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); }
bool exactEq(Rgba8 a, Rgba8 b) { return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a; }
int chDelta(Rgba8 a, Rgba8 b) {
    return std::max({std::abs(int(a.r) - int(b.r)), std::abs(int(a.g) - int(b.g)),
                     std::abs(int(a.b) - int(b.b)), std::abs(int(a.a) - int(b.a))});
}

class SpriteEffects : public ::testing::Test {
protected:
    static inline SDL_GPUDevice* device_ = nullptr;
    static inline std::string    initError_;
    static void SetUpTestSuite() {
        if (!SDL_Init(SDL_INIT_VIDEO)) { initError_ = std::string("SDL_Init failed: ") + SDL_GetError(); return; }
        device_ = SDL_CreateGPUDevice(
            SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_MSL, false, nullptr);
        if (!device_) initError_ = std::string("SDL_CreateGPUDevice failed: ") + SDL_GetError();
    }
    static void TearDownTestSuite() {
        if (device_) { SDL_DestroyGPUDevice(device_); device_ = nullptr; }
        SDL_Quit();
    }
    void SetUp() override {
        if (!device_) {
            if (kDeviceOptional)
                GTEST_SKIP() << "Windows on ARM has no production-representative GPU backend in CI; the "
                                "sprite-effect math is the device-free evalSpriteFxRecords mirror, and the "
                                "D3D12 path is covered by Windows x64. (" << initError_ << ")";
            FAIL() << "no GPU device reachable — " << initError_
                   << ". This sprite-effect harness requires a GPU device on every production-representative "
                      "platform; a software rasterizer suffices; on a headless runner set SDL_VIDEODRIVER=offscreen.";
        }
    }
};

struct Art { AtlasId atlas{}; PaletteId palette{}; };

Art uploadBgArt(Renderer& r) {
    std::array<std::uint8_t, 16 * 16> idx{};
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x)
            idx[static_cast<std::size_t>(y) * 16 + x] = static_cast<std::uint8_t>(((x / 4) + (y / 4)) % 4);
    const AtlasId atlas = r.uploadAtlas(idx.data(), 16, 16);
    const std::array<Rgba8, 4> pal{{{20, 20, 30}, {200, 60, 60}, {60, 200, 90}, {230, 230, 240}}};
    return {atlas, r.uploadPalette(std::span<const Rgba8>(pal))};
}
DrawLayer bgLayer(const Art& art, std::vector<TileCell>& keep) {
    keep.resize(8 * 8);
    for (int ty = 0; ty < 8; ++ty)
        for (int tx = 0; tx < 8; ++tx)
            keep[static_cast<std::size_t>(ty) * 8 + tx] =
                TileCell{.atlas = art.atlas, .tile = static_cast<std::uint16_t>((tx + ty) % 4), .palette = art.palette};
    DrawLayer l{.key = "bg"};
    l.z = 0; l.size = PixelSize{kW, kH};
    l.content = TileContent{.widthInTiles = 8, .heightInTiles = 8, .cells = std::span<const TileCell>(keep)};
    return l;
}
Art uploadSolid(Renderer& r, Rgba8 colour) {
    std::array<std::uint8_t, 8 * 8> idx{};
    const AtlasId atlas = r.uploadAtlas(idx.data(), 8, 8);
    const std::array<Rgba8, 4> pal{{colour, colour, colour, colour}};
    return {atlas, r.uploadPalette(std::span<const Rgba8>(pal))};
}

// The whole per-pixel oracle sweep. `configure` sets the sprite's effects/regions. Every covered pixel is
// checked against evalSpriteFxRecords (its base colour from a no-effect capture, its u/v from the pixel);
// a discarded pixel must reveal the background. OFF the sprite the effect frame equals the no-sprite scene.
// Returns the count of covered pixels the effect actually MOVED off the base (so a test can require motion).
int runEffectCell(SDL_GPUDevice* dev, const char* name,
                  const std::function<void(Sprite&)>& configure, int tol = 2) {
    Renderer r{dev, nullptr, ViewportResolution{kW, kH}};
    const Art bg    = uploadBgArt(r);
    const Art solid = uploadSolid(r, Rgba8{110, 140, 190, 255});
    std::vector<TileCell> cB, cN, cG;

    FrameDrawState base;
    base.layers.push_back(bgLayer(bg, cB));
    const std::vector<Rgba8> B = r.captureViewport(base);

    auto scene = [&](bool withEffect, std::vector<TileCell>& cells, std::vector<Sprite>& keepS) {
        FrameDrawState f;
        f.layers.push_back(bgLayer(bg, cells));
        Sprite s{.key = "fx", .x = kSpriteX, .y = kSpriteY, .atlas = solid.atlas, .tile = 0, .palette = solid.palette};
        if (withEffect) configure(s);
        keepS = {s};
        DrawLayer sp{.key = "sprites"};
        sp.z = 10; sp.size = PixelSize{kW, kH};
        sp.content = SpriteContent{.sprites = std::span<const Sprite>(keepS)};
        f.layers.push_back(sp);
        return f;
    };
    std::vector<Sprite> sN, sG;
    FrameDrawState fN = scene(false, cN, sN);
    FrameDrawState fG = scene(true,  cG, sG);
    const std::vector<Rgba8> N = r.captureViewport(fN);   // base sprite colour on coverage
    const std::vector<Rgba8> G = r.captureViewport(fG);   // the effected sprite
    EXPECT_EQ(N.size(), B.size());
    EXPECT_EQ(G.size(), B.size());

    // Records for the oracle — from the same configured sprite.
    Sprite oracleSprite{.key = "fx"};
    configure(oracleSprite);
    const std::vector<SpriteFxRecord> recs = buildSpriteFxRecords(oracleSprite);

    int covered = 0, leaked = 0, moved = 0;
    for (std::size_t i = 0; i < B.size(); ++i) {
        const int px = static_cast<int>(i) % kW, py = static_cast<int>(i) / kW;
        if (!exactEq(N[i], B[i])) {  // the sprite covers here
            ++covered;
            const float u = (static_cast<float>(px) + 0.5f - kSpriteX) / 8.0f;
            const float v = (static_cast<float>(py) + 0.5f - kSpriteY) / 8.0f;
            const std::optional<Vec4> out = evalSpriteFxRecords(norm(N[i]), u, v, 8, 8,
                                                                std::span<const SpriteFxRecord>(recs));
            Rgba8 want;
            if (!out) {
                want = B[i];  // whole-silhouette discard → the background shows through
            } else {
                // The sprite composites over the background by its final alpha (layer/sprite alpha are 1 here);
                // a Transparency effect that lowers alpha reveals the opaque background beneath, exactly as the
                // fragment's premultiplied blend does. alpha 1 (the colour-only effects) leaves want == the fill.
                const Vec4 over = alphaOver(norm(B[i]), *out);
                want = Rgba8{quant(over.x), quant(over.y), quant(over.z), quant(over.w)};
            }
            if (chDelta(G[i], want) > tol) {
                ADD_FAILURE() << name << " mismatch at (" << px << "," << py << "): got " << int(G[i].r) << ","
                              << int(G[i].g) << "," << int(G[i].b) << "/" << int(G[i].a) << "  want " << int(want.r)
                              << "," << int(want.g) << "," << int(want.b) << "/" << int(want.a);
            } else if (chDelta(G[i], N[i]) > tol) {
                ++moved;
            }
        } else if (!exactEq(G[i], B[i])) {  // off the sprite the effected frame must equal the no-sprite scene
            ++leaked;
        }
    }
    EXPECT_GT(covered, 0) << name << ": the sprite covered no pixel";
    EXPECT_EQ(leaked, 0) << name << ": " << leaked << " pixels changed OFF the sprite";
    return moved;
}

TEST_F(SpriteEffects, ChainColorFillReplacesTheSilhouette) {
    const int moved = runEffectCell(device_, "chain_colorfill", [](Sprite& s) {
        s.effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill, .fill = Rgba8{240, 40, 40, 255}}};
    });
    EXPECT_GT(moved, 0) << "the fill did not change the sprite";
}

TEST_F(SpriteEffects, RegionColorFillFlashesInsideTheRectangleOnly) {
    // A rectangle over the left half of the sprite (quad x∈[0,4]); a Normal flash at alpha 0.6. Covered
    // pixels inside the rect grade toward the fill; covered pixels outside stay the base colour.
    const int moved = runEffectCell(device_, "region_flash", [](Sprite& s) {
        Region rg{.key = "flash"};
        rg.shape   = ShapePoints::rectangle(Point{0, 0}, 4, 8);
        rg.alpha   = 0.6f;
        rg.effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill, .fill = Rgba8{255, 255, 255, 255}}};
        s.regions  = {rg};
    });
    EXPECT_GT(moved, 0);
}

TEST_F(SpriteEffects, RegionMultiplyTintsWholeSilhouette) {
    const int moved = runEffectCell(device_, "region_multiply", [](Sprite& s) {
        Region rg{.key = "tint"};                        // empty shape ⇒ the whole silhouette
        rg.blend   = BlendMode::Multiply;
        rg.effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill, .fill = Rgba8{128, 128, 255, 255}}};
        s.regions  = {rg};
    });
    EXPECT_GT(moved, 0);
}

TEST_F(SpriteEffects, GleamIsIdentityAtGainZero) {
    // gain 0 is the identity contract — the sprite is byte-identical to the no-effect capture.
    const int moved = runEffectCell(device_, "gleam_identity", [](Sprite& s) {
        s.effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::Gleam, .sweep = 0.5f, .width = 0.3f, .gain = 0.0f}};
    }, /*tol=*/0);
    EXPECT_EQ(moved, 0) << "gleam at gain 0 moved a pixel";
}

TEST_F(SpriteEffects, GleamBrightensAlongTheBand) {
    const int moved = runEffectCell(device_, "gleam_boost", [](Sprite& s) {
        s.effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::Gleam, .sweep = 0.5f, .width = 0.6f,
                                       .gain = 2.0f, .slant = 0.0f}};
    });
    EXPECT_GT(moved, 0) << "gleam boost changed nothing";
}

TEST_F(SpriteEffects, TransparencyRegionPunchesAHole) {
    // A circle at the sprite centre, TransparentInside: those pixels discard and reveal the background. The
    // oracle returns nullopt there, so `want` becomes B (the no-sprite scene) — verified per pixel.
    const int moved = runEffectCell(device_, "transparency_hole", [](Sprite& s) {
        Region rg{.key = "hole"};
        rg.shape   = ShapePoints::circle(Point{4, 4}, 2.5f);
        rg.effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::Transparency,
                                        .stencil = StencilMode::TransparentInside}};
        s.regions  = {rg};
    });
    EXPECT_GT(moved, 0) << "the hole revealed nothing";
}

TEST_F(SpriteEffects, FillIntensityAboveOneBrightensThroughMultiply) {
    // A Multiply fill with fillIntensity > 1 BRIGHTENS (the float16 intermediates carry the headroom).
    const int moved = runEffectCell(device_, "fill_intensity", [](Sprite& s) {
        Region rg{.key = "expose"};
        rg.blend   = BlendMode::Multiply;
        rg.effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill,
                                        .fill = Rgba8{255, 255, 255, 255}, .fillIntensity = 1.8f}};
        s.regions  = {rg};
    });
    EXPECT_GT(moved, 0);
}

TEST_F(SpriteEffects, ChainThenRegionComposeInOrder) {
    const int moved = runEffectCell(device_, "chain_then_region", [](Sprite& s) {
        s.effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill, .fill = Rgba8{80, 80, 80, 255}}};
        Region rg{.key = "sheen"};
        rg.effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::Gleam, .sweep = 0.5f, .width = 0.8f, .gain = 1.5f}};
        s.regions = {rg};
    });
    EXPECT_GT(moved, 0);
}

TEST_F(SpriteEffects, NoEffectIsByteIdenticalToAPlainSprite) {
    // A sprite that sets empty effects/regions renders byte-identical to a plain sprite — the fxCount-0
    // early-out. runEffectCell's tol-0 covered comparison against the no-effect capture pins it.
    const int moved = runEffectCell(device_, "no_effect", [](Sprite& s) {
        s.effects.clear();
        s.regions.clear();
    }, /*tol=*/0);
    EXPECT_EQ(moved, 0);
}

// Red→green: a corrupted oracle (a Screen instead of the shipped Multiply) must redden the comparison — the
// device path is genuinely being checked, not trivially agreeing.
TEST_F(SpriteEffects, CorruptedOracleReddens) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const Art bg    = uploadBgArt(r);
    const Art solid = uploadSolid(r, Rgba8{110, 140, 190, 255});
    std::vector<TileCell> cN, cG;
    auto scene = [&](BlendMode blend, std::vector<TileCell>& cells, std::vector<Sprite>& keepS) {
        FrameDrawState f;
        f.layers.push_back(bgLayer(bg, cells));
        Sprite s{.key = "fx", .x = kSpriteX, .y = kSpriteY, .atlas = solid.atlas, .tile = 0, .palette = solid.palette};
        Region rg{.key = "t"};
        rg.blend   = blend;
        rg.effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill, .fill = Rgba8{200, 80, 80, 255}}};
        s.regions  = {rg};
        keepS = {s};
        DrawLayer sp{.key = "sprites"};
        sp.z = 10; sp.size = PixelSize{kW, kH};
        sp.content = SpriteContent{.sprites = std::span<const Sprite>(keepS)};
        f.layers.push_back(sp);
        return f;
    };
    std::vector<Sprite> sN, sG;
    FrameDrawState fN = scene(BlendMode::Normal,   cN, sN);   // base colour capture (region present but…)
    FrameDrawState fG = scene(BlendMode::Multiply,  cG, sG);
    const std::vector<Rgba8> N = r.captureViewport(fN);
    const std::vector<Rgba8> G = r.captureViewport(fG);

    Sprite oracle{.key = "fx"};
    Region rg{.key = "t"};
    rg.blend   = BlendMode::Screen;  // WRONG on purpose — the device rendered Multiply
    rg.effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill, .fill = Rgba8{200, 80, 80, 255}}};
    oracle.regions = {rg};
    const std::vector<SpriteFxRecord> recs = buildSpriteFxRecords(oracle);

    int mismatches = 0;
    for (std::size_t i = 0; i < G.size(); ++i) {
        const int px = static_cast<int>(i) % kW, py = static_cast<int>(i) / kW;
        const float u = (static_cast<float>(px) + 0.5f - kSpriteX) / 8.0f;
        const float v = (static_cast<float>(py) + 0.5f - kSpriteY) / 8.0f;
        const auto out = evalSpriteFxRecords(norm(N[i]), u, v, 8, 8, std::span<const SpriteFxRecord>(recs));
        if (out) {
            const Rgba8 want{quant(out->x), quant(out->y), quant(out->z), quant(out->w)};
            if (chDelta(G[i], want) > 2) ++mismatches;
        }
    }
    EXPECT_GT(mismatches, 0) << "a wrong (Screen) oracle agreed with the Multiply device output — the harness is inert";
}

}  // namespace
