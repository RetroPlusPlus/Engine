// Colour-parity on the PER-LAYER path: a ColorFill under a container blend mode, carried by a
// DrawLayer's region, graded at each ScreenSpaceEffectScope over transparent-holed content. This is
// the colour-modifier idiom — Multiply tint / day-night, Add / Screen glow, Normal flash-fade, a
// Multiply that BRIGHTENS via fillIntensity > 1 — verified end-to-end on the layer path a sprite's
// effect surface routes through: Layer scope grades a layer's own art in place (holes stay
// transparent), Below scope grades the accumulated image beneath (reaching through the holes).
//
// What lives elsewhere (this file adds only the missing cells):
//   - The CPU mirror — applyBlendMode / blendChannel / Normal==alpha-over / source-alpha weighting /
//     the ColorFill-grade equivalence — is tests/blend_mode_test.cpp (device-free, static_assert-anchored).
//   - The FRAME-level blend × ColorFill goldens (FrameDrawState::blend + whole-frame postEffects /
//     regions: blend_{add,subtract,multiply,screen,half}, colorfill_solid, multiply_brighten) are
//     tests/golden_readback_test.cpp. That file covers the frame path; the PER-LAYER DrawLayer path with
//     an explicit {Layer, Below} scope — what a per-layer / per-sprite effect uses — is covered here.
//
// Method: compare against the CPU mirror, with no new committed golden. Each cell captures a no-effect
// baseline of the scene, then captures the graded scene, and compares the graded pixels against the
// value the CPU mirror predicts — expectFillBlend(scene, fill, alpha, mode) = applyBlendMode(scene, {fill·intensity,
// alpha}, mode). Region alpha enters as the source alpha of that combine (post-mix by region alpha and
// src-alpha-into-the-blend are algebraically identical). The comparison tolerance is 2 8-bit steps: the
// mirror's destination is the 8-bit-CAPTURED baseline, so it carries one extra quantization step versus
// the renderer's float16 scene, on top of the per-pass rounding the arithmetic composites already incur
// (golden_readback_test.cpp holds its same-pipeline goldens to one step). The holes semantics need no
// tolerance at all — they are differential (Layer leaves a hole byte-identical to no effect; Below does not).
//
// Device-backed like golden_readback_test.cpp: a compose-only, windowless Renderer on a real GPU device
// (a software rasterizer in CI). Each device-backed test file bootstraps its own device; the fixture
// below mirrors the golden harness's setup.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "retropp/draw_state.h"
#include "retropp/geometry.h"
#include "retropp/image.h"       // TransparentIndices
#include "retropp/palette.h"
#include "retropp/postprocess.h"  // applyBlendMode, colorFillParams
#include "retropp/renderer.h"
#include "retropp/viewport.h"

namespace {
using namespace retropp;

// A pitch-friendly viewport (matches the golden harness): 64 px wide → 256-byte rows.
constexpr int kW = 64;
constexpr int kH = 64;

// ── device fixture (mirrors GoldenReadback's device bootstrap) ──────────────────────────────────

const char* archTag() {
#if defined(__aarch64__) || defined(_M_ARM64)
    return "arm64";
#elif defined(__x86_64__) || defined(_M_X64) || defined(__amd64__)
    return "x64";
#else
    return "unknown-arch";
#endif
}

// Windows on ARM is a courtesy runner with no production-representative GPU backend in CI; its
// production path (D3D12 + DXIL) is covered by the Windows x64 job, so a missing device there is an
// out-of-scope skip. Every production-representative platform requires a device (a missing one FAILS).
#if defined(_WIN32) && (defined(_M_ARM64) || defined(__aarch64__))
inline constexpr bool kDeviceOptional = true;
#else
inline constexpr bool kDeviceOptional = false;
#endif

class ColourParityLayer : public ::testing::Test {
protected:
    static inline SDL_GPUDevice* device_ = nullptr;
    static inline std::string    initError_;

    static void SetUpTestSuite() {
        if (!SDL_Init(SDL_INIT_VIDEO)) {
            initError_ = std::string("SDL_Init(SDL_INIT_VIDEO) failed: ") + SDL_GetError();
            return;
        }
        device_ = SDL_CreateGPUDevice(
            SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_MSL,
            /*debug_mode=*/false, /*name=*/nullptr);
        if (!device_) initError_ = std::string("SDL_CreateGPUDevice failed: ") + SDL_GetError();
    }

    static void TearDownTestSuite() {
        if (device_) {
            SDL_DestroyGPUDevice(device_);
            device_ = nullptr;
        }
        SDL_Quit();
    }

    void SetUp() override {
        if (!device_) {
            if (kDeviceOptional) {
                GTEST_SKIP() << "Windows on ARM is a courtesy runner with no production-representative GPU "
                                "backend in CI; the per-layer colour math is the device-free applyBlendMode "
                                "mirror (blend_mode_test.cpp), and the D3D12 path is covered by Windows x64. ("
                             << initError_ << ")";
            }
            FAIL() << "no GPU device reachable — " << initError_
                   << ". This colour-parity harness requires a GPU device on every production-representative "
                      "platform (macOS/Metal, Windows-x64/D3D12, Linux/Vulkan); a software rasterizer "
                      "(lavapipe / WARP) suffices; on a headless runner set SDL_VIDEODRIVER=offscreen.";
        }
    }
};

// ── shared art ──────────────────────────────────────────────────────────────────────────────────

struct Art {
    AtlasId   atlas{};
    PaletteId palette{};
};

// A 16×16 index atlas (a 2×2 grid of 8×8 tiles; indices 0..3 in 4-px blocks) + a 4-colour opaque
// palette — the same deterministic art the golden harness uses. `transparent` selects whether index 0
// is a structural hole (GameBoy) or an opaque colour (None).
Art uploadArt(Renderer& r, TransparentIndices transparent) {
    std::array<std::uint8_t, 16 * 16> idx{};
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x)
            idx[static_cast<std::size_t>(y) * 16 + static_cast<std::size_t>(x)] =
                static_cast<std::uint8_t>(((x / 4) + (y / 4)) % 4);
    const AtlasId atlas = r.uploadAtlas(idx.data(), 16, 16, transparent);
    const std::array<Rgba8, 4> pal{{{20, 20, 30}, {200, 60, 60}, {60, 200, 90}, {230, 230, 240}}};
    const PaletteId palette = r.uploadPalette(std::span<const Rgba8>(pal));
    return {atlas, palette};
}

// A full-viewport, fully-opaque tile layer at `z` — the base every parity scene grades. `keepCells`
// outlives the returned layer's span (the caller owns it).
DrawLayer tileLayer(const char* key, std::int32_t z, const Art& art, std::vector<TileCell>& keepCells) {
    keepCells.resize(8 * 8);
    for (int ty = 0; ty < 8; ++ty)
        for (int tx = 0; tx < 8; ++tx)
            keepCells[static_cast<std::size_t>(ty) * 8 + static_cast<std::size_t>(tx)] =
                TileCell{.atlas   = art.atlas,
                         .tile    = static_cast<std::uint16_t>((tx + ty) % 4),
                         .palette = art.palette};
    DrawLayer l{.key = key};
    l.z       = z;
    l.size    = PixelSize{kW, kH};
    l.content = TileContent{.widthInTiles = 8, .heightInTiles = 8,
                            .cells = std::span<const TileCell>(keepCells)};
    return l;
}

// ── CPU-mirror expected pixel ─────────────────────────────────────────────────────────────────────

Vec4 norm(Rgba8 c) {
    return Vec4{static_cast<float>(c.r) / 255.0f, static_cast<float>(c.g) / 255.0f,
                static_cast<float>(c.b) / 255.0f, static_cast<float>(c.a) / 255.0f};
}

std::uint8_t quant(float v) {
    const float c = std::clamp(v, 0.0f, 1.0f);
    return static_cast<std::uint8_t>(std::lround(c * 255.0f));
}

// The colour a ColorFill of `fill` (scaled by `intensity`) under `mode` at container alpha `alpha`
// grades over an opaque scene pixel — the applyBlendMode / colorFillParams authority, quantized.
Rgba8 expectFillBlend(Rgba8 scenePx, Rgba8 fill, float intensity, float alpha, BlendMode mode) {
    const ColorFillParams fp =
        colorFillParams(ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill,
                                          .fill = fill, .fillIntensity = intensity});
    const Vec4 out = applyBlendMode(norm(scenePx), Vec4{fp.r, fp.g, fp.b, alpha}, mode);
    return Rgba8{quant(out.x), quant(out.y), quant(out.z), quant(out.w)};
}

// Compare a graded capture against the mirror's prediction from the captured baseline, per channel.
// maxDelta 2: the mirror's destination is the 8-bit baseline (one extra quantization vs the renderer's
// float16 scene) atop the arithmetic composite's per-pass rounding.
::testing::AssertionResult
matchesGrade(const std::vector<Rgba8>& got, const std::vector<Rgba8>& baseline, Rgba8 fill,
             float intensity, float alpha, BlendMode mode, int maxDelta = 2) {
    if (got.size() != baseline.size())
        return ::testing::AssertionFailure() << "size mismatch " << got.size() << " vs " << baseline.size();
    for (std::size_t i = 0; i < got.size(); ++i) {
        const Rgba8 want = expectFillBlend(baseline[i], fill, intensity, alpha, mode);
        const int dr = std::abs(int(got[i].r) - int(want.r));
        const int dg = std::abs(int(got[i].g) - int(want.g));
        const int db = std::abs(int(got[i].b) - int(want.b));
        const int da = std::abs(int(got[i].a) - int(want.a));
        if (dr > maxDelta || dg > maxDelta || db > maxDelta || da > maxDelta) {
            const int x = int(i) % kW, y = int(i) / kW;
            return ::testing::AssertionFailure()
                   << "grade mismatch at (" << x << ", " << y << "): got " << int(got[i].r) << ","
                   << int(got[i].g) << "," << int(got[i].b) << "," << int(got[i].a) << "  mirror wants "
                   << int(want.r) << "," << int(want.g) << "," << int(want.b) << "," << int(want.a)
                   << "  (allowed ±" << maxDelta << ")";
        }
    }
    return ::testing::AssertionSuccess();
}

bool exactEq(Rgba8 a, Rgba8 b) { return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a; }
bool changed(Rgba8 a, Rgba8 b) {
    return std::abs(int(a.r) - int(b.r)) > 2 || std::abs(int(a.g) - int(b.g)) > 2 ||
           std::abs(int(a.b) - int(b.b)) > 2 || std::abs(int(a.a) - int(b.a)) > 2;
}

// One whole-layer ColorFill grade: a DrawLayer carrying a single empty-shape (whole-reach) Region whose
// ColorFill is graded by the region's `blend` at `scope`. This is the colour-modifier idiom (the
// no-shape region that honours blend + alpha), on the per-layer path.
Region gradeRegion(Rgba8 fill, float intensity, float alpha, BlendMode blend, ScreenSpaceEffectScope scope) {
    Region reg{.key = "grade"};
    reg.blend   = blend;
    reg.alpha   = alpha;
    reg.effects = {ScreenSpaceEffect{
        .kind = ScreenSpaceEffectKind::ColorFill, .scope = scope, .fill = fill, .fillIntensity = intensity}};
    return reg;
}

// ── The Below-scope matrix: each blend mode grades the accumulated image beneath the fx layer ──────
//
// A content-less fx layer (z 5) above an opaque tile background (z 0) carries the grade at Below scope,
// so it grades the composited image — the direct precedent for a Below-scope per-sprite effect. Each
// mode's output is checked against the applyBlendMode mirror.

struct BelowCell {
    const char* name;
    BlendMode   mode;
    Rgba8       fill;
};

void runBelowCell(SDL_GPUDevice* dev, const BelowCell& cell) {
    Renderer r{dev, nullptr, ViewportResolution{kW, kH}};
    const Art art = uploadArt(r, TransparentIndices::None);
    std::vector<TileCell> bgCells;

    // Baseline: the opaque background alone.
    FrameDrawState base;
    base.layers.push_back(tileLayer("bg", 0, art, bgCells));
    const std::vector<Rgba8> baseline = r.captureViewport(base);

    // Graded: a content-less fx layer whose Below-scope ColorFill grades the composite.
    FrameDrawState frame;
    frame.layers.push_back(tileLayer("bg", 0, art, bgCells));
    DrawLayer fx{.key = "fx"};
    fx.z       = 5;
    fx.size    = PixelSize{kW, kH};
    fx.regions = {gradeRegion(cell.fill, 1.0f, 1.0f, cell.mode, ScreenSpaceEffectScope::Below)};
    frame.layers.push_back(fx);
    const std::vector<Rgba8> got = r.captureViewport(frame);

    EXPECT_TRUE(matchesGrade(got, baseline, cell.fill, 1.0f, 1.0f, cell.mode)) << cell.name;
}

TEST_F(ColourParityLayer, BelowScopeNormal)   { runBelowCell(device_, {"below_normal",   BlendMode::Normal,   Rgba8{200, 100, 50, 255}}); }
TEST_F(ColourParityLayer, BelowScopeAdd)      { runBelowCell(device_, {"below_add",      BlendMode::Add,      Rgba8{40, 30, 20, 255}}); }
TEST_F(ColourParityLayer, BelowScopeSubtract) { runBelowCell(device_, {"below_subtract", BlendMode::Subtract, Rgba8{40, 30, 20, 255}}); }
TEST_F(ColourParityLayer, BelowScopeMultiply) { runBelowCell(device_, {"below_multiply", BlendMode::Multiply, Rgba8{128, 128, 200, 255}}); }
TEST_F(ColourParityLayer, BelowScopeScreen)   { runBelowCell(device_, {"below_screen",   BlendMode::Screen,   Rgba8{80, 80, 120, 255}}); }
TEST_F(ColourParityLayer, BelowScopeHalf)     { runBelowCell(device_, {"below_half",     BlendMode::Half,     Rgba8{200, 100, 50, 255}}); }

// ── The Layer-scope matrix: each blend mode grades the layer's OWN opaque art in place ─────────────
//
// The grade rides the bottom (opaque, full-viewport) layer itself at Layer scope, so it grades that
// layer's own content — the direct precedent for a per-sprite Layer-scope effect. Over fully opaque
// content the composited result is the graded pixel, checked against the same mirror.

void runLayerCell(SDL_GPUDevice* dev, const char* name, BlendMode mode, Rgba8 fill) {
    Renderer r{dev, nullptr, ViewportResolution{kW, kH}};
    const Art art = uploadArt(r, TransparentIndices::None);
    std::vector<TileCell> baseCells, gradeCells;

    FrameDrawState base;
    base.layers.push_back(tileLayer("bg", 0, art, baseCells));
    const std::vector<Rgba8> baseline = r.captureViewport(base);

    FrameDrawState frame;
    DrawLayer bg = tileLayer("bg", 0, art, gradeCells);
    bg.regions   = {gradeRegion(fill, 1.0f, 1.0f, mode, ScreenSpaceEffectScope::Layer)};
    frame.layers.push_back(bg);
    const std::vector<Rgba8> got = r.captureViewport(frame);

    EXPECT_TRUE(matchesGrade(got, baseline, fill, 1.0f, 1.0f, mode)) << name;
}

TEST_F(ColourParityLayer, LayerScopeNormal)   { runLayerCell(device_, "layer_normal",   BlendMode::Normal,   Rgba8{200, 100, 50, 255}); }
TEST_F(ColourParityLayer, LayerScopeMultiply) { runLayerCell(device_, "layer_multiply", BlendMode::Multiply, Rgba8{128, 128, 200, 255}); }
TEST_F(ColourParityLayer, LayerScopeAdd)      { runLayerCell(device_, "layer_add",      BlendMode::Add,      Rgba8{40, 30, 20, 255}); }

// ── Region alpha: a partial-strength grade lies between the scene and the full grade ───────────────
//
// The region alpha is the source alpha of the combine — a Normal grade at 0.5 is a half-flash toward the
// fill, a Multiply at 0.5 a half-strength shadow. Both checked against applyBlendMode at that alpha.

void runAlphaCell(SDL_GPUDevice* dev, const char* name, BlendMode mode, Rgba8 fill, float alpha) {
    Renderer r{dev, nullptr, ViewportResolution{kW, kH}};
    const Art art = uploadArt(r, TransparentIndices::None);
    std::vector<TileCell> bgCells;

    FrameDrawState base;
    base.layers.push_back(tileLayer("bg", 0, art, bgCells));
    const std::vector<Rgba8> baseline = r.captureViewport(base);

    FrameDrawState frame;
    frame.layers.push_back(tileLayer("bg", 0, art, bgCells));
    DrawLayer fx{.key = "fx"};
    fx.z       = 5;
    fx.size    = PixelSize{kW, kH};
    fx.regions = {gradeRegion(fill, 1.0f, alpha, mode, ScreenSpaceEffectScope::Below)};
    frame.layers.push_back(fx);
    const std::vector<Rgba8> got = r.captureViewport(frame);

    EXPECT_TRUE(matchesGrade(got, baseline, fill, 1.0f, alpha, mode)) << name;
}

TEST_F(ColourParityLayer, RegionAlphaNormalHalf)   { runAlphaCell(device_, "alpha_normal_half",   BlendMode::Normal,   Rgba8{240, 240, 255, 255}, 0.5f); }
TEST_F(ColourParityLayer, RegionAlphaMultiplyHalf) { runAlphaCell(device_, "alpha_multiply_half", BlendMode::Multiply, Rgba8{128, 128, 200, 255}, 0.5f); }

// ── fillIntensity > 1 under Multiply BRIGHTENS on the layer path ───────────────────────────────────
//
// A white ColorFill at fillIntensity 1.5, Multiply, at Layer scope: scene · 1.5, an exposure the float16
// intermediates carry past 1 before the blit clamps. Checked both as a capability (no pixel darker than
// the baseline) and against the mirror (clamp(scene · 1.5)).

TEST_F(ColourParityLayer, LayerScopeMultiplyBrighten) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const Art art = uploadArt(r, TransparentIndices::None);
    std::vector<TileCell> baseCells, gradeCells;

    FrameDrawState base;
    base.layers.push_back(tileLayer("bg", 0, art, baseCells));
    const std::vector<Rgba8> baseline = r.captureViewport(base);

    FrameDrawState frame;
    DrawLayer bg = tileLayer("bg", 0, art, gradeCells);
    bg.regions   = {gradeRegion(Rgba8{255, 255, 255, 255}, 1.5f, 1.0f, BlendMode::Multiply,
                                ScreenSpaceEffectScope::Layer)};
    frame.layers.push_back(bg);
    const std::vector<Rgba8> got = r.captureViewport(frame);

    ASSERT_EQ(got.size(), baseline.size());
    bool anyDarker = false, anyBrighter = false;
    std::size_t darkerAt = 0;
    for (std::size_t i = 0; i < got.size(); ++i) {
        if (got[i].r + 2 < baseline[i].r || got[i].g + 2 < baseline[i].g || got[i].b + 2 < baseline[i].b) {
            anyDarker = true;
            darkerAt  = i;
            break;
        }
        if (got[i].r > baseline[i].r || got[i].g > baseline[i].g || got[i].b > baseline[i].b)
            anyBrighter = true;
    }
    EXPECT_FALSE(anyDarker) << "Multiply at fillIntensity 1.5 darkened pixel " << darkerAt
                            << " — a multiplicative exposure must not dim the layer";
    EXPECT_TRUE(anyBrighter) << "fillIntensity 1.5 brightened no pixel — the float16 headroom is not reaching the blend";
    EXPECT_TRUE(matchesGrade(got, baseline, Rgba8{255, 255, 255, 255}, 1.5f, 1.0f, BlendMode::Multiply))
        << "layer_multiply_brighten";
}

// ── Holes: Layer scope keeps a layer's transparent holes transparent; Below scope grades through ───
//
// A holed upper layer (index 0 = a structural hole) over an opaque lower layer, graded by one Multiply
// ColorFill at Layer vs Below scope. The differential IS the semantics, so no tolerance is needed:
//   - a HOLE pixel: Layer scope leaves it byte-identical to no effect (the upper layer's transparent
//     pixels are untouched, the lower layer shows through ungraded), while Below scope grades it (the
//     effect reached the accumulated image through the hole) — so the two scopes DIFFER there.
//   - an ART pixel: both scopes grade the same opaque pixel identically — so the two scopes AGREE there,
//     and both differ from no effect.
// The scene is classified pixel-by-pixel from the three captures, and both classes must be non-empty.

TEST_F(ColourParityLayer, HolesLayerVsBelowScope) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const Art lower = uploadArt(r, TransparentIndices::None);     // opaque backing
    const Art upper = uploadArt(r, TransparentIndices::GameBoy);  // index 0 → holes
    const Rgba8 fill{60, 60, 255, 255};                           // a strongly visible Multiply tint

    // Three scenes sharing the lower + holed-upper stack; the region's scope is the only variable.
    auto build = [&](ScreenSpaceEffectScope scope, bool withGrade, std::vector<TileCell>& lo,
                     std::vector<TileCell>& up, FrameDrawState& frame) {
        frame.layers.push_back(tileLayer("lo", 0, lower, lo));
        DrawLayer hi = tileLayer("hi", 10, upper, up);
        if (withGrade)
            hi.regions = {gradeRegion(fill, 1.0f, 1.0f, BlendMode::Multiply, scope)};
        frame.layers.push_back(hi);
    };

    std::vector<TileCell> l0, u0, l1, u1, l2, u2;
    FrameDrawState fNone, fLayer, fBelow;
    build(ScreenSpaceEffectScope::Layer, false, l0, u0, fNone);
    build(ScreenSpaceEffectScope::Layer, true,  l1, u1, fLayer);
    build(ScreenSpaceEffectScope::Below, true,  l2, u2, fBelow);
    const std::vector<Rgba8> none  = r.captureViewport(fNone);
    const std::vector<Rgba8> layer = r.captureViewport(fLayer);
    const std::vector<Rgba8> below = r.captureViewport(fBelow);
    ASSERT_EQ(none.size(), layer.size());
    ASSERT_EQ(none.size(), below.size());

    int holePixels = 0, artPixels = 0;
    for (std::size_t i = 0; i < none.size(); ++i) {
        const bool layerUnchanged = exactEq(layer[i], none[i]);
        const bool belowGraded    = changed(below[i], none[i]);
        if (layerUnchanged && belowGraded) {
            // A hole: the two scopes must genuinely diverge here (Layer left it, Below graded it).
            EXPECT_TRUE(changed(layer[i], below[i]))
                << "hole pixel " << i << ": Layer and Below scope agree — the differential proves nothing";
            ++holePixels;
        }
        const bool layerGraded = changed(layer[i], none[i]);
        if (layerGraded && !changed(layer[i], below[i])) {
            // Art: Layer graded it, and Below graded it identically (same opaque pixel).
            ++artPixels;
        }
    }
    EXPECT_GT(holePixels, 0) << "no hole pixels found — the upper layer has no transparent index-0 texels";
    EXPECT_GT(artPixels, 0) << "no art pixels found — the grade reached no opaque upper-layer pixel";
}

}  // namespace
