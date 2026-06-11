Closing #155 — mutate API concurrency/safety doc accuracy commit landed in 86fc150.

**What shipped** (1 file, +72 lines, all docs):

Issue #155 (per #153's docs alignment effort). The `mutate_api.md` lacked precision about the C++ layer's actual concurrency + safety mechanisms. Today's commit adds §6 "Current Concurrency & Safety Implementation (C++ Layer)" with six sub-sections that map directly to the source code in `src/compiler/evaluator.ixx`:

- **6.1** `std::shared_mutex workspace_mtx_` — documents the read/write locking discipline (`mutate:*` gets `unique_lock`; `query:*` / typecheck / `eval_current` get `shared_lock`; multiple readers + single writer).
- **6.2** `defuse_version_` + `defuse_affected_syms_` — the increment on every successful mutation, and how downstream caches (`defuse_index_` etc.) use the monotonic version to detect staleness.
- **6.3** `workspace_read_only_` fast-path — the bool flag checked BEFORE lock acquisition to skip the no-op path when the workspace is read-only (`workspace:lock` sets it).
- **6.4** Panic checkpoint / auto-rollback — the `std::stack<>` `panic_safe_source_` mechanism that lets multi-step mutations be safely grouped (save / restore / commit).
- **6.5** Fiber yield at mutation boundaries — the `g_fiber_yield_mutation_boundary` call that yields to other fibers during a mutate, giving the multi-agent orchestration a fair scheduler.
- **6.6** C++ ↔ Aura surface split — the table that says the C++ core has the 12 `mutate:*` primitives + all the concurrency machinery, while `lib/std/workspace.aura` has the workspace management forms (not `mutate:*`).

The old §6 ("#11 时代遗留" implementation details) is renumbered to §7; Future Work to §8.

**Verified at ship:** docs-only commit. No code, no build, no test changes. 173/173 safety / 10/10 regression / all test_issue_* suites still green by construction.

**Note on §6 vs §0:** Unlike #154's §0 (which became the standard Implementation Status pattern), this commit added §6 because the section is concurrency + safety, not a complete C++/Aura primitive inventory. The §0 template formalized in #156 explicitly allows §6 as a domain-specific variant; the README process rule calls out the §6 form as an accepted alternative.

**Follow-ups** (deferred to separate issues):

1. **§6 → §0 renumber for consistency** — `mutate_api.md`'s §6 (concurrency + safety) is a §6 not a §0 because the doc has no other status section. Could be renumbered to §0 in a small follow-up; the README process rule from #156 already calls out this as an accepted variant.
2. **§0 template for remaining 8 core docs** — #156 ships §0 sections for the 8 docs that didn't have any Implementation Status. Natural follow-up to #155.
3. **Source-level audit of §6 claims** — each ✓ claim in the §6 sub-sections is best-effort from `evaluator.ixx` + `evaluator_impl.cpp` knowledge. A deep audit pass would be audit-grade. Low priority unless the §6 is treated as a hard contract by readers.
4. **Bench numbers** — add wall-clock + contention micro-benchmarks to §6 (e.g., `std::shared_mutex` cost under 1-writer / N-readers) to give readers a sense of the actual overhead. Easy follow-up if `tests/bench/` is set up.
5. **Deadlock detection in fuzz tests** — the `tests/suite/concurrent.aura` 12/12 PASS covers happy-path concurrency; chaos-fuzz (random interleaving with `assert` checkpoints) would catch potential deadlocks earlier. Open follow-up.
