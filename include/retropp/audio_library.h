#pragma once

// The AudioLibrary: the one place a project's registered audio lives.
//
// SINGLE INSTANCE BY CONSTRUCTION. There is exactly one AudioLibrary per program, reached through
// AudioLibrary::instance(). The constructor is private and copying is deleted, so a second one cannot be
// declared — "more than one library" is a compile error, not a runtime check (the Swift-singleton shape).
//
// OPTIONAL + LEAN. instance() is a function-local static: it is materialized only when first referenced,
// so a project that registers no audio at all — a pure-render demoscene / digital-art program with no
// AudioSystem — never links it in. You pay for the catalog only if you use it.
//
// ITS OWN STORAGE — no VM, no Routine, no machine type. The library holds portable audio DEFINITIONS (the
// driver bytecode for a chiptune, or the .asm path to assemble; a PCM track streams from its path by
// default — a future arm) plus each one's tags, and hands out an AudioId per registration. How a
// definition becomes sound is the
// AudioSystem's job — it places the definition into the VM it owns, on play — so the library never touches
// a VM, which also keeps it from force-linking one. This header therefore pulls no vm.h.
//
// TWO DOORS, identical effect (the uploadAtlas / loadAtlas precedent), mirrored on AudioSystem:
//   * upload*   — you hand over READY BYTES (pre-assembled chiptune bytecode; PCM samples later). No
//                 policy: you brought the bytes.
//   * register* — you hand over a path; the Embed / LoadFromPath policy decides whether the build bakes
//                 the bytes or ships the file beside the binary.
// The chiptune-vs-PCM KIND is inferred (element type for bytes, extension for a path) and frozen into the
// entry; the developer-facing call is identical for both. PCM is a tagged seam here — no PCM code yet.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "retropp/asset_policy.h"   // AssetPolicy (a register* path entry carries its per-call policy)
#include "retropp/isa.h"            // Isa (the developer selects the ISA a chiptune is written for, here)
#include "retropp/literal_path.h"   // LiteralPath (the register* door takes a compile-time literal path)

namespace retropp {

// How a registered audio is used — the Music / Sfx routing tag. Stored on every entry; with a single
// instance, routing is the original hardware's natural channel-stealing.
enum class AudioType { Music, Sfx };

// WHAT a registered audio is, inferred once at registration and frozen into its entry: a chiptune driver
// (the VM / driver path that exists today) or PCM (the sample-mixer arm — a tagged seam now, no PCM code
// yet). PCM streams from a path by DEFAULT — decoded samples never sit resident in RAM, and the bytes are
// never baked into the binary; embedding PCM is an opt-in override, never automatic. (Chiptune defaults
// the other way — Embed — only because a driver is hundreds of bytes, not a multi-MB track.) Orthogonal
// to AudioType (kind = what it is; type = how it routes).
enum class AudioKind { Chiptune, Pcm };

// An opaque handle to a registered audio, minted by the AudioLibrary and cued with AudioSystem::play().
// Its lifetime is the library's (the whole program) — the AtlasId / PaletteId value-handle contract.
enum class AudioId : std::uint32_t {};

namespace detail {
// Case-sensitive suffix match (logical asset paths are authored lowercase). constexpr so the kind
// inference below — and the compile-time door check on ChiptunePath — run the same code.
[[nodiscard]] constexpr bool endsWith(std::string_view s, std::string_view suffix) noexcept {
    return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}
}  // namespace detail

// Infer the audio KIND from a path's extension: `.asm` (ISA assembly source) is a Chiptune; an audio
// container (`.wav` / `.ogg` / `.flac` / `.mp3`) is PCM. Anything else is taken as Chiptune (the only
// realized kind in v1 — PCM is a tagged seam). Used by the SUGAR (path) door; the RAW (bytes) door is
// always a chiptune (PCM raw arrives as AudioFrame samples — a future overload). constexpr: the one
// predicate drives both the runtime inference and the compile-time door check.
[[nodiscard]] constexpr AudioKind audioKindForExtension(std::string_view path) noexcept {
    if (detail::endsWith(path, ".wav") || detail::endsWith(path, ".ogg") ||
        detail::endsWith(path, ".flac") || detail::endsWith(path, ".mp3")) {
        return AudioKind::Pcm;
    }
    return AudioKind::Chiptune;  // `.asm` and anything else — the only realized kind in v1
}

// A compile-time literal path for the ISA (chiptune) registration door. Its consteval constructor rejects
// any literal that resolves to PCM — a `.wav` / `.ogg` / `.mp3` / `.flac` audio file plays without the VM
// and has no ISA, so handing one to the ISA door is a category error. Reaching the throw during the
// required constant evaluation is ill-formed, so such a literal does not compile; a non-PCM literal (`.asm`
// and anything else) constructs normally and keeps the natural `f("song.asm")` call syntax. The wrong door
// is impossible, not merely diagnosed at runtime. (Built on LiteralPath, so the build-time asset scan still
// sees the literal text verbatim.)
class ChiptunePath {
public:
    template <std::size_t N>
    consteval ChiptunePath(const char (&literal)[N])  // NOLINT(google-explicit-constructor)
        : path_(literal) {
        if (audioKindForExtension(path_.view()) == AudioKind::Pcm) {
            throw "registerAudio with an Isa is the chiptune door; a .wav/.ogg/.mp3/.flac audio file must "
                  "use the no-Isa registerAudio overload";
        }
    }

    [[nodiscard]] constexpr std::string_view view() const noexcept { return path_.view(); }

private:
    LiteralPath path_;
};

class AudioLibrary {
public:
    // The one library — see the header note (single by construction, lean, optional). A function-local
    // static; returns the same object on every call.
    static AudioLibrary& instance();

    AudioLibrary(const AudioLibrary&)            = delete;
    AudioLibrary& operator=(const AudioLibrary&) = delete;

    // What the library stores per AudioId: the kind / type / ISA tags, the per-call embed/load policy
    // (path entries), and the portable chiptune definition — EXACTLY ONE of `bytecode` (the raw / Embed
    // form, owned by the library) or `asmPath` (the path / LoadFromPath form) is populated for a Chiptune
    // entry. `isa` is the ISA a Chiptune's bytes/source are written for, SELECTED BY THE DEVELOPER at
    // registration (ignored for PCM): AudioSystem::play() rejects an AudioId cued on a VM of a different
    // ISA. The AudioSystem reads this to place the driver into the VM it owns; a developer never inspects it.
    struct Entry {
        AudioKind                  kind;
        AudioType                  type;
        Isa                        isa;       // the ISA a Chiptune is written for (ignored for PCM)
        std::optional<AssetPolicy> policy;    // path entries: per-call embed/load policy (nullopt = default)
        std::vector<std::uint8_t>  bytecode;  // chiptune ready bytes (upload door / baked Embed) — owned
        std::string                asmPath;   // chiptune source path (register door / LoadFromPath)
    };

    // RAW door: copy pre-assembled driver `bytecode` (written for ISA `isa`) into the library's own
    // storage, tag it, and mint a handle. The bytes are owned by the library from here on (the span need
    // not outlive the call). No embed/load policy — you brought the bytes.
    AudioId uploadAudio(std::span<const std::uint8_t> bytecode, AudioType type, Isa isa);

    // SUGAR (path) door — CHIPTUNE form: record a logical `.asm` path to assemble later (on play) in the
    // ISA `isa` it targets, with its per-call embed/load `policy`, and mint a handle. ISA is a
    // chiptune-only concept — `isa` is the developer-selected compatibility unit that gates the chiptune
    // at play() (a cue on a VM of a different ISA throws). Only this chiptune door takes an ISA. The path
    // is a ChiptunePath: a PCM-extension literal (a `.wav` / `.ogg` / `.mp3` / `.flac` audio file, which
    // runs without the VM and has no ISA) does not compile here — use the no-ISA overload below for those.
    //
    // Templated on the ISA argument purely to GATE candidacy: a no-ISA call (the PCM door below) deduces a
    // non-Isa third argument, fails the constraint, and is removed from the candidate set BEFORE the
    // ChiptunePath conversion is ever formed — so a legitimate `.wav` / `.ogg` registration resolves
    // cleanly to the PCM door rather than hard-erroring on this overload's compile-time path check. With an
    // actual Isa, this is the only viable match.
    template <class IsaT>
        requires std::is_same_v<IsaT, Isa>
    AudioId registerAudio(ChiptunePath resourcePath, AudioType type, IsaT isa,
                          std::optional<AssetPolicy> policy = {}) {
        entries_.push_back(Entry{
            .kind     = AudioKind::Chiptune,  // ChiptunePath rejected any PCM extension at compile time
            .type     = type,
            .isa      = isa,
            .policy   = policy,
            .bytecode = {},
            .asmPath  = std::string(resourcePath.view()),
        });
        return static_cast<AudioId>(entries_.size() - 1);
    }

    // SUGAR (path) door — PCM (audio-file) form: record a `.wav` / `.ogg` path to decode + stream later
    // (on play), with its per-call embed/load `policy`, and mint a handle. NO ISA — a decoded audio file
    // runs no code, so ISA (a chiptune-only concept) is meaningless here. The KIND is inferred from the
    // extension as Pcm; the entry plays on an AudioKind::Pcm system.
    AudioId registerAudio(LiteralPath resourcePath, AudioType type,
                          std::optional<AssetPolicy> policy = {});

    // How many audio resources are registered — also the next AudioId to be minted. (The library
    // accumulates for the life of the program; ids are dense and ascending from 0.)
    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

    // The stored definition for `id`. Precondition: static_cast<std::size_t>(id) < size().
    [[nodiscard]] const Entry& entry(AudioId id) const;

private:
    AudioLibrary() = default;
    std::vector<Entry> entries_;
};

}  // namespace retropp
