# EvalValue 64-bit Tagged Encoding Redesign (Issue #181)

**Status:** Design + 4-cycle migration plan. 0 sub-items shipped.
**Date:** 2026-06-13
**Priority:** P0 (correctness + performance)

## Problem (per issue body)

`EvalValue` uses a 64-bit tagged encoding. The current scheme has a
**tag collision**: strings with odd `idx` produce `val & 3 == 1`, which
collides with `is_ref()`. When the ref_type bits (5..2) match defined
types, strings are **misclassified as RefError** or **RefKeyword**.

**Critical impacts**:
- `idx ≡ 31 (mod 64)` → `is_error()` → **potential data loss** in
  `query:def-use`, mutation analysis, etc.
- `idx ≡ 19 (mod 64)` → cosmetic `:<kwd>` display bugs

Current mitigation is defense-in-depth (extra guards in hot paths) but
**not a root fix**.

## Current encoding (audit)

```cpp
// src/compiler/service.ixx:148
static constexpr std::int64_t STRING_BIAS_VAL = -9000000000000000000LL;
// -9e18 in binary: ...0000 (low bit = 0, even)

// make_string(idx) = STRING_BIAS_VAL - idx
// - For odd idx: result is odd (low bit 1) — collides with is_ref()
// - For even idx: result is even (low bit 0) — no collision

// The is_string check is just: val <= STRING_BIAS_VAL
// So odd-idx strings ARE classified as strings by is_string,
// but they ALSO satisfy is_ref (val & 1 == 1). The collision
// is the source of the bug.
```

## 3 options (per issue body)

### Option A (Recommended)
Adjust bias so strings always have `(val & 1) == 0` (dedicated tag bit).
Update `as_string_idx` / `string_idx_raw`.

**Key insight**: the parity of `STRING_BIAS_VAL - idx` flips with the parity
of `idx`. To make all strings have a fixed tag bit, we need a different
encoding. The cleanest approach:
- Use `(val & 3) == 2` for strings (currently unused / "special" in some
  schemes)
- `make_string(idx) = (STRING_BIAS_VAL - idx) | 2`
- `is_string(v) = (v & 3) == 2` (and the value is in the string range)

This requires:
- Changing the bias tag from "any low bit 0" to specifically `(val & 3) == 2`
- Updating all encoding/decoding sites
- Adding `consteval` invariants to enforce the encoding at compile time

**Effort: 1-2 weeks focused work**

### Option B
Reassign `RefError` (8) and `RefKeyword` (11) to unused slots (≥12).
Minimal binary change.

**Key insight**: the collision is `idx ≡ 31 (mod 64) → RefError` and
`idx ≡ 19 (mod 64) → RefKeyword`. Moving these ref types to unused
slots eliminates the collision. But the runtime still has the predicate
chain (check ref, check ref_type) — the collision is just at specific
indices.

This is a smaller change than Option A but doesn't fix the underlying
issue (strings still collide with refs in general, just not at
specific index values).

**Effort: 3-5 days focused work**

### Option C
New encoding scheme with stronger static guarantees + `consteval`
invariants. The most ambitious option.

**Key insight**: completely redesign the encoding to make collisions
impossible at compile time. Use `consteval` to verify the encoding
before runtime. Add per-tag-bits static assertions that prevent
unintentional changes.

**Effort: 2-3 weeks focused work**

## Recommended approach

**Option A** (per the issue body). It's the proper root fix:
- Eliminates the collision at the source
- Enables `consteval` invariants for compile-time verification
- Removes (or drastically reduces) runtime guards
- Provides the cleanest foundation for future encoding work

## Migration plan (4 cycles, each shippable)

### Cycle 1: Encoding prototype + micro-benchmarks
- Prototype Option A in a feature branch
- Run targeted micro-benchmarks on `eval_flat` / value dispatch
- Verify the encoding is correct (all string indices → unique tags,
  no ref collisions)
- Estimate: 2-3 days

### Cycle 2: Encoding migration
- Apply the new encoding to all sites:
  - `make_string`, `as_string_idx`, `string_idx_raw`
  - `is_string` check (now `(v & 3) == 2` and in range)
  - All `is_*` predicates
  - Serialization (the value's bits need to be stable across save/load)
  - JIT emitter (the LLVM IR emission uses the encoding)
- Add `consteval` invariants
- Estimate: 1 week

### Cycle 3: Integration + cleanup
- Remove the runtime guards (defense-in-depth) that are no longer
  needed
- Update Contracts (Issue #144) — the 13 contract_assert sites can
  be tightened
- Update ShapeProfiler — the predicates are now reliable
- Update Tests (existing tests should pass + new exhaustive odd-idx
  tests)
- Estimate: 3-5 days

### Cycle 4: Benchmarks + compatibility check
- Run the full test suite (9 suites)
- Run the regression tests
- Run the safety tests
- TSan + ASan runs
- Verify no performance regression (the fix should be neutral or
  slightly faster — the guards can be removed)
- Estimate: 1-2 days

## Total effort

- Cycle 1: 2-3 days
- Cycle 2: 1 week
- Cycle 3: 3-5 days
- Cycle 4: 1-2 days
- Total: 2-3 weeks focused work

The P0 correctness rating makes this a high-priority follow-up.

## Test scenarios (test_issue_181.cpp, future commits)

- Exhaustive odd-idx test: for every `idx` in `[0, 64)`, verify
  `is_string(make_string(idx))` and `!is_ref(make_string(idx))` and
  `!is_error(make_string(idx))` and `!is_kwd(make_string(idx))`
- Roundtrip: `make_string(idx) → as_string_idx` returns the same idx
- Serialization: save/load preserves the encoding
- JIT: a closure that returns a string produces the same value
- Predicate order: `is_string(v) && is_ref(v)` is now always false
  (the encodings are disjoint)
- Performance: the value dispatch is no slower (ideally faster
  after removing the guards)

## Why design + 4 follow-ups (not one big commit)

The encoding change touches every site that creates or consumes
`EvalValue`. The full PR is 1000+ lines across many files. Following
the marathon's "design + N small cycles" pattern keeps each commit
small + reviewable + bisectable.

The new encoding is incompatible with the old (you can't read old
serialized data with the new encoding). Cycle 2 needs to handle the
migration path: either dual-read (try old, fall back to new) or
versioned serialization.

## Composes with

- #144 (Contracts) — the 13 contract_assert sites can be tightened
  after the encoding is fixed
- #145 (SoA) — the SoA work doesn't depend on the encoding, but
  clean predicates are a prerequisite
- #147 (post-mutation invariant checks) — depends on reliable
  predicates
- #149 (Type Specialization Wrap) — narrow_evidence depends on
  reliable predicates
- `docs/design/value_encoding.md` — the design doc that documents
  the contract; this is the follow-up that fixes the contract
  violation

## Why this is a design doc (not implementation)

The fix is a 2-3 week focused effort. The 3 options in the issue body
are mutually exclusive (different encodings, different migration
paths). A feature branch prototype (Cycle 1) is the right way to
decide which option is best. The remaining cycles (2-4) are
implementation once the choice is made.

## Immediate workaround (already shipped)

Per the issue, the current mitigation is defense-in-depth:
- Extra `&& !is_string(*ar)` guards in hot paths
- These add branch + predicate cost in the interpreter/JIT hot loop
- They are NOT a root fix

The proposal in this design doc (Option A) eliminates the guards
because the encoding is collision-free at the source.
