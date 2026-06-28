# Primitive vs Stdlib Decision Framework

> **Authoritative answer** for "should this be a C++ primitive or an Aura stdlib function?"
> Required reading before adding any new primitive (`primitives_.add("foo", ...)`).

---

## 0. TL;DR

**Default = stdlib.** Start in `lib/std/`. Promote to a primitive only when
**at least one red-line criterion** is true AND moving it would clearly
outweigh the costs (see §2). Most "wouldn't this be nice as a primitive"
requests fail this test.

---

## 1. Red Lines — MUST remain a primitive

If **any** of these is true, it MUST be a primitive (no stdlib workaround is
acceptable). These are the production-engine invariants the runtime
guarantees; they can't be reconstructed in pure Aura code.

1. **Engine-boot dependency** — Must exist before `set-code` / `eval-current`
   can run. The parser + lowerer + workspace boot call these primitives as
   their **first action**. Without them the workspace can't load.
   Examples: `make_int`, `make_string`, `make_pair`, `cons`, `car`, `cdr`,
   `is_int`, `is_string`, `is_pair`, `+`, `-`, `*`, `/`, `define`,
   `set-code`, `eval-current`, `lambda`, `apply`, `eval`.

2. **Internal-state access** — Must touch private engine state that Aura
   code has no path to:
   - `workspace_mtx_` (`std::shared_mutex`) for the Mutate lock protocol
   - `FlatAST` internals (node_id ↔ symbol mapping, dirty bitmask,
     panic-checkpoint stack, env_frames_ deque)
   - `defuse_index_` + `defuse_affected_syms_` for query/mutate consistency
   - `ir_cache_` + `jit_cache_` for incremental compilation
   - `metrics_` (CompilerMetrics) for observability counters
   - `arena_` + `pmr::vector` for memory accounting
   - `messaging_bridge` for fiber / yield / IPC
   Examples: every `mutate:*`, every `query:*`, every `compile:*`,
   `compile-current-output`, `api-reference`, `panic-checkpoint`.

3. **Performance hot path** — Must compile to <10ns in the JIT or <1ns in
   `eval_flat` because it's called per node / per instruction.
   Examples: type predicates (`is_int`/`is_string`/`is_pair`), arithmetic
   (`+`/`-`/`*`/`<`/`=`), `car`/`cdr`/`cons`, `not`.

4. **FFI / hardware bridge** — Crosses the Aura ↔ host boundary (libc,
   filesystem, network, git, messaging). Must be C++ because Aura can't
   reach libc without FFI.
   Examples: `git:*`, `fs:*`, `network:*`, `messaging:*`, `time:*`,
   `compile-binary-*`, FFI runtime registration.

5. **Type-system / IR bridge** — Must participate in the type checker,
   occurrence narrowing, or JIT lowering. Aura can't observe these
   internally.
   Examples: `query:type`, `compile:current-type`, `compile:verify-*`,
   `compile:assert`, `aot:*`.

6. **Observability / debug-required** — Must produce stats that the
   `--evo-explain` / `--serve-async` JSON protocol expects as a structured
   integer (not a free-form Aura expression). Counters, histograms, and
   snapshots live here.
   Examples: every `query:*-stats` primitive, `compile:snapshot`,
   `compile:*-trace`, `(api-reference)`.

7. **Diagnostic / recovery primitive** — Required for safe operation
   under panic / corruption / Guard failure. The stdlib cannot
   self-heal because the stdlib itself may be the corrupted state.
   Examples: `panic-checkpoint`, `panic-restore`, `panic-safe-source`,
   `panic-auto-rollback?`, `eval-with-recovery`.

**If you find yourself trying to re-implement a red-line primitive in stdlib,
that's a strong signal the stdlib layer is trying to do too much — promote
the upstream caller, not the downstream workaround.**

---

## 2. Green Lights — promote to stdlib

If the candidate **fails every red-line test** AND satisfies **at least one**
green-light criterion, it should be a stdlib function. Default to this.

1. **Pure function** — No state, no env, no side-effects beyond its args.
   Examples: `(map f xs)`, `(filter pred xs)`, `(reduce op init xs)`,
   `(string-lower s)`, `(clamp x lo hi)`.

2. **User convenience over primitive** — Already expressible as 5-20 lines
   of existing primitives. The stdlib is the "DSL of composition".
   Examples: `(take n xs) = (if (<= n 0) nil (cons (car xs) (take (- n 1) (cdr xs))))`,
   `(group-by keyfn xs)`, `(partition pred xs)`.

3. **Domain-specific** — Belongs to a vertical (string utils, collection
   utils, math, EDSL metaprogramming). Keep verticals in their own
   `lib/std/<vertical>/` so re-org doesn't churn primitives.
   Examples: `lib/std/string/format.aura`, `lib/std/json/encode.aura`,
   `lib/std/edsl/syntax-rules.aura`.

4. **Layered over a primitive** — The primitive provides the engine hook
   (e.g. `eval`); the stdlib provides the wrapper that's actually called
   from user code.
   Examples: `lib/std/edsl/with-exception-handling.aura` (wraps `eval`
   with `try`/`catch` from `try-begin`/`try-end` primitives).

5. **Slow / uncommon** — Not on any hot path. The cost of primitive
   registration + JIT lowering per primitive is worth it only when
   the primitive is called frequently. Library code that runs once per
   AST or per user action is a stdlib candidate.

6. **Experimental / unstable** — Not yet stable enough to bake into the
   engine. Promote to primitive after the API has stabilized AND
   one of red-lines 2/3/5 applies.

---

## 3. The grey zone — when to ask

Some candidates are ambiguous. Examples:

- **Format / serialization** — `to_json` could be a primitive (JSON
  protocol contract) OR stdlib (user-side serialization). Rule: if it
  must produce exactly the wire format `--serve-async` speaks, primitive.
  Otherwise stdlib.
- **Deep-equal / structural compare** — Primitive if needed by the
  type-checker / ir-executor for memoization; otherwise stdlib.
- **Pretty-print** — Almost always stdlib. The engine never needs to pretty-
  print internally.
- **Statistical / numeric aggregations** — Primitives if they need to be
  JIT-fast on large arrays (`(reduce + 0 xs)` is JIT-compiled; `(quantile 0.95 xs)` is stdlib).

When in doubt, **default to stdlib**. Promotion from stdlib to primitive
is a refactor with clear scope; demotion the other way isn't.

---

## 4. The cost of a primitive

Don't underestimate. A primitive adds to:

1. **Build time** — every test binary links the `evaluator_*.cpp` TU
   that registers it; ~31 TUs now.
2. **Registry lookup cost** — the `Primitives` table is an
   `unordered_map<string, PrimFn>` checked on **every** `eval_flat`
   instruction call. Naming collisions, typo exposure, and lookup
   overhead all grow.
3. **API surface** — primitives are part of the long-term API
   contract. Removing a primitive is a breaking change for every
   downstream Agent that learned the name.
4. **CI gate** — adding a primitive triggers `docs/generated/
   primitives.md` regeneration (see §6 of `contributing.md`); forgetting
   this breaks `python3 build.py gate`.
5. **JIT code-gen** — every primitive needs an `aura_jit.cpp` lowering
   entry (or an explicit "interpreter-only" comment). JIT-less
   primitives pay a per-call eval-flat cost.

If your candidate doesn't pull its weight on **at least one** of these
five axes, it's a stdlib.

---

## 5. Decision flow (use this when in doubt)

```
                ┌─────────────────────────────────┐
                │ New candidate primitive?        │
                └────────────┬────────────────────┘
                             │
       Does it satisfy ANY red line (1-7)?  ──── yes ──► PRIMITIVE
                │
               no
                │
       Does it satisfy ANY green light (1-6)? ──── yes ──► STDLIB
                │
               no
                │
       Is the design stable (≥1 production user)?  ── no ──► STDLIB (experimental)
                │
               yes
                │
       Ask the team. Default to STDLIB until convinced.
```

When asking the team, post:

- The candidate's name + a 3-line description
- Which red-lines you considered (and why each was rejected)
- Which green-lights apply
- The estimated cost of each axis in §4 for **this** primitive
- A proposed stdlib fallback (so reviewers can judge the gap)

---

## 6. Migration playbook

When a stdlib function turns out to need a primitive (e.g. JIT perf
bottleneck at scale):

1. Land the stdlib version first (covers immediate need).
2. Ship the primitive alongside it as a `libc:`-prefixed twin
   (e.g. `(libc:fast-reduce op init xs)`). Document the JIT lowering
   that's required for the speed-up.
3. After 1+ production deployment confirms the speed-up justifies
   the engine cost, swap the stdlib default to call the primitive.
4. Never remove the stdlib function — Agents may have learned it.

---

## 7. What this framework is NOT

- **Not a freeze on primitives.** Adding a primitive that's clearly
  justified by red-line 2 (internal-state access) is welcome.
- **Not a substitute for code review.** Reviewers can still override
  this framework with documented justification.
- **Not a license to bypass the CI gate.** Every primitive addition
  MUST regenerate `docs/generated/primitives.md` (see `contributing.md`
  §6). Forgetting breaks `python3 build.py gate`.

---

## 8. References

- [`docs/contributing.md` §2 — Add primitive](../contributing.md)
- [`docs/architecture.md`](../architecture.md) — module map + data flow
- [`docs/generated/primitives.md`](../generated/primitives.md) — current primitive catalog
- [`docs/design/compilation/ir_pipeline.md`](../design/compilation/ir_pipeline.md)
  — JIT lowering path that primitives must integrate with
- [`docs/PRODUCTION_ISSUES_TRACKER_REFINED.md`](../PRODUCTION_ISSUES_TRACKER_REFINED.md)
  — production review that motivated this framework

---

_Last reviewed: 2026-06-28. Send edits / PR to `docs/design/primitive-vs-stdlib-decision-framework.md`._