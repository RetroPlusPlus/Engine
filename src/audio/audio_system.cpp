// ENG-4.A — AudioSystem implementation.
//
// The Impl owns everything the public surface hides: the VM that hosts the sound driver, the SPSC ring
// the APU produces into, the registered driver routines, and the wiring to the sink. The public
// AudioSystem is a thin pimpl over it, so no VM or ring-buffer type reaches the public header.
#include "retropp/audio_system.h"

#include <atomic>
#include <span>
#include <vector>

#include "src/audio/ring_buffer.h"

namespace retropp {

namespace {
// The output buffer is kept filled to ~`targetFrames` (a small latency buffer, sampleRate / 20 ≈ 50 ms)
// and sized far larger (sampleRate / 4 ≈ 250 ms) so device-drain bursts and tick jitter never starve
// it — the fill stays at the target, so latency is bounded by the target, not the capacity. Production
// per tick is the deficit, run in `kChunkFrames`-sized pieces so it never overshoots the target much.
constexpr std::size_t kChunkFrames      = 128;
constexpr int         kMaxChunksPerTick = 64;   // bounds one tick's work (fill-from-empty + slack)
}  // namespace

struct AudioSystem::Impl {
    AudioSink&                        sink;
    unsigned                          sampleRate;
    std::size_t                       targetFrames;   // keep the buffer filled to ~this (latency buffer)
    std::uint64_t                     cyclesPerChunk;  // CPU cycles whose APU output is ~kChunkFrames
    audio::SpscRingBuffer<AudioFrame> ring;
    Vm                                vm;
    std::vector<Routine<void()>>      drivers;     // one per registered audio, indexed by AudioId
    std::vector<AudioType>            types;        // parallel to drivers — the Music/Sfx tag (ENG-4.D)
    std::atomic<std::size_t>          framesDropped{0};
    std::atomic<std::size_t>          underflowFrames{0};
    bool                              playing = false;

    Impl(AudioSink& s, VMPlatform platform, TimingProfile timing, unsigned rate)
        : sink(s),
          sampleRate(rate),
          targetFrames(rate / 20),
          cyclesPerChunk(static_cast<std::uint64_t>(timing.cpu ? timing.cpu->cpuClockHz : 4'194'304u) *
                         kChunkFrames / rate),
          ring(rate / 4),
          vm(platform, timing) {
        // Producer side: each APU sample becomes a ring push, on the main loop (inside tick()). A
        // ring-full push drops the frame and counts it — the sim never blocks on audio.
        vm.enableAudio(sampleRate, [this](std::int16_t left, std::int16_t right) {
            if (!ring.push(AudioFrame{left, right})) {
                framesDropped.fetch_add(1, std::memory_order_relaxed);
            }
        });
        // Begin draining immediately: the sink pulls (its audio thread) from the ring; empty ring =>
        // silence until something plays. The pull is the consumer side of the one SPSC hand-off.
        sink.start(sampleRate, kAudioChannels, [this](std::span<AudioFrame> out) -> std::size_t {
            const std::size_t got = ring.pop(out);
            if (got < out.size()) {
                underflowFrames.fetch_add(out.size() - got, std::memory_order_relaxed);
            }
            return got;
        });
    }
};

AudioSystem::AudioSystem(AudioSink& sink, VMPlatform platform, TimingProfile timing,
                         unsigned sampleRate)
    : impl_(std::make_unique<Impl>(sink, platform, timing, sampleRate)) {}

AudioSystem::~AudioSystem() {
    // Stop the sink first so its audio thread stops pulling the ring, THEN the Impl (ring + vm + the
    // APU callback that pushes the ring) is destroyed — no produced or pulled frame touches a dead ring.
    impl_->sink.stop();
}

AudioId AudioSystem::registerAudio(std::string_view asmFilePath, AudioType type) {
    // The driver is a continuously-running, no-I/O, hardware-speed routine. Assembled in this system's
    // console ISA by the owned VM's backend; the game supplies a path, never a Vm or Routine.
    Routine<void()> driver = impl_->vm.registerRoutine<void()>(
        asmFilePath, RoutineBinding{.throttle = Throttle::HardwareSpeed});
    impl_->drivers.push_back(driver);
    impl_->types.push_back(type);
    return static_cast<AudioId>(impl_->drivers.size() - 1);
}

void AudioSystem::play(AudioId id) {
    const auto index = static_cast<std::size_t>(id);
    if (index >= impl_->drivers.size()) {
        return;  // unknown handle — nothing to cue
    }
    impl_->vm.startDriver(impl_->drivers[index]);  // single instance: preempts any current driver
    impl_->playing = true;
}

void AudioSystem::stop() {
    // Stop stepping the driver: the APU stops producing, the ring drains, the sink silence-fills.
    impl_->playing = false;
}

void AudioSystem::tick() {
    if (!impl_->playing) {
        return;
    }
    // Refill the output buffer TOWARD its target rather than producing a fixed amount: the audio device
    // drains on its own clock, which drifts from the host clock pacing these ticks, so matching
    // production to the buffer's actual level is what keeps the stream from slowly starving (crackle) or
    // backing up (lag), and self-corrects run-to-run timing variance. On the first tick after play() the
    // deficit is the whole target, so the buffer primes in one go (no startup starve). Each piece is
    // small so production overshoots the target by at most one chunk.
    for (int chunk = 0;
         chunk < kMaxChunksPerTick && impl_->ring.sizeApprox() < impl_->targetFrames;
         ++chunk) {
        impl_->vm.stepDriver(impl_->cyclesPerChunk);  // the APU pushes ~kChunkFrames into the ring
    }
}

std::size_t AudioSystem::framesBuffered() const noexcept { return impl_->ring.sizeApprox(); }
std::size_t AudioSystem::framesDropped() const noexcept {
    return impl_->framesDropped.load(std::memory_order_relaxed);
}
std::size_t AudioSystem::underflowFrames() const noexcept {
    return impl_->underflowFrames.load(std::memory_order_relaxed);
}

}  // namespace retropp
