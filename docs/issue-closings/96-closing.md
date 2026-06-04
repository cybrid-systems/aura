# Issue #96 — Enterprise Large Codebase Autonomous Evolution Agent

## Status: ⚠️ PARTIAL — Proposed Action #1 LANDED, #2-#3 ARE FOLLOW-UPS

Issue #96 is a market-insight / product-roadmap issue
("Top Insight 1"). Its three proposed actions are at
different states of completion.

## Action 1: Git integration primitives — ✅ LANDED

Six new primitives in `src/compiler/evaluator_impl.cpp`
(lines 3093-3233 area, in the `// ── Git integration (Issue
#96) ──` block):

| Primitive | Args | Returns | Description |
|---|---|---|---|
| `git-status` | — | string | `git status --short` |
| `git-diff` | optional "staged" | string | unified diff (or staged) |
| `git-log` | optional `n` (1..1000) | string | `git log --oneline -n` |
| `git-commit` | message | int | `git commit -m`, exit code |
| `git-branch-current` | — | string | current branch (empty if detached) |
| `git-stage` | file1 file2 ... | int | `git add`, exit code |
| `git-rev-parse` | — | string | current HEAD short SHA |

**Why these are deliberately minimal**: they expose the
"do one thing" primitives the orchestrator can compose
(query status → decide → stage → commit → verify). Higher-
level wrappers like `(git-commit-all "msg")` or
`(git-pr-draft)` are *not* included — those need design
decisions (PR body, reviewers, base branch, etc.) that
are project-specific and should be implemented as
**user-side Aura code** on top of the primitives.

**Build**: passes (201 + 5 new git tests = 206/206 pass).

**Demo**: `tests/git_integration_demo.aura` shows the
primitives in action.

## Action 2: Enhance safe-refactor with pre/post conditions — ⚠️ PARTIAL

The infrastructure is in place:

- `AURA_CONTRACT` (added in #83) provides `pre:` / `post:`
  predicates
- `mutate:refactor/extract` exists in the evaluator
- `mutate:extract-function` exists in the evaluator
- `typecheck-current` is the pre-check
- The agent loop pattern (`intend` → contract verify → commit)
  is documented in `code_evolution_pipeline.md`

What's **missing**: a specific `safe-refactor` wrapper
primitive that *automatically* wires up:

1. Snapshot the affected code
2. Run the proposed refactor
3. Evaluate `AURA_CONTRACT_POST` on the result
4. Compare typecheck output before/after
5. If equal (or stricter) and contracts pass, hot-swap
6. Otherwise restore snapshot

This is **the orchestrator glue** between primitives. It
can be implemented in ~50 lines of Aura on top of
existing primitives, but it's a follow-up.

```aura
;; Skeleton (not yet committed):
(define (safe-refactor target-fn new-body)
  (let* ((snap (ast:snapshot (string-append "pre-" target-fn)))
         (before (typecheck-current))
         (ok? (begin
                (mutate:rebind target-fn new-body)
                (typecheck-current)
                (and (post:contract-passed? target-fn)
                     (= (compute-coverage target-fn)
                        (compute-coverage target-fn))))))
    (if ok?
        snap
        (begin (ast:restore snap) #f))))
```

## Action 3: Build E2E demo for 100k+ LOC codebase — ❌ NOT STARTED

This is a **multi-week effort** requiring:

- A large open-source Aura codebase to refactor against
  (Aura itself is ~50k LOC, so even self-refactoring is
  not 100k)
- A benchmark task suite (Jira-ticket style) with expected
  outcomes
- Performance metrics (latency, PR throughput, regression
  rate)
- A demo runbook / video

This is **a research-grade deliverable**, not a single-PR
fix. The right path is to:

1. Land Action 1 (✅ done in this commit)
2. Build Action 2 in a follow-up PR
3. Use Aura's own codebase as the 100k-LOC target — at
   current growth rate, it'll be 100k LOC in ~6 months
4. Run the demo on Aura itself as a self-hosting benchmark

The EDSL benchmark (148 tasks, 85% pass rate) is the
**closest existing artifact** to a "100k LOC demo" —
it exercises `mutate:*` / `query:*` / `intend` /
`synthesize:*` on Aura's own modules. See
`docs/benchmark.md`.

## Why this isn't a "done" closing

The issue is **OPEN** and the three proposed actions are
at three different states. Rather than mark the issue
closed, the right action is to:

1. **Land the git primitives** (done in this commit)
2. **Open a follow-up issue** for Action 2 (safe-refactor
   wrapper) — ~1 day of work
3. **Track Action 3** in the agent-evolution roadmap —
   ~multi-week

The git primitives unblock the most common need (let the
agent commit its own work). The safe-refactor wrapper is
a thin layer. The 100k LOC demo is a research effort.

## Reference

- `src/compiler/evaluator_impl.cpp` — git primitive
  implementations
- `tests/run-tests.sh` — 5 new git tests (lines 404-413)
- `tests/git_integration_demo.aura` — usage demo
- `docs/design/code_evolution_pipeline.md` — the agent
  loop pattern these primitives slot into

## How to close on GitHub

If you accept Action 1 as sufficient:

```bash
gh issue close 96 -c "Action 1 (git primitives) landed:
git-status / git-diff / git-log / git-commit / git-stage /
git-branch-current / git-rev-parse in evaluator_impl.cpp +
206/206 tests pass. Action 2 (safe-refactor wrapper) and
Action 3 (100k LOC demo) are follow-up issues; the EDSL
benchmark (148 tasks, 85% pass) is the closest existing
artifact to Action 3."
```

Or keep it OPEN and link to this closing file as a status
update comment.
