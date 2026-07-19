# 1648 — nested struct reflection observability (scope-limited-progressive Phase 1)

**Status:** Phase 1 shipped (commit pending, on `49f97b5c` baseline)
**Branch:** `main`
**Date:** 2026-07-19

## Context

The static reflection infrastructure in `src/reflect/reflect.hh` supports
flat structs + basic containers (`std::vector<T>`, `std::array<T,N>`,
`std::optional<T>`, `std::variant<Ts...>`) via `auto_serialize<T>` /
`auto_deserialize<T>`. For nested structs (`struct Wrapper { S inner; }`
or `std::vector<S>` where `S` has a nested struct member) the
`auto_serialize<T>` template's nested-`MemberKind::Struct` case **deliberately
throws** per Issue #1124 / #1125, which established a "refuse silent drop"
invariant for the nested-struct gap.

The body of #1648 reports that AI Agent ergonomics around deeply
nested reflection are limited — agents can't serialize complex state
without hitting the throw. Full recursive support
(depth-aware + cycle-detect + auto-forwarding through the generic
struct template) is genuinely multi-session template-metaprogramming
work that exceeds single-ship scope.

Predecessor coverage:

- `#1679` — `fix(reflect): terminate AST validate walk on cyclic children`
  (cycle detection at the AST walk layer)
- `#1611` — `feat(reflect): MacroIntroduced hygiene gate schema` (hygiene gate)
- `#1907` — `feat(bridge): close reflect/EDSL bridge with runtime mutate + hygiene validation`
- `#1047` — Phase 1 for `#1047–#1071` (hygiene / type / mutate safety)

## What landed (this commit, Phase 1)

### 1. Process-wide observability counter + C-linkage accessor

`src/reflect/reflect.hh` adds a file-scope atomic counter at the top of
the file (just before the `// ── auto_serialize<T> ──` block):

```cpp
namespace aura::reflect {
inline std::atomic<std::uint64_t>& reflect_nested_struct_throw_count_ref() noexcept {
    static std::atomic<std::uint64_t> counter{0};
    return counter;
}
}  // namespace aura::reflect

extern "C" {
inline std::uint64_t aura_reflect_nested_struct_throw_count_v_read() noexcept { ... }
inline void aura_reflect_nested_struct_throw_count_v_bump(std::uint64_t delta) noexcept { ... }
}
```

This makes the nested-struct gap **observable** through the existing
C-shared counter surface (compiled with the rest of `aura-reflect`,
exposed via the C-bridge shim layer).

### 2. Counter bump at both throw sites

`src/reflect/reflect.hh` adds `aura_reflect_nested_struct_throw_count_v_bump(1);`
immediately before the throw at:

- The `auto_serialize<T>` nested `MemberKind::Struct` case (around line 678)
- The `auto_deserialize_struct` nested `MemberKind::Struct` case (around line 993)

**The throws are preserved** per the `#1124` / `#1125` "refuse silent drop"
invariant. Phase 1 adds observability BEFORE the throw — the throw is
still surfaced to the caller (preserving the invariant) but now the
monitor can see how often it fires.

The error message is augmented with a `(see #1676)` pointer to the
follow-up issue.

## What's NOT shipped (deferred to Phase 2+)

| Why deferred                                                                | Follow-up issue |
|------------------------------------------------------------------------------|-----------------|
| Full recursive `reflect_members<T>` (depth-aware + cycle-detect + auto-forwarding through generic struct template) | **#1676** |
| `reflect:serialize-node` EDSL primitive — composes the new recursive support | **#1678** |
| `reflect:mutate-member` EDSL primitive with Guard + marker stamp            | **#1678** (paired with above; single demo+helper scope) |
| IR fragment serialize/mutate sample (IRFunction / BasicBlock round-trip)     | **#1679** |
| Full nested-struct roundtrip test (including `vector<struct>` with non-trivial nested fields) | **#1677** (paired with #1676 since the recursive support is the precondition) |
| No-regression coverage on existing mutation tests                             | covered by predecessors + Phase 2 of #1676 |

## Phase 2+ execution recipe (for the follow-up maintainer)

1. Run `python3 scripts/audit_dead_bumps.py` to see how `bump_*` patterns
   in the C-linkage accessor surface (none today; this counter doesn't
   bump through `Evaluator::*`).
2. Verify `aura_reflect_nested_struct_throw_count_v_read()` returns
   > 0 under expected nested-struct workloads (the test in
   `tests/test_reflect_nested.cpp` exercises the static accessor).
3. Implement #1676 — recursive `reflect_members<T>` with depth limit
   (default `depth = 16`) + cycle detect via `std::unordered_set<const void*>`
   + auto-forwarding through the generic struct template.
4. Replace the throw + counter-bump at both sites with the recursive call:
   ```cpp
   case MemberKind::Struct: {
       if (depth > 16) break;  // guard
       if (!visited.insert(&obj).second) break;  // cycle detect
       auto sub_buf = auto_serialize<SubT>(buf, obj.*sub_ptr, depth + 1, visited);
       visited.erase(&obj);
       break;
   }
   ```
5. Add `tests/test_reflect_nested_roundtrip.cpp` exercising:
   - `struct S { int x; std::string name; }`
   - `struct Wrapper { S inner; }` (nested roundtrip)
   - `std::vector<Wrapper>` (vector of nested-struct)
6. Update `docs/design/1648-reflect-nested.md` to mark the Phase 2
   work as complete and link to the follow-up.

## Verification (this commit, Phase 1)

- **Pre-commit hooks:** clang-format `-i` + `--dry-run -Werror` clean on
  `src/reflect/reflect.hh`; ruff clean; test-includes linter — `scripts/check_test_includes.py`
  (the new `tests/test_reflect_nested.cpp` added); docs regen via `./build.py docs`.
- **Pre-push gates:** `scripts/check_ir_hygiene_full_pipeline_coverage.py`
  still 7/7 green (no #1644 / #1645 / #1646 / #1647 regression);
  `scripts/check_dead_bump_rate.py --self-test` passes.
- **Build:** object-compile-only verify per the recurring `arena.ixx.o`
  link-stage CI infra deadlock pattern (#1907 / #1908 / #1641 / #1644
  / #1646 same-day).

## Related issues (predecessors verified on `origin/main` post-rebase)

| Predecessor | What it shipped for static reflection                                |
|-------------|------------------------------------------------------------------------|
| #1679       | Cycle detection at the AST validate walk layer (the sibling mechanism for #1676's depth + cycle) |
| #1611       | `MacroIntroduced` hygiene gate schema (the EDSL→reflect bridge anchor) |
| #1907       | Reflect/EDSL bridge with runtime mutate + hygiene validation         |
| #1047       | Phase 1 hygiene / type / mutate safety (foundation under everything) |

## Phase 2+ follow-up queue (per design doc +3-4 issues)

| Issue | Description |
|-------|-------------|
| **#1676** | Full recursive `reflect_members<T>` (depth + cycle detect + auto-forwarding through generic struct template) — the main Phase 2 work |
| **#1677** | Full nested-struct roundtrip test (`vector<Wrapper>` where `Wrapper` has nested struct) — depends on #1676 |
| **#1678** | `reflect:serialize-node` + `reflect:mutate-member` EDSL primitives with Guard + marker stamp — depends on #1676 |
| **#1679** | IR fragment serialize/mutate sample (IRFunction / BasicBlock round-trip) — separate mini-scope |
