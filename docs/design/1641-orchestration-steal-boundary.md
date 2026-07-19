# Issue #1641 — Scheduler/Worker work-stealing awareness for YieldReason::MutationBoundary

## 来源
Runtime 多 fiber 并发 + Mutation 安全 生产级 Code Review (2026-07-16)
+ Scheduler/fiber 协调 gap 分析 (2026-07-19). Building on the existing
infrastructure laid by #1633 (nested MutationBoundary steal defer),
#1492 (orchestration steal boost),#1445 (orchestration boost),
#1270 (threshold boost after repeated defers),
#1483 (adaptive safepoint coordination).

## 问题描述
`fiber.cpp` 已实现 `YieldReason::MutationBoundary` (轻量标记,
不实际 yield),`check_gc_safepoint` 在持 boundary 时 defer + 计数
`safepoint_wait_while_mutation_held`,`is_stealable()` 语义倾向
允许 steal 以支持 orchestration,`g_fiber_yield_mutation_boundary`
hook 存在。`worker.cpp::WorkerThread::steal()` 已 wire
inner boundary defer + `apply_starvation_mitigation` (paired with
`bump_steal_inner_mutation_boundary_deferred()`)。但：

- 跨 fiber 的 safe-steal observability 缺乏 per-CompilerMetrics
  surface(`bump_cross_fiber_mutation_safe_steal` 是 per-Fiber
  atomic,不是 per-CompilerMetrics)。
- inner boundary block 触发 defer + mitigation 时,缺 per-CompilerMetrics
  对应的 counter(目前只有 per-Fiber `bump_steal_inner_mutation_boundary_deferred`)。
- `scheduler.cpp` 触发 `apply_starvation_mitigation(f)` 时,缺
  per-CompilerMetrics surface。

三个 gap 的 closed-loop observability surface = 3 个新 metrics
(`steal_mutation_boundary_deferred_total` +
 `starvation_mitigated_for_boundary_count` +
 `boundary_held_steal_safe_total`),分别 wire 在 3 个 call site。

## 代码证据 (code anchors)

### 修复前

```cpp
// serve/worker.cpp WorkerThread::steal() — pre-#1641
if (stolen->last_yield_reason() == YieldReason::MutationBoundary) {
    stolen->bump_cross_fiber_mutation_safe_steal();  // per-Fiber only
}
// ...
if (stolen->is_at_inner_mutation_boundary()) {
    stolen->bump_steal_inner_mutation_boundary_deferred();  // per-Fiber only
    apply_starvation_mitigation(stolen);  // no per-CompilerMetrics bump
}
// ...
// serve/scheduler.cpp — pre-#1641
apply_starvation_mitigation(f);
// no per-CompilerMetrics bump
```

### 修复后 (Issue #1641 hardened)

```cpp
// serve/worker.cpp WorkerThread::steal() — post-#1641
if (stolen->last_yield_reason() == YieldReason::MutationBoundary) {
    stolen->bump_cross_fiber_mutation_safe_steal();
    if (auto* ev = Evaluator::yield_hook_evaluator())
        ev->bump_boundary_held_steal_safe_total();  // #1641
}
// ...
if (stolen->is_at_inner_mutation_boundary()) {
    stolen->bump_steal_inner_mutation_boundary_deferred();
    apply_starvation_mitigation(stolen);
    if (auto* ev = Evaluator::yield_hook_evaluator()) {
        ev->bump_steal_mutation_boundary_deferred_total();  // #1641
        ev->bump_starvation_mitigated_for_boundary_count();  // #1641
    }
}
// serve/scheduler.cpp — post-#1641
apply_starvation_mitigation(f);
if (auto* ev = Evaluator::yield_hook_evaluator())
    ev->bump_starvation_mitigated_for_boundary_count();  // #1641
```

## 精确改动位置 (file-by-file)

1. **src/serve/worker.cpp** — 2 wire-up sites in `WorkerThread::steal()`:
   - `boundary_held_steal_safe_total` after `bump_cross_fiber_mutation_safe_steal`
   - `steal_mutation_boundary_deferred_total` + `starvation_mitigated_for_boundary_count`
     after `apply_starvation_mitigation(stolen)` in the inner boundary block

2. **src/serve/scheduler.cpp** — 1 wire-up site after
   `apply_starvation_mitigation(f)` in the #1633 starvation mitigation loop.

3. **src/compiler/observability_metrics.h** — 3 atomic counter slots
   (paired with the legacy per-Fiber counters).

4. **src/compiler/compiler_metrics_fields.inc** — 3 X-macro fields.

5. **src/compiler/evaluator.ixx** — 3 bump_/getter pairs.

6. **tests/test_orchestration_steal_boundary.cpp** (new, ~280 lines,
   7 ACs source-driven).

7. **scripts/check_orchestration_steal_boundary_coverage.py** (new,
   ~150 lines, 10 ACs linter).

8. **CMakeLists.txt** — add `aura_add_issue_test(test_orchestration_steal_boundary)`
   + `aura_issue_test_link_llvm_jit(test_orchestration_steal_boundary)`
   entries.

## 验收标准 (AC)

| AC | Verification |
| --- | --- |
| AC1 | `test_orchestration_steal_boundary` AC1: `worker.cpp::steal` wires `bump_boundary_held_steal_safe_total` after `bump_cross_fiber_mutation_safe_steal` |
| AC2 | AC2: `worker.cpp::steal` wires `bump_steal_mutation_boundary_deferred_total` + `bump_starvation_mitigated_for_boundary_count` after `apply_starvation_mitigation(stolen)` in inner boundary block |
| AC3 | AC3: `scheduler.cpp` wires `bump_starvation_mitigated_for_boundary_count` after `apply_starvation_mitigation(f)` |
| AC4 | AC4: 3 metric slots in `observability_metrics.h` |
| AC5 | AC5: 3 X-macro fields in `compiler_metrics_fields.inc` |
| AC6 | AC6: 3 `bump_/get_` pairs declared in `evaluator.ixx` |
| AC7 | AC7: cross-layer baseline regression — `CompilerService` round-trip still works |

10 linter ACs in `scripts/check_orchestration_steal_boundary_coverage.py`:
production-code wire-up checks for the 3 wire-up sites + metric slot
presence + bump_/getter pair presence + X-macro fields.

## 预期收益
- 三类 closed-loop observability surface 完整(defer + mitigation +
  safe-steal),dashboards 可以区分触发事件 vs outcome。
- 保持 legacy per-Fiber counter 的兼容性(`bump_cross_fiber_mutation_
  safe_steal` / `bump_steal_inner_mutation_boundary_deferred` 继续 bump
  独立),新 metrics 是 per-CompilerMetrics 聚合视图。
- 构建在已有 #1633 inner boundary defer 之上,不破坏已有行为,
  仅补充 observability surface。

## 优先级
**P1** (高并发场景调度可靠性)

## 标签
P1, scheduler, fiber, mutation, stability, work-stealing,
production-readiness

## 相关 Issues
- #1633 (nested MutationBoundary steal defer)
- #1492 (orchestration steal boost)
- #1445 (orchestration boost)
- #1270 (threshold boost after repeated defers)
- #1483 (adaptive safepoint coordination)

## 验证方式
- `tests/test_orchestration_steal_boundary.cpp`: 7 ACs all green
- `scripts/check_orchestration_steal_boundary_coverage.py`: 10 ACs all green
- pre-commit hooks: clang-format clean, `./build.py docs` regen clean,
  `gen_docs.py --check` verify clean
- pre-push gates: primitive surface freeze (no new primitives added —
  existing AOT / scheduler metrics surface extended within 521 budget
  per #1734 raise) + test-registry (#1572) + test binding + coverage (#1453)
- Same PR cycle as #1637 / #1638 / #1639 / #1640: edit → build → run
  tests → descriptive commit → push `main` (direct, no PR review per
  MEMORY.md workflow).