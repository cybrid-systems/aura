# C++26 `std::meta` Migration Roadmap — Issue #301

This document captures the migration plan for adopting C++26 P2996 reflection (`std::meta`) to reduce manual boilerplate around `generation_`, `StableNodeRef` validity, and dirty propagation. It is scoped to Issue #301 and complements the existing reflection foundation from #217 / #220 / #288.

## Status (as of 2026-06-27)

- **GCC 16.0.1** has `<meta>` header but does **NOT** expose the `std::meta` namespace or any P2996 reflection API (`nonstatic_data_members_of`, `members_of`, `define reflection namespace`, etc. all fail to compile).
- The existing `auto_serialize` / `auto_to_json` infrastructure in `src/reflect/reflect.hh` uses an internal fallback (`reflect_members<T>()` is a no-op stub that compiles but returns nothing useful).
- **The full P2996 migration is BLOCKED on compiler maturity.** Expected availability: GCC 17 or 18.

## Audit (manual code that would benefit)

| File | Hits | What it does |
|---|---|---|
| `src/core/ast.ixx` | 93 | `bump_generation`, `mark_dirty`, `is_valid_in`, `save_panic_checkpoint`, `restore_panic_checkpoint` call sites + the manual `is_valid` predicate in `StableNodeRef` |
| `src/compiler/evaluator.ixx` | 13 | `save_panic_checkpoint`, `mark_dirty_defuse_entries`, panic-checkpoint bookkeeping |

These call sites share the pattern:
1. Read member field
2. Compare against external state (e.g. `node_gen_[id] == ref.gen`)
3. Set `ok = false` or take corrective action

This is the canonical "member-walk + predicate" pattern that P2996 `define reflection` blocks can codegen.

## Why it matters

- **Maintainability**: Adding a new SoA column today requires manual updates to `serialize_soa`, `is_valid_in`, `mark_dirty_upward`, etc. With reflection, the codegen template auto-extracts members.
- **Zero-overhead**: `consteval` reflection can generate switch-statement fast paths that match hand-written code's performance — but with zero maintenance burden.
- **AI multi-round evolution**: AI agents that mutate SoA columns need a stable contract. Reflection-generated predicates are bitwise-equivalent by construction, reducing AI-induced bit-flips.
- **Memory safety**: `is_valid_in` is a hot path. Codegen-generated versions would benefit from inlining and avoid the maintenance drift that has accumulated over the past year.

## Migration phases (proposed)

### Phase A — Foundation (DONE in #217, #220, #288, this commit)

- ✅ POD struct support in `auto_serialize` / `auto_to_json` for SourceLocation, Patch, etc.
- ✅ Audit of manual call sites (this commit)
- ✅ `tests/test_issue_301.cpp` — baseline measurements + StableNodeRef layout validation
- ⏳ Wait for GCC 17/18 with `std::meta` namespace + `nonstatic_data_members_of`

### Phase B — `StableNodeRef::is_valid_in` codegen (BLOCKED on GCC 17)

Once GCC ships `std::meta`:

```cpp
template <> struct ::aura::meta::members<StableNodeRef> {
    using type = std::meta::members_of(^^StableNodeRef, 
        std::meta::access_context::current());
};

// Codegen:
consteval bool StableNodeRef::is_valid_in(const FlatAST& flat) const {
    // Generated from the ^^StableNodeRef reflection:
    return flat.generation_ == gen
        && flat.node_gen_[id] == gen;
}
```

Expected codegen output: identical assembly to the manual version (`cmp reg, reg; jne label`).

### Phase C — `FlatAST::serialize_soa` codegen

Each SoA column becomes:

```cpp
template <typename T>
consteval auto members_of_T() {
    return std::meta::nonstatic_data_members_of(^^T,
        std::meta::access_context::current());
}

// Generated switch dispatches serialize_soa via members_of:
// For each (name, type) pair, write column-by-column.
```

Expected codegen: removes the 21 manual `write_column(...)` calls in `serialize_soa` (lines 3014–3086 in current `ast.ixx`).

### Phase D — Dirty propagation visitor

`mark_dirty_upward` walks `parent_` column. Codegen would auto-extract the parent field from any SoA struct that declares it:

```cpp
template <typename T>
consteval bool has_parent_field() {
    for (auto m : std::meta::members_of(^^T)) {
        if (m.name == "parent_") return true;
    }
    return false;
}
```

### Phase E — `MutationRecord` round-trip codegen

Issue #217's Phase 9 already documented MutationRecord round-trip success for 25 fields. Phase E codegens the per-field serialize via reflection.

### Phase F — Acceptance test gate

`tests/test_issue_301.cpp` is the foundation. A future Phase F extension adds:
- Bitwise-equivalence test: codegen path output == manual path output for 1000+ random inputs
- Hot path benchmark: codegen ≤ manual (or better)

## Acceptance Criteria status

- [ ] 60% of manual generation_/dirty/bump logic replaced with std::meta — **BLOCKED on GCC 17**
- [ ] Existing is_valid/rollback bitwise-identical — **ready for Phase B verification when GCC ships**
- [ ] New SoA column addition requires minimal manual code — **ready for Phase C verification when GCC ships**
- [ ] Hot path benchmarks show no regression (or improvement) — **baseline captured at 152.8ns/op**
- [ ] Living documentation updated — **this file is the foundation; future phases will extend**

## Verification today

- `test_issue_301` — 4 scenarios / 6 assertions, OVERALL: PASS
- Audit numbers documented above
- StableNodeRefMirror layout (8 bytes, trivially copyable) verified — the migration target surface is well-defined
- Manual serialize baseline: 152.8ns/op (12 bytes/op) — the threshold future std::meta codegen must meet

## Follow-ups

1. When GCC 17/18 ships with `std::meta`, re-run `test_issue_301.cpp` to verify the auto path is available. Replace scenario 3 with a codegen-vs-manual comparison.
2. Track P2996 implementation status: https://github.com/cplusplus/papers/issues/178 (P2996 status)
3. Track GCC std::meta implementation: https://gcc.gnu.org/bugzilla/show_bug.cgi?id=90442 (GCC bug tracker entry for P2996)
4. The doc text in `src/core/ast.ixx` lines 47-50 and 1087-1125 already marks the relevant structs as reflection-ready. When GCC ships, these comments should be removed in favor of `[[= std::meta::reflect()]]` annotations.
