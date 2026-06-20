#pragma once

// The VM host public API (ENG-3.B) — the engine's runtime virtual-machine surface for the narrow
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
// NO ROM. This is a port, not an emulator. No game ROM is loaded or executed anywhere. The only
// original code that runs in the VM is the narrow correctness-impossibility set, supplied as
// surgically-extracted `const` byte arrays embedded at build time and injected into the VM's code
// space. There is no Vm::loadRom and no ROM-relative addressing.
//
// The header pulls NO backend type (no SameBoy GB_*, no SM83 register enum): the template callable
// converts typed arguments to width-tagged values and delegates the machine work to non-template Vm
// members defined in vm.cpp, which dispatch through the abstract backend seam.

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

#include "retropp/asset_policy.h"   // AssetPolicy (the routine sugar door's Embed / LoadFromPath choice)
#include "retropp/isa.h"            // Isa + the VMPlatform → Isa mapping below
#include "retropp/literal_path.h"   // LiteralPath (the routine sugar door takes a compile-time literal path)
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

// Where one input or output value lives in the target machine: a CPU register, or an absolute memory
// address. PLATFORM-NEUTRAL — a register is an opaque id whose meaning the selected backend defines;
// a platform header (retropp/gb.h) supplies the typed constants that name them (gb::A, gb::HL, …). The
// address is 32-bit so systems with address spaces wider than 16-bit (e.g. the SNES's 24-bit bus)
// fit without a surface change.
class Location {
public:
    enum class Kind : std::uint8_t { Register, Memory };

    // A register location, identified by a backend-defined id. Consumers use a platform header's
    // typed constants (gb::A, …) rather than calling this directly.
    static constexpr Location reg(std::uint16_t registerId) noexcept {
        Location loc;
        loc.kind_ = Kind::Register;
        loc.id_ = registerId;
        return loc;
    }

    // An absolute memory address in the target machine's address space.
    static constexpr Location memory(std::uint32_t address) noexcept {
        Location loc;
        loc.kind_ = Kind::Memory;
        loc.id_ = address;
        return loc;
    }

    [[nodiscard]] constexpr Kind kind() const noexcept { return kind_; }
    [[nodiscard]] constexpr std::uint16_t registerId() const noexcept {
        return static_cast<std::uint16_t>(id_);
    }
    [[nodiscard]] constexpr std::uint32_t address() const noexcept { return id_; }

private:
    constexpr Location() = default;
    Kind kind_ = Kind::Register;
    std::uint32_t id_ = 0;  // register id (Kind::Register) or memory address (Kind::Memory)
};

// How a routine is paced. HostSpeed runs as fast as the host allows and is byte-identical — the RNG
// path, fully realized here. HardwareSpeed throttles to the CPU clock for a real-time consumer (the
// audio driver); it is a declared seam realized at ENG-4 (registering one throws today).
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

// The VM host. Owns one backend machine (selected by VMPlatform); routines registered on it share
// its memory (so RNG seed state persists across calls). Non-copyable (owns a machine); movable.
class Vm {
public:
    explicit Vm(VMPlatform platform, TimingProfile timing = TimingProfile::GameBoyColor);
    ~Vm();

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

    // ── Audio chain (ENG-4.A — the hardware-speed driver path) ──────────────────────────────────────
    // The narrow set of original routines that produce sound run as continuously-executing DRIVERS at
    // the hardware CPU clock (Throttle::HardwareSpeed), their APU register writes synthesizing PCM at
    // the original cadence — distinct from a HostSpeed routine that is CALLED for a return value (RNG).
    // These three members are that path: enable the APU + sink once, position the driver, then step it
    // one cycle budget per sim tick. (The cue surface a game drives by meaning is ENG-4.B; this is the
    // raw chain it sits on.)

    // Enable the backend's APU and route each produced stereo PCM frame to `onSample` (called per
    // sample on the thread that steps the driver — for the audio chain, the AudioSystem's production
    // thread, ENG-4.D.1). The APU's sample rate is set to
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

    // Register a surgically-extracted routine from its EMBEDDED BYTES (a build-time `const` array)
    // + its I/O binding, returning a typed callable. The engine injects the bytes into the VM's code
    // space; there is no ROM. `instances` is a declared seam — only 1 is realized in v1 (multi-
    // instance routing is ENG-4). The signature determines I/O widths; the binding determines I/O
    // locations and pacing. Throws on: the ENG-4 seams (HardwareSpeed / instances > 1), an
    // inputs/arity mismatch, a width/location mismatch, an unknown register for the backend, or an
    // exhausted code arena.
    template <typename Sig>
    Routine<Sig> uploadRoutine(std::span<const std::uint8_t> routineBytes,
                               const RoutineBinding& binding, int instances = 1);

    // Register a routine from a `.asm` FILE — the SUGAR door, the mirror of loadAtlas: hand over a
    // compile-time LITERAL logical path (never bytes, never a runtime string), and the engine resolves
    // it by the embed/load `policy`. The literal is what a build-time scan can find to bake an Embed
    // routine; a genuinely runtime path is not a door — read its bytes yourself and use uploadRoutine.
    //   * Embed (default)    — use the bytes the build baked into the binary for this logical path (the
    //                          routine registry, ENG-4.B Step 4). If none were baked (no scan ran), fall
    //                          through to the on-disk read so the path still works during development.
    //   * LoadFromPath       — read `routineRoot() / path` at registration and assemble it in-process
    //                          with this VM's platform assembler (the Game Boy family → SM83, the
    //                          engine's own — NO external toolchain), for a copyright-derived routine.
    // `policy` precedence: per-call > EngineConfig::defaultRoutinePolicy > the per-type default (Embed).
    // `binding`/`instances`/the signature mean exactly what they do for uploadRoutine; entry is offset 0
    // (a leaf routine). Throws if the file cannot be opened, on a source error (with line context), or
    // on any of the byte form's validation failures.
    template <typename Sig>
    Routine<Sig> registerRoutine(LiteralPath asmFilePath, const RoutineBinding& binding,
                                 std::optional<AssetPolicy> policy = {}, int instances = 1);

    // Assemble assembly SOURCE into machine-code bytes for THIS VM's ISA (the Game Boy family → SM83) —
    // the VM's platform alone decides the ISA, so the right assembler is always selected. A source →
    // bytes transform, NOT a path or registration door: it is how a consumer holding routine/audio
    // source obtains bytes to hand to uploadRoutine (the "runtime need ⇒ hand raw bytes" path for
    // source). The audio system uses it to materialize a LoadFromPath chiptune. Throws on a source error.
    [[nodiscard]] std::vector<std::uint8_t> assemble(std::string_view source);

private:
    template <typename Sig>
    friend class Routine;

    // Non-template core (defined in vm.cpp). registerResolved validates + places the bytes through
    // the backend + stores the resolved binding, returning its handle; invoke sets up the call
    // frame, marshals inputs, runs to return, and reads the output — all via the backend seam.
    std::size_t registerResolved(std::span<const std::uint8_t> routineBytes,
                                 const RoutineBinding& binding,
                                 std::span<const int> inputWidths,
                                 int outputWidth, int instances);
    // The sugar door's non-template core: resolve the embed/load policy for `logicalPath`, then either
    // place the build-baked bytes (Embed) or read `routineRoot() / logicalPath` + assemble it (LoadFromPath
    // or an un-baked Embed), placing + resolving as registerResolved does.
    std::size_t registerRoutineResolvingPolicy(std::string_view logicalPath, const RoutineBinding& binding,
                                               std::optional<AssetPolicy> policy,
                                               std::span<const int> inputWidths, int outputWidth,
                                               int instances);
    std::uint64_t invoke(std::size_t handle, std::span<const CallValue> inputs);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ── Template definitions ──────────────────────────────────────────────────────────────────────

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
