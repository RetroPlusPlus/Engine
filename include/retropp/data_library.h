#pragma once

// The DataLibrary: the one place a project's registered DATA lives — arbitrary bytes the platform stores,
// hands back by id, and never interprets.
//
// This is the third ingestible family, beside graphics and audio, and it is the one the platform has no
// opinion about. An atlas ends up GPU-resident because the platform owns pixel interpretation; audio ends
// up in the audio library because the platform owns decode and streaming. A data asset ends up as bytes:
// a text corpus, a character table, a stat block, a script — whatever the game means by them. The platform
// delivers them and stops there.
//
// SINGLE INSTANCE BY CONSTRUCTION. There is exactly one DataLibrary per program, reached through
// DataLibrary::instance(). The constructor is private and copying is deleted, so a second one cannot be
// declared — "more than one library" is a compile error, not a runtime check.
//
// OPTIONAL + LEAN. instance() is a function-local static: it is materialized only when first referenced,
// so a program that registers no data never links it in.
//
// Two ways to register, each returning a DataId:
//   * uploadData   — pass bytes you already have; the library copies and owns them. No policy (you
//                    brought the bytes), so nothing is baked or copied by the build.
//   * registerData — pass a compile-time literal logical path; the Embed / LoadFromPath policy decides
//                    whether the build bakes the file's bytes into the binary or ships the file beside
//                    it. Unset resolves to LoadFromPath (see below).
//
// THE PER-TYPE DEFAULT IS LoadFromPath, and it is a legal posture, not a performance one. Data is the
// family a game is most likely to derive from content it has no right to redistribute — a corpus
// extracted from a player's own ROM is the motivating case — and baking such a file into a shipped
// binary is the one outcome that must never happen by omission. A registration that names no policy
// therefore ships the file beside the binary and reads it at runtime; Embed on this family is only ever
// the explicit per-call AssetPolicy::Embed token at the call site.
//
// LIFETIME. data() resolves an entry on first call and caches it: the bytes are held for the life of the
// program and every later call returns the same span. There is no eviction and no refresh — at retro-data
// scale a corpus is kilobytes, and a stable span is what lets a consumer keep a decoded view over it.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "retropp/asset_policy.h"  // AssetPolicy — a path registration carries its per-call policy
#include "retropp/literal_path.h"  // LiteralPath — registerData takes a compile-time literal path

namespace retropp {

// An opaque handle to a registered data asset, minted by the DataLibrary and resolved with data().
// Its lifetime is the library's (the whole program) — the AtlasId / AudioId value-handle contract.
enum class DataId : std::uint32_t {};

class DataLibrary {
public:
    // The one library. A function-local static: constructed on first use, destroyed at program exit,
    // thread-safe initialization.
    static DataLibrary& instance();

    DataLibrary(const DataLibrary&)            = delete;
    DataLibrary& operator=(const DataLibrary&) = delete;

    // What the library stores per DataId. EXACTLY ONE source populates an entry: `path` (registerData)
    // or `owned` holding the caller's copied bytes (uploadData). `resolved` is what data() returns once
    // the entry has been resolved — a span into `owned`, or into the array the build baked.
    struct Entry {
        std::optional<AssetPolicy>    policy{};    // path entries: per-call policy (nullopt = default)
        std::string                   path{};      // path entries: the logical path (empty for uploadData)
        std::vector<std::uint8_t>     owned{};     // bytes the library owns: the upload copy, or a disk read
        std::span<const std::uint8_t> resolved{};  // what data() returns; meaningful once `ready`
        bool                          ready = false;  // resolved has been computed (an empty file is ready)
    };

    // Register `bytes` from memory: copy them into the library's own storage and return a handle. The
    // bytes are owned by the library from here on, so the span need not outlive the call. No embed/load
    // policy — you brought the bytes, so the build has nothing to bake or copy.
    DataId uploadData(std::span<const std::uint8_t> bytes);

    // Register a data file by its logical (project-root-relative) literal path, with its per-call
    // embed/load `policy`; returns a handle. Nothing is read here — the file is resolved on the first
    // data() call, because registration normally runs before EngineConfig::setActive() has resolved the
    // asset root. An unset policy resolves to LoadFromPath.
    DataId registerData(LiteralPath resourcePath, std::optional<AssetPolicy> policy = {});

    // The bytes for `id`. Resolves the entry on first call and caches the result; every later call
    // returns the same span.
    //
    // Embed reads the array the build baked for this path. LoadFromPath reads the file at
    // assetRoot() / path. An Embed path the build baked nothing for logs one warning naming it and falls
    // back to the disk read — the same fallback every other family has, and the same diagnosis.
    //
    // Throws std::out_of_range when `id` was never minted by this library, and std::runtime_error naming
    // the path when a file cannot be read. A missing file is a real failure a game needs told about: an
    // empty span would be indistinguishable from a file that is legitimately empty.
    [[nodiscard]] std::span<const std::uint8_t> data(DataId id);

    // How many data assets are registered — also the next DataId to be minted. Ids are dense and
    // ascending from 0, and the library accumulates for the life of the program.
    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

private:
    DataLibrary() = default;

    std::vector<Entry> entries_;
};

}  // namespace retropp
