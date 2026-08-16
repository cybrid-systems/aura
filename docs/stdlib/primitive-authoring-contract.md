# Primitive authoring contract (Issue #2915)

Stable contract for **humans and AI Agents** that add C++ Aura primitives.
Follow this document; do **not** invent a new registration style.

## Canonical scaffolding

| Piece | Location |
|-------|----------|
| Official helper | `src/compiler/prim_registrar_scaffold.hh` → `register_prim` + `PrimSpec` |
| Render hot-path helper | Retired with tui:* (#2626). Do not add new render-hot prims. |
| Meta macros (legacy/compatible) | `DEFINE_PRIMITIVE_META` / `DEFINE_PRIMITIVE_META_SECURE` in `primitives_detail.h` |
| Capture / error helpers | `PRIM_ERROR`, `PRIM_CAPTURE_*` in `primitives_detail.h` |
| Central boot map | `src/compiler/evaluator_primitives_registry.cpp` (#1552) |
| Error-return convention | [primitive-error-convention.md](primitive-error-convention.md) (#2914) |

## Minimal registration (proof pattern)

```cpp
#include "prim_registrar_scaffold.hh"

void register_misc_primitives(PrimRegistrar add, Evaluator& ev) {
    using aura::compiler::register_prim;
    using aura::compiler::pure_general;

    register_prim(add, ev, "current-time",
        [](const auto&) -> EvalValue {
            return make_int(static_cast<std::int64_t>(std::time(nullptr)));
        },
        pure_general(0, "() -> int",
                     "Wall-clock seconds since Unix epoch."));
}
```

Factories: `pure_general`, `io_general`, `mutate_general` (all in the scaffold header).

## Rules Agents must keep

1. **One style** — Prefer `register_prim(...)`. Do not add ad-hoc `add` + custom meta maps in new TUs. `register_render_hot_prim` is retired (#2626).
2. **Central registry** — Every new `register_*_primitives` group is called from `Evaluator::register_all_primitives()`.
3. **PrimMeta auto-effects** — Leave `PrimSpec.required_effects == 0` so issue **#2152** infers from the name (`mutate:`, `file:`, …). Set explicit bits only when the name does not encode the effect.
4. **Hot table** — Set `perf_tier = kPrimPerfHot` only when the prim is on a trusted hot path; `finalize_hot_table()` (Evaluator ctor) rebuilds `hot_map_` after all registrations. Scaffold never bypasses that path.
5. **Errors** — True failures return `make_primitive_error` / structured `make_merr`, not silent `#f` / void (see error convention).
6. **Discovery** — After landing, names appear in `query:primitives-meta` / `primitive:describe` / generated `docs/generated/primitives.md`. Do not invent parallel discovery APIs without updating the #1552 registry comment block.
7. **Tests** — Suite or unit test for behavior + update coverage gate if the issue adds a linter.

## Discovery surfaces (unchanged)

- `(require "std/primitives" all:)` → `primitives:help` / `:list` / `:discover`
- `(primitive:describe name)`
- `(query:primitives-meta)` / `(query:primitives-meta-catalog)`
- `(query:primitive-list-with-meta)`
- `docs/generated/primitives.md` + `docs/generated/primitives-registry.md`

## Proof migration

`evaluator_primitives_misc.cpp` uses `register_prim` for `current-time`, `current-time-ms`, `monotonic-ms`, and `arena-offset` (Issue #2915).

## Core TU migration (Issue #2996)

These commercial hot-path TUs are **migrated** onto `register_prim` + `PrimSpec` (non-empty `schema` + `doc` on every name). Do not reintroduce bare `add(...)` without PrimMeta in them:

| TU | Group |
|----|--------|
| `evaluator_primitives_list.cpp` | list / list? / null? / length / list-ref / member / append / reverse / map / filter / take / drop / foldl / list-sort |
| `evaluator_primitives_math.cpp` | math + regex + arithmetic (+ gated m4-*) |
| `evaluator_primitives_json.cpp` | json-encode / json-parse / json-get-string |
| `evaluator_primitives_pair.cpp` | pair + string core constructors |
| `evaluator_primitives_vector.cpp` | vector + hash core |

`g_register_prim_scaffold_total` counts every `register_prim` stamp. Discovery: `query:primitives-meta` / `primitive:describe`.
