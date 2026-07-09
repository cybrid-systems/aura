# Domain test suites (preferred)

Issue numbers are **labels**, not **processes**.

## Policy (减法)

| Do | Don't |
|----|--------|
| Add a row to a domain suite / case table | Create `tests/test_issue_<N>.cpp` by default |
| Put issue id in AC messages / comments (`Close #804`) | Link a new ~200MB binary per issue |
| Extend `obs_schema_cases.hpp` for new `query:*` schemas | Copy-paste 8 AC CHECK blocks |

## Suites

| Binary | Domain | How to extend |
|--------|--------|----------------|
| `test_obs_schema_matrix` | Observability `query:*` schemas + standard total/hits metrics | Edit `obs_schema_cases.hpp` |
| `test_aura_result_error_policy` | Exception / AuraResult policy (#807/#808) | Add ACs in that file |
| `test_issues_*` bundles | Historical issue members (link consolidation) | Prefer domain suite first |

## Closing an observability issue

1. Implement metrics + `query:…` + bumps in production code.
2. Add one row to `kStandardCases` or `kFieldListCases` in `obs_schema_cases.hpp`.
3. If you need a new `bump_slug`, add a `CASE(...)` in `test_obs_schema_matrix.cpp`.
4. Run: `ninja -C build test_obs_schema_matrix && ./build/test_obs_schema_matrix`.

## Legacy `test_issue_*`

Kept for history and late bundles (`jit_late*`). New work should not grow that set.
On-demand debug: `ninja -C build test_issue_804` (EXCLUDE_FROM_ALL duals).
