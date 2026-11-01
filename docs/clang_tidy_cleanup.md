# clang-tidy cleanup plan (#4829)

## Gate

`clang-tidy` CI job runs `scripts/clang_tidy_gate.py` over the
Clang-parseable, non-module TUs under `src/{core,parser,serve,reflect,exec,compiler}`.

- `scripts/clang_tidy_baseline.json` holds the existing per-(file, check) counts.
- A PR fails only when a (file, check) count grows past the baseline.
- `WarningsAsErrors` stays empty; the gate parses diagnostics and enforces counts
  itself, so a clang-tidy version bump does not silently disable it.

## Coverage limit

Clang cannot parse the project's GCC module flags (`-fmodules-ts`,
`-fdeps-format`, `-fcontracts`, `-fmodule-mapper`). The gate filters those
flags and skips TUs that `import` modules: 21 TUs are covered, ~96 module TUs
are not. Follow-up: a Clang module build (`-DCMAKE_CXX_COMPILER=clang++`) with
its own `compile_commands.json`.

## Cleanup order

1. **owning-memory** — `src/core/mutation_audit_wal.hh` (~23), 
   `src/core/security_event_wal.hh` (~6). Replace manual `new[]/delete[]`
   stdio buffers with an RAII owner (fclose-before-free ordering preserved).
2. **special-member-functions** — `src/core/capability_model.hh`,
   `src/core/sharded_counter.h`, `src/compiler/observability_metrics.h`.
   Complete rule-of-five or explicitly delete copy/move.
3. **c-style-cast / reinterpret-cast** — `src/compiler/aura_jit.cpp` (~79
   C-style casts), `src/reflect/reflect.hh` (~34 reinterpret casts). Prefer
   named casts; keep function-pointer registrations as `reinterpret_cast<void*>`
   or typed ExecutorAddr.
4. **vendored code** — exclude `src/linenoise/**` from the tidy scope; its
   init-variables warnings are pure noise.
5. **module coverage** — add the Clang module build, then point the gate at that
   db and regenerate the baseline.

## Refreshing the baseline

Only when accepting debt deliberately:

```sh
python3 scripts/clang_tidy_gate.py --update-baseline
```

The diff to `scripts/clang_tidy_baseline.json` is the review surface.
