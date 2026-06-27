// AudioSystem implementation.
//
// The Impl owns everything the public surface hides: the VM that hosts the sound driver, the SPSC ring
// the APU produces into, the registered driver routines, the wiring to the sink, and the dedicated
// PRODUCTION THREAD that runs the VM. The public AudioSystem is a thin pimpl over it, so no VM,
// ring-buffer, or thread type reaches the public header.
//
// Production runs off the main thread: the game's play()/stop() marshal a command onto a lock-free SPSC
// cue queue and wake the production thread; that thread owns the VM, drains the queue, and runs the
// device-paced refill-to-target loop in frame-quantized steps. The VM core is deterministic and each pass
// runs whole frames, so the produced PCM does not depend on how production is chunked across passes.
#include "retropp/audio_system.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include "retropp/asset_policy.h"      // resolveAssetPolicy
#include "retropp/asset_registry.h"    // assetRoot — the single project-relative resource root
#include "retropp/audio_library.h"     // the single catalog play() reads entries from
#include "retropp/routine_registry.h"  // detail::findEmbeddedRoutine
#include "retropp/sdl_platform.h"      // SdlAudioSink — the auto-owned production sink (ctor 3)
#include "src/audio/audio_system_testing.h"  // detail::AudioSystemTestAccess — the synchronous test seam
#include "src/audio/cue_queue.h"       // audio::AudioCommand / CueQueue — the main→production channel
#include "src/audio/produce_step.h"    // detail::produceFrame / ProduceConfig — the pure produce pass
#include "src/audio/ring_buffer.h"

namespace retropp {

namespace {
// The output buffer is kept filled to ~`targetFrames` (a small latency buffer, sampleRate / 20 ≈ 50 ms)
// and sized far larger (sampleRate / 4 ≈ 250 ms) so device-drain bursts never starve it. Production
// steps the driver in WHOLE-FRAME cycle units (the frame quantum, TimingProfile::cpuCyclesPerTick) — see
// produce_step.h for why the frame quantum is a scheduling choice that does not change the samples.

// How long the production thread parks between periodic refills WHILE PLAYING. Must be strictly less than
// the latency buffer's drain time (targetFrames / sampleRate ≈ 50 ms) so the ring never drains empty
// between wakes; a few ms tops it well ahead of drain and the device-paced refill-to-target self-corrects
// any residual drift, exactly as the old per-tick refill did. (Idle — nothing playing — is an UNtimed
// wait: zero wakeups until a cue arrives, preserving the auto-close CPU win.)
constexpr std::chrono::milliseconds kProductionWaitInterval{4};

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
    if (resolveAssetPolicy(entry.policy, AssetPolicy::Embed) == AssetPolicy::Embed) {
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
    // are torn down — safe because the dtor stops the sink AND joins the production thread before any of
    // this runs). The active sink is always reached through `sink`, so the wiring below is identical on
    // both paths.
    std::unique_ptr<AudioSink>        ownedSink;
    AudioSink&                        sink;
    unsigned                          sampleRate;
    std::size_t                       targetFrames;   // keep the buffer filled to ~this (latency buffer)
    std::size_t                       autoStopSilenceFrames;  // a one-shot SFX auto-closes after this many
                                                              // consecutive exact-zero output frames (~250ms)
    std::uint64_t                     cyclesPerFrame;  // the frame quantum — one stepDriver() runs this many
    int                               maxStepsPerWake;  // safety cap on steps per produce pass
    audio::SpscRingBuffer<AudioFrame> ring;
    Vm                                vm;
    // Per-VM materialization cache. The catalog (kind / type / bytes-or-path) lives in the single
    // AudioLibrary; THIS system places a definition into ITS OWN vm as a driver the first time it plays
    // that AudioId, indexed by the id, and reuses the Routine after. Sharing the one library across N
    // systems is exactly this: the catalog is shared, the placed per-vm drivers are not (each voice
    // materializes its own on demand) — which is also why a Routine, a handle into one vm, never lives
    // in the library. Production-thread-only (placement happens when a Play command is applied there).
    std::vector<std::optional<Routine<void()>>> driverCache;
    std::atomic<std::size_t>          framesDropped{0};
    std::atomic<std::size_t>          underflowFrames{0};

    // ── production thread + cross-thread cueing ──────────────────────────────────────────────────────
    // The cue channel (main→production) and the wait/wake the production thread parks on. The cue queue
    // is lock-free SPSC (main pushes in play()/stop(), production pops in drainCues()); the mutex + cv
    // govern ONLY the thread's park/wake, never the cue data or the PCM ring. `running` gates the loop;
    // `threaded` records whether a production thread was started (false in the test's manual mode, where
    // play()/stop() apply their cue inline on the calling thread). `playing` is the cross-thread status
    // flag — written by the production thread when it applies Play/Stop/auto-close, read by isPlaying().
    audio::CueQueue          cueQueue{256};
    std::mutex               mtx;
    std::condition_variable  cv;
    std::atomic<bool>        running{false};
    bool                     threaded = false;
    std::atomic<bool>        playing{false};
    std::thread              productionThread;

    // Auto-close lifecycle (production-thread-only — the APU producer below and the auto-close check both
    // run on that thread): `currentType` is the type of the audio cued by the last applied Play;
    // `silenceRun` is the count of consecutive exact-zero output frames produced. A finished one-shot SFX
    // trips detail::shouldAutoStop() and stops being stepped. Single-writer by construction → no atomic.
    AudioType                currentType = AudioType::Music;  // Music default = never auto-close
    std::size_t              silenceRun  = 0;

    // BORROW: `ownedSink` stays null; `sink` binds the external reference (non-owning).
    Impl(AudioSink& s, VMPlatform platform, TimingProfile timing, unsigned rate)
        : ownedSink(nullptr),
          sink(s),
          sampleRate(rate),
          targetFrames(rate / 20),
          autoStopSilenceFrames(rate / 4),
          cyclesPerFrame(cyclesPerFrameFor(timing)),
          maxStepsPerWake(maxStepsPerWakeFor(timing, rate)),
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
          cyclesPerFrame(cyclesPerFrameFor(timing)),
          maxStepsPerWake(maxStepsPerWakeFor(timing, rate)),
          ring(rate / 4),
          vm(platform, timing) {
        wire();
    }

    // The frame quantum: the CPU cycles in one render tick (= one driver frame). Falls back to the Game
    // Boy frame if the profile carries no CPU model (degenerate — every GB-family preset carries one).
    static std::uint64_t cyclesPerFrameFor(TimingProfile timing) {
        const std::uint32_t perFrame = timing.cpuCyclesPerTick();
        return perFrame != 0 ? perFrame : 70'224u;
    }

    // Steps needed to fill the latency buffer from empty (ceil(target / framesPerStep)) plus slack. The
    // device drains the ring on its own clock, so a wake usually needs far fewer; this only bounds a
    // fill-from-empty pass (and any runaway). Rate-independent ≈ 3 for the GB family, but derived so an
    // atypical profile (much smaller per-frame budget) still fills rather than under-running silently.
    static int maxStepsPerWakeFor(TimingProfile timing, unsigned rate) {
        const std::uint64_t cpuClock = timing.cpu ? timing.cpu->cpuClockHz : 4'194'304u;
        const std::uint64_t framesPerStep =
            std::max<std::uint64_t>(cyclesPerFrameFor(timing) * rate / cpuClock, 1);
        const std::uint64_t target = rate / 20;
        return static_cast<int>((target + framesPerStep - 1) / framesPerStep) + 2;  // ceil + slack
    }

    // Wire the APU producer and the sink consumer to the ring — identical on both construction paths,
    // since the active sink is always reached through `sink`.
    void wire() {
        // Producer side: each APU sample becomes a ring push, on the PRODUCTION THREAD (inside the
        // produce pass run by the production loop, or by the test seam in manual mode). A ring-full push
        // drops the frame and counts it — production never blocks on a full ring.
        vm.enableAudio(sampleRate, [this](std::int16_t left, std::int16_t right) {
            // Auto-close bookkeeping: track the run of consecutive exact-zero output frames. A finished
            // one-shot SFX's DAC-on tail settles to exact (0,0) (verified against the real drivers), while
            // an active tone is high-pass-centred and oscillates through 0 — so the run only reaches the
            // threshold once the sound has actually stopped. Production-thread-only (no atomic needed).
            silenceRun = (left == 0 && right == 0) ? silenceRun + 1 : 0;
            if (!ring.push(AudioFrame{left, right})) {
                framesDropped.fetch_add(1, std::memory_order_relaxed);
            }
        });
        // Begin draining immediately: the sink pulls (its audio thread) from the ring; empty ring =>
        // silence until something plays. The pull is the consumer side of the one SPSC PCM hand-off.
        sink.start(sampleRate, kAudioChannels, [this](std::span<AudioFrame> out) -> std::size_t {
            const std::size_t got = ring.pop(out);
            if (got < out.size()) {
                underflowFrames.fetch_add(out.size() - got, std::memory_order_relaxed);
            }
            return got;
        });
    }

    // ── Cue application (production thread, or inline in manual mode) ─────────────────────────────────
    // Drain every queued cue and apply it. SPSC: only the production thread (or, in manual mode, the
    // single calling thread) pops here.
    void drainCues() {
        std::array<audio::AudioCommand, 1> one;
        while (cueQueue.pop(std::span<audio::AudioCommand>(one)) == 1) {
            applyCommand(one[0]);
        }
    }

    void applyCommand(const audio::AudioCommand& cmd) {
        switch (cmd.op) {
            case audio::AudioCommand::Op::Play:
                applyPlay(cmd.id);
                break;
            case audio::AudioCommand::Op::Stop:
                // Stop stepping the driver: the APU stops producing, the ring drains, the sink
                // silence-fills. Registered audio stays registered — a later Play re-cues it.
                playing.store(false, std::memory_order_relaxed);
                break;
        }
    }

    // Apply a Play cue: bounds-check the id, materialize the driver into THIS VM on first use, position
    // it, and flip the status. VM-owning, so this runs ONLY on the production thread (or the manual
    // calling thread). Single instance: starting one preempts any current driver (the original hardware's
    // natural channel-stealing; a future routing mode places Music / Sfx on separate instances so they
    // coexist).
    void applyPlay(AudioId id) {
        const AudioLibrary& library = AudioLibrary::instance();
        const auto index = static_cast<std::size_t>(id);
        if (index >= library.size()) {
            return;  // unknown handle — nothing to cue
        }
        const AudioLibrary::Entry& entry = library.entry(id);
        if (index >= driverCache.size()) {
            driverCache.resize(index + 1);
        }
        if (!driverCache[index].has_value()) {
            if (entry.kind != AudioKind::Chiptune) {
                return;  // PCM: the sample-mixer arm is a future seam — no VM driver to place yet.
            }
            // Place the portable definition into THIS system's VM as a continuously-running, no-I/O,
            // hardware-speed driver (ISA-verified, then bytes / baked-Embed / LoadFromPath-assembled).
            driverCache[index] = placeChiptune(vm, entry);
        }
        vm.startDriver(*driverCache[index]);
        playing.store(true, std::memory_order_relaxed);
        currentType = entry.type;  // Sfx auto-closes when its output goes silent; Music never does
        silenceRun  = 0;           // a fresh cue counts its silence run from scratch
    }

    // One produce pass: top the ring back up to its target in frame-quantized steps, then auto-close a
    // finished one-shot SFX. The pure decision lives in produce_step.h so the production loop and the
    // synchronous test drive identical code.
    void produceOnce() {
        const detail::ProduceConfig cfg{
            .targetFrames          = targetFrames,
            .cyclesPerFrame        = cyclesPerFrame,
            .maxStepsPerWake       = maxStepsPerWake,
            .autoStopSilenceFrames = autoStopSilenceFrames,
            .currentType           = currentType,
        };
        detail::produceFrame(vm, ring, cfg, silenceRun, playing);
    }

    // ── Production thread ────────────────────────────────────────────────────────────────────────────
    // Start the self-pacing production thread (the threaded ctors call this after construction; the
    // manual test ctor does not, leaving `threaded` false). From here the VM is touched ONLY by this
    // thread — applyPlay/produceOnce/drainCues never run elsewhere.
    void startProductionThread() {
        threaded = true;
        running.store(true, std::memory_order_relaxed);
        productionThread = std::thread([this] { productionLoop(); });
    }

    // Signal the loop to exit, wake it, and join — a no-op in manual mode (no thread was started). MUST
    // run before the Impl members (vm/ring/cueQueue) destruct, since the thread is their only producer.
    void stopProductionThread() {
        if (!productionThread.joinable()) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mtx);
            running.store(false, std::memory_order_relaxed);
        }
        cv.notify_one();
        productionThread.join();
    }

    // The self-pacing loop: drain cues, produce while playing, then park — timed while playing (periodic
    // refill), untimed while idle (zero wakeups until a cue). Woken early by play()/stop()/shutdown.
    void productionLoop() {
        for (;;) {
            drainCues();
            if (playing.load(std::memory_order_relaxed)) {
                produceOnce();
            }

            std::unique_lock<std::mutex> lock(mtx);
            if (!running.load(std::memory_order_relaxed)) {
                break;
            }
            // A cue that landed during the produce pass above is visible here under the lock — handle it
            // without waiting (closes the lost-wakeup window: play()/stop() push THEN take this mutex to
            // notify, so a push is always observed either here or by the wait predicate below).
            if (cueQueue.sizeApprox() > 0) {
                continue;
            }
            if (playing.load(std::memory_order_relaxed)) {
                cv.wait_for(lock, kProductionWaitInterval);  // periodic device-paced refill
            } else {
                cv.wait(lock, [this] {
                    return !running.load(std::memory_order_relaxed) || cueQueue.sizeApprox() > 0;
                });  // park until a cue arrives or shutdown
            }
        }
    }

    // Enqueue is on the main thread; this routes the wake: a started production thread is notified, while
    // manual mode (no thread) applies the cue inline on the calling thread so a test sees it immediately.
    void wakeOrApply() {
        if (threaded) {
            {
                std::lock_guard<std::mutex> lock(mtx);  // order the push before the production thread's
            }                                            // predicate check / wait entry (no lost wakeup)
            cv.notify_one();
        } else {
            drainCues();
        }
    }
};

AudioSystem::AudioSystem(AudioSink& sink, VMPlatform platform, TimingProfile timing,
                         unsigned sampleRate)
    : impl_(std::make_unique<Impl>(sink, platform, timing, sampleRate)) {
    impl_->startProductionThread();
}

AudioSystem::AudioSystem(std::unique_ptr<AudioSink> sink, VMPlatform platform, TimingProfile timing,
                         unsigned sampleRate)
    : impl_(std::make_unique<Impl>(std::move(sink), platform, timing, sampleRate)) {
    impl_->startProductionThread();
}

// The zero-boilerplate default: own an internally-constructed production sink. Delegates to the owning
// ctor with a fresh SdlAudioSink — adds only an include, no new library dependency (sdl_platform.cpp is
// already in this static lib). A non-SDL audio backend uses the injection seam (the two ctors above).
AudioSystem::AudioSystem(VMPlatform platform, TimingProfile timing, unsigned sampleRate)
    : AudioSystem(std::make_unique<SdlAudioSink>(), platform, timing, sampleRate) {}

// Manual (thread-suppressed) construction for the internal test seam. Borrows `sink`; leaves `threaded`
// false so play()/stop() apply inline and the test drives production via AudioSystemTestAccess.
AudioSystem::AudioSystem(ManualTag, AudioSink& sink, VMPlatform platform, TimingProfile timing,
                         unsigned sampleRate)
    : impl_(std::make_unique<Impl>(sink, platform, timing, sampleRate)) {}

AudioSystem::~AudioSystem() {
    // Stop the sink first so its audio thread stops pulling the ring, THEN join the production thread so
    // it stops pushing the ring and touching the VM, THEN the Impl (cue queue + ring + vm + the APU
    // callback) is destroyed — no produced or pulled frame touches a dead ring or VM.
    impl_->sink.stop();
    impl_->stopProductionThread();
}

void AudioSystem::play(AudioId id) {
    impl_->cueQueue.push(audio::AudioCommand{audio::AudioCommand::Op::Play, id});
    impl_->wakeOrApply();
}

void AudioSystem::stop() {
    impl_->cueQueue.push(audio::AudioCommand{audio::AudioCommand::Op::Stop, AudioId{}});
    impl_->wakeOrApply();
}

bool        AudioSystem::isPlaying() const noexcept { return impl_->playing.load(std::memory_order_relaxed); }
std::size_t AudioSystem::framesBuffered() const noexcept { return impl_->ring.sizeApprox(); }
std::size_t AudioSystem::framesDropped() const noexcept {
    return impl_->framesDropped.load(std::memory_order_relaxed);
}
std::size_t AudioSystem::underflowFrames() const noexcept {
    return impl_->underflowFrames.load(std::memory_order_relaxed);
}

// ── Internal test seam (src/audio/audio_system_testing.h) ────────────────────────────────────────────
// Defined here, where Impl is complete. A friend of AudioSystem, so it reaches the private manual ctor
// and impl_. Drives production synchronously on the calling thread — the deterministic path device-free
// tests use in place of the autonomous production thread.
namespace detail {

std::unique_ptr<AudioSystem> AudioSystemTestAccess::makeManual(AudioSink& sink, VMPlatform platform,
                                                               TimingProfile timing, unsigned sampleRate) {
    return std::unique_ptr<AudioSystem>(
        new AudioSystem(AudioSystem::ManualTag{}, sink, platform, timing, sampleRate));
}

void AudioSystemTestAccess::step(AudioSystem& sys) {
    sys.impl_->drainCues();
    if (sys.impl_->playing.load(std::memory_order_relaxed)) {
        sys.impl_->produceOnce();
    }
}

std::uint64_t AudioSystemTestAccess::stepDriverRaw(AudioSystem& sys, std::uint64_t cycles) {
    sys.impl_->drainCues();
    if (sys.impl_->playing.load(std::memory_order_relaxed)) {
        return sys.impl_->vm.stepDriver(cycles);
    }
    return 0;
}

}  // namespace detail

}  // namespace retropp
