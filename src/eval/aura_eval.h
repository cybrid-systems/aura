// aura_eval.h — Issue #4946: in-process evaluation ABI for Aura embedders.
//
// The C entry points below let a host program run Aura source in its own
// process: no `aura --serve` child, no line protocol over pipes. A session
// owns one CompilerService (parser + typechecker + evaluator + stdlib module
// loader), so `require`, defines and mutations persist across calls exactly
// as they do in the REPL.
//
// Every call returns a malloc'd JSON record through *result_json (free with
// aura_eval_string_free). Shapes:
//
//   success  {"status":"ok","value":"<printed value>","type":"int","forms":1}
//   failure  {"status":"error","message":"...","line":3,"column":9,"kind":"error"}
//
// A failure is reported as data: the host gets AURA_EVAL_ERROR plus the JSON
// record, and the process keeps running. Only a contract violation (a bug in
// Aura or in the host's use of Aura headers) aborts, per C++26 contracts.
//
// Environment (read at process scope, not per call):
//   AURA_PATH          module search path for (require "std/...");
//                      "std/list" resolves as <dir>/std/list.aura
//   AURA_SANDBOX       sandbox mode for the first session: unset/Restricted
//                      (default) | strict | off
//   AURA_EVAL_GRANT    extra effects for the session: write,exec,network,ffi
//   AURA_PIPELINE_STRICT  keep the production walker-fallback policy
//   AURA_EVAL_DEBUG=1  print the resolved mode/grant on session creation
//
// The session is a development face, like `aura file.aura`: the first
// session applies the sandbox mode and mints Aura's Soft session grant
// (mutate + read inside the session), which a mutating stdlib such as
// std/moead needs. The embedded capability path has no CLI Off bypass, so a
// session created while AURA_SANDBOX=off pins Restricted and mints the Soft
// grant rather than refusing every mutation.
//
// Thread-safety: calls on one session are serialized by an internal mutex, so
// a host may call aura_eval_source() from several threads and each call runs
// to completion. Aura's evaluator is process-wide state (the active service
// pointer, the type registry and the session registry are shared globals), so
// only ONE live session per process is supported; creating a second session
// while the first is alive is a programming error (it retargets the global
// hooks to the newest service). Use one session per process, or one session
// per thread in a host that never evaluates concurrently.
#ifndef AURA_EVAL_H
#define AURA_EVAL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AURA_EVAL_ABI_VERSION 1
/* len argument: use strlen(source) instead of a byte count. */
#define AURA_EVAL_LEN ((size_t)-1)

typedef struct aura_eval_session aura_eval_session;

typedef enum aura_eval_status {
    AURA_EVAL_OK = 0,
    AURA_EVAL_INVALID_ARGUMENT = 1,
    AURA_EVAL_ERROR = 2, /* Aura diagnostic: read the JSON record */
    AURA_EVAL_IO_ERROR = 3,
    AURA_EVAL_INTERNAL = 4, /* host-side failure inside Aura, not source */
} aura_eval_status;

/* Version of this ABI, "1". */
const char* aura_eval_abi_version(void);

/* Create a session. label may be NULL ("embed"). Returns NULL on failure. */
aura_eval_session* aura_eval_session_create(const char* label);

/* Destroy a session. NULL is a no-op. */
void aura_eval_session_destroy(aura_eval_session* session);

/* Session label as passed to create (never NULL for a live session). */
const char* aura_eval_session_label(const aura_eval_session* session);

/* Evaluate every top-level form in source.
 * len == (size_t)-1 means strlen(source). source must not be NULL.
 * On return *result_json is a malloc'd JSON record (or NULL when the
 * arguments are invalid); free it with aura_eval_string_free. */
int aura_eval_source(aura_eval_session* session, const char* source, size_t len,
                     char** result_json);

/* Read path and evaluate it like `aura --load path` (NUL-terminated).
 * A missing/unreadable file yields AURA_EVAL_IO_ERROR plus a JSON record. */
int aura_eval_file(aura_eval_session* session, const char* path, char** result_json);

/* Release a record returned by aura_eval_source / aura_eval_file. */
void aura_eval_string_free(char* text);

#ifdef __cplusplus
}
#endif

#endif /* AURA_EVAL_H */
