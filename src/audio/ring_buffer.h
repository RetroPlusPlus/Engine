// ENG-4.A — a single-producer / single-consumer lock-free ring buffer for PCM audio frames.
//
// This is the ONE cross-thread hand-off in the audio chain: the producer is the main loop (the APU
// sample callback pushes frames during a per-tick budgeted VM run); the consumer is the platform's
// audio thread (the sink pops frames to feed the device). It keeps the single-threaded-main-loop
// discipline intact for everything else — no mutex on the audio path, only two atomic cursors.
//
// SPSC contract: exactly one thread calls push(); exactly one (different) thread calls pop(). Calling
// either side from two threads concurrently is undefined. The buffer never blocks: a full push drops
// the overflow (returns a short count), an empty pop returns a short count (the caller silence-fills).
//
// INTERNAL — under src/audio/, never include/retropp/. Header-only so it is unit-testable device-free.
#ifndef RETROPP_SRC_AUDIO_RING_BUFFER_H
#define RETROPP_SRC_AUDIO_RING_BUFFER_H

#include <atomic>
#include <bit>
#include <cstddef>
#include <span>
#include <vector>

namespace retropp::audio {

// A lock-free SPSC ring of T (T must be trivially copyable — audio frames are PODs). Capacity is
// rounded up to a power of two so the cursor wrap is a mask, not a modulo; one slot is reserved to
// distinguish full from empty, so usable capacity is (rounded capacity - 1).
template <typename T>
class SpscRingBuffer {
public:
    // `minCapacity` is the minimum number of items the buffer must hold; the actual capacity is the
    // next power of two ≥ minCapacity+1 (the +1 covers the reserved slot). A zero/one request still
    // yields a usable buffer (capacity 2 → 1 usable).
    explicit SpscRingBuffer(std::size_t minCapacity)
        : buf_(std::bit_ceil(minCapacity < 1 ? std::size_t{2} : minCapacity + 1)),
          mask_(buf_.size() - 1) {}

    // Items the buffer can hold without dropping (one slot is reserved).
    [[nodiscard]] std::size_t capacity() const noexcept { return buf_.size() - 1; }

    // Approximate number of items currently buffered (exact when called from one side only; a racing
    // counterpart may shift it by the time it is read). For diagnostics and tests.
    [[nodiscard]] std::size_t sizeApprox() const noexcept {
        const std::size_t head = head_.load(std::memory_order_acquire);
        const std::size_t tail = tail_.load(std::memory_order_acquire);
        return (head - tail) & mask_;
    }

    // ── Producer side (one thread) ────────────────────────────────────────────────────────────────
    // Push one item. Returns false if the buffer is full (the item is dropped — never blocks).
    bool push(const T& item) noexcept {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t next = (head + 1) & mask_;
        if (next == tail_.load(std::memory_order_acquire)) {
            return false;  // full
        }
        buf_[head] = item;
        head_.store(next, std::memory_order_release);
        return true;
    }

    // Push up to items.size() items. Returns the number actually pushed (< items.size() on overflow).
    std::size_t push(std::span<const T> items) noexcept {
        std::size_t pushed = 0;
        for (const T& item : items) {
            if (!push(item)) {
                break;
            }
            ++pushed;
        }
        return pushed;
    }

    // ── Consumer side (one thread) ────────────────────────────────────────────────────────────────
    // Pop up to out.size() items into `out`. Returns the number actually popped (< out.size() when the
    // buffer drains — the caller fills the remainder, e.g. with silence).
    std::size_t pop(std::span<T> out) noexcept {
        std::size_t tail = tail_.load(std::memory_order_relaxed);
        const std::size_t head = head_.load(std::memory_order_acquire);
        std::size_t popped = 0;
        while (popped < out.size() && tail != head) {
            out[popped] = buf_[tail];
            tail = (tail + 1) & mask_;
            ++popped;
        }
        tail_.store(tail, std::memory_order_release);
        return popped;
    }

private:
    std::vector<T>           buf_;
    std::size_t              mask_;
    std::atomic<std::size_t> head_{0};  // producer writes here next
    std::atomic<std::size_t> tail_{0};  // consumer reads here next
};

}  // namespace retropp::audio

#endif  // RETROPP_SRC_AUDIO_RING_BUFFER_H
