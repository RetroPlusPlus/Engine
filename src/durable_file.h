#pragma once

#include <filesystem>
#include <span>

// Writing a file so that a crash cannot leave a half-written one behind. Engine-internal: the two
// file-backed stores (SaveStore, UserFiles) both commit through these, so there is one durability
// implementation rather than a copy per store.
//
// The sequence a caller runs is write-to-temp, then replace: writeFileDurably puts the whole contents in
// a sibling temp file and forces them to the device, atomicReplace moves that temp over the target in one
// filesystem operation, and syncDirectoryEntry flushes the directory entry the move created. A crash at
// any point leaves either the previous file or the new one, never a partial file — which is why the temp
// must be a SIBLING of the target: a rename is only atomic within one filesystem.

namespace retropp::detail {

// Write `header` followed by `payload` to `file`, then force both to the device — std::ofstream can flush
// its own buffer but cannot ask the OS to flush ITS buffer to disk, and durability needs both. Pass an
// empty header for a file that carries no envelope. Returns false on any failure.
[[nodiscard]] bool writeFileDurably(const std::filesystem::path& file,
                                    std::span<const std::byte> header,
                                    std::span<const std::byte> payload);

// The atomic commit point: replace `target` with `temp` in one filesystem move.
[[nodiscard]] bool atomicReplace(const std::filesystem::path& temp,
                                 const std::filesystem::path& target);

// Flush the directory entry the replace created: a file's bytes are durable once written, but the entry
// pointing at them is its own disk block. Best-effort — by this point the replace has committed, so a
// failure here cannot un-write the file. A no-op where the platform's replace already flushes it.
void syncDirectoryEntry(const std::filesystem::path& dir);

}  // namespace retropp::detail
