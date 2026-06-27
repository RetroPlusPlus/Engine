// The generic VM host. System-agnostic: it owns one VmBackend chosen by VMPlatform and
// drives it through the abstract seam. No SM83 / Game Boy / SameBoy idiom appears here — that lives
// in the concrete backend (src/vm/sameboy_backend.cpp). Adding a system is adding a backend + a
// factory case; this file does not change.
#include "retropp/vm.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "retropp/asset_policy.h"      // resolveAssetPolicy
#include "retropp/asset_registry.h"    // assetRoot — the single project-relative resource root (no routine root)
#include "retropp/routine_registry.h"  // detail::findEmbeddedRoutine
#include "src/vm/gameboy/sameboy_backend.h"
#include "src/vm/vm_backend.h"

namespace retropp {

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
    Throttle                throttle;  // HostSpeed = called for a value; HardwareSpeed = driven (audio)
};

struct Vm::Impl {
    VMPlatform                   platform;
    TimingProfile                timing;  // held for the hardware-speed path; unused here
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

void Vm::enableAudio(unsigned sampleRate,
                     std::function<void(std::int16_t, std::int16_t)> onSample) {
    impl_->backend->enableAudio(sampleRate, std::move(onSample));
}

void Vm::startDriver(const Routine<void()>& driver) {
    if (driver.vm_ != this) {
        throw std::invalid_argument("startDriver: the routine was not registered on this Vm");
    }
    const ResolvedRoutine& routine = impl_->routines[driver.handle_];
    if (routine.throttle != Throttle::HardwareSpeed) {
        throw std::invalid_argument(
            "startDriver: only a Throttle::HardwareSpeed routine can be driven as an audio driver");
    }
    impl_->backend->beginContinuous(routine.entry);
}

std::uint64_t Vm::stepDriver(std::uint64_t cpuCycles) {
    return impl_->backend->runForCycles(cpuCycles);
}

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
    // `instances > 1` is a declared seam (multi-instance routing for anti-channel-stealing audio) —
    // not built yet, so it throws. A HardwareSpeed routine is NOT a seam: it registers like any other
    // and is driven via startDriver / stepDriver instead of being called for a value.
    if (instances != 1) {
        throw std::logic_error(
            "multi-instance routing (anti-channel-stealing audio) is not built yet");
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
    resolved.throttle = binding.throttle;
    impl_->routines.push_back(std::move(resolved));
    return impl_->routines.size() - 1;
}

std::vector<std::uint8_t> Vm::assemble(std::string_view source) {
    // The VM's platform (fixed at construction) selects the backend, which selects the ISA's assembler —
    // so "which ISA" is never ambiguous. A pure source → bytes transform; placement is a separate step.
    return impl_->backend->assemble(std::string(source)).bytes;
}

std::size_t Vm::registerRoutineResolvingPolicy(std::string_view logicalPath,
                                               const RoutineBinding& binding,
                                               std::optional<AssetPolicy> policy,
                                               std::span<const int> inputWidths, int outputWidth,
                                               int instances) {
    // Embed (default): the build scan baked the assembled bytes into the routine registry, keyed by the
    // logical path; place them directly. If none were baked (the target was not run through the scan)
    // fall through to the on-disk read so the literal path still resolves during development.
    if (resolveAssetPolicy(policy, AssetPolicy::Embed) == AssetPolicy::Embed) {
        if (const std::span<const std::uint8_t> baked = detail::findEmbeddedRoutine(logicalPath);
            !baked.empty()) {
            return registerResolved(baked, binding, inputWidths, outputWidth, instances);
        }
    }
    // LoadFromPath (or an un-baked Embed): resolve the full project-relative logical path against the
    // engine's single assetRoot(), read it, assemble it in this VM's ISA, and register the resulting bytes
    // exactly as the byte form does — registerResolved copies them into the code arena, so the temporary
    // buffer's lifetime is fine.
    const std::filesystem::path full = assetRoot() / std::filesystem::path(logicalPath);
    std::ifstream in{full, std::ios::binary};
    if (!in) {
        throw std::runtime_error("VM: cannot open routine .asm file: " + full.string());
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    const vm::AssembledRoutine assembled = impl_->backend->assemble(ss.str());
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

}  // namespace retropp
