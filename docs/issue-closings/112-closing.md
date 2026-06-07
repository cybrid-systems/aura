# Issue #112 — Evaluator developer guide (follow-up to #111 audit)

## Status: ✅ CLOSED — `docs/developer/evaluator.md` shipped

Issue #112 is the explicit follow-up the #111 audit requested.
The audit's last paragraph was:

> This should be documented in the developer guide for the
> evaluator (in `docs/developer/evaluator.md` or similar — to be
> created).

This is that document.

## Commit

| Commit | Description |
|--------|-------------|
| `TBD`  | `docs(developer): add evaluator developer guide (#112)` |

## What the guide covers

`docs/developer/evaluator.md` (15 sections, ~23 KB) consolidates
the evaluator-specific dev patterns that have emerged across
#107 (concurrency), #108 (stdlib gaps), #109 (fibers), and
#110 (self-modifying-flat bug). The high-value sections:

| § | Section | Why it matters |
|---|---------|----------------|
| 0 | Mental model | The 3 invariants (flat-grows-during-eval, multi-thread, def-use invalidation) |
| 1 | **Self-modifying-flat iteration rule** | The #1 footgun; snapshot `end_id = flat.size()` before the loop |
| 2 | Adding a new primitive | Arg validation, return conventions, the `merr` pattern |
| 3 | Mutate locking protocol | `unique_lock` / `shared_lock` / no-re-entrance |
| 4 | DefUseIndex touch protocol | Both `defuse_affected_syms_` and `defuse_touch_fn_` |
| 5 | Closures vs primitives | When `make_closure` vs `make_primitive` |
| 6 | C FFI primitives | `c-load` / `c-func` / `c-call` / `c-close` conventions |
| 7 | ADT constructor table | Why `g_adt_constructors` exists; how to model similar features |
| 8 | Recursion guards | `MAX_ENV_DEPTH`, `MAX_C_STACK_DEPTH`, why both are `thread_local` |
| 9 | IRContext | Why future IR-runtime state belongs in `IRContext`, not `IRInterpreter` |
| 10 | AST snapshot/restore | What invalidates, what preserves, what the OOM fallback does |
| 11 | **Common pitfalls checklist** | 11-item pre-merge checklist (snapshot, lock, defuse, args, …) |
| 12 | Testing conventions | Unit, fuzz, ASAN expectations |
| 13 | File map | Where each kind of code lives |
| 14 | Related docs | Cross-refs to #110 follow-up, #111 audit, defuse design, etc. |

## Why this matters

The patterns documented here were either:

- **Hard-won through bugs.** The §1 self-modifying-flat rule
  came from the `qar` crash (#110, ~3 days of investigation).
- **Easy to forget across sessions.** The §3 locking protocol
  and §4 defuse touch both have "do both" rules that are easy
  to drop one of on a Friday afternoon.
- **Architectural decisions worth recording.** §7 (ADT ctor
  table) and §9 (IRContext) are non-obvious choices made for
  specific reasons that future contributors would re-litigate
  without context.

The guide is **living**: §15 explicitly invites future sections
as new patterns emerge (dyn-*, jit bridge, AOT emit, …).

## What's NOT in the guide (deliberate)

- **Tutorial-level content**: how to build Aura, how to write a
  primitive from scratch in 5 steps. That's `docs/tutorial.md`
  territory.
- **API reference**: every primitive signature. That's
  `docs/api-reference.md`.
- **C++ style guide**: pure-function > state-machine, DOD, etc.
  That's `docs/design/cpp26_guide.md`.
- **The actual code**: the guide references code paths with
  line numbers and snippets, but doesn't reproduce the file.

The guide is the *missing layer* between "I know C++" and "I
know how to add a primitive to the Aura evaluator without
introducing a regression."

## Verification

- Document compiles to a clean markdown (46 headers, 11 code
  blocks, 22.9 KB, 630 lines).
- All line-number references verified against the current tree
  (`evaluator_impl.cpp`, `evaluator.ixx`, `ir_executor.ixx`).
- All `merr` / `MAX_*_DEPTH` / `g_fiber_yield_mutation_boundary`
  / `mark_dirty_upward` patterns referenced in the doc exist
  in the code as described.
- No code changes from this issue. Working tree was clean
  before the doc was added.

## Total #112 work

| Commit | Description |
|--------|-------------|
| `TBD`  | `docs(developer): add evaluator developer guide` |

1 commit, 1 new doc, 0 code changes. Future sessions add
sections as new patterns emerge.
