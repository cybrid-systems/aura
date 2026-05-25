// serve/serve_async.h — Async serve mode (fiber-based multi-session)
#ifndef AURA_SERVE_SERVE_ASYNC_H
#define AURA_SERVE_SERVE_ASYNC_H

namespace aura::serve {

// Entry point for --serve-async mode.
// Reads JSON-line protocol from stdin in non-blocking mode,
// dispatches to session fibers, returns JSON-line results on stdout.
void run_serve_async();

} // namespace aura::serve

#endif // AURA_SERVE_SERVE_ASYNC_H
