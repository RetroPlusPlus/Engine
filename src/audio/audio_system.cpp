// AudioSystem implementation.
//
// The Impl owns everything the public surface hides: the active VOICES (each cued sound that is still
// sounding — a chiptune voice hosts its driver on its own VM with its own APU; a PCM voice streams its
// decoded frames), the SPSC ring their mixed output lands in, the wiring to the sink, and the dedicated
// PRODUCTION THREAD that steps every voice. The public AudioSystem is a thin pimpl over it, so no VM,
// ring-buffer, or thread type reaches the public header.
//
// play() NEVER cuts off audio that is already playing. Each cue starts a NEW voice beside the current
// ones, and the production pass sums every voice's post-gain PCM into the one output ring (saturating).
// Any channel contention that exists lives INSIDE a single voice's VM — the console's sound channels,
// allocated by the driver running there, exactly as on the original hardware — never at the system
// level. stop() silences the whole system (per-voice control arrives with the voice-handle surface);
// a finished one-shot SFX voice closes itself. A closing voice never truncates at amplitude — stop(),
// a Retrigger cut, and a PCM buffer's end all ride a short release fade to zero (a hard cut clicks).
//
// Production runs off the main thread: the game's play()/stop() marshal a command onto a lock-free SPSC
// cue queue and wake the production thread; that thread owns the voices, drains the queue, and runs the
// device-paced refill-to-target loop in frame-quantized steps. Each VM core is deterministic and each
// pass runs whole frames, so a voice's produced PCM does not depend on how production is chunked across
// passes.
#include "retropp/audio_system.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
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
#include "retropp/audio_mixer.h"       // AudioMixer::instance().effectiveGain — the per-voice output scale
#include "retropp/routine_registry.h"  // detail::findEmbeddedRoutine
#include "retropp/sdl_platform.h"      // SdlAudioSink — the auto-owned production sink (ctor 3)
#include "src/audio/audio_system_testing.h"  // detail::AudioSystemTestAccess — the synchronous test seam
#include "src/audio/auto_close.h"      // detail::shouldAutoStop — the one-shot-SFX lifecycle decision
#include "src/audio/cue_queue.h"       // audio::AudioCommand / CueQueue — the main→production channel
#include "src/audio/pcm_decode.h"      // detail::g_pcmDecode — the Pcm decode hook (installed by the no-ISA registerAudio)
#include "src/audio/produce_step.h"    // detail::mixFrames / rampFrame — the pure mixdown + release fade
#include "src/audio/ring_buffer.h"

namespace retropp {

namespace {
// The output buffer is kept filled to ~`targetFrames` (a small latency buffer, sampleRate / 20 ≈ 50 ms)
// and sized far larger (sampleRate / 4 ≈ 250 ms) so device-drain bursts never starve it. Production
// steps every chiptune voice in WHOLE-FRAME cycle units (the frame quantum,
// TimingProfile::cpuCyclesPerTick) and mixes after each step; the frame quantum is a scheduling choice
// that does not change any voice's samples (each VM core is deterministic).

// How long the production thread parks between periodic refills WHILE PLAYING. Must be strictly less than
// the latency buffer's drain time (targetFrames / sampleRate ≈ 50 ms) so the ring never drains empty
// between wakes; a few ms tops it well ahead of drain and the device-paced refill-to-target self-corrects
// any residual drift. (Idle — no voices — is an UNtimed wait: zero wakeups until a cue arrives,
// preserving the auto-close CPU win.)
constexpr std::chrono::milliseconds kProductionWaitInterval{4};

// Resolve a Chiptune catalog entry (registered on the single AudioLibrary) to its machine-code bytes and
// place them into `vm` as a hardware-speed driver, returning the callable. This is the per-voice
// placement done at play(): the shared library holds the portable definition (bytes or a path + policy,
// and the ISA the developer SELECTED at registration); each voice places its OWN copy into its own VM.
// The ISA is VERIFIED first — a cheap enum compare of the entry's developer-selected ISA against this
// VM's ISA, before any file read or assembly — so a mismatch throws immediately (in practice at startup,
// when audio is loaded/warmed up), never garbage-running foreign bytes. `cachedAsm` is the per-id
// system-level cache for the assemble path, so replaying a LoadFromPath sound assembles once, not per
// voice.
Routine<void()> placeChiptune(Vm& vm, const AudioLibrary::Entry& entry,
                              std::optional<std::vector<std::uint8_t>>& cachedAsm) {
    if (entry.isa != isaFor(vm.platform())) {
        throw std::runtime_error(
            "AudioSystem::play: this chiptune was registered for a different ISA than this audio "
            "system's VM — it cannot run here");
    }
    const RoutineBinding binding{.throttle = Throttle::HardwareSpeed};

    // Raw-bytes entry (AudioLibrary::uploadAudio): the bytes are already this ISA's machine code — place
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
    // single assetRoot(), read it, assemble it in this VM's ISA once (cached per id thereafter), place.
    if (!cachedAsm.has_value()) {
        const std::filesystem::path full = assetRoot() / std::filesystem::path(entry.asmPath);
        std::ifstream in{full, std::ios::binary};
        if (!in) {
            throw std::runtime_error("AudioSystem::play: cannot open audio .asm file: " + full.string());
        }
        std::ostringstream ss;
        ss << in.rdbuf();
        cachedAsm = vm.assemble(ss.str());
    }
    return vm.uploadRoutine<void()>(*cachedAsm, binding);
}
}  // namespace

struct AudioSystem::Impl {
    // ── A voice: one cued sound, currently sounding ──────────────────────────────────────────────────
    // A Chiptune voice owns its OWN VM (with its own APU) hosting its driver at the hardware clock — so
    // simultaneous voices each get the console's full complement of sound channels, and any channel
    // contention is a single driver's own doing inside its VM, never one voice silencing another. A Pcm
    // voice holds a share of the decoded frames and a read cursor. `lane` is the voice's FIFO of
    // produced-but-unmixed post-gain frames: the APU callback appends, the mix consumes from `laneHead`.
    // The lane is a FIFO (not a per-pass scratch) because simultaneous VMs may emit ±1 frame per step —
    // the mix consumes the minimum available across voices and the remainder simply waits, so no voice's
    // stream ever gains or loses a sample. Voices live behind unique_ptr so the APU callback's captured
    // pointer stays stable however the vector grows. All state here is production-thread-only.
    struct Voice {
        AudioId      id{};  // the catalog entry this voice sounds (Retrigger replaces by matching it)
        AudioType    type;
        std::size_t  silenceRun = 0;  // consecutive exact-zero RAW output frames (auto-close input)

        // Release: a closing voice never truncates at amplitude — it rides a short linear fade
        // (rampFrame) over `rampRemaining` of `rampTotal` frames and is removed at zero. Entered by
        // stop(), by a Retrigger replacing this id, and by a PCM buffer running out (the tail then
        // decays from `lastFrame`, so a file whose final sample is off zero still lands silently).
        // The silence auto-close (a one-shot SFX already at exact zero) removes directly — no ramp.
        bool         releasing = false;
        std::size_t  rampRemaining = 0;
        std::size_t  rampTotal = 0;

        // Chiptune half
        std::optional<Vm>              vm;      // the voice's own VM+APU; nullopt on the Pcm path
        std::optional<Routine<void()>> driver;  // the placed driver (a handle into `vm`)
        std::vector<AudioFrame>        lane;    // produced post-gain frames awaiting the mix
        std::size_t                    laneHead = 0;

        // Pcm half
        std::shared_ptr<const std::vector<AudioFrame>> pcm;  // decoded frames (shared with the cache)
        std::size_t                                    cursor = 0;
        AudioFrame                                     lastFrame{};  // the frame a past-end tail decays from
    };

    // `ownedSink` is null on the borrow path and holds the sink on the owning path; it is declared
    // FIRST so it is constructed before `sink` is bound to it, and destroyed LAST (after the ring +
    // voices are torn down — safe because the dtor stops the sink AND joins the production thread before
    // any of this runs). The active sink is always reached through `sink`, so the wiring below is
    // identical on both paths.
    std::unique_ptr<AudioSink>        ownedSink;
    AudioSink&                        sink;
    unsigned                          sampleRate;
    AudioKind                         kind_;          // the system's fixed backend — Chiptune or Pcm
    VMPlatform                        platform_;      // the console each chiptune voice's VM is built as
    TimingProfile                     timing_;        // the CPU-timing block those VMs run under
    std::size_t                       targetFrames;   // keep the buffer filled to ~this (latency buffer)
    std::size_t                       autoStopSilenceFrames;  // a one-shot SFX auto-closes after this many
                                                              // consecutive exact-zero output frames (~250ms)
    std::size_t                       releaseFrames;  // a closing voice's fade length (~8 ms — the de-click)
    std::uint64_t                     cyclesPerFrame;  // the frame quantum — one stepDriver() runs this many
    int                               maxStepsPerWake;  // safety cap on steps per produce pass
    audio::SpscRingBuffer<AudioFrame> ring;

    // The active voices — every cued sound still sounding, mixed together each pass. Production-thread-
    // only (voices are created when a Play command is applied there, stepped there, and removed there).
    std::vector<std::unique_ptr<Voice>> voices;

    // Per-id caches shared across this system's voices. `asmCache` holds LoadFromPath-assembled driver
    // bytes so replaying a sound assembles once; `pcmCache` holds decoded PCM frames, shared into each
    // voice (shared_ptr keeps a voice's frames alive and stable however the cache vector grows).
    // Production-thread-only, like the voices.
    std::vector<std::optional<std::vector<std::uint8_t>>>       asmCache;
    std::vector<std::shared_ptr<const std::vector<AudioFrame>>> pcmCache;

    std::atomic<std::size_t>          framesDropped{0};
    std::atomic<std::size_t>          underflowFrames{0};

    // Scratch for the mixdown (reused every pass, no per-pass allocation once warm).
    std::vector<AudioFrame> mixScratch;

    // ── production thread + cross-thread cueing ──────────────────────────────────────────────────────
    // The cue channel (main→production) and the wait/wake the production thread parks on. The cue queue
    // is lock-free SPSC (main pushes in play()/stop(), production pops in drainCues()); the mutex + cv
    // govern ONLY the thread's park/wake, never the cue data or the PCM ring. `running` gates the loop;
    // `threaded` records whether a production thread was started (false in the test's manual mode, where
    // play()/stop() apply their cue inline on the calling thread). `playing` is the cross-thread status
    // flag — maintained by the production thread as voices come and go, read by isPlaying().
    audio::CueQueue          cueQueue{256};
    std::mutex               mtx;
    std::condition_variable  cv;
    std::atomic<bool>        running{false};
    bool                     threaded = false;
    std::atomic<bool>        playing{false};
    std::thread              productionThread;

    // BORROW: `ownedSink` stays null; `sink` binds the external reference (non-owning).
    Impl(AudioKind kind, AudioSink& s, VMPlatform platform, TimingProfile timing, unsigned rate)
        : ownedSink(nullptr),
          sink(s),
          sampleRate(rate),
          kind_(kind),
          platform_(platform),
          timing_(timing),
          targetFrames(rate / 20),
          autoStopSilenceFrames(rate / 4),
          releaseFrames(rate * 8 / 1000),
          cyclesPerFrame(cyclesPerFrameFor(timing)),
          maxStepsPerWake(maxStepsPerWakeFor(timing, rate)),
          ring(rate / 4) {
        wire();
    }

    // OWN: move the sink into `ownedSink`; `sink` binds to it. `ownedSink` is initialised before `sink`
    // (declaration order), so `*ownedSink` is live when the reference binds.
    Impl(AudioKind kind, std::unique_ptr<AudioSink> s, VMPlatform platform, TimingProfile timing,
         unsigned rate)
        : ownedSink(std::move(s)),
          sink(*ownedSink),
          sampleRate(rate),
          kind_(kind),
          platform_(platform),
          timing_(timing),
          targetFrames(rate / 20),
          autoStopSilenceFrames(rate / 4),
          releaseFrames(rate * 8 / 1000),
          cyclesPerFrame(cyclesPerFrameFor(timing)),
          maxStepsPerWake(maxStepsPerWakeFor(timing, rate)),
          ring(rate / 4) {
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

    // Wire the sink consumer to the ring — identical on both construction paths, since the active sink
    // is always reached through `sink`. (The producer side is per-voice: each chiptune voice's APU
    // callback is wired when the voice is created in applyPlay.) Begin draining immediately: the sink
    // pulls (its audio thread) from the ring; empty ring => silence until something plays. The pull is
    // the consumer side of the one SPSC PCM hand-off.
    void wire() {
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
                applyPlay(cmd.id, cmd.mode);
                break;
            case audio::AudioCommand::Op::Stop:
                // Silence the SYSTEM: every voice enters its release fade (a truncation at amplitude
                // would click) and is removed at zero within milliseconds; the ring then drains and the
                // sink silence-fills. `playing` clears when the last tail finishes, in the produce pass.
                // Registered audio stays registered — a later Play cues it afresh.
                for (const std::unique_ptr<Voice>& v : voices) {
                    beginRelease(*v);
                }
                break;
        }
    }

    // Apply a Play cue: bounds-check the id, verify its kind matches THIS system's kind, and start a NEW
    // voice for it. Under Layer (the fixed default) the voice starts beside the ones already sounding —
    // play() never touches a playing voice. Under Retrigger the voices already playing the SAME id are
    // closed first (the arcade re-fire restarts the sound); every other voice plays on untouched. A
    // Chiptune voice gets its own VM with the driver materialized into it; a Pcm voice gets a share of
    // the decoded frames. Backend-owning, so this runs ONLY on the production thread (or the manual
    // calling thread).
    void applyPlay(AudioId id, CueMode mode) {
        const AudioLibrary& library = AudioLibrary::instance();
        const auto index = static_cast<std::size_t>(id);
        if (index >= library.size()) {
            return;  // unknown handle — nothing to cue
        }
        const AudioLibrary::Entry& entry = library.entry(id);
        // One kind per system: an id of the other backend cannot play here (the ISA-mismatch precedent —
        // a cheap enum compare before any placement / decode, so a mismatch throws loudly).
        if (entry.kind != kind_) {
            throw std::runtime_error(
                "AudioSystem::play: this audio was registered for a different backend (chiptune vs PCM) "
                "than this audio system — it cannot play here");
        }
        if (mode == CueMode::Retrigger) {
            // Restart THIS sound: the voices already playing the same id enter their release fade (a
            // hard cut would click) while the fresh voice below takes over. Everything else keeps
            // sounding untouched.
            for (const std::unique_ptr<Voice>& v : voices) {
                if (v->id == id) {
                    beginRelease(*v);
                }
            }
        }
        auto voice  = std::make_unique<Voice>();
        voice->id   = id;
        voice->type = entry.type;
        if (kind_ == AudioKind::Chiptune) {
            // The voice's own VM+APU: wire its sample callback into the voice's lane. The callback runs
            // on the production thread (inside stepDriver), so the raw-pointer capture is safe — the
            // voice lives behind unique_ptr (stable address) and is only destroyed on this same thread.
            voice->vm.emplace(platform_, timing_);
            Voice* vp = voice.get();
            voice->vm->enableAudio(sampleRate, [vp](std::int16_t left, std::int16_t right) {
                // Auto-close bookkeeping: track the run of consecutive exact-zero output frames. A
                // finished one-shot SFX's DAC-on tail settles to exact (0,0) (verified against the real
                // drivers), while an active tone is high-pass-centred and oscillates through 0 — so the
                // run only reaches the threshold once the sound has actually stopped. Keyed off the RAW
                // pre-gain sample: a low mixer level could round a still-playing quiet tone to zero, and
                // that must not count as the sound stopping.
                vp->silenceRun = (left == 0 && right == 0) ? vp->silenceRun + 1 : 0;
                // Scale by the voice's bus level (one relaxed atomic load + one integer multiply-shift
                // per channel). At the default unity gain this is the exact identity, so the laned frame
                // is bit-for-bit the produced one.
                const std::uint32_t gain = AudioMixer::instance().effectiveGain(vp->type);
                vp->lane.push_back(AudioFrame{applyGain(left, gain), applyGain(right, gain)});
            });
            if (index >= asmCache.size()) {
                asmCache.resize(index + 1);
            }
            voice->driver = placeChiptune(*voice->vm, entry, asmCache[index]);
            voice->vm->startDriver(*voice->driver);
        } else {  // AudioKind::Pcm — no VM: decode the file once, then the voice plays the shared frames
            if (index >= pcmCache.size()) {
                pcmCache.resize(index + 1);
            }
            if (pcmCache[index] == nullptr) {
                pcmCache[index] =
                    std::make_shared<const std::vector<AudioFrame>>(decodePcmEntry(entry));
            }
            voice->pcm    = pcmCache[index];
            voice->cursor = 0;
        }
        voices.push_back(std::move(voice));
        playing.store(true, std::memory_order_relaxed);
    }

    // Decode a Pcm entry's audio file into stereo frames at this system's sample rate. Embed → the build
    // baked the container bytes into the asset registry (keyed by the logical path); otherwise the file
    // ships beside the binary and is read from assetRoot(). PCM's per-type default is LoadFromPath.
    std::vector<AudioFrame> decodePcmEntry(const AudioLibrary::Entry& entry) {
        // Decode through the hook the audio-file registration installs, never by naming the decoder
        // directly — that is what keeps the decoder out of binaries that register no audio file. A Pcm entry
        // only exists because that registration ran, so the hook is always set here; the guard is defensive.
        if (detail::g_pcmDecode == nullptr) {
            throw std::runtime_error(
                "AudioSystem::play: PCM decoder is not linked — no audio file was registered");
        }
        if (resolveAssetPolicy(entry.policy, AssetPolicy::LoadFromPath) == AssetPolicy::Embed) {
            if (const std::span<const std::uint8_t> baked = detail::findEmbeddedAsset(entry.asmPath);
                !baked.empty()) {
                return detail::g_pcmDecode(baked, sampleRate);
            }
        }
        const std::filesystem::path full = assetRoot() / std::filesystem::path(entry.asmPath);
        std::ifstream in{full, std::ios::binary};
        if (!in) {
            throw std::runtime_error("AudioSystem::play: cannot open audio file: " + full.string());
        }
        std::ostringstream ss;
        ss << in.rdbuf();
        const std::string contents = ss.str();
        const std::vector<std::uint8_t> bytes(contents.begin(), contents.end());
        return detail::g_pcmDecode(bytes, sampleRate);
    }

    // Enter the release fade: the voice's remaining output rides a short linear ramp to zero
    // (rampFrame) and the voice is removed when it lands. Idempotent — a voice already releasing keeps
    // its ramp position (a second stop()/Retrigger does not restart the fade).
    void beginRelease(Voice& v) const {
        if (v.releasing) {
            return;
        }
        v.releasing     = true;
        v.rampTotal     = releaseFrames;
        v.rampRemaining = releaseFrames;
    }

    // Whether any voice is riding its release fade — production continues past the latency target for
    // these (the tail is a few hundred frames; the ring's capacity, several times the target, absorbs
    // it), so stop() reaches silence even when nothing is draining the ring.
    [[nodiscard]] bool anyReleasing() const {
        for (const auto& v : voices) {
            if (v->releasing) {
                return true;
            }
        }
        return false;
    }

    // ── The mixdown ────────────────────────────────────────────────────────────────────────────────────
    // Consume the minimum frame count available across every chiptune voice's lane, sum position-by-
    // position (saturating), and push the mixed frames to the ring. The remainder of a longer lane simply
    // waits for the next mix — simultaneous VMs may emit ±1 frame per step, and consuming at the minimum
    // keeps every voice's stream sample-exact (no padding, no dropping). A ring-full push drops that
    // mixed frame and counts it — production never blocks on a full ring. With one voice the sum is the
    // identity, so a lone sound's bytes reach the ring exactly as its APU produced them.
    void mixLanes() {
        if (voices.empty()) {
            return;
        }
        std::size_t n = SIZE_MAX;
        for (const auto& v : voices) {
            n = std::min(n, v->lane.size() - v->laneHead);
        }
        for (std::size_t i = 0; i < n; ++i) {
            mixScratch.clear();
            for (const auto& v : voices) {
                const AudioFrame f = v->lane[v->laneHead + i];
                mixScratch.push_back(
                    v->releasing
                        ? detail::rampFrame(f, v->rampRemaining > i ? v->rampRemaining - i : 0,
                                            v->rampTotal)
                        : f);
            }
            if (!ring.push(detail::mixFrames(mixScratch))) {
                framesDropped.fetch_add(1, std::memory_order_relaxed);
            }
        }
        for (const auto& v : voices) {
            v->laneHead += n;
            if (v->laneHead == v->lane.size()) {
                v->lane.clear();
                v->laneHead = 0;
            }
            if (v->releasing) {
                v->rampRemaining = v->rampRemaining > n ? v->rampRemaining - n : 0;
            }
        }
    }

    // One produce pass: top the ring back up to its latency target, stepping EVERY voice together and
    // mixing after each step, then close the voices that finished (a one-shot SFX gone silent; a PCM
    // buffer exhausted). `playing` tracks whether any voice remains.
    void produceOnce() {
        if (kind_ == AudioKind::Pcm) {
            producePcmMix();
            return;
        }
        for (int n = 0; n < maxStepsPerWake && !voices.empty() &&
                        (ring.sizeApprox() < targetFrames || anyReleasing());
             ++n) {
            for (const auto& v : voices) {
                v->vm->stepDriver(cyclesPerFrame);  // the APU lanes ~one frame's worth of samples
            }
            mixLanes();
        }
        // Auto-close: a finished one-shot SFX voice (output exact-zero past the threshold) is removed —
        // its VM is torn down with it. Music/Vocals voices are never auto-closed; the other voices play
        // on untouched.
        std::erase_if(voices, [this](const std::unique_ptr<Voice>& v) {
            return (v->releasing && v->rampRemaining == 0) ||
                   detail::shouldAutoStop(v->silenceRun, autoStopSilenceFrames, v->type);
        });
        playing.store(!voices.empty(), std::memory_order_relaxed);
    }

    // One PCM produce pass: stream every voice's decoded frames forward together, summing at each
    // position, up to the latency target. A voice whose buffer is exhausted contributes nothing and is
    // removed after the pass; the others play on. A ring-full push consumes nothing (all cursors hold),
    // so the next pass resumes exactly where this one stopped. At unity gain with a single voice the
    // streamed frames are the decoded bytes unchanged.
    void producePcmMix() {
        while (!voices.empty() && (ring.sizeApprox() < targetFrames || anyReleasing())) {
            // A voice at its buffer's end enters release: the tail decays from the final frame, so a
            // file whose last sample sits off zero still lands at silence instead of clicking.
            for (const auto& v : voices) {
                if (!v->releasing && v->cursor >= v->pcm->size()) {
                    v->lastFrame = v->pcm->empty() ? AudioFrame{} : v->pcm->back();
                    beginRelease(*v);
                }
            }
            mixScratch.clear();
            for (const auto& v : voices) {
                if (v->releasing && v->rampRemaining == 0) {
                    continue;  // tail finished — removed below
                }
                const AudioFrame src =
                    v->cursor < v->pcm->size() ? (*v->pcm)[v->cursor] : v->lastFrame;
                const std::uint32_t gain = AudioMixer::instance().effectiveGain(v->type);
                AudioFrame f{applyGain(src.left, gain), applyGain(src.right, gain)};
                if (v->releasing) {
                    f = detail::rampFrame(f, v->rampRemaining, v->rampTotal);
                }
                mixScratch.push_back(f);
            }
            if (mixScratch.empty()) {
                break;  // every voice done — the erase below closes them
            }
            if (!ring.push(detail::mixFrames(mixScratch))) {
                break;  // ring full — nothing consumed; resume here next pass
            }
            for (const auto& v : voices) {
                if (v->cursor < v->pcm->size()) {
                    ++v->cursor;
                }
                if (v->releasing && v->rampRemaining > 0) {
                    --v->rampRemaining;
                }
            }
        }
        std::erase_if(voices, [](const std::unique_ptr<Voice>& v) {
            return v->releasing && v->rampRemaining == 0;
        });
        playing.store(!voices.empty(), std::memory_order_relaxed);
    }

    // ── Production thread ────────────────────────────────────────────────────────────────────────────
    // Start the self-pacing production thread (the threaded ctors call this after construction; the
    // manual test ctor does not, leaving `threaded` false). From here the voices are touched ONLY by this
    // thread — applyPlay/produceOnce/drainCues never run elsewhere.
    void startProductionThread() {
        threaded = true;
        running.store(true, std::memory_order_relaxed);
        productionThread = std::thread([this] { productionLoop(); });
    }

    // Signal the loop to exit, wake it, and join — a no-op in manual mode (no thread was started). MUST
    // run before the Impl members (voices/ring/cueQueue) destruct, since the thread is their only producer.
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

AudioSystem::AudioSystem(AudioKind kind, AudioSink& sink, VMPlatform platform, TimingProfile timing,
                         unsigned sampleRate)
    : impl_(std::make_unique<Impl>(kind, sink, platform, timing, sampleRate)) {
    impl_->startProductionThread();
}

AudioSystem::AudioSystem(AudioKind kind, std::unique_ptr<AudioSink> sink, VMPlatform platform,
                         TimingProfile timing, unsigned sampleRate)
    : impl_(std::make_unique<Impl>(kind, std::move(sink), platform, timing, sampleRate)) {
    impl_->startProductionThread();
}

// The zero-boilerplate default: own an internally-constructed production sink. Delegates to the owning
// ctor with a fresh SdlAudioSink — adds only an include, no new library dependency (sdl_platform.cpp is
// already in this static lib). A non-SDL audio backend uses the injection seam (the two ctors above).
AudioSystem::AudioSystem(AudioKind kind, VMPlatform platform, TimingProfile timing, unsigned sampleRate)
    : AudioSystem(kind, std::make_unique<SdlAudioSink>(), platform, timing, sampleRate) {}

// Manual (thread-suppressed) construction for the internal test seam. Borrows `sink`; leaves `threaded`
// false so play()/stop() apply inline and the test drives production via AudioSystemTestAccess.
AudioSystem::AudioSystem(ManualTag, AudioKind kind, AudioSink& sink, VMPlatform platform,
                         TimingProfile timing, unsigned sampleRate)
    : impl_(std::make_unique<Impl>(kind, sink, platform, timing, sampleRate)) {}

AudioSystem::~AudioSystem() {
    // Stop the sink first so its audio thread stops pulling the ring, THEN join the production thread so
    // it stops pushing the ring and touching the voices, THEN the Impl (cue queue + ring + voices + each
    // voice's VM) is destroyed — no produced or pulled frame touches a dead ring or VM.
    impl_->sink.stop();
    impl_->stopProductionThread();
}

void AudioSystem::play(AudioId id, CueMode mode) {
    impl_->cueQueue.push(audio::AudioCommand{audio::AudioCommand::Op::Play, id, mode});
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

std::unique_ptr<AudioSystem> AudioSystemTestAccess::makeManual(AudioKind kind, AudioSink& sink,
                                                               VMPlatform platform, TimingProfile timing,
                                                               unsigned sampleRate) {
    return std::unique_ptr<AudioSystem>(
        new AudioSystem(AudioSystem::ManualTag{}, kind, sink, platform, timing, sampleRate));
}

void AudioSystemTestAccess::step(AudioSystem& sys) {
    sys.impl_->drainCues();
    if (sys.impl_->playing.load(std::memory_order_relaxed)) {
        sys.impl_->produceOnce();
    }
}

std::uint64_t AudioSystemTestAccess::stepDriverRaw(AudioSystem& sys, std::uint64_t cycles) {
    sys.impl_->drainCues();
    // Chiptune-only seam (the golden gate drives a chiptune driver); a Pcm system has no VM to step.
    // Steps the FIRST voice's VM by exactly `cycles`, then mixes its laned samples into the ring — with
    // one voice (the golden gate's shape) the mix is the identity, so the captured PCM is the driver's
    // bytes unchanged.
    if (sys.impl_->playing.load(std::memory_order_relaxed) && !sys.impl_->voices.empty() &&
        sys.impl_->voices.front()->vm.has_value()) {
        const std::uint64_t ran = sys.impl_->voices.front()->vm->stepDriver(cycles);
        sys.impl_->mixLanes();
        return ran;
    }
    return 0;
}

}  // namespace detail

}  // namespace retropp
