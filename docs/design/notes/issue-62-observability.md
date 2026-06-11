# Design: Issue #62 — Production-grade observability for self-evolving paths

## Context

Issue #62: "Self-evolving systems (Speculative JIT, auto-evolution,
arena allocation) are hard to debug and observe in production."

Proposed features:
- Structured logging for specialization / deopt / arena usage
- `evo explain` command showing current hot path specializations
- Metrics: `specialization_hit_rate`, `deopt_count`, `arena_usage`
- Version rollback for JIT specializations
- Integration with existing INFO command

I read `src/compiler/ir_executor_impl.cpp`,
`src/compiler/aura_jit.cpp`, `src/compiler/aura_jit.h`,
`src/compiler/shape.h`, `src/compiler/shape_profiler.h`,
`src/compiler/service.ixx`, `src/reflect/reflect.hh`,
`src/compiler/arena.ixx`, `src/main.cpp`.

## What already exists

The reflect framework (`src/reflect/reflect.hh`) provides
`aura::reflect::auto_to_json<T>()` for any POD-ish struct.
This is the foundation for Iter 2-3.

Existing observability primitives:

| Type | Where | What |
| --- | --- | --- |
| `ShapeFnMetrics` | `shape.h:106` | per-function: total_calls, deopt_count, shape_stability_ratio, shape_change_frequency, unique_shapes_seen, is_good_deopt_candidate |
| `ArenaStats` | `arena.ixx:9` | per-arena: capacity, used, peak_used, allocation_count, wasted |
| `cs.cached_function_count()` | `service.ixx:2425` | IR cache size |
| `cs.shape_metrics(name)` | `service.ixx:3537` | ShapeFnMetrics for a function |
| `cs.last_closures()`, `cs.last_cells()` | `service.ixx` | last-eval side effects |
| `kDeoptTrace` env var | `ir_executor_impl.cpp:20` | deopt tracing toggle (from #61 Iter 4) |
| `--env-json` CLI | `main.cpp:1931` | JSON dump of last eval |

What's missing:

- **Counters are not actively collected**. `ShapeFnMetrics.deopt_count`
  is defined but never incremented. `specialization_hit_rate` would
  need a hit/miss counter that doesn't exist. `arena_usage` is
  per-arena but not aggregated.
- **No structured logger**. Current logging is `std::fprintf(stderr,
  ...)` with ad-hoc format strings. No JSON. No time stamps. No
  consistent schema.
- **No `evo explain` command**. The closest is `ShapeFnMetrics`
  accessible via `service.shape_metrics(name)`, but there's no
  CLI wrapper that aggregates and pretty-prints it.
- **No AuraQuery hooks**. The QueryEngine has 6 kinds
  (HasType, HasError, ...); nothing about JIT/arena/deopt metrics.

## The fix (4 iterations, small to big)

### Iter 1 — Counters (foundation)

**File**: `src/compiler/observability_metrics.h` (new)

Define POD-ish structs suitable for `aura::reflect::auto_to_json`:

```cpp
export struct CompilerMetrics {
    std::uint64_t deopt_count = 0;             // #61 Iter 4 trace
    std::uint64_t specialization_hits = 0;     // #60 shape-based fast path
    std::uint64_t specialization_misses = 0;  // deopt OR generic
    std::uint64_t shape_changes_observed = 0;  // ShapeProfiler
    std::uint64_t jit_compilations = 0;       // AuraJIT::compile
    std::uint64_t jit_compile_misses = 0;      // returned null
    std::uint64_t jit_cache_evictions = 0;    // invalidate_function
    std::uint64_t aot_emits = 0;              // --emit-binary success
    std::uint64_t aot_fallbacks = 0;          // --emit-binary → shell wrapper
    std::uint64_t arena_bytes_used = 0;        // ArenaStats::used (sum)
    std::uint64_t arena_bytes_peak = 0;        // ArenaStats::peak_used (sum)
};

export struct FnMetrics {
    std::uint64_t total_calls = 0;
    std::uint64_t deopt_count = 0;
    std::uint64_t hit_count = 0;
    std::uint64_t miss_count = 0;
    double hit_rate = 0.0;  // computed: hit_count / (hit_count + miss_count)
};
```

Wire into existing hot paths:
- `kDeoptTrace` branch in `ir_executor_impl.cpp`: increment
  `deopt_count` on mismatch.
- `JIT::compile()` success: increment `jit_compilations`; null
  return: increment `jit_compile_misses`.
- `jit_cache_.erase()` in `invalidate_function()`: increment
  `jit_cache_evictions`.
- `--emit-binary` success: increment `aot_emits`; fallback to
  shell wrapper: increment `aot_fallbacks`.
- ArenaGroup `total_stats()` queried on the way out:
  increment `arena_bytes_used`/`peak_used`.

Expose via `CompilerService::metrics()` returning a const ref to a
`CompilerMetrics` (atomic for thread safety).

### Iter 2 — Structured JSON logger via reflect

**File**: `src/compiler/observability_logger.h` (new)

```cpp
export inline void log_event_json(const char* event_type,
                                   const auto& fields) {
    if (const char* e = std::getenv("AURA_OBS_LOG");
        e && (e[0] == '1' || e[0] == 't')) {
        // fields must be auto_to_json-able: POD struct
        std::string body = aura::reflect::auto_to_json(fields);
        std::println(stderr, R"({{"event":"{}","fields":{}}})",
                     event_type, body);
    }
}
```

Replace existing `std::fprintf(stderr, ...)` calls in:
- `kDeoptTrace` block in `ir_executor_impl.cpp`
- AOT fallback message in `aura_jit_bridge.cpp`

Each call site defines a small POD struct and calls `log_event_json`.

### Iter 3 — `evo explain` CLI subcommand

**File**: `src/main.cpp`

New flag `--evo-explain` (or just `--evo`):

```cpp
if (std::string_view(argv[1]) == "--evo-explain") {
    // Run the user's program (argv[2] or stdin)
    // After eval: emit a JSON snapshot of CompilerMetrics +
    //   per-function ShapeFnMetrics + jit_cache_ size + arena usage
    auto snapshot = cs.snapshot();
    std::println("{}", aura::reflect::auto_to_json(snapshot));
}
```

`CompilerService::snapshot()` returns a struct holding the metrics
above + a vector of `FnMetrics` (one per cached function name).

The reflect framework handles the JSON serialization — no manual
`std::println` of fields.

### Iter 4 — AuraQuery integration

**File**: `src/compiler/query_impl.cpp`

Add 3 new query kinds:

```cpp
enum class Kind { ..., Specialization, DeoptCount, ArenaUsage };

case Specialization:
    // Find the JIT cache entry for the queried function name and
    // report has_shape_map, last hit_count, last miss_count.
    return jit_cache_.find(name)->second has_shape_map;
case DeoptCount:
    return cs.metrics().deopt_count;
case ArenaUsage:
    return cs.metrics().arena_bytes_used;
```

Usage: `./aura --query '(:deopt-count)'` or similar.

## Out of scope (defer)

- **Version rollback for JIT specializations**: needs per-function
  versioning plus a rollback path on `mutate:*`. Natural follow-up
  after #84 (Evo-KV concurrency).
- **Prometheus / OpenTelemetry export**: a metrics export layer
  is independent of the in-process observability. Defer until
  observability is actually used in production.
- **Persistent metrics across restarts**: requires a metrics
  store on disk. Punt.

## Backward compat

- Iter 1: new atomic counters, no behavior change.
- Iter 2: new logger, only emits when `AURA_OBS_LOG=1` set.
  Default off, like the existing deopt trace.
- Iter 3: new CLI subcommand, no behavior change for existing
  flags.
- Iter 4: new AuraQuery kinds, existing queries unchanged.

## Test plan

- `tests/test_ir.cpp`:
  - TC62 OK: `auto_to_json(CompilerMetrics{})` round-trips with
    correct field names.
  - TC62 OK: `log_event_json("test", struct{...})` produces a
    JSON string with `"event":"test"` prefix when env var set.

- `tests/test_regression.py`:
  - Add a case that calls `--evo-explain` after a simple program
    and asserts the output is valid JSON with the expected metrics
    fields.

## Affected files (incremental)

- Iter 1: `src/compiler/observability_metrics.h` (new),
  `src/compiler/service.ixx`, `src/compiler/ir_executor_impl.cpp`,
  `src/compiler/aura_jit.cpp`, `src/compiler/aura_jit_bridge.cpp`,
  `src/main.cpp`
- Iter 2: `src/compiler/observability_logger.h` (new),
  `src/compiler/ir_executor_impl.cpp`,
  `src/compiler/aura_jit_bridge.cpp`
- Iter 3: `src/main.cpp`, `src/compiler/service.ixx`
- Iter 4: `src/compiler/query_impl.cpp`, `src/compiler/query.ixx`

## Acceptance

- ✅ Clear visibility into self-evolving behavior (`--evo-explain`
  + per-fn metrics)
- ✅ Easy debugging of performance regressions
  (`AURA_DEOPT_TRACE=1` + `AURA_OBS_LOG=1` together)
- ✅ No significant runtime overhead (all paths env-gated or
  atomic-fast)
