#pragma once

// The VM host public API — the engine's runtime virtual-machine surface for the narrow
// set of original routines whose output cannot be faithfully native-ported (gameplay RNG; later a
// sound driver).
//
// This header is PLATFORM-AGNOSTIC. It selects a per-system backend (VMPlatform) and exposes a
// generic, function-like call surface; the per-system vocabulary a routine binding names — the CPU
// register set, the memory map — lives in a platform-specific header (the Game Boy / SM83 family in
// retropp/gb.h; other systems are drop-in). The call surface is identical across systems because each
// routine's convention is sealed in its binding.
//
// A consumer registers a surgically-extracted routine ONCE, declaring where each input and the
// output live (a CPU register or an absolute memory address), and thereafter calls it as an ordinary
// typed C++ function:
//
//     retropp::Vm vm{retropp::VMPlatform::GameBoyColor};
//     auto rng = vm.uploadRoutine<std::uint8_t()>(routineBytes, {.output = retropp::gb::A});
//     std::uint8_t roll = rng();          // no register / memory / address idiom at the call site
//
// This carries the engine's "no hardware-register variables exist in the port" principle to the VM
// boundary: registers, memory addresses, and entry offsets appear ONLY inside a routine's binding —
// never where a routine is called.
//
// NO GAME ROM. This is a port, not an emulator: no game ROM is loaded or executed. The only original
// code that runs in the VM is the narrow correctness-impossibility set — gameplay RNG, and a game's own
// sound driver — supplied as surgically-extracted byte images (embedded at build time, or read from a
// path when they cannot be embedded) and PLACED at declared addresses in the VM's code space. A driver
// image may be bank-qualified, and the VM bank-switches through its own placed code; there is no
// Vm::loadRom, and no game code beyond that sanctioned set ever runs.
//
// The header pulls NO backend type (no SameBoy GB_*, no SM83 register enum): the template callable
// converts typed arguments to width-tagged values and delegates the machine work to non-template Vm
// members defined in vm.cpp, which dispatch through the abstract backend seam.

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

#include "retropp/asset_policy.h"      // AssetPolicy (registerRoutine's Embed / LoadFromPath choice)
#include "retropp/driver_binding.h"    // DriverBinding / Instruction — the resident-driver surface below
#include "retropp/isa.h"               // Isa + the VMPlatform → Isa mapping below
#include "retropp/literal_path.h"      // LiteralPath (registerRoutine takes a compile-time literal path)
#include "retropp/location.h"          // Location — the register / memory value-home vocabulary
#include "retropp/memory_region.h"     // MemoryRegion — where a declared place in the guest lives
#include "retropp/timing.h"

namespace retropp {

// The target system whose VM backend runs the routine. Each enumerator selects a per-system backend;
// the call surface is identical across systems because each routine's convention is sealed in its
// binding. GameBoy / GameBoyColor map to the SM83 / SameBoy backend — the only backend built in v1.
// Any other enumerator throws at Vm construction ("no backend built in v1"); it is a drop-in when a
// consumer exercises it (the ViewportResolution::Snes precedent). Extend this list as systems land.
enum class VMPlatform { GameBoy, GameBoyColor, Snes, Nes, Genesis, MasterSystem };

// The ISA a VM of `platform` runs — the assembler it uses and the byte format it accepts. Several
// platforms can share one ISA (the Game Boy and Game Boy Color both run SM83), which is why a chiptune's
// compatibility is keyed on the ISA, not the exact platform. The audio system uses this to verify, at
// play(), that a catalog entry's (developer-selected) ISA matches the VM it is being cued on. Unbuilt
// platforms have no backend (Vm construction throws), so their mapping is a placeholder for now.
[[nodiscard]] constexpr Isa isaFor(VMPlatform platform) noexcept {
    switch (platform) {
        case VMPlatform::GameBoy:
        case VMPlatform::GameBoyColor:
            return Isa::Sm83;
        case VMPlatform::Snes:
        case VMPlatform::Nes:
        case VMPlatform::Genesis:
        case VMPlatform::MasterSystem:
            break;  // ISA added with the backend; unreachable today (Vm ctor throws for these)
    }
    return Isa::Sm83;
}

// How a routine is paced. HostSpeed runs the routine as fast as the host allows — the form for a
// routine you CALL for a return value (RNG). HardwareSpeed throttles to the CPU clock for a real-time
// consumer (a continuously-running audio driver), which is stepped via startDriver / stepDriver rather
// than called for a value.
enum class Throttle {
    HostSpeed,
    HardwareSpeed,
};

// The developer-declared I/O binding: the ONLY place registers / memory / entry offsets are named.
// The WIDTH of each input and the output comes from the callable signature (sizeof), not from here;
// the binding names only WHERE each value lives and HOW the routine is paced.
//   RoutineBinding{ .inputs = {gb::A, gb::B}, .output = gb::A, .throttle = Throttle::HostSpeed }
struct RoutineBinding {
    std::vector<Location>   inputs;                  // argument i marshals to inputs[i]
    std::optional<Location> output{};               // return value reads from output (nullopt = void)
    Throttle                throttle = Throttle::HostSpeed;
    std::uint32_t           entryOffset = 0;         // first instruction's offset WITHIN the supplied
                                                     // routine bytes (usually 0). NOT a ROM address.
};

// Compile-time constraint on the values a routine I/O location can hold: the unsigned-integral
// widths a console CPU register or memory location carries (8 / 16 / 32-bit). The selected backend
// further constrains a value bound to one of ITS registers to that register's actual width.
template <typename T>
inline constexpr bool kIsVmValue =
    std::is_same_v<T, std::uint8_t> || std::is_same_v<T, std::uint16_t> ||
    std::is_same_v<T, std::uint32_t>;

class Vm;

// A typed handle to a registered routine: call it like a plain function. Copyable value handle that
// holds a non-owning Vm* + the routine's handle within that Vm — valid only while the owning Vm is
// alive (the same lifetime contract as AtlasId / PaletteId; do not outlive or move the owning Vm).
template <typename Sig>
class Routine;  // primary left undefined — only function-type specializations are valid

template <typename Ret, typename... Args>
class Routine<Ret(Args...)> {
    static_assert((kIsVmValue<Args> && ...),
                  "Routine arguments must be uint8_t, uint16_t, or uint32_t");
    static_assert(std::is_void_v<Ret> || kIsVmValue<Ret>,
                  "Routine return type must be void, uint8_t, uint16_t, or uint32_t");

public:
    Routine() = default;  // empty handle; calling it is undefined (no Vm)

    Ret operator()(Args... args) const;

private:
    friend class Vm;
    Routine(Vm* vm, std::size_t handle) noexcept : vm_(vm), handle_(handle) {}

    Vm* vm_ = nullptr;
    std::size_t handle_ = 0;
};

// A marshalled call value crossing from the typed template into the non-template VM core: the value
// zero-extended to 64 bits + its width in bytes. Internal to the call path; consumers never build one.
struct CallValue {
    std::uint64_t value;
    int width;  // 1, 2, or 4
};

// ── Naming places in the guest's address space ──────────────────────────────────────────────────
//
// A game declares the places it cares about in a machine as ONE batch, and the engine checks every
// entry when the batch is registered — so a table of two hundred places is answered once, not one
// failure at a time deep in gameplay.
//
// The keys are fields of a game-defined struct, named by pointer-to-member, so a typo is a compile
// error rather than a bad address. The struct is a vocabulary, never instantiated: it exists so the
// places have names.
//
//   struct Places {
//       MemoryRegion tileArt;
//       MemoryRegion textTable;
//   };
//
//   const auto places = vm.registerRegions(regions(
//       region(&Places::tileArt,   MemoryRegion{.at = gb::banked(2, 0x4000), .size = 16, .count = 384},
//              "tile art"),
//       region(&Places::textTable, MemoryRegion{.at = 0x3000, .size = 32, .count = 64},
//              "text table")));
//
// The batch is not an asset table — it is the places this game cares about in this machine. One
// declared field is read and written through the same key.

// One declared place bound to a game struct field. Templated on the struct so every entry in one
// regions(...) batch belongs to the SAME struct — a field of another type does not compile.
template <class S>
struct RegionBinding {
    MemoryRegion S::* key;
    MemoryRegion      where;
    std::string_view  name;
};

// Bind a game struct field to a place in the guest's address space. `name` is what a registration
// failure reports: a pointer-to-member carries no name at runtime, so without it a bad entry can only
// be identified by its address — and for a batch generated from a symbol file, that is materially
// worse than the name the generator already had.
template <class S>
[[nodiscard]] RegionBinding<S> region(MemoryRegion S::* member, MemoryRegion where,
                                      std::string_view name) {
    return RegionBinding<S>{.key = member, .where = where, .name = name};
}

// The declared batch for one machine.
template <class S>
struct RegionMap {
    std::vector<RegionBinding<S>> bindings;
};

// Collect one or more region() bindings into a RegionMap<S>. S is deduced from the first binding;
// every other must name a field of that same struct.
template <class S, class... Rest>
[[nodiscard]] RegionMap<S> regions(RegionBinding<S> first, Rest... rest) {
    RegionMap<S> out;
    out.bindings.reserve(1 + sizeof...(rest));
    out.bindings.push_back(std::move(first));
    (out.bindings.push_back(std::move(rest)), ...);
    return out;
}

// A registered batch on one VM, remembering the struct its keys come from. Hold it and name places
// through it; it carries the declaration order the keys resolve against.
template <class S>
class RegionMapId {
public:
    RegionMapId() = default;  // empty handle; naming a place through it is undefined (no Vm)

    // The place a key was declared to name, or nullopt if the key is not in this batch.
    [[nodiscard]] std::optional<MemoryRegion> declared(MemoryRegion S::* member) const {
        for (std::size_t i = 0; i < keys_.size(); ++i) {
            if (keys_[i] == member) {
                return declarations_[i];
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] std::size_t size() const noexcept { return keys_.size(); }

private:
    friend class Vm;
    RegionMapId(std::size_t handle, std::vector<MemoryRegion S::*> keys,
                std::vector<MemoryRegion> declarations) noexcept
        : handle_(handle), keys_(std::move(keys)), declarations_(std::move(declarations)) {}

    std::size_t                     handle_ = 0;
    std::vector<MemoryRegion S::*>  keys_;
    std::vector<MemoryRegion>       declarations_;
};

// One declared place flattened for the non-template registration core: where it is and what to call
// it in an error. Internal to the registration path; consumers never build one.
struct DeclaredRegion {
    MemoryRegion     where;
    std::string_view name;
};

// The VM host. Owns one backend machine (selected by VMPlatform); routines registered on it share
// its memory (so RNG seed state persists across calls). Non-copyable (owns a machine); movable.
class Vm {
public:
    explicit Vm(VMPlatform platform, TimingProfile timing = TimingProfile::GameBoyColor);
    ~Vm();

    // Nested platform-bound instantiation types — the CPU-half symmetric spelling of
    // AudioSystem::{GB,GBC} (the all-caps hardware vocabulary of the driver-hosting design). Each fixes the
    // console (VMPlatform + TimingProfile) so a consumer names the hardware once at the type and never
    // repeats it at construction: `retropp::Vm::GBC vm;` is `Vm{VMPlatform::GameBoyColor,
    // TimingProfile::GameBoyColor}`. They ARE Vms (they add no state, only the pre-binding) — use one
    // anywhere a Vm is expected. Platform namespaces (gb::, …) stay HARDWARE vocabulary only; a system type
    // never lives in one.
    class GB;
    class GBC;

    Vm(const Vm&) = delete;
    Vm& operator=(const Vm&) = delete;
    Vm(Vm&&) noexcept;
    Vm& operator=(Vm&&) noexcept;

    // The system this VM hosts (the one passed at construction).
    [[nodiscard]] VMPlatform platform() const noexcept;

    // Reset the machine to its post-reset state — clears persistent routine state (e.g. RNG seeds).
    // Registered routines stay registered (their bytes live in the VM's code space, untouched).
    void reset();

    // Advance the machine's free-running clock by `cycles` CPU cycles WITHOUT executing a routine —
    // so hardware registers a routine reads (e.g. the Game Boy's rDIV divider) keep ticking BETWEEN
    // calls, exactly as they do on always-running hardware. This is what makes a hardware-RNG host
    // faithful: rDIV must reflect the time elapsed since the last call, or the RNG degenerates into a
    // counter. Drive it from the host's clock — one tick's worth of cycles per engine tick, which the
    // timing profile already defines: pass TimingProfile::cpuCyclesPerTick() (no hardcoded count).
    // Calling it is optional: a routine that reads no time-based register (pure computation) does not
    // need it.
    void advanceClock(std::uint64_t cycles);

    // Advance the machine by exactly one engine tick's worth of ITS OWN cycles, carrying the
    // sub-cycle remainder from tick to tick. Say "a tick happened" and let the machine work out what
    // that is worth to it, rather than computing a cycle count at the call site.
    //
    // This is what makes a machine hosted at a cadence that is not its own stay honest. The cycles a
    // tick is worth come from the machine's clock rate and the period actually being run — never from
    // the machine's own frame count, which is right only when the two cadences coincide. A guest whose
    // clock does not divide the tick period leaves a fraction of a cycle behind every tick; the
    // fraction is kept and spent later, so the running total is exact over any number of ticks and the
    // instantaneous error never exceeds one cycle.
    //
    // Pass the period the run loop is actually ticking at. The no-argument form uses this VM's own
    // profile cadence, which is the common case: a machine running at its native rate.
    //
    // Does nothing if this VM's timing profile carries no CPU model.
    void advanceTick(std::chrono::nanoseconds enginePeriod);
    void advanceTick();

    // ── Audio chain (the hardware-speed driver path) ────────────────────────────────────────────────
    // The narrow set of original routines that produce sound run as continuously-executing DRIVERS at
    // the hardware CPU clock (Throttle::HardwareSpeed), their APU register writes synthesizing PCM at
    // the original cadence — distinct from a HostSpeed routine that is CALLED for a return value (RNG).
    // These three members are that path: enable the APU + sink once, position the driver, then step it
    // one cycle budget per sim tick. (The cue surface a game drives by meaning is the AudioSystem; this
    // is the raw chain it sits on.)

    // Enable the backend's APU and route each produced stereo PCM frame to `onSample` (called per
    // sample on the thread that steps the driver — for the audio chain, the AudioSystem's production
    // thread). The APU's sample rate is set to
    // `sampleRate` so it resamples to the sink rate internally. Call once before driving a routine.
    void enableAudio(unsigned sampleRate,
                     std::function<void(std::int16_t left, std::int16_t right)> onSample);

    // Position a hardware-speed driver routine to run continuously (PC → its entry). It is not run to a
    // return for a value — stepDriver advances it. `driver` must be registered on THIS Vm with
    // Throttle::HardwareSpeed (throws otherwise).
    void startDriver(const Routine<void()>& driver);

    // Run the started driver for `cpuCycles` CPU cycles (the TimingProfile CPU unit — pass
    // TimingProfile::cpuCyclesPerTick() once per sim tick); the APU produces ~rate/frameRate frames
    // into the enabled sink during the run. Returns the CPU cycles actually run.
    std::uint64_t stepDriver(std::uint64_t cpuCycles);

    // Host a whole cartridge image on THIS VM: hand over the image's BYTES and the machine comes up
    // on them, with every byte of the cartridge addressable. Use it to reach content that already
    // exists inside a game's own cartridge — art, tables, text — and feed it to the ingestion
    // surfaces, converting first if the format needs it.
    //
    // BYTES, NEVER A PATH. A path would force one delivery policy; bytes take either. Register the
    // image with `registerData` and pass `data(id)`, or read it however the game likes.
    //
    // This makes the image READABLE, not running: there is no boot and no entry point. The backend
    // parses the image's own header, and the engine exposes no cartridge metadata — no title, no
    // mapper, no size. Every one of those is console-shaped, and the caller holds the bytes.
    //
    // Hosting a game's cartridge and hosting an engine-built one are EXCLUSIVE, and each refuses the
    // other. hostDriver synthesizes a cartridge — the engine writes its header and places content
    // into the gaps — so the engine owns that image; here the game does. On a hosted cartridge,
    // uploadRoutine / registerRoutine throw as well: there is no arena to inject into. One VM does
    // one or the other.
    //
    // Throws std::invalid_argument for an empty image, or std::logic_error if this VM already hosts
    // a driver.
    void hostRom(std::span<const std::uint8_t> rom);

    // Declare the places in this machine the game cares about, as one batch, and get back the handle
    // that names them. Every entry is checked here — reachable on this machine, and wholly contained
    // in the memory it starts in — so the batch is answered once instead of one failure at a time
    // during play. A batch with bad entries throws naming ALL of them, each by its declared name; a
    // report that stops at the first is what makes a generated two-hundred-entry table painful.
    //
    // Regions are checked against the machine as it stands, so host the cartridge first — a place
    // inside an image that has not been loaded is not reachable yet.
    //
    // Throws std::invalid_argument (an empty batch, or any entry that does not fit).
    template <class S>
    [[nodiscard]] RegionMapId<S> registerRegions(const RegionMap<S>& map);

    // Read one entry of a declared place, and write one back. The bytes are the caller's — a plain
    // buffer, not a catalogued handle, because minting one for a pile of bytes about to be converted
    // and discarded is ceremony. Hand the result to uploadData if it should be catalogued, or to
    // uploadAtlas after converting it.
    //
    // `index` names which entry of the place to move; a place declared with the default count of 1
    // has only entry 0, which is the whole of it. An index the place does not declare throws.
    // Entries are resolved in the machine's decoded address space, so an array longer than a bank
    // reads correctly across the boundaries rather than running off the end of the first one.
    //
    // WRITING IS ALLOWED EVERYWHERE, including into a hosted cartridge: the image is a buffer this
    // process owns, and patching one is a thing a game extending an existing cartridge legitimately
    // does. The write lands in memory only — the file the bytes came from is untouched, and
    // re-hosting replaces the image.
    //
    // Reading a machine that is running gives the bytes as they are at the moment of the call.
    //
    // Throws std::invalid_argument if the key is not in `map` or the byte count is not one entry,
    // std::out_of_range for an index the place does not declare.
    template <class S>
    [[nodiscard]] std::vector<std::uint8_t> read(const RegionMapId<S>& map, MemoryRegion S::* key,
                                                 std::uint32_t index = 0);
    template <class S>
    void write(const RegionMapId<S>& map, MemoryRegion S::* key,
               std::span<const std::uint8_t> bytes, std::uint32_t index = 0);

    // The same verbs against a place built on the spot rather than declared. Much real content is
    // not tabular — a pointer table points at variable-length blobs, so reaching one means reading
    // the table, decoding an entry, and building a place from what was just read. These forms are
    // checked at the call instead of at registration.
    [[nodiscard]] std::vector<std::uint8_t> read(const MemoryRegion& where, std::uint32_t index = 0);
    void write(const MemoryRegion& where, std::span<const std::uint8_t> bytes,
               std::uint32_t index = 0);

    // ── Resident driver (the hosted-machine path) ───────────────────────────────────────────────
    // A hosted sound driver is richer than a single startDriver routine: N placed images (optionally
    // banked), a per-frame tick entry, declared state slots, and player verbs realized as Instructions.
    // These members are the machine-layer mechanics the audio surfaces compose (AudioSystem::host →
    // HostedDriver). A game does not name them directly; it declares a DriverBinding and acts through
    // the durable handle.

    // Configure THIS VM's machine as a resident-driver host from `binding`: build a cartridge image
    // sized to hold the highest placed bank, install the mapper, place each image at its (possibly
    // bank-qualified) base, relocate the scratch stack to the declared top, then perform the binding's
    // `init` gesture once (the engine-run .init). The ISA is verified against this VM's platform.
    // After this, tickDriver / readSlot drive the resident machine. Throws (std::invalid_argument /
    // std::runtime_error) on: an ISA mismatch, a banked placement with the none mapper, overlapping
    // placed ranges, placement into the boot-ROM window or the engine-reserved header gap, a stack top
    // outside work RAM, or a cartridge the backend cannot address. Uploaded routines already placed on
    // this VM are preserved (the arena and the placed images share one space).
    void hostDriver(const DriverBinding& binding);

    // Run one resident-driver frame: perform each queued Instruction in submission order (mailbox
    // writes and entry calls), call the tick entry to its return, then idle the machine for the
    // remainder of `cyclesPerFrame` (pass TimingProfile::cpuCyclesPerTick()) so the APU synthesizes at
    // the hardware cadence. Returns the CPU cycles consumed by the performed instructions + the tick
    // call (the idle pads the frame to `cyclesPerFrame`). Read the published slots afterwards with
    // readSlot. Throws std::logic_error if no driver is hosted on this VM.
    std::uint64_t tickDriver(std::span<const Instruction> queued, std::uint64_t cyclesPerFrame);

    // Read a declared slot's current value from the hosted machine (slot `index` is the i-th SlotSpec
    // in the hosted binding, in declaration order). The value's width is the slot's declared width.
    // Throws std::logic_error if no driver is hosted, or std::out_of_range for a bad index.
    [[nodiscard]] std::uint64_t readSlot(std::size_t index);

    // Register a surgically-extracted routine from its EMBEDDED BYTES (a build-time `const` array)
    // + its I/O binding, returning a typed callable. The engine injects the bytes into the VM's code
    // space; there is no ROM. `instances` is a declared seam — only 1 is realized; registering with
    // more throws (multi-instance routing for anti-channel-stealing audio is not built yet). The
    // signature determines I/O widths; the binding determines I/O locations and pacing. Throws on:
    // instances > 1, an inputs/arity mismatch, a width/location mismatch, an unknown register for the
    // backend, or an exhausted code arena.
    template <typename Sig>
    Routine<Sig> uploadRoutine(std::span<const std::uint8_t> routineBytes,
                               const RoutineBinding& binding, int instances = 1);

    // Register a routine from a `.asm` FILE (the mirror of loadAtlas): hand over a
    // compile-time LITERAL logical path (never bytes, never a runtime string), and the engine resolves
    // it by the embed/load `policy`. The literal is what a build-time scan can find to bake an Embed
    // routine; a genuinely runtime path is not supported here — read its bytes yourself and use uploadRoutine.
    //   * Embed (default)    — use the bytes the build baked into the binary for this logical path (the
    //                          routine registry). If none were baked (no scan ran), fall
    //                          through to the on-disk read so the path still works during development.
    //   * LoadFromPath       — read `assetPath(path)` at registration and assemble it in-process
    //                          with this VM's platform assembler (the Game Boy family → SM83, the
    //                          engine's own — NO external toolchain), for a copyright-derived routine.
    // `policy` precedence: per-call > the per-type default (Embed).
    // `binding`/`instances`/the signature mean exactly what they do for uploadRoutine; entry is offset 0
    // (a leaf routine). Throws if the file cannot be opened, on a source error (with line context), or
    // on any of the byte form's validation failures.
    template <typename Sig>
    Routine<Sig> registerRoutine(LiteralPath asmFilePath, const RoutineBinding& binding,
                                 std::optional<AssetPolicy> policy = {}, int instances = 1);

    // Assemble assembly SOURCE into machine-code bytes for THIS VM's ISA (the Game Boy family → SM83) —
    // the VM's platform alone decides the ISA, so the right assembler is always selected. A source →
    // bytes transform, NOT a path or registration call: it is how a consumer holding routine/audio
    // source obtains bytes to hand to uploadRoutine (the "runtime need ⇒ hand raw bytes" path for
    // source). The audio system uses it to materialize a LoadFromPath chiptune. Throws on a source error.
    [[nodiscard]] std::vector<std::uint8_t> assemble(std::string_view source);

private:
    template <typename Sig>
    friend class Routine;

    // Non-template core (defined in vm.cpp). registerResolved validates + places the bytes through
    // the backend + stores the resolved binding, returning its handle; invoke sets up the call
    // frame, marshals inputs, runs to return, and reads the output — all via the backend seam.
    // registerRegions' non-template core: validate every declared place against the machine and
    // store the batch, returning its handle. Throws naming every entry that failed.
    std::size_t registerRegionsResolved(std::span<const DeclaredRegion> declared);

    std::size_t registerResolved(std::span<const std::uint8_t> routineBytes,
                                 const RoutineBinding& binding,
                                 std::span<const int> inputWidths,
                                 int outputWidth, int instances);
    // registerRoutine's non-template core: resolve the embed/load policy for `logicalPath`, then either
    // place the build-baked bytes (Embed) or read `assetPath(logicalPath)` + assemble it (LoadFromPath
    // or an un-baked Embed), placing + resolving as registerResolved does.
    std::size_t registerRoutineResolvingPolicy(std::string_view logicalPath, const RoutineBinding& binding,
                                               std::optional<AssetPolicy> policy,
                                               std::span<const int> inputWidths, int outputWidth,
                                               int instances);
    std::uint64_t invoke(std::size_t handle, std::span<const CallValue> inputs);

    // Perform one declared Instruction on the hosted resident driver: a mailbox write (returns 0), or
    // an entry call run to return with the given cycle cap (returns the CPU cycles consumed). The value
    // it carries is the Instruction's fixed value when set, else 0 — the audio handle bakes the play(id)
    // value into a fixed-value clone before queuing, so a queued Instruction is always fully determined.
    std::uint64_t performInstruction(const Instruction& instruction, std::uint64_t cycleCap);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// The nested platform-bound Vm types (declared above): a Game Boy and a Game Boy Color VM with their
// platform + timing pre-bound. No new state — construction is the only thing they fix.
class Vm::GB : public Vm {
public:
    GB() : Vm(VMPlatform::GameBoy, TimingProfile::GameBoy) {}
};

class Vm::GBC : public Vm {
public:
    GBC() : Vm(VMPlatform::GameBoyColor, TimingProfile::GameBoyColor) {}
};

// ── Template definitions ──────────────────────────────────────────────────────────────────────

template <class S>
RegionMapId<S> Vm::registerRegions(const RegionMap<S>& map) {
    std::vector<DeclaredRegion>    flat;
    std::vector<MemoryRegion S::*> keys;
    std::vector<MemoryRegion>      declarations;
    flat.reserve(map.bindings.size());
    keys.reserve(map.bindings.size());
    declarations.reserve(map.bindings.size());
    for (const RegionBinding<S>& b : map.bindings) {
        flat.push_back(DeclaredRegion{.where = b.where, .name = b.name});
        keys.push_back(b.key);
        declarations.push_back(b.where);
    }
    const std::size_t handle = registerRegionsResolved(flat);
    return RegionMapId<S>{handle, std::move(keys), std::move(declarations)};
}

template <class S>
std::vector<std::uint8_t> Vm::read(const RegionMapId<S>& map, MemoryRegion S::* key,
                                   std::uint32_t index) {
    const std::optional<MemoryRegion> where = map.declared(key);
    if (!where.has_value()) {
        throw std::invalid_argument("read: that field is not one of this batch's declared places");
    }
    return read(*where, index);
}

template <class S>
void Vm::write(const RegionMapId<S>& map, MemoryRegion S::* key,
               std::span<const std::uint8_t> bytes, std::uint32_t index) {
    const std::optional<MemoryRegion> where = map.declared(key);
    if (!where.has_value()) {
        throw std::invalid_argument("write: that field is not one of this batch's declared places");
    }
    write(*where, bytes, index);
}

// Decomposes a function-type Sig into the per-argument widths and the return width the non-template
// core needs. Only Ret(Args...) is valid; the primary is left undefined.
template <typename Sig>
struct RoutineSignature;

template <typename Ret, typename... Args>
struct RoutineSignature<Ret(Args...)> {
    static std::array<int, sizeof...(Args)> inputWidths() {
        return {static_cast<int>(sizeof(Args))...};
    }
    static constexpr int outputWidth() {
        if constexpr (std::is_void_v<Ret>) {
            return 0;
        } else {
            return static_cast<int>(sizeof(Ret));
        }
    }
};

template <typename Sig>
Routine<Sig> Vm::uploadRoutine(std::span<const std::uint8_t> routineBytes,
                               const RoutineBinding& binding, int instances) {
    const auto widths = RoutineSignature<Sig>::inputWidths();
    const std::size_t handle = registerResolved(
        routineBytes, binding, std::span<const int>(widths),
        RoutineSignature<Sig>::outputWidth(), instances);
    return Routine<Sig>{this, handle};
}

template <typename Sig>
Routine<Sig> Vm::registerRoutine(LiteralPath asmFilePath, const RoutineBinding& binding,
                                 std::optional<AssetPolicy> policy, int instances) {
    const auto widths = RoutineSignature<Sig>::inputWidths();
    const std::size_t handle = registerRoutineResolvingPolicy(
        asmFilePath.view(), binding, policy, std::span<const int>(widths),
        RoutineSignature<Sig>::outputWidth(), instances);
    return Routine<Sig>{this, handle};
}

template <typename Ret, typename... Args>
Ret Routine<Ret(Args...)>::operator()(Args... args) const {
    const std::array<CallValue, sizeof...(Args)> inputs{
        CallValue{static_cast<std::uint64_t>(args), static_cast<int>(sizeof(Args))}...};
    const std::uint64_t result = vm_->invoke(handle_, std::span<const CallValue>(inputs));
    if constexpr (!std::is_void_v<Ret>) {
        return static_cast<Ret>(result);
    }
}

}  // namespace retropp
