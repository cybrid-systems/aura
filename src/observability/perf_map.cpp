// perf_map.cpp — standard Linux perf map for JIT-compiled Aura functions.
//
// Issue #5409: the JIT owns executable pages that the ELF symbol table does
// not describe. writing /tmp/perf-<pid>.map lets stock `perf record -g` and
// FlameGraph name Lisp frames without an in-program tracer. The map is opt-in
// (AURA_PERF_MAP=1) so normal runs keep zero file-system side effects.
#include "observability/perf_map.hh"

#include "core/runtime_config.hh"

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace aura::observability {
namespace {

constexpr std::size_t kDefaultJitFunctionSize = 4096;
constexpr std::size_t kMaxMapName = 180;

struct MapState {
    int fd = -1;
    bool initialized = false;
    bool enabled = false;
    std::unordered_set<std::uintptr_t> seen;
};

MapState& state() {
    static MapState s;
    return s;
}

std::mutex& map_mutex() {
    static std::mutex m;
    return m;
}

void open_locked(MapState& s) {
    if (s.initialized)
        return;
    s.initialized = true;
    s.enabled = ::aura::config::flag(::aura::config::Key::AURA_PERF_MAP);
    if (!s.enabled)
        return;
    char path[64];
    const int n = std::snprintf(path, sizeof path, "/tmp/perf-%ld.map",
                                static_cast<long>(::getpid()));
    if (n <= 0 || static_cast<std::size_t>(n) >= sizeof path)
        return;
    s.fd = ::open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (s.fd < 0)
        s.enabled = false;
}

std::string clean_name(const char* name) {
    std::string out;
    out.reserve(64);
    if (!name || !*name)
        return "anonymous";
    for (const unsigned char c : std::string_view(name)) {
        if (out.size() >= kMaxMapName)
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

}  // namespace

void perf_map_note_jit(const void* address, std::size_t size, const char* name) noexcept {
    if (!address)
        return;
    std::scoped_lock lock(map_mutex());
    auto& s = state();
    open_locked(s);
    if (!s.enabled || s.fd < 0)
        return;
    const auto addr = reinterpret_cast<std::uintptr_t>(address);
    if (!s.seen.insert(addr).second)
        return;
    const std::string symbol = "Lisp:" + clean_name(name);
    const std::size_t map_size = size ? size : kDefaultJitFunctionSize;
    char line[320];
    const int n = std::snprintf(line, sizeof line, "%" PRIxPTR " %zx %s\n", addr, map_size,
                                symbol.c_str());
    if (n > 0 && static_cast<std::size_t>(n) < sizeof line)
        (void)::write(s.fd, line, static_cast<std::size_t>(n));
}

bool perf_map_enabled() noexcept {
    std::scoped_lock lock(map_mutex());
    auto& s = state();
    open_locked(s);
    return s.enabled;
}

}  // namespace aura::observability
