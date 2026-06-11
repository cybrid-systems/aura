Closing #156 — docs-only commit landed in 2b603d6.

**What shipped** (~375 lines, all docs):

8 §0 Implementation Status sections, one per remaining core/compilation/runtime doc (parallel to #154's §0 in query_edsl.md and #155's §6 in mutate_api.md):

- `design/core/agent_orchestration.md` — TL;DR C++ + Aura tables pointing to the doc's existing massive "实现状态（2026-06）" section; covers fiber:join (#109+#119), orch:parallel real parallelism, workspace_mtx_ + MutationBoundary yield (#107), DefUseIndex per-sym (#107 part 5), FlatAST deep-copy snapshot (#107 part 6). Cross-host / persistent state / auto-scaling / AutoFixEngine marked 🔴 (Phase 4).
- `design/core/typesystem.md` — C++ table covering TypeChecker / InferenceEngine / TypeEnv / ConstraintSystem (T2a) / TypeRegistry / OwnershipEnv / 渐进类型 / Coercion / Occurrence Typing / 增量 (T4) / Let-Poly (T4) / ADT 穷尽性 single-level (T4 partial) / Blame (T2c) / Value Restriction / DeadCoercionEliminationPass (T2e). Multi-mutation 粒化 / type-into-IR / nested-match / module signature propagation marked 🔴.
- `design/core/typed_mutation.md` — C++ table covering MutationRecord / MutationLog / TypedMutationOp (7 ops) / check_mutation / workspace_mtx_ (#107 part 1) / ast:version (#107 part 3) / DefUseIndex per-sym (#107 part 5) / WorkspaceTree+COW (#107 part 6) / Direct FlatAST snapshot / ast:defs/ast:nodes (#108 part 2) / qar (#110) / typed_mutate / rollback_*. Capability Effects / Versioned Types marked 🔴 (P5).
- `design/core/workspace_layering.md` — C++ + Aura tables covering WorkspaceTree / WorkspaceNode / create_child / delete_child / set_active / ensure_local_flat (COW + memory_budget) / workspace:create / switch / delete / list / current / lock / can-write?. 跨层 NodeId 分配 / StringPool 一致性 / 跨 workspace 读写锁 / 三路合并 marked ✗ (设计 / §8 Open Issues).
- `design/compilation/ir_pipeline.md` — C++ table covering IROpcode extensions / primitives-aware lowering / Bool semantics / chained comparisons / closure bridge / Pair+Quote lowering / Bool literal / DeadCoercionEliminationPass / eval() unified entry + needs_tree_walker_fallback. Module system / EDSL / special forms marked ✗ (intentional fallback). Native Pair IR ops / per-module dirty skip marked 🟡 (Future Steps 3-4).
- `design/compilation/jit.md` — C++ table covering AuraJIT / compile_function / 算术+比较+控制流 (38 opcode) / 闭包+Cell / PrimCall bridge / 运行时符号 / display+eval 集成 / --jit flag / LLVM -O2 PassBuilder / 增量 cache / CastOp JIT 化 / hot_swap_function. AOT 路径 marked 🟡 (设计); 跨 host 共享 cache marked 🔴.
- `design/runtime/async_serve.md` — C++ + Aura tables covering Fiber (ucontext+mmap+guard page) / Scheduler (epoll+ready queue+wait map) / 多线程 + work-stealing (#109 Phase 1-2) / Mailbox / Session / recv 5-line yield改造 / send 唤醒 / --serve-async flag / AURA_SERVE_ASYNC 条件编译. Cross-host marked 🔴 (Phase 4).
- `design/runtime/ffi.md` — C++ + Aura tables covering c-load / c-func / apply_closure FFI dispatch / marshalling / eval_flat integration / IR interpreter integration. AuraJIT::register_symbol marked △ (API stub, 集成未做). --safe 模式禁用 c-load marked ✗ (设计).

`docs/README.md` process rule added: "All design/core|compilation|runtime/ docs must have a §0 Implementation Status section" with the 4 requirements (C++ table / Aura table / Future Work / AI Agent 读者请注意) and the 2 maintenance rules (新建文档时同步建 §0; 修改原语时同步更新 §0). Updated core|compilation|runtime counts from `~6/~2/~2 篇` (approximate, pre-#153) to exact `6/2/2 篇` (post-#153).

**After this commit:** all 10 core/compilation/runtime docs have some form of Implementation Status section (8 §0 from #156 + 1 §0 from #154 + 1 §6 from #155). The template is the §0 format; mutate_api's §6 (concurrency + safety) is a domain-specific variant that fits the same intent.

**Scope:** docs-only commit. No code, no build, no test changes. Verified by construction: 173/173 safety / 10/10 regression / all test_issue_* suites still green (no changes touched any .cpp / .ixx files). Source-level verification of each ✓/✗/🟡/△ claim is best-effort from MEMORY.md and the existing "实现状态" tables in each doc body; a deep audit pass is a potential follow-up if the team wants audit-grade accuracy on the §0 tables.

**Follow-ups** (deferred to a separate issue or as needed):

1. **Source-level audit** — each ✓ claim in the §0 tables is best-effort from MEMORY.md and existing "实现状态" tables; a deep audit pass against `src/compiler/evaluator_impl.cpp` etc. would be audit-grade. Low priority unless AI agents start reading §0 as a hard contract.
2. **Mutate_api §0 unification** — `mutate_api.md` has its Implementation Status in §6 (concurrency + safety, from #155) rather than §0. Renumbering to §0 is a single small follow-up; for now the README process rule explicitly calls out the §6 variant.
3. **New docs enforcement** — the README process rule mandates §0 for new docs, but enforcement is social, not CI. A `tests/docs/check_status_section.py` linter is a possible follow-up.
4. **AOT path / cross-host / etc.** — already documented in the §0 tables as 🟡/🔴, captured as future work. No new work needed.
