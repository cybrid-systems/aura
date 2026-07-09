# Domain test suites (preferred)

Issue numbers are **labels**, not **processes**.

Sources live here (`tests/domain/`). Shared case tables live in `cases/`.

> **Note:** This replaces the old `tests/suites/` path. Aura language E2E
> suites remain in `tests/suite/` (singular) — different thing.

## Policy (减法)

| Do | Don't |
|----|--------|
| Add a row to a domain suite / case table | Create `tests/test_issue_<N>.cpp` by default |
| Put issue id in AC messages / comments (`Close #804`) | Link a new ~200MB binary per issue |
| Extend `cases/obs_schema_cases.hpp` for new `query:*` schemas | Copy-paste 8 AC CHECK blocks |

## Layout

```
tests/domain/
├── README.md
├── cases/
│   └── obs_schema_cases.hpp    # table-driven schema rows
├── test_obs_schema_matrix.cpp
├── test_domain_fiber_orchestration.cpp
├── test_domain_hygiene_dirty.cpp
└── test_domain_typed_mutate.cpp
```

## Suites

| Binary | Domain | How to extend |
|--------|--------|----------------|
| `test_obs_schema_matrix` | Observability `query:*` schemas + standard total/hits metrics | Edit `cases/obs_schema_cases.hpp` |
| `test_domain_fiber_orchestration` | Fiber/steal/Guard/JIT exception | Add CHECKs in that TU |
| `test_domain_hygiene_dirty` | Macro/pattern hygiene, dirty-epoch, terminal/render | Add CHECKs in that TU |
| `test_domain_typed_mutate` | Typed mutate / type-system + shape/SoA/arena | Add CHECKs in that TU |
| `test_aura_result_error_policy` | Exception / AuraResult (`tests/` root for now) | Add ACs in that file |

CMake registration: `cmake/AuraDomainTests.cmake`.

## Closing an observability issue

1. Implement metrics + `query:…` + bumps in production code.
2. Add one row to `kStandardCases` or `kFieldListCases` in `cases/obs_schema_cases.hpp`.
3. If you need a new `bump_slug`, add a `CASE(...)` in `test_obs_schema_matrix.cpp`.
4. Run: `ninja -C build test_obs_schema_matrix && ./build/test_obs_schema_matrix`.

## Closing a fiber / hygiene / typed issue

1. Prefer the matching `test_domain_*` binary above.
2. Add schema + bump + light wiring checks (issue id in messages).
3. Run that single binary; do **not** add `test_issue_N.cpp`.

```bash
ninja -C build test_domain_fiber_orchestration test_domain_hygiene_dirty test_domain_typed_mutate
./build/test_domain_fiber_orchestration
```

## PR fast fixture

`tests/fixtures/issues_fast.json` lists domain suites first, then profile
bundles. Extend a domain suite instead of growing that list with more
`test_issue_*` samples.

## Legacy `test_issue_*`

Flat files under `tests/test_issue_*.cpp` are **legacy** (history + late
bundles). New work must not grow that set. On-demand:
`ninja -C build test_issue_804` (EXCLUDE_FROM_ALL duals).
