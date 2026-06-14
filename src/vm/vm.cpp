// ENG-3.B — the generic VM host. System-agnostic: it owns one VmBackend chosen by VMPlatform and
// drives it through the abstract seam. No SM83 / Game Boy / SameBoy idiom appears here — that lives
// in the concrete backend (src/vm/sameboy_backend.cpp). Adding a system is adding a backend + a
// factory case; this file does not change.
#include "gbcpp/vm.h"

#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "src/vm/gameboy/sameboy_backend.h"
#include "src/vm/vm_backend.h"

namespace gbcpp {

namespace {

// Construct the backend for a platform. GameBoy / GameBoyColor → the SM83 / SameBoy backend (the
// only one built in v1). Other systems are drop-in: add a backend and a case here; the rest of the
// host is unchanged. An unbuilt system throws — the seam exists, the implementation lands when a
// consumer exercises it.
std::unique_ptr<vm::VmBackend> makeBackend(VMPlatform platform) {
    switch (platform) {
        case VMPlatform::GameBoy:
            return std::make_unique<vm::SameBoyBackend>(vm::ConsoleModel::GameBoy);
        case VMPlatform::GameBoyColor:
            return std::make_unique<vm::SameBoyBackend>(vm::ConsoleModel::GameBoyColor);
        case VMPlatform::Snes:
        case VMPlatform::Nes:
        case VMPlatform::Genesis:
        case VMPlatform::MasterSystem:
            break;
    }
    throw std::runtime_error("VMPlatform (" + std::to_string(static_cast<int>(platform)) +
                             "): no backend built in v1 (only GameBoy / GameBoyColor)");
}

}  // namespace

// A registration resolved at registerRoutine time: the entry address its bytes were placed at, and
// the per-input / output locations + widths the call path marshals against.
struct ResolvedRoutine {
    std::uint32_t           entry;
    std::vector<Location>   inputs;
    std::vector<int>        inputWidths;
    std::optional<Location> output;
    int                     outputWidth;
};

struct Vm::Impl {
    VMPlatform                   platform;
    TimingProfile                timing;  // held for the ENG-4 hardware-speed path; unused here
    std::unique_ptr<vm::VmBackend> backend;
    std::vector<ResolvedRoutine> routines;

    Impl(VMPlatform p, TimingProfile t) : platform(p), timing(t), backend(makeBackend(p)) {}
};

Vm::Vm(VMPlatform platform, TimingProfile timing)
    : impl_(std::make_unique<Impl>(platform, timing)) {}

Vm::~Vm() = default;
Vm::Vm(Vm&&) noexcept = default;
Vm& Vm::operator=(Vm&&) noexcept = default;

VMPlatform Vm::platform() const noexcept { return impl_->platform; }

void Vm::reset() { impl_->backend->reset(); }

void Vm::advanceClock(std::uint64_t cycles) { impl_->backend->advanceClock(cycles); }

namespace {

// Validate a binding location against the width the signature gives that slot. Registers must match
// the backend's register width; an unknown register id is rejected. Memory accepts any width.
void validateLocation(const vm::VmBackend& backend, const Location& loc, int valueWidth,
                      const char* role, std::size_t index) {
    if (loc.kind() == Location::Kind::Register) {
        const int regWidth = backend.registerWidthBytes(loc.registerId());
        if (regWidth == 0) {
            throw std::invalid_argument(std::string(role) + " " + std::to_string(index) +
                                        ": register id " + std::to_string(loc.registerId()) +
                                        " is not a register on this system");
        }
        if (regWidth != valueWidth) {
            throw std::invalid_argument(
                std::string(role) + " " + std::to_string(index) + ": a " +
                std::to_string(valueWidth * 8) + "-bit value cannot bind to a " +
                std::to_string(regWidth * 8) + "-bit register");
        }
    } else if (!backend.addressIsAccessible(loc.address())) {
        throw std::invalid_argument(std::string(role) + " " + std::to_string(index) +
                                    ": address " + std::to_string(loc.address()) +
                                    " is not directly accessible on this system");
    }
}

}  // namespace

std::size_t Vm::registerResolved(std::span<const std::uint8_t> routineBytes,
                                 const RoutineBinding& binding,
                                 std::span<const int> inputWidths,
                                 int outputWidth, int instances) {
    // Declared seams realized at ENG-4.
    if (instances != 1) {
        throw std::logic_error(
            "multi-instance routing is realized at ENG-4 (anti-channel-stealing audio)");
    }
    if (binding.throttle == Throttle::HardwareSpeed) {
        throw std::logic_error("Throttle::HardwareSpeed is realized at ENG-4 (audio driver)");
    }

    // Arity + width/location validation.
    if (binding.inputs.size() != inputWidths.size()) {
        throw std::invalid_argument(
            "RoutineBinding.inputs has " + std::to_string(binding.inputs.size()) +
            " entries but the routine signature has " + std::to_string(inputWidths.size()) +
            " argument(s)");
    }
    for (std::size_t i = 0; i < binding.inputs.size(); ++i) {
        validateLocation(*impl_->backend, binding.inputs[i], inputWidths[i], "argument", i);
    }
    if (outputWidth == 0 && binding.output.has_value()) {
        throw std::invalid_argument("a void routine signature cannot bind an output location");
    }
    if (outputWidth != 0 && !binding.output.has_value()) {
        throw std::invalid_argument("a value-returning routine signature requires binding.output");
    }
    if (binding.output.has_value()) {
        validateLocation(*impl_->backend, *binding.output, outputWidth, "return value", 0);
    }

    if (routineBytes.empty()) {
        throw std::invalid_argument("routine has no bytes");
    }
    if (binding.entryOffset >= routineBytes.size()) {
        throw std::invalid_argument("entryOffset is past the end of the routine bytes");
    }

    // Inject the bytes into the backend's code space; entry is the placement base + the binding's
    // offset within those bytes.
    const std::uint32_t base = impl_->backend->placeRoutine(routineBytes);

    ResolvedRoutine resolved;
    resolved.entry = base + binding.entryOffset;
    resolved.inputs.assign(binding.inputs.begin(), binding.inputs.end());
    resolved.inputWidths.assign(inputWidths.begin(), inputWidths.end());
    resolved.output = binding.output;
    resolved.outputWidth = outputWidth;
    impl_->routines.push_back(std::move(resolved));
    return impl_->routines.size() - 1;
}

std::size_t Vm::registerRoutineFromFile(std::string_view asmFilePath, const RoutineBinding& binding,
                                        std::span<const int> inputWidths, int outputWidth,
                                        int instances) {
    // Read the routine's .asm file, then assemble it through the backend (the Game Boy backend → SM83,
    // in-process, no external toolchain) and register the resulting bytes exactly as the byte form
    // does — placeRoutine copies them into the code arena, so the temporary buffer's lifetime is fine.
    std::ifstream in{std::string(asmFilePath), std::ios::binary};
    if (!in) {
        throw std::runtime_error("VM: cannot open routine .asm file: " + std::string(asmFilePath));
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string source = ss.str();
    const vm::AssembledRoutine assembled = impl_->backend->assemble(source);
    return registerResolved(std::span<const std::uint8_t>(assembled.bytes), binding, inputWidths,
                            outputWidth, instances);
}

std::uint64_t Vm::invoke(std::size_t handle, std::span<const CallValue> inputs) {
    const ResolvedRoutine& routine = impl_->routines[handle];
    vm::VmBackend& backend = *impl_->backend;

    backend.beginCall(routine.entry);
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        const Location& loc = routine.inputs[i];
        if (loc.kind() == Location::Kind::Register) {
            backend.writeRegister(loc.registerId(), inputs[i].value, inputs[i].width);
        } else {
            backend.writeMemory(loc.address(), inputs[i].value, inputs[i].width);
        }
    }
    backend.run();

    if (!routine.output.has_value()) {
        return 0;
    }
    const Location& out = *routine.output;
    if (out.kind() == Location::Kind::Register) {
        return backend.readRegister(out.registerId());
    }
    return backend.readMemory(out.address(), routine.outputWidth);
}

}  // namespace gbcpp
