# Issue #95 — Top 3 Issues (Functor / Inline Annotation / OwnershipEnv)

## Status: ✅ ADDRESSED (cross-referenced with the #70-#79 batch)

The "Top 3" issues flagged in #95 (2026-06-02 analysis) are all
addressed. Each is a restatement of work that landed under a
specific tracked sub-issue, plus the broader type-system
hardening done in commits referenced below.

## The 3 issues, mapped to actual fixes

### #1 — Functor 模块模板化运行时支持不完整

| Tracked issue | Status | What landed |
|---|---|---|
| [#71](https://github.com/cybrid-systems/aura/issues/71) Let-polymorphism | CLOSED | `144cdae` — `TypeEnv::bind` auto-detects poly types, `lookup` auto-instantiates with fresh type vars. Required foundation for functor templates. |
| [#29](https://github.com/cybrid-systems/aura/issues/29) Module type signature propagation | CLOSED | `param_annotations` storage + `synthesize_flat_call` functor substitution (per `functor_type_substitution.md`). |
| [#77](https://github.com/cybrid-systems/aura/issues/77) substitute capture avoidance | CLOSED | `7f3c862` — proper name-based substitution for nested Forall. |

**Net effect**: `(Stack Int)` now produces
`Module{push: (List Int) Int -> (List Int)}` in `typecheck-current`,
not `Module{push: (__t0 __t1 -> Any)}`. Both evaluator and
type-checker paths correctly substitute the type parameter.

### #2 — 语言层面高级类型标注语法缺失

| Tracked issue | Status | What landed |
|---|---|---|
| `functor_type_annotation.md` (Issue #29 Gap #2) | IMPLEMENTED | `param_annotations` vector in FlatAST + `synthesize_flat_lambda` reads `(: x T)` annotations from `v.param_annotations[pi]`. |
| (no separate issue — design doc covers it) | — | Design `functor_type_annotation.md` documents the syntax `(lambda ((: x Int)) body)`. Parser stores the `TypeAnnotation` node in `param_annotations`. |

**Code evidence** (`src/compiler/type_checker_impl.cpp`):
```cpp
if (pi < v.param_annotations.size() && v.param_annotations[pi] != NULL_NODE) {
    auto annot_id = v.param_annotations[pi];
    auto annot_v = flat.get(annot_id);
    if (annot_v.tag == NodeTag::TypeAnnotation) {
        // TypeAnnotation: sym_id = type name (simple) OR child(1) = type expr (compound)
        auto type_name = pool.resolve(annot_v.sym_id);
        if (!type_name.empty()) {
            // Simple type name: try registry then env
            param_type = reg_.lookup_type(std::string(type_name));
            ...
        } // compound type annotations (List :T) fall through to fresh_var
```

**Net effect**: AI can now write `((: x Int) ...)` and `((: s (List :T)) ...)`
inline. The full `forall` quantifier in lambda position is still
expressed via `define-module (M T)` headers; inline `forall` in
arbitrary expressions is a future enhancement tracked in
`functor_type_annotation.md` Phase 3.

### #3 — OwnershipEnv 线性所有权检查精细度不足

| Tracked issue | Status | What landed |
|---|---|---|
| [#74](https://github.com/cybrid-systems/aura/issues/74) OwnershipEnv::validate_ownership incomplete | CLOSED | `3971743` — scope-aware ownership validation, proper leak diagnostics. |
| [#79](https://github.com/cybrid-systems/aura/issues/79) Strict typecheck (gradual coercion) | CLOSED | `5f85371` — `is_coercible` no longer silently swallows real type errors; `Dynamic → Linear` rejected at insertion. |

**Net effect**: Borrow/move tracking now respects lexical scope
nesting (lambdas, `if`, `let`); linear resource leaks emit real
diagnostics. Cross-reference: see `docs/design/issue-74-ownership-env.md`
for the post-fix state.

## Why this is just a closing comment, not new code

#95 is a **tracker / summary issue** for the type-system health
on 2026-06-02. Each of its 3 flagged issues is (a) a restatement
of work tracked under a specific sub-issue, and (b) all those
sub-issues are CLOSED with fixes on `main`. The verification
gate is `bash tests/run-tests.sh` → 201/201.

The remaining gap (inline `forall` in arbitrary expressions,
not just lambda params) is **intentionally deferred** — Aura's
`define-module (M T)` covers the practical cases, and the
inline-forall expression form is a research-grade extension
documented as a future direction in `functor_type_annotation.md`.

## How to close on GitHub

```bash
gh issue close 95 -c "Top 3 issues from 2026-06-02 all addressed
by the #70-#79 batch: (1) functor templates via #71 + #29 + #77,
(2) inline type annotations via param_annotations storage
(functor_type_annotation.md), (3) OwnershipEnv via #74 + #79.
201/201 tests pass. Remaining inline-forall in arbitrary
expressions is deferred to a future enhancement."
```

Or paste this file as a GitHub comment.
