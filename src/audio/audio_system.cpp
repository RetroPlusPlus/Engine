// ENG-4.A — AudioSystem implementation.
//
// The Impl owns everything the public surface hides: the VM that hosts the sound driver, the SPSC ring
// the APU produces into, the registered driver routines, and the wiring to the sink. The public
// AudioSystem is a thin pimpl over it, so no VM or ring-buffer type reaches the public header.
#include "retropp/audio_system.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

#include "retropp/asset_policy.h"      // resolveAssetPolicy
#include "retropp/asset_registry.h"    // assetRoot — the single project-relative resource root
#include "retropp/audio_library.h"     // the single catalog play() reads entries from
#include "retropp/routine_registry.h"  // detail::findEmbeddedRoutine / configDefaultRoutinePolicy
#include "retropp/sdl_platform.h"      // SdlAudioSink — the auto-owned production sink (ctor 3)
#include "src/audio/auto_close.h"      // detail::shouldAutoStop — the pure one-shot-SFX auto-close decision
#include "src/audio/ring_buffer.h"

namespace retropp {

namespace {
// The output buffer is kept filled to ~`targetFrames` (a small latency buffer, sampleRate / 20 ≈ 50 ms)
// and sized far larger (sampleRate / 4 ≈ 250 ms) so device-drain bursts and tick jitter never starve
// it — the fill stays at the target, so latency is bounded by the target, not the capacity. Production
// per tick is the deficit, run in `kChunkFrames`-sized pieces so it never overshoots the target much.
constexpr std::size_t kChunkFrames      = 128;
constexpr int         kMaxChunksPerTick = 64;   // bounds one tick's work (fill-from-empty + slack)

// Materialize a Chiptune catalog entry (registered on the single AudioLibrary) into `vm` as a
// hardware-speed driver and return the callable. This is the per-VM placement deferred to first play():
// the shared library holds the portable definition (bytes or a path + policy, and the ISA the developer
// SELECTED at registration); each system places its OWN copy here. The ISA is VERIFIED first — a cheap
// enum compare of the entry's developer-selected ISA against this VM's ISA, before any file read or
// assembly — so a mismatch throws immediately (in practice at startup, when audio is loaded/warmed up),
// never garbage-running foreign bytes. This is a compatibility CHECK, not selection — selection happened
// on the library at registration.
Routine<void()> placeChiptune(Vm& vm, const AudioLibrary::Entry& entry) {
    if (entry.isa != isaFor(vm.platform())) {
        throw std::runtime_error(
            "AudioSystem::play: this chiptune was registered for a different ISA than this audio "
            "system's VM — it cannot run here");
    }
    const RoutineBinding binding{.throttle = Throttle::HardwareSpeed};

    // Raw-door entry (AudioLibrary::uploadAudio): the bytes are already this ISA's machine code — place
    // them directly.
    if (!entry.bytecode.empty()) {
        return vm.uploadRoutine<void()>(entry.bytecode, binding);
    }
    // Path entry (AudioLibrary::registerAudio): Embed → the build baked the assembled bytes into the
    // routine registry, keyed by the logical path; place them. Falls through to the disk read if none
    // were baked.
    if (resolveAssetPolicy(entry.policy, detail::configDefaultRoutinePolicy(), AssetPolicy::Embed) ==
        AssetPolicy::Embed) {
        if (const std::span<const std::uint8_t> baked = detail::findEmbeddedRoutine(entry.asmPath);
            !baked.empty()) {
            return vm.uploadRoutine<void()>(baked, binding);
        }
    }
    // LoadFromPath (or an un-baked Embed): resolve the full project-relative path against the engine's
    // single assetRoot(), read it, assemble it in this VM's ISA, place.
    const std::filesystem::path full = assetRoot() / std::filesystem::path(entry.asmPath);
    std::ifstream in{full, std::ios::binary};
    if (!in) {
        throw std::runtime_error("AudioSystem::play: cannot open audio .asm file: " + full.string());
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::vector<std::uint8_t> bytes = vm.assemble(ss.str());
    return vm.uploadRoutine<void()>(bytes, binding);
}
}  // namespace

struct AudioSystem::Impl {
    // `ownedSink` is null on the borrow path and holds the sink on the owning path; it is declared
    // FIRST so it is constructed before `sink` is bound to it, and destroyed LAST (after the ring + vm
    // are torn down — safe because the dtor stops the sink before any of this runs). The active sink is
    // always reached through `sink`, so the wiring below is identical on both paths.
    std::unique_ptr<AudioSink>        ownedSink;
    AudioSink&                        sink;
    unsigned                          sampleRate;
    std::size_t                       targetFrames;   // keep the buffer filled to ~this (latency buffer)
    std::size_t                       autoStopSilenceFrames;  // a one-shot SFX auto-closes after this many
                                                              // consecutive exact-zero output frames (~250ms)
    std::uint64_t                     cyclesPerChunk;  // CPU cycles whose APU output is ~kChunkFrames
    audio::SpscRingBuffer<AudioFrame> ring;
    Vm                                vm;
    // Per-VM materialization cache. The catalog (kind / type / bytes-or-path) lives in the single
    // AudioLibrary; THIS system places a definition into ITS OWN vm as a driver the first time it plays
    // that AudioId, indexed by the id, and reuses the Routine after. Sharing the one library across N
    // systems is exactly this: the catalog is shared, the placed per-vm drivers are not (each voice
    // materializes its own on demand) — which is also why a Routine, a handle into one vm, never lives
    // in the library.
    std::vector<std::optional<Routine<void()>>> driverCache;
    std::atomic<std::size_t>          framesDropped{0};
    std::atomic<std::size_t>          underflowFrames{0};
    bool                              playing = false;
    // Auto-close lifecycle (main-thread only — the producer below runs inside tick()): `currentType` is the
    // type of the audio cued by the last play(); `silenceRun` is the count of consecutive exact-zero output
    // frames produced. A finished one-shot SFX trips detail::shouldAutoStop() and stops being stepped.
    AudioType                         currentType = AudioType::Music;  // Music default = never auto-close
    std::size_t                       silenceRun  = 0;

    // BORROW: `ownedSink` stays null; `sink` binds the external reference (non-owning).
    Impl(AudioSink& s, VMPlatform platform, TimingProfile timing, unsigned rate)
        : ownedSink(nullptr),
          sink(s),
          sampleRate(rate),
          targetFrames(rate / 20),
          autoStopSilenceFrames(rate / 4),
          cyclesPerChunk(cyclesPerChunkFor(timing, rate)),
          ring(rate / 4),
          vm(platform, timing) {
        wire();
    }

    // OWN: move the sink into `ownedSink`; `sink` binds to it. `ownedSink` is initialised before `sink`
    // (declaration order), so `*ownedSink` is live when the reference binds.
    Impl(std::unique_ptr<AudioSink> s, VMPlatform platform, TimingProfile timing, unsigned rate)
        : ownedSink(std::move(s)),
          sink(*ownedSink),
          sampleRate(rate),
          targetFrames(rate / 20),
          autoStopSilenceFrames(rate / 4),
          cyclesPerChunk(cyclesPerChunkFor(timing, rate)),
          ring(rate / 4),
          vm(platform, timing) {
        wire();
    }

    static std::uint64_t cyclesPerChunkFor(TimingProfile timing, unsigned rate) {
        return static_cast<std::uint64_t>(timing.cpu ? timing.cpu->cpuClockHz : 4'194'304u) *
               kChunkFrames / rate;
    }

    // Wire the APU producer and the sink consumer to the ring — identical on both construction paths,
    // since the active sink is always reached through `sink`.
    void wire() {
        // Producer side: each APU sample becomes a ring push, on the main loop (inside tick()). A
        // ring-full push drops the frame and counts it — the sim never blocks on audio.
        vm.enableAudio(sampleRate, [this](std::int16_t left, std::int16_t right) {
            // Auto-close bookkeeping: track the run of consecutive exact-zero output frames. A finished
            // one-shot SFX's DAC-on tail settles to exact (0,0) (verified against the real drivers), while
            // an active tone is high-pass-centred and oscillates through 0 — so the run only reaches the
            // threshold once the sound has actually stopped. Same thread as tick() (no atomic needed).
            silenceRun = (left == 0 && right == 0) ? silenceRun + 1 : 0;
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

AudioSystem::AudioSystem(std::unique_ptr<AudioSink> sink, VMPlatform platform, TimingProfile timing,
                         unsigned sampleRate)
    : impl_(std::make_unique<Impl>(std::move(sink), platform, timing, sampleRate)) {}

// The zero-boilerplate default: own an internally-constructed production sink. Delegates to the owning
// ctor with a fresh SdlAudioSink — adds only an include, no new library dependency (sdl_platform.cpp is
// already in this static lib). A non-SDL audio backend uses the injection seam (the two ctors above).
AudioSystem::AudioSystem(VMPlatform platform, TimingProfile timing, unsigned sampleRate)
    : AudioSystem(std::make_unique<SdlAudioSink>(), platform, timing, sampleRate) {}

AudioSystem::~AudioSystem() {
    // Stop the sink first so its audio thread stops pulling the ring, THEN the Impl (ring + vm + the
    // APU callback that pushes the ring) is destroyed — no produced or pulled frame touches a dead ring.
    impl_->sink.stop();
}

void AudioSystem::play(AudioId id) {
    const AudioLibrary& library = AudioLibrary::instance();
    const auto index = static_cast<std::size_t>(id);
    if (index >= library.size()) {
        return;  // unknown handle — nothing to cue
    }
    const AudioLibrary::Entry& entry = library.entry(id);
    if (index >= impl_->driverCache.size()) {
        impl_->driverCache.resize(index + 1);
    }
    if (!impl_->driverCache[index].has_value()) {
        if (entry.kind != AudioKind::Chiptune) {
            return;  // PCM: the sample-mixer arm is a future seam — no VM driver to place yet.
        }
        // Place the portable definition into THIS system's VM as a continuously-running, no-I/O,
        // hardware-speed driver (ISA-verified, then bytes / baked-Embed / LoadFromPath-assembled).
        impl_->driverCache[index] = placeChiptune(impl_->vm, entry);
    }
    impl_->vm.startDriver(*impl_->driverCache[index]);  // single instance: preempts any current driver
    impl_->playing     = true;
    impl_->currentType = entry.type;  // Sfx auto-closes when its output goes silent; Music never does
    impl_->silenceRun  = 0;           // a fresh cue counts its silence run from scratch
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
    // A finished one-shot SFX (output exact-zero past the threshold) stops being stepped: clear `playing`
    // exactly as stop() does, so the next tick() early-returns and the ring drains to the sink's
    // silence-fill. The audible output is unchanged — the driver was already producing (0,0); only the VM
    // stops advancing. Music is never auto-closed (the gate is inside shouldAutoStop).
    if (detail::shouldAutoStop(impl_->silenceRun, impl_->autoStopSilenceFrames, impl_->currentType)) {
        stop();
        impl_->silenceRun = 0;
    }
}

bool        AudioSystem::isPlaying() const noexcept { return impl_->playing; }
std::size_t AudioSystem::framesBuffered() const noexcept { return impl_->ring.sizeApprox(); }
std::size_t AudioSystem::framesDropped() const noexcept {
    return impl_->framesDropped.load(std::memory_order_relaxed);
}
std::size_t AudioSystem::underflowFrames() const noexcept {
    return impl_->underflowFrames.load(std::memory_order_relaxed);
}

}  // namespace retropp
