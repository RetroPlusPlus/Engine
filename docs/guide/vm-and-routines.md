# VM host & routines

The VM host runs the narrow set of original-machine routines whose output a native re-implementation
**cannot** reproduce byte-for-byte — gameplay RNG that reads a free-running hardware register, and
(later) a cycle-driven sound driver — and exposes each as an ordinary typed C++ function. Everything
else in a port is native code; this is the surgical exception.

```cpp
#include "gbcpp/vm.h"          // VMPlatform, Vm, Routine, Location, RoutineBinding, Throttle
#include "gbcpp/gb.h"          // gb::Reg + gb::A … gb::PC — the Game Boy register vocabulary
#include "gbcpp/gb_routines.h" // gbcpp::sameboy::divRng / dualSeedRng — ready-made GB routine presets
```

## The shape of it

```cpp
gbcpp::Vm vm{gbcpp::VMPlatform::GameBoyColor};

// Register a routine ONCE, declaring where its inputs/output live. Then call it like a function.
auto rng = gbcpp::sameboy::dualSeedRng(vm); // a ready-made preset — no bytes, no addresses
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

## Registering your own routine

```cpp
template <typename Sig>
Routine<Sig> Vm::registerRoutine(std::span<const std::uint8_t> routineBytes,
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
auto add = vm.registerRoutine<std::uint8_t(std::uint8_t, std::uint8_t)>(
    kAdd, {.inputs = {gb::A, gb::B}, .output = gb::A});
std::uint8_t s = add(3, 4);    // 7

// a routine that reads a byte from HRAM and writes one back — bound by address
auto f = vm.registerRoutine<std::uint8_t(std::uint8_t)>(
    bytes, {.inputs = {gbcpp::Location::memory(0xFF90)}, .output = gbcpp::Location::memory(0xFF91)});
```

The Game Boy register constants (`gbcpp/gb.h`): `gb::A gb::F gb::B gb::C gb::D gb::E gb::H gb::L`
(8-bit) and `gb::AF gb::BC gb::DE gb::HL gb::SP gb::PC` (16-bit). A value's width must match its bound
register (a `uint16_t` bound to `gb::A` throws at registration).

`registerRoutine` validates the binding and throws (`std::invalid_argument`) on an inputs/arity
mismatch, a width/location mismatch, an unknown register, or an inaccessible address — so a malformed
binding fails loudly at registration, not silently at call time.

### Authoring in assembly: register from a `.asm` file

You normally don't hand-write byte arrays — you write the routine as **SM83 assembly in a `.asm`
file** and point `registerRoutine` at it. The VM reads the file and assembles it in-process with an
in-engine SM83 assembler — no external toolchain, no build step, nothing to install:

```cpp
template <typename Sig>
Routine<Sig> Vm::registerRoutine(std::string_view asmFilePath,
                                 const RoutineBinding& binding, int instances = 1);
```

```cpp
// my_add.asm:
//     add a, b
//     ret
auto add = vm.registerRoutine<std::uint8_t(std::uint8_t, std::uint8_t)>(
    "routines/my_add.asm", {.inputs = {gb::A, gb::B}, .output = gb::A});
std::uint8_t s = add(3, 4);   // 7
```

The assembly is RGBDS-flavoured — the same syntax the Game Boy disassembly is written in: `;` line
comments, `label:` definitions, `$hex` / `%bin` / decimal literals, `[hl]` / `[$FF04]` memory
operands, condition codes, and the standard SM83 instruction set. Game Boy hardware registers are
predefined by name, so you write `ldh a, [rDIV]` rather than `ldh a, [$FF04]`; labels are resolved
across the routine (e.g. `jr` targets). The same `Sig` / `binding` rules as the byte form apply, and
the routine's entry is its first byte. A bad mnemonic, a malformed operand, an unknown symbol, or an
unreadable file throws at registration with the offending line — a typo fails loudly, not at call time.

The byte form above (`registerRoutine(span, …)`) is the low-level path the `.asm` form assembles down
to; reach for it only when you already hold assembled bytes.

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
loop.setTick([&](const gbcpp::InputState&) {
    vm.advanceClock(perTick);    // rDIV (and any time-based register) free-runs with engine time
    // ... game logic, which may call rng() ...
});
```

`advanceClock` only advances timing/divider state — registers, RAM, and the RNG seed are untouched.
A routine that reads no time-based register (pure computation — math, decompression) does not need it.

Two `TimingProfile` helpers keep cadence values out of your code (`#include "gbcpp/timing.h"`):

| Helper | Returns | Use |
|---|---|---|
| `cpuCyclesPerTick()` | `std::uint32_t` | cycles to `advanceClock` per tick (0 if the profile has no CPU model) |
| `ticksForDuration(std::chrono::duration)` | `std::uint64_t` | a wall-clock interval as a tick count, e.g. `ticksForDuration(std::chrono::seconds{2})` — the same `uint64_t` `RunLoop::tickCount()` uses |

## Pacing: `Throttle` (and the audio seam)

```cpp
enum class Throttle { HostSpeed, HardwareSpeed };
```

`HostSpeed` runs the routine as fast as the host allows and is byte-identical — correct for RNG, and
the only mode realized today. `HardwareSpeed` (throttle to the CPU clock, for a real-time consumer
like the sound driver) and `instances > 1` (multiple independent copies of a routine) are **declared
seams**: registering with either throws `std::logic_error` today and is realized when the audio chain
lands. They are in the surface now so that work plugs in without an API break.

## The typed callable: `Routine<Sig>`

`registerRoutine` returns a `Routine<Sig>` — a small copyable value handle you call like a function.
It refers back to its `Vm`, so keep the `Vm` alive for as long as you hold the routine (the same
lifetime rule as the renderer's `AtlasId` / `PaletteId`). Arguments and the return value must be
`uint8_t` / `uint16_t` / `uint32_t` (or `void` return).

## Ready-made presets: `gbcpp::sameboy`

Standard original-hardware routines have a fixed convention, so the engine ships them — each is
authored as a `.asm` file the engine assembles and binds for you. You pass nothing but the `Vm&`:

```cpp
auto a = gbcpp::sameboy::divRng(vm);      // ldh a,[rDIV]; ret — a raw DIV read (stateless)
auto b = gbcpp::sameboy::dualSeedRng(vm); // a general-purpose RNG: DIV folded into a dual seed
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
  `src/vm/gameboy/routines/`, then add a factory (declared in `include/gbcpp/gb_routines.h`, defined
  in `src/vm/gameboy/gb_routines.cpp`) that points `registerRoutine` at it and builds the binding —
  mirror `divRng` / `dualSeedRng`.
- **Add register/memory vocabulary for the Game Boy family:** extend `include/gbcpp/gb.h`.
- **Add a whole new system (SNES, NES, …):** add a `src/vm/<system>/` folder with that system's
  backend (and its own ISA assembler + routines), and a factory case — the public `vm.h` surface does
  not change. Every system's machine idiom stays behind its own backend; `vm.h` stays system-agnostic.

## Status

Available: the Game Boy / Game Boy Color backend, `registerRoutine` in both forms (a `.asm` file the
engine assembles in-process, or pre-assembled bytes), the in-engine SM83 assembler, the `Location` /
`RoutineBinding` surface, the `gb::` register vocabulary, the `divRng` / `dualSeedRng` presets,
`advanceClock` (the free-running-divider model), and the host-speed / single-instance path. Declared
seams, not yet realized: the `HardwareSpeed` throttle, `instances > 1`, binding a location by label
name, and non-Game-Boy backends.
