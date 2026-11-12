# Embedding Aura runtime pieces (#4774, #4946)

Three link targets cover a host C++ program: two static libraries that stay
clear of the Evaluator, and one that embeds the Evaluator itself.

| Target | Contents |
| --- | --- |
| `aura::core` | `core/persistent_child_vector.hh`, `compiler/fiber_cow_overlay.hh`, `core/fiber_context.hh` (header-only), plus the C++26 contract violation handler and the process-wide `fiber_ctx` thread_local |
| `aura::fiber_rt` | `aura::serve::Fiber` / `Scheduler` (`serve/scheduler.h`), worker threads and work stealing; links `aura::core` |
| `aura::eval` | the in-process evaluation ABI (`eval/aura_eval.h`): `aura_eval_session_create` / `aura_eval_source` / `aura_eval_file` / `aura_eval_session_destroy`, backed by one `CompilerService` per session (`#4946`) |

```cmake
find_package(aura CONFIG REQUIRED)      # or add_subdirectory / in-tree target
target_link_libraries(my_app PRIVATE aura::fiber_rt)   # or just aura::core
target_link_libraries(my_app PRIVATE aura::eval)       # run Aura source in-process
```

Install with `cmake --install <build> --component aura_core --prefix <dir>`.
(`aura::eval` is build-tree only for now — see "In-process evaluation".)

## Contract handler

Aura headers use C++26 contracts (`pre()`, `contract_assert`). A translation
unit compiled with `-fcontracts` that includes them needs a
`handle_contract_violation(const std::contracts::contract_violation&)` at link
time. `aura::core` provides it (and sets `-fcontracts` on consumers); a host
must not define its own. To observe violations, register a hook with
`aura_set_contract_violation_hook()`; the handler calls it, then aborts.

## Evaluator-side hooks in `aura::fiber_rt`

The fiber and scheduler code reaches the Evaluator only through C-ABI hooks.
In `aura::fiber_rt` they default to no-ops (`src/compiler/fiber_bridge.cpp`,
`src/serve/fiber_rt_defaults.cpp`); all defaults are weak, so a host that also
links the real owners gets theirs. The values installed by the runtime can be
read back with `aura_fiber_rt_current_fiber_id_fn()` and
`aura_fiber_rt_long_mutation_hook()`.

A multi-worker `Scheduler` refuses to run outside a sandbox decision (#3586);
a host that does not use the Aura sandbox sets `AURA_SANDBOX=off`.

Regression tests: `tests/core/test_aura_core_embed.cpp`,
`tests/core/test_aura_fiber_rt_embed.cpp`.

## Line protocol (`--serve` and `--serve-async`) (#5158)

`aura --serve` and `aura --serve-async` both read one request per line from
stdin and write a JSON reply to stdout. A line whose first non-space
character is `{` is a JSON command. Any other non-blank line is a plain Aura
s-expression and is evaluated like `exec`. `--serve-async` also skips a line
whose first non-space character is `;`.

`aura help --all` prints the command names, the framing, and the reply shapes.

### Shared commands

The same client works against both modes for this set:

| Request | What it does |
| --- | --- |
| `{"cmd":"exec","code":"(+ 1 2)"}` | Evaluate `code`. |
| `{"cmd":"session","action":"list"}` | List session names. |
| `{"cmd":"session","action":"create","name":"s"}` | Create session `s`. |
| `(+ 1 2)` | Plain s-expression. Same as `exec` of that line. |

An unknown command is an error whose `msg` is `unknown command: <cmd>`
(`eval` is not a command). A JSON object with no `cmd` field has `msg`
`missing cmd field`.

### Reply shapes

`--serve` replies have no session envelope:

| Result | Shape |
| --- | --- |
| value | `{"status":"ok","value":"..."}` |
| error | `{"status":"error","msg":"..."}` |
| list | `{"status":"ok","sessions":[...],"active":"<name>"}` (pretty-printed across lines) |
| created | `{"status":"created","session":"<name>"}` |
| already exists | `{"status":"ok","session":"<name>"}` |

Creating a session on `--serve` also activates it: later lines that omit a
session run there. `(display ...)` writes its bytes to stdout ahead of the
JSON line. `display` does not add a newline, so those bytes can share the
line with the reply.

`--serve-async` wraps every reply with the session that handled it. Printed
output is captured into `display` instead of being written raw:

| Result | Shape |
| --- | --- |
| value | `{"session":"default","status":"ok","value":"..."}` |
| value with output | `{"session":"default","status":"ok","value":"...","display":"..."}` |
| error | `{"session":"default","status":"error","msg":"..."}` |
| list | `{"session":"default","status":"ok","sessions":[...],"active":"default"}` (one line) |
| created | `{"session":"default","status":"created","name":"<name>"}` |
| already exists | `{"session":"default","status":"ok","name":"<name>"}` |

Async routes a JSON line by its `session` field. An empty field or
`"default"` stays on the default session. A plain s-expression has no field,
so it stays on default too. `active` in an async list reply is therefore
`"default"`: creating a session does not retarget later unscoped lines. Put
`"session":"<name>"` on a later `exec` to run it there. `session` `list` and
`create` are handled on the default session.

### Mode-only commands

`--serve-async` only:

- `{"cmd":"session-send","target":"<name>","data":"..."}` replies
  `{"session":"...","status":"sent","target":"..."}`.
- `{"cmd":"session-recv"}` replies `{"session":"...","status":"msg","data":"..."}`
  or `{"session":"...","status":"timeout"}`.

`--serve` only: `session` action `delete`
(`{"status":"deleted","session":"<name>"}`; the default session cannot be
deleted), plus `module`, `mutate`, `rollback`, `mutation-log`, `jit:metrics`,
`config`, `defmacro`, `define`, `redefine`, `unparse`, `write`, and `query`.
Those are outside the shared set. Async does not delete a session: the
session fiber holds the session for the life of the process.

This `msg` field is the line protocol. The in-process `aura::eval` records
below use `message` instead.

## In-process evaluation (#4946)

Before #4946 the only way to run Aura from another process was a child:
`aura --serve` plus the line protocol above. `aura::eval` embeds the
evaluator instead — same parser, typechecker, evaluator and stdlib module
loader the REPL drives, in the host's own process.

```c
#include "eval/aura_eval.h"

aura_eval_session* s = aura_eval_session_create("memory-runtime");
char* rec = nullptr;
if (aura_eval_source(s, "(require \"std/moead\" all:)", AURA_EVAL_LEN, &rec) == AURA_EVAL_OK) {
    /* rec: {"status":"ok","value":"...","type":"module"} */
}
aura_eval_string_free(rec);
aura_eval_session_destroy(s);
```

Pass `AURA_EVAL_LEN` (`(size_t)-1`) to use `strlen`, or an explicit byte
count for a non-NUL-terminated slice.

### Result records

Every call that gets past argument checking returns a malloc'd JSON record
through its `char**` out-parameter, even when it fails:

| Shape | When |
| --- | --- |
| `{"status":"ok","value":"3","type":"int"}` | evaluated; `value` is the printed last form |
| `{"status":"error","message":"...","line":3,"column":9,"kind":"UnboundVariable"}` | parse / type / eval diagnostic |
| `{"status":"error","message":"...","kind":"error-value"}` | the form produced an Aura error *value* (a refused mutate, a bad `car`) |
| `{"status":"error","message":"cannot read file: ...","kind":"io-error"}` | `aura_eval_file` could not read the path |

The status code mirrors the record: `AURA_EVAL_OK`, `AURA_EVAL_ERROR`,
`AURA_EVAL_IO_ERROR`, `AURA_EVAL_INTERNAL`, or `AURA_EVAL_INVALID_ARGUMENT`
(NULL handle/source/out-parameter — no record is allocated). Errors are data:
the session stays usable and the process keeps running. Only a C++26 contract
violation aborts, exactly as it does inside `aura` itself.

### State, stdlib and environment

A session is a REPL-like scope: `define`, `mutate!`, loaded modules and the
JIT/IR caches persist across calls. `(require "std/list" all:)`,
`(require "std/moead" all:)` and the rest of `lib/std` resolve through
Aura's module search path: `AURA_PATH` first (colon-separated directories;
`std/list` becomes `<dir>/std/list.aura`), then the executable-relative
`lib/` and `../lib/`, then the cwd. A host whose binary does not sit next to
an Aura tree sets `AURA_PATH=/path/to/aura/lib`.

The session is a development face, like `aura file.aura`: the first session
applies the process sandbox mode from `AURA_SANDBOX` (default Restricted, as
the CLI does) and mints the Soft session grant (mutate + read inside the
session). `AURA_EVAL_GRANT=write,exec,network,ffi` adds effects; TenantAdmin,
Syscall and MacroSelfEvo are never granted from the environment. The embedded
capability path has no CLI Off bypass, so a session created while
`AURA_SANDBOX=off` pins Restricted and mints the Soft grant instead of
refusing every mutation (`AURA_EVAL_DEBUG=1` shows `sandbox=1 soft=1`). The
walker fallback policy is set to
`Allow` at session creation unless `AURA_PIPELINE_STRICT` is set, matching
the `--load` / oneshot faces. `AURA_EVAL_DEBUG=1` prints the resolved
sandbox mode, the single-tenant gate and the grant bits on session creation.

### Thread-safety boundary

* Calls on **one session** are serialized by an internal mutex. A host may
  call `aura_eval_source` from several threads; each call runs to completion,
  they do not run in parallel.
* Aura's evaluator is **process-global** (active-service pointer, type
  registry, session registry, contract hooks). **One live session per
  process** is the supported configuration; create the session on the thread
  that will use it, and destroy it before creating another.
* Sessions are not a sandbox. `aura_eval_source` runs Aura with whatever
  effects the process allows (`AURA_SANDBOX` / `AURA_EVAL_GRANT`).

### Artifact

The ABI's objects are compiled into the shared runtime library the test suite
already links (`libaura_test_objects.so`), and `aura::eval` is an interface
target over it. Splitting a dedicated installed `libaura_eval.so` means a
second full compile of the evaluator graph; that split is follow-up work, not
part of #4946.

Regression test: `tests/core/test_aura_eval_embed.cpp`
(`test_aura_eval_embed_verification`), which links only `aura::eval` and
covers eval-string, `require std/moead` ask/tell, error paths, file load and
concurrent callers.
