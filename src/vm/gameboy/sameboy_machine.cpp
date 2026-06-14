// ENG-3.A — SameBoy backend implementation. The only engine TU that includes
// gb.h; everything SameBoy is contained here (the header is pimpl'd).
#include "src/vm/gameboy/sameboy_machine.h"

#include "gb.h"

namespace gbcpp::vm {

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

}  // namespace gbcpp::vm
