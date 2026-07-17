# reflect MacroIntroduced hygiene gate (#1611)

**Issue:** [#1611](https://github.com/cybrid-systems/aura/issues/1611)  
**Builds on:** #750 · #502 · #551 · #373 · #1609 · #1610  
**Status:** Production closed-loop (reflect.hh API + post_mutation gate + stats).

## Contract

| Layer | Behavior |
|-------|----------|
| `auto_validate_with_marker` | MacroIntroduced context requires `allow_macro_evolution` |
| `validate_mutation_reflect_health` | Optional hard reject via `enforce_macro_hygiene_reject` |
| `validate_deserialize_hygiene` | Phase-4 module deserialize payload marker gate |
| `post_mutation_reflect_validate` | Builds `MutationReflectHealth`; hard-rejects unclean MacroIntroduced without allow |
| `reflect:validate-macro-body` | Optional `:allow-macro?`; bumps check/reject counters |
| mutate path | Pre-existing `hygiene_protected_error` + `allow_macro_mutate_` |

## Metrics (`query:reflect-postmutate-stats`, schema **1611**)

| Key | Meaning |
|-----|---------|
| `reflect-macro-hygiene-checks` | post_mutation / validate-macro-body hygiene walks |
| `reflect-macro-hygiene-rejects` | rejects without allow |
| `allow-macro-mutate` | current global flag (0/1) |
| `hygiene-aware-validate-wired` | 1 |
| `post-mutation-macro-check-wired` | 1 |
| `deserialize-hygiene-wired` | 1 |
| `schema` | **1611** |

No new public `query:*-stats` (#1448 freeze) — folded into reflect-postmutate.

## Tests

| File | Role |
|------|------|
| `tests/test_reflect_macro_hygiene_1611.cpp` | **#1611** AC |
| `tests/test_reflection_runtime_validate_macro_edsl_mutate.cpp` | #750 lineage |
| `tests/test_reflect_postmutate_guard_snapshot.cpp` | #502 lineage |

## Related

- `docs/design/query-pattern-hygiene-1609.md`
- `docs/design/ir-hygiene-propagation-1610.md`
