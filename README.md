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

`tests/core/test_aura_core_embed.cpp` is the reference consumer.
