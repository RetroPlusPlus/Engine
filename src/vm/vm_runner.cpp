#include "src/vm/vm_runner.h"

#include <span>
#include <stdexcept>
#include <utility>

namespace retropp::vm {

VmRunner::VmRunner(Vm machine, StepKind kind, std::uint64_t cyclesPerStep)
    : machine_(std::move(machine)),
      kind_(kind),
      cyclesPerStep_(cyclesPerStep),
      mailbox_(kMailboxCapacity),
      // The landing buffer is sized once and reused: a drain writes over its front and the step reads
      // the count the drain returned, so no step allocates.
      drained_(kMailboxCapacity) {}

bool VmRunner::enqueue(const Instruction& gesture) {
    if (kind_ != StepKind::Resident) {
        throw std::logic_error(
            "VmRunner::enqueue: a gesture is performed through a resident machine's declared entries — "
            "this runner drives a started driver, which has none");
    }
    return mailbox_.push(gesture);
}

std::uint64_t VmRunner::stepOnce() {
    std::uint64_t spent = 0;
    if (kind_ == StepKind::Resident) {
        // Drain first: the gestures this step performs are the ones already offered, and a gesture
        // arriving during the step waits for the next one.
        const std::size_t queued = mailbox_.pop(std::span<Instruction>(drained_));
        spent = machine_.tickDriver(std::span<const Instruction>(drained_.data(), queued),
                                    cyclesPerStep_);
    } else {
        spent = machine_.stepDriver(cyclesPerStep_);
    }
    if (afterStep_) {
        afterStep_();
    }
    return spent;
}

void VmRunner::afterEachStep(std::function<void()> hook) { afterStep_ = std::move(hook); }

}  // namespace retropp::vm
