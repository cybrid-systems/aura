// perf_map.hh — external /tmp/perf-<pid>.map support (Issue #5409).
//
// The JIT owns executable pages that are invisible to the ELF symbol table.
// With AURA_PERF_MAP=1, every compiled Aura function is appended to the
// standard Linux perf map so perf record/report and FlameGraph name Lisp
// frames without any in-program tracing hooks.
//
// Header-only on purpose: aura_jit.cpp is compiled into many test targets,
// and a separate perf_map.cpp would require adding it to every target's
// source list. Inline functions share one function-local state per program.
#ifndef AURA_OBSERVABILITY_PERF_MAP_HH
#define AURA_OBSERVABILITY_PERF_MAP_HH

#include "core/runtime_config.hh"

#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace aura::observability {
namespace detail {

    struct PerfMapState {
        int fd = -1;
        bool initialized = false;
        bool enabled = false;
        std::unordered_set<std::uintptr_t> seen;
    };

    inline PerfMapState& perf_map_state() {
        static PerfMapState s;
        return s;
    }

    inline std::mutex& perf_map_mutex() {
        static std::mutex m;
        return m;
    }

    inline void open_locked(PerfMapState& s) {
        if (s.initialized)
            return;
        s.initialized = true;
        s.enabled = ::aura::config::flag(::aura::config::Key::AURA_PERF_MAP);
        if (!s.enabled)
            return;
        char path[64];
        const int n =
            std::snprintf(path, sizeof path, "/tmp/perf-%ld.map", static_cast<long>(::getpid()));
        if (n <= 0 || static_cast<std::size_t>(n) >= sizeof path)
            return;
        s.fd = ::open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
        if (s.fd < 0)
            s.enabled = false;
    }

    inline std::string clean_name(const char* name) {
        std::string out;
        out.reserve(64);
        if (!name || !*name)
            return "anonymous";
        for (const unsigned char c : std::string_view(name)) {
            if (out.size() >= 180)
                break;
            // The perf map is whitespace-delimited: a space or newline in a
            // Lisp symbol would break every following field. Keep printable
            // non-space bytes (including the :, / and - Lisp names commonly use).
            if (c <= 0x20 || c == 0x7f)
                out.push_back('_');
            else
                out.push_back(static_cast<char>(c));
        }
        return out.empty() ? std::string("anonymous") : out;
    }

} // namespace detail

// Append one JIT function to /tmp/perf-<pid>.map when AURA_PERF_MAP is on.
// Duplicate addresses are ignored. Never throws.
inline void perf_map_note_jit(const void* address, std::size_t size, const char* name) noexcept {
    if (!address)
        return;
    std::scoped_lock lock(detail::perf_map_mutex());
    auto& s = detail::perf_map_state();
    detail::open_locked(s);
    if (!s.enabled || s.fd < 0)
        return;
    const auto addr = reinterpret_cast<std::uintptr_t>(address);
    if (!s.seen.insert(addr).second)
        return;
    const std::string symbol = "Lisp:" + detail::clean_name(name);
    const std::size_t map_size = size ? size : 4096;
    char line[320];
    const int n =
        std::snprintf(line, sizeof line, "%" PRIxPTR " %zx %s\n", addr, map_size, symbol.c_str());
    if (n > 0 && static_cast<std::size_t>(n) < sizeof line)
        (void)::write(s.fd, line, static_cast<std::size_t>(n));
}

inline bool perf_map_enabled() noexcept {
    std::scoped_lock lock(detail::perf_map_mutex());
    auto& s = detail::perf_map_state();
    detail::open_locked(s);
    return s.enabled;
}

} // namespace aura::observability

#endif // AURA_OBSERVABILITY_PERF_MAP_HH
