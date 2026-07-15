# CHANGELOG — v2.0 prep (surface reduction)

## [v2.0-prep] — 2026-07-14

### Issue #1440 (P5b) — Convenience taxonomy + stdlib honesty

**What the issue asked for:** remove ~170 `string-*` / `json-*` / `math-*` / `vector-*` / `path-*` / `time-*` C++ `add()` registrations because “stdlib already implements them.”

**What the tree actually had:**

| Fact | Detail |
|------|--------|
| Registered names matching those prefixes | **~20** (not 170) |
| `path-*` / `time-*` / `math-*` C++ regs | **0** (path/time live in `lib/std`; math are bare `floor`/`sin`/…) |
| Gen-docs “convenience: 170” | Heuristic: any hyphenated bare name → convenience (mislabeled FFI `c-*`, `file-*`, hot-path string/vector, …) |
| `lib/std/string|math|json` | **Compose on** C++ hot-path prims (`string-append`, `string-ref`, `json-parse`, `floor`, …) — not full pure rewrites |

**Shipped in this change:**

1. **`scripts/gen_docs.py` taxonomy fix** — longest-prefix classification; no default “hyphen ⇒ convenience”; hot-path string/vector/json/hash → **core**; host `c-*`/`file-*`/`git-*` → **mutation-safety**.
2. **`docs/primitive_categories.yaml`** — explicit #1440 core pins for string/vector/json/math builtins.
3. **`lib/std/json.aura`** — fixed broken `builtin:json-encode` / `builtin:json-parse` wrappers; `(json-stringify)` → `(json-encode)`; parse stays the engine prim.
4. **Docs regen** — `convenience` category **170 → ~8** (product wrappers like `auto-evolve-*`, `format`); string/json/vector no longer listed as convenience.

**Not done (blocked by red-lines — needs a follow-up epic):**

- Deleting `add("string-append")` / `string-length` / `string-ref` / `vector-*` / `json-parse` — would break boot, stdlib, and Agent loops.
- A full rename to `builtin:string-append` with stdlib-only public names (possible later; large migration).

**Agent guidance:** prefer `(require "std/surface" all:)` / `std/string` / `std/json` / `std/math` for product APIs; engine keeps hot-path prims.

---

### Issue #1439 (P5a) — Public `query:*-stats` removal (v2.0-prep) — *in progress on same branch*

**Breaking:** `query:*-stats` / `compile:*-stats` removed from the **public** primitive registry (not in `(api-reference)`).

**Replacement:**

```scheme
(engine:metrics "query:pattern-stats")   ; by-name compat hash
(engine:metrics)                         ; nested CompilerMetrics groups (schema 2)
(engine:metrics :group "jit")
(engine:metrics :prefix "query:")
(engine:metrics :all)                    ; every legacy stats hash (expensive)
```

Full guide: [migration-stats-to-metrics.md](migration-stats-to-metrics.md).

**Implementation notes:**

- Hash builders remain as `ObservabilityPrims::register_stats_impl` (internal table).
- `prim_registrar()` does not publish `query:`/`compile:*-stats` into `Primitives`.
- Tests use `(engine:metrics "…")` and `aura::test::aura_call_expr`.

---

### Prior surface work (already on main)

| Issue | Change |
|-------|--------|
| #1432 | Surface freeze gate (no new stats/convenience/ref names) |
| #1433 | `(engine:metrics)` schema 2 + groups |
| #1434 | Top-20 deprecate + migrate |
| #1435–#1437 | `(query\|mutate\|workspace :op …)` dispatchers |
| #1438 | Docs canonical op-dispatch |
