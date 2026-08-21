#pragma once

// Driver-hosting declaration vocabulary — the audio-agnostic data a game declares to host its own
// resident sound driver as a long-lived, addressable machine.
//
// This header is PLATFORM- and AUDIO-agnostic: it names WHAT to place (code/data images at absolute,
// optionally bank-qualified bases), HOW the cartridge is mapped (an opaque Mapper the backend decodes),
// WHERE the per-frame tick lives, WHICH state slots the game reads/writes, and the machine gestures
// (Instruction) that realize a player verb. The imperative work — placing the images, sizing the
// cartridge, running the tick, applying the gestures at the frame boundary — is the engine's. The
// per-system vocabulary a binding names (registers via retropp/gb.h, bank encodings via gb::banked,
// mappers via gb::Mbc3) lives in the platform header; this header never mentions a console.
//
// It sits BELOW the VM host: retropp/vm.h consumes these types (Vm::hostDriver / Vm::tickDriver), and
// the audio surfaces compose the same Vm operations. Nothing here pulls a VM or backend type.

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "retropp/isa.h"       // Isa — the binding records the ISA its images are written for
#include "retropp/location.h"  // Location — a slot address / an instruction's register or mailbox

namespace retropp {

// A cartridge memory-bank controller, declared once at registration. An OPAQUE hardware id (the
// Location opaque-id pattern) the selected backend decodes; the platform header supplies the constants
// (gb::Mbc3, …). The default is none — a flat ROM-only image with no banking (a driver that fits the
// low 32 KiB, e.g. a small flat driver). A driver whose placement exceeds 32 KiB requires a
// mapper (banked placement); hosting one with the none mapper throws.
class Mapper {
public:
    constexpr Mapper() = default;  // none — flat ROM-only image

    // A mapper from a backend-defined id. Consumers use the platform header's constants (gb::Mbc3)
    // rather than calling this directly. Id 0 is reserved for "none".
    static constexpr Mapper fromId(std::uint16_t id) noexcept {
        Mapper m;
        m.id_ = id;
        return m;
    }

    [[nodiscard]] constexpr std::uint16_t id() const noexcept { return id_; }
    [[nodiscard]] constexpr bool isNone() const noexcept { return id_ == 0; }

private:
    std::uint16_t id_ = 0;  // backend-defined (for the Game Boy family, the cartridge-type header byte)
};

// A placed image: the extracted bytes and the absolute base address they occupy. The base may be
// bank-qualified — gb::banked(bank, addr16) folds the bank into the high bits of the 32-bit value; the
// backend decodes it to a physical offset in the assembled cartridge. A plain base in the low 32 KiB is
// flat (bank 0 for 0x0000–0x3FFF; the first switchable bank for 0x4000–0x7FFF). The bytes need only
// outlive the hostDriver call — the engine copies them into the machine image.
struct DriverImage {
    std::span<const std::uint8_t> bytes;
    std::uint32_t                 base = 0;
};

// Which way a declared state slot moves. Read: the game only reads it back (from the published
// snapshot). Write: the game only writes it (a mailbox / control field). ReadWrite: both. The direction
// keeps the published read value small and makes a write to a read-only field a loud error at the layer
// that types the slot (the audio handle); at the machine layer it is carried, not enforced.
enum class SlotDirection : std::uint8_t { Read, Write, ReadWrite };

// A declared state slot at the machine layer: an absolute address, its byte width, and its direction.
// The typed game-struct binding (a plain value type's pointer-to-member fields) is a higher layer; the
// machine speaks addresses. Slot order is the binding's declaration order — the index a read names.
struct SlotSpec {
    std::uint32_t address   = 0;
    int           width     = 1;  // 1 or 2 (a console flag/word)
    SlotDirection direction = SlotDirection::ReadWrite;
};

// A fixed register preset an argument-family call always applies (independent of the performed value):
// a register set to a constant before the entry is called. (e.g. a driver whose play entry expects a
// mode selector in a second register.)
struct RegisterPreset {
    Location      reg;    // must be a register Location
    std::uint64_t value = 0;
};

// A declared machine gesture — one type, two families — reified as data at registration and PERFORMED
// by the engine at the tick boundary. It is how a standard player verb (play / stop / a slot batch)
// realizes on a specific driver without any machine idiom reaching the call site.
//
//   * Instruction::write — the RAM-flag family (the mailbox lineage): a value lands in a
//     memory mailbox the driver polls. play(id) writes the id; stop() writes its declared fixed value.
//   * Instruction::call  — the argument family (the tracker-driver lineage): a value rides a CPU
//     register into an entry the engine calls (run to return). play(id) rides the id; fixed register
//     presets always apply.
//
// A fixed value, when set, is what the gesture carries when PERFORMED — overriding the performer's
// argument (this is stop()'s constant, or an init that takes no argument). When unset, the performer
// supplies the value (play(id)).
class Instruction {
public:
    enum class Kind : std::uint8_t { Write, Call };

    // RAM-flag family: write a value of `width` bytes to `location` (a memory mailbox). `fixedValue`,
    // when set, is always written; otherwise the performer's value is.
    static Instruction write(Location location, int width = 1,
                             std::optional<std::uint64_t> fixedValue = {}) {
        Instruction ins;
        ins.kind_ = Kind::Write;
        ins.location_ = location;
        ins.width_ = width;
        ins.fixedValue_ = fixedValue;
        return ins;
    }

    // Argument family: load a value into `argRegister`, apply any fixed register `presets`, then call
    // `entry` (run to return). `fixedValue`, when set, is loaded instead of the performer's value.
    static Instruction call(std::uint32_t entry, Location argRegister,
                            std::optional<std::uint64_t> fixedValue = {},
                            std::vector<RegisterPreset> presets = {}) {
        Instruction ins;
        ins.kind_ = Kind::Call;
        ins.entry_ = entry;
        ins.location_ = argRegister;
        ins.fixedValue_ = fixedValue;
        ins.presets_ = std::move(presets);
        return ins;
    }

    [[nodiscard]] Kind kind() const noexcept { return kind_; }
    [[nodiscard]] Location location() const noexcept { return location_; }  // Write: mailbox; Call: arg reg
    [[nodiscard]] std::uint32_t entry() const noexcept { return entry_; }   // Call only
    [[nodiscard]] int width() const noexcept { return width_; }             // Write only
    [[nodiscard]] const std::vector<RegisterPreset>& presets() const noexcept { return presets_; }
    [[nodiscard]] const std::optional<std::uint64_t>& fixedValue() const noexcept { return fixedValue_; }

    // The value this gesture carries when performed with (or without) an argument: the fixed value if
    // one is declared, else the supplied performer value.
    [[nodiscard]] std::uint64_t valueFor(std::uint64_t performerValue) const noexcept {
        return fixedValue_ ? *fixedValue_ : performerValue;
    }

private:
    Kind                         kind_ = Kind::Write;
    Location                     location_ = Location::memory(0);
    std::uint32_t                entry_ = 0;
    int                          width_ = 1;
    std::optional<std::uint64_t> fixedValue_{};
    std::vector<RegisterPreset>  presets_{};
};

// The machine-layer binding for a hosted driver: everything the engine needs to configure the resident
// machine and drive it. Untyped, portable data (no VM or audio type) — the AudioLibrary stores exactly
// this, and Vm::hostDriver consumes it. The typed game-struct slot vocabulary and the per-lane verb
// realizations are layered on top by the audio surface; this is the common substrate.
struct DriverBinding {
    std::vector<DriverImage>   images;        // placed at their (possibly bank-qualified) bases
    Mapper                     mapper{};       // default none (flat); banked placement requires one
    std::uint32_t              tickEntry = 0;  // the per-frame entry, run to return each tick
    std::optional<std::uint32_t> stackTop{};   // scratch stack top; default the platform scratch top
    std::vector<SlotSpec>      slots;          // declared state slots (index = declaration order)
    std::optional<Instruction> init{};         // performed ONCE at host() (Gap 3 — engine-run .init)
    Isa                        isa = Isa::Sm83; // the ISA the images are written for (verified at host)
};

}  // namespace retropp
