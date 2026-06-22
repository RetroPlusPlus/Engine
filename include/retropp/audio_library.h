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

// Infer the audio KIND from a path's extension: `.asm` (ISA assembly source) is a Chiptune; an audio
// container (`.wav` / `.ogg` / `.flac` / `.mp3`) is PCM. Anything else is taken as Chiptune (the only
// realized kind in v1 — PCM is a tagged seam). Used by the SUGAR (path) door; the RAW (bytes) door is
// always a chiptune (PCM raw arrives as AudioFrame samples — a future overload).
[[nodiscard]] AudioKind audioKindForExtension(std::string_view path) noexcept;

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

    // SUGAR (path) door: record a logical resource path to materialize later (on play), tag it with the
    // KIND inferred from its extension + the ISA it targets + its per-call embed/load `policy`, and mint
    // a handle. A `.asm` path is a Chiptune (assembled in `isa` on play); a PCM container is tagged Pcm
    // (a seam — no PCM code yet). `isa` is selected by the developer; it gates a Chiptune at play().
    AudioId registerAudio(LiteralPath resourcePath, AudioType type, Isa isa,
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
