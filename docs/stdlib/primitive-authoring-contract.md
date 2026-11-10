# Primitive authoring contract (Issue #2915)

Stable contract for **humans and AI Agents** that add C++ Aura primitives.
Follow this document; do **not** invent a new registration style.

## Canonical scaffolding

| Piece | Location |
|-------|----------|
| Official helper | `src/compiler/prim_registrar_scaffold.hh` → `register_prim` + `PrimSpec` |
| Render hot-path helper | Retired with tui:* (#2626). Do not add new render-hot prims. |
| Meta macros (legacy/compatible) | `DEFINE_PRIMITIVE_META` / `DEFINE_PRIMITIVE_META_SECURE` in `primitives_detail.h` |
| Capture / error helpers | `PRIM_ERROR`, `PRIM_CAPTURE_*` in `primitives_detail.h` |
| Central boot map | `src/compiler/evaluator_primitives_registry.cpp` (#1552) |
| Error-return convention | [primitive-error-convention.md](primitive-error-convention.md) (#2914) |

## Minimal registration (proof pattern)

```cpp
#include "prim_registrar_scaffold.hh"

void register_misc_primitives(PrimRegistrar add, Evaluator& ev) {
    using aura::compiler::register_prim;
    using aura::compiler::pure_general;

    register_prim(add, ev, "current-time",
        [](const auto&) -> EvalValue {
            return make_int(static_cast<std::int64_t>(std::time(nullptr)));
        },
        pure_general(0, "() -> int",
                     "Wall-clock seconds since Unix epoch."));
}
```

Factories: `pure_general`, `impure_general`, `io_general`, `mutate_general` (all in the scaffold header). `impure_general` keeps the general defaults but marks the prim impure (e.g. `for-each`, which invokes its callee for effects without mutating its own arguments).

## Rules Agents must keep

1. **One style** — Prefer `register_prim(...)`. Do not add ad-hoc `add` + custom meta maps in new TUs. `register_render_hot_prim` is retired (#2626).
2. **Central registry** — Every new `register_*_primitives` group is called from `Evaluator::register_all_primitives()`.
3. **PrimMeta auto-effects** — Leave `PrimSpec.required_effects == 0` so issue **#2152** infers from the name (`mutate:`, `file:`, …). Set explicit bits only when the name does not encode the effect.
4. **Hot table** — Set `perf_tier = kPrimPerfHot` only when the prim is on a trusted hot path; `finalize_hot_table()` (Evaluator ctor) rebuilds `hot_map_` after all registrations. Scaffold never bypasses that path.
5. **Errors** — True failures return `make_primitive_error` / structured `make_merr`, not silent `#f` / void (see error convention).
6. **Discovery** — After landing, names appear in `query:primitives-meta` / `primitive:describe` / generated `docs/generated/primitives.md`. Do not invent parallel discovery APIs without updating the #1552 registry comment block.
7. **Tests** — Suite or unit test for behavior + update coverage gate if the issue adds a linter.

## Discovery surfaces (unchanged)

- `(require "std/primitives" all:)` → `primitives:help` / `:list` / `:discover`
- `(primitive:describe name)`
- `(query:primitives-meta)` / `(query:primitives-meta-catalog)`
- `(query:primitive-list-with-meta)`
- `docs/generated/primitives.md` + `docs/generated/primitives-registry.md`

## Proof migration

`evaluator_primitives_misc.cpp` uses `register_prim` for `current-time`, `current-time-ms`, `monotonic-ms`, and `arena-offset` (Issue #2915).

## Core TU migration (Issue #2996)

These commercial hot-path TUs are **migrated** onto `register_prim` + `PrimSpec` (non-empty `schema` + `doc` on every name). Do not reintroduce bare `add(...)` without PrimMeta in them:

| TU | Group |
|----|--------|
| `evaluator_primitives_list.cpp` | list / list? / null? / length / list-ref / member / append / reverse / map / filter / take / drop / foldl / list-sort |
| `evaluator_primitives_math.cpp` | math + regex + arithmetic (+ gated m4-*) |
| `evaluator_primitives_json.cpp` | json-encode / json-parse / json-get-string |
| `evaluator_primitives_pair.cpp` | pair + string core constructors |
| `evaluator_primitives_vector.cpp` | vector + hash core |

`g_register_prim_scaffold_total` counts every `register_prim` stamp. Discovery: `query:primitives-meta` / `primitive:describe`.

## Primitive tiers and the registration gate (Issue #5008)

v0.2.0 shrinks the primitive surface. This section is the contract that keeps
the shrink from growing back: one tier per name, one list, one gate.

### The one criterion

**A name may stay in C++ only if Aura code cannot implement it without losing
atomicity or a host effect.** Nothing else counts — not "it is convenient",
not "it has always been a primitive", not "it is faster" (speed is a separate
discussion, and a stdlib-candidate can still be a fast Aura function).

Every registered name carries exactly one tier:

| Tier | Meaning | Examples |
|------|---------|----------|
| `kernel` | Aura cannot implement it without losing atomicity or a host effect | `car` (value model), `+` (hot table), `workspace:rollback-to` (atomic workspace state), `read-file` (host effect), `query:calls` (reads the def-use index under a workspace lock) |
| `stdlib-candidate` | Pure value->value function that Aura can host in `lib/std` | `map` / `filter` / `foldl`, `string-join`, `caar`, `equal?`, `json-encode` |
| `delete` | Redundant: another registered name already provides the behaviour | `truncate` ("Alias of trunc."), `ceiling` ("Alias of ceil."), `exact` / `inexact` |

The tier says where a name *belongs*, not what ships today. Turning
`stdlib-candidate` into a stdlib function and `delete` into a removal is the
job of the follow-up issues (#5009 / #5006); this contract only fixes the
labels and the gate.

### One list, three registration channels

Three channels register primitives and all are part of the catalog:

1. `register_prim(add, ev, "name", ...)` and `add("name", ...)` in
   `src/**/*.cpp`.
2. `add_mutate("mutate:name", ...)` in `evaluator_primitives_mutate.cpp`.
   The name is a string literal; the wrapper forwards it to `add`. The
   #5008 scan reads that literal (#5095). A new `add_mutate` name with no
   matching removal and no `kernel_exceptions` entry fails the gate.
3. The evaluator ctor hot table `table_["name"] = ...` in
   `src/compiler/evaluator_primitives_builtins.cpp`. `+ - * / < <= = > >= not and or eq?`
   are registered *only* here.

`scripts/tools/prim_surface.py` is the single scan for all three channels.
`scripts/tools/gen_docs.py` (docs) and
`scripts/coverage/checks/check_primitive_tiers_5008.py` (gate) both import it, and
both read the same tier list, so `docs/generated/primitives.md` and the CI check
cannot disagree.

**The list** is `docs/stdlib/primitive-tiers.json`:

```json
{
  "rule_notes": { "K-HOT": "hot table ...", "D-ALIAS": "redundant registration" },
  "kernel_exceptions": [],
  "primitives": { "+": { "tier": "kernel", "rule": "K-HOT" } }
}
```

Each entry carries the tier and the rule id that produced it (`rule_notes` explains
the rule), so a reviewer can re-derive the label instead of trusting it. The
bulk of the list was classified by those rules; contested names carry a
per-name `note`. Do not hand-maintain a second list in a document.

### The growth gate

`check_primitive_tiers_5008.py` enforces four things:

1. **Coverage** — every registered name has a tier; every tier entry still
   matches a registration (renames must update the list).
2. **Ratchet** — the count relative to
   `docs/generated/primitives-baseline.json` (recorded when this contract
   landed) may only shrink. A new name must be paid for by removing at least
   one registration in the same change, or be listed in
   `kernel_exceptions`. Otherwise the check fails with the offending names
   and both fixes.
3. **Retirement** — a deprecated name that is still registered must be flagged
   `"deprecated": true`; a name flagged deprecated must not be called from
   `lib/std`.
4. **Delete hygiene** — a `delete`-tier name must not be called from `lib/std`.

Baseline updates are explicit (`--update-baseline`) and need a reason in the
PR, exactly like `stats-primitives-baseline.json` (#1448). Grow the count only
through `kernel_exceptions`, which is the contract's kernel exception table:

> A name may be added when it is genuinely kernel (no Aura implementation
> without atomicity / host-effect loss) and no removable name exists in the
> same change. List it in `kernel_exceptions` with the justification, in the
> same commit. The list stays empty by default; the first entries are the
> SRFI-151 bitwise names added with #5003, whose justification is recorded in
> `rule_notes` (no bit operation existed in Aura before them).

Run it directly (`python3 scripts/coverage/checks/check_primitive_tiers_5008.py`) or
through `./build.py gate --changed`, which auto-discovers `check_*.py`.
`--report` prints the tier table plus which names have no `lib/std` caller —
the evidence the deletion issues work from.

### Retirement channel

v0.2.1 removed the v0.2.0 alias window. `ast:` `workspace:` `dirty:`
`mutation-log:` `ws:` `syntax:` `fiber:overlay-*` `fiber:spawn-backend`, the
non-kernel `orch:` names, and the seven bare spellings are not registered.
Their bodies are `query:` / `mutate:` primitives or a std module. `aot:` stays;
it was not marked deprecated.

`PrimMeta.deprecated` is still the runtime switch for a name that is registered
and retiring:

- `(query :api-reference)` lists those names under `*deprecated*` and prints
  `PrimMeta.doc`. It does not emit `[use (engine:metrics "name")]`.
- The propagation path is `ObservabilityPrims::mark_p1b_top_stats_deprecated` /
  `mark_residual_public_stats_deprecated` in
  `evaluator_primitives_observability.cpp`, plus the tier list flag.
- A name that is being retired stays registered and deprecated for one
  release cycle (the alias window), then the registration is removed and the
  tier entry goes with it.

The 8 `mutate:*` aliases in `DEPRECATED_PRIMITIVES` (#1436) are already past that
window: they are not registered any more, so `(query :api-reference)` on the current
tree has no `*deprecated*` entries while the channel itself is unchanged and
still exercised by the engine's own mark lists. That is the "retired names are
already out of the catalog" completion branch of #5008.

### Adding a primitive (checklist)

1. Is it kernel by the criterion above? If not, write it in Aura under
   `lib/std` and do not add a registration.
2. Remove at least one registration in the same change, or add the name to
   `kernel_exceptions` with the justification.
3. Add the tier entry (`kernel` / `stdlib-candidate` / `delete`) with a rule id.
4. `python3 build.py docs` then `./build.py gate --changed`.

### Known disputes

- `json-*`, `string-*` and vector helpers are `stdlib-candidate` here because the
  criterion is atomicity/host-effect only. Issue #1440 kept them core for
  speed; that is a performance judgement, not a semantic one. Whoever moves
  them must bring a benchmark, and nothing in this gate forbids keeping them.
- Engine-verb families (`query:`, `orch:`, `workspace:`) are
  kernel because they read or mutate engine internals. Their reduction comes
  from deleting redundant verbs (#5009/#5006), not from re-labelling them.

## One read verb, one write verb (Issue #5009)

The algebra the v0.2.0 surface converges on:

| Verb | Shape | What it covers |
|------|-------|----------------|
| read | `(query :op …)` | nodes, children/parent, find/filter, def-use, provenance, dirty, reachability, type feedback, stable refs, mutation log |
| write | `(mutate :op …)` | snapshot/restore/validate, atomic batches, overlays, workspace sync, syntax markers |

Two op spellings are accepted:

- a **keyword** carries the canonical short name — `(query :node-type id)`,
  `(mutate :snapshot "tag")`;
- a **string** carries the primitive's own name —
  `(query "qr:diff" id)`. A retired prefix in that position is an error.

An unknown `:op` still fails loudly (with a `did you mean` hint), and an
unknown string op is `no-prim`.

**Retired prefixes.** `ast:` `workspace:` `dirty:` `mutation-log:` `syntax:`
`ws:` and `fiber:overlay-*` are not registered (#5097). `(query :op)` looks up
`query:op`, then `qr:op` for a body that moved off a retired prefix (so the
public `query:` add() list stays under its ceiling). `(mutate :op)` looks up
`mutate:op`, then `mw:op`. Snapshot and restore are `(mutate :snapshot)` /
`(mutate :restore)`. `fiber:spawn` `fiber:yield` `fiber:join` `fiber:cancel`
are not query or mutate ops.

**fiber:** keeps `spawn` `yield` `join` `cancel`. The COW overlay verbs
(`overlay-begin` … `overlay-commit`) are `(mutate :overlay-…)`; the backend
read is `(query :fiber-backend)`.

**Wrappers.** `std/query`, `std/ast`, `std/workspace` and `std/compat` are thin
wrappers over the two algebras: they compose, format and default, and every
engine call they make is a verb. Do not add a second synonym for an op that
already exists.

## Five orchestration verbs (Issue #5006)

`orch:` keeps `spawn` `send` `recv` `join` `cancel` — the scheduler/mailbox
surface Aura cannot implement itself. Each verb is the kernel's name for
exactly one primitive — `spawn` = `fiber:spawn`, `send`/`recv` = the
`channel:` pair, `join` = `fiber:join`, `cancel` = `fiber:cancel` — and
`std/orch` composes exactly those five.

Everything else the prefix used to carry (workflow composition, supervision,
handoff tokens, the agent directory, metrics reset) is `std/orch`. Those 24
names are not registered (#5097). `_agent:spawn` /
`_agent:list` stay as the underscore-plane kernel entry points, and
`agent:recover-from-error` / `agent:tick` / `agent:running?` stay as
agent-loop kernel state.

**Single-use prefixes are not kernel.** `session:` `closure:` `hot-swap:`
`string-pool:` `scheduler:` `evaluator:` `runtime:` `ir-cache-v2:` `jit:`
`hygiene:` `arena:` `orchestration:` are the same class of duplicate: each is
deprecated in favour of an `engine:metrics` / `mutate` / `query` op or a std
library function. `aot:` is the deliberate remainder in this cycle because
`lib/std/hot-update*` still calls it; it moves when that library gets its verb.

### The keep/leave table for bare names

| Class | Decision |
|-------|----------|
| Scheme evaluation kernel (car cdr cons eq?, arithmetic, comparisons, apply, type predicates, the native map/list-sort hot paths) | keep |
| Host effects Aura cannot express (file descriptors, process, time, FFI allocation) | keep, and the name says which effect |
| Pure formatting over metadata (diagnose, check-success) | std/diagnose and std/test; the bare primitive is unbound |
| Engine-state reads and writes (api-reference, arena-offset, auto-rollback-on-panic, define-strategy, edsl:define-struct) | a query or mutate op; the bare primitive is unbound |
| The C FFI family (c-load c-func c-alloc c-free c-opaque c-struct-size) | keep: dlopen/dlsym/native memory is a host effect; std/ffi is the library face |

The bare spellings are unbound. `kRetiredBare5006` only flags a
re-registration. `lib/std` calls `(query …)`, `(mutate …)`, `std/diagnose`, or
`std/test`.

#### The bare names this cycle moves (Issue #5006)

| Name | Replacement | Why it leaves |
|------|-------------|---------------|
| `diagnose` | `std/diagnose` - `(require "std/diagnose" all:)` | pure string -> plan table; no engine state |
| `check-success` | `std/test` - `(require "std/test" all:)` | pure match over output strings |
| `api-reference` | `(query :api-reference)` | prints the registered primitive table: engine state |
| `arena-offset` | `(query :arena-offset)` | thread-local arena bump offset: engine state |
| `auto-rollback-on-panic` | `(mutate :auto-rollback-on-panic #t\|#f)`, `(query :auto-rollback-on-panic)` | panic checkpoint state |
| `define-strategy` | `(mutate :define-strategy name body ...)` | writes the agent strategy registry |
| `edsl:define-struct` | `(mutate :define-struct name doc schema)` | records the EDSL struct schema and its counters |

The `c-*` family is the deliberate remainder: `dlopen` / `dlsym` / `malloc` are
host effects, so no Aura implementation exists. The names stay kernel, keep
`std/ffi` as their library face, and are recorded in `kernel_exceptions` with
the `K-FFI-5006` note instead of being silently re-labelled.

`(require "std/test" all:)` and `(require "std/diagnose" all:)` install the
new implementation, so a file that requires them calls the std function while
the kernel registration keeps working for code that does not.

The rule that decides a new name is still the one criterion above: atomicity or
a host effect. "It is a primitive today" is not a reason.
