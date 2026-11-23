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

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <ios>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>

#include <unistd.h>

namespace aura::observability {
namespace detail {

    struct PerfMapState {
        std::ofstream stream;
        bool initialized = false;
        bool enabled = false;
        std::unordered_set<std::uintptr_t> seen;
    };

    inline PerfMapState& perf_map_state() {
        static PerfMapState state;
        return state;
    }

    inline std::mutex& perf_map_mutex() {
        static std::mutex mutex;
        return mutex;
    }

    inline void open_perf_map(PerfMapState& state) {
        if (state.initialized)
            return;
        state.initialized = true;
        state.enabled = ::aura::config::flag(::aura::config::Key::AURA_PERF_MAP);
        if (state.enabled == false)
            return;
        const std::string path =
            "/tmp/perf-" + std::to_string(static_cast<long>(::getpid())) + ".map";
        state.stream.open(path, std::ios::out | std::ios::app);
        state.enabled = state.stream.is_open();
    }

    inline std::string clean_name(const char* name) {
        if (name == nullptr)
            return "anonymous";
        const std::string_view name_view(name);
        if (name_view.empty())
            return "anonymous";
        std::string cleaned;
        cleaned.reserve(64);
        for (const unsigned char character : name_view) {
            if (cleaned.size() >= 180)
                break;
            // The perf map is whitespace-delimited: a space or newline in a
            // Lisp symbol would break every following field. Keep printable
            // non-space bytes (including the :, / and - Lisp names commonly use).
            if (character <= 0x20 || character == 0x7f)
                cleaned.push_back('_');
            else
                cleaned.push_back(static_cast<char>(character));
        }
        return cleaned.empty() ? std::string("anonymous") : cleaned;
    }

} // namespace detail

// Append one JIT function to /tmp/perf-<pid>.map when AURA_PERF_MAP is on.
// Duplicate addresses are ignored. Never throws.
inline void perf_map_note_jit(const void* address, std::size_t size, const char* name) noexcept {
    if (address == nullptr)
        return;
    const std::scoped_lock lock(detail::perf_map_mutex());
    auto& state = detail::perf_map_state();
    detail::open_perf_map(state);
    if (state.enabled == false)
        return;
    const auto address_value = reinterpret_cast<std::uintptr_t>(address);
    if (state.seen.insert(address_value).second == false)
        return;
    const std::size_t map_size = size != 0 ? size : 4096;
    const std::string symbol = "Lisp:" + detail::clean_name(name);
    state.stream << std::hex << address_value << ' ' << map_size << ' ' << symbol << '\n';
    state.stream.flush();
}

inline bool perf_map_enabled() noexcept {
    const std::scoped_lock lock(detail::perf_map_mutex());
    auto& state = detail::perf_map_state();
    detail::open_perf_map(state);
    return state.enabled;
}

} // namespace aura::observability

#endif // AURA_OBSERVABILITY_PERF_MAP_HH
