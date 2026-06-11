## Status — pmr/arena 控制 primitives 大部分已暴露

Issue #81 asks for fine-grained control of double-arena + pmr from
.aura level, and structured observability for the evolution engine.
Most of this is **already exposed** in the existing primitive set:

### Existing primitives (verify with `tests/evo_kv_smoke.aura`)

- **`arena-offset`** (evaluator_impl.cpp:2903) — current TL arena offset
  for benchmarking.
- **`gc-heap`** (11712) — full GC (calls g_gc_collect if available)
- **`gc-freeze`** (11712) — freeze closures (prevent GC)
- **`gc-temp`** (11721) — reset temp arena + clear temp closures (used
  by evo-kv's `evo-kv-bench.aura` between iterations)
- **`gc-stats`** (11758) — closure count + per-module counters
- **`gc-module name`** (11778) — unload a module's per-module arena
- **`gc-module-count`** (11788) — current loaded module count
- **`gc-arena-stats`** (11859) — `;`-delimited string of all arena usage
- **`gc-arena-info`** (11895) — **structured vector of hashes** with
  per-arena `{name, used, capacity, used-pct}` for the evolution
  engine to pattern-match on
- **`memory-pressure`** (12013) — summary hash with overall-pct +
  list of warnings
- **`set-memory-policy`** (12147) — set `auto-gc`, `warn-pct`,
  `critical-pct`, `sample-every`, `cooldown-evals`,
  `recent-gc-temp-window` from a hash. Returns the previous policy.

### What's NOT done (per the issue's full requirement)

- **Per-arena hard budget** (`arena-reserve name bytes`): the issue's
  "hot key cache sizing" example would benefit from being able to
  reserve a fixed budget for a specific named arena and have
  evo-kv's evolution engine react when it's near full. No such
  primitive yet; would need new arena infrastructure.

- **Aura-level callback registration for pressure events**: the
  evolution engine currently polls `memory-pressure` to detect
  thresholds; an event-driven model (call this Aura function when
  warn_pct is crossed) would be cleaner. Not implemented; would
  need a callback list + dispatch hook in `sample_counter_` block.

- **KV data structure allocation integration**: `RefHash` and `Pairs`
  use `std::vector` (heap-allocated, not pmr). Converting to
  `pmr::vector` with the per-module arena would tie their memory
  lifetime to the module's GC. Substantial work; tracked
  separately.

### What to recommend for the evolution engine TODAY

`(gc-arena-info)` is the right primitive to call in the evolution
loop. Example pattern for evo-kv's hot-key cache:

```aura
(define (cache-evolve-step)
  (let ((info (gc-arena-info))
        (json-info (car (filter (lambda (i) 
                                 (equal? (cdr (assq 'name i)) 
                                         "json.aura"))
                               info))))
    (if (> (cdr (assq 'used-pct json-info)) 80)
        (gc-temp)        ;; reclaim temp arena
        (cache-promote-hot-keys))))
```

This gives the evolution engine structured observability already.

Closing this issue as **largely resolved** by the existing primitive
set — follow-up issues to be opened for per-arena hard budgets
and event-driven pressure callbacks.
