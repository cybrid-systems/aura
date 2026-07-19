# Issue #1655 — apply_closure epoch-stale helper extraction (single source of truth for closure_epoch_mismatch_fallback gate)

## 来源

Memory safety review of #1485 (2026-07-17) + 2026-07-19 production
audit. Building on the existing infrastructure laid by #1485 (C1 stale-
closure defense in depth: `closure_needs_safe_fallback` helper + inline
race-window path), #1660 (unified `closure_is_epoch_or_env_stale`
public helper), #1478 / #1626 (linear post-mutate third arm with
distinct metric), and #1632 (live-closure stale-prevented metric
parity).

## 问题描述

`closure_needs_safe_fallback` (`src/compiler/evaluator_eval_flat.cpp:142-208`)
carefully tracks `bool epoch_or_env_stale` inline and only bumps
`closure_epoch_mismatch_fallback` when `bridge_epoch` OR `env_frame` was
the specific cause:

```cpp
if (epoch_or_env_stale)
    ev.bump_closure_epoch_mismatch_fallback();
```

A late invariant check (`if (ev.closure_is_epoch_or_env_stale(cl))`)
was added per #1660 to guarantee the inline tracking matched the
public predicate.

But the inline race-window path in `apply_closure` (#1485 C1, #1558,
#1632, ~line 459+) — the post-`materialize_call_env` dual-check
designed to catch mutations racing the helper call — inlines the
same bridge/env checks and bumps `closure_epoch_mismatch_fallback`
**unconditionally** inside the `if (bridge_stale || env_stale)` body:

```cpp
if (bridge_stale || env_stale) {
    // ... per-cause bumps ...
    bump_stale_closure_prevented();
    bump_closure_epoch_mismatch_fallback();  // unconditional — bug
    bump_compiler_live_closure_stale_prevented();
}
```

The metric's contract (per `observability_metrics.h:222+`) is
specifically "lifetime count of safe-fallback paths after a
bridge_epoch / defuse_version_ mismatch — distinct from linear-stale
fallbacks". The unconditional bump in the race-window path loses
this semantic split. While today's race-window path doesn't check
linear (only bridge OR env), the moment someone broadens the
predicate (e.g. adds a linear arm to handle linear-only stale
captures under steal), the metric is silently inflated for any
non-epoch stale caught here.

## 代码证据 (code anchors)

### 修复前

```cpp
// src/compiler/evaluator_eval_flat.cpp — pre-#1655 (helper)
static bool closure_needs_safe_fallback(const Evaluator& ev, const Closure& cl,
                                        CompilerMetrics* m) {
    bool stale = false;
    bool epoch_or_env_stale = false;          // duplicate tracking
    const auto cur_epoch = ev.current_bridge_epoch();
    if (ev.is_bridge_stale(cl.bridge_epoch, cur_epoch)) {
        stale = true;
        epoch_or_env_stale = true;            // dup assignment
        // ... per-cause bridge bumps ...
    }
    if (cl.env_id != NULL_ENV_ID) {
        if (ev.is_env_frame_invalid(cl.env_id) || ev.is_env_frame_stale(cl.env_id)) {
            stale = true;
            epoch_or_env_stale = true;        // dup assignment
            // ... per-cause env bumps ...
        }
        if (!ev.linear_post_mutate_enforce(cl.env_id)) {
            stale = true;
            // no epoch_or_env_stale = true (linear-only — correct)
            // ... linear bumps ...
        }
    }
    // #1660 invariant: redundant after #1655 extraction.
    if (ev.closure_is_epoch_or_env_stale(cl)) {
        stale = true;
        epoch_or_env_stale = true;            // third place!
    }
    if (stale) {
        // ...
        if (epoch_or_env_stale)               // gate
            ev.bump_closure_epoch_mismatch_fallback();
    }
    return stale;
}

// src/compiler/evaluator_eval_flat.cpp — pre-#1655 (inline race-window)
{
    const auto cur_epoch = current_bridge_epoch();
    const bool bridge_stale = is_bridge_stale(cl_copy.bridge_epoch, cur_epoch);
    const bool env_stale =
        cl_copy.env_id != NULL_ENV_ID &&
        (is_env_frame_invalid(cl_copy.env_id) || is_env_frame_stale(cl_copy.env_id));
    if (bridge_stale || env_stale) {
        if (metrics) { /* ... per-cause bumps ... */ }
        bump_stale_closure_prevented();
        bump_closure_epoch_mismatch_fallback();   // <-- unconditional bug
        bump_compiler_live_closure_stale_prevented();
        // ...
    }
}
```

### 修复后 (Issue #1655 hardened — Option B per issue recommendation)

```cpp
// src/compiler/evaluator_eval_flat.cpp — post-#1655 (helper, file-local)
static bool closure_is_epoch_stale(const Evaluator& ev, const Closure& cl) noexcept {
    if (ev.is_bridge_stale(cl.bridge_epoch, ev.current_bridge_epoch()))
        return true;
    if (cl.env_id != NULL_ENV_ID &&
        (ev.is_env_frame_invalid(cl.env_id) || ev.is_env_frame_stale(cl.env_id)))
        return true;
    return false;
}

static bool closure_needs_safe_fallback(const Evaluator& ev, const Closure& cl,
                                        CompilerMetrics* m) {
    bool stale = false;
    const auto cur_epoch = ev.current_bridge_epoch();
    if (ev.is_bridge_stale(cl.bridge_epoch, cur_epoch)) {
        stale = true;
        if (m) { /* ... per-cause bridge bumps ... */ }
    }
    if (cl.env_id != NULL_ENV_ID) {
        if (ev.is_env_frame_invalid(cl.env_id) || ev.is_env_frame_stale(cl.env_id)) {
            stale = true;
            if (m) { /* ... per-cause env bumps ... */ }
        }
        if (!ev.linear_post_mutate_enforce(cl.env_id)) {
            stale = true;
            if (m) { /* ... linear bumps ... */ }
        }
    }
    if (stale) {
        // ...
        // #1655: gate on the single-source-of-truth predicate.
        if (closure_is_epoch_stale(ev, cl))
            ev.bump_closure_epoch_mismatch_fallback();
    }
    return stale;
}

// src/compiler/evaluator_eval_flat.cpp — post-#1655 (inline race-window)
{
    const bool env_stale =
        cl_copy.env_id != NULL_ENV_ID &&
        (is_env_frame_invalid(cl_copy.env_id) || is_env_frame_stale(cl_copy.env_id));
    if (closure_is_epoch_stale(*this, cl_copy)) {     // #1655 single source
        if (metrics) {
            /* ... per-cause bumps ... */
            if (env_stale)                              // envframe per-cause bump
                metrics->compiler_closure_envframe_stale_total.fetch_add(
                    1, std::memory_order_relaxed);
            /* ... */
        }
        bump_stale_closure_prevented();
        bump_closure_epoch_mismatch_fallback();         // now naturally gated
        bump_compiler_live_closure_stale_prevented();
        // ...
    }
}
```

## 精确改动位置 (file-by-file)

1. **src/compiler/evaluator_eval_flat.cpp** — 3 sites:
   - New file-static helper `closure_is_epoch_stale` (above
     `closure_needs_safe_fallback`, ~line 134).
   - `closure_needs_safe_fallback` refactor: removed `bool
     epoch_or_env_stale` local + 3 `epoch_or_env_stale = true`
     assignments + the late `ev.closure_is_epoch_or_env_stale(cl)`
     invariant check; gate now uses `closure_is_epoch_stale(ev, cl)`.
   - Inline race-window path refactor: replaced `bridge_stale || env_stale`
     if-gate with `closure_is_epoch_stale(*this, cl_copy)`;
     `bridge_stale` local removed; `env_stale` local retained for
     the per-cause envframe metric bump.

2. **tests/test_issue_1655.cpp** (new, ~280 lines, 10 ACs source-driven
   + runtime baseline).

3. **scripts/check_apply_closure_epoch_stale_coverage.py** (new, ~150
   lines, 10 ACs linter).

4. **CMakeLists.txt** — add `aura_add_issue_test(test_issue_1655)` +
   `aura_issue_test_link_llvm_jit(test_issue_1655)` entries.

## 验收标准 (AC)

| AC | Verification |
| --- | --- |
| AC1 | `test_issue_1655` AC1: file-static `closure_is_epoch_stale` helper exists at file scope in `evaluator_eval_flat.cpp` |
| AC2 | AC2: helper checks bridge_epoch via `ev.is_bridge_stale(cl.bridge_epoch, ev.current_bridge_epoch())` |
| AC3 | AC3: helper checks env_frame stale via `ev.is_env_frame_invalid || ev.is_env_frame_stale` (guarded by `env_id != NULL_ENV_ID`) |
| AC4 | AC4: helper default branch returns `false` (no-stale case) |
| AC5 | AC5: `closure_needs_safe_fallback` uses `closure_is_epoch_stale` for the gate; previous inline `bool epoch_or_env_stale` tracking removed |
| AC6 | AC6: late `ev.closure_is_epoch_or_env_stale(cl)` invariant check removed (#1660 redundant after #1655 extraction) |
| AC7 | AC7: inline race-window path uses `closure_is_epoch_stale(*this, cl_copy)` for if-gate; previous inline `bridge_stale` local removed |
| AC8 | AC8: `bump_closure_epoch_mismatch_fallback` appears in both gated blocks (helper + race-window) — never unconditional |
| AC9 | AC9: ≥3 `Issue #1655` rationale comments present (helper + caller + race-window call site) |
| AC10 | AC10: cross-baseline — `CompilerService` (set-code) + (eval-current) round-trip still works after the #1655 wire-up |

10 linter ACs in `scripts/check_apply_closure_epoch_stale_coverage.py`:
source-driven production-code checks mirroring AC1–AC9 plus the
runtime-baseline test file presence.

## 预期收益

- **Closes the metric-inflation bug** in the inline race-window path:
  `closure_epoch_mismatch_fallback` is now gated on the same single-
  source-of-truth predicate as the helper path. The metric's contract
  (epoch-vs-env-vs-linear split per #1485 C1 / #1660) is preserved
  end-to-end across both `apply_closure` paths.
- **Eliminates duplicate inline tracking** in `closure_needs_safe_fallback`:
  the previous `bool epoch_or_env_stale` local + 3 assignments + late
  invariant check is replaced by a single `closure_is_epoch_stale(ev, cl)`
  call at the gate. ~10 lines of redundant bookkeeping gone.
- **Prevents future drift**: if the "epoch-stale" predicate ever evolves
  (e.g. adds a linear arm to handle linear-only stale captures under
  steal — currently out of scope but architecturally anticipated), both
  call sites stay consistent because they share the same helper.
- **Co-located source of truth**: the helper lives in the file where it's
  used (next to both call sites), not as a member of `Evaluator`. The
  public `Evaluator::closure_is_epoch_or_env_stale` member (added per
  #1660) remains the canonical Evaluator-API predicate; the new
  file-local helper is the canonical metric-gate predicate for this file.
- **Builds on #1660 + #1485**: zero behavior change for the existing
  helper path (same gate, same per-cause bumps). Pure refactor +
  correctness fix for the inline race-window path. No API change.

## 优先级

**P1** (correctness — metric contract preservation; future-drift
prevention; defense in depth)

## 标签

P1, evaluator, apply_closure, metric, race-window, single-source-of-truth,
memory-safety, future-drift-prevention

## 相关 Issues

- #1485 (C1 stale-closure defense in depth — introduced both the helper
  and the inline race-window path)
- #1660 (unified `closure_is_epoch_or_env_stale` public helper)
- #1478 / #1626 (linear post-mutate third arm — distinct metric)
- #1632 (live-closure stale-prevented metric parity)
- #1509 (concurrent steal/mutate race surface)
- #1525 (multi-fiber mutate↔eval race counter)
- #1558 (race-window dual-check post-materialize)
- #1287 (race-window for flat*/pool* dangling case)

## 验证方式

- `tests/test_issue_1655.cpp`: 10 ACs all green (source-driven + runtime
  baseline)
- `scripts/check_apply_closure_epoch_stale_coverage.py`: 10 ACs all green
- pre-commit hooks: clang-format clean, `./build.py docs` regen clean,
  `gen_docs.py --check` verify clean
- pre-push gates: primitive surface freeze (no new primitives added —
  existing observability metrics surface used within 521 budget per
  #1734 raise) + test-registry (#1572) + test binding + coverage (#1453)
- Same PR cycle as #1638 / #1639 / #1640 / #1641 / #1654: edit → build
  → run tests → descriptive commit → push `main` (direct, no PR review
  per MEMORY.md workflow).