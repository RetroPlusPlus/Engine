// Audio keyboard demo (ENG-4.A) — a window of on-screen buttons you play like a little keyboard.
//
// The d-pad and the A / B buttons each play a gentle triangle note (a C-major pentatonic: C D E G A,
// plus C an octave up on B). The pressed button lights up (a steady palette change — no flashing), and
// because each note is its OWN AudioSystem draining its own output stream, holding several buttons at
// once layers the notes into a chord — a direct showcase of the engine's "run as many independent
// audio systems as you like" model. No Vm, no register writes appear here: a system registers an audio
// asset and is cued by handle; the audio system hides everything underneath.
//
// (Why one AudioSystem per note: a faithful gentle triangle driver fills a VM's small code arena, so a
// system hosts one. That turns the limit into the feature — six independent systems = real polyphony.)
//
// Built on every CI platform so the audio + tile paths keep compiling against the live engine; never
// run in CI (no display, no audio device). Dev-run only, and photosensitivity-safe (static layout, the
// only change is a steady highlight while a key is held).

// Take ownership of main(): SDL's header would otherwise redirect main → SDL_main.
#define SDL_MAIN_HANDLED
#include <SDL3/SDL_main.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "gbcpp/audio_system.h"
#include "gbcpp/clock.h"
#include "gbcpp/draw_state.h"
#include "gbcpp/engine_config.h"
#include "gbcpp/input.h"
#include "gbcpp/palette.h"
#include "gbcpp/renderer.h"
#include "gbcpp/run_loop.h"
#include "gbcpp/sdl_platform.h"
#include "gbcpp/windowed_host.h"

using namespace gbcpp;

namespace {

constexpr int kTile = 8;
constexpr int kMapW = 20, kMapH = 18;

// Glyph atlas indices.
enum Glyph { GBlank, GUp, GDown, GLeft, GRight, GA, GB, GC, GD, GE, GG, GCount };

// A 5×7 font: 7 rows per glyph, low 5 bits per row (bit 4 = leftmost column). Direction arrows + the
// letters used as note names (C D E G A) and face-button labels (A B).
constexpr std::array<std::array<std::uint8_t, 7>, GCount> kFont{{
    {0, 0, 0, 0, 0, 0, 0},                                          // blank
    {0b00100, 0b01110, 0b10101, 0b00100, 0b00100, 0b00100, 0b00100},  // up arrow
    {0b00100, 0b00100, 0b00100, 0b00100, 0b10101, 0b01110, 0b00100},  // down arrow
    {0b00000, 0b00100, 0b01000, 0b11111, 0b01000, 0b00100, 0b00000},  // left arrow
    {0b00000, 0b00100, 0b00010, 0b11111, 0b00010, 0b00100, 0b00000},  // right arrow
    {0b01110, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001},  // A
    {0b11110, 0b10001, 0b10001, 0b11110, 0b10001, 0b10001, 0b11110},  // B
    {0b01110, 0b10001, 0b10000, 0b10000, 0b10000, 0b10001, 0b01110},  // C
    {0b11110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b11110},  // D
    {0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b11111},  // E
    {0b01110, 0b10001, 0b10000, 0b10111, 0b10001, 0b10001, 0b01111},  // G
}};

// Build an indexed atlas: one 8×8 tile per glyph, index 1 = lit, index 0 = background. The 5×7 glyph
// sits with a 1-px left and 1-px top margin.
std::vector<std::uint8_t> buildFontAtlas() {
    const int width = kTile * GCount;
    std::vector<std::uint8_t> atlas(static_cast<std::size_t>(width) * kTile, 0);
    for (int g = 0; g < GCount; ++g) {
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

// One on-screen key: the button that triggers it, the glyph drawn for its identity (an arrow or a face
// letter) + the note-name glyph below it, the top-left grid cell, and which audio system plays it.
struct Key {
    Button button;
    int    idGlyph;
    int    noteGlyph;
    int    col;
    int    row;
    int    system;  // index into the per-note AudioSystem list
};

}  // namespace

int main() {
    SDL_SetMainReady();

    const EngineConfig config{.window = {.title = "GBCPP — audio keyboard (d-pad + A/B play notes)"}};

    SteadyClock clock;
    RunLoop     loop{clock, config.timing};
    SdlPlatform platform{config};
    Renderer    renderer{platform.device(), platform.window(), config.viewport};

    // The six tone assets, copied next to the binary after the build; resolve them at the executable's
    // own location so the demo runs from any working directory.
    const char* base = SDL_GetBasePath();
    const std::string dir = std::string(base ? base : "") + "assets/tones/";
    const std::array<const char*, 6> toneFiles{
        "tone_c.asm", "tone_d.asm", "tone_e.asm", "tone_g.asm", "tone_a.asm", "tone_c2.asm"};

    // One AudioSystem (and its own output stream) per note — each gentle-triangle driver fills its own
    // VM, and the OS mixes the independent streams, so held notes sound together as a chord. The sink
    // must outlive its system, so sinks are declared/destroyed after the systems.
    std::vector<std::unique_ptr<SdlAudioSink>> sinks;
    std::vector<std::unique_ptr<AudioSystem>>  systems;
    std::vector<AudioId>                        toneIds;
    for (const char* file : toneFiles) {
        auto sink = std::make_unique<SdlAudioSink>();
        auto system = std::make_unique<AudioSystem>(*sink, VMPlatform::GameBoyColor);
        // Real-driver shape: set the wave channel up ONCE (wave_init), then a note just retunes and
        // triggers — so a replay never rewrites wave RAM (which corrupts it) or toggles the DAC (a pop).
        const AudioId initId = system->registerAudio(dir + "wave_init.asm", AudioType::Music);
        toneIds.push_back(system->registerAudio(dir + file, AudioType::Music));
        system->play(initId);  // arm the channel; runs during the warm-up ticks below, stays silent
        sinks.push_back(std::move(sink));
        systems.push_back(std::move(system));
    }

    const std::vector<std::uint8_t> atlas = buildFontAtlas();
    const AtlasId atlasId = renderer.uploadAtlas(atlas.data(), kTile * GCount, kTile);
    // Two palettes sharing a dark background: index 1 is the glyph colour — dim when idle, bright when
    // the key is held. Switching a cell's palette is the steady highlight (no animation, no flashing).
    const std::array<Rgba8, 4> idleColours{{{18, 20, 30}, {96, 120, 112}, {0, 0, 0}, {0, 0, 0}}};
    const std::array<Rgba8, 4> heldColours{{{18, 20, 30}, {244, 230, 120}, {0, 0, 0}, {0, 0, 0}}};
    const PaletteId idlePal = renderer.uploadPalette(std::span<const Rgba8>(idleColours));
    const PaletteId heldPal = renderer.uploadPalette(std::span<const Rgba8>(heldColours));
    const std::array<PaletteId, 2> paletteSet{idlePal, heldPal};

    // The keyboard layout: a d-pad cross on the left, the A / B face buttons on the right. Each key
    // shows its identity glyph with the note name directly below.
    const std::array<Key, 6> keys{{
        {Button::Up,    GUp,    GC, 4, 4, 0},   // Up    → C
        {Button::Left,  GLeft,  GE, 2, 6, 2},   // Left  → E
        {Button::Right, GRight, GG, 6, 6, 3},   // Right → G
        {Button::Down,  GDown,  GD, 4, 8, 1},   // Down  → D
        {Button::A,     GA,     GA, 13, 5, 4},  // A     → A
        {Button::B,     GB,     GC, 15, 7, 5},  // B     → C (octave up)
    }};

    // Lay the glyphs into the tile grid once; only their palette (idle/held) changes per frame.
    std::vector<TileCell> cells(static_cast<std::size_t>(kMapW) * kMapH);
    for (TileCell& c : cells) {
        c.tile = GBlank;
        c.palette = 0;
    }
    auto idCell   = [&](const Key& k) -> TileCell& { return cells[static_cast<std::size_t>(k.row) * kMapW + k.col]; };
    auto noteCell = [&](const Key& k) -> TileCell& { return cells[static_cast<std::size_t>(k.row + 1) * kMapW + k.col]; };
    for (const Key& k : keys) {
        idCell(k).tile   = static_cast<std::uint16_t>(k.idGlyph);
        noteCell(k).tile = static_cast<std::uint16_t>(k.noteGlyph);
    }

    FrameDrawState frame;
    frame.layers.resize(1);
    DrawLayer& layer = frame.layers[0];
    layer.id      = "keyboard";
    layer.z       = 0;
    layer.size    = PixelSize{config.viewport.width, config.viewport.height};
    layer.content = TileContent{atlasId, std::span<const PaletteId>(paletteSet),
                                kMapW, kMapH, std::span<const TileCell>(cells)};

    // Run wave_init on every system for a few ticks before any note can trigger, so each channel's
    // wave RAM is set up first. Input is ignored during this brief warm-up (the channels stay silent).
    int warmup = 12;
    loop.setTick([&](const InputState& in) {
        if (warmup > 0) {
            --warmup;
            for (std::unique_ptr<AudioSystem>& s : systems) {
                s->tick();
            }
            return;
        }
        for (const Key& k : keys) {
            const auto sys = static_cast<std::size_t>(k.system);
            if (in.justPressed(k.button)) {
                systems[sys]->play(toneIds[sys]);  // retune + trigger the already-set-up channel
            }
            if (in.justReleased(k.button)) {
                systems[sys]->stop();
            }
            const std::uint8_t pal = in.isHeld(k.button) ? 1 : 0;
            idCell(k).palette   = pal;
            noteCell(k).palette = pal;
        }
        // Advance every audio system one frame's worth — playing ones synthesize, stopped ones are silent.
        for (std::unique_ptr<AudioSystem>& s : systems) {
            s->tick();
        }
    });
    loop.setRender([&](float /*alpha*/) { renderer.renderFrame(frame, /*alpha=*/0.0f); });

    std::printf("Audio keyboard — press the d-pad and A / B to play notes; hold several for a chord. "
                "Close the window to quit.\n");
    WindowedHost{loop, platform}.run();
    return 0;
}
