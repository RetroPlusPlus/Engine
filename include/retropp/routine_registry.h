#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "retropp/asset_policy.h"  // AssetPolicy (reused — a routine's embed/load is the same choice)

// ENG-4.B — routine delivery runtime state, the routine analog of asset_registry.h's EMBED table. Backs
// the SUGAR door (registerRoutine(LiteralPath, …) / AudioLibrary::registerAudio(LiteralPath, …)); the RAW
// door (a byte span) bypasses it entirely.
//
//   Embed (default) — the .asm is assembled to BYTECODE AT COMPILE TIME (the constexpr SM83 assembler)
//                     and ONLY the bytecode ships; the .asm is never stored in the binary nor read at
//                     runtime. The build scan (ENG-4.B Step 4) bakes each Embed routine's bytes and
//                     records them here, keyed by logical path; the SUGAR door looks them up and hands
//                     the span to the RAW path.
//   LoadFromPath    — the .asm ships beside the binary and is read + assembled ONCE at startup, in
//                     memory, never baked — for a copyright-derived routine or a developer's load-by-
//                     choice. There is NO separate routine root: a LoadFromPath path is a full project-
//                     root-relative LITERAL resolved against the engine's single assetRoot() (asset_registry.h),
//                     exactly like loadAtlas / loadMapPng.
//
// LEAN BINARY (locked): pure data — a path → bytes map with NO dependency on Vm or the VM backend, so it
// never force-links the VM. Only routines with a scanned call site are baked and recorded; an unused
// routine is never assembled, baked, or linked in. The engine's own presets use the RAW door directly
// (compile-time-baked arrays odr-used only by their preset fn → dropped if unused) and bypass this.

namespace retropp {

namespace detail {

// The engine-config default routine policy (middle precedence tier; set by EngineConfig::setActive,
// nullopt = unset → the SUGAR door falls through to the per-type default, Embed).
[[nodiscard]] std::optional<AssetPolicy> configDefaultRoutinePolicy() noexcept;
void setConfigDefaultRoutinePolicy(std::optional<AssetPolicy> policy) noexcept;

// Record that a logical routine path resolves to embedded bytecode (a constexpr array valid for the
// program lifetime). Called before main() from the auto-generated per-target registry TU
// (retropp_autoembed, extended to routines at ENG-4.B Step 4) — emitted ONLY for routines with a scanned
// call site, so an unused routine is never recorded here.
void registerEmbeddedRoutine(std::string_view path, const std::uint8_t* bytes, std::size_t size);

// The embedded bytecode registered for `path`, or an empty span if none was baked for it. The SUGAR door
// surfaces an empty span under an Embed policy as a loud error (declared Embed but not baked).
[[nodiscard]] std::span<const std::uint8_t> findEmbeddedRoutine(std::string_view path);

}  // namespace detail
}  // namespace retropp
