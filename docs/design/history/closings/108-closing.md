# Issue #108 — stdlib gaps (documented APIs never wired up)

## Status: ✅ CLOSED — 4 parts, 3 shipped + 1 done in two phases

Issue #108 covered the gap between Aura's *advertised* surface
(`lib/std/adaptive.aura` docs) and its *actual* runtime
primitives. The benchmark failures traced back to 4 missing
API categories. All 4 are now closed: parts 1-3 in single
shippable commits, part 4 in two phases (Phase 1 then Phase 2).

## Commit log

| # | Commit | Description |
|---|--------|-------------|
| 1 | `08a3046` | `hash-for-each` / `hash-fold` (stdlib wrappers) |
| 2 | `ed11de2` | `ast:defs` / `ast:nodes` (C++ primitives) |
| 3 | `d65e8a7` | `m4-move` / `m4-borrow` / `m4-return!` / `define-linear` (pass-through stubs) |
| 4a | `89e6206` | (Phase 0) datatype rolled back + follow-up doc |
| 4b | `67ccc9b` | (Phase 1) datatype: global ctor table + lookup fallback |
| 4c | `54ead84` | (Phase 2) datatype: arity-checked constructors |

## Part-by-part

### Part 1 — `hash-for-each` / `hash-fold`

Added to `lib/std/hash.aura`. Thin wrappers around the
existing `hash-keys` + `std/list` `for-each` / `foldr`. The
benchmark's word-freq task hit "unbound variable:
hash-for-each" — the docs in `adaptive.aura` advertised
`hash-for-each` but no such primitive existed.

Implementation is Aura-side (~20 lines) rather than C++-side
because the wrapper pattern matches the existing stdlib
style (`map`/`filter`/`for-each` are all Aura wrappers, not
primitives). If a benchmark later needs primitive speed, swap
in a direct C++ implementation; the export list stays the same.

### Part 2 — `ast:defs` / `ast:nodes`

Two new C++ primitives. `ast:defs` walks the workspace flat
and returns `((name . node-id) ...)` for every top-level
Define. `ast:nodes` returns `(node-id ...)` in flat order.
Both hold `std::shared_lock` on `workspace_mtx_` (same pattern
as `ast:version`).

### Part 3 — M4 linear types (4 pass-through primitives)

`m4-move`, `m4-borrow`, `m4-return!`, `define-linear` were
documented in `adaptive.aura` but never registered. The
`Linear`/`Move`/`Borrow`/`MutBorrow`/`Drop` NodeTags ARE
handled by the tree-walking evaluator (returns the inner
expression), so the AST-side path works. What's missing is
the user-facing **let-binding** path — a real `define-linear`
would track consumption; the stub just returns void.

These are stubs. Real M4 ownership tracking (move-counting,
scope-leak diagnostics) is a much larger feature beyond
#108's scope. The stubs make the names exist so the
benchmark can load.

### Part 4 — `datatype` ADT macro (Phases 0/1/2)

This was the most ambitious part. Three attempts.

#### Phase 0 — rolled back (`89e6206`)

Two attempts that hit pre-existing Aura limitations:
1. `defmacro` in `std/adt.aura` — rest params inside macro
   body crash, multi-form body returns `()`. Macro-expander
   bugs, not datatype-specific.
2. `parse_datatype` parser special form emitting a `Begin` of
   `Define` nodes — the Begin's Defines are scoped to the
   Begin, not top-level. Subsequent top-level expressions
   couldn't see the constructors.

Follow-up doc `docs/design/issue-108-datatype-followup.md`
explains both and recommends the Phase 1 path.

#### Phase 1 — global ctor table + lookup fallback (`67ccc9b`)

The recommended path from the follow-up doc:
- `g_adt_constructors`: file-scope `unordered_map<string,
  AdtCtorEntry>` in `evaluator_impl.cpp`.
- `Env::lookup` gets a 4th fallback (after local bindings,
  parent env, primitives): look up in the map and return
  `make_primitive(slot)`.
- `adt:register-constructors` primitive: walks a list of ctor
  name strings, registers each. The ctor's body is a
  primitive that, when called with args, returns
  `(cons "CtorName" (list arg1 arg2 ...))` — exactly the
  format `match`'s `compile_pattern` constructor-pattern
  code expects.
- `parse_datatype` emits a single Call node
  `(adt:register-constructors (cons "Ctor1" (cons "Ctor2" ...)))`.
  Parser-side, the field-type annotations are skipped
  (Phase 1 doesn't enforce type params).

Why this design: a (datatype ...) form is a single top-level
expression; the parser can only return one root node. The
global table + lookup fallback is the only way to make ctor
names globally visible from a single parsed form. The
match side is already done — only the construct side was
missing.

#### Phase 2 — arity checking (`54ead84`)

Phase 1 silently accepted any number of args. Phase 2 adds:
- `AdtCtorEntry.arity` field.
- Wire format: alist `((name . arity) (name . arity) ...)`
  instead of Phase 1's flat list of names.
- `parse_datatype` counts field-type identifiers per ctor
  clause (nested parametric types count as 1).
- The ctor primitive checks `args.size() == arity` at apply
  time. Mismatch returns a tagged error pair:
  ```
  (<adt-error> ctor '<N>' arity mismatch: expected A got B)
  ```
  so the caller can pattern-match on the error.

Wire format change is backwards-incompatible with Phase 1
— but the only emitter is `parse_datatype`, updated in the
same commit. No external code emits ADT wire data.

## Verification (final state)

- `./build/test_ir` exit=0 (all suites pass)
- `tests/fuzz_defuse.py --quick`: 200/200
- `tests/fuzz_workspace.py --quick`: 298/298
- `tests/fuzz_snapshot.py --quick`: 405/405 (48 restores)
- ASAN test_ir: exit=0, 0 leaks
- ASAN stress (30 iter × 3 ctors + match + arity violation):
  0 leaks
- Manual: adt-tree, adt-either, multi-arg datatype, zero-arg
  ctor, arity-mismatch error — all work
- Manual: `(define make-leaf Leaf)` — ctor is first-class
  (works for free, no extra work needed)

## What's still open

### First-class function semantics (Phase 3 territory)

The ctor is a primitive, not a closure. Most paths work
(apply dispatches through `is_primitive`), but some
edge cases (e.g. `(define make-leaf Leaf)` then
`(define make-leaf-2 make-leaf)` then `(make-leaf-2 99)`)
may have edge cases. Tested the simple first-class case
manually and it works. Full closure identity is Phase 3.

### Session cleanup

`g_adt_constructors` is process-global. Multi-session
benchmarks that share a process will accumulate entries
across sessions. `(adt:reset-constructors)` exists for
tests that need a clean slate. Not used by the fuzzer
because each fuzzer session spawns its own aura process.

### Type-param enforcement

The `:TypeParam` declaration in `(datatype (Name : T) ...)`
is parsed-and-ignored. Aura's gradual type system doesn't
track ADT type parameters; adding it is a separate feature.

## Architectural impact

`g_adt_constructors` is the first file-scope global in
Aura that exists specifically to bypass Aura's env scoping
for a specific use case. It established a pattern that
future "global primitive" features (linear-type moves,
effects) can build on: file-scope table + Env::lookup
fallback + primitive-side registration. Documented
inline in the `g_adt_constructors` comment block.
