// chunk_io.hh — Issue #4936 B3. Fixed read/copy scratch is std::array.
// The chunk size is the old C array extent (4096 read, 8192 copy).
// EINTR is retried. A short read is appended; EOF is success.
#ifndef AURA_COMPILER_CHUNK_IO_HH
#define AURA_COMPILER_CHUNK_IO_HH

#include <array>
#include <cerrno>
#include <cstddef>
#include <string>
#include <unistd.h>

namespace aura::compiler::io {

inline constexpr std::size_t kReadChunk = 4096;
inline constexpr std::size_t kCopyChunk = 8192;

// Append from fd until EOF. On failure (anything but EINTR), returns false
// and keeps every byte already in `out`, including bytes this call appended
// before the failing read. It does not roll those bytes back. The fd stays open.
[[nodiscard]] inline bool append_fd(int fd, std::string& out) {
    std::array<char, kReadChunk> buf{};
    while (true) {
        const auto n = ::read(fd, buf.data(), buf.size());
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (n == 0)
            return true;
        out.append(buf.data(), static_cast<std::size_t>(n));
    }
}

// Copy until EOF. 0 = success, -1 = read or write error. Does not close fds.
[[nodiscard]] inline int copy_fd(int src, int dst) {
    std::array<char, kCopyChunk> buf{};
    while (true) {
        const auto n = ::read(src, buf.data(), buf.size());
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            return 0;
        std::size_t left = static_cast<std::size_t>(n);
        const char* cursor = buf.data();
        while (left > 0) {
            const auto wrote = ::write(dst, cursor, left);
            if (wrote < 0) {
                if (errno == EINTR)
                    continue;
                return -1;
            }
            if (wrote == 0)
                return -1;
            cursor += wrote;
            left -= static_cast<std::size_t>(wrote);
        }
    }
}

} // namespace aura::compiler::io

#endif
