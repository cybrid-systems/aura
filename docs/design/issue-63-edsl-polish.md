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

### Phase 2 — task 3 (workspace injection) ✅ DONE

**Commit**: `0a61077` (2026-06-03)
**Actual approach**: a `(workspace-state)` primitive, not a
compiler flag. The original design proposed an
`--inject-workspace-state` flag that auto-emits the mutation
history into the LLM's context. The implementation went with
a pull-style primitive instead, because:
- Pull-style is more compositional (the LLM can call it
  conditionally, e.g. only when the user asks about workspace).
- Avoids the complexity of injecting into LLM context (which
  requires threading through the eval loop).
- Mirrors how `intend-history` / `intend-analytics` work
  (also pull-style, not injected).

**Task 3: `workspace-context-awareness.aura`** ✅ added
- New primitive: `(workspace-state)` in `evaluator_impl.cpp:11079`.
  Returns a multi-line string:
  ```
  WORKSPACE: <n> defines
  DEFINE: foo
  DEFINE: bar
  MUTATIONS (last 10):
    0:...
  ```
  First line is a summary header so the LLM has a one-token
  extractable count.
- New task: parse the count from the header line.
- Bug fixes in `tests/edsl_benchmark.py` (extract_code):
  - Mask `<think>...</think>` blocks before scanning for
    ``` fence blocks (thinking content was false-matching).
  - Skip fence blocks that don't contain `(` (intro/outro
    paragraphs were being captured as code).

### Phase 3 — task 4 (E4 auto-tune `max-attempts`) ✅ DONE

**Status**: completed 2026-06-04. Final cost: ~1 day (vs 1-2 weeks
original estimate — most primitives already existed, just needed
field extension and the `evolve-strategy` runtime).

**What landed**:
- ✅ `StrategyDef` struct gained 5 tunable fields:
  `max_attempts` (1..20, default 3), `temperature` (0.0..1.0,
  default 0.3), `sys_prompt_template` (free-form), `evolution`
  (generation counter), `parent` (parent strategy name).
- ✅ `intend` now reads `max_attempts` from the strategy when
  `:strategy name` keyword arg is given, overriding the
  positional int arg. Format:
  `(intend goal gen ver [fixer] [max-attempts] :strategy name)`.
- ✅ `define-strategy` / `register-strategy!` accept the new
  keyword args (`:max-attempts`, `:temperature`, `:sys-prompt-template`).
- ✅ `strategy-field` / `strategy-set-field!` / `strategy-inspect`
  extended to read/write the new fields. `strategy-set-field!`
  rejects out-of-range values (1..20 for max-attempts, 0..1 for
  temperature) and read-only fields (name/evolution/parent).
- ✅ `evolve-strategy` primitive implemented. Consumes
  `intend-analytics` output, applies heuristics from
  `e4_evolvable_strategies.md §3`, returns a new strategy name
  (e.g. `"adaptive-v2"`). Old strategy preserved.
- ✅ Benchmark task `tests/tasks/edsl/auto-tune-max-attempts.aura`:
  LLM must call `evolve-strategy` on a strategy with low
  success-rate + low attempts, verify the returned strategy
  has a higher `max-attempts` (expect: 4 when starting from 2).

**Heuristics implemented in `evolve-strategy`**:
- `success-rate < 0.5` AND `avg-attempts = max-attempts` →
  bump `max-attempts` by 2 (cap 20)
- `success-rate > 0.9` AND `avg-attempts < 1.5` →
  lower `max-attempts` by 1 (floor 1)
- Top-errors `unbound-variable` ≥ 2 → append
  `"Do NOT use undefined variables."` to `sys-prompt-template`
- Top-errors `type-mismatch` ≥ 2 → append
  `"Use (check x : Type) before operations."`
- Top-errors `div-zero` ≥ 2 → append
  `"Guard division with (if (= d 0) ...)."`
- Top-errors `syntax-error` ≥ 2 → append
  `"Check parentheses carefully."`

**Verified**: 201/201 bash regression tests pass. The
`auto-tune-max-attempts.aura` task produces the expected `4` when
run end-to-end.

## Out of scope (defer)

- Updating `docs/benchmark.md` to add "EDSL Polish" subsection
  (defer; not in core task list) — task count updated to 149 (auto-tune-max-attempts added in Phase 3)
  — *2026-06-04: benchmark.md updated with M3 run (118/139, 85%), but
   EDSL Polish subsection still deferred.*

## Phase 1+2 acceptance

- ✅ 3 micro-tasks added to existing benchmark task list
  (Phase 1: query-where-basic, structured-error-feedback;
   Phase 2: workspace-context-awareness)
- ✅ All use existing or new EDSL primitives (no new compiler
  flag, just the `workspace-state` primitive)
- ✅ `minimax-m3` model routing works via env var
- ✅ Existing tasks still load (no regressions; 149 total)
- ✅ Design documented, deferred work (Phase 3) clearly scoped
- ✅ Verified `workspace-state` primitive output:
  ```
  WORKSPACE: 2 defines
  DEFINE: foo
  DEFINE: bar
  MUTATIONS (last 10):
    (none)
  ```

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
