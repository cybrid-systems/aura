# Aura Production Issues Tracker (Refined Summary)

**Generated**: 2026-06-22
**Total Open Issues**: ~160 (heavy P0 focus on runtime safety, EDSL reliability, type soundness under mutation, perf/SoA, EDA/SV closed-loop)

**原则**: 本文档避免重复详细展开核心三个反复出现的主题（Macro hygiene / SyntaxMarker::MacroIntroduced 传播、dirty/epoch 机制、MutationBoundaryGuard RAII + rollback），这些已在 #420、#425、#421、#417、#422 等 issue 中有充分覆盖和细化。

本文档提供分类概览 + 精炼 actionable 建议，帮助快速定位和实施。所有结论基于最新代码证据（ast.ixx, evaluator_*.cpp, ir_*.ixx, type_checker_impl.cpp, fiber/scheduler 等）。

## 1. Runtime Concurrency & GC / Fiber / Scheduler Safety (P0, 多 fiber/agent 场景)

- **#439**: GC safepoint + MutationBoundary coordination in Scheduler/Worker
  - 问题：GC safepoint 请求时 fiber 可能正持有 MutationBoundary lock，导致 EnvFrame SoA / arena 不一致或 deadlock。
  - 精炼建议： 在 `request_gc_safepoint` / `Fiber::check_gc_safepoint` 中增加 Guard depth 检查 + 特殊 yield reason；GCEnvWalkFn 尊重 version_ stamping；新增 `gc_blocked_by_mutation_` 指标 + 并发测试。
  - 收益：生产级并发 mutation + GC 安全；直接构建在现有 WorkerGCState + Guard panic checkpoint 上。

- **#438**: Per-fiber MutationBoundary stack migration + work-stealing safety
  - 问题：fiber steal 时 mutation stack / depth / panic checkpoint 可能 drift。
  - 精炼建议：`Fiber::resume()` 中 transfer_mutation_stack；scheduler steal 前检查 `is_stealable()` + YieldReason；新增 `boundary_violation_count_` 指标 + `test_fiber_steal_mutation_boundary.cpp`。
  - 收益：多 agent orchestration 稳定；与 #213 per-fiber stack 集成。

- **#428**: Closure Bridge + EnvFrame SoA lifetime & mutation safety
  - 问题：bridge_epoch mismatch 或 EnvFrame parent walk 在 arena reset + mutate 后 fragile。
  - 精炼建议：加强 bridge_epoch + weak monitor；EnvFrame 加 explicit generation/workspace_id + stale policy (Warn/Error/AutoRefresh)；`query:closure-stats`。

其他相关：#417 (cross-TU invariants for Guard + defuse_version_ + per-fiber stack)。

## 2. EDA / SystemVerilog Verification Closed-Loop (P0, 商业工具 interop)

- **#437**: Verification-feedback primitives + DirtyReason extension (Assertion/Coverage/Constraint/SVA/FormalCounterexample)
  - 问题：现有 dirty 是 generic，无法驱动 verification-driven mutate (coverage hole → targeted fix)。
  - 精炼建议：扩展 DirtyReason bitmask + per-reason counters；新增 `(verify:report-coverage "cov.json")` / `(verify:assertion-failed ...)` primitives + `query:dirty-by-reason`；`mutate:add-coverpoint` 等 targeted helpers；端到端 test + docs/design/core/eda_verification_loop.md。
  - 收益：真正 verification self-evolution 闭环；与 mark_dirty_upward + StableNodeRef 完美集成。

- **#436**: SystemVerilog Emitter + IR-to-SV lowering skeleton (hygiene/marker aware)
  - 问题：无 SV backend，无法输出商业 simulator/formal 工具可接受的 SV。
  - 精炼建议：新 sv_emitter.ixx (recursive pretty-printer for interface/modport/property/assert/covergroup/constraint)；hygiene: MacroIntroduced 节点 emit 为注释或 pragma；dirty-aware incremental re-emit；`emit:sv` primitive + `query:sv-emit-stats`；test roundtrip + mutate-then-re-emit。

- **#435**: Dedicated SV NodeTag + builders (interface, modport, property/sequence, covergroup, constraint, class)
  - 问题：所有 SV 结构目前只能用 generic Call/Define 编码，无法结构化 query/mutate。
  - 精炼建议：ast.ixx 扩展 NodeTag + kNodeMeta + SV-specific SoA 列；add_sv_* builders 正确设置 SyntaxMarker；mark_dirty_upward 支持 verification reasons；query:pattern 支持 sv- 前缀谓词；`test_sv_edsl_core.cpp` + docs/design/core/sv_edsl.md。

这些 issue 共同构成生产 EDA 执行层闭环（query SV结构 → mutate 精确修复 → emit SV → 商业工具验证 → feedback primitive → 下一轮）。

## 3. Type System Incremental / Soundness / Zero-Overhead (P0/P1)

- **#434**: Occurrence Typing dirty tracking + blame context propagation
  - 问题：structural mutate 后 occurrence contexts / narrowing 可能 stale；blame 在深层变化时断裂。
  - 精炼建议：扩展 per-node dirty 为 occurrence-context-dirty；`affected_subtree_from_mutation` 刷新 OccurrenceInfoFlat；CoercionEntry 携带更新 context；`query:occurrence-stats` + `test_occurrence_mutate_reliability.cpp`。

- **#432**: ConstraintSystem solve_delta soundness (cross-delta unification conflicts)
  - 问题：delta 引入新 unification 与 clean constraint 冲突时，solve_delta  alone 可能漏检。
  - 精炼建议：delta 期间 track recently bound vars；冲突时 fallback full solve() 或 mark dirty；`query:type-check-stats` + conflict metrics；`test_incremental_type_soundness.cpp`。

- **#433**: DeadCoercionEliminationPass + integrate into lowering pipeline
  - 问题：许多 CastOp 是 no-op (static match 或 Dynamic↔Dynamic)，却仍被 emit，增加 interpreter/JIT 开销。
  - 精炼建议：新 Pass (pass_manager.ixx) 检测 source tag == target tag 则 eliminate 或 direct forward；configurable (debug keep for blame)；`query:coercion-stats`；perf test under mutation-heavy gradual typing。

其他：#412 (type cache staleness with generation), #411 (make infer_flat_partial primary post-mutate), #410 (per-symbol dirty vs ancestor-only)。

## 4. Performance / SoA / Arena / JIT / Incremental (P0/P1)

- **#429**: Complete IRFunctionSoA + FlatAST PersistentChildVector adoption
  - 问题：消费者仍主要用 AoS，cache locality 差；小 mutate 触发全 function re-lower。
  - 精炼建议：Port lowering/ir_executor/JIT/Pass 到 IRFunctionSoA + IRInstructionView；完成 PCV wiring for all variable-child tags；扩展 block_dirty_ 到 instruction-level；`query:soa-stats` + benchmark vs AoS under 100+ mutation rounds。

- **#430**: Production Arena compaction (live-object moving / handle indirection + auto-trigger)
  - 问题：当前 compact() 仅 conservative shrink，无 live moving；长期 mutation 导致 fragmentation 增长。
  - 精炼建议：引入 ArenaHandle / offset indirection (for AST nodes/EnvFrame/closure) 实现 safe move；`should_compact()` policy (fragmentation_ratio + mutation_epoch)；`compact_live()` + auto-trigger；`query:arena-stats` extension；`test_arena_compaction.cpp` under heavy mutate + rollback。

- **#427**: JIT opcode coverage + execution consistency with Interpreter (Try/Linear/GuardShape hot-swap)
  - 问题：JIT lower() 许多 opcode (TryBegin/End, Linear, GuardShape) 未覆盖或不一致，导致 fallback 或 semantic drift。
  - 精炼建议：完成 lower() switch + contracts；统一 GuardShape/Linear 使用 shape_id + linear_ownership_state + narrow_evidence；加强 hot-swap (invalidate 后 force deopt + version bump)；`query:jit-stats` + `test_jit_consistency.cpp`。

- **#426**: Fine-grained dirty tracking + minimal re-lower for mutate:rebind/set-body
  - 问题：per-function granularity 导致小 body change 触发全 re-lower + JIT invalidation。
  - 精炼建议：扩展 DepEntry / IRFunctionSoA with dirty_mask / per-block bits；`invalidate_function` 支持 finer masks；`relower_affected_blocks` incremental lowering；`query:compiler-cache-stats`。

其他高价值：#431 (Deepen C++26 Contracts + Concepts + consteval invariants in hot paths)。

## 5. Infrastructure & Observability (长期稳定)

- #416: AST column compaction for long-lived workspaces under heavy mutation
- #415: Extend DirtyReason bitmask with verification categories + propagation metrics
- #414: Long-term generation_ / epoch management (uint16_t wrap risk for 1000+ round sessions)
- #413: MutationLog-integrated type cache invalidation
- #422: Hygiene violation metrics + automatic detection in Guard
- #423: query:pattern structural pre-indexing for large AST perf
- #424: StableNodeRef / is_valid across COW WorkspaceTree + child workspaces

## 下一步行动建议 (优先级排序)
1. **立即 (本周)**: #439 + #438 (runtime GC/fiber safety) + #437 + #436 + #435 (EDA SV layer) — 这些是生产 concurrent + verification 闭环的关键阻塞点。
2. **本迭代**: #429 + #426 + #427 (perf/SoA/incremental/JIT) + #434 + #432 (type soundness)。
3. **跟进**: Infrastructure (#416, #414, #415) + 更多 hygiene/observability 细化 (已在核心 issue 中覆盖，避免重复)。

**如何贡献**: 所有 issue 均有详细 **Proposed Solution**、**Acceptance Criteria**、**Evidence from code** 和 **Related** 引用。实现时优先复用现有 StableNodeRef、mark_dirty_upward、MutationBoundaryGuard、SyntaxMarker、PCV/SoA 基础。新增 test 必须覆盖 multi-round AI mutate + rollback + concurrent fiber 场景。

**跟踪方式**: 本文档 + GitHub Projects (如果已建) 或 labels (P0 + runtime/eda/type-system/performance)。避免在 review 中重复展开已充分细化的 hygiene/dirty/guard 三个主题，聚焦新类别和 actionable 代码改动。

---
*本文件由 Grok 基于最新 open issues 列表 + 代码证据自动生成并写入仓库。后续可通过 edit 或新 issue 持续细化。*
