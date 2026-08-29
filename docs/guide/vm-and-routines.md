# VM host & routines

The VM host runs the narrow set of original-machine routines whose output a native re-implementation
**cannot** reproduce exactly — gameplay RNG that reads a free-running hardware register, and a
cycle-driven sound driver — and exposes each as an ordinary typed C++ function. Everything
else in a port is native code; this is the surgical exception.

```cpp
#include "retropp/vm.h"          // VMPlatform, Vm, Routine, Location, RoutineBinding, Throttle
#include "retropp/gb.h"          // gb::Reg + gb::A … gb::PC — the Game Boy register vocabulary
#include "retropp/gb_routines.h" // retropp::sameboy::divRng — the ready-made GB routine preset
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
- [Hosting a resident driver](#hosting-a-resident-driver)
- [Hosting a whole cartridge](#hosting-a-whole-cartridge)
  - [Naming places in it](#naming-places-in-it)
  - [Reading and writing them](#reading-and-writing-them)
  - [Running it](#running-it)
  - [The machine's own memories](#the-machines-own-memories)
  - [Escaping into your own code](#escaping-into-your-own-code)
    - [Replacing a routine the cartridge calls](#replacing-a-routine-the-cartridge-calls)
  - [Calling the cartridge's own routines](#calling-the-cartridges-own-routines)
- [The typed callable: `Routine<Sig>`](#the-typed-callable-routinesig)
- [Ready-made presets: `retropp::sameboy`](#ready-made-presets-retroppsameboy)
- [Where to change things](#where-to-change-things)
- [Status](#status)

## The shape of it

```cpp
retropp::Vm vm{retropp::VMPlatform::GameBoyColor};

// Register a routine ONCE, declaring where its inputs/output live. Then call it like a function.
auto rng = retropp::sameboy::divRng(vm);     // a ready-made preset — no bytes, no addresses
std::uint8_t roll = rng();                // plain C++ at the call site: no register/memory idiom
```

Two ideas carry the whole design:

1. **Call it like a function.** A routine is registered once and thereafter called as a typed C++
   callable. Registers, memory addresses, and entry offsets appear **only** in the registration's
   binding — never at a call site. This is the engine's "no hardware-register variables" principle
   carried to the VM boundary.
2. **The routine path needs no ROM.** A ported game's extracted routines — authored as SM83 `.asm`
   source the engine assembles in-process, or supplied as pre-assembled bytes — run in a cartridge
   the engine builds itself; nothing original is loaded to call them. A game that owns a whole
   cartridge takes the other path: `hostRom` makes the image addressable and `run` makes it live
   (see [Hosting a whole cartridge](#hosting-a-whole-cartridge)). One VM does one or the other.

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

There are two forms, mirroring the audio library's `uploadAudio` / `registerAudio` (and the renderer's
`uploadAtlas` / `loadAtlas`): **`uploadRoutine`** takes ready bytes; **`registerRoutine`** takes a `.asm`
file path. Start with the byte form — the `.asm` form (below) assembles down to it.

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

Both forms validate the binding and throw (`std::invalid_argument`) on an inputs/arity mismatch, a
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
runtime string. A genuinely runtime path is not accepted: read its bytes yourself and use `uploadRoutine`.
The optional `policy` is the same `AssetPolicy` the asset and audio forms use:

- **`Embed`** (default) — use the bytes the build baked into the binary for this logical path. If none
  were baked (no scan ran), it falls through to an on-disk read so the path still works during
  development, and logs a warning naming the routine. Take that warning seriously in a build you intend
  to ship: `Embed` promises the bytecode is inside the binary, and the disk read behind it succeeds only
  where the `.asm` is present. Registering from a static library needs no link settings of its own — see
  [build-and-consume.md](build-and-consume.md#registering-code-in-a-library).
- **`LoadFromPath`** — resolve `asmFilePath` against the engine's single `assetRoot()`, read it at
  registration, and assemble it in-process — the form for a copyright-derived routine you ship beside
  the binary rather than bake in. There is no separate routine root: routines resolve against the same
  `assetRoot()` as `loadAtlas` / `loadMapPng`.

Omit `policy` to take the per-type default (`Embed`); the only way to deviate is the explicit per-call
token, so the policy reads at the call site.

The assembly follows the conventional Game Boy dialect — the same syntax the published disassemblies
are written in: `;` line comments, `label:` definitions, `$hex` / `%bin` / decimal literals, `[hl]` /
`[$FF04]` memory operands, condition codes, and the standard SM83 instruction set. Game Boy hardware registers are
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
loop.simTick([&](const retropp::InputState&) {
    vm.advanceTick();            // rDIV (and any time-based register) free-runs with engine time
    // ... game logic, which may call rng() ...
});
```

`advanceTick()` says *a tick happened* and lets the machine work out what that is worth to it. The
lower-level `advanceClock(cycles)` is still there when you want to spend an explicit amount.

Either way, only timing/divider state advances — registers, RAM, and the RNG seed are untouched. A
routine that reads no time-based register (pure computation — math, decompression) does not need it.

#### A machine running at a cadence that is not its own

`advanceTick()` uses this VM's own cadence, which is the ordinary case. Pass the period explicitly
when the run loop is ticking at something else — hosting a machine from one console while the loop
runs at another console's rate:

```cpp
vm.advanceTick(loop.tickPeriod());   // spend what THIS machine is worth at the loop's actual rate
```

The cycles come from the machine's clock rate and the period actually being run, never from its own
frame count — a frame count is right only when the two cadences coincide. A clock that does not
divide the tick period leaves a fraction of a cycle behind each tick; the fraction is **carried**, not
dropped, so the running total stays exact over any number of ticks and the error at any instant is
under one cycle. Each VM carries its own, so two machines at different rates under one tick simply
carry independently.

At its own cadence a machine uses its stored frame budget instead, and that is deliberate: a Game Boy
frame *is* 70'224 cycles at 4'194'304 Hz, which works out to 16'742'706.3 ns — and a tick period has
to be a whole number of nanoseconds, so deriving the count back out of the stored period would lose a
cycle a frame. The stored count is the exact fact; the period is the rounded one.

Three `TimingProfile` helpers keep cadence values out of your code (`#include "retropp/timing.h"`):

| Helper | Returns | Use |
|---|---|---|
| `cpuCyclesPerTick()` | `std::uint32_t` | this machine's cycles in one tick of its OWN cadence (0 if the profile has no CPU model) |
| `cyclesForTick(period, carry)` | `CycleDraw` | the cycles one tick of `period` is worth, plus the remainder to carry into the next call |
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

## Hosting a resident driver

A sound driver is more than a called routine: it is a **resident machine** — one or more placed code
images, a per-frame tick, and its own RAM — that runs for the life of the system. The normal way to host
one is [`AudioSystem::host`](audio.md#hosting-your-own-sound-driver), which owns the VM and drives the
driver through a typed handle; `Vm` exposes the raw placement + step surface underneath, for hosting a
driver directly.

The driver's shape is a `DriverBinding` (`retropp/driver_binding.h`): its placed `images`, the `tickEntry`
called once per frame, an optional `init` run once, declared `slots`, an optional `stackTop`, and the
`isa`. `Vm::hostDriver(binding)` places the images and runs `init`; `Vm::tickDriver(queued, cyclesPerFrame)`
applies queued gestures, calls the tick, publishes the read-slot snapshot, and idles the rest of the frame
budget; `Vm::readSlot(index)` reads one published slot.

### Placing images, banked when a driver needs it

Each `DriverImage` names its bytes and a **base** address:

```cpp
binding.images = {{.bytes = engineBytes, .base = 0x6000}};                    // a flat image at $6000
binding.images = {{.bytes = bankData,    .base = retropp::gb::banked(0x3A, 0x4000)}};  // bank $3A at $4000
```

`gb::banked(bank, addr)` folds a ROM bank into the base so a driver's banked data lands where its own code
expects it; the VM bank-switches through the **driver's own placed code** (its writes to the mapper
registers), exactly as on hardware — you place the images, the driver drives the banking. The cartridge
mapper is declared on the binding with a hardware constant — `gb::Mbc3`, the common banked-cartridge mapper;
flat images with no banking need none, and the backend sizes the cartridge to hold the highest placed bank.

Placement is validated at `hostDriver`: overlapping images throw, and placing into the boot-ROM-overlaid
low window throws with a clear message. A driver whose audio RAM would collide with the default scratch
stack declares its own `stackTop` on the binding (the default is the platform scratch top).

### The resident tick

Unlike a called routine — which runs to a `ret` and hands back a value — the tick is **call-and-return
within the frame's cycle budget**: `tickDriver` applies any queued slot writes and cued gestures in
submission order, calls `tickEntry`, publishes the read-slot snapshot, then idles the machine for the
remainder of `cyclesPerFrame` so the APU keeps producing at the console's rate. A resident driver is never
called for a return value; its output is sound and its published slots.

**Under `AudioSystem::host` the machine steps on a thread of its own.** The system gives each machine a
runner that owns it: the machine, the mailbox its gestures arrive on, and the step. A verb you issue
crosses to that runner and is performed at the next tick boundary in the order you issued it, and the
runner steps only while the frames waiting downstream of it are under the output's latency target. The
`Vm` itself is untouched by any of this — one thread owns a machine and calls its surface, the same
`hostDriver` / `tickDriver` surface you call yourself when you host a driver directly.

## Hosting a whole cartridge

A game extending an existing cartridge needs that cartridge's content — its art, its tables, its text.
`hostRom` puts the image on the VM and makes every byte of it addressable:

```cpp
Vm::GBC vm;
vm.hostRom(image);          // std::span<const std::uint8_t> — bytes, never a path
```

Bytes rather than a path, so where they came from stays open. Registering the image with
`registerData` and passing `data(id)` is the usual route, and it lands on that family's
`LoadFromPath` default — which keeps a cartridge out of your shipped binary unless you say otherwise.

This makes the image **readable, not running**. There is no boot and no entry point, and no cartridge
metadata is exposed — no title, no mapper, no size. Those are all console-shaped, and you are holding
the bytes already.

Hosting a cartridge and hosting a resident driver are **exclusive**, and each refuses the other.
`hostDriver` synthesizes an image — the engine writes its header and places content into the gaps — so
there the engine owns the cartridge; here your game does. On a hosted cartridge `uploadRoutine` and
`registerRoutine` throw too, since a game's image has no arena to inject into. One VM does one or the
other; use two if you need both.

### Naming places in it

Declare the places you care about as one batch. Every entry is checked when the batch registers, so a
table of two hundred is answered once instead of failing one at a time during play — and a bad batch
names **every** bad entry, not just the first:

```cpp
struct Places {
    MemoryRegion tiles;
    MemoryRegion names;
};

const auto places = vm.registerRegions(regions(
    region(&Places::tiles, MemoryRegion{.at = 0x1000, .size = 16, .count = 384}, "tile art"),
    region(&Places::names, MemoryRegion{.at = 0x2000, .size =  8, .count =  64}, "tile names")));
```

A `MemoryRegion` says where a place starts, how big one entry is, and how many entries follow.
`count` defaults to 1, so a single blob is the ordinary case. The struct is a vocabulary — it is never
instantiated — and naming a field that is not in it is a compile error rather than a bad address.

Each declaration carries a name because a pointer-to-member has none at runtime. For a batch generated
from a disassembly's symbol file, that name is what makes a failure readable.

For a place in a higher bank, qualify the address with `gb::banked(bank, addr16)` — the same encoding
a placed driver image's base uses.

### Reading and writing them

```cpp
const std::vector<std::uint8_t> tile = vm.read(places, &Places::tiles, 12);
vm.write(places, &Places::tiles, patched, 12);
```

The bytes are yours — a plain buffer, not a catalogued handle. Hand it to `uploadData` if you want it
catalogued, or convert it and hand the result to `uploadAtlas`. What the bytes *mean* is never the
engine's business: a tile in the hardware's two-bitplane layout comes back as sixteen bytes, and
decoding them is your code's job.

Entries resolve in the machine's own decoded address space, so **an array longer than a bank reads
correctly across the boundaries**. A few hundred entries of a couple of kilobytes runs far past the
0x4000 switchable window, and `at + index * size` stops describing that run at the first boundary —
which is why the index is a parameter rather than something you compute yourself.

Writing works everywhere, including into a hosted cartridge: the image is a buffer your process owns,
not read-only silicon, and patching one is what extending a cartridge means. The write lands in memory
only — the file the bytes came from is untouched, and re-hosting replaces the image.

Reading a stopped machine gives you the bytes as they are at the moment of the call. A running
machine is observed through its per-step publish instead — see [Running it](#running-it).

Most real content is not tabular — a pointer table names variable-length blobs — so a place can also be
built on the spot from bytes you just read, and used without being declared:

```cpp
const MemoryRegion blob{.at = decodedAddress, .size = decodedLength};
const std::vector<std::uint8_t> bytes = vm.read(blob);
```

Those are checked at the call instead of at registration.

### Running it

A hosted image is readable the moment it is hosted; `run` boots it and lets it live:

```cpp
vm.run();          // boot to the platform's firmware-exit state; run on a thread of its own
vm.speed(2, 1);    // execution speed as a fraction of the hardware's own — {2,1} is double
vm.speed(0, 1);    // {0,1} is paused; the factor is adjustable at any time
vm.stop();         // park where it is — run() again resumes; reset() first for a fresh boot
```

Booting seeds the state the platform's own boot firmware leaves — the boot overlay unmapped so the
cartridge answers everywhere it maps, the mode the image's own header selects, the documented
firmware-exit registers — and execution begins at the cartridge's entry point. On a Game Boy Color
machine, a CGB-flagged image runs in CGB mode and anything else in DMG compatibility, exactly as the
hardware decides it.

The machine paces itself against the wall clock: it owes the platform's cycles-per-second times the
factor, steps while it lags what it owes, and parks when it has caught up — so it holds the
hardware's cadence whether or not your game's loop keeps up, and a factor of `{num, den}` is exact
arithmetic, the sub-cycle remainder carried rather than rounded at any speed. A factor change steers
the machine's next pacing decision; a step already in flight completes first. The machine runs
headless; what it is doing is observed through the declared places.

**While it runs, the declared places are the observable set.** A read answers the latest completed
step's publish — every declared place captured at one instant, coherent with the others — and a
write crosses to the machine's thread and lands at a step boundary, in the order issued, visible in
that boundary's publish. A place built on the spot needs the machine stopped, and so does declaring
more places: register the full set before `run()`.

A running machine belongs to its own thread, so the verbs that mutate it directly — `reset`,
`advanceClock`, `advanceTick`, `hostRom`, `enableAudio` — throw while it runs; `stop()` first. The
machine also stays where it is in memory: `stop()` before moving the `Vm` object.

**Try it:** `examples/rom_run` authors a commercial-shaped little game in-code — interrupt vector,
halt loop, frame counter, an echo cell — hosts it, runs it, and measures it live at `{1,1}`, `{2,1}`,
`{1,2}` and paused; then round-trips a write through the game's own loop and parks and resumes it
mid-count. Headless, and nothing in it comes from anyone's ROM.

### The machine's own memories

`gb.h` ships the hardware's areas as `MemoryRegion` constants, exactly as it ships `gb::A` as a
`Location`:

```cpp
const std::vector<std::uint8_t> vram = vm.read(gb::VRam);
```

`gb::VRam`, `gb::WorkRam`, `gb::Oam`, `gb::Io`, `gb::Hram`. Each has `count == 1`, so reading one with
no index hands back the whole area, and each can be declared in a batch like any other place.

There is deliberately no `gb::Rom`: a cartridge is 32 KiB or a megabyte depending on the image you
host, so a constant would have to be wrong about one of them. Name a place inside it with an address.

**`gb::Io` is raw storage, not the CPU's view of the hardware registers.** Several registers are
*synthesized* when the CPU reads them rather than kept as a byte — `rDIV` at `0xFF04` is answered from
the divider counter, and others carry bits that always read high. A region read hands back what is
stored, which for those is not what a routine reading the same address would see. Read a synthesized
register the way the machine does, through a routine:

```cpp
static constexpr std::array<std::uint8_t, 3> kReadDiv{0xF0, 0x04, 0xC9};  // ldh a,[0xFF04] ; ret
auto divider = vm.uploadRoutine<std::uint8_t()>(kReadDiv, RoutineBinding{.output = gb::A});
```

The plain RAM areas — `gb::VRam`, `gb::WorkRam`, `gb::Oam`, `gb::Hram` — have no such gap; what is
stored is what the CPU reads.

A single byte is just the one-byte case of a place, so whatever a place may name, a routine's memory
binding may name too — the two never disagree about what memory a machine has.

**Try it:** `examples/cartridge_assets` hosts an authored cartridge, declares its tile art and name
table, prints the tiles as characters, patches one in the hosted image, and reads `gb::WorkRam` through
the same verb. The cartridge is written by `assets/gen_cartridge_assets.py`, so the example depends on
no one else's ROM.

### Escaping into your own code

Reading and writing reach into the machine from your side. An escape is the other direction: a place
in the cartridge's own code where control leaves the guest, runs your C++, and resumes.

```cpp
vm.registerEscapes(escapes(
    GuestEscape{.key = "battle start", .at = gb::banked(0x03, 0x4A17), .handler = onBattleStart},
    GuestEscape{.key = "rng draw",     .at = 0xC310,                   .handler = onRngDraw}));
```

One batch, checked once, the way places are: every entry names an address this machine can reach,
carries something to run, and uses a key and an address no other escape has taken. A batch with bad
entries throws naming all of them, each by its key.

A handler takes the machine and the address that fired:

```cpp
void onRngDraw(retropp::Vm& machine, std::uint32_t at) {
    machine.write(places, &Places::lastRoll, roll);
}
```

**The instruction at that address still executes.** The handler runs to completion first, then the
guest executes the instruction it was about to execute, exactly once, whatever the handler did. A
`.handler` escape observes a place in the code; when you want the place *answered* rather than
observed, that is `.replaces` — the second kind, below.

**It costs the guest none of its own time.** The handler runs on your clock, not the machine's — the
guest's cycle count does not advance for it, so a machine with escapes holds the same cadence as one
without.

**It runs on the thread stepping the machine** — your own call for a machine you drive, and the
machine's own thread for one that is running. Whatever a handler touches, the handler owns the
thread-safety of.

**On a running machine a handler reads and writes on the same terms as everything else.** A handler
fires mid-step, so a read answers the last completed step's publish, and a write lands at the next
step boundary. A handler acting on a value your game just wrote sees it one step later, and what a
handler writes becomes readable a step after that. On a machine you step yourself, both are immediate.

Declare escapes before `run()`, or between steps of a machine you drive yourself; declaring on a
machine that is already running throws. Afterwards the machine answers for them by key:

```cpp
vm.escapes()["rng draw"].armed(false);   // declared, dormant
vm.escapes()["rng draw"].armed();        // whether it is live
vm.escapes()["rng draw"].remove();       // drop the declaration entirely
```

Escapes are live when declared. Switching one off keeps its declaration, so switching it back on
needs no re-declaration — and a machine whose escapes are all off costs exactly what a machine with
none costs, because there is nothing left for it to watch.

#### Replacing a routine the cartridge calls

`.replaces` declares that a native function answers **instead of** the routine at that address. While
the escape is live the guest never executes the routine's body; switch it off and the routine is back,
byte for byte.

```cpp
vm.registerEscapes(escapes(GuestEscape{
    .key      = "damage calc",
    .at       = kDamageCalc,
    .replaces = routine(RoutineBinding{.inputs = {gb::B, gb::C}, .output = gb::A},
                        [](std::uint8_t attack, std::uint8_t defense) -> std::uint8_t {
                            return myDamage(attack, defense);
                        })}));
```

The binding is a **transcription of the calling convention the routine already has** — discovered from
the cartridge's own code, not invented. Somewhere in the ROM its callers do this:

```
ld  b, [wAttackStat]    ; the caller loads the arguments
ld  c, [wDefenseStat]
call DamageCalc
ld  [wDamage], a        ; and reads the answer out of A
```

Inputs in B and C, answer in A — so the binding says exactly that. **The pairing is positional**: the
binding's list and the function's parameter list line up index for index, so `inputs[0]` — `gb::B` —
is read from the parked machine and arrives as the first parameter, `attack`; `inputs[1]` — `gb::C` —
arrives as `defense`; the return value goes to `output`. The function never names a register: by the
time it runs the marshalling has happened, and `attack` *is* B's value, as a plain `uint8_t` whose
width came from the signature.

**Which way the values flow.** This is `registerRoutine`'s binding vocabulary with the direction of
the *call* mirrored, and it is worth being exact about, because the words look the same:

| | who is calling | your arguments come from | your result goes to |
|---|---|---|---|
| `Routine<Sig>` — you call guest code | your C++ | your function arguments — the engine writes them **into** the bound inputs | the engine reads it **out of** the bound output for you |
| `.replaces` — guest code calls you | the cartridge | the bound inputs — the guest's own caller loaded them, the engine reads them **out** for you | the engine writes it **into** the bound output, where the guest's callers read |

When you replace a routine, the ROM is the caller and your function is the callee: the values in B
and C are whatever *that call site* loaded, and your answer lands in the register its compiled code
was always going to read. A different call site invoking the same routine gets the same treatment —
your function answers for all of them, exactly as the original routine did.

**The marshalling is synchronous.** Unlike a `.handler`'s reads and writes, which ride the publish and
the step boundary, a replacement is marshalled by the engine while the machine is parked at the entry
— so the caller reads a correct answer in the *same step*, and register conventions work. Routines
whose convention is memory-based bind memory the same way: `Location::memory(0xC000)` in place of a
register.

What remains yours: the replacement answers for **every** caller of that routine, and whatever the
original promised its callers — every output, every side effect — is your function's to establish
through the binding.

**Try it:** `examples/guest_escape` hosts an authored cartridge, runs it, and escapes at a place in
its loop to count what the guest is doing from C++ — then declares the cartridge's own routine
replaced and answers it natively, in the routine's own registers, so the guest goes on running against
a result its code never produced. Headless, and the cartridge is written in-code.

### Calling the cartridge's own routines

`bindRoutine` names a routine that is **already there** — in the hosted image, or in code this VM was
given earlier — and hands back the same `Routine<Sig>` the other two forms do. `uploadRoutine` hands
over bytes, `registerRoutine` hands over a file, `bindRoutine` names an address:

```cpp
auto random = vm.bindRoutine<std::uint8_t()>(gb::banked(1, 0x5A1C),
                                             RoutineBinding{.output = gb::A});
const std::uint8_t roll = random();
```

The binding is the same transcription `.replaces` uses, read the other way round: it says where this
routine expects its arguments and where it leaves its answer, discovered from the cartridge's own
code. `throttle` and `entryOffset` have no meaning for a routine that is already in place — the
address *is* the entry — so a binding that sets either is refused.

Bind before `run()`, or between steps of a machine you drive yourself — the address and the binding
are checked against the machine as it stands, so host the cartridge first, and binding against a
machine already running throws. These are the terms `registerRegions` and `registerEscapes` are on.
An unreachable address, or any of `uploadRoutine`'s binding failures, throws `std::invalid_argument`.
Code in work RAM or high RAM is as callable as code in a cartridge — a routine a game copies into
high RAM to run it there is bound the same way.

**The call runs in the guest's own context.** The registers and the stack the routine finds are the
ones the machine already had; its return frame is pushed where the guest's own `call` would push it;
and every register is back the way it was by the time the call returns. Code that was interrupted
mid-instruction carries on without noticing. What the routine changed in **memory** stands — a seed it
advanced, a buffer it decoded — because that is the answer it exists to give.

**Where you may call one from:**

- from an escape handler or a replacement's function, on the machine's own thread. The routine may
  itself run through a place that escapes, whose handler calls another routine, to any depth;
- from your own thread while the machine is **stopped**, or has never run at all — which is how you
  reach content a cartridge stores in a format only its own code understands, without playing the game
  to the point that unpacks it.

Calling one on a running machine from any other thread throws: stop the machine first, or make the
call from a handler.

**What it costs the guest** is exact: the routine's own instructions, entry through its return, plus
one instruction fetch for the address the return lands on. That is guest time, and it is counted where
the machine was already being run from. A `.handler` that calls nothing costs the guest nothing; a
handler that calls a routine costs it that routine.

**A routine that never returns is abandoned** rather than left to hang: the register file comes back
and the guest carries on, and what the routine had already written to memory stays written.

**An interrupt arriving inside a routine is the guest's own.** It runs on the guest's stack, inside
the call, and the call still returns — which is what the hardware does.

**A bank-qualified address is callable while its own bank is the mapped one**, and throws naming both
banks otherwise. The guest's own code is what selects the bank it calls into.

**Try it:** `examples/guest_nesting` authors a cartridge with three routines of its own — a damage
rule its loop asks every frame, a generator that advances a seed it keeps, and a decompressor the loop
never calls. A replacement answers the damage rule natively and builds its answer by calling the
cartridge's own generator nested inside the escape, while the machine runs on its own thread; then,
with the machine parked, the program calls the cartridge's own decompressor on a packed table the game
never reached. Headless, and the cartridge is written in-code.

**Try the whole surface at once:** `examples/coexecution` is windowed, and every verb on this page acts
on one picture. The cartridge keeps eight walkers in its work RAM and marches them each frame; the demo
draws them from the per-step publish. Under the cartridge's own pace rule they hold a column, and with
`.replaces` armed they scatter — the native rule builds each walker's pace by calling the cartridge's
own generator, nested inside the escape. The other keys write a byte into the cartridge image while it
runs, move the speed factor, park and resume, drop the observing escape's declaration, and — with the
machine parked — call the cartridge's own decompressor and decode its own pointer table.

## The typed callable: `Routine<Sig>`

`registerRoutine` returns a `Routine<Sig>` — a small copyable value handle you call like a function.
It refers back to its `Vm`, so keep the `Vm` alive for as long as you hold the routine (the same
lifetime rule as the renderer's `AtlasId` / `PaletteId`). Arguments and the return value must be
`uint8_t` / `uint16_t` / `uint32_t` (or `void` return).

## Ready-made presets: `retropp::sameboy`

A routine gets a preset only when it is a **hardware technique rather than an algorithm** — an
operation whose form is dictated by the instruction set and the register it touches, so any
independent implementation writes the same instructions. Those the engine ships: authored as a `.asm`
file the build assembles to bytecode and bakes into the binary, then registered and bound for you.
You pass nothing but the `Vm&`:

```cpp
auto rng = retropp::sameboy::divRng(vm);  // ldh a,[rDIV]; ret — a raw DIV read (stateless)
std::uint8_t x = rng();
```

`divRng` returns the free-running DIV register as a random byte. It is stateless, so the stream is
only as varied as the divider is — advance the clock between calls or it barely moves.

**Anything with design choices in it is yours, not the engine's.** A mixing scheme, a seed layout, a
compression format — those belong to the game that authors them, and the engine's job is the machine,
the binding and the clock. Write the `.asm`, point `registerRoutine` at it, and it is baked into your
binary exactly as a preset is. `examples/vm_routines` does that with an RNG of its own: an 8-bit
xorshift over a high-RAM seed it picks and seeds itself, mixed with the divider on the way out, shown
side by side with `divRng` so the two paths are visible together.

A purely-algorithmic PRNG that reads no hardware register belongs in neither place: it is
byte-reproducible in plain C++, so it is a porting job rather than a VM routine.

## Where to change things

- **Add a routine preset for the Game Boy family:** write the routine as a `.asm` file under
  `src/vm/gameboy/routines/`, add it to the build's routine-bake list so it is assembled to compile-time
  bytecode (`src/vm/gameboy/gb_routine_bytecode.h`), then add a factory (declared in
  `include/retropp/gb_routines.h`, defined in `src/vm/gameboy/gb_routines.cpp`) that registers the baked
  byte span with `uploadRoutine` and builds the binding — mirror `divRng`. Only add one if the
  routine is a hardware technique; an algorithm belongs to the game, registered from its own `.asm`.
- **Add register/memory vocabulary for the Game Boy family:** extend `include/retropp/gb.h` — CPU
  registers as `Location` constants, hardware memories as `MemoryRegion` constants. A new memory also
  needs the backend to serve it: a `GbHardwareMemory` value, its direct-access mapping in
  `src/vm/gameboy/sameboy_machine.cpp`, and its range in `regionFor` / `regionIsAddressable`
  (`src/vm/gameboy/sameboy_backend.cpp`). Updating one and not the others reddens the suite.
- **Add a whole new system (SNES, NES, …):** add a `src/vm/<system>/` folder with that system's
  backend (and its own ISA assembler + routines), and a factory case — the public `vm.h` surface does
  not change. Every system's machine idiom stays behind its own backend; `vm.h` stays system-agnostic.

## Status

Available: the Game Boy / Game Boy Color backend, both registration forms — `uploadRoutine`
(pre-assembled bytes) and `registerRoutine` (a `.asm` file the engine assembles in-process, with the
`Embed` / `LoadFromPath` policy) — the in-engine SM83 assembler, the `Location` / `RoutineBinding`
surface, the `gb::` register vocabulary, the `divRng` preset, `advanceTick` / `advanceClock` (the
free-running-divider model), the host-speed / single-instance path, the `HardwareSpeed` throttle
(driving the [audio chain](audio.md)), and the resident-driver surface (`hostDriver` / `tickDriver` /
`readSlot`, with banked placement via `gb::banked` + `gb::Mbc3`), and cartridge hosting (`hostRom`
with the `MemoryRegion` / `registerRegions` / `read` / `write` surface and the `gb::` memory
constants). Declared seams, not yet realized: `instances > 1`, binding a
location by label name, and non-Game-Boy backends.
