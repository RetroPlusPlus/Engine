// ENG-2.B.2.a manual runtime demo — the smallest real host that exercises the live
// platform + tile-compositor + blit path: open a window, upload a procedural tile atlas,
// composite a continuously-scrolling tile layer into the internal viewport, blit it integer-
// scaled and letterboxed onto the swapchain at display refresh, and route keyboard + gamepad
// input through to the tick callback. Run it on a dev machine and confirm: the window shows a
// scrolling tile pattern (a centred rect on black bars), resizing re-letterboxes it, the
// close button quits, and pressing a mapped button prints a line.
//
// This is also the only target that instantiates SdlPlatform + Renderer in a real run, so it
// keeps the live SDL_GPU pipeline/upload/present path compiling and linking on every CI
// platform even though CI never opens the window.

// Take ownership of main(): SDL's header would otherwise redirect main → SDL_main and
// expect SDL's entry shim. We init SDL ourselves inside SdlPlatform.
#define SDL_MAIN_HANDLED
#include <SDL3/SDL_main.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <span>
#include <utility>
#include <vector>

#include "gbcpp/clock.h"
#include "gbcpp/draw_state.h"
#include "gbcpp/input.h"
#include "gbcpp/renderer.h"
#include "gbcpp/run_loop.h"
#include "gbcpp/sdl_platform.h"
#include "gbcpp/windowed_host.h"

namespace {

using namespace gbcpp;

constexpr int kTile      = 8;  // GB tile edge
constexpr int kAtlasCols = 2;  // 2×2-tile atlas → tiles 0..3
constexpr int kAtlasRows = 2;
constexpr int kMapW      = 16; // tilemap dimensions in tiles (wraps under scroll)
constexpr int kMapH      = 16;

// Paint one 8×8 atlas tile a solid RGBA colour. The atlas is row-major RGBA8, laid out as a
// kAtlasCols×kAtlasRows grid of tiles; tile index t lives at grid (t % cols, t / cols).
void paintTile(std::vector<std::uint8_t>& rgba, int atlasW, int tileIndex,
               std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    const int col = tileIndex % kAtlasCols;
    const int row = tileIndex / kAtlasCols;
    for (int y = 0; y < kTile; ++y) {
        for (int x = 0; x < kTile; ++x) {
            const int px = (col * kTile + x);
            const int py = (row * kTile + y);
            const std::size_t o = (static_cast<std::size_t>(py) * atlasW + px) * 4;
            rgba[o + 0] = r;
            rgba[o + 1] = g;
            rgba[o + 2] = b;
            rgba[o + 3] = 255;
        }
    }
}

}  // namespace

int main() {
    SDL_SetMainReady();

    SteadyClock clock;
    RunLoop     loop{clock};
    SdlPlatform platform{{.title = "GBCPP — ENG-2.B.2.a tile compositor demo"}};
    Renderer    renderer{platform.device(), platform.window()};

    // Build + upload a 2×2-tile atlas: red / green / blue / yellow.
    const int atlasW = kAtlasCols * kTile;
    const int atlasH = kAtlasRows * kTile;
    std::vector<std::uint8_t> atlasPixels(static_cast<std::size_t>(atlasW) * atlasH * 4, 0);
    paintTile(atlasPixels, atlasW, 0, 220,  60,  60);  // red
    paintTile(atlasPixels, atlasW, 1,  60, 200,  90);  // green
    paintTile(atlasPixels, atlasW, 2,  70, 120, 220);  // blue
    paintTile(atlasPixels, atlasW, 3, 230, 210,  70);  // yellow
    const AtlasId atlas = renderer.uploadAtlas(atlasPixels.data(), atlasW, atlasH);

    // A 16×16-tile map cycling the four atlas tiles in a checker-ish pattern. Kept alive for
    // the program's duration; the draw layer's TileContent.cells references it each frame.
    std::vector<TileCell> cells(static_cast<std::size_t>(kMapW) * kMapH);
    for (int y = 0; y < kMapH; ++y) {
        for (int x = 0; x < kMapW; ++x) {
            cells[static_cast<std::size_t>(y) * kMapW + x].tile =
                static_cast<std::uint16_t>((x ^ y) & 3);
        }
    }

    constexpr std::array<std::pair<Button, const char*>, kButtonCount> kLabels{{
        {Button::Up, "Up"}, {Button::Down, "Down"}, {Button::Left, "Left"},
        {Button::Right, "Right"}, {Button::A, "A"}, {Button::B, "B"},
        {Button::Start, "Start"}, {Button::Select, "Select"},
    }};

    auto familyName = [](ControllerType t) {
        switch (t) {
            case ControllerType::Xbox:        return "Xbox";
            case ControllerType::PlayStation: return "PlayStation";
            case ControllerType::Nintendo:    return "Nintendo";
            case ControllerType::Standard:    return "Standard";
            default:                          return "none";
        }
    };

    ControllerType lastType = ControllerType::Unknown;
    loop.setTick([&](const InputState& in) {
        if (platform.controllerType() != lastType) {  // auto-detected on connect
            lastType = platform.controllerType();
            std::printf("controller: %s\n", familyName(lastType));
        }
        for (const auto& [button, name] : kLabels) {
            if (in.justPressed(button))  std::printf("press   %s\n", name);
            if (in.justReleased(button)) std::printf("release %s\n", name);
        }
    });

    // The game owns the draw state; the render callback rebuilds + scrolls it each advance()
    // (the ENG-1 render-callback contract is unchanged at void(float)).
    FrameDrawState frame;
    int scrollX = 0;
    int scrollY = 0;
    loop.setRender([&](float alpha) {
        frame.layers.clear();
        DrawLayer layer{};
        layer.id      = LayerId{0};
        layer.z       = 0;
        layer.size    = PixelSize{160, 144};
        layer.scroll  = LayerScroll{scrollX, scrollY};
        layer.alpha   = 1.0f;
        layer.content = TileContent{atlas, kMapW, kMapH, std::span<const TileCell>(cells)};
        frame.layers.push_back(std::move(layer));

        renderer.renderFrame(frame, alpha);

        ++scrollX;                    // continuous horizontal scroll
        if ((scrollX & 1) == 0) ++scrollY;  // gentle diagonal drift
    });

    std::printf("ENG-2.B.2.a tile compositor demo — close the window to quit.\n");
    WindowedHost host{loop, platform};
    host.run();
    return 0;
}
