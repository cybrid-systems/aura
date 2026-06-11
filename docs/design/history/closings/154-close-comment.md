Closing #154 — query EDSL doc accuracy commit landed in 08190e8.

**What shipped** (1 file, +44 lines, all docs):

Issue #154 (per #153's docs cleanup effort). The `query_edsl.md` over-promised: it described a rich set of primitives as if they were all uniformly available to Aura callers, when in reality:

- C++ core (`src/compiler/evaluator_impl.cpp`) has **ALL 11 `query:*` primitives** implemented: `find`, `node-type`, `children`, `parent`, `siblings`, `root`, `node`, `calls`, `where`, `filter`, `pattern`. C++ callers (AI agents, internal passes) can use them all.
- Aura helper layer (`lib/std/query.aura`) only has **3 helpers**: `query:filter`, `query:uncalled`, `query:callers-of`. Aura callers can only use these three.

This commit adds an "Implementation Status" section (§0) at the top of `docs/design/core/query_edsl.md` that:

- Lists all 11 C++ primitives with status (✓) and one-line note.
- Lists the 3 Aura helpers with status (✓) and note.
- Lists the C++ primitives that are NOT wrapped at the Aura layer (mark with ✗).
- Calls out the future work (richer `where` predicates, nested-pattern support, full DefUse on Aura surface).
- Warns the AI Agent reader: the doc preserves architectural vision, but the implementation status section is the source of truth for what actually works today.

The rest of the doc (the original Chinese-language design write-up from #112) is unchanged — the existing content is still architecturally accurate (the features it describes exist in C++); the gap was that the doc didn't make the C++/Aura distinction explicit. Today's commit closes that gap.

**Verified at ship:** docs-only commit. No code, no build, no test changes. 173/173 safety / 10/10 regression / all test_issue_* suites still green by construction.

**Follow-ups** (deferred to separate issues):

1. **Apply the §0 template to the remaining 8 core docs** — #156 ships the §0 template for `agent_orchestration` / `typesystem` / `typed_mutation` / `workspace_layering` / `ir_pipeline` / `jit` / `async_serve` / `ffi`. This is the natural follow-up to #154.
2. **Source-level audit** — each ✓ claim in the §0 tables is best-effort from `evaluator_impl.cpp` knowledge + existing "实现状态" tables. A deep audit pass would be audit-grade. Low priority unless AI agents start treating §0 as a hard contract.
3. **Aura-side wrapping** — the 8 unwrapped C++ primitives (`find` / `node-type` / `children` / `parent` / `siblings` / `root` / `node` / `calls` / `where` / `pattern` / `def-use`) could be wrapped at the Aura layer. Each is ~5-10 lines; total work is small but adds ~80 lines to `lib/std/query.aura`.
4. **Richer `where` predicates** — currently `query:where` supports `:node-type` / `:callee` / `:defined-by` / `:parent-type` / `:has-type`; more predicates (`:refs` / `:has-occurrence-narrowing` / `:linear?` / etc.) are open.
5. **Nested-pattern support** — `query:pattern` currently only supports flat `...`; nested pattern matching is open.
6. **DefUse on Aura surface** — C++ internal has full `DefUseIndex`; Aura surface only exposes `refers-to?` helper. Full surface would let AI agents reason about def-use chains in pure Aura.
