#include "durable_file.h"

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
#include <cerrno>

namespace retropp::detail {

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

bool atomicReplace(const std::filesystem::path& temp, const std::filesystem::path& target) {
    return MoveFileExW(temp.c_str(), target.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

void syncDirectoryEntry(const std::filesystem::path&) {
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

void syncDirectoryEntry(const std::filesystem::path& dir) {
    const int fd = ::open(dir.c_str(), O_RDONLY);
    if (fd < 0) return;
    ::fsync(fd);
    ::close(fd);
}

#endif

}  // namespace retropp::detail
