# Static Reflection Phase 4+ — Design (Issue #178)

**Status:** Design + 5 follow-up cycles. 0/5 sub-items shipped.
**Date:** 2026-06-13
**Priority:** P0 Critical

## Scope (per issue body)

| # | Sub-item | Status | Effort |
|---|---|---|---|
| 1 | `reflect_module_exports` (module export tables) | TODO | 2-3 days |
| 2 | `auto_deserialize` for containers + nested struct | TODO | 1-2 days |
| 3 | `reflect_schema.hh` + `query:schema` integrate with mutate | TODO | 2-3 days |
| 4 | Migrate AST/IR to `reflect_members` + `auto_serialize` | TODO | 1 week |
| 5 | Tests: module roundtrip, mutate+schema, hygienic macro | TODO | 1-2 days |

Total: 2-3 weeks focused effort.

## Why a design doc (not implementation)

The 5 sub-items are large (1 week of focused work for the AST/IR migration alone).
None of them are partially done in origin/main — the existing reflection files
(`reflect.hh`, `reflect_schema.hh`, `type_validate.hh`, `opcode_reflect.hh`,
`cache_reflect.cpp`) implement Phases 1-3 only. Phase 4+ is fresh work.

This commit ships the design + caller inventory + 5-cycle migration plan. The
actual code is follow-up work.

## Existing reflection surface (Phase 1-3)

- `reflect.hh` (850 lines): the core reflection primitives
  - `nonstatic_data_members_of` for compile-time member enumeration
  - `auto_to_json` / `auto_serialize` for cache/IR serialization
  - `auto_validate` for type validation
- `reflect_schema.hh` (217 lines): schema generation
- `type_validate.hh` (139 lines): type-level validation
- `opcode_reflect.hh` (90 lines): opcode reflection
- `read_auto_validate.hh` (72 lines): read-side validation
- `tag_dispatch.hh` (86 lines): tag-based dispatch
- `cache_reflect.cpp` (61 lines): cache-specific reflection

## Detailed sub-item scope

### Sub-item 1: `reflect_module_exports`

Compile-time scan of a module's public API:
- Inputs: module name (e.g., `"std/list"`)
- Output: list of (function_name, signature, return_type) tuples
- Mechanism: extend the `nonstatic_data_members_of` pattern to functions
  (which it currently doesn't cover). For Aura's stdlib, also a
  separate pass that scans the stdlib .aura source files at build time
  and extracts the public bindings.
- Consumers: `query:module-exports` primitive, AI Agent for module
  discovery, IDE/UI for autocomplete

### Sub-item 2: `auto_deserialize` for containers + nested struct

Current deserialization covers POD types. Extend to:
- `std::vector<T>` — needs T's reflect
- `std::optional<T>` — has_value + value
- `std::array<T, N>` — N from template param
- `std::variant<Ts...>` — alt index + value
- Nested struct — recursive `auto_deserialize` on each member

The mechanism is similar to `auto_serialize` but reversed (read fields
from a serialized form).

### Sub-item 3: `reflect_schema.hh` + `query:schema` integration

Schema generation for mutate safety:
- For each reflectable type, generate a schema (field name, type,
  writable flag, range constraints)
- The schema is queryable via a new primitive `query:schema` (or
  `mutate:validate`)
- A mutate that violates the schema (wrong type, out-of-range value)
  is rejected at the API boundary
- This complements #165 (post-mutation hygiene) by adding
  pre-mutation validation

### Sub-item 4: AST/IR migration to `reflect_members` + `auto_serialize`

The biggest sub-item. Today the AST and IR have hand-written visitor
patterns, type-dispatch, and serialization. Migrating to the reflection
primitives:
- Each AST node type gets `RELECT_MEMBERS` macro for its fields
- `auto_serialize` replaces the hand-written JSON / binary serializers
- `auto_deserialize` for the load path
- The `kOpcodeInfo` table (ir_executor.ixx) becomes reflection-driven
- Estimated 1 week focused work; touches many files

### Sub-item 5: Tests

- `tests/test_issue_178.cpp` (new file)
- Module export roundtrip: `reflect_module_exports("std/list")` returns
  the same set as a manual enumeration
- Mutate + schema: a mutate that writes an out-of-range value is
  rejected; a valid mutate is committed
- Hygienic macro: `query:schema` on a macro-introduced binding
  returns the user's intended schema (not the macro's internal
  one) — this composes with #140's hygiene

## Migration plan (5 cycles, each shippable)

### Cycle 1: `reflect_module_exports` (Sub-item 1)
- Add `module_exports<T>()` to `reflect.hh`
- Add a stdlib source scanner (build-time tool that extracts public
  bindings from .aura files)
- Add `query:module-exports` primitive
- Test: `test_issue_178.cpp` Cycle 1 tests

### Cycle 2: `auto_deserialize` for containers (Sub-item 2)
- Extend `reflect.hh` with container reflection
- Add `auto_deserialize` overloads for vector/optional/array/variant
- Test: roundtrip serialize → deserialize → verify equality

### Cycle 3: `query:schema` + mutate integration (Sub-item 3)
- Add `schema<T>()` to `reflect_schema.hh`
- Add `query:schema` primitive
- Wire into `mutate:rebind` to validate before applying
- Test: schema-rejected mutate + valid mutate

### Cycle 4: AST/IR migration (Sub-item 4, the biggest)
- Migrate IR node types to `RELECT_MEMBERS`
- Migrate AST node types
- Replace hand-written serializers with `auto_serialize`
- Replace hand-written deserializers with `auto_deserialize`
- Replace `kOpcodeInfo` with reflection-driven dispatch
- Test: ensure existing tests still pass (no behavior change)

### Cycle 5: Tests + integration (Sub-item 5)
- Module export roundtrip
- Mutate + schema + hygienic macro
- TSan + ASan runs

## Test scenarios (test_issue_178.cpp, future commits)

- `reflect_module_exports("std/list")` returns the same set as a manual enumeration
- `auto_deserialize(vector<int>{1,2,3})` roundtrips correctly
- `auto_deserialize(optional<int>{42})` roundtrips correctly
- `query:schema` on a known type returns its schema
- A `mutate:rebind` with a schema-violating value is rejected
- A `mutate:rebind` with a schema-conformant value succeeds
- Reflection-driven IR serialization produces the same bytes as hand-written
- TSan-clean for 1000+ iterations of mutate + serialize + deserialize

## Effort estimate

- Cycle 1: 2-3 days
- Cycle 2: 1-2 days
- Cycle 3: 2-3 days
- Cycle 4: 1 week
- Cycle 5: 1-2 days
- Total: 2-3 weeks focused work

The marathon session (4 hours today, 19 commits, 15 issues closed) shipped
the related workstreams #170-177, #186, #193, #194, #198, #199, #200, #204.
#178 is the natural next P0 but doesn't fit in a single session.

## Why design + 5 follow-ups (not one big commit)

The 5 sub-items are mostly independent but each is a 1+ day effort.
The full PR would be 1000+ lines across many files. Following the
marathon's pattern of "design + N small shippable cycles", each
cycle is its own verify+close unit. The commit history becomes
a step-by-step migration that's easy to review and bisect.
