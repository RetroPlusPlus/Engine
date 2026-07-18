# VM host & routines

The VM host runs the narrow set of original-machine routines whose output a native re-implementation
**cannot** reproduce exactly — gameplay RNG that reads a free-running hardware register, and a
cycle-driven sound driver — and exposes each as an ordinary typed C++ function. Everything
else in a port is native code; this is the surgical exception.

```cpp
#include "retropp/vm.h"          // VMPlatform, Vm, Routine, Location, RoutineBinding, Throttle
#include "retropp/gb.h"          // gb::Reg + gb::A … gb::PC — the Game Boy register vocabulary
#include "retropp/gb_routines.h" // retropp::sameboy::divRng / dualSeedRng — ready-made GB routine presets
```

## Contents

- [The shape of it](#the-shape-of-it)
- [Selecting a system: `VMPlatform`](#selecting-a-system-vmplatform)
- [Registering your own routine](#registering-your-own-routine)
  - [Authoring in assembly: register from a `.asm` file](#authoring-in-assembly-register-from-a-asm-file)
  - [Assembling source to bytes yourself](#assembling-source-to-bytes-yourself)
  - [State persists across calls](#state-persists-across-calls)
  - [Time-based registers: advance the clock between calls](#time-based-registers-advance-the-clock-between-calls)
- [Pacing: `Throttle` (and the audio seam)](#pacing-throttle-and-the-audio-seam)
- [The typed callable: `Routine<Sig>`](#the-typed-callable-routinesig)
- [Ready-made presets: `retropp::sameboy`](#ready-made-presets-retroppsameboy)
- [Where to change things](#where-to-change-things)
- [Status](#status)

## The shape of it

```cpp
retropp::Vm vm{retropp::VMPlatform::GameBoyColor};

// Register a routine ONCE, declaring where its inputs/output live. Then call it like a function.
auto rng = retropp::sameboy::dualSeedRng(vm); // a ready-made preset — no bytes, no addresses
std::uint8_t roll = rng();                // plain C++ at the call site: no register/memory idiom
```

Two ideas carry the whole design:

1. **Call it like a function.** A routine is registered once and thereafter called as a typed C++
   callable. Registers, memory addresses, and entry offsets appear **only** in the registration's
   binding — never at a call site. This is the engine's "no hardware-register variables" principle
   carried to the VM boundary.
2. **No ROM.** This is a *port*, not an emulator. No game ROM is ever loaded or executed. The only
   original code that runs in the VM is a handful of extracted routines — authored as SM83 `.asm`
   source the engine assembles in-process (or supplied as pre-assembled bytes) and injected into the
   VM's code space. There is no `loadRom`, no ROM-relative address, no cartridge image anywhere.

## Selecting a system: `VMPlatform`

```cpp
enum class VMPlatform { GameBoy, GameBoyColor, Snes, Nes, Genesis, MasterSystem };
```

`VMPlatform` selects a per-system backend. The call surface is identical across systems because each
routine's convention is sealed in its own binding; only the register/memory *vocabulary* a binding
names is system-specific (and lives in a per-system header — `gb.h` for the Game Boy family).

In v1 the **GameBoy / GameBoyColor** backend is the only one built; any other enumerator throws
`std::runtime_error` ("no backend built in v1") at `Vm` construction. The other entries are the
declared seam — a new system is a drop-in backend, not a change to this surface.
(`vm.platform()` reports back the system a `Vm` was constructed for.)

The constructor takes a second, optional parameter — the stepping cadence:
`Vm(VMPlatform platform, TimingProfile timing = TimingProfile::GameBoyColor)`. It defaults to the Game
Boy Color clock; pass a different `TimingProfile` to step the routine at another console's rate. A `Vm`
is non-copyable but movable.

Each platform maps to an instruction-set architecture — `Isa` (`retropp/isa.h`), via
`isaFor(VMPlatform)`. The ISA is the real compatibility unit: several consoles can share one (the Game
Boy and Game Boy Color both run `Isa::Sm83`), so a routine's bytes run on any VM of the same ISA. The
audio library uses it to verify a chiptune is cued on a compatible VM; a routine consumer rarely names
it directly.

## Registering your own routine

There are two doors, mirroring the audio library's `uploadAudio` / `registerAudio` (and the renderer's
`uploadAtlas` / `loadAtlas`): **`uploadRoutine`** takes ready bytes; **`registerRoutine`** takes a `.asm`
file path. Start with the byte door — the `.asm` door (below) assembles down to it.

```cpp
template <typename Sig>
Routine<Sig> Vm::uploadRoutine(std::span<const std::uint8_t> routineBytes,
                               const RoutineBinding& binding, int instances = 1);
```

`Sig` is the function signature you want to call it as — e.g. `std::uint8_t(std::uint8_t)`. The
**signature** fixes the width of each value (`uint8_t` → 1 byte, `uint16_t` → 2, `uint32_t` → 4); the
**binding** fixes where each value lives and how the routine is paced:

```cpp
struct RoutineBinding {
    std::vector<Location>   inputs;          // argument i is marshalled to inputs[i]
    std::optional<Location> output{};        // return value is read from output (nullopt = void)
    Throttle                throttle = Throttle::HostSpeed;
    std::uint32_t           entryOffset = 0; // first instruction's offset WITHIN routineBytes
};
```

A `Location` is a register or an absolute memory address:

```cpp
Location::reg(id)        // a register, by backend id — use the typed constants below, not raw ids
Location::memory(addr)   // an absolute address in the target machine's memory map
```

For the Game Boy family, name registers with the `gb::` constants instead of raw ids:

```cpp
// ADD A,B ; RET   — sum two bytes, both in registers
static constexpr std::uint8_t kAdd[] = {0x80, 0xC9};
auto add = vm.uploadRoutine<std::uint8_t(std::uint8_t, std::uint8_t)>(
    kAdd, {.inputs = {gb::A, gb::B}, .output = gb::A});
std::uint8_t s = add(3, 4);    // 7

// a routine that reads a byte from HRAM and writes one back — bound by address
auto f = vm.uploadRoutine<std::uint8_t(std::uint8_t)>(
    bytes, {.inputs = {retropp::Location::memory(0xFF90)}, .output = retropp::Location::memory(0xFF91)});
```

The Game Boy register constants (`retropp/gb.h`): `gb::A gb::F gb::B gb::C gb::D gb::E gb::H gb::L`
(8-bit) and `gb::AF gb::BC gb::DE gb::HL gb::SP gb::PC` (16-bit). A value's width must match its bound
register (a `uint16_t` bound to `gb::A` throws at registration).

Both doors validate the binding and throw (`std::invalid_argument`) on an inputs/arity mismatch, a
width/location mismatch, an unknown register, an inaccessible address, a void signature that binds an
output (or a value-returning one that binds none), empty routine bytes, or an `entryOffset` past the end
of the bytes — so a malformed binding fails loudly at registration, not silently at call time.

### Authoring in assembly: register from a `.asm` file

You normally don't hand-write byte arrays — you write the routine as **SM83 assembly in a `.asm`
file** and point `registerRoutine` at it. The VM reads the file and assembles it in-process with an
in-engine SM83 assembler — no external toolchain, no build step, nothing to install:

```cpp
template <typename Sig>
Routine<Sig> Vm::registerRoutine(LiteralPath asmFilePath, const RoutineBinding& binding,
                                 std::optional<AssetPolicy> policy = {}, int instances = 1);
```

```cpp
// my_add.asm:
//     add a, b
//     ret
auto add = vm.registerRoutine<std::uint8_t(std::uint8_t, std::uint8_t)>(
    "routines/my_add.asm", {.inputs = {gb::A, gb::B}, .output = gb::A});   // Embed (default policy)
std::uint8_t s = add(3, 4);   // 7
```

The path is a **compile-time `LiteralPath`** (a string literal), so a build-time scan can find it — not a
runtime string. A genuinely runtime path is not a door: read its bytes yourself and use `uploadRoutine`.
The optional `policy` is the same `AssetPolicy` the asset and audio doors use:

- **`Embed`** (default) — use the bytes the build baked into the binary for this logical path. If none
  were baked (no scan ran), it falls through to an on-disk read so the path still works during
  development.
- **`LoadFromPath`** — resolve `asmFilePath` against the engine's single `assetRoot()`, read it at
  registration, and assemble it in-process — the form for a copyright-derived routine you ship beside
  the binary rather than bake in. There is no separate routine root: routines resolve against the same
  `assetRoot()` as `loadAtlas` / `loadMapPng`.

Omit `policy` to take the per-type default (`Embed`); the only way to deviate is the explicit per-call
token, so the policy reads at the call site.

The assembly is RGBDS-flavoured — the same syntax the Game Boy disassembly is written in: `;` line
comments, `label:` definitions, `$hex` / `%bin` / decimal literals, `[hl]` / `[$FF04]` memory
operands, condition codes, and the standard SM83 instruction set. Game Boy hardware registers are
predefined by name, so you write `ldh a, [rDIV]` rather than `ldh a, [$FF04]`; labels are resolved
across the routine (e.g. `jr` targets). The same `Sig` / `binding` rules as the byte form apply, and
the routine's entry is its first byte. A bad mnemonic, a malformed operand, an unknown symbol, or an
unreadable file throws at registration with the offending line — a typo fails loudly, not at call time.

The byte form (`uploadRoutine(span, …)`) is the low-level path the `.asm` form assembles down to; reach
for it only when you already hold assembled bytes.

### Assembling source to bytes yourself

```cpp
std::vector<std::uint8_t> Vm::assemble(std::string_view source);
```

`assemble` runs SM83 source through this VM's ISA assembler and hands back the machine-code bytes
**without registering anything** — the path when you hold routine source as a *runtime* string (not a
compile-time literal path) and want bytes to pass to `uploadRoutine`. It throws on a source error, with
the offending line. The VM's platform fixes the ISA, so the right assembler is always selected.

### State persists across calls

Routines registered on one `Vm` share its machine memory, so state a routine writes (e.g. an RNG seed
in HRAM) carries into the next call — that is exactly what makes an evolving RNG work. `vm.reset()`
returns the machine to its post-reset state (clearing that state); the registered routines stay
registered.

> **Power-on state is not zero.** Like real hardware, the machine powers on with arbitrary RAM. A
> routine that reads a seed should have that seed written first (the game does this during init).
> Write a known byte with a one-line routine whose input binds to the address, or call a routine that
> seeds it.

### Time-based registers: advance the clock between calls

Some routines read a **free-running hardware register** — most importantly the Game Boy's `rDIV`
divider, which an RNG folds into its seed. On real hardware that register ticks continuously while
the game runs between calls, so it holds an unpredictable value each time. The VM, by contrast, only
runs *during* a routine and is frozen between calls — so without help `rDIV` barely moves and an RNG
degenerates into a slow counter.

`Vm::advanceClock(cycles)` ticks the machine's free-running clock without executing a routine, so
those registers keep advancing between calls exactly as on hardware. Drive it from your run loop —
one tick's worth of cycles per tick — and read the amount from the timing profile (don't hardcode it):

```cpp
const std::uint64_t perTick = config.timing.cpuCyclesPerTick();  // 70'224 for the Game Boy
loop.simTick([&](const retropp::InputState&) {
    vm.advanceClock(perTick);    // rDIV (and any time-based register) free-runs with engine time
    // ... game logic, which may call rng() ...
});
```

`advanceClock` only advances timing/divider state — registers, RAM, and the RNG seed are untouched.
A routine that reads no time-based register (pure computation — math, decompression) does not need it.

Two `TimingProfile` helpers keep cadence values out of your code (`#include "retropp/timing.h"`):

| Helper | Returns | Use |
|---|---|---|
| `cpuCyclesPerTick()` | `std::uint32_t` | cycles to `advanceClock` per tick (0 if the profile has no CPU model) |
| `ticksForDuration(std::chrono::duration)` | `std::uint64_t` | a wall-clock interval as a tick count, e.g. `ticksForDuration(std::chrono::seconds{2})` — the same `uint64_t` `RunLoop::tickCount()` uses |

## Pacing: `Throttle` (and the audio seam)

```cpp
enum class Throttle { HostSpeed, HardwareSpeed };
```

`HostSpeed` runs the routine as fast as the host allows — correct for a routine you CALL for a return
value (RNG). `HardwareSpeed` throttles the routine to the CPU clock for a real-time consumer like a
sound driver; it is **realized** and drives the audio chain — but you don't register a HardwareSpeed
routine directly, the [`AudioSystem`](audio.md) does it for you (you register *audio*, not a routine).
`instances > 1` (multiple independent copies of a routine, for anti-channel-stealing audio) remains a
**declared seam**: registering with it throws `std::logic_error` today and is realized with the
anti-stealing backend. It is in the surface now so that work plugs in without an API break.

The raw driver chain underneath the `AudioSystem` is three `Vm` members: `enableAudio(rate, onSample)`
turns on the APU and routes each produced PCM frame, `startDriver(routine)` positions a `HardwareSpeed`
routine to run continuously, and `stepDriver(cpuCycles)` advances it one cycle budget (producing audio
into the sink). You normally let the [`AudioSystem`](audio.md) own these; reach for them directly only
to host a driver yourself.

## The typed callable: `Routine<Sig>`

`registerRoutine` returns a `Routine<Sig>` — a small copyable value handle you call like a function.
It refers back to its `Vm`, so keep the `Vm` alive for as long as you hold the routine (the same
lifetime rule as the renderer's `AtlasId` / `PaletteId`). Arguments and the return value must be
`uint8_t` / `uint16_t` / `uint32_t` (or `void` return).

## Ready-made presets: `retropp::sameboy`

Standard original-hardware routines have a fixed convention, so the engine ships them — each is
authored as a `.asm` file the build assembles to bytecode and bakes into the binary, then registers and
binds for you. You pass nothing but the `Vm&`:

```cpp
auto a = retropp::sameboy::divRng(vm);      // ldh a,[rDIV]; ret — a raw DIV read (stateless)
auto b = retropp::sameboy::dualSeedRng(vm); // a general-purpose RNG: DIV folded into a dual seed
std::uint8_t x = a();
std::uint8_t y = b();
```

`divRng` returns the free-running DIV register as a random byte — the *common* DIV-read technique,
stateless. `dualSeedRng` folds DIV into a dual, carry-chained seed that persists across calls, so it
mixes well and serves as a true general-purpose RNG for any Game Boy-family game. Its algorithm is an
implementation of the disassembled Pokémon Gen 1/2 `_Random` routine (the engine ships its own
assembly of the publicly-documented algorithm — no copyrighted game code); the name is
mechanism-descriptive rather than title-specific. A purely-algorithmic PRNG that reads no hardware
register is **not** here: it is byte-reproducible in plain C++ and so is native-ported, not a VM
routine.

## Where to change things

- **Add a routine preset for the Game Boy family:** write the routine as a `.asm` file under
  `src/vm/gameboy/routines/`, add it to the build's routine-bake list so it is assembled to compile-time
  bytecode (`src/vm/gameboy/gb_routine_bytecode.h`), then add a factory (declared in
  `include/retropp/gb_routines.h`, defined in `src/vm/gameboy/gb_routines.cpp`) that registers the baked
  byte span with `uploadRoutine` and builds the binding — mirror `divRng` / `dualSeedRng`.
- **Add register/memory vocabulary for the Game Boy family:** extend `include/retropp/gb.h`.
- **Add a whole new system (SNES, NES, …):** add a `src/vm/<system>/` folder with that system's
  backend (and its own ISA assembler + routines), and a factory case — the public `vm.h` surface does
  not change. Every system's machine idiom stays behind its own backend; `vm.h` stays system-agnostic.

## Status

Available: the Game Boy / Game Boy Color backend, both registration doors — `uploadRoutine`
(pre-assembled bytes) and `registerRoutine` (a `.asm` file the engine assembles in-process, with the
`Embed` / `LoadFromPath` policy) — the in-engine SM83 assembler, the `Location` / `RoutineBinding`
surface, the `gb::` register vocabulary, the `divRng` / `dualSeedRng` presets, `advanceClock` (the
free-running-divider model), the host-speed / single-instance path, and the `HardwareSpeed` throttle
(driving the [audio chain](audio.md)). Declared seams, not yet realized: `instances > 1`, binding a
location by label name, and non-Game-Boy backends.
