#ifndef RETROPP_SRC_VM_VM_TESTING_H
#define RETROPP_SRC_VM_VM_TESTING_H

// The deterministic seam device-free tests step a running cartridge through: the same machinery
// Vm::run() stands up, in the runner's Inline mode — no thread, every step on the calling thread —
// so a test orders boots, steps, writes and reads exactly. The torn-publish pair drives the
// publish's seqlock through its mid-flight state deterministically, which no thread schedule can
// be trusted to do.
//
// INTERNAL — under src/vm/, never include/retropp/; test TUs only.

#include <cstdint>

#include "retropp/vm.h"

namespace retropp::vm {

struct VmTestAccess {
    // Stand the run up inline: boot (first episode), the first publish, both step hooks — and no
    // thread. The machine reports running (reads route to the publish, writes queue) and advances
    // only when stepOnce is called.
    static void runInline(Vm& vm);

    // One step on the calling thread: drain queued writes, advance one budget, publish. Returns the
    // CPU cycles the step consumed.
    static std::uint64_t stepOnce(Vm& vm);

    // Hold the publish mid-flight (sequence to odd), and complete it (capture + sequence to even).
    // Between the two, the machine's published set is officially unstable — what a reader must
    // refuse to return.
    static void tornPublishBegin(Vm& vm);
    static void tornPublishEnd(Vm& vm);

    // One seqlock attempt without the retry loop: whether a read taken now would be stable. The
    // real read() loops on exactly this condition.
    [[nodiscard]] static bool readIsStable(const Vm& vm);

    // The publish sequence word: even when stable, odd mid-flight, advanced by two per publish —
    // how a test pins that every step publishes exactly once.
    [[nodiscard]] static std::uint32_t publishSeq(const Vm& vm);
};

}  // namespace retropp::vm

#endif  // RETROPP_SRC_VM_VM_TESTING_H
