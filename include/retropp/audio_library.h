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
// driver bytecode for a chiptune, or the .asm path to assemble; or a PCM audio-file path the PCM backend
// decodes and streams) plus each one's tags, and hands out an AudioId per registration. How a
// definition becomes sound is the
// AudioSystem's job — it places the definition into the VM it owns, on play — so the library never touches
// a VM, which also keeps it from force-linking one. This header therefore pulls no vm.h.
//
// Two ways to register, each returning an AudioId:
//   * uploadAudio   — pass already-assembled chiptune bytecode; the library copies and owns it. No policy
//                     (you brought the bytes). Always a chiptune.
//   * registerAudio — pass a file path; the Embed / LoadFromPath policy decides whether the build bakes the
//                     bytes into the binary or ships the file beside it. The kind is inferred from the
//                     extension: `.asm` is a chiptune, `.wav` / `.ogg` / `.flac` / `.mp3` is PCM.
// The kind is frozen into the entry at registration.

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
#include "retropp/literal_path.h"   // LiteralPath (registerAudio takes a compile-time literal path)

namespace retropp {

// How a registered audio is used — the Music / Sfx routing tag. Stored on every entry; with a single
// instance, routing is the original hardware's natural channel-stealing.
enum class AudioType { Music, Sfx };

// WHAT a registered audio is, inferred once at registration and frozen into its entry: a chiptune driver
// (runs on the VM) or PCM (a WAV/OGG audio file the PCM backend decodes and streams; no VM). PCM defaults
// to LoadFromPath — the file ships beside the binary rather than being baked in; embedding PCM is an
// opt-in override (a multi-MB track is not baked automatically). Chiptune defaults the other way — Embed —
// because a driver is hundreds of bytes. Orthogonal to AudioType (kind = what it is; type = how it routes).
enum class AudioKind { Chiptune, Pcm };

// An opaque handle to a registered audio, minted by the AudioLibrary and cued with AudioSystem::play().
// Its lifetime is the library's (the whole program) — the AtlasId / PaletteId value-handle contract.
enum class AudioId : std::uint32_t {};

namespace detail {
// Case-sensitive suffix match (logical asset paths are authored lowercase). constexpr so the kind
// inference below — and the compile-time check on ChiptunePath — run the same code.
[[nodiscard]] constexpr bool endsWith(std::string_view s, std::string_view suffix) noexcept {
    return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}
}  // namespace detail

// Infer the audio KIND from a path's extension: `.asm` (ISA assembly source) is a Chiptune; an audio
// container (`.wav` / `.ogg` / `.flac` / `.mp3`) is PCM. Anything else is taken as Chiptune. Used when
// registering by path; uploadAudio (bytecode) is always a chiptune. constexpr: the one predicate drives
// both the runtime inference and the compile-time ChiptunePath check.
[[nodiscard]] constexpr AudioKind audioKindForExtension(std::string_view path) noexcept {
    if (detail::endsWith(path, ".wav") || detail::endsWith(path, ".ogg") ||
        detail::endsWith(path, ".flac") || detail::endsWith(path, ".mp3")) {
        return AudioKind::Pcm;
    }
    return AudioKind::Chiptune;  // `.asm` and anything else
}

// A compile-time literal path for the ISA (chiptune) registerAudio overload. Its consteval constructor
// rejects any literal that resolves to PCM — a `.wav` / `.ogg` / `.mp3` / `.flac` audio file plays without
// the VM and has no ISA, so passing one to the ISA overload is a category error. Reaching the throw during
// the required constant evaluation is ill-formed, so such a literal does not compile; a non-PCM literal
// (`.asm` and anything else) constructs normally and keeps the natural `f("song.asm")` call syntax — the
// wrong overload is a compile error, not a runtime one. (Built on LiteralPath, so the build-time asset
// scan still sees the literal text verbatim.)
class ChiptunePath {
public:
    template <std::size_t N>
    consteval ChiptunePath(const char (&literal)[N])  // NOLINT(google-explicit-constructor)
        : path_(literal) {
        if (audioKindForExtension(path_.view()) == AudioKind::Pcm) {
            throw "registerAudio with an Isa registers a chiptune; a .wav/.ogg/.mp3/.flac audio file must "
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
        std::vector<std::uint8_t>  bytecode;  // chiptune ready bytes (uploadAudio / baked Embed) — owned
        std::string                asmPath;   // chiptune source path (registerAudio / LoadFromPath)
    };

    // Register a chiptune from pre-assembled `bytecode` (written for ISA `isa`): copy it into the library's
    // own storage, tag it, and return a handle. The bytes are owned by the library from here on (the span
    // need not outlive the call). No embed/load policy — you brought the bytes.
    AudioId uploadAudio(std::span<const std::uint8_t> bytecode, AudioType type, Isa isa);

    // Register a chiptune from a `.asm` path, assembled on first play for the ISA `isa` it targets, with
    // its per-call embed/load `policy`; returns a handle. ISA is a chiptune-only concept — `isa` is the
    // developer-selected compatibility unit that gates the chiptune at play() (a cue on a VM of a different
    // ISA throws). Only chiptune registration takes an ISA. The path is a ChiptunePath: a PCM-extension
    // literal (`.wav` / `.ogg` / `.mp3` / `.flac` — an audio file that runs without the VM and has no ISA)
    // does not compile here; use the no-ISA overload below for those.
    //
    // Templated on the ISA argument purely to gate candidacy: a no-ISA call deduces a non-Isa third
    // argument, fails the constraint, and is dropped from the candidate set BEFORE the ChiptunePath
    // conversion is formed — so a legitimate `.wav` / `.ogg` registration resolves to the no-ISA overload
    // instead of failing this one's compile-time path check. With an actual Isa, this is the only match.
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

    // Register a PCM audio file (`.wav` / `.ogg` / `.flac` / `.mp3`) from a path, decoded and streamed on
    // first play, with its per-call embed/load `policy`; returns a handle. No ISA — a decoded audio file
    // runs no code, so ISA (a chiptune-only concept) is meaningless here. The kind is inferred from the
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
