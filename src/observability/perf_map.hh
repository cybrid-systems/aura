// perf_map.hh — external /tmp/perf-<pid>.map support (Issue #5409).
//
// The JIT owns executable pages that are invisible to the ELF symbol table.
// With AURA_PERF_MAP=1, every compiled Aura function is appended to the
// standard Linux perf map so perf record/report and FlameGraph can name
// Lisp frames without any in-program tracing hooks.
#ifndef AURA_OBSERVABILITY_PERF_MAP_HH
#define AURA_OBSERVABILITY_PERF_MAP_HH

#include <cstddef>

namespace aura::observability {

// Append one JIT function to /tmp/perf-<pid>.map when AURA_PERF_MAP is on.
// `name` should be the Lisp-visible function name; the map writer prefixes
// it with Lisp: and replaces whitespace so the standard start-size-name
// format stays parseable. Duplicate addresses are ignored. Never throws.
void perf_map_note_jit(const void* address, std::size_t size, const char* name) noexcept;

// True when the map writer is enabled for this process. The first call reads
// the environment and opens the map; later calls are constant-time.
bool perf_map_enabled() noexcept;

}  // namespace aura::observability

#endif  // AURA_OBSERVABILITY_PERF_MAP_HH
