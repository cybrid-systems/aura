# CI Platforms (#1573)

Multi-OS coverage for Aura. **Linux remains the production OS** for full
C++ builds, sanitizers, and the benchmark SLO gate. Windows and macOS add
platform-specific early detection without doubling the expensive Linux
container path.

## Matrix (`.github/workflows/ci.yml`)

| Job | Runner | What runs | PR (fast) | main (full) |
|-----|--------|-----------|-----------|-------------|
| `gate` | ubuntu + `ghcr.io/cybrid-systems/dev` | docs/lint/format/fixtures/surface/registry/binding | ✅ | ✅ |
| `build-test` | ubuntu + dev image | `build.py ci` + **bench --strict** (#1569) | issues tier=fast | issues tier=full |
| `ubsan-smoke` / `asan-*` | ubuntu + dev image | sanitizer builds + unit | ubsan on PR; asan main-only | ✅ |
| `platform-gates` / **macos-core** | `macos-latest` | Homebrew GCC 16 + `cmake --preset macos-release` + REPL smoke | aura + smoke | + `test_ir` unit |
| `platform-gates` / **windows-scripts** | `windows-latest` | `build.py gate --scripts-only` | scripts gate | + pure-Python unit gates |

`platform-gates` uses `strategy.fail-fast: false` so one OS cannot cancel the other.

## Tier mapping

| Tier | When | Linux (`build-test`) | macOS | Windows |
|------|------|----------------------|-------|---------|
| **fast** | pull_request | `AURA_ISSUES_TIER=fast` | build `aura` + REPL `(+ 1 2)` | `gate --scripts-only` |
| **full** | push to `main` | full issue matrix + bench SLO | + build/run `test_ir` | + `test_benchmark_gate` / binding / surface unit tests |

Sanitizers and benchmark SLO stay on **Linux only** (key platform). macOS
lacks eventfd/epoll for serve-async; Windows native MSVC is not yet a
supported C++ toolchain for this tree (C++26 modules + GCC `-fcontracts`
+ bash module launcher).

## Local equivalents

```bash
# Linux (production path)
./build.py gate
./build.py ci
./build.py bench --strict
./build.py --sanitizer=asan build && ./build.py --sanitizer=asan test unit

# macOS (Homebrew GCC ≥ 14, recommended gcc@16)
brew install gcc@16 cmake ninja
cmake --preset macos-release
cmake --build build-mac -j --target aura
printf '%s\n' '(+ 1 2)' | ./build-mac/aura   # → 3
# Optional: point build.py at the macOS binary dir
AURA_BUILD_DIR=build-mac ./build.py test unit

# Windows (scripts / DX only until native C++ port)
python build.py gate --scripts-only
# or: set AURA_GATE_SCRIPTS_ONLY=1
```

## `gate --scripts-only` (#1573)

Skips **clang-format** only. Still runs:

- docs --check  
- ruff lint + format  
- fixtures  
- primitive surface freeze  
- test-registry freshness  
- test binding / coverage  

Used by the Windows platform job so CI does not require LLVM `clang-format`
on that runner.

## Non-goals (this issue)

- Full `build.py ci` on macOS/Windows (wall-time + missing serve-async / MSVC).
- ASAN/UBSAN/TSAN on macOS or Windows runners.
- Replacing the Linux `ghcr.io/cybrid-systems/dev` image path.
- Inventing a second workflow file (still only `workflows/ci.yml` for PR/main).

## Related

- Path map: [test_harness_pattern.md](test_harness_pattern.md)  
- CI performance: [ci-performance.md](ci-performance.md)  
- Sanitizers: [sanitizers.md](sanitizers.md)  
- macOS notes: [../README.md](../README.md#platform-notes)  
