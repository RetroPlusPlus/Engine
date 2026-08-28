// The generic VM host. System-agnostic: it owns one VmBackend chosen by VMPlatform and
// drives it through the abstract seam. No SM83 / Game Boy / SameBoy idiom appears here — that lives
// in the concrete backend (src/vm/sameboy_backend.cpp). Adding a system is adding a backend + a
// factory case; this file does not change.
#include "retropp/vm.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "retropp/asset_policy.h"      // resolveAssetPolicy
#include "retropp/asset_registry.h"    // assetRoot — the single project-relative resource root (no routine root)
#include "retropp/routine_registry.h"  // detail::findEmbeddedRoutine
#include "src/vm/gameboy/sameboy_backend.h"
#include "src/vm/run_governor.h"       // RunGovernor — what a running cartridge owes the wall clock
#include "src/vm/vm_backend.h"
#include "src/vm/vm_runner.h"          // VmRunner — the thread a running cartridge steps on
#include "src/vm/vm_testing.h"         // VmTestAccess — the deterministic seam, defined at file end

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

// The resolved state of the resident driver hosted on a VM (set by hostDriver): where the per-frame
// tick lives and the declared slots readSlot indexes. The images live in the backend's cartridge image.
struct HostedDriverState {
    bool                  hosted = false;
    std::uint32_t         tickEntry = 0;
    std::vector<SlotSpec> slots;
};

// One declared place's berth in the run publish: where it lives in the machine and where its bytes
// land in the published block.
struct PublishedRegion {
    MemoryRegion where;
    std::size_t  offset;
};

// A write waiting to cross to the running machine's own thread. Validated against the declared
// place at the call, applied in issue order at the next step boundary.
struct PendingRegionWrite {
    MemoryRegion              where;
    std::uint32_t             index;
    std::vector<std::uint8_t> bytes;
};

// Everything Vm::run() stands up: the governor owed cycles accrue against, the seqlock publish of
// the declared places, the write channel, and the runner whose thread steps the machine. The runner
// exists exactly while the machine runs — stop() takes the thread down with it — so a machine with
// no runner is quiescent and every direct-access path is safe.
struct RomRunState {
    bool booted = false;  // the image booted once; run() after stop() resumes rather than re-boots

    std::optional<vm::RunGovernor> governor;  // survives across run/stop episodes (factor + carry)

    // The seqlock publish (the DriverSnapshot idiom): even = stable, odd = a publish is in flight.
    // `published` is laid out by `table` and sized once per run(), written in place by the stepping
    // thread after each step, read wait-free by the game thread.
    std::atomic<std::uint32_t>   seq{0};
    std::vector<std::uint8_t>    published;
    std::vector<PublishedRegion> table;

    std::mutex                      writeMx;
    std::vector<PendingRegionWrite> pendingWrites;

    std::unique_ptr<vm::VmRunner> runner;
};

// A replacement's argument count is bounded so the fire path allocates nothing: sixteen is far past
// any register file's worth of distinct argument homes, and registration refuses a longer binding.
constexpr std::size_t kMaxReplacementInputs = 16;

// One escape as the host layer holds it: the game's key, what runs (a handler, or a native routine
// answering by its binding), and the address the backend watches. The key owns its bytes, so a key
// built at runtime outlives the call that declared it.
struct DeclaredEscape {
    std::string   key;
    std::uint32_t at = 0;
    EscapeHandler handler;
    NativeRoutine replaces;
    bool          armed = true;
};

struct Vm::Impl {
    VMPlatform                   platform;
    TimingProfile                timing;  // held for the hardware-speed path; unused here
    std::unique_ptr<vm::VmBackend> backend;
    std::vector<ResolvedRoutine> routines;
    HostedDriverState            driver;

    // The Vm this Impl belongs to, kept current across a move so an escape handler is handed the
    // machine at its present address rather than where it was declared.
    Vm* owner = nullptr;

    // Declared escapes, in declaration order. The backend watches the armed ones' addresses and
    // reports each fire back here; the keys, the handlers and the arming all live at this layer.
    std::vector<DeclaredEscape> escapes;
    bool                        escapeSinkInstalled = false;
    // Registered region batches, in registration order; a RegionMapId holds an index into this.
    std::vector<std::vector<DeclaredRegion>> regionBatches;
    // Sub-cycle remainder carried between advanceTick calls, so a machine whose clock does not
    // divide the tick period stays exact over any number of ticks.
    std::uint64_t cycleCarryNs = 0;

    bool romHosted = false;  // hostRom has run: the machine holds a game's own cartridge

    // Declared after `backend` on purpose: members destroy in reverse order, so the runner (and its
    // thread) is gone before the machine it steps.
    RomRunState romRun;

    Impl(VMPlatform p, TimingProfile t) : platform(p), timing(t), backend(makeBackend(p)) {}

    // Whether the hosted cartridge is running — the gate every machine-mutating verb checks, since
    // a running machine belongs to its own thread.
    [[nodiscard]] bool running() const noexcept { return romRun.runner != nullptr; }

    void requireNotRunning(const char* verb) const {
        if (running()) {
            throw std::logic_error(std::string(verb) +
                                   ": the machine is running its hosted cartridge; stop() first");
        }
    }

    // ── Escapes ──────────────────────────────────────────────────────────────────────────────────

    [[nodiscard]] DeclaredEscape* findEscape(std::string_view key) noexcept {
        for (DeclaredEscape& e : escapes) {
            if (e.key == key) {
                return &e;
            }
        }
        return nullptr;
    }

    [[nodiscard]] DeclaredEscape& requireEscape(std::string_view key) {
        if (DeclaredEscape* e = findEscape(key)) {
            return *e;
        }
        throw std::out_of_range("escapes: this machine declares no escape named '" +
                                std::string(key) + "'");
    }

    // Stepping thread: an armed address is about to execute. Runs the handler to completion before
    // the instruction executes; the guest's clock does not advance for it.
    //
    // The answering kind marshals synchronously against the PARKED machine — this thread owns it and
    // nothing moves while the escape runs, so reading the register file and live memory here is
    // coherent by construction. The guest's own calling code loaded the bound inputs before its call;
    // the bound output is where its callers read the answer; the instruction that executes on return
    // is the backend's own return, sending control straight back to the caller.
    void dispatchEscape(std::uint32_t firedAt) {
        for (DeclaredEscape& e : escapes) {
            if (!(e.armed && e.at == firedAt)) {
                continue;
            }
            if (e.replaces) {
                const NativeRoutine& r = e.replaces;
                std::array<std::uint64_t, kMaxReplacementInputs> values{};
                for (std::size_t i = 0; i < r.inputs.size(); ++i) {
                    const Location& in = r.inputs[i];
                    values[i] = in.kind() == Location::Kind::Register
                                    ? backend->readRegister(in.registerId())
                                    : backend->readMemory(in.address(), r.inputWidths[i]);
                }
                const std::uint64_t result = r.fn(values.data());
                if (r.output) {
                    if (r.output->kind() == Location::Kind::Register) {
                        backend->writeLiveRegister(r.output->registerId(), result, r.outputWidth);
                    } else {
                        backend->writeMemory(r.output->address(), result, r.outputWidth);
                    }
                }
            } else if (e.handler) {
                e.handler(*owner, firedAt);
            }
            return;
        }
    }

    // Install the sink once, the first time this machine declares an escape.
    void ensureEscapeSink() {
        if (escapeSinkInstalled) {
            return;
        }
        backend->setEscapeSink([this](std::uint32_t firedAt) { dispatchEscape(firedAt); });
        escapeSinkInstalled = true;
    }

    // The governor, created on first need. Runs and speed factors both require a CPU model — with
    // no clock rate, the platform's speed is undefined.
    vm::RunGovernor& ensureGovernor() {
        if (!romRun.governor) {
            if (!timing.cpu) {
                throw std::logic_error(
                    "run/speed: this VM's timing profile carries no CPU model, so the platform's "
                    "speed is undefined");
            }
            romRun.governor.emplace(timing.cpu->cpuClockHz);
        }
        return *romRun.governor;
    }

    // Lay the declared places out into one published block. Rebuilt at each run(), so places
    // registered between episodes join the observable set.
    void buildPublishTable() {
        romRun.table.clear();
        std::size_t offset = 0;
        for (const std::vector<DeclaredRegion>& batch : regionBatches) {
            for (const DeclaredRegion& d : batch) {
                romRun.table.push_back(PublishedRegion{.where = d.where, .offset = offset});
                offset += static_cast<std::size_t>(d.where.size) * d.where.count;
            }
        }
        romRun.published.assign(offset, 0);
        romRun.seq.store(0, std::memory_order_relaxed);
    }

    [[nodiscard]] const PublishedRegion* publishedEntry(const MemoryRegion& where) const {
        for (const PublishedRegion& p : romRun.table) {
            if (p.where.at == where.at && p.where.size == where.size &&
                p.where.count == where.count) {
                return &p;
            }
        }
        return nullptr;
    }

    // The publish's capture: every declared place's bytes into the published block. Only ever runs
    // between the seqlock's odd and even edges.
    void capturePublished() {
        for (const PublishedRegion& p : romRun.table) {
            for (std::uint32_t i = 0; i < p.where.count; ++i) {
                const std::size_t at = p.offset + static_cast<std::size_t>(i) * p.where.size;
                backend->readRegion(p.where, i,
                                    std::span<std::uint8_t>(romRun.published.data() + at,
                                                            p.where.size));
            }
        }
    }

    // Stepping thread, after each step: capture every declared place as one coherent set.
    void publishStep() {
        romRun.seq.fetch_add(1, std::memory_order_release);  // -> odd (writing)
        capturePublished();
        romRun.seq.fetch_add(1, std::memory_order_release);  // -> even (stable)
    }

    // Game thread, while running: the latest coherent capture of one declared place. Retries only
    // while a publish is mid-flight (bounded — a publish is one block of reads).
    [[nodiscard]] std::vector<std::uint8_t> readPublished(const MemoryRegion& where,
                                                          std::uint32_t index) const {
        const PublishedRegion* entry = publishedEntry(where);
        if (entry == nullptr) {
            throw std::logic_error(
                "read: the machine is running and this place is not among the declared regions — "
                "register it before run(), or stop() the machine to read a place built on the spot");
        }
        if (index >= where.count) {
            throw std::out_of_range("read: index " + std::to_string(index) +
                                    " is out of range (the place declares " +
                                    std::to_string(where.count) + " entries)");
        }
        const std::size_t at = entry->offset + static_cast<std::size_t>(index) * where.size;
        std::vector<std::uint8_t> out(where.size);
        for (;;) {
            const std::uint32_t before = romRun.seq.load(std::memory_order_acquire);
            if (before & 1u) {
                continue;  // a publish is in flight — wait for it to finish
            }
            std::copy_n(romRun.published.data() + at, where.size, out.data());
            std::atomic_thread_fence(std::memory_order_acquire);
            if (romRun.seq.load(std::memory_order_acquire) == before) {
                return out;
            }
        }
    }

    // Game thread, while running: queue a write for the stepping thread. Validated here in full —
    // the apply on the stepping thread cannot throw.
    void queueRegionWrite(const MemoryRegion& where, std::span<const std::uint8_t> bytes,
                          std::uint32_t index) {
        if (publishedEntry(where) == nullptr) {
            throw std::logic_error(
                "write: the machine is running and this place is not among the declared regions — "
                "register it before run(), or stop() the machine to write a place built on the spot");
        }
        if (bytes.size() != where.size) {
            throw std::invalid_argument("write: " + std::to_string(bytes.size()) +
                                        " bytes is not one entry (the place's entries are " +
                                        std::to_string(where.size) + " bytes)");
        }
        if (index >= where.count) {
            throw std::out_of_range("write: index " + std::to_string(index) +
                                    " is out of range (the place declares " +
                                    std::to_string(where.count) + " entries)");
        }
        const std::lock_guard<std::mutex> lock(romRun.writeMx);
        romRun.pendingWrites.push_back(PendingRegionWrite{
            .where = where, .index = index,
            .bytes = std::vector<std::uint8_t>(bytes.begin(), bytes.end())});
    }

    // Stepping thread, before each step: land the writes queued so far, in issue order.
    void drainRegionWrites() {
        std::vector<PendingRegionWrite> writes;
        {
            const std::lock_guard<std::mutex> lock(romRun.writeMx);
            writes.swap(romRun.pendingWrites);
        }
        for (const PendingRegionWrite& w : writes) {
            backend->writeRegion(w.where, w.index, w.bytes);
        }
    }

    // Stand the run up. Boot (first episode) and the first publish happen on the calling thread —
    // the machine is quiescent until start(), and thread creation orders these writes before the
    // stepping thread's first look. The threaded mode is the public one; Inline is the
    // deterministic seam device-free tests step through (vm_testing.h).
    void startRun(Vm* self, vm::VmRunner::Mode mode) {
        if (!romHosted) {
            throw std::logic_error("run: no cartridge is hosted on this VM (hostRom first)");
        }
        if (running()) {
            throw std::logic_error("run: the machine is already running");
        }
        vm::RunGovernor& gov = ensureGovernor();
        if (timing.cpuCyclesPerTick() == 0) {
            throw std::logic_error("run: the CPU model's per-frame budget is zero");
        }
        buildPublishTable();
        if (!romRun.booted) {
            backend->bootHostedRom();
            romRun.booted = true;
        }
        publishStep();
        auto runner = std::make_unique<vm::VmRunner>(self, vm::VmRunner::StepKind::Started,
                                                     timing.cpuCyclesPerTick(), mode);
        runner->beforeEachStep([this] { drainRegionWrites(); });
        runner->afterEachStep([this] { publishStep(); });
        gov.restart(std::chrono::steady_clock::now());
        romRun.runner = std::move(runner);
        if (mode == vm::VmRunner::Mode::Threaded) {
            // The pace: step while cycles run lag cycles owed, park otherwise. Owed advances with
            // the wall clock at the platform's speed times the factor; both sides are read on the
            // stepping thread, so the closure is race-free by construction.
            vm::VmRunner*    raw = romRun.runner.get();
            vm::RunGovernor* g   = &gov;
            romRun.runner->start(
                [g, raw] {
                    const std::uint64_t owed =
                        g->owedThrough(std::chrono::steady_clock::now());
                    const std::uint64_t ran = raw->cyclesRun();
                    return static_cast<std::size_t>(ran > owed ? ran - owed : 0);
                },
                /*highWater=*/1);
        }
    }
};

namespace {
// A generous one-time runaway cap for the engine-run .init gesture at host() (no frame budget applies).
constexpr std::uint64_t kInitCycleCap = 1u << 24;  // ~4 s of SM83 time — an init returns in far less
}  // namespace

Vm::Vm(VMPlatform platform, TimingProfile timing)
    : impl_(std::make_unique<Impl>(platform, timing)) {
    impl_->owner = this;
}

Vm::~Vm() = default;

// The move operations re-point the Impl at its new owner: an escape handler is handed the machine, so
// the machine it is handed must be where the machine now lives.
Vm::Vm(Vm&& other) noexcept : impl_(std::move(other.impl_)) {
    if (impl_) {
        impl_->owner = this;
    }
}

Vm& Vm::operator=(Vm&& other) noexcept {
    impl_ = std::move(other.impl_);
    if (impl_) {
        impl_->owner = this;
    }
    return *this;
}

VMPlatform Vm::platform() const noexcept { return impl_->platform; }

void Vm::reset() {
    impl_->requireNotRunning("reset");
    impl_->backend->reset();
    impl_->romRun.booted = false;  // a reset machine boots fresh on the next run()
}

void Vm::advanceClock(std::uint64_t cycles) {
    impl_->requireNotRunning("advanceClock");
    impl_->backend->advanceClock(cycles);
}

void Vm::advanceTick(std::chrono::nanoseconds enginePeriod) {
    impl_->requireNotRunning("advanceTick");
    if (!impl_->timing.cpu.has_value()) {
        return;  // no CPU model: nothing to advance
    }
    // The carry rides on the VM, so consecutive ticks compose: the fraction of a cycle this tick
    // leaves behind is spent by a later one, and the running total never drifts from the machine's
    // true rate however the tick period relates to it.
    const CycleDraw draw = impl_->timing.cyclesForTick(enginePeriod, impl_->cycleCarryNs);
    impl_->cycleCarryNs = draw.carryNs;
    if (draw.cycles != 0) {
        impl_->backend->advanceClock(draw.cycles);
    }
}

void Vm::advanceTick() { advanceTick(impl_->timing.tickPeriod()); }

void Vm::enableAudio(unsigned sampleRate,
                     std::function<void(std::int16_t, std::int16_t)> onSample) {
    impl_->requireNotRunning("enableAudio");
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

// ── Resident driver ─────────────────────────────────────────────────────────────────────────────

std::uint64_t Vm::performInstruction(const Instruction& instruction, std::uint64_t cycleCap) {
    const std::uint64_t value = instruction.valueFor(0);
    if (instruction.kind() == Instruction::Kind::Write) {
        impl_->backend->writeMemory(instruction.location().address(), value, instruction.width());
        return 0;
    }
    // A call: apply the declared fixed register presets, then the folded argument in its register.
    std::vector<vm::ResidentRegister> presets;
    presets.reserve(instruction.presets().size() + 1);
    for (const RegisterPreset& p : instruction.presets()) {
        presets.push_back({p.reg.registerId(), p.value});
    }
    presets.push_back({instruction.location().registerId(), value});
    return impl_->backend->callResident(instruction.entry(),
                                        std::span<const vm::ResidentRegister>(presets), cycleCap);
}

void Vm::hostRom(std::span<const std::uint8_t> rom) {
    impl_->requireNotRunning("hostRom");
    impl_->backend->loadRom(rom);
    impl_->romHosted = true;
    impl_->romRun.booted = false;  // a fresh image boots fresh
}

void Vm::run() { impl_->startRun(this, vm::VmRunner::Mode::Threaded); }

void Vm::speed(std::uint32_t num, std::uint32_t den) {
    impl_->ensureGovernor().setFactor(num, den);
    if (impl_->running()) {
        impl_->romRun.runner->wake();  // a parked machine reacts to the new pace now, not next park
    }
}

std::pair<std::uint32_t, std::uint32_t> Vm::speed() const {
    if (!impl_->romRun.governor) {
        return {1u, 1u};  // the platform's own speed — nothing has been steered yet
    }
    return impl_->romRun.governor->factor();
}

void Vm::stop() {
    if (!impl_->running()) {
        return;
    }
    impl_->romRun.runner->requestStop();
    impl_->romRun.runner->wake();
    impl_->romRun.runner.reset();  // joins: the thread is gone by return, the machine parked
}

std::vector<std::uint8_t> Vm::read(const MemoryRegion& where, std::uint32_t index) {
    if (impl_->running()) {
        return impl_->readPublished(where, index);
    }
    std::vector<std::uint8_t> bytes(where.size);
    impl_->backend->readRegion(where, index, bytes);
    return bytes;
}

void Vm::write(const MemoryRegion& where, std::span<const std::uint8_t> bytes, std::uint32_t index) {
    if (impl_->running()) {
        impl_->queueRegionWrite(where, bytes, index);
        return;
    }
    impl_->backend->writeRegion(where, index, bytes);
}

std::size_t Vm::registerRegionsResolved(std::span<const DeclaredRegion> declared) {
    impl_->requireNotRunning("registerRegions");
    if (declared.empty()) {
        throw std::invalid_argument("registerRegions: the batch declares no places");
    }
    // Check every entry before reporting any. The whole point of handing the batch over is that a
    // two-hundred-entry table is answered once — a report that stops at the first bad entry turns
    // one registration into as many rounds as there are mistakes.
    std::string failures;
    std::size_t failed = 0;
    for (const DeclaredRegion& d : declared) {
        if (impl_->backend->regionIsAddressable(d.where)) {
            continue;
        }
        ++failed;
        failures += "\n  ";
        failures += d.name.empty() ? "(unnamed)" : std::string(d.name);
        failures += " at " + std::to_string(d.where.at) + ", " + std::to_string(d.where.size) +
                    " bytes x " + std::to_string(d.where.count);
    }
    if (failed != 0) {
        throw std::invalid_argument(
            "registerRegions: " + std::to_string(failed) + " of " +
            std::to_string(declared.size()) +
            " declared places are not reachable on this machine (host the cartridge before "
            "registering places inside it):" + failures);
    }
    impl_->regionBatches.emplace_back(declared.begin(), declared.end());
    return impl_->regionBatches.size() - 1;
}

// ── Escapes ─────────────────────────────────────────────────────────────────────────────────────

void Vm::registerEscapes(const EscapeMap& map) {
    impl_->requireNotRunning("registerEscapes");
    if (map.declarations.empty()) {
        throw std::invalid_argument("registerEscapes: the batch declares no escapes");
    }

    // Every entry is checked before any is reported, for the same reason a region batch is: a
    // generated table is answered once, not once per mistake.
    std::string failures;
    std::size_t failed = 0;
    const auto fail = [&](std::string_view key, const std::string& why) {
        ++failed;
        failures += "\n  ";
        failures += key.empty() ? "(unnamed)" : std::string(key);
        failures += ": " + why;
    };

    // One binding location checked against this machine: a register must exist here at the width the
    // signature carries; a memory home must be reachable for that many bytes. Reports through `fail`
    // and answers whether the location passed.
    const auto locationFits = [&](std::string_view key, const std::string& what, const Location& loc,
                                  int width) -> bool {
        if (loc.kind() == Location::Kind::Register) {
            const int regWidth = impl_->backend->registerWidthBytes(loc.registerId());
            if (regWidth == 0) {
                fail(key, what + " names a register this machine does not have");
                return false;
            }
            if (regWidth != width) {
                fail(key, what + " binds a " + std::to_string(width) + "-byte value to a " +
                              std::to_string(regWidth) + "-byte register");
                return false;
            }
            return true;
        }
        if (!impl_->backend->regionIsAddressable(MemoryRegion{
                .at = loc.address(), .size = static_cast<std::uint32_t>(width)})) {
            fail(key, what + " names memory this machine cannot reach");
            return false;
        }
        return true;
    };

    for (std::size_t i = 0; i < map.declarations.size(); ++i) {
        const GuestEscape&     e   = map.declarations[i];
        const std::string_view key = e.key;
        if (key.empty()) {
            fail(key, "the key is empty");
            continue;  // nothing else about this entry can be reported against a name
        }
        if (!e.handler && !e.replaces) {
            fail(key, "neither a handler nor a replacement — a declared escape with nothing to run "
                      "would never do anything");
        }
        if (e.handler && e.replaces) {
            fail(key, "both a handler and a replacement — one escape does one or the other");
        }
        if (e.replaces) {
            const NativeRoutine& r = e.replaces;
            if (r.inputs.size() != r.inputWidths.size()) {
                fail(key, "the binding declares " + std::to_string(r.inputs.size()) +
                              " input(s) for a function taking " +
                              std::to_string(r.inputWidths.size()));
            }
            if (r.inputs.size() > kMaxReplacementInputs) {
                fail(key, "a replacement takes at most " + std::to_string(kMaxReplacementInputs) +
                              " inputs");
            }
            if (r.output && r.outputWidth == 0) {
                fail(key, "the binding declares an output for a function returning nothing");
            }
            if (!r.output && r.outputWidth != 0) {
                fail(key, "the function returns a value the binding gives no home — its answer "
                          "would vanish");
            }
            if (r.declaredEntryOffset != 0 || r.declaredHardwarePacing) {
                fail(key, "a replacement binding names no pacing and no entry offset — the guest's "
                          "own call decides both");
            }
            for (std::size_t j = 0; j < r.inputs.size() && j < r.inputWidths.size(); ++j) {
                locationFits(key, "input " + std::to_string(j), r.inputs[j], r.inputWidths[j]);
            }
            if (r.output && r.outputWidth != 0) {
                locationFits(key, "the output", *r.output, r.outputWidth);
            }
        }
        if (!impl_->backend->addressIsAccessible(e.at)) {
            fail(key, "address " + std::to_string(e.at) + " is not reachable on this machine");
        }
        if (impl_->findEscape(key) != nullptr) {
            fail(key, "this machine already declares an escape by that name");
        }
        for (const DeclaredEscape& already : impl_->escapes) {
            if (already.at == e.at) {
                fail(key, "address " + std::to_string(e.at) + " already escapes, as '" +
                              already.key + "'");
                break;
            }
        }
        for (std::size_t j = 0; j < i; ++j) {
            const GuestEscape& earlier = map.declarations[j];
            if (std::string_view(earlier.key) == key) {
                fail(key, "the batch declares that name twice");
            }
            if (earlier.at == e.at) {
                fail(key, "the batch declares address " + std::to_string(e.at) + " twice");
            }
        }
    }
    if (failed != 0) {
        throw std::invalid_argument("registerEscapes: " + std::to_string(failed) + " of " +
                                    std::to_string(map.declarations.size()) +
                                    " declared escapes did not pass (host the cartridge before "
                                    "declaring escapes inside it):" + failures);
    }

    // Arm before recording, and undo what was armed if the backend refuses one — so a machine that
    // cannot watch addresses at all is left exactly as it was rather than half-declared.
    impl_->ensureEscapeSink();
    std::vector<std::uint32_t> armedHere;
    try {
        for (const GuestEscape& e : map.declarations) {
            if (e.armed) {
                impl_->backend->armEscape(e.at, static_cast<bool>(e.replaces));
                armedHere.push_back(e.at);
            }
        }
    } catch (...) {
        for (const std::uint32_t address : armedHere) {
            impl_->backend->disarmEscape(address);
        }
        throw;
    }

    for (const GuestEscape& e : map.declarations) {
        impl_->escapes.push_back(DeclaredEscape{.key      = std::string(std::string_view(e.key)),
                                                .at       = e.at,
                                                .handler  = e.handler,
                                                .replaces = e.replaces,
                                                .armed    = e.armed});
    }
}

EscapeTable Vm::escapes() noexcept { return EscapeTable(*this); }

bool Vm::escapeArmed(std::string_view key) const { return impl_->requireEscape(key).armed; }

void Vm::setEscapeArmed(std::string_view key, bool on) {
    DeclaredEscape& e = impl_->requireEscape(key);
    if (e.armed == on) {
        return;
    }
    e.armed = on;
    if (on) {
        impl_->backend->armEscape(e.at, static_cast<bool>(e.replaces));
    } else {
        impl_->backend->disarmEscape(e.at);
    }
}

void Vm::removeEscape(std::string_view key) {
    DeclaredEscape& e = impl_->requireEscape(key);
    if (e.armed) {
        impl_->backend->disarmEscape(e.at);
    }
    impl_->escapes.erase(impl_->escapes.begin() +
                         static_cast<std::ptrdiff_t>(&e - impl_->escapes.data()));
}

bool Vm::hasEscape(std::string_view key) const noexcept {
    return impl_->findEscape(key) != nullptr;
}

std::size_t Vm::escapeCount() const noexcept { return impl_->escapes.size(); }

bool EscapeRef::armed() const { return machine_->escapeArmed(key_); }
void EscapeRef::armed(bool on) { machine_->setEscapeArmed(key_, on); }
void EscapeRef::remove() { machine_->removeEscape(key_); }

EscapeRef EscapeTable::operator[](std::string_view key) const {
    if (!machine_->hasEscape(key)) {
        throw std::out_of_range("escapes: this machine declares no escape named '" +
                                std::string(key) + "'");
    }
    return EscapeRef(*machine_, ObjectKey(key));
}

bool EscapeTable::contains(std::string_view key) const { return machine_->hasEscape(key); }

std::size_t EscapeTable::size() const { return machine_->escapeCount(); }

void Vm::hostDriver(const DriverBinding& binding) {
    if (binding.isa != isaFor(impl_->platform)) {
        throw std::invalid_argument(
            "hostDriver: the binding's ISA does not match this VM's platform");
    }
    // Configure the cartridge image (place + validate); stackTop 0 = the backend's default scratch top.
    impl_->backend->configureResidentImage(std::span<const DriverImage>(binding.images),
                                           binding.mapper, binding.stackTop.value_or(0));
    impl_->driver.hosted = true;
    impl_->driver.tickEntry = binding.tickEntry;
    impl_->driver.slots = binding.slots;
    // Perform the declared .init once — the engine runs it at host time (Gap 3: no call site exists).
    if (binding.init.has_value()) {
        performInstruction(*binding.init, kInitCycleCap);
    }
}

std::uint64_t Vm::tickDriver(std::span<const Instruction> queued, std::uint64_t cyclesPerFrame) {
    if (!impl_->driver.hosted) {
        throw std::logic_error("tickDriver: no driver is hosted on this VM (call hostDriver first)");
    }
    std::uint64_t spent = 0;
    // Perform queued gestures in submission order (mailbox writes and entry calls).
    for (const Instruction& ins : queued) {
        const std::uint64_t cap = (cyclesPerFrame > spent) ? (cyclesPerFrame - spent) : 1;
        spent += performInstruction(ins, cap);
    }
    // Call the per-frame tick entry to its return (no register presets).
    const std::uint64_t tickCap = (cyclesPerFrame > spent) ? (cyclesPerFrame - spent) : 1;
    spent += impl_->backend->callResident(impl_->driver.tickEntry, {}, tickCap);
    // Idle the machine for the remainder so the APU synthesizes at the hardware cadence.
    if (spent < cyclesPerFrame) {
        impl_->backend->advanceClock(cyclesPerFrame - spent);
    }
    return spent;
}

std::uint64_t Vm::readSlot(std::size_t index) {
    if (!impl_->driver.hosted) {
        throw std::logic_error("readSlot: no driver is hosted on this VM");
    }
    if (index >= impl_->driver.slots.size()) {
        throw std::out_of_range("readSlot: slot index " + std::to_string(index) +
                                " is out of range (" + std::to_string(impl_->driver.slots.size()) +
                                " slots declared)");
    }
    const SlotSpec& s = impl_->driver.slots[index];
    return impl_->backend->readMemory(s.address, s.width);
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
        detail::warnEmbedNotBaked("routine", logicalPath);
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

// ── The deterministic seam (vm_testing.h) ───────────────────────────────────────────────────────

namespace vm {

std::unique_ptr<VmBackend> VmTestAccess::substituteBackend(Vm& v,
                                                           std::unique_ptr<VmBackend> backend) {
    std::unique_ptr<VmBackend> previous = std::move(v.impl_->backend);
    v.impl_->backend = std::move(backend);
    // The sink belongs to the machine that holds it, so the replacement is handed its own on the
    // next declaration rather than inheriting one installed elsewhere.
    v.impl_->escapeSinkInstalled = false;
    return previous;
}

void VmTestAccess::runInline(Vm& v) { v.impl_->startRun(&v, VmRunner::Mode::Inline); }

std::uint64_t VmTestAccess::stepOnce(Vm& v) { return v.impl_->romRun.runner->stepOnce(); }

void VmTestAccess::tornPublishBegin(Vm& v) {
    v.impl_->romRun.seq.fetch_add(1, std::memory_order_release);  // -> odd (mid-flight)
}

void VmTestAccess::tornPublishEnd(Vm& v) {
    v.impl_->capturePublished();
    v.impl_->romRun.seq.fetch_add(1, std::memory_order_release);  // -> even (stable)
}

bool VmTestAccess::readIsStable(const Vm& v) {
    return (v.impl_->romRun.seq.load(std::memory_order_acquire) & 1u) == 0;
}

std::uint32_t VmTestAccess::publishSeq(const Vm& v) {
    return v.impl_->romRun.seq.load(std::memory_order_acquire);
}

}  // namespace vm

}  // namespace retropp
