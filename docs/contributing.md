# Contributing

Aura: AI-native Lisp — code is the source of truth. Prefer small, focused
changes with tests next to the subsystem they protect.

## Workflow

```bash
./build.py gate     # docs / lint / format / fixtures / surfaces / registry
./build.py check    # gate + build + tests
./build.py bench --strict   # compiler benchmark SLO (#1569)

# Unified Python runner (#1961)
python3 tests/run.py list
python3 tests/run.py issues-fast
python3 tests/run.py fixtures
python3 scripts/fixtures_tool.py status   # shard inventory (#1962)
```

Commit style matches history: `fix(scope): summary (#issue)` (see `git log`).

## Testing (default path)

**Theme-based domain tests > per-issue files.**

| Do | Don't |
|----|--------|
| Extend `tests/domain/` | Add `tests/issues/test_issue_N.cpp` |
| Add obs rows to `tests/domain/cases/obs_schema_cases.hpp` | One binary per stats schema |
| Use issue ids as labels in comments / CHECKs | Encode the issue only in the filename |

Full layout, naming, good/bad examples, and exceptions:

- [`tests/README.md`](../tests/README.md) — policy & decision tree
- [`tests/domain/README.md`](../tests/domain/README.md) — domain suite rules
- [`tests/domain/arena/README.md`](../tests/domain/arena/README.md) — first theme pilot (#1959)
- [`tests/test_harness.hpp`](../tests/test_harness.hpp) — unified harness (#1960)
- [`tests/templates/test_domain_pattern.cpp`](../tests/templates/test_domain_pattern.cpp) — scaffold
- [`tests/legacy_test_inventory.md`](../tests/legacy_test_inventory.md) — legacy inventory (#1957)

## Production + test binding

Changes under certain production primitives may require a paired test
(see `scripts/check_test_binding.py` / gate). Prefer pairing with a
**domain** suite edit, not a new legacy issue file.

## Docs

Hand-written docs stay thin. Generated catalogs live under `docs/generated/`
(`./build.py gate` refreshes them). Prefer updating tests and code comments
over long prose.

## Architecture / module boundaries (#1885)

Dependency direction and cross-layer contracts:

- [`src/core/module_boundary.ixx`](../src/core/module_boundary.ixx) — authority
- [`docs/architecture.md`](architecture.md) — overview

**Rule of thumb:** Core ← Parser ← Compiler ← Serve/Exec/…. Core never imports
Compiler. When you add a new cross-layer edge, update `module_boundary.ixx`
(and tick the PR template checkbox).
