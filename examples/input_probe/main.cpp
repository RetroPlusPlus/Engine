// Input probe — a live diagnostic for the action-based input system. It binds one of everything the
// binding surface offers — every face button (positional AND printed-letter), d-pad, shoulders,
// triggers, stick clicks, Start/Select, both sticks, keyboard keys, and mouse buttons — and shows it
// working against real hardware, so a keyboard + any controller (Xbox / PlayStation / Nintendo /
// generic) can be verified by hand:
//
//   • two rows of blocks, one per digital action — lit while the action is held, from ANY of its
//     sources at once;
//   • a dot driven by the Move VECTOR — the left stick and the arrows/WASD/d-pad feed the same
//     read (stick = analog deflection, keys = unit steps);
//   • an amber Aim pointer orbiting the dot on the right stick — the twin-stick pairing;
//   • a throttle bar driven by the left trigger's AXIS value;
//   • an active-device swatch — white while the keyboard/mouse last produced input, a family
//     colour while a pad did (green Xbox, blue PlayStation, red Nintendo, orange generic) — the
//     signal a game's glyph layer reads;
//   • a mouse marker at the viewport cursor while the pointer is over the drawn area;
//   • a gate box plotting the left stick in throw space — a dim dot for the raw throw and a bright dot
//     for the processed value, with M cycling the left-stick gate (Round / Square / Scaled) so the
//     circle→square remap is visible (the bright dot reaches the corners under Square);
//   • a vibration mode (R toggles it; top-right swatch lights magenta while ON) that declares the pad's
//     motor state from the live analog inputs each tick — left stick → the big motor, right stick → the
//     small motor, each trigger → its own trigger motor — so the OUTPUT half of the pad is felt, not
//     just drawn (the whole input→output loop in one object).
//
// Every action edge and device change also prints to the console, so each physical press can be
// matched to the action it landed on. Swap controllers mid-run: bindings keep working (device-class
// binding), the printed-letter aliases re-resolve to the new family, and the swatch follows.
//
// Close the window to quit.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

#include "retropp/clock.h"
#include "retropp/draw_state.h"
#include "retropp/engine_config.h"
#include "retropp/geometry.h"
#include "retropp/input.h"
#include "retropp/input_actions.h"
#include "retropp/palette.h"
#include "retropp/renderer.h"
#include "retropp/run_loop.h"
#include "retropp/sdl_platform.h"
#include "retropp/viewport.h"
#include "retropp/vibration.h"
#include "retropp/windowed_host.h"

namespace {

using namespace retropp;

constexpr int kViewW = 320, kViewH = 240;
constexpr int kMapW = 40, kMapH = 30;  // 40×30 8-px tiles cover the 320×240 viewport

// The probe's vocabulary: the first fifteen are the digital blocks (two display rows, in order);
// Move/Aim/Throttle are the valued reads.
enum class Action : std::uint8_t {
    Up, Down, Left, Right, Confirm, Cancel, LabelX, LabelY,          // row 1
    Fire, ShoulderL, ShoulderR, ClickL, ClickR, Select, Pause, GuideBtn, ShareBtn,  // row 2
    Move, Aim, Throttle,
    CycleGate,  // M — cycles the left-stick gate (Round / Square / Scaled) to show it in the gate box
    ToggleRumble,  // R — toggles the vibration mode: the live analog inputs drive the pad's motors
};
constexpr int kDigitalCount = 17;
constexpr int kRowOneCount  = 8;
constexpr std::array<const char*, kDigitalCount> kActionNames{
    "Up", "Down", "Left", "Right", "Confirm(A)", "Cancel(B)", "LabelX", "LabelY",
    "Fire", "ShoulderL", "ShoulderR", "ClickL", "ClickR", "Select", "Pause", "Guide", "Share"};

[[nodiscard]] ScreenSpaceEffect solidFill(Rgba8 colour) {
    return ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill, .fill = colour};
}

[[nodiscard]] const char* familyName(ControllerType family) {
    switch (family) {
        case ControllerType::Xbox:        return "Xbox";
        case ControllerType::PlayStation: return "PlayStation";
        case ControllerType::Nintendo:    return "Nintendo";
        case ControllerType::Standard:    return "Standard";
        case ControllerType::Unknown:     return "Unknown";
    }
    return "Unknown";
}

[[nodiscard]] Rgba8 deviceColour(ActiveDevice device) {
    if (device.kind == DeviceKind::KeyboardMouse) return Rgba8{235, 235, 235};  // white
    if (device.kind == DeviceKind::Gamepad) {
        switch (device.family) {
            case ControllerType::Xbox:        return Rgba8{60, 200, 80};    // green
            case ControllerType::PlayStation: return Rgba8{70, 110, 245};   // blue
            case ControllerType::Nintendo:    return Rgba8{230, 60, 60};    // red
            default:                          return Rgba8{240, 150, 40};   // orange (generic)
        }
    }
    return Rgba8{60, 64, 80};  // none yet — dark slate
}

}  // namespace

int main() {

    const EngineConfig config{
        .identity = {.organization = "Retro++", .application = "Input Probe"},
        .window   = {.title = "Retro++ — input probe (keyboard + any controller)"},
        .viewport = ViewportResolution{kViewW, kViewH}};
    EngineConfig::setActive(config);
    SteadyClock clock;
    RunLoop     loop{clock};
    SdlPlatform platform;
    Renderer    renderer{platform.device(), platform.sdlWindow()};

    // One of everything — every pad control is bound to a visible block, so no press on any family's
    // pad is silent. Label aliases (Confirm/Cancel/LabelX/LabelY follow the pad's PRINTED letters), a
    // trigger as a digital source (Fire past the threshold), shoulders/stick-clicks/Select/Start by
    // their neutral names, mouse buttons beside keys and pad on the same actions, and the valued
    // reads (Move + Aim vectors — the twin-stick pairing — and the Throttle axis). The digital
    // directional preset and the vector preset coexist on purpose: one W press lights the Up block
    // AND moves the dot.
    ActionMap map{
        {Action::Confirm,   {SDL_SCANCODE_RETURN, PadButton::FaceLabelA}},
        {Action::Cancel,    {SDL_SCANCODE_BACKSPACE, PadButton::FaceLabelB, MouseButton::Right}},
        {Action::LabelX,    {SDL_SCANCODE_X, PadButton::FaceLabelX}},
        {Action::LabelY,    {SDL_SCANCODE_Y, PadButton::FaceLabelY}},
        {Action::Fire,      {SDL_SCANCODE_SPACE, MouseButton::Left, PadButton::TriggerR}},
        {Action::ShoulderL, {SDL_SCANCODE_Q, PadButton::ShoulderL}},
        {Action::ShoulderR, {SDL_SCANCODE_E, PadButton::ShoulderR}},
        {Action::ClickL,    {SDL_SCANCODE_Z, PadButton::StickClickL}},
        {Action::ClickR,    {SDL_SCANCODE_C, PadButton::StickClickR}},
        {Action::Select,    {SDL_SCANCODE_TAB, PadButton::Select}},
        {Action::Pause,     {SDL_SCANCODE_P, PadButton::Start}},
        {Action::GuideBtn,  {SDL_SCANCODE_G, PadButton::Guide}},
        {Action::ShareBtn,  {SDL_SCANCODE_V, PadButton::Share}},
        {Action::Aim,       {PadStick::Right}},
        {Action::Throttle,  {PadButton::TriggerL}},
        {Action::CycleGate,    {SDL_SCANCODE_M}},
        {Action::ToggleRumble, {SDL_SCANCODE_R}},
    };
    map.add(presets::directional(Action::Up, Action::Down, Action::Left, Action::Right));
    map.add(presets::directionalVector(Action::Move));
    platform.actions(map);

    // An opaque dim-grid backdrop (in-code art — the probe ships no assets).
    std::array<std::uint8_t, 64> gridArt{};
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x)
            gridArt[static_cast<std::size_t>(y) * 8 + x] = (x == 0 || y == 0) ? 2 : 1;
    const AtlasId gridAtlas = renderer.uploadAtlas(gridArt.data(), 8, 8).atlasId;
    const std::array<Rgba8, 3> gridPal{{{0, 0, 0}, {26, 28, 40}, {38, 42, 60}}};
    const PaletteId gridPalId = renderer.uploadPalette(std::span<const Rgba8>(gridPal));
    const std::vector<TileCell> gridCells(static_cast<std::size_t>(kMapW) * kMapH,
                                          TileCell{.atlas = gridAtlas, .tile = 0, .palette = gridPalId});

    // The left-stick gate cycles Round → Square → Scaled on the M key, so the gate box shows the same
    // raw throw mapped three ways. Scaled sits partway to Square (gateScale below).
    constexpr std::array<GateShape, 3> kGates{GateShape::Round, GateShape::Square, GateShape::Scaled};
    constexpr std::array<const char*, 3> kGateNames{"Round", "Square", "Scaled(1.3)"};
    int gateMode = 0;
    auto applyGate = [&] {
        AnalogResponse response{};
        response.leftStick.gate      = kGates[static_cast<std::size_t>(gateMode)];
        response.leftStick.gateScale = 1.3f;  // read only for Scaled
        platform.analogResponse(response);
    };
    applyGate();

    // Probe state the tick writes and the render reads.
    std::array<bool, kDigitalCount> held{};
    Vec2  dotPos{80.0f, 160.0f};
    Vec2  aim{};
    float throttle    = 0.0f;
    Vec2  stickProc{};   // left stick after the gate — reaches the box corners under Square
    Vec2  stickRawL{};   // left stick raw — stays on the inscribed circle
    ActiveDevice device{};
    Vec2i cursor{};
    bool  cursorOn    = false;
    ActiveDevice lastPrinted{};
    std::size_t  lastPadCount = 0;
    bool         rumbleOn     = false;  // vibration mode: while ON, the live analog inputs drive the motors

    loop.simTick([&](const InputState& in) {
        // Digital blocks + console edges.
        for (int i = 0; i < kDigitalCount; ++i) {
            const auto a = static_cast<Action>(i);
            held[static_cast<std::size_t>(i)] = in.isHeld(a);
            if (in.justPressed(a)) std::printf("pressed  %s\n", kActionNames[static_cast<std::size_t>(i)]);
            if (in.justReleased(a)) std::printf("released %s\n", kActionNames[static_cast<std::size_t>(i)]);
        }

        // The Move vector drives the dot (stick deflection is proportional; keys are unit steps);
        // the dot stays inside its box.
        const Vec2 move = in.vector(Action::Move);
        dotPos.x = std::clamp(dotPos.x + move.x * 2.0f, 12.0f, 148.0f);
        dotPos.y = std::clamp(dotPos.y + move.y * 2.0f, 96.0f, 228.0f);

        // Cycle the left-stick gate and reapply the config.
        if (in.justPressed(Action::CycleGate)) {
            gateMode = (gateMode + 1) % 3;
            applyGate();
            std::printf("left-stick gate -> %s\n", kGateNames[static_cast<std::size_t>(gateMode)]);
        }
        // The processed left stick (post-gate) beside its raw throw — the gate box plots both.
        stickProc = in.stick(Stick::Left);
        stickRawL = in.stickRaw(Stick::Left);

        aim      = in.vector(Action::Aim);
        throttle = in.axis(Action::Throttle);
        device   = in.activeDevice();
        cursor   = in.cursor();
        cursorOn = in.cursorOnScreen();

        // Vibration mode: toggle on R, and while ON declare the pad's motor state from the live analog
        // inputs every tick — the whole input→output loop in one object (feel the same values drawn).
        // Left-stick deflection → the big (low) motor, right-stick → the small (high) motor, each
        // analog trigger → its own trigger motor. While OFF the probe declares nothing (the no-call
        // tick = silence path), so releasing the toggle stops the motors on the next tick.
        if (in.justPressed(Action::ToggleRumble)) {
            rumbleOn = !rumbleOn;
            std::printf("vibration mode -> %s\n", rumbleOn ? "ON" : "OFF");
        }
        if (rumbleOn) {
            const auto magnitude = [](Vec2 v) {
                return std::clamp(std::sqrt(v.x * v.x + v.y * v.y), 0.0f, 1.0f);
            };
            const auto toByte = [](float f) {
                return static_cast<std::uint8_t>(std::clamp(f, 0.0f, 1.0f) * 255.0f);
            };
            platform.gamepad(0).vibration({
                .low          = toByte(magnitude(in.stick(Stick::Left))),
                .high         = toByte(magnitude(in.stick(Stick::Right))),
                .triggerLeft  = toByte(in.trigger(Trigger::Left)),
                .triggerRight = toByte(in.trigger(Trigger::Right)),
            });
        }

        // Console: device transitions + pad connect/disconnect.
        if (device != lastPrinted) {
            if (device.kind == DeviceKind::KeyboardMouse) {
                std::printf("active device -> keyboard/mouse\n");
            } else if (device.kind == DeviceKind::Gamepad) {
                std::printf("active device -> gamepad (%s)\n", familyName(device.family));
            }
            lastPrinted = device;
        }
        const auto pads = platform.connectedGamepads();
        if (pads.size() != lastPadCount) {
            std::printf("connected gamepads: %zu\n", pads.size());
            for (const GamepadInfo& pad : pads) {
                std::printf("  id %u — %s — player %d\n", static_cast<unsigned>(pad.id),
                            familyName(pad.family), pad.slot);
            }
            lastPadCount = pads.size();
        }
    });

    FrameDrawState frame;
    loop.renderLoop([&]() {
        frame.layers.clear();
        DrawLayer bg{.key = "backgroundGrid"};
        bg.z       = -10;
        bg.size    = PixelSize{kViewW, kViewH};
        bg.content = TileContent{.widthInTiles = kMapW, .heightInTiles = kMapH,
                                 .cells = std::span<const TileCell>(gridCells)};
        frame.layers.push_back(bg);

        frame.regions.clear();

        // The active-device swatch (top-left).
        frame.regions.push_back(Region{.key = "device",
                                       .shape = ShapePoints::rectangle(Point{8, 6}, 40, 12),
                                       .effects = {solidFill(deviceColour(device))}});

        // The vibration-mode indicator (top-right): magenta while ON (motors driven by the analog
        // inputs), dark slate while OFF. Press R to toggle.
        frame.regions.push_back(
            Region{.key = "rumble",
                   .shape = ShapePoints::rectangle(Point{kViewW - 48, 6}, 40, 12),
                   .effects = {solidFill(rumbleOn ? Rgba8{235, 60, 200} : Rgba8{52, 56, 74})}});

        // The digital blocks, two rows: dim slate when idle, bright cyan while held.
        static constexpr std::array<const char*, kDigitalCount> kBlockKeys{
            "blkUp", "blkDown", "blkLeft", "blkRight", "blkConfirm", "blkCancel", "blkLX", "blkLY",
            "blkFire", "blkShL", "blkShR", "blkClkL", "blkClkR", "blkSelect", "blkPause",
            "blkGuide", "blkShare"};
        for (int i = 0; i < kDigitalCount; ++i) {
            const bool  rowOne = i < kRowOneCount;
            const int   column = rowOne ? i : i - kRowOneCount;
            const float x      = 8.0f + static_cast<float>(column) * 26.0f;
            const float y      = rowOne ? 26.0f : 52.0f;
            const Rgba8 colour = held[static_cast<std::size_t>(i)] ? Rgba8{40, 220, 255}
                                                                   : Rgba8{52, 56, 74};
            frame.regions.push_back(Region{.key = kBlockKeys[static_cast<std::size_t>(i)],
                                           .shape = ShapePoints::rectangle(Point{x, y}, 20, 20),
                                           .effects = {solidFill(colour)}});
        }

        // The Move box + dot + Aim pointer.
        ShapePoints moveBox = ShapePoints::rectangle(Point{8, 92}, 144, 140);
        moveBox.strokeWidth = 2.0f;
        frame.regions.push_back(Region{.key = "moveBox", .shape = moveBox,
                                       .effects = {solidFill(Rgba8{70, 76, 100})}});
        frame.regions.push_back(
            Region{.key = "dot",
                   .shape = ShapePoints::rectangle(Point{dotPos.x - 3.0f, dotPos.y - 3.0f}, 6, 6),
                   .effects = {solidFill(Rgba8{255, 255, 255})}});
        // The Aim vector (right stick): an amber pointer orbiting the move dot in the aimed
        // direction — the twin-stick pairing (Move walks, Aim points the guns).
        if (aim.x * aim.x + aim.y * aim.y > 0.02f) {
            frame.regions.push_back(
                Region{.key = "aim",
                       .shape = ShapePoints::rectangle(
                           Point{dotPos.x + aim.x * 22.0f - 2.0f, dotPos.y + aim.y * 22.0f - 2.0f}, 4, 4),
                       .effects = {solidFill(Rgba8{255, 180, 40})}});
        }

        // The throttle bar (left trigger): a fixed track + a fill proportional to the pull.
        ShapePoints track = ShapePoints::rectangle(Point{170, 92}, 142, 14);
        track.strokeWidth = 2.0f;
        frame.regions.push_back(Region{.key = "throttleTrack", .shape = track,
                                       .effects = {solidFill(Rgba8{70, 76, 100})}});
        const int fillW = static_cast<int>(throttle * 138.0f);
        if (fillW > 0) {
            frame.regions.push_back(Region{.key = "throttleFill",
                                           .shape = ShapePoints::rectangle(Point{172, 94}, fillW, 10),
                                           .effects = {solidFill(Rgba8{255, 180, 40})}});
        }

        // The gate box: a square throw-space plot of the left stick. The dim dot is the raw throw (it
        // stays on the inscribed circle, corners unreachable); the bright dot is the processed value —
        // under Square it reaches the box corners, under Round it tracks the raw dot. A swatch names the
        // current gate (grey Round, green Square, amber Scaled). Press M to cycle.
        constexpr float kGateCx = 250.0f, kGateCy = 170.0f, kGateHalf = 48.0f;
        ShapePoints gateBox = ShapePoints::rectangle(
            Point{kGateCx - kGateHalf, kGateCy - kGateHalf}, kGateHalf * 2.0f, kGateHalf * 2.0f);
        gateBox.strokeWidth = 2.0f;
        frame.regions.push_back(
            Region{.key = "gateBox", .shape = gateBox, .effects = {solidFill(Rgba8{70, 76, 100})}});
        const std::array<Rgba8, 3> kGateSwatch{{{120, 124, 140}, {60, 200, 80}, {240, 150, 40}}};
        frame.regions.push_back(
            Region{.key = "gateSwatch",
                   .shape = ShapePoints::rectangle(Point{kGateCx - kGateHalf, kGateCy - kGateHalf - 12.0f}, 14, 8),
                   .effects = {solidFill(kGateSwatch[static_cast<std::size_t>(gateMode)])}});
        frame.regions.push_back(
            Region{.key = "gateRaw",
                   .shape = ShapePoints::rectangle(
                       Point{kGateCx + stickRawL.x * kGateHalf - 2.0f, kGateCy + stickRawL.y * kGateHalf - 2.0f}, 4, 4),
                   .effects = {solidFill(Rgba8{90, 96, 120})}});
        frame.regions.push_back(
            Region{.key = "gateProc",
                   .shape = ShapePoints::rectangle(
                       Point{kGateCx + stickProc.x * kGateHalf - 3.0f, kGateCy + stickProc.y * kGateHalf - 3.0f}, 6, 6),
                   .effects = {solidFill(Rgba8{40, 220, 255})}});

        // The mouse marker, while the pointer is over the drawn viewport.
        if (cursorOn) {
            frame.regions.push_back(Region{.key = "mouse",
                                           .shape = ShapePoints::rectangle(
                                               Point{static_cast<float>(cursor.x - 2),
                                                     static_cast<float>(cursor.y - 2)}, 4, 4),
                                           .effects = {solidFill(Rgba8{120, 255, 120})}});
        }

        renderer.renderFrame(frame);
    });

    std::printf(
        "input probe — a block per digital action (lit while held; row 1 above row 2), a dot on the\n"
        "Move vector, an amber Aim pointer on the right stick, a throttle bar on the left trigger,\n"
        "an active-device swatch (white = keyboard/mouse; green = Xbox pad, blue = PlayStation,\n"
        "red = Nintendo, orange = generic), and a marker on the mouse cursor. Bindings:\n"
        "  row 1: Up Down Left Right   arrows + WASD + d-pad\n"
        "         Confirm              Return + the pad button PRINTED A\n"
        "         Cancel               Backspace + right mouse button + the pad button PRINTED B\n"
        "         LabelX / LabelY      X / Y keys + the pad buttons PRINTED X / Y\n"
        "  row 2: Fire                 Space + left mouse button + right trigger (past threshold)\n"
        "         ShoulderL/R          Q / E + the pad shoulders (LB·L1·L / RB·R1·R)\n"
        "         ClickL/R             Z / C + the stick clicks (L3 / R3)\n"
        "         Select               Tab + the pad's Select/Back/Minus\n"
        "         Pause                P + Start/Menu/Plus\n"
        "         Guide                G + the Xbox guide / PS button / Switch Home (the OS or\n"
        "                              Steam may intercept it before the engine ever sees it)\n"
        "         Share                V + Share (Xbox) / Create (PS5) / Capture (Switch)\n"
        "  Move (dot)                  left stick + arrows/WASD/d-pad as a vector\n"
        "  Aim (amber pointer)         right stick — the twin-stick pairing with Move\n"
        "  Throttle (bar)              left trigger, analog\n"
        "  Gate box (right)            left stick in throw space — dim dot raw, bright dot processed\n"
        "  M                           cycle the left-stick gate: Round / Square / Scaled\n"
        "  R                           toggle vibration mode (top-right swatch): while ON, the left\n"
        "                              stick drives the big motor, the right stick the small motor,\n"
        "                              and each trigger its own trigger motor — feel what you move\n"
        "Every edge and device change prints here. Swap controllers mid-run — everything keeps\n"
        "working. Close the window to quit.\n\n");
    WindowedHost host{loop, platform};
    host.run();
    return 0;
}
