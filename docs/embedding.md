# Embedding Aura runtime pieces (#4774)

Two static library targets can be linked into a host C++ program without the
Evaluator, the JIT or reflection.

| Target | Contents |
| --- | --- |
| `aura::core` | `core/persistent_child_vector.hh`, `compiler/fiber_cow_overlay.hh`, `core/fiber_context.hh` (header-only), plus the C++26 contract violation handler and the process-wide `fiber_ctx` thread_local |
| `aura::fiber_rt` | `aura::serve::Fiber` / `Scheduler` (`serve/scheduler.h`), worker threads and work stealing; links `aura::core` |

```cmake
find_package(aura CONFIG REQUIRED)      # or add_subdirectory / in-tree target
target_link_libraries(my_app PRIVATE aura::fiber_rt)   # or just aura::core
```

Install with `cmake --install <build> --component aura_core --prefix <dir>`.

## Contract handler

Aura headers use C++26 contracts (`pre()`, `contract_assert`). A translation
unit compiled with `-fcontracts` that includes them needs a
`handle_contract_violation(const std::contracts::contract_violation&)` at link
time. `aura::core` provides it (and sets `-fcontracts` on consumers); a host
must not define its own. To observe violations, register a hook with
`aura_set_contract_violation_hook()`; the handler calls it, then aborts.

## Evaluator-side hooks in `aura::fiber_rt`

The fiber and scheduler code reaches the Evaluator only through C-ABI hooks.
In `aura::fiber_rt` they default to no-ops (`src/compiler/fiber_bridge.cpp`,
`src/serve/fiber_rt_defaults.cpp`); all defaults are weak, so a host that also
links the real owners gets theirs. The values installed by the runtime can be
read back with `aura_fiber_rt_current_fiber_id_fn()` and
`aura_fiber_rt_long_mutation_hook()`.

A multi-worker `Scheduler` refuses to run outside a sandbox decision (#3586);
a host that does not use the Aura sandbox sets `AURA_SANDBOX=off`.

Regression tests: `tests/core/test_aura_core_embed.cpp`,
`tests/core/test_aura_fiber_rt_embed.cpp`.
