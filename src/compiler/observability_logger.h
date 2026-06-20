// observability_logger.h — structured JSON logger (Issue #62 Iter 2/3)
//
// Replaces ad-hoc fprintf(stderr, ...) with a single
// log_event_deopt() helper that emits a JSON object to stderr.
// Off by default; enable with AURA_OBS_LOG=1 (env var), like the
// deopt trace from #61 Iter 4.
//
// Implementation note: this header is designed to be #include'd
// in module TU's via the global module fragment, where consteval
// (used by reflect.hh's auto_to_json) is not available. So we
// use std::fprintf with manual JSON formatting. The schema is
// fixed (fn/expected/actual/generic_block for deopt events).
// For more complex event types, extend the dispatcher below.
//
// For --evo-explain (Iter 3), the JSON serialization of
// CompilerSnapshot is done in observability_json.cpp (a
// non-module TU compiled with -freflection). Module TU's
// call the `snapshot_to_json()` function below.
//
// Usage:
//   log_event_deopt("foo", 1, 4, 7);
//   →  {"event":"deopt","fields":{"fn":"foo","expected":1,"actual":4,"generic_block":7}}

#ifndef AURA_COMPILER_OBSERVABILITY_LOGGER_H
#define AURA_COMPILER_OBSERVABILITY_LOGGER_H

#include <cstdio>
#include <cstdlib>
#include <string>

#include "observability_snapshot.h"

namespace aura::compiler {

// Issue #62 Iter 2: log a structured deopt event as JSON.
// Enabled by AURA_OBS_LOG=1 in the env. Cost when off: one
// getenv() call (cached as a static bool).
inline void log_event_deopt(const char* fn, std::uint32_t expected_shape,
                            std::uint32_t actual_shape, std::uint32_t generic_block) {
    static const bool enabled = []() {
        const char* e = std::getenv("AURA_OBS_LOG");
        return e && (e[0] == '1' || e[0] == 't' || e[0] == 'T');
    }();
    if (!enabled)
        return;
    const char* fn_str = fn ? fn : "?";
    // Hand-formatted JSON: avoid the JSON serializer dependency
    // (reflect.hh uses consteval which is not available in the
    // global module fragment, and the IR interp is a module TU).
    std::fprintf(stderr,
                 "{\"event\":\"deopt\",\"fields\":{"
                 "\"fn\":\"%s\",\"expected\":%u,\"actual\":%u,\"generic_block\":%u}}\n",
                 fn_str, expected_shape, actual_shape, generic_block);
}

// Issue #62 Iter 3: forward-declared in observability_json.cpp.
// Returns a JSON object literal of the snapshot. Compiled with
// -freflection (the framework's auto_to_json).
std::string snapshot_to_json(const CompilerSnapshot& s);

} // namespace aura::compiler

#endif // AURA_COMPILER_OBSERVABILITY_LOGGER_H
