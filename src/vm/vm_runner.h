#ifndef RETROPP_SRC_VM_VM_RUNNER_H
#define RETROPP_SRC_VM_VM_RUNNER_H

// The one home of continuous-execution scheduling: a machine, the gestures waiting for it, and the step
// that advances it. Every continuously-running machine in the engine is driven through a runner.
//
// A runner OWNS its Vm. Everything placed into that machine — a driver routine, a hosted image, the APU
// sink — is reached through machine(), so a handle into it stays valid for as long as the runner lives:
// a runner is neither copyable nor movable, which is what keeps the machine's address fixed under a
// Routine's non-owning pointer.
//
// Gestures arrive through enqueue() and are performed at the next step boundary, in submission order,
// inside that step's cycle budget. The mailbox is the engine's SPSC hand-off — one thread enqueues, the
// stepping thread drains — so a gesture never reaches the machine mid-instruction.
//
// A step is one machine frame, and what that means depends on how the machine runs:
//   Started  — it runs continuously from a placed entry, so a step advances it by the budget.
//   Resident — it performs the drained gestures, calls its tick entry, then idles the remainder of the
//              budget so its hardware synthesizes at the original cadence.
// After the step, the installed hook runs. The hook is how a consumer reads what the step produced
// without the runner knowing what is being read.
//
// A runner drives a machine, whatever that machine is for.
//
// INTERNAL — under src/vm/, never include/retropp/.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include "retropp/driver_binding.h"  // Instruction — the gesture a mailbox carries
#include "retropp/vm.h"              // Vm — the machine a runner owns
#include "src/audio/ring_buffer.h"   // SpscRingBuffer — the engine's lock-free cross-thread hand-off

namespace retropp::vm {

class VmRunner {
public:
    // How a step advances the machine.
    enum class StepKind { Started, Resident };

    // Gestures the mailbox holds between steps. It matches the depth of the cue channel a gesture
    // reaches the mailbox through, so the upstream bound is the binding one; a full mailbox refuses the
    // gesture rather than blocking the thread that offered it.
    static constexpr std::size_t kMailboxCapacity = 256;

    // Take ownership of `machine` and drive it `cyclesPerStep` CPU cycles at a time. Pass a machine that
    // is otherwise untouched: place its content through machine() afterwards, so every handle into it is
    // taken at the address it keeps.
    VmRunner(Vm machine, StepKind kind, std::uint64_t cyclesPerStep);

    VmRunner(const VmRunner&) = delete;
    VmRunner& operator=(const VmRunner&) = delete;
    VmRunner(VmRunner&&) = delete;
    VmRunner& operator=(VmRunner&&) = delete;

    // The machine itself — where content is placed and where its state is read.
    [[nodiscard]] Vm&       machine() noexcept { return machine_; }
    [[nodiscard]] const Vm& machine() const noexcept { return machine_; }

    [[nodiscard]] StepKind      kind() const noexcept { return kind_; }
    [[nodiscard]] std::uint64_t cyclesPerStep() const noexcept { return cyclesPerStep_; }

    // Queue a gesture for the next step. Returns false when the mailbox is full — the gesture is
    // refused, and enqueueing never blocks. Throws std::logic_error on a started machine: a gesture is
    // performed through a resident machine's declared entries, and a started driver has none.
    bool enqueue(const Instruction& gesture);

    // Advance the machine by one step, then run the hook. Returns the CPU cycles the step consumed —
    // for a resident machine the gestures plus the tick call, the idle padding excluded.
    std::uint64_t stepOnce();

    // Install what runs after each step. Replaces any hook already installed.
    void afterEachStep(std::function<void()> hook);

private:
    Vm                                 machine_;
    StepKind                           kind_;
    std::uint64_t                      cyclesPerStep_;
    audio::SpscRingBuffer<Instruction> mailbox_;
    std::vector<Instruction>           drained_;  // the mailbox's landing buffer, sized once
    std::function<void()>              afterStep_;
};

}  // namespace retropp::vm

#endif  // RETROPP_SRC_VM_VM_RUNNER_H
