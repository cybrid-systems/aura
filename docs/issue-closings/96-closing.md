# Issue #96 — Enterprise Large Codebase Autonomous Evolution Agent

## Status: ⚠️ PARTIAL — Actions 1 & 2 LANDED, Action 3 is follow-up

Issue #96 is a market-insight / product-roadmap issue
("Top Insight 1"). Its three proposed actions are at
different states of completion.

## Action 1: Git integration primitives — ✅ LANDED (commit a425797)

Seven new primitives in `src/compiler/evaluator_impl.cpp`:

| Primitive | Args | Returns | Description |
|---|---|---|---|
| `git-status` | — | string | `git status --short` |
| `git-diff` | optional "staged" | string | unified diff (or staged) |
| `git-log` | optional `n` (1..1000) | string | `git log --oneline -n` |
| `git-commit` | message | int | `git commit -m`, exit code |
| `git-branch-current` | — | string | current branch (empty if detached) |
| `git-stage` | file1 file2 ... | int | `git add`, exit code |
| `git-rev-parse` | — | string | current HEAD short SHA |

5 read-only tests; 206/206 pass.

## Action 2: Safe-refactor wrapper primitives — ✅ LANDED (this commit)

New stdlib module `lib/std/safe-refactor.aura` with **4 functions**:

### `safe-refactor:with-snapshot`
```aura
(safe-refactor:with-snapshot tag thunk)
;; → thunk result on success
;; → (list 'rolled-back "reason") on error or explicit fail
```
Wraps work in a snapshot; auto-restores on raise or `'fail` return.

### `safe-refactor:check-and-apply`
```aura
(safe-refactor:check-and-apply pre-verify post-verify apply-fn)
;; → (list 'applied result)        on success
;; → (list 'rejected "reason")     if pre-verify fails
;; → (list 'rolled-back "reason")  if post-verify fails
;; → (list 'error "reason")        on exception
```
Pre/post verification gate. Apply only if pre-verify passes; rollback
if post-verify fails.

### `safe-refactor:replace-fn`
```aura
(safe-refactor:replace-fn fn-name new-body)
;; → (list 'applied snap-id)   on typecheck pass
;; → (list 'rejected "reason") on typecheck fail
```
Atomically replaces a function body. Snapshots, appends new definition,
runs `typecheck-current`, restores on failure.

### `safe-refactor:rollback`
```aura
(safe-refactor:rollback)
;; → #t on success, #f if no snapshots
```
Rolls back the most recent safe-refactor snapshot.

5 new tests in `tests/run-tests.sh` (lines 415-427):
- `safe-refactor:loaded` — module imports cleanly
- `safe-refactor:success` — returns thunk result
- `safe-refactor:rollback` — restores on error
- `safe-refactor:pre-fail` — rejects when pre-verify fails
- `safe-refactor:applied` — applies when all verifications pass

**Total: 211/211 tests pass** (was 206; +5 safe-refactor tests).

### AURA_CONTRACT integration

The post-verify hook in `safe-refactor:check-and-apply` is the
intended seam for `AURA_CONTRACT_POST`:

```aura
(define (post:sort-returns-sorted lst)
  (define sorted (sort-fn lst))
  (equal? sorted (sort (copy lst))))

(safe-refactor:check-and-apply
  (lambda () #t)                                    ; pre-verify
  (lambda () (post:sort-returns-sorted '(3 1 2)))   ; post-verify via contract
  (lambda ()
    (mutate:rebind 'sort-fn '(lambda (x) ...))))
```

This is the safe-refactor wrapper's primary use case.

## Action 3: Build E2E demo for 100k+ LOC codebase — ❌ NOT STARTED

This is a **multi-week effort** requiring:

- A large open-source Aura codebase (Aura itself is ~50k LOC; not
  yet 100k)
- A benchmark task suite (Jira-ticket style) with expected outcomes
- Performance metrics (latency, PR throughput, regression rate)
- A demo runbook / video

**Closest existing artifact**: the EDSL benchmark (148 tasks, 85%
pass rate) exercises `mutate:*` / `query:*` / `intend` /
`synthesize:*` on Aura's own modules. See `docs/benchmark.md`.

**Right path**:
1. Land Action 1 (✅ done in `a425797`)
2. Build Action 2 (✅ done in this commit)
3. Use Aura's own codebase as the 100k-LOC target — at current
   growth rate, it'll be 100k LOC in ~6 months
4. Run the demo on Aura itself as a self-hosting benchmark

## Implementation notes

### Use `list` not `cons` (workaround)

The safe-refactor functions return `(list 'tag "reason")` rather
than `(cons 'tag "reason")`. This works around an apparent
**cons-corruption bug in module-loaded functions**: when a function
defined in a `lib/std/*.aura` module returns a cons cell, the cons
sometimes has its `car` or `cdr` corrupted to a large integer
(observed: `147605376151711743`). The same function works
correctly when defined inline in the calling script, and works
correctly when using `list` instead of `cons`.

This is a real Aura bug worth tracking. Workaround in place;
functionality is correct.

### Why these specific primitives

- `with-snapshot` and `check-and-apply` are the **atomic** primitives
  the agent can build higher-level workflows on.
- `replace-fn` provides a specific common case (function replacement)
  but the agent should mostly use `check-and-apply` for custom
  verification.
- All four compose with existing `mutate:rebind` / `set-code` /
  `typecheck-current` / `ast:snapshot` primitives.

## Reference

- `lib/std/safe-refactor.aura` — module (4 functions, ~150 lines)
- `lib/std/safe-refactor.aura-type` — type signatures
- `tests/safe_refactor_test.aura` — standalone test (8 cases)
- `tests/run-tests.sh` lines 415-427 — 5 runner tests
- `docs/design/code_evolution_pipeline.md` — agent loop pattern

## How to close on GitHub

If you accept Actions 1 & 2 as sufficient for now:

```bash
gh issue close 96 -c "Actions 1 (git primitives) and 2 (safe-
refactor stdlib module with with-snapshot / check-and-apply /
replace-fn / rollback, 211/211 tests pass) landed. Action 3
(100k+ LOC E2E demo) is a multi-week follow-up; EDSL benchmark
(148 tasks, 85% pass) is the closest existing artifact."
```

Or keep it OPEN and link to this closing file as a status update
comment.
