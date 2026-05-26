# SIGSEGV in bench.aura after ~119 tasks (edsl-synthesize-pipeline area)

**Type:** Bug  
**Priority:** P0 — blocks full benchmark completion  
**Found:** 2026-05-26  
**Commit:** `d60036d` (latest as of filing)

## Symptom

Running `tests/bench.aura` with 135 tasks via DeepSeek Chat consistently crashes with SIGSEGV (exit code 139) after task 119 (`edsl-synthesize-pipeline`) when transitioning to task 120 (`edsl-synthesize-template`). The crash is:

```
edsl-synthesize-pipeline FAIL
/bin/bash: line X: XXXXX Segmentation fault
```

## Conditions

- **Always reproducible** after ~100+ preceding tasks (tasks 0-118 must have run)
- **Not reproducible** with fewer preceding tasks (tasks 100-122 run fine in isolation)
- **Exact same spot** every time (between task 119 and 120)
- Crash is inside `eval-current-output` (either `macro_expand_all` or `eval_flat`)

## Already Fixed (P0, shipped)

These fixes addressed other crashes found during debugging but did NOT resolve this SIGSEGV:

1. **Module resolver `std::ifstream` opens directories** → `S_ISREG` check (`b4470d1`)
2. **File I/O primitives read directories** → `S_ISREG` check (`67ef227`)
3. **JIT missing `aura_drop_pair/cell/closure` symbols** → symbol registration (`b4bd033`)
4. **Arena `null_memory_resource()` returns nullptr on OOM** → `new_delete_resource()` fallback (`b4bd033`)

## Suspected Root Cause

Crash is cumulative-state-dependent. After ~119 iterations of `set-code` + `eval-current-output`, the evaluator's internal state (likely `workspace_flat_`, `pairs_`, `string_heap_`, or `cells_`) has been modified enough to trigger a memory access violation. Specific hypotheses:

1. **`string_heap_` / `pairs_` reallocation** invalidates captured references in some lambda
2. **`ast:snapshot` / `ast:restore`** leaves workspace in inconsistent state after `synthesize:pipeline` failure
3. **`macro_expand_all`** on a `FlatAST` with unusual structure (e.g., deeply nested template fills)

## To Reproduce

```bash
cd ~/code/aura
cat > /tmp/repro.aura << 'EOF'
(require "std/bench" all:)
(define tasks
  (let loop ((i 0) (acc (list)))
    (if (< i 122)
      (loop (+ i 1) (append acc (list (list-ref all-tasks i))))
      acc)))
(define results (run-rounds tasks "deepseek-chat" 1 1))
(display "Done.")
EOF
LLM_API_KEY="..." LLM_MODEL="deepseek-chat" ./build/aura < /tmp/repro.aura
```

## Needed

- GDB backtrace or ASAN report from the crash site
- Or: audit of `eval-current-output` / `macro_expand_all` / `eval_flat` for unsafe memory access patterns under repeated state mutation

## Workaround

Use Python runner (`tests/edsl_benchmark.py`) which does not trigger this code path.
