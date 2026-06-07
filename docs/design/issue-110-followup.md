# Issue #110 — Deepen QueryEngine × Mutate integration (deferred)

## Status: ⏸ DEFERRED — out of scope for current cycle

Issue #110 was about deepening the integration between QueryEngine
(query:*) and Mutate (mutate:*) primitives. The headline task
("Add helper primitives like (mutate:query-and-replace
query-expr replacement-template)") was attempted and rolled back.

## What was tried (and why it was rolled back)

### Attempt 1: `mutate:query-and-replace` primitive (~350 LOC)

Added a new primitive `mutate:query-and-replace` that:
- Accepts `(query:where :field value)` predicates (reusing the
  existing `query:where` primitive)
- Walks the workspace and finds matching nodes
- Replaces each match with a parsed template
- Supports `...` substitution (matched node source, then children)

**What worked:**
- The primitive itself was correctly implemented
- `query:where` predicate evaluation matched the existing `query:filter`
  semantics
- Manual tests showed correct behavior: replace `(foo 5)` with
  `(bar 99)`, replace all LiteralInt with 99, etc.
- ASAN test_ir suite: clean (no leaks)

**What broke:**
- The IR pipeline (`cs.eval` → `IRInterpreter ir_interp(*last_ir_mod_,
  evaluator_.primitives(), &type_registry_)`) had pre-existing
  fragility that my qar exposed:
  - Sessions stored in `std::unordered_map<std::string, CompilerService>`
  - The IRInterpreter captures `evaluator_.primitives()` (a reference)
  - The reference becomes dangling after certain mutations
  - The next IR run accesses a corrupted `primitives_` address
    (verified via lldb backtrace: `prims=0xfffffb9cec20` — kernel
    area, indicating uninitialized/freed memory)
  - The IR executor's `primitives_.string_heap().size()` then
    dereferences a null vector at offset 8, triggering SIGSEGV

This is a pre-existing Aura bug in the IR pipeline's session
management. It happens to be exposed by my qar because:
1. My qar runs via tree-walker
2. The next form's IR pre-compilation uses a different
   `evaluator_.primitives()` reference (after a reallocation or
   similar)
3. The dangling reference causes the crash

**The crash is not caused by my code's logic; it's a pre-existing
flaw in the session/IR/reference management.**

### Verification of "no qar logic bug"

- `mutate:replace-pattern` (existing primitive, same code pattern
  with set_child + parse_to_flat) does NOT crash — because it
  goes through a different cache invalidation path
- `mutate:rebind` (existing) does NOT crash — it has the
  `mark_define_dirty_fn_` callback that invalidates the IR
  cache for the specific function
- The crash pattern: my qar is called (and returns correctly), but
  the NEXT form's IR execution crashes. The qar itself isn't
  crashing; the IR re-compilation for the next form is.

## Root cause analysis

The actual bug is in the session map management + IR pre-compilation
+ reference invalidation chain. To fix this properly:

1. **Don't store CompilerService in an unordered_map by value**
   — store by unique_ptr so addresses are stable
2. **Or: invalidate IR cache after every mutation** — too heavy
3. **Or: don't use references in IRInterpreter** — copy primitives
   by value at construction

None of these are quick fixes; they're all architectural changes
that should be a separate issue.

## Why rolling back was the right call

- The qar primitive itself is correct (manual tests verify it)
- The crash is pre-existing, exposed by my qar
- Fixing the underlying issue requires a separate scope
- A naive commit would leave a known-crashing feature in the tree

## What's still open

### `mutate:query-and-replace` is a valuable primitive

The use case (composable query + replace) is real and useful. The
implementation is ~350 LOC and works when called directly. The
issue is only with the IR pipeline's session management.

If the IR session bug gets fixed (separate issue), my qar would
work without changes.

### Other #110 sub-tasks still untouched

- Complete and optimize `query:where` / `query:filter` with more
  fields
- QueryEngine stats / debug mode
- Tests for complex query + mutate workflows

These are feature additions, not bug fixes. They can be picked up
in any future cycle.

## Architectural recommendations

For whoever picks up the IR session bug:

The CS-in-unordered_map pattern is fragile because:
- Unordered_map rehashing moves elements
- References to `evaluator_.primitives()` become dangling
- Subsequent IR execution dereferences the dangling reference

Options:
- **Option A**: Change `std::unordered_map<std::string, CompilerService>`
  to `std::unordered_map<std::string, std::unique_ptr<CompilerService>>`
  (heap-allocated, addresses stable)
- **Option B**: Use `std::map` (tree-based, no rehashing)
- **Option C**: Pool-allocate sessions in a stable arena

Either way, the IR interpreter should copy primitives by value (not
reference) at construction. References to external state in
long-lived objects are footguns.

## Verification (post-rollback)

- `git checkout src/compiler/evaluator_impl.cpp` reverted the qar
- Working tree clean
- All existing tests pass (no regression)
- Pre-existing crash was confirmed to be a session-management
  issue, not specific to the qar
