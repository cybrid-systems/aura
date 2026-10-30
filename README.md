# Aura

AI-native Lisp — 代码自己进化。

## Embedding the runtime core (`aura::core`)

`core/persistent_child_vector.hh`, `compiler/fiber_cow_overlay.hh` and
`core/fiber_context.hh` are header-only, but they use C++26 contracts
(`pre(...)`), so a consumer built with `-fcontracts` must link a
`handle_contract_violation`. Link the `aura::core` static target (Issue
#4774) instead of copying sources: it provides aura's contract handler
(logs, calls the hook set by `aura_set_contract_violation_hook`, then
aborts) and the single process-wide fiber-context `thread_local`, adds
`src/` to the include path and `-fcontracts` to the consumer, and pulls in
no evaluator code.

```cmake
target_link_libraries(my_app PRIVATE aura::core)   # in this build tree
```

Out of tree, install the core component and use `find_package`:

```bash
cmake -S . -B build -G Ninja && cmake --build build --target aura_core
cmake --install build --prefix /opt/aura --component aura_core
```

```cmake
find_package(aura CONFIG REQUIRED)                 # -DCMAKE_PREFIX_PATH=/opt/aura
target_link_libraries(my_app PRIVATE aura::core)
```

Installed headers live under `include/aura/` (`core/persistent_child_vector.hh`,
`core/fiber_context.hh`, `compiler/fiber_cow_overlay.hh`), so the same
`#include "core/..."` paths work in and out of the tree.

`tests/core/test_aura_core_embed.cpp` is the reference consumer.
