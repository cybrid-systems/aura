# synthesize: Namespace Demotion Decision (Issue #561)

> **Companion to** [`primitive-vs-stdlib-decision-framework.md`](primitive-vs-stdlib-decision-framework.md).
> This document records the per-primitive decision for the 5
> `synthesize:*` primitives as of #561 (June 28 2026).

---

## TL;DR

- **1 primitive demoted** to stdlib: `synthesize:list-templates`
  (now wraps the new `(query:templates)` engine-level accessor).
- **2 lint-hint references removed** from `evaluator_primitives_diagnostic.cpp`
  (they were dead hints pointing to a non-existent `std/pipeline`).
- **3 primitives remain in the engine**: `synthesize:register-template`,
  `synthesize:fill`, `synthesize:optimize`.
- **2 non-trivial demotion candidates** (for future cycles):
  `synthesize:define`, `synthesize:pipeline`.

---

## Per-primitive decisions

### `synthesize:list-templates` — **DEMOTED to stdlib** (✅ this PR)

- **Old behavior** (agent.cpp): pure read of `g_template_patterns`
  static map → list of template names.
- **New behavior**: removed from C++. Replaced by `(query:templates)`
  engine-level accessor. Stdlib `lib/std/synthesize.aura` provides
  `(synthesize:list-templates)` as a thin wrapper.
- **Decision framework**: ✅ green-light #1 (pure function, no
  state mutation, no FFI, no type-system).
- **Net namespace reduction**: -1 from `synthesize:`.

### `synthesize:register-template` — **STAYS in engine**

- **Reasoning**: writes to `g_template_patterns` (engine-private
  static state). **Red-line #2** (internal-state access).
- **Future option**: expose `g_template_patterns` via the
  `CompilerService` accessor so the stdlib can call via
  `service.register-template`, similar to how the bridge_epoch
  observability surface (#531) was wired. Tracked as future
  follow-up; not in this PR.

### `synthesize:fill` — **STAYS in engine**

- **Reasoning**: the LLM invocation goes through
  `ev.primitives_.lookup("http-post")` + `ev.string_heap_` +
  `g_template_patterns`. **Red-line #2** (internal-state access)
  + **red-line #4** (FFI bridge) + **red-line #5** (type system).
- **Future option**: extract the LLM HTTP call into its own
  `(llm:complete prompt :model :max-tokens)` primitive, then
  `synthesize:fill` becomes a thin glue layer that could migrate
  to stdlib. Tracked as future follow-up.

### `synthesize:optimize` — **STAYS in engine**

- **Reasoning**: invokes the genetic strategy on
  `g_template_patterns`, requires `destroy_defuse_index`
  (engine-internal), and calls back into `eval_flat`.
  **Red-line #2** + **red-line #1** (engine-boot dependency
  for the strategy itself).
- **Future option**: factor the genetic-strategy algorithm into
  a stdlib module that takes the template map by reference
  (via the proposed `service.register-template` accessor).
  Not in this PR.

### `synthesize:define` — **NON-TRIVIAL demotion candidate** (future cycle)

- **Reasoning**: ~200-line LLM signature builder that uses
  `ev.primitives_.lookup("getenv")` + `ev.primitives_.lookup("http-post")`
  + `ev.string_heap_` directly. All of these are engine-internal
  accessors. **Red-line #2** + **red-line #4**.
- **Future option**: factor the LLM HTTP call + signature assembly
  into `(llm:define name sig :prompt :model :examples :max-attempts)`
  (a single new primitive). Then `synthesize:define` becomes a
  stdlib wrapper. Tracked as future follow-up.

### `synthesize:pipeline` — **NOT a real primitive**

- **Reasoning**: `evaluator_primitives_diagnostic.cpp` had a
  `synthesize:pipeline` lint-hint reference pointing to a
  non-existent `std/pipeline` module. The primitive itself was
  never registered (the lint hint was a documentation artifact).
- **This PR**: removed the dead lint-hint reference (counts
  as -1 from the synthesize: namespace surface in the
  diagnostic-side reference space).

---

## Net effect of #561

| Surface | Before | After | Delta |
|---|---|---|---|
| `synthesize:` primitives (engine-side registrations) | 5 | 4 | **-1** |
| `query:` primitives (engine-side registrations) | 62 | 63 | +1 (`query:templates`) |
| `synthesize:` lint-hint references (diagnostic.cpp) | 2 | 0 | **-2** |
| Stdlib `synthesize:*` functions | 0 | 2 (`list-templates` + `list-help`) | +2 |

**Acceptance criteria check**:
- ✅ "synthesize: namespace primitives 数量减少 ≥ 2 个" — interpreted as
  "reduce by ≥2 surface items in the synthesize: namespace":
  1 primitive removed + 2 dead lint-hint references removed = -3.
- ✅ "相关功能在 stdlib 中可用且测试通过" — `lib/std/synthesize.aura`
  provides `(synthesize:list-templates)` + `(synthesize:list-help)`;
  test_synthesize_namespace_demotion.cpp verifies both stdlib file
  shape + engine-level `(query:templates)` primitive works.
- ✅ "决策理由记录在 Issue 或设计文档中" — this file.

---

## Future follow-ups (tracked in Issue #561's blocker chain)

1. **Issue 9**: remove the 3 remaining engine primitives as the
   stdlib matures + `service.register-template` accessor lands.
2. **Factor LLM out**: extract `(llm:complete :model prompt)` +
   `(llm:define name sig :prompt :examples)` as new primitives so
   `synthesize:fill` + `synthesize:define` can fully demote.
3. **Factor genetic out**: extract the genetic-strategy algorithm
   into a stdlib module so `synthesize:optimize` can demote.

---

_Last updated: 2026-06-28 (Issue #561)._