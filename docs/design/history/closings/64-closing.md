## Closing — Top 3 Critical Issues all addressed

This review's three flagged P0 issues each have their own tracked issue, and all three have landed fixes on `main`:

| #64 flag | Tracked issue | Fix commit | What landed |
|---|---|---|---|
| **#1** Incremental type checking失效 (`FlatAST::dirty_` 不读) | [#72](https://github.com/cybrid-systems/aura/issues/72) | `88f9202` | Incremental cache check now correctly rejects contained `TYPE_VAR`s. A resolved `TypeId` is reused only if it's concrete (not a type var that may have stale bindings). Dirty-flag propagation is wired through the cache. |
| **#2** Let-polymorphism禁用 (`is_poly` 永远 false, `list`/`map`/`filter` 退化为 `Dynamic`) | [#71](https://github.com/cybrid-systems/aura/issues/71) | `144cdae` | `TypeEnv::bind` auto-detects poly types (forall, ADTs, functors). `lookup` auto-instantiates with fresh type vars, so `(define id (lambda (x) x))` now gives `id` the scheme `∀a. a -> a` instead of `Dyn -> Dyn`. |
| **#3** Gradual coercion + Linear ownership unsoundness (`Dynamic → Linear` 绕过 ownership 检查) | [#74](https://github.com/cybrid-systems/aura/issues/74) + [#79](https://github.com/cybrid-systems/aura/issues/79) | `3971743` + `5f85371` | Ownership validation is now scope-aware (tracks `Linear` bindings across nested scopes, reports leaks). `is_coercible` no longer silently swallows real type errors; `Dynamic → Linear` is rejected at insertion time, not at runtime. |

## Also addressed in this batch (cross-references)

- **#70** `is_subtype` 真正实现(`445b7d3` + `04cc383` TypeId interning) — makes the subtyping checks that the gradual system relies on actually meaningful
- **#73** TypeId 流到 IR/JIT(`cc8171a` — 5 phases) — fixes the underlying "type_id was dropped at every step" problem
- **#75** ADT match exhaustiveness(`be2f6f0`) — fixes bare-identifier pattern detection
- **#76** `instantiate_forall` 用 fresh vars(`2005c69`)
- **#77** `substitute` capture avoidance(`7f3c862`)

## Verification

`bash tests/run-tests.sh` → 201/201 pass. The 145 benchmark tasks still run, and `minimax-m3` scores 85% (118/139) on the EDSL benchmark — type-system correctness is no longer blocking AI workflows.

Closing this issue as superseded by the completed #70-#79 batch.
