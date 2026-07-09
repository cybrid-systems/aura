# CI Performance Roadmap (#873 / #874)

**Goal:** bring `build-test` wall time from ~12–18 min to **&lt; 8 min** on PRs
(and make main-branch full CI noticeably faster).

| Issue | Role |
|-------|------|
| [#873](https://github.com/cybrid-systems/aura/issues/873) | English meta-issue: iterative quick wins → architecture |
| [#874](https://github.com/cybrid-systems/aura/issues/874) | Epic (中文): 从源码架构根治构建慢 + rebuild cascade |
| [#871](https://github.com/cybrid-systems/aura/issues/871) | Done: 减法原则 — bundles, `--changed`, BMI cleanup |

## Root causes (architecture)

1. **Huge `aura.compiler.evaluator` surface** — many partitions / primitives TUs.
2. **Flat link graph** — almost everything links `aura_test_objects` (+ JIT + LLVM).
3. **C++26 modules + GCC 16** — BMI generation, dyndep scans, launcher overhead.
4. **Test binary explosion** — 100+ `test_issue_*` / closed-loop executables.

## Phase roadmap

### Phase 1 — Quick wins (this close / #873+#874 kickoff) ✅

| Change | Knob | Effect |
|--------|------|--------|
| **mold / lld** linker | `AURA_USE_MOLD=1` (default; probes mold, falls back to lld); `=0` to disable | Faster link of many fat test binaries. Mold is skipped when incompatible with GCC 16 `libatomic_asneeded` scripts. lld is invoked via `scripts/linker-bin/ld.lld` to hide benign libxml2 SONAME noise from `/usr/local` LLVM builds |
| **Link job pool** | `AURA_LINK_JOBS=4` (CMake, default 4) | Avoids RAM thrash from N parallel LLVM links |
| **Phase timings** | always printed by `build.py build` | `⏱ cmake configure / build aura / …` for profiling |
| **ccache (local auto)** | on PATH + unset `CCACHE_DISABLE`; CI keeps `CCACHE_DISABLE=1` | Local recompiles faster; CI stays clean |
| **Late issue bundles** | `jit_late1..5` + `light_late` (~240 members) | Fold post-#400 standalones into ~5 link units instead of ~240 × 200MB exes |
| **EXCLUDE_FROM_ALL duals** | automatic for bundle members | `all_test_issue_targets` ~25 deps (was 260+); on-demand `ninja test_issue_N` still works |
| **Bundle-only issue build** | `AURA_ISSUE_BUILD=bundles` | Full tier smoke: profile `test_issues_*` only |
| **Shared `aura_jit_test_objects`** | (landed with #818) | One compile of JIT TUs vs 100+ |

### Phase 2 — Workflow + cache (next)

- [ ] Split workflows: `gate.yml` (~30s) vs `build-test.yml` (PR) vs `heavy.yml` (main)
- [ ] Safe ccache strategy with `cmake/aura_module_launcher.sh` (producer-keyed keys)
- [ ] sccache / remote cache experiment (optional)
- [ ] PR default: `AURA_ISSUE_BUILD=bundles` or expand `--changed` coverage

### Phase 3 — Componentization

- [ ] Split `aura_test_objects` into `aura_core_objects` / `aura_eval_objects` / `aura_serve_objects`
- [ ] Stop re-linking LLVM into every issue binary (more consumers of `aura_jit_test_objects`)
- [ ] Reduce `all_test_issue_targets` fan-out (generate deps from `issue_link_profiles.json`)

### Phase 4 — Modules + unity

- [ ] Smaller BMI producers / fewer `.ixx` re-emits per consumer
- [ ] Selective unity builds for non-module pure `.cpp` clusters
- [ ] PCH only where modules cannot replace

### Phase 5 — Test layering

- [ ] `ctest -L issue` / `-L fiber` already partially via #871 labels — expand
- [ ] Main CI: bundles + unbundled standalones only (skip dead dual standalones)
- [ ] Nightly: full individual matrix

## Local usage

```bash
# Fast linker (if installed)
AURA_USE_MOLD=1 ./build.py build

# Phase timings are always printed:
#   ⏱ cmake configure: 1.2s
#   ⏱ build aura: 45.0s
#   …

# ccache: automatic when installed and CCACHE_DISABLE is unset
# CI keeps CCACHE_DISABLE=1

# Default full build: profile bundles + true standalones only
# (~12 fat links, not 200+ × 200MB)
AURA_ISSUES_TIER=full ./build.py build

# Bundle-only smoke (even fewer targets)
AURA_ISSUES_TIER=full AURA_ISSUE_BUILD=bundles ./build.py build

# Debug a single issue that lives in a late bundle (still available):
ninja -C build test_issue_804

# Cap parallel links
AURA_LINK_JOBS=2 ./build.py build
```

## CI policy

| Setting | PR | main |
|---------|----|------|
| `AURA_ISSUES_TIER` | `fast` | `full` |
| `CCACHE_DISABLE` | `1` (until Phase 2) | `1` |
| `AURA_USE_MOLD` | `1` (install mold if missing) | `1` |
| `AURA_LINK_JOBS` | `4` | `4` |

## Measuring progress

1. Watch `⏱ build total` in CI logs after each phase.
2. Record wall time of `build-test` job in Actions.
3. Success criterion: PR `build-test` **&lt; 8 min** p50 for 2 consecutive weeks.

## Non-goals (for now)

- Turning on ccache by default in CI without producer-keyed BMI safety.
- Full evaluator module split in a single PR (Phase 3+).
