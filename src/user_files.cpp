#include "retropp/user_files.h"

#include <atomic>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string>

#include "durable_file.h"  // detail::writeFileDurably / atomicReplace / syncDirectoryEntry

namespace retropp {

namespace {

// A relative path may descend, but never escape. Rejecting an absolute path, a drive designator and any
// ".." component is what bounds the result to the store's directory; a "." component cannot escape but is
// refused as well, so the path a caller passes is the path they get back from pathFor().
std::filesystem::path validateRelative(std::string_view relativePath) {
    const std::filesystem::path rel{std::string(relativePath)};
    // A root-DIRECTORY is refused on its own, not just via is_absolute(): on Windows "/etc/passwd" is not
    // an absolute path (that needs a drive too), but appending it to the store's directory drops the
    // store's own root and yields "C:/etc/passwd". The three tests together — root name, root directory,
    // is_absolute — are what make containment mean the same thing on every platform.
    bool bad = relativePath.empty() || rel.is_absolute() || rel.has_root_name() ||
               rel.has_root_directory();
    // ANY "." or ".." component is refused, wherever it sits — a check that stopped at the first clean
    // component would forgive "assets/../../outside.bin", whose every other part looks ordinary.
    for (const std::filesystem::path& part : rel) {
        if (part == ".." || part == ".") bad = true;
    }
    // An empty trailing component ("assets/") names a directory, not a file.
    if (rel.filename().empty()) bad = true;
    if (bad) {
        throw std::invalid_argument(
            "UserFiles: a path must be relative and stay inside the store (no leading '/', no drive "
            "designator, no '.' or '..' component, and it must name a file): \"" +
            std::string(relativePath) + "\"");
    }
    return rel;
}

}  // namespace

UserFiles::UserFiles() : base_(userDataDir()) {}

UserFiles UserFiles::atPath(std::filesystem::path base) { return UserFiles(std::move(base)); }

std::filesystem::path UserFiles::pathFor(std::string_view relativePath) const {
    return base_ / validateRelative(relativePath);
}

bool UserFiles::write(std::string_view relativePath, std::span<const std::byte> bytes) {
    const std::filesystem::path target = pathFor(relativePath);
    const std::filesystem::path dir    = target.parent_path();

    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) return false;

    // A process-unique sibling temp name: the rename below is only atomic within one filesystem, so the
    // temp lives in the target's own directory.
    static std::atomic<unsigned> counter{0};
    const std::filesystem::path temp =
        dir / (target.filename().string() + "." + std::to_string(counter.fetch_add(1)) + ".tmp");

    // No envelope — the file on disk is exactly the caller's bytes, so the header span is empty.
    if (!detail::writeFileDurably(temp, {}, bytes)) {
        std::filesystem::remove(temp, ec);
        return false;
    }
    if (!detail::atomicReplace(temp, target)) {
        std::filesystem::remove(temp, ec);
        return false;
    }
    detail::syncDirectoryEntry(dir);
    return true;
}

std::optional<std::vector<std::byte>> UserFiles::read(std::string_view relativePath) const {
    const std::filesystem::path file = pathFor(relativePath);

    std::error_code ec;
    if (!std::filesystem::is_regular_file(file, ec)) return std::nullopt;

    std::ifstream in{file, std::ios::binary};
    if (!in) return std::nullopt;

    const std::uintmax_t size = std::filesystem::file_size(file, ec);
    if (ec) return std::nullopt;

    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    if (size > 0 && !in.read(reinterpret_cast<char*>(bytes.data()),
                             static_cast<std::streamsize>(size))) {
        return std::nullopt;
    }
    return bytes;
}

bool UserFiles::exists(std::string_view relativePath) const {
    std::error_code ec;
    return std::filesystem::is_regular_file(pathFor(relativePath), ec);
}

bool UserFiles::remove(std::string_view relativePath) {
    std::error_code ec;
    return std::filesystem::remove(pathFor(relativePath), ec);
}

}  // namespace retropp
