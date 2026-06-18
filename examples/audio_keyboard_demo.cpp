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

#include "retropp/audio_library.h"  // AudioLibrary — registration lives here, not on a system
#include "retropp/audio_system.h"
#include "retropp/clock.h"
#include "retropp/draw_state.h"
#include "retropp/engine_config.h"
#include "retropp/input.h"
#include "retropp/palette.h"
#include "retropp/renderer.h"
#include "retropp/run_loop.h"
#include "retropp/sdl_platform.h"
#include "retropp/windowed_host.h"

using namespace retropp;

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

    const EngineConfig config{.window = {.title = "Retro++ — audio keyboard (d-pad + A/B play notes)"}};

    EngineConfig::setActive(config);  // make it the active config — the bare ctors below inherit it
    SteadyClock clock;
    RunLoop     loop{clock};
    SdlPlatform platform;
    Renderer    renderer{platform.device(), platform.window()};

    // One AudioSystem (and its own output stream) per note — each gentle-triangle driver fills its own
    // VM, and the OS mixes the independent streams, so held notes sound together as a chord. Each system
    // auto-owns its production sink (the sink-less ctor), so there is no sink to declare or keep alive.
    std::vector<std::unique_ptr<AudioSystem>> systems;
    for (int i = 0; i < 6; ++i) {
        systems.push_back(std::make_unique<AudioSystem>(VMPlatform::GameBoyColor));
    }

    // Registration lives on the single AudioLibrary, NOT on a system — a system only CUES. Register the
    // assets ONCE (selecting their SM83 ISA), each by its full project-relative LITERAL path; the engine
    // resolves it against assetRoot() (set by setActive above, the executable's own location), and the
    // AudioIds are shared so each system materializes its own driver copy on first play. Real-driver
    // shape: wave_init sets the wave channel up ONCE, then a note just retunes and triggers — so a replay
    // never rewrites wave RAM (which corrupts it) or toggles the DAC (a pop).
    //
    // This demo is the EXHAUSTIVE policy test: a few notes are Embed (baked into the binary by the build
    // scan — the .asm never ships) and the rest are LoadFromPath (the .asm rides along beside the binary
    // and is assembled at startup). Both resolve to identical sound; the only difference is where the
    // bytes come from. (Chiptune's per-type default is Embed, but the policy is written out here so the
    // split is explicit.)
    // ONE register call per statement — the build scan keys each call to its own `;`, so a call buried
    // in a multi-element initializer would not be seen individually (the asset-scan convention).
    AudioLibrary& library = AudioLibrary::instance();
    // Embed (baked into the binary, .asm never ships): the shared channel init + the first two notes.
    const AudioId waveInit = library.registerAudio("examples/assets/tones/wave_init.asm", AudioType::Music, Isa::Sm83, AssetPolicy::Embed);
    const AudioId toneC    = library.registerAudio("examples/assets/tones/tone_c.asm",    AudioType::Music, Isa::Sm83, AssetPolicy::Embed);
    const AudioId toneD    = library.registerAudio("examples/assets/tones/tone_d.asm",    AudioType::Music, Isa::Sm83, AssetPolicy::Embed);
    // LoadFromPath (the .asm rides along beside the binary): the remaining four notes.
    const AudioId toneE    = library.registerAudio("examples/assets/tones/tone_e.asm",    AudioType::Music, Isa::Sm83, AssetPolicy::LoadFromPath);
    const AudioId toneG    = library.registerAudio("examples/assets/tones/tone_g.asm",    AudioType::Music, Isa::Sm83, AssetPolicy::LoadFromPath);
    const AudioId toneA    = library.registerAudio("examples/assets/tones/tone_a.asm",    AudioType::Music, Isa::Sm83, AssetPolicy::LoadFromPath);
    const AudioId toneC2   = library.registerAudio("examples/assets/tones/tone_c2.asm",   AudioType::Music, Isa::Sm83, AssetPolicy::LoadFromPath);
    const std::array<AudioId, 6> toneIds{toneC, toneD, toneE, toneG, toneA, toneC2};
    // Arm every channel once; wave_init runs during the warm-up ticks below, staying silent.
    for (std::unique_ptr<AudioSystem>& s : systems) {
        s->play(waveInit);
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
