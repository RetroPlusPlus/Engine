// RNG display demo (ENG-3.B) — a window showing a decimal byte produced by a REAL SM83 routine
// running in the engine's VM host. It registers BOTH Game Boy RNG presets — retropp::sameboy::divRng
// (a raw rDIV read) and dualSeedRng (a dual-seed hardware RNG) — rolls a fresh byte every ~2 s with
// the active one, and shows the rolled value plus the active RNG's name ("rDivRng" / "SeedRng") in a
// hand-built tile font. Press X (or the pad's south face button) to switch RNGs. The engine embeds the
// routine bytes — NO ROM, no address, no register idiom at the call site.
//
// The VM's free-running divider is advanced one frame's worth of cycles per engine tick, so rDIV
// keeps ticking between rolls exactly as on always-running hardware — without that, a hardware RNG
// degenerates into a slow counter.
//
// Built on every CI platform (so the VM host + tile path keep compiling against the live engine);
// never run in CI (no display). Calm by design: one value change every ~2 s, no strobing.

// Take ownership of main(): SDL's header would otherwise redirect main → SDL_main.
#define SDL_MAIN_HANDLED
#include <SDL3/SDL_main.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "retropp/clock.h"
#include "retropp/draw_state.h"
#include "retropp/engine_config.h"
#include "retropp/gb_routines.h"
#include "retropp/input.h"
#include "retropp/input_actions.h"
#include "retropp/palette.h"
#include "retropp/renderer.h"
#include "retropp/run_loop.h"
#include "retropp/sdl_platform.h"
#include "retropp/vm.h"
#include "retropp/windowed_host.h"

using namespace retropp;

namespace {

constexpr int kTile = 8;

// The demo's input vocabulary: one action, switching the active RNG.
enum class Action : std::uint8_t { SwitchRng };

// The glyphs the demo can draw: a leading space (blank tile 0), the digits, and the letters used in
// the labels "rDivRng" / "SeedRng". A character's tile index is its position in this string.
constexpr std::string_view kGlyphs = " 0123456789DRivrngSed";

// A 5×7 font: 7 rows per glyph, low 5 bits per row (bit 4 = leftmost column). One entry per glyph in
// kGlyphs order (the leading space is all-blank).
constexpr std::array<std::array<std::uint8_t, 7>, 21> kFont{{
    {0,0,0,0,0,0,0},                                              // ' '
    {0b01110,0b10001,0b10011,0b10101,0b11001,0b10001,0b01110},    // 0
    {0b00100,0b01100,0b00100,0b00100,0b00100,0b00100,0b01110},    // 1
    {0b01110,0b10001,0b00001,0b00010,0b00100,0b01000,0b11111},    // 2
    {0b11111,0b00010,0b00100,0b00010,0b00001,0b10001,0b01110},    // 3
    {0b00010,0b00110,0b01010,0b10010,0b11111,0b00010,0b00010},    // 4
    {0b11111,0b10000,0b11110,0b00001,0b00001,0b10001,0b01110},    // 5
    {0b00110,0b01000,0b10000,0b11110,0b10001,0b10001,0b01110},    // 6
    {0b11111,0b00001,0b00010,0b00100,0b01000,0b01000,0b01000},    // 7
    {0b01110,0b10001,0b10001,0b01110,0b10001,0b10001,0b01110},    // 8
    {0b01110,0b10001,0b10001,0b01111,0b00001,0b00010,0b01100},    // 9
    {0b11110,0b10001,0b10001,0b10001,0b10001,0b10001,0b11110},    // D
    {0b11110,0b10001,0b10001,0b11110,0b10100,0b10010,0b10001},    // R
    {0b00100,0b00000,0b01100,0b00100,0b00100,0b00100,0b01110},    // i
    {0b00000,0b00000,0b10001,0b10001,0b10001,0b01010,0b00100},    // v
    {0b00000,0b00000,0b10110,0b11000,0b10000,0b10000,0b10000},    // r
    {0b00000,0b00000,0b10110,0b11001,0b10001,0b10001,0b10001},    // n
    {0b00000,0b01111,0b10001,0b10001,0b01111,0b00001,0b01110},    // g
    {0b01111,0b10000,0b10000,0b01110,0b00001,0b00001,0b11110},    // S
    {0b00000,0b00000,0b01110,0b10001,0b11111,0b10000,0b01110},    // e
    {0b00001,0b00001,0b00001,0b01111,0b10001,0b10001,0b01111},    // d
}};

// Tile index for a character (0 = blank for anything unsupported).
int tileFor(char c) {
    const std::size_t pos = kGlyphs.find(c);
    return pos == std::string_view::npos ? 0 : static_cast<int>(pos);
}

// Build an indexed atlas: one 8×8 tile per glyph, index 1 = lit, index 0 = background. The 5×7 glyph
// sits with a 1-px left and 1-px top margin.
std::vector<std::uint8_t> buildFontAtlas(int& tilesOut) {
    const int tiles = static_cast<int>(kGlyphs.size());
    tilesOut = tiles;
    const int width = kTile * tiles;
    std::vector<std::uint8_t> atlas(static_cast<std::size_t>(width) * kTile, 0);
    for (int g = 0; g < tiles; ++g) {
        for (int row = 0; row < 7; ++row) {
            const std::uint8_t bits = kFont[static_cast<std::size_t>(g)][static_cast<std::size_t>(row)];
            for (int col = 0; col < 5; ++col) {
                if ((bits >> (4 - col)) & 1) {
                    atlas[static_cast<std::size_t>(1 + row) * width + (g * kTile + 1 + col)] = 1;
                }
            }
        }
    }
    return atlas;
}

constexpr int kMapW = 20, kMapH = 18;

}  // namespace

int main() {
    SDL_SetMainReady();

    const EngineConfig config{
        .identity = {.organization = "Retro++", .application = "RNG Display Demo"},
        .window = {.title = "Retro++ — VM host RNG (X switches RNG)"}};

    EngineConfig::setActive(config);  // make it the active config — the bare ctors below inherit it
    SteadyClock clock;
    RunLoop     loop{clock};
    SdlPlatform platform;
    Renderer    renderer{platform.device(), platform.window()};

    ActionMap map{
        {Action::SwitchRng, {SDL_SCANCODE_X, PadButton::FaceSouth}},
    };
    platform.actions(map);

    int atlasTiles = 0;
    const std::vector<std::uint8_t> atlas = buildFontAtlas(atlasTiles);
    const AtlasId atlasId = renderer.uploadAtlas(atlas.data(), kTile * atlasTiles, kTile);
    const std::array<Rgba8, 4> colours{{{16, 18, 28}, {120, 230, 140}, {0, 0, 0}, {0, 0, 0}}};
    const PaletteId pal = renderer.uploadPalette(std::span<const Rgba8>(colours));

    // Every cell draws from the font sheet through the one palette — named directly per cell. drawText only
    // rewrites `.tile`, so the sheet + palette are set once here and left untouched.
    std::vector<TileCell> cells(static_cast<std::size_t>(kMapW) * kMapH,
                                TileCell{.atlas = atlasId, .palette = pal});
    auto clearCells = [&] {
        for (auto& c : cells) c.tile = 0;  // tile 0 = blank
    };
    auto drawText = [&](int col, int row, std::string_view text) {
        for (std::size_t i = 0; i < text.size() && col + static_cast<int>(i) < kMapW; ++i) {
            cells[static_cast<std::size_t>(row) * kMapW + col + static_cast<int>(i)].tile =
                static_cast<std::uint16_t>(tileFor(text[i]));
        }
    };
    auto drawCentered = [&](int row, std::string_view text) {
        drawText((kMapW - static_cast<int>(text.size())) / 2, row, text);
    };
    clearCells();

    // The VM host: create a machine and register BOTH RNG presets (engine-embedded — no ROM); the
    // SwitchRng action toggles which one is active.
    Vm vm{VMPlatform::GameBoyColor};
    auto divr = sameboy::divRng(vm);
    auto seed = sameboy::dualSeedRng(vm);

    bool useSeed = true;  // start on dualSeedRng
    auto activeName = [&]() -> std::string_view { return useSeed ? "SeedRng" : "rDivRng"; };
    auto rollActive = [&]() -> std::uint8_t { return useSeed ? seed() : divr(); };

    auto redraw = [&](std::uint8_t value) {
        clearCells();
        drawCentered(6, activeName());            // the active RNG's name
        drawCentered(10, std::to_string(value));  // the rolled byte
    };
    std::uint8_t current = rollActive();
    redraw(current);

    FrameDrawState frame;
    frame.layers.push_back(DrawLayer{.key = "rngDisplay"});
    DrawLayer& layer = frame.layers[0];
    layer.z       = 0;
    layer.size    = PixelSize{config.viewport.width, config.viewport.height};
    layer.content = TileContent{.widthInTiles = kMapW, .heightInTiles = kMapH,
                                .cells = std::span<const TileCell>(cells)};

    // Advance the divider one tick's worth of cycles per tick (rDIV free-runs with engine time). The
    // timing profile already defines this — no hardcoded cycle count.
    const std::uint64_t cyclesPerTick = config.timing.cpuCyclesPerTick();

    // "Every 2 seconds" expressed as a duration; the profile converts it to a tick count.
    const std::uint64_t ticksPerRoll = config.timing.ticksForDuration(std::chrono::seconds{2});
    std::uint64_t tick = 0;
    loop.setTick([&](const InputState& in) {
        vm.advanceClock(cyclesPerTick);

        if (in.justPressed(Action::SwitchRng)) {  // switch RNG, re-roll immediately with the new one
            useSeed = !useSeed;
            tick = 0;
            current = rollActive();
            redraw(current);
            std::printf("switched to %s\n", std::string(activeName()).c_str());
        }

        if (++tick >= ticksPerRoll) {
            tick = 0;
            current = rollActive();
            redraw(current);
            std::printf("%s roll: %u\n", std::string(activeName()).c_str(),
                        static_cast<unsigned>(current));
        }
    });
    loop.setRender([&]() { renderer.renderFrame(frame); });

    std::printf("VM host RNG demo — a real SM83 routine rolls a byte every ~2 s, shown with the active "
                "RNG's name. Press X (pad south) to switch between SeedRng and rDivRng. Close to quit.\n");
    WindowedHost{loop, platform}.run();
    return 0;
}
