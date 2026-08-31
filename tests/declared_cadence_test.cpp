// A layer declares how often its world advances, and the renderer eases its motion across that many
// ticks instead of across one. These cases pin the factor arithmetic, the mirror's rotation rule, where
// the declaration comes from, and the identity that a layer declaring nothing draws exactly what it drew
// before the declaration existed.

#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <SDL3/SDL.h>
#include <gtest/gtest.h>

#include "retropp/draw_state.h"
#include "retropp/interpolation.h"
#include "retropp/renderer.h"
#include "retropp/viewport.h"
#include "steady_timing.h"

namespace retropp {
namespace {

// ── The factor, as arithmetic ────────────────────────────────────────────────────────────────

// The expression the run loop publishes as `alpha`. A layer that declares no cadence must ease at
// exactly this, bit for bit, or a game that never heard of the declaration renders differently.
[[nodiscard]] float shippedFactor(std::uint32_t span, float raw) {
    return (static_cast<float>(span - 1) + raw) / static_cast<float>(span);
}
[[nodiscard]] std::uint32_t bits(float f) { return std::bit_cast<std::uint32_t>(f); }

TEST(DeclaredCadence, TheUndeclaredFactorIsBitwiseTheShippedExpression) {
    for (const std::uint32_t span : {1u, 2u, 3u, 7u, 14u}) {
        for (const float raw : {0.0f, 0.1f, 0.25f, 0.5f, 0.75f, 0.9999f}) {
            // Cadence 1, changed on this commit — the steady state and every catch-up frame.
            EXPECT_EQ(bits(easeFactor(1, span, 0, raw)), bits(shippedFactor(span, raw)))
                << "span " << span << " raw " << raw;
        }
    }
}

TEST(DeclaredCadence, TheFactorSpreadsAMoveOverWhicheverSpanIsLonger) {
    // Cadence 2 against a single-tick commit: the move covers two ticks, so the fraction halves.
    EXPECT_FLOAT_EQ(easeFactor(2, 1, 0, 0.0f), 0.0f);
    EXPECT_FLOAT_EQ(easeFactor(2, 1, 0, 0.5f), 0.25f);
    EXPECT_FLOAT_EQ(easeFactor(2, 1, 1, 0.0f), 0.5f);
    EXPECT_FLOAT_EQ(easeFactor(2, 1, 1, 0.5f), 0.75f);

    // A commit that ran five ticks covers more ground than the cadence, so the span wins and the
    // render lands well along the interval — where a large catch-up belongs.
    EXPECT_FLOAT_EQ(easeFactor(2, 5, 0, 0.0f), 0.6f);
    EXPECT_FLOAT_EQ(easeFactor(1, 5, 0, 0.0f), 0.8f);  // the shipped catch-up value, unchanged
}

TEST(DeclaredCadence, TheFactorSaturatesRatherThanRunningPastTheCurrentValue) {
    EXPECT_FLOAT_EQ(easeFactor(2, 1, 5, 0.5f), 1.0f);   // a world that stopped draws where it stopped
    EXPECT_FLOAT_EQ(easeFactor(1, 1, 3, 0.0f), 1.0f);
}

TEST(DeclaredCadence, ADeclaredZeroReadsAsOne) {
    EXPECT_EQ(resolveCadence(std::nullopt, 1), 1u);
    EXPECT_EQ(resolveCadence(std::nullopt, 4), 4u);   // the frame's, when the layer declares none
    EXPECT_EQ(resolveCadence(3, 4), 3u);              // the layer's, when it declares one
    EXPECT_EQ(resolveCadence(1, 4), 1u);              // including declaring its way back down to 1
    EXPECT_EQ(resolveCadence(0, 4), 1u);              // zero is not a cadence
    EXPECT_EQ(resolveCadence(std::nullopt, 0), 1u);
    EXPECT_EQ(bits(easeFactor(0, 1, 0, 0.25f)), bits(easeFactor(1, 1, 0, 0.25f)));
}

// ── The mirror under a declaration ───────────────────────────────────────────────────────────

[[nodiscard]] FrameDrawState worldAt(int x, std::optional<std::uint32_t> layerCadence = std::nullopt,
                                     std::uint32_t frameCadence = 1) {
    FrameDrawState f;
    f.advancesEvery = frameCadence;
    f.layers.push_back(
        DrawLayer{.key = "world", .z = 0, .scroll = {x, 0}, .advancesEvery = layerCadence});
    return f;
}

// The un-rounded drawn x for the world layer at sub-tick `sub`.
[[nodiscard]] float drawnX(const Interpolator& interp, float sub) {
    const auto p = interp.interpolatedLayerScroll("world", tickAt(sub));
    return p ? p->x : 0.0f;
}

// A world that advances `step` pixels every `moveEvery` ticks while declaring `declare`, driven one tick
// per commit. What it does and what it says it does are separate arguments on purpose — the cases below
// turn on the difference. Returns the drawn x at `sub` after each of `ticks` commits.
[[nodiscard]] std::vector<float> walk(int step, int moveEvery, std::optional<std::uint32_t> declare,
                                      int ticks, float sub) {
    Interpolator       interp;
    std::vector<float> drawn;
    int                x = 0;
    for (int t = 1; t <= ticks; ++t) {
        if (t > 1 && (t - 1) % moveEvery == 0) x += step;
        interp.reconcile(worldAt(x, declare), tickAt(sub));
        drawn.push_back(drawnX(interp, sub));
    }
    return drawn;
}

TEST(DeclaredCadence, ATwoTickCadenceSplitsATwoPixelMoveEvenly) {
    // 2 px every 2 ticks, declared. The drawn position advances one pixel per frame — the move is
    // spread over the two ticks it actually takes instead of over the one the engine saw it on.
    const std::vector<float> d = walk(2, 2, 2, 7, 0.0f);
    ASSERT_EQ(d.size(), 7u);
    for (std::size_t i = 3; i + 1 < d.size(); ++i) {
        EXPECT_FLOAT_EQ(d[i + 1] - d[i], 1.0f) << "frame " << i;
    }
}

TEST(DeclaredCadence, TheSplitStaysEvenWhereverTheSubTickFractionLands) {
    // The defect this removes is a split that depends on where the fraction falls: at 0.5 the two
    // frames share the move evenly, at 0.9 one takes 1.8 px and the other 0.2. Declared, every
    // fraction gives the same even advance; undeclared, only 0.5 does.
    for (const float sub : {0.1f, 0.5f, 0.9f}) {
        const std::vector<float> declared = walk(2, 2, 2, 7, sub);
        for (std::size_t i = 3; i + 1 < declared.size(); ++i) {
            EXPECT_FLOAT_EQ(declared[i + 1] - declared[i], 1.0f) << "sub " << sub;
        }
    }
    const std::vector<float> undeclared = walk(2, 2, std::nullopt, 7, 0.9f);
    const float              a = undeclared[4] - undeclared[3];
    const float              b = undeclared[5] - undeclared[4];
    EXPECT_NE(a, b);                 // the uneven split an undeclared layer still has
    EXPECT_FLOAT_EQ(a + b, 2.0f);    // and it is one 2 px move either way
}

TEST(DeclaredCadence, TheDrawnPositionLagsByTheDeclaredCadence) {
    // The picture runs a cadence behind: at the moment the engine sees a new value it still needs
    // somewhere to travel to. Two ticks of a 1 px-per-tick average is 2 px.
    const std::vector<float> d = walk(2, 2, 2, 7, 0.0f);
    const int                worldAfter7 = 6;   // moves at ticks 3, 5, 7
    EXPECT_FLOAT_EQ(d.back(), static_cast<float>(worldAfter7 - 2));
}

TEST(DeclaredCadence, OverDeclaringCostsLagAndNotSmoothness) {
    // A world that moves every tick but declares 2 still draws even motion — one pixel per frame —
    // one tick further behind than it need be. A wrong declaration is a latency mistake, not a
    // smoothness one.
    const std::vector<float> d = walk(1, 1, 2, 7, 0.0f);
    for (std::size_t i = 2; i + 1 < d.size(); ++i) {
        EXPECT_FLOAT_EQ(d[i + 1] - d[i], 1.0f) << "frame " << i;
    }
}

TEST(DeclaredCadence, AnEarlyChangeStartsANewSpan) {
    // The submitted diff decides when a span begins; the declaration only says how wide it is. A world
    // that moves sooner than it declared restarts the ease from where it had reached, rather than
    // being held to a schedule it has already left.
    Interpolator interp;
    interp.reconcile(worldAt(0, 3), tickAt(0.0f));    // tick 1: mount at 0
    interp.reconcile(worldAt(30, 3), tickAt(0.0f));   // tick 2: prev 0, cur 30, over 3 ticks
    EXPECT_FLOAT_EQ(drawnX(interp, 0.0f), 0.0f);
    interp.reconcile(worldAt(30, 3), tickAt(0.0f));   // tick 3: still easing
    EXPECT_FLOAT_EQ(drawnX(interp, 0.0f), 10.0f);
    interp.reconcile(worldAt(60, 3), tickAt(0.0f));   // tick 4: early — the span restarts here
    EXPECT_FLOAT_EQ(drawnX(interp, 0.0f), 30.0f);     // from the value it had reached, not from 20
}

TEST(DeclaredCadence, TheSpanAtTheChangeSetsTheWidthNotTheCurrentSpan) {
    // The two held values are as far apart as the commit that produced them, so a later single-tick
    // commit must not narrow the interval they are eased across.
    Interpolator interp;
    interp.reconcile(worldAt(0, 2), tickAt(0.0f, 1));
    interp.reconcile(worldAt(100, 2), tickAt(0.0f, 5));  // five ticks of ground in one commit
    EXPECT_FLOAT_EQ(drawnX(interp, 0.0f), 60.0f);        // (5 - 2 + 0) / 5
    interp.reconcile(worldAt(100, 2), tickAt(0.0f, 1));  // a single-tick commit with nothing new
    EXPECT_FLOAT_EQ(drawnX(interp, 0.0f), 80.0f);        // (5 - 2 + 1) / 5, not (2 - 2 + 1) / 2
}

TEST(DeclaredCadence, AStoppedWorldFinishesItsStepThenSettles) {
    Interpolator interp;
    interp.reconcile(worldAt(0, 3), tickAt(0.0f));
    EXPECT_TRUE(interp.allSettled());                 // a mount holds one value
    interp.reconcile(worldAt(30, 3), tickAt(0.0f));   // moved — mid-ease from here
    EXPECT_FALSE(interp.allSettled());
    interp.reconcile(worldAt(30, 3), tickAt(0.0f));
    EXPECT_FALSE(interp.allSettled());                // one tick into a three-tick step
    interp.reconcile(worldAt(30, 3), tickAt(0.0f));
    EXPECT_FALSE(interp.allSettled());                // two
    interp.reconcile(worldAt(30, 3), tickAt(0.0f));
    EXPECT_TRUE(interp.allSettled());                 // the step is finished — the frame can be skipped
    EXPECT_FLOAT_EQ(drawnX(interp, 0.0f), 30.0f);
}

TEST(DeclaredCadence, AnUndeclaredWorldSettlesOnTheFirstUnchangedCommit) {
    // The compose skip gates on allSettled(), so a layer that declares nothing must settle exactly
    // when it always has: the commit after the one that stopped it.
    Interpolator interp;
    interp.reconcile(worldAt(0), tickAt(0.0f));
    interp.reconcile(worldAt(10), tickAt(0.0f));
    EXPECT_FALSE(interp.allSettled());
    interp.reconcile(worldAt(10), tickAt(0.0f));
    EXPECT_TRUE(interp.allSettled());
}

TEST(DeclaredCadence, ALayerWithNoDeclarationTakesTheFrames) {
    Interpolator interp;
    interp.reconcile(worldAt(0, std::nullopt, 2), tickAt(0.0f));
    interp.reconcile(worldAt(20, std::nullopt, 2), tickAt(0.0f));
    EXPECT_FLOAT_EQ(drawnX(interp, 0.0f), 0.0f);
    interp.reconcile(worldAt(20, std::nullopt, 2), tickAt(0.0f));
    EXPECT_FLOAT_EQ(drawnX(interp, 0.0f), 10.0f);   // eased across two ticks, from the frame's default
}

TEST(DeclaredCadence, ALayersDeclarationOverridesTheFrames) {
    Interpolator interp;
    interp.reconcile(worldAt(0, 1, 4), tickAt(0.5f));   // the frame says 4; this layer says 1
    interp.reconcile(worldAt(20, 1, 4), tickAt(0.5f));
    // Half way through a one-tick step. Had the frame's 4 won, an eighth of the way — 2.5 px.
    EXPECT_FLOAT_EQ(drawnX(interp, 0.5f), 10.0f);
}

TEST(DeclaredCadence, ADeclarationChangedMidRunAppliesAtTheNextCommit) {
    Interpolator interp;
    interp.reconcile(worldAt(0, 4), tickAt(0.0f));
    interp.reconcile(worldAt(40, 4), tickAt(0.0f));
    interp.reconcile(worldAt(40, 4), tickAt(0.0f));
    EXPECT_FLOAT_EQ(drawnX(interp, 0.0f), 10.0f);       // a quarter of the way, at cadence 4
    interp.reconcile(worldAt(40, 1), tickAt(0.0f));     // the game narrows its declaration
    EXPECT_FLOAT_EQ(drawnX(interp, 0.0f), 40.0f);       // in force at once — a one-tick step is done
}

// ── Sprites ──────────────────────────────────────────────────────────────────────────────────

[[nodiscard]] FrameDrawState spriteFrame(std::span<const Sprite> sprites,
                                         std::optional<std::uint32_t> layerCadence,
                                         std::uint32_t frameCadence) {
    FrameDrawState f;
    f.advancesEvery = frameCadence;
    f.layers.push_back(DrawLayer{.key           = "actors",
                                 .z             = 0,
                                 .content       = SpriteContent{sprites},
                                 .advancesEvery = layerCadence});
    return f;
}

TEST(DeclaredCadence, ASpriteTakesItsLayersCadence) {
    // A sprite advances with the layer it lives in, so the layer's declaration governs it — not the
    // frame's, which this layer overrides.
    const std::vector<Sprite> a{Sprite{.key = "hero", .x = 0, .y = 0}};
    const std::vector<Sprite> b{Sprite{.key = "hero", .x = 20, .y = 0}};

    Interpolator interp;
    interp.reconcile(spriteFrame(a, 2, 1), tickAt(0.0f));
    interp.reconcile(spriteFrame(b, 2, 1), tickAt(0.0f));
    ASSERT_TRUE(interp.interpolatedSpritePos("hero", tickAt(0.0f)).has_value());
    EXPECT_FLOAT_EQ(interp.interpolatedSpritePos("hero", tickAt(0.0f))->x, 0.0f);
    interp.reconcile(spriteFrame(b, 2, 1), tickAt(0.0f));
    EXPECT_FLOAT_EQ(interp.interpolatedSpritePos("hero", tickAt(0.0f))->x, 10.0f);
}

TEST(DeclaredCadence, TheFloatAccessorAndTheFrameAgreeOnOneObject) {
    // The output-resolution placement path and the draw-state path read the same factor, so they can
    // never place an object in two places. The values here land on whole pixels, so the rounding the
    // frame does is not what is being compared.
    Interpolator interp;
    interp.reconcile(worldAt(0, 2), tickAt(0.0f));
    const FrameDrawState moved = worldAt(10, 2);
    interp.reconcile(moved, tickAt(0.4f));

    const FrameTiming t = tickAt(0.4f);
    const auto        f = interp.interpolatedLayerScroll("world", t);
    ASSERT_TRUE(f.has_value());
    EXPECT_FLOAT_EQ(f->x, 2.0f);                                   // (0 + 0.4) / 2 of 10 px
    EXPECT_EQ(interp.interpolate(moved, t).layers[0].scroll.x, 2);
}

// ── Interpolation off ────────────────────────────────────────────────────────────────────────

#if defined(_WIN32) && (defined(_M_ARM64) || defined(__aarch64__))
inline constexpr bool kDeviceOptional = true;
#else
inline constexpr bool kDeviceOptional = false;
#endif

constexpr int kW = 32, kH = 32;

class CadenceCompose : public ::testing::Test {
protected:
    static inline SDL_GPUDevice* device_ = nullptr;
    static inline std::string    initError_;
    static void                  SetUpTestSuite() {
        if (!SDL_Init(SDL_INIT_VIDEO)) {
            initError_ = std::string("SDL_Init failed: ") + SDL_GetError();
            return;
        }
        device_ = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL |
                                          SDL_GPU_SHADERFORMAT_METALLIB,
                                      false, nullptr);
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
            if (kDeviceOptional)
                GTEST_SKIP() << "Windows on ARM has no production-representative GPU backend in CI; the "
                                "factor and mirror coverage is the device-free part, and the D3D12 path is "
                                "covered by Windows x64. (" << initError_ << ")";
            FAIL() << "no GPU device reachable — " << initError_;
        }
    }
};

TEST_F(CadenceCompose, ADeclarationDoesNotReachAComposeThatIsNotInterpolating) {
    // captureViewport composes the submission verbatim — no mirror, no factor. A declaration is a
    // statement about easing, so it must leave those bytes alone.
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const std::array<std::uint8_t, 8 * 8> idxs{};
    const AtlasId                         atlas = r.uploadAtlas(idxs.data(), 8, 8).atlasId;
    const std::array<Rgba8, 4>            pal{{Rgba8{200, 40, 40, 255}, Rgba8{200, 40, 40, 255},
                                               Rgba8{200, 40, 40, 255}, Rgba8{200, 40, 40, 255}}};
    const PaletteId                       palette = r.uploadPalette(std::span<const Rgba8>(pal));

    std::vector<Sprite> sprites{
        Sprite{.key = "one", .x = 8, .y = 8, .atlas = atlas, .tile = 0, .palette = palette}};

    const std::vector<Rgba8> plain    = r.captureViewport(spriteFrame(sprites, std::nullopt, 1));
    const std::vector<Rgba8> declared = r.captureViewport(spriteFrame(sprites, 4, 3));
    ASSERT_EQ(plain.size(), declared.size());
    EXPECT_EQ(std::memcmp(plain.data(), declared.data(), plain.size() * sizeof(Rgba8)), 0);
}

}  // namespace
}  // namespace retropp
