// SameBoy backend implementation. The only engine TU that includes
// gb.h; everything SameBoy is contained here (the header is pimpl'd).
#include "src/vm/gameboy/sameboy_machine.h"

#include <utility>

#include "gb.h"

namespace retropp::vm {

// Embeds the SameBoy instance plus the run-to-return bookkeeping the execution
// callback writes. Heap-held behind the pimpl so the header pulls no GB_* type.
struct SameBoyMachine::Impl {
    GB_gameboy_t gb{};
    ConsoleModel model;

    // run-to-return state, read/written by the static execution callback below.
    std::uint16_t returnAddress = 0;
    std::size_t instructionCount = 0;
    bool reachedReturn = false;
    bool running = false;

    // Audio: the sink the APU sample callback forwards each frame to.
    SameBoyMachine::SampleSink sampleSink;

    explicit Impl(ConsoleModel m) : model(m) {
        const GB_model_t sbModel =
            (m == ConsoleModel::GameBoyColor) ? GB_MODEL_CGB_E : GB_MODEL_DMG_B;
        GB_init(&gb, sbModel);
        GB_set_user_data(&gb, this);
        // The VM is headless — it never displays. Disable PPU rendering so running the CPU for
        // extended periods (e.g. ticking the divider between RNG calls) never drives the PPU to
        // invoke the pixel/colour-encode callbacks this surgical backend never sets (a null call
        // → crash). Short routine calls never render; sustained idle does.
        GB_set_rendering_disabled(&gb, true);
    }

    ~Impl() { GB_free(&gb); }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
};

namespace {

GB_direct_access_t toDirectAccess(MemoryRegion region) {
    switch (region) {
        case MemoryRegion::Rom:     return GB_DIRECT_ACCESS_ROM;
        case MemoryRegion::WorkRam: return GB_DIRECT_ACCESS_RAM;
        case MemoryRegion::Hram:    return GB_DIRECT_ACCESS_HRAM;
        case MemoryRegion::Io:      return GB_DIRECT_ACCESS_IO;
    }
    return GB_DIRECT_ACCESS_HRAM;  // unreachable; quiets -Wreturn-type
}

// Fires once per instruction at its start (sm83_cpu.c passes the opcode's own
// address). One GB_run == one GB_cpu_run == one instruction, so the count is
// exact. Watches for control reaching the declared return address.
void executionCallback(GB_gameboy_t* gb, std::uint16_t address, std::uint8_t /*opcode*/) {
    auto* impl = static_cast<SameBoyMachine::Impl*>(GB_get_user_data(gb));
    if (impl == nullptr || !impl->running) {
        return;
    }
    ++impl->instructionCount;
    if (address == impl->returnAddress) {
        impl->reachedReturn = true;
    }
}

// Fires once per produced APU output sample. Forwards the stereo frame to the installed
// sink — the producer side of the audio ring buffer. GB_sample_t exposes int16 left/right whether or
// not GB_INTERNAL is defined (this TU compiles without it, so it is the plain struct form).
void sampleCallback(GB_gameboy_t* gb, GB_sample_t* sample) {
    auto* impl = static_cast<SameBoyMachine::Impl*>(GB_get_user_data(gb));
    if (impl != nullptr && impl->sampleSink) {
        impl->sampleSink(sample->left, sample->right);
    }
}

}  // namespace

SameBoyMachine::SameBoyMachine(ConsoleModel model)
    : impl_(std::make_unique<Impl>(model)) {}

SameBoyMachine::~SameBoyMachine() = default;
SameBoyMachine::SameBoyMachine(SameBoyMachine&&) noexcept = default;
SameBoyMachine& SameBoyMachine::operator=(SameBoyMachine&&) noexcept = default;

ConsoleModel SameBoyMachine::model() const { return impl_->model; }

void SameBoyMachine::loadRom(std::span<const std::uint8_t> rom) {
    GB_load_rom_from_buffer(&impl_->gb, rom.data(), rom.size());
}

void SameBoyMachine::reset() { GB_reset(&impl_->gb); }

Registers SameBoyMachine::registers() const {
    const GB_registers_t* r = GB_get_registers(&impl_->gb);
    return Registers{r->af, r->bc, r->de, r->hl, r->sp, r->pc};
}

void SameBoyMachine::setRegisters(const Registers& regs) {
    GB_registers_t* r = GB_get_registers(&impl_->gb);
    r->af = regs.af;
    r->bc = regs.bc;
    r->de = regs.de;
    r->hl = regs.hl;
    r->sp = regs.sp;
    r->pc = regs.pc;
}

std::span<std::uint8_t> SameBoyMachine::memory(MemoryRegion region) {
    std::size_t size = 0;
    std::uint16_t bank = 0;
    void* p = GB_get_direct_access(&impl_->gb, toDirectAccess(region), &size, &bank);
    if (p == nullptr || size == 0) {
        return {};
    }
    return {static_cast<std::uint8_t*>(p), size};
}

std::size_t SameBoyMachine::runToReturn(std::uint16_t returnAddress,
                                        std::size_t maxInstructions) {
    impl_->returnAddress = returnAddress;
    impl_->instructionCount = 0;
    impl_->reachedReturn = false;
    impl_->running = true;
    GB_set_execution_callback(&impl_->gb, &executionCallback);
    while (!impl_->reachedReturn && impl_->instructionCount < maxInstructions) {
        GB_run(&impl_->gb);
    }
    impl_->running = false;
    GB_set_execution_callback(&impl_->gb, nullptr);
    return impl_->instructionCount;
}

void SameBoyMachine::enableAudio(unsigned sampleRate) {
    GB_set_sample_rate(&impl_->gb, sampleRate);
    // Hardware-faithful DC-blocking highpass — matches the filter on real hardware (the faithful
    // default; GB_HIGHPASS_OFF would keep a DC offset, GB_HIGHPASS_REMOVE_DC_OFFSET is non-hardware).
    GB_set_highpass_filter_mode(&impl_->gb, GB_HIGHPASS_ACCURATE);
    GB_apu_set_sample_callback(&impl_->gb, &sampleCallback);
}

void SameBoyMachine::setSampleSink(SampleSink sink) { impl_->sampleSink = std::move(sink); }

std::uint64_t SameBoyMachine::runForCycles(std::uint64_t ticks8MHz) {
    // Step the CPU in raw GB_run increments (each returns the 8 MHz ticks that instruction took)
    // until the budget is met. No execution callback is installed here, so there is no per-instruction
    // hook — only the APU sample callback fires, draining produced PCM to the sink. The headless
    // machine has PPU rendering disabled (see the ctor), so running for extended periods is safe.
    std::uint64_t ran = 0;
    while (ran < ticks8MHz) {
        ran += GB_run(&impl_->gb);
    }
    return ran;
}

}  // namespace retropp::vm
