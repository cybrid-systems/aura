// serve/serve_async.h — Async serve mode (fiber-based multi-session)
#ifndef AURA_SERVE_SERVE_ASYNC_H
#define AURA_SERVE_SERVE_ASYNC_H

#include <string>

namespace aura::serve {

// Entry point for --serve-async mode.
// Reads JSON-line protocol from stdin in non-blocking mode,
// dispatches to session fibers, returns JSON-line results on stdout.
void run_serve_async();

// Entry point for --serve-async-bench mode.
// Loads an Aura source file and executes it in serve-async runtime,
// enabling fiber parallelism for benchmarks. Falls back to stdin
// mode if not available.
void run_serve_async_bench(const std::string& file_path);

} // namespace aura::serve

#endif // AURA_SERVE_SERVE_ASYNC_H
