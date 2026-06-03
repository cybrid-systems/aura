# Design: Issue #63 — EDSL Polish micro-tasks for benchmark

## Context

Issue #63 asks for 4 new micro-tasks in the EDSL benchmark
(`tests/edsl_benchmark.py` + `intend` mode), each polishing
a single capability:
1. `query_where_basic` — `query:where` for filtering
2. `structured_error_feedback` — parse and use structured errors
3. `grok_context_injection` — workspace state injection (issue-named after a specific LLM)
4. `e4_basic_evolve` — E4 auto-tunes `max-attempts`

I read the issue, `tests/edsl_benchmark.py:107` (task loader),
`tests/tasks/edsl/*.aura` (existing task format), and the
prerequisite compiler primitives.

## Issues with the issue's naming

- **Task 3 name**: "grok" is a specific LLM (xAI's). The
  benchmark lists Grok as ONE of three model routing options.
  Naming a task after one model suggests it only applies to that
  model. Better: `workspace_context_awareness` (model-agnostic).
- **Task 4 name**: "E4" = "可演化策略 (Evolvable Strategies)",
  the 4th evolution phase of the EDSL framework. Internal
  project terminology, opaque to outside readers. The actual
  capability is "auto-tune `max-attempts` parameter". Better:
  `auto_tune_max_attempts`.

Both renames are mentioned in the issue thread but I'm using
the better names in the design.

## What I found

| Prerequisite | Status |
| --- | --- |
| `query:where` primitive | ✅ implemented (`evaluator_impl.cpp:4931`) |
| `intend-history` / `intend-analytics` | ✅ implemented (`evaluator_impl.cpp:11058+`) |
| `max-attempts` parameter | ✅ exists (`intend` at line 10302) |
| Structured error output | ✅ added by #79 Iter 1 (rich diagnostics) |
| `minimax-m3` in benchmark | ❌ only `MiniMax-M2.7` recognized; need to add |
| E4 framework (strategy as data) | ❌ design doc only, not implemented |
| LLM context injection | ❌ no such mechanism |

Task format (from `tests/edsl_benchmark.py:107`):
- Tasks are `tests/tasks/<category>/*.aura` files
- Header: `;; goal:` (LLM prompt), `;; expect:` (one per line),
  `;; depend:` (stdlib), `;; hint:` (LLM hints)
- Loader auto-discovers via `TASKS_DIR.rglob("*.aura")`

## Phased scope

I broke this into 3 phases based on value × cost:

### Phase 1 — task 1 + task 2 (compiler already supports)

**Cost**: ~3-4 hours
**Value**: high — both exercise real EDSL features

**Task 1: `query-where-basic.aura`** (rename from
`query_where_basic` for consistency with existing task naming
that uses hyphens)
- Goal: use `query:where` + `query:filter` to find a specific
  node in a sample AST (e.g. find all `Call` nodes to `+`).
- Expect: count of matching nodes.
- Hint: provide a sample program; show `query:where` syntax
  with `:node-type "Call"` and `:callee "+"`.

**Task 2: `structured-error-feedback.aura`**
- Goal: parse a structured error (from #79's `with_suggestion` /
  `with_blame`) and use the suggestion to fix the code.
- Expect: the original program is broken; the suggestion tells
  the LLM the right way; correct fix shown via `mutate:fix`.
- Hint: trigger a type error, show how `error -> suggestion` is
  encoded in stderr.

### Phase 2 — task 3 (workspace injection)

**Cost**: 1-2 days (new compiler feature)
**Value**: medium — niche

**Task 3: `workspace-context-awareness.aura`**
- Compiler needs `--inject-workspace-state` flag that emits the
  recent mutation history into the LLM's context.
- Task: LLM should be able to answer "what functions exist in
  the workspace right now?" by reading the injected state.
- Not done in this PR (defer; high cost).

### Phase 3 — task 4 (E4 auto-tune)

**Cost**: 1-2 weeks (implement E4 framework)
**Value**: high but overlaps #80-#84
**Defer**: until Evo-KV track starts.

## Out of scope (defer)

- Phase 2 and Phase 3 (above)
- Updating `docs/benchmark.md` to add "EDSL Polish" subsection
  (defer; not in core task list)

## Model support: add `minimax-m3` to benchmark

The user provided the API key in `~/code/keys/minimax` and
asked for `minimax-m3`. The current benchmark only recognizes
`MiniMax-M2.7`. I'll add a case for `minimax-m3` (and the
case-insensitive variant) in the `_normalize_model` function.

## Backward compat

- Existing task discovery is unchanged.
- Existing 145 tasks continue to work.
- The new model case for `minimax-m3` only activates when
  `LLM_MODEL=minimax-m3` is set.

## Test plan

- `tests/edsl_benchmark.py`:
  - Verify task loader auto-discovers the 2 new tasks.
  - Verify `_normalize_model("minimax-m3")` returns the right
    ModelConfig.
- Manual verification: run the benchmark with
  `LLM_MODEL=minimax-m3 LLM_API_KEY=<key> python3 tests/edsl_benchmark.py`
  on a 2-task subset and check the new tasks run.

## Affected files

- `tests/tasks/edsl/query-where-basic.aura` (new)
- `tests/tasks/edsl/structured-error-feedback.aura` (new)
- `tests/edsl_benchmark.py` (add minimax-m3 model case)
- `docs/design/issue-63-edsl-polish.md` (this file)

## Acceptance

- ✅ 2 micro-tasks added to existing benchmark task list
- ✅ Both use existing EDSL primitives (no new compiler code)
- ✅ `minimax-m3` model routing works via env var
- ✅ Existing 145 tasks still pass (no regressions)
- ✅ Design documented, deferred work clearly scoped
