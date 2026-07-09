#include "retropp/save_store.h"

#include <SDL3/SDL_filesystem.h>  // SDL_GetPrefPath — the platform per-user data directory
#include <SDL3/SDL_stdinc.h>      // SDL_free

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <fstream>
#include <string>
#include <utility>

namespace retropp {

namespace {

// The on-disk envelope, fixed little-endian so a file written on one platform reads on any
// other: a 4-byte magic tag, the container-format version (the store's own layout version,
// independent of the consumer schema version), the consumer schema version, and the payload
// length, followed by the payload bytes.
constexpr std::array<std::byte, 4> kMagic = {std::byte{'R'}, std::byte{'P'}, std::byte{'S'},
                                             std::byte{'V'}};
constexpr std::uint32_t kContainerVersion = 1;
constexpr std::size_t kHeaderSize = 4 + 4 + 4 + 8;  // magic + container + schema + length

void putU32(std::byte* out, std::uint32_t v) {
    out[0] = std::byte(v & 0xFF);
    out[1] = std::byte((v >> 8) & 0xFF);
    out[2] = std::byte((v >> 16) & 0xFF);
    out[3] = std::byte((v >> 24) & 0xFF);
}

void putU64(std::byte* out, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) out[i] = std::byte((v >> (8 * i)) & 0xFF);
}

std::uint32_t getU32(const std::byte* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

std::uint64_t getU64(const std::byte* p) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(p[i]) << (8 * i);
    return v;
}

// A document name is a flat identifier, never a path: rejecting separators (and the drive
// designator, and the two special directory names) guarantees a document cannot land
// outside the store's directory.
void validateName(std::string_view name) {
    const bool bad = name.empty() || name == "." || name == ".." ||
                     name.find('/') != std::string_view::npos ||
                     name.find('\\') != std::string_view::npos ||
                     name.find(':') != std::string_view::npos;
    if (bad) {
        throw std::invalid_argument("SaveStore: a document name must be a flat identifier "
                                    "(no path separators, drive designators, '.' or '..'): \"" +
                                    std::string(name) + "\"");
    }
}

// Write the temp file's full contents and force them to the device, via OS handles —
// std::ofstream can flush its own buffer but cannot ask the OS to flush ITS buffer to
// disk, and the durability guarantee needs both. Returns false on any failure.
#ifdef _WIN32

bool writeFileDurably(const std::filesystem::path& file, std::span<const std::byte> header,
                      std::span<const std::byte> payload) {
    HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool ok = true;
    for (std::span<const std::byte> part : {header, payload}) {
        std::size_t done = 0;
        while (ok && done < part.size()) {
            const DWORD chunk = static_cast<DWORD>(
                std::min<std::size_t>(part.size() - done, 1u << 30));
            DWORD written = 0;
            ok = WriteFile(h, part.data() + done, chunk, &written, nullptr) && written > 0;
            done += written;
        }
    }
    if (ok) ok = FlushFileBuffers(h) != 0;
    ok = (CloseHandle(h) != 0) && ok;
    return ok;
}

// The atomic commit point: replace the target with the temp in one filesystem move.
bool atomicReplace(const std::filesystem::path& temp, const std::filesystem::path& target) {
    return MoveFileExW(temp.c_str(), target.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

void bestEffortSyncDir(const std::filesystem::path&) {
    // MOVEFILE_WRITE_THROUGH flushes the rename itself; no separate directory sync exists.
}

#else

bool writeFileDurably(const std::filesystem::path& file, std::span<const std::byte> header,
                      std::span<const std::byte> payload) {
    const int fd = ::open(file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;
    bool ok = true;
    for (std::span<const std::byte> part : {header, payload}) {
        std::size_t done = 0;
        while (ok && done < part.size()) {
            const ssize_t n = ::write(fd, part.data() + done, part.size() - done);
            if (n < 0) {
                if (errno == EINTR) continue;
                ok = false;
            } else {
                done += static_cast<std::size_t>(n);
            }
        }
    }
    if (ok) ok = ::fsync(fd) == 0;
    ok = (::close(fd) == 0) && ok;
    return ok;
}

bool atomicReplace(const std::filesystem::path& temp, const std::filesystem::path& target) {
    return ::rename(temp.c_str(), target.c_str()) == 0;
}

// Flush the rename itself: the file's bytes are durable after fsync(fd), but the directory
// entry pointing at them is its own disk block. Best-effort — by this point the rename has
// committed, so a failure here cannot un-write the document.
void bestEffortSyncDir(const std::filesystem::path& dir) {
    const int fd = ::open(dir.c_str(), O_RDONLY);
    if (fd < 0) return;
    ::fsync(fd);
    ::close(fd);
}

#endif

}  // namespace

SaveStore::SaveStore() {
    // No fallback name: a fallback would give every unconfigured game the same directory,
    // and their saves would collide. An unset identity is refused, loudly, on first run.
    const AppIdentity& id = defaultIdentity;
    if (id.organization.empty() || id.application.empty()) {
        throw SaveStoreError(
            "SaveStore: the application identity is not set — assign "
            "config.identity = {\"YourOrg\", \"YourGame\"} and call EngineConfig::setActive "
            "before constructing a SaveStore (or root one explicitly with SaveStore::atPath)");
    }
    const std::string& org = id.organization;
    const std::string& app = id.application;
    char* pref = SDL_GetPrefPath(org.c_str(), app.c_str());
    if (pref == nullptr) {
        throw SaveStoreError("SaveStore: the platform did not supply a save directory for \"" +
                             org + "/" + app + "\"");
    }
    base_ = std::filesystem::path(pref);
    SDL_free(pref);
}

SaveStore SaveStore::atPath(std::filesystem::path base) { return SaveStore(std::move(base)); }

std::filesystem::path SaveStore::documentPath(std::string_view name) const {
    validateName(name);
    return base_ / std::filesystem::path(std::string(name));
}

bool SaveStore::write(std::string_view name, std::uint32_t schemaVersion,
                      std::span<const std::byte> payload) {
    const std::filesystem::path target = documentPath(name);

    std::error_code ec;
    std::filesystem::create_directories(base_, ec);
    if (ec) return false;

    std::array<std::byte, kHeaderSize> header{};
    std::copy(kMagic.begin(), kMagic.end(), header.begin());
    putU32(header.data() + 4, kContainerVersion);
    putU32(header.data() + 8, schemaVersion);
    putU64(header.data() + 12, payload.size());

    // A process-unique sibling temp name: the rename below is only atomic within one
    // filesystem, so the temp lives in the target's own directory.
    static std::atomic<unsigned> counter{0};
    const std::filesystem::path temp =
        base_ / (std::string(name) + "." + std::to_string(counter.fetch_add(1)) + ".tmp");

    if (!writeFileDurably(temp, header, payload)) {
        std::filesystem::remove(temp, ec);
        return false;
    }

    if (preCommitHook_) {
        try {
            preCommitHook_();
        } catch (...) {
            std::filesystem::remove(temp, ec);
            return false;
        }
    }

    if (!atomicReplace(temp, target)) {
        std::filesystem::remove(temp, ec);
        return false;
    }
    bestEffortSyncDir(base_);
    return true;
}

std::optional<SaveStore::Document> SaveStore::read(std::string_view name) const {
    const std::filesystem::path file = documentPath(name);

    std::error_code ec;
    if (!std::filesystem::exists(file, ec)) return std::nullopt;

    std::ifstream in(file, std::ios::binary);
    if (!in) {
        throw SaveStoreError("SaveStore: document \"" + std::string(name) +
                             "\" exists but cannot be opened");
    }
    std::vector<std::byte> bytes(std::filesystem::file_size(file, ec));
    if (ec || !in.read(reinterpret_cast<char*>(bytes.data()),
                       static_cast<std::streamsize>(bytes.size()))) {
        throw SaveStoreError("SaveStore: document \"" + std::string(name) +
                             "\" could not be read");
    }

    if (bytes.size() < kHeaderSize) {
        throw SaveStoreError("SaveStore: document \"" + std::string(name) +
                             "\" is truncated (shorter than the envelope header)");
    }
    if (!std::equal(kMagic.begin(), kMagic.end(), bytes.begin())) {
        throw SaveStoreError("SaveStore: document \"" + std::string(name) +
                             "\" is not a save document (bad magic tag)");
    }
    const std::uint32_t container = getU32(bytes.data() + 4);
    if (container != kContainerVersion) {
        throw SaveStoreError("SaveStore: document \"" + std::string(name) +
                             "\" uses container format " + std::to_string(container) +
                             "; this store reads format " + std::to_string(kContainerVersion));
    }
    const std::uint64_t length = getU64(bytes.data() + 12);
    if (length != bytes.size() - kHeaderSize) {
        throw SaveStoreError("SaveStore: document \"" + std::string(name) +
                             "\" payload length does not match its envelope (corrupt or "
                             "truncated)");
    }

    Document doc;
    doc.schemaVersion = getU32(bytes.data() + 8);
    doc.payload.assign(bytes.begin() + kHeaderSize, bytes.end());
    return migrate(std::move(doc));
}

SaveStore::Document SaveStore::migrate(Document doc) const {
    if (!currentVersion_.has_value()) return doc;
    const std::uint32_t current = *currentVersion_;
    if (doc.schemaVersion > current) {
        throw SaveStoreError("SaveStore: document schema version " +
                             std::to_string(doc.schemaVersion) +
                             " is newer than the declared current version " +
                             std::to_string(current) + " — refusing to read it");
    }
    while (doc.schemaVersion < current) {
        const auto step = migrations_.find(doc.schemaVersion);
        if (step == migrations_.end()) {
            throw SaveStoreError("SaveStore: no migration registered from schema version " +
                                 std::to_string(doc.schemaVersion) +
                                 " (current version is " + std::to_string(current) + ")");
        }
        doc.payload = step->second(std::move(doc.payload));
        ++doc.schemaVersion;
    }
    return doc;
}

void SaveStore::setCurrentVersion(std::uint32_t version) { currentVersion_ = version; }

void SaveStore::registerMigration(std::uint32_t fromVersion, MigrationStep step) {
    migrations_[fromVersion] = std::move(step);
}

bool SaveStore::exists(std::string_view name) const {
    std::error_code ec;
    return std::filesystem::exists(documentPath(name), ec);
}

bool SaveStore::remove(std::string_view name) {
    std::error_code ec;
    return std::filesystem::remove(documentPath(name), ec);
}

}  // namespace retropp
