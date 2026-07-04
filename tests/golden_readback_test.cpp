// Golden-readback harness: compose a fixed battery of representative frames OFFSCREEN on a real GPU
// device, download the composed viewport, and diff it pixel-exactly against a committed per-backend
// golden. It is the engine's first device-backed test — every other test exercises the format-
// independent constexpr CPU mirrors and creates no device. It is compose-only and windowless (it
// builds a Renderer with no window, so it needs a GPU device but no display), which lets it run on a
// software rasterizer in CI — lavapipe (Vulkan) on Linux, WARP (D3D12) on Windows, Metal on the Mac.
//
// Each backend commits its own golden under tests/fixtures/golden/<backend>/. Set the environment
// variable RETROPP_CAPTURE_GOLDEN to (re)capture the goldens for the live backend — the first run on
// each platform writes them; every later run compares. A scene's tolerance tag is exact (arithmetic-
// free relocations / selections / constant writes) or within one 8-bit step per channel (the blend and
// feather composites, where per-pass rounding can shift a value by at most one).
//
// A device is REQUIRED on every production-representative platform (macOS/Metal, Windows-x64/D3D12,
// Linux/Vulkan): if none is reachable the test FAILS with the SDL reason rather than skipping. The one
// exception is Windows on ARM — a courtesy coverage runner with no production-representative GPU backend
// in CI (its real path, D3D12 + DXIL, is pixel-covered by the Windows x64 job), where a missing device
// is a documented out-of-scope skip. The only other skip is the transient capture window — a backend
// whose golden is not committed yet — which the capture run then closes.

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "retropp/curve.h"
#include "retropp/draw_state.h"
#include "retropp/geometry.h"
#include "retropp/palette.h"
#include "retropp/renderer.h"
#include "retropp/viewport.h"

namespace {
using namespace retropp;

// A small, pitch-friendly viewport: 64 px wide → 256-byte rows, aligned for backend texture transfer.
constexpr int kW = 64;
constexpr int kH = 64;

// Per-scene comparison tolerance. Exact = byte-identical; OneStep = each channel within one 8-bit step
// (the blend / feather composites round per pass). In this 8-bit pipeline both pass exactly; the tag is
// the documented bar each scene is held to.
enum class Tol { Exact, OneStep };

// The CPU arch this binary was built for. A software rasterizer (lavapipe / WARP) is LLVM-codegen'd
// per arch, so its byte output can differ between x64 and ARM64; the golden key includes the arch so
// each runner owns its own exact golden rather than comparing across architectures.
const char* archTag() {
#if defined(__aarch64__) || defined(_M_ARM64)
    return "arm64";
#elif defined(__x86_64__) || defined(_M_X64) || defined(__amd64__)
    return "x64";
#else
    return "unknown-arch";
#endif
}

// The live backend's fixture key: "<backend>-<arch>" (e.g. metal-arm64, vulkan-x64, d3d12-x64). SDL
// reports the GPU driver name; one key per (backend, arch) = one committed golden per runner.
std::string backendKey(const char* driver) {
    std::string b = "unknown";
    if (driver) {
        const std::string d = driver;
        if (d == "metal") b = "metal";
        else if (d == "vulkan") b = "vulkan";
        else if (d == "direct3d12") b = "d3d12";
        else b = d;  // an unmapped backend keeps its own key rather than colliding with a mapped one
    }
    return b + "-" + archTag();
}

// Windows on ARM is a courtesy coverage runner: in a VM it has no production-representative GPU backend
// (a Windows-ARM guest gets no D3D12 adapter, and SDL_GPU has no software fallback), and a real
// Windows-ARM device's production path is D3D12 with arch-independent DXIL — the same backend + the same
// shader bytecode the Windows x64 job already pixel-validates. So when no device is reachable HERE, the
// golden test is out of scope by design (a documented, visible skip). On every production-representative
// platform (macOS/Metal, Windows-x64/D3D12, Linux/Vulkan) a missing device is a hard failure. A real
// device on this platform (a future hardware runner) still runs the test normally.
#if defined(_WIN32) && (defined(_M_ARM64) || defined(__aarch64__))
inline constexpr bool kDeviceOptional = true;
#else
inline constexpr bool kDeviceOptional = false;
#endif

std::filesystem::path goldenPath(const std::string& backend, const std::string& scene) {
    return std::filesystem::path(RETROPP_FIXTURES_DIR) / "golden" / backend / (scene + ".rgba");
}

// .rgba blob: 'RGBA' magic + width + height (uint32 LE) + width·height packed Rgba8 texels.
void writeBlob(const std::filesystem::path& p, int w, int h, const std::vector<Rgba8>& px) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    const std::uint32_t W = static_cast<std::uint32_t>(w);
    const std::uint32_t H = static_cast<std::uint32_t>(h);
    f.write("RGBA", 4);
    f.write(reinterpret_cast<const char*>(&W), sizeof(W));
    f.write(reinterpret_cast<const char*>(&H), sizeof(H));
    f.write(reinterpret_cast<const char*>(px.data()),
            static_cast<std::streamsize>(px.size() * sizeof(Rgba8)));
}

bool readBlob(const std::filesystem::path& p, int& w, int& h, std::vector<Rgba8>& px) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    char magic[4] = {};
    f.read(magic, 4);
    if (std::memcmp(magic, "RGBA", 4) != 0) return false;
    std::uint32_t W = 0, H = 0;
    f.read(reinterpret_cast<char*>(&W), sizeof(W));
    f.read(reinterpret_cast<char*>(&H), sizeof(H));
    w = static_cast<int>(W);
    h = static_cast<int>(H);
    px.resize(static_cast<std::size_t>(W) * static_cast<std::size_t>(H));
    f.read(reinterpret_cast<char*>(px.data()), static_cast<std::streamsize>(px.size() * sizeof(Rgba8)));
    return static_cast<bool>(f);
}

::testing::AssertionResult compareGolden(const std::vector<Rgba8>& got,
                                         const std::vector<Rgba8>& want, int w, Tol tol) {
    if (got.size() != want.size()) {
        return ::testing::AssertionFailure()
               << "pixel count mismatch — got " << got.size() << ", want " << want.size();
    }
    const int maxDelta = (tol == Tol::Exact) ? 0 : 1;
    for (std::size_t i = 0; i < got.size(); ++i) {
        const Rgba8& a = got[i];
        const Rgba8& b = want[i];
        const int dr = std::abs(static_cast<int>(a.r) - static_cast<int>(b.r));
        const int dg = std::abs(static_cast<int>(a.g) - static_cast<int>(b.g));
        const int db = std::abs(static_cast<int>(a.b) - static_cast<int>(b.b));
        const int da = std::abs(static_cast<int>(a.a) - static_cast<int>(b.a));
        if (dr > maxDelta || dg > maxDelta || db > maxDelta || da > maxDelta) {
            const int x = static_cast<int>(i) % w;
            const int y = static_cast<int>(i) / w;
            return ::testing::AssertionFailure()
                   << "first mismatch at (" << x << ", " << y << "): got "
                   << static_cast<int>(a.r) << "," << static_cast<int>(a.g) << ","
                   << static_cast<int>(a.b) << "," << static_cast<int>(a.a) << "  want "
                   << static_cast<int>(b.r) << "," << static_cast<int>(b.g) << ","
                   << static_cast<int>(b.b) << "," << static_cast<int>(b.a)
                   << "  (allowed per-channel delta " << maxDelta << ")";
        }
    }
    return ::testing::AssertionSuccess();
}

// A small deterministic indexed atlas (16×16 px = a 2×2 tile grid, indices 0..3 in a blocky pattern)
// and a 4-colour palette. Shared by every scene so the composite is identical art across the battery.
struct BaseArt {
    AtlasId   atlas{};
    PaletteId palette{};
};

BaseArt uploadBaseArt(Renderer& r) {
    std::array<std::uint8_t, 16 * 16> idx{};
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            idx[static_cast<std::size_t>(y) * 16 + static_cast<std::size_t>(x)] =
                static_cast<std::uint8_t>(((x / 4) + (y / 4)) % 4);
        }
    }
    const AtlasId atlas = r.uploadAtlas(idx.data(), 16, 16);  // no structural hole (default None)
    const std::array<Rgba8, 4> pal{{{20, 20, 30}, {200, 60, 60}, {60, 200, 90}, {230, 230, 240}}};
    const PaletteId palette = r.uploadPalette(std::span<const Rgba8>(pal));
    return {atlas, palette};
}

// Backing storage a scene's layer spans point into — must outlive the captureViewport call, so the
// caller owns these and passes them by reference.
struct SceneBacking {
    std::vector<PaletteId> palSet;
    std::vector<TileCell>  cells;
    std::vector<Sprite>    sprites;
};

// A tile layer (z 0, 8×8 tiles filling the viewport) + a sprite layer (z 10, two opaque sprites over
// index-0 holes). The base composite every scene starts from.
void addBaseScene(FrameDrawState& frame, const BaseArt& art, SceneBacking& b) {
    b.cells.resize(8 * 8);
    for (int ty = 0; ty < 8; ++ty) {
        for (int tx = 0; tx < 8; ++tx) {
            b.cells[static_cast<std::size_t>(ty) * 8 + static_cast<std::size_t>(tx)] =
                TileCell{.atlas = art.atlas,
                         .tile = static_cast<std::uint16_t>((tx + ty) % 4), .palette = art.palette};
        }
    }
    DrawLayer bg{.key = "bg"};
    bg.z       = 0;
    bg.size    = PixelSize{kW, kH};
    bg.content = TileContent{.widthInTiles = 8, .heightInTiles = 8,
                             .cells = std::span<const TileCell>(b.cells)};
    frame.layers.push_back(bg);

    b.sprites = {Sprite{.key = "sp0", .x = 12, .y = 20, .atlas = art.atlas, .tile = 1, .palette = art.palette},
                 Sprite{.key = "sp1", .x = 40, .y = 36, .atlas = art.atlas, .tile = 3, .palette = art.palette}};
    DrawLayer sp{.key = "sprites"};
    sp.z       = 10;
    sp.size    = PixelSize{kW, kH};
    sp.content = SpriteContent{.sprites = std::span<const Sprite>(b.sprites)};
    frame.layers.push_back(sp);
}

// A tile-only background (z 0, 8×8 tiles filling the viewport) — the base the crisp-parity scenes layer a
// single destination-driven path on top of. No sprites: transformed-sprite crispness is a separate scope, so the
// parity of the destination-driven paths is measured against a background that already upscales cleanly.
void addTileBackground(FrameDrawState& frame, const BaseArt& art, SceneBacking& b) {
    b.cells.resize(8 * 8);
    for (int ty = 0; ty < 8; ++ty) {
        for (int tx = 0; tx < 8; ++tx) {
            b.cells[static_cast<std::size_t>(ty) * 8 + static_cast<std::size_t>(tx)] =
                TileCell{.atlas = art.atlas,
                         .tile = static_cast<std::uint16_t>((tx + ty) % 4), .palette = art.palette};
        }
    }
    DrawLayer bg{.key = "bg"};
    bg.z       = 0;
    bg.size    = PixelSize{kW, kH};
    bg.content = TileContent{.widthInTiles = 8, .heightInTiles = 8,
                             .cells = std::span<const TileCell>(b.cells)};
    frame.layers.push_back(bg);
}

// Nearest-upscale a captured image by an integer factor: each source pixel becomes a scale×scale block.
// The reference the crisp-parity scenes compare a scaled compose against.
std::vector<Rgba8> nearestUpscale(const std::vector<Rgba8>& src, int w, int h, int scale) {
    const int ow = w * scale;
    std::vector<Rgba8> out(static_cast<std::size_t>(ow) * static_cast<std::size_t>(h * scale));
    for (int y = 0; y < h * scale; ++y) {
        for (int x = 0; x < ow; ++x) {
            out[static_cast<std::size_t>(y) * ow + x] =
                src[static_cast<std::size_t>(y / scale) * w + (x / scale)];
        }
    }
    return out;
}

// The crisp invariant, machine-checked: a Viewport-grid capture at compose `scale` must equal the scale-1
// capture nearest-upscaled, byte-for-byte. Every destination-driven analytic path snaps to the viewport
// grid, so scaling the compose only relocates whole viewport pixels onto the finer output grid — no
// per-output-pixel softening. Exact equality — no tolerance; a non-exact path is a defect, not a threshold.
void runCrispParity(const std::string& name, const FrameDrawState& frame, Renderer& r, int scale = 3) {
    r.setEvaluationGrid(EvaluationGrid::Viewport);
    const std::vector<Rgba8> one    = r.captureViewport(frame, 1);
    const std::vector<Rgba8> scaled = r.captureViewport(frame, scale);
    const std::vector<Rgba8> want   = nearestUpscale(one, kW, kH, scale);
    ASSERT_EQ(scaled.size(), want.size()) << name;
    EXPECT_TRUE(compareGolden(scaled, want, kW * scale, Tol::Exact)) << name;
}

class GoldenReadback : public ::testing::Test {
protected:
    static inline SDL_GPUDevice* device_ = nullptr;
    static inline std::string    backend_;
    static inline std::string    initError_;

    static void SetUpTestSuite() {
        // The GPU subsystem needs the video subsystem initialized (mirrors SdlPlatform). A headless
        // machine needs an offscreen video driver (SDL_VIDEODRIVER=offscreen) + an installed GPU device
        // (a software rasterizer is fine). A failure here is recorded and surfaced as a hard test
        // failure in SetUp with the SDL reason — the harness REQUIRES a device, it does not skip past one.
        if (!SDL_Init(SDL_INIT_VIDEO)) {
            initError_ = std::string("SDL_Init(SDL_INIT_VIDEO) failed: ") + SDL_GetError();
            return;
        }
        device_ = SDL_CreateGPUDevice(
            SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_MSL,
            /*debug_mode=*/false, /*name=*/nullptr);
        if (!device_) {
            initError_ = std::string("SDL_CreateGPUDevice failed: ") + SDL_GetError();
            return;
        }
        backend_ = backendKey(SDL_GetGPUDeviceDriver(device_));
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
                                "backend in CI; its production path (D3D12 + DXIL) is pixel-covered by the "
                                "Windows x64 job. ("
                             << initError_ << ")";
            }
            FAIL() << "no GPU device reachable — " << initError_
                   << ". The golden harness requires a GPU device on every production-representative "
                      "platform (macOS/Metal, Windows-x64/D3D12, Linux/Vulkan); install a GPU driver (a "
                      "software rasterizer such as lavapipe suffices) and, on a headless runner, set "
                      "SDL_VIDEODRIVER=offscreen so SDL video init succeeds.";
        }
    }

    // Capture `frame` and either write the live backend's golden (capture mode) or diff against the
    // committed one. A missing golden skips with a parity-gap message rather than passing.
    void runScene(const std::string& name, Tol tol, const FrameDrawState& frame, Renderer& r) {
        const std::vector<Rgba8> px = r.captureViewport(frame);
        ASSERT_EQ(px.size(), static_cast<std::size_t>(kW) * static_cast<std::size_t>(kH));

        const std::filesystem::path path = goldenPath(backend_, name);
        if (std::getenv("RETROPP_CAPTURE_GOLDEN") != nullptr) {
            writeBlob(path, kW, kH, px);
            SUCCEED() << "captured golden " << path.string();
            return;
        }
        int gw = 0, gh = 0;
        std::vector<Rgba8> want;
        if (!readBlob(path, gw, gh, want)) {
            GTEST_SKIP() << "no committed golden for backend '" << backend_ << "' scene '" << name
                         << "' (" << path.string()
                         << ") — capture (RETROPP_CAPTURE_GOLDEN=1) + commit it to close the parity gap";
        }
        ASSERT_EQ(gw, kW);
        ASSERT_EQ(gh, kH);
        EXPECT_TRUE(compareGolden(px, want, kW, tol));
    }
};

// ── The battery ───────────────────────────────────────────────────────────────────────────────────

TEST_F(GoldenReadback, Default) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const BaseArt art = uploadBaseArt(r);
    FrameDrawState frame;
    SceneBacking b;
    addBaseScene(frame, art, b);
    runScene("default", Tol::Exact, frame, r);
}

TEST_F(GoldenReadback, Displace) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const BaseArt art = uploadBaseArt(r);
    FrameDrawState frame;
    SceneBacking b;
    addBaseScene(frame, art, b);
    frame.postEffects.push_back(ScreenSpaceEffect{.kind      = ScreenSpaceEffectKind::RowDisplacement,
                                                  .amplitude = 4.0f,
                                                  .frequency = 2.0f,
                                                  .phase     = 0.25f,
                                                  .axis      = Axis::Horizontal,
                                                  .edge      = DisplacementEdge::Blank});
    runScene("displace", Tol::Exact, frame, r);
}

TEST_F(GoldenReadback, Ripple) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const BaseArt art = uploadBaseArt(r);
    FrameDrawState frame;
    SceneBacking b;
    addBaseScene(frame, art, b);
    frame.postEffects.push_back(ScreenSpaceEffect{.kind      = ScreenSpaceEffectKind::Ripple,
                                                  .amplitude = 3.0f,
                                                  .frequency = 4.0f,
                                                  .phase     = 0.2f,
                                                  .center    = Point{32.0f, 32.0f},
                                                  .decay     = 1.5f});
    runScene("ripple", Tol::Exact, frame, r);
}

TEST_F(GoldenReadback, ColorfillSolid) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const BaseArt art = uploadBaseArt(r);
    FrameDrawState frame;
    SceneBacking b;
    addBaseScene(frame, art, b);
    Region reg{.key = "reg"};
    reg.shape   = ShapePoints::rectangle(Point{16, 16}, 32, 32);
    reg.effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill, .fill = Rgba8{255, 140, 0, 255}}};
    frame.regions.push_back(reg);
    runScene("colorfill_solid", Tol::Exact, frame, r);
}

TEST_F(GoldenReadback, RegionSelect) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const BaseArt art = uploadBaseArt(r);
    FrameDrawState frame;
    SceneBacking b;
    addBaseScene(frame, art, b);
    Region reg{.key = "reg"};
    reg.shape   = ShapePoints::triangle(Point{8, 8}, Point{56, 16}, Point{24, 52});
    reg.effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill, .fill = Rgba8{40, 120, 220, 255}}};
    frame.regions.push_back(reg);
    runScene("region_select", Tol::Exact, frame, r);
}

TEST_F(GoldenReadback, StencilHard) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const BaseArt art = uploadBaseArt(r);
    FrameDrawState frame;
    SceneBacking b;
    addBaseScene(frame, art, b);
    frame.regions = stencil(ShapePoints::circle(Point{32, 32}, 14), StencilMode::TransparentInside, 0.0f);
    runScene("stencil_hard", Tol::Exact, frame, r);
}

TEST_F(GoldenReadback, StencilFeather) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const BaseArt art = uploadBaseArt(r);
    FrameDrawState frame;
    SceneBacking b;
    addBaseScene(frame, art, b);
    frame.regions = stencil(ShapePoints::circle(Point{32, 32}, 14), StencilMode::TransparentInside, 6.0f);
    runScene("stencil_feather", Tol::OneStep, frame, r);
}

// One scene per BlendMode: a whole-frame ColorFill combined over the base composite with that mode.

TEST_F(GoldenReadback, BlendMultiply) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const BaseArt art = uploadBaseArt(r);
    FrameDrawState frame;
    SceneBacking b;
    addBaseScene(frame, art, b);
    frame.blend = BlendMode::Multiply;
    frame.postEffects.push_back(ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill, .fill = Rgba8{128, 128, 255, 255}});
    runScene("blend_multiply", Tol::OneStep, frame, r);
}

TEST_F(GoldenReadback, BlendScreen) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const BaseArt art = uploadBaseArt(r);
    FrameDrawState frame;
    SceneBacking b;
    addBaseScene(frame, art, b);
    frame.blend = BlendMode::Screen;
    frame.postEffects.push_back(ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill, .fill = Rgba8{80, 80, 120, 255}});
    runScene("blend_screen", Tol::OneStep, frame, r);
}

TEST_F(GoldenReadback, BlendAdd) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const BaseArt art = uploadBaseArt(r);
    FrameDrawState frame;
    SceneBacking b;
    addBaseScene(frame, art, b);
    frame.blend = BlendMode::Add;
    frame.postEffects.push_back(ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill, .fill = Rgba8{60, 40, 20, 255}});
    runScene("blend_add", Tol::OneStep, frame, r);
}

TEST_F(GoldenReadback, BlendSubtract) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const BaseArt art = uploadBaseArt(r);
    FrameDrawState frame;
    SceneBacking b;
    addBaseScene(frame, art, b);
    frame.blend = BlendMode::Subtract;
    frame.postEffects.push_back(ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill, .fill = Rgba8{60, 40, 20, 255}});
    runScene("blend_subtract", Tol::OneStep, frame, r);
}

TEST_F(GoldenReadback, BlendHalf) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const BaseArt art = uploadBaseArt(r);
    FrameDrawState frame;
    SceneBacking b;
    addBaseScene(frame, art, b);
    frame.blend = BlendMode::Half;
    frame.postEffects.push_back(ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill, .fill = Rgba8{200, 100, 50, 255}});
    runScene("blend_half", Tol::OneStep, frame, r);
}

TEST_F(GoldenReadback, CurveRegion) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const BaseArt art = uploadBaseArt(r);
    FrameDrawState frame;
    SceneBacking b;
    addBaseScene(frame, art, b);
    Curve c = Curve::quadratic(Vec2{12, 12}, Vec2{52, 8}, Vec2{52, 52});
    c.lineTo(Vec2{12, 52});  // fromCurve closes the loop back to the start
    Region reg{.key = "reg"};
    reg.shape   = ShapePoints::fromCurve(c);
    reg.effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill, .fill = Rgba8{200, 80, 160, 255}}};
    frame.regions.push_back(reg);
    runScene("curve_region", Tol::Exact, frame, r);
}

TEST_F(GoldenReadback, CurveStencil) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const BaseArt art = uploadBaseArt(r);
    FrameDrawState frame;
    SceneBacking b;
    addBaseScene(frame, art, b);
    Curve c = Curve::quadratic(Vec2{16, 16}, Vec2{48, 12}, Vec2{48, 48});
    c.quadraticTo(Vec2{16, 52}, Vec2{16, 16});
    frame.regions = stencil(ShapePoints::fromCurve(c), StencilMode::TransparentInside, 5.0f);
    runScene("curve_stencil", Tol::OneStep, frame, r);
}

// A Multiply ColorFill at fillIntensity > 1 BRIGHTENS the scene — a multiplicative exposure impossible at
// 8-bit (the fill would clamp to 1 before the blend, so Multiply could only darken). The float16
// intermediates carry the > 1 fill through to the blend. This proves the capability against a no-effect
// baseline of the same scene (always runs, no golden needed) AND pins the exact result with a committed
// golden.
TEST_F(GoldenReadback, MultiplyBrighten) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const BaseArt art = uploadBaseArt(r);

    // Baseline: the base composite with no whole-frame colour.
    FrameDrawState base;
    SceneBacking  bb;
    addBaseScene(base, art, bb);
    const std::vector<Rgba8> baseline = r.captureViewport(base);

    // The brighten scene: a white whole-frame ColorFill at fillIntensity 1.5, Multiply — scene · 1.5.
    FrameDrawState frame;
    SceneBacking   b;
    addBaseScene(frame, art, b);
    frame.blend = BlendMode::Multiply;
    frame.postEffects.push_back(ScreenSpaceEffect{
        .kind = ScreenSpaceEffectKind::ColorFill, .fill = Rgba8{255, 255, 255, 255}, .fillIntensity = 1.5f});
    const std::vector<Rgba8> brightened = r.captureViewport(frame);

    // Capability: no channel is darker than the baseline, and at least one is strictly brighter.
    ASSERT_EQ(baseline.size(), brightened.size());
    bool        anyBrighter = false;
    bool        anyDarker   = false;
    std::size_t darkerAt    = 0;
    for (std::size_t i = 0; i < baseline.size(); ++i) {
        if (brightened[i].r < baseline[i].r || brightened[i].g < baseline[i].g ||
            brightened[i].b < baseline[i].b) {
            anyDarker = true;
            darkerAt  = i;
            break;
        }
        if (brightened[i].r > baseline[i].r || brightened[i].g > baseline[i].g ||
            brightened[i].b > baseline[i].b) {
            anyBrighter = true;
        }
    }
    EXPECT_FALSE(anyDarker) << "Multiply at fillIntensity 1.5 darkened pixel " << darkerAt
                            << " — a multiplicative exposure must not dim the scene";
    EXPECT_TRUE(anyBrighter) << "Multiply ColorFill at fillIntensity 1.5 brightened no pixel — the float16 "
                                "headroom is not reaching the blend";

    // Regression: the exact composited result is pinned by a committed golden (arithmetic composite → OneStep).
    runScene("multiply_brighten", Tol::OneStep, frame, r);
}

// 90° rotation on the tile + sprite paths. The base tiles are asymmetric under a quarter turn, so a
// Rot90 layer must change the composed pixels (the always-runs capability check, no golden needed). The
// scene also drives a Rot90 sprite and a non-square (8×16) sprite at Rot270 — whose read transposes — so
// both GPU rotation paths are exercised, then pins the exact composite with a committed per-backend golden.
TEST_F(GoldenReadback, TileSpriteRotation) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const BaseArt art = uploadBaseArt(r);

    const auto buildTiles = [&](Rotation rot, SceneBacking& b, FrameDrawState& frame) {
        b.cells.resize(8 * 8);
        for (int ty = 0; ty < 8; ++ty) {
            for (int tx = 0; tx < 8; ++tx) {
                b.cells[static_cast<std::size_t>(ty) * 8 + static_cast<std::size_t>(tx)] =
                    TileCell{.atlas = art.atlas,
                             .tile = static_cast<std::uint16_t>((tx + ty) % 4), .palette = art.palette, .rotation = rot};
            }
        }
        DrawLayer bg{.key = "bg"};
        bg.z       = 0;
        bg.size    = PixelSize{kW, kH};
        bg.content = TileContent{.widthInTiles = 8, .heightInTiles = 8,
                                 .cells = std::span<const TileCell>(b.cells)};
        frame.layers.push_back(bg);
    };

    // Capability: an unrotated tile layer vs the same layer at Rot90 — the asymmetric tiles must differ.
    FrameDrawState none;
    SceneBacking   bn;
    buildTiles(Rotation::None, bn, none);
    const std::vector<Rgba8> unrotated = r.captureViewport(none);

    FrameDrawState rotated;
    SceneBacking   br;
    buildTiles(Rotation::Rot90, br, rotated);
    br.sprites = {
        Sprite{.key = "sp0", .x = 16, .y = 16, .atlas = art.atlas, .tile = 1, .palette = art.palette,
               .rotation = Rotation::Rot90},
        Sprite{.key = "sp1", .x = 40, .y = 24, .size = AssetDimensions::GameBoy8x16, .atlas = art.atlas,
               .tile = 0, .palette = art.palette, .rotation = Rotation::Rot270},
    };
    DrawLayer sp{.key = "sprites"};
    sp.z       = 10;
    sp.size    = PixelSize{kW, kH};
    sp.content = SpriteContent{.sprites = std::span<const Sprite>(br.sprites)};
    rotated.layers.push_back(sp);
    const std::vector<Rgba8> withRot = r.captureViewport(rotated);

    ASSERT_EQ(unrotated.size(), withRot.size());
    EXPECT_NE(0, std::memcmp(unrotated.data(), withRot.data(), unrotated.size() * sizeof(Rgba8)))
        << "Rot90 on asymmetric tiles left the composite unchanged — the shader rotation path is dead";

    // Regression: pin the exact rotated composite per backend (a quarter-turn relocation is arithmetic-free).
    runScene("tile_sprite_rotation", Tol::Exact, rotated, r);
}

// The harness has teeth: a 1-pixel scroll of the background changes the captured pixels. Proves the
// readback reflects the composed frame rather than returning a constant — it always runs (no golden
// needed), so the compare path's sensitivity is verified on every platform that has a device.
TEST_F(GoldenReadback, HarnessHasTeeth) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const BaseArt art = uploadBaseArt(r);

    FrameDrawState f1;
    SceneBacking b1;
    addBaseScene(f1, art, b1);
    const std::vector<Rgba8> a = r.captureViewport(f1);

    FrameDrawState f2;
    SceneBacking b2;
    addBaseScene(f2, art, b2);
    f2.layers[0].scroll = LayerScroll{1, 0};
    const std::vector<Rgba8> shifted = r.captureViewport(f2);

    ASSERT_EQ(a.size(), shifted.size());
    EXPECT_NE(0, std::memcmp(a.data(), shifted.data(), a.size() * sizeof(Rgba8)))
        << "a 1px scroll left the capture unchanged — the readback has no teeth";
}

// ── Crisp parity: every destination-driven path is byte-identical to the viewport-res upscale ────────
//
// These need no committed golden — they compare a scaled compose against the scale-1 compose nearest-
// upscaled, so they run on every platform with a device. One scene per destination-driven path.

// A Mode-7-style homography on the tile layer: the transform evaluates at the snapped (integer) content
// pixel, so the scale-3 compose is the scale-1 image blown up 3×.
TEST_F(GoldenReadback, CrispParityTransformTile) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const BaseArt art = uploadBaseArt(r);
    FrameDrawState frame;
    SceneBacking b;
    addTileBackground(frame, art, b);
    frame.layers[0].transform =
        Transform::rotation(15.0f, 32.0f, 32.0f).then(Transform::perspective(0.0f, 0.005f));
    runCrispParity("transform_tile", frame, r);
}

TEST_F(GoldenReadback, CrispParityRegionSelect) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const BaseArt art = uploadBaseArt(r);
    FrameDrawState frame;
    SceneBacking b;
    addTileBackground(frame, art, b);
    Region reg{.key = "reg"};
    reg.shape   = ShapePoints::triangle(Point{8, 8}, Point{56, 16}, Point{24, 52});
    reg.effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill, .fill = Rgba8{40, 120, 220, 255}}};
    frame.regions.push_back(reg);
    runCrispParity("region_select", frame, r);
}

TEST_F(GoldenReadback, CrispParityRegionStroke) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const BaseArt art = uploadBaseArt(r);
    FrameDrawState frame;
    SceneBacking b;
    addTileBackground(frame, art, b);
    Region reg{.key = "reg"};
    reg.shape             = ShapePoints::circle(Point{32, 32}, 18);
    reg.shape.strokeWidth = 5.0f;  // gate confined to the boundary band (the outline)
    reg.effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill, .fill = Rgba8{240, 200, 60, 255}}};
    frame.regions.push_back(reg);
    runCrispParity("region_stroke", frame, r);
}

TEST_F(GoldenReadback, CrispParityRegionStencil) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const BaseArt art = uploadBaseArt(r);
    FrameDrawState frame;
    SceneBacking b;
    addTileBackground(frame, art, b);
    frame.regions = stencil(ShapePoints::circle(Point{32, 32}, 14), StencilMode::TransparentInside, 0.0f);
    runCrispParity("region_stencil", frame, r);
}

TEST_F(GoldenReadback, CrispParityCurveRegion) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const BaseArt art = uploadBaseArt(r);
    FrameDrawState frame;
    SceneBacking b;
    addTileBackground(frame, art, b);
    Curve c = Curve::quadratic(Vec2{12, 12}, Vec2{52, 8}, Vec2{52, 52});
    c.lineTo(Vec2{12, 52});
    Region reg{.key = "reg"};
    reg.shape   = ShapePoints::fromCurve(c);
    reg.effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill, .fill = Rgba8{200, 80, 160, 255}}};
    frame.regions.push_back(reg);
    runCrispParity("curve_region", frame, r);
}

TEST_F(GoldenReadback, CrispParityCurveMaskRegion) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const BaseArt art = uploadBaseArt(r);
    FrameDrawState frame;
    SceneBacking b;
    addTileBackground(frame, art, b);
    const std::array<Vec2, 4> pts{{{16, 14}, {50, 20}, {44, 50}, {18, 46}}};
    const Curve c = Curve::throughPoints(std::span<const Vec2>(pts), /*closed=*/true);  // cubic → mask path
    Region reg{.key = "reg"};
    reg.shape   = r.bakeCurveRegion(c);
    reg.effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill, .fill = Rgba8{90, 210, 180, 255}}};
    frame.regions.push_back(reg);
    runCrispParity("curve_mask_region", frame, r);
}

TEST_F(GoldenReadback, CrispParityDisplace) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const BaseArt art = uploadBaseArt(r);
    FrameDrawState frame;
    SceneBacking b;
    addTileBackground(frame, art, b);
    frame.postEffects.push_back(ScreenSpaceEffect{.kind      = ScreenSpaceEffectKind::RowDisplacement,
                                                  .amplitude = 4.0f,
                                                  .frequency = 2.0f,
                                                  .phase     = 0.25f,
                                                  .axis      = Axis::Horizontal,
                                                  .edge      = DisplacementEdge::Blank});
    runCrispParity("displace", frame, r);
}

TEST_F(GoldenReadback, CrispParityRipple) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const BaseArt art = uploadBaseArt(r);
    FrameDrawState frame;
    SceneBacking b;
    addTileBackground(frame, art, b);
    frame.postEffects.push_back(ScreenSpaceEffect{.kind      = ScreenSpaceEffectKind::Ripple,
                                                  .amplitude = 3.0f,
                                                  .frequency = 4.0f,
                                                  .phase     = 0.2f,
                                                  .center    = Point{32.0f, 32.0f},
                                                  .decay     = 1.5f});
    runCrispParity("ripple", frame, r);
}

// A custom shader is crisp with ZERO shader change: the generated entry point hands its main() the
// viewport-cell centre and sampleSource() quantizes the requested displacement — this scene drives both
// (a spatially-varying sample displacement + procedural tint math) through an unmodified shader file.
TEST_F(GoldenReadback, CrispParityCustomWave) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const BaseArt art = uploadBaseArt(r);
    const PostProcessStageId probe = r.registerPostProcessStage("tests/shaders/wave_probe.frag.hlsl");
    FrameDrawState frame;
    SceneBacking b;
    addTileBackground(frame, art, b);
    frame.postEffects.push_back(ScreenSpaceEffect{.kind         = ScreenSpaceEffectKind::Custom,
                                                  .customShader = probe,
                                                  .wobble       = 0.05f,
                                                  .bands        = 5.0f});
    runCrispParity("custom_wave", frame, r);
}

// The same shader with the displacement off: sampleSource(uv) is the identity request (the fragment
// samples its own true uv), isolating the procedural-math half of the contract.
TEST_F(GoldenReadback, CrispParityCustomProcedural) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const BaseArt art = uploadBaseArt(r);
    const PostProcessStageId probe = r.registerPostProcessStage("tests/shaders/wave_probe.frag.hlsl");
    FrameDrawState frame;
    SceneBacking b;
    addTileBackground(frame, art, b);
    frame.postEffects.push_back(ScreenSpaceEffect{.kind         = ScreenSpaceEffectKind::Custom,
                                                  .customShader = probe,
                                                  .wobble       = 0.0f,
                                                  .bands        = 9.0f});
    runCrispParity("custom_procedural", frame, r);
}

// ── Crisp parity: transformed sprites resolve coverage on the viewport grid ──────────────────
//
// A tile background + one sprite layer of geometrically-transformed sprites, whose analytic (Viewport-
// grid) coverage must upscale to an exact nearest-multiple of the scale-1 image. The base art is fully
// opaque, so parity is a clean coverage + colour test: every viewport cell inside the true quad writes
// its texel, every cell outside discards.

void addTransformedSpriteScene(FrameDrawState& frame, const BaseArt& art, SceneBacking& b,
                               const Transform& layer, const Transform& spriteXf) {
    addTileBackground(frame, art, b);
    b.sprites = {
        Sprite{.key = "sp0", .x = 24, .y = 20, .size = AssetDimensions{8, 8}, .atlas = art.atlas,
               .tile = 1, .palette = art.palette, .transform = spriteXf},
        Sprite{.key = "sp1", .x = 40, .y = 36, .size = AssetDimensions{8, 8}, .atlas = art.atlas,
               .tile = 3, .palette = art.palette, .transform = spriteXf},
    };
    DrawLayer sp{.key = "sprites"};
    sp.z        = 10;
    sp.size     = PixelSize{kW, kH};
    sp.content  = SpriteContent{.sprites = std::span<const Sprite>(b.sprites)};
    sp.transform = layer;
    frame.layers.push_back(sp);
}

TEST_F(GoldenReadback, CrispParitySpriteRotation) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const BaseArt art = uploadBaseArt(r);
    FrameDrawState frame;
    SceneBacking b;
    addTransformedSpriteScene(frame, art, b, Transform{}, Transform::rotation(30.0f, 4.0f, 4.0f));
    runCrispParity("sprite_rotation", frame, r);
}

TEST_F(GoldenReadback, CrispParitySpriteScale) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const BaseArt art = uploadBaseArt(r);
    FrameDrawState frame;
    SceneBacking b;
    addTransformedSpriteScene(frame, art, b, Transform{}, Transform::scale(2.5f, 1.75f, 4.0f, 4.0f));
    runCrispParity("sprite_scale", frame, r);
}

TEST_F(GoldenReadback, CrispParitySpritePerspective) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const BaseArt art = uploadBaseArt(r);
    FrameDrawState frame;
    SceneBacking b;
    const Transform xf =
        Transform::rotation(18.0f, 4.0f, 4.0f).then(Transform::perspective(0.01f, 0.006f));
    addTransformedSpriteScene(frame, art, b, Transform{}, xf);
    runCrispParity("sprite_perspective", frame, r);
}

TEST_F(GoldenReadback, CrispParitySpriteLayerTransform) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const BaseArt art = uploadBaseArt(r);
    FrameDrawState frame;
    SceneBacking b;
    // A per-layer transform composed over sprites that carry their OWN transforms — coverage of both.
    const Transform layer =
        Transform::rotation(12.0f, 32.0f, 32.0f).then(Transform::scale(1.3f, 1.1f, 32.0f, 32.0f));
    addTransformedSpriteScene(frame, art, b, layer, Transform::rotation(25.0f, 4.0f, 4.0f));
    runCrispParity("sprite_layer_transform", frame, r);
}

TEST_F(GoldenReadback, CrispParitySpriteFractional) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const BaseArt art = uploadBaseArt(r);
    FrameDrawState frame;
    SceneBacking b;
    // Sub-pixel placement via a fractional translation baked into the transform. Placement stays
    // sub-pixel; parity holds because both captures use the identical fractional transform.
    const Transform xf =
        Transform::rotation(15.0f, 4.0f, 4.0f).then(Transform::translation(0.5f, 0.25f));
    addTransformedSpriteScene(frame, art, b, Transform{}, xf);
    runCrispParity("sprite_fractional", frame, r);
}

}  // namespace
