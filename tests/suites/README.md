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
| `test_domain_fiber_orchestration` | Fiber/steal/Guard/JIT exception init (#810–#814, #811, #821–#823, #875) | Add CHECKs in that TU |
| `test_domain_hygiene_dirty` | Macro/pattern hygiene, dirty-epoch, terminal/render (#815–#819, #824–#826, #847) | Add CHECKs in that TU |
| `test_domain_typed_mutate` | Typed mutate / type-system + shape/SoA/arena (#820, #832–#836, #827–#829, #862–#864) | Add CHECKs in that TU |
| `test_aura_result_error_policy` | Exception / AuraResult policy (#807/#808) | Add ACs in that file |
| `test_issues_*` bundles | Historical issue members (link consolidation) | Prefer domain suite first |

## Closing an observability issue

1. Implement metrics + `query:…` + bumps in production code.
2. Add one row to `kStandardCases` or `kFieldListCases` in `obs_schema_cases.hpp`.
3. If you need a new `bump_slug`, add a `CASE(...)` in `test_obs_schema_matrix.cpp`.
4. Run: `ninja -C build test_obs_schema_matrix && ./build/test_obs_schema_matrix`.

## Closing a fiber / hygiene / typed issue

1. Prefer the matching `test_domain_*` binary above.
2. Add schema + bump + light wiring checks (issue id in messages).
3. Run that single binary; do **not** add `test_issue_N.cpp`.

```bash
ninja -C build test_domain_fiber_orchestration test_domain_hygiene_dirty test_domain_typed_mutate
./build/test_domain_fiber_orchestration
./build/test_domain_hygiene_dirty
./build/test_domain_typed_mutate
```

## PR fast fixture

`tests/fixtures/issues_fast.json` lists domain suites first, then profile
bundles. Extend a domain suite instead of growing that list with more
`test_issue_*` samples.

## Legacy `test_issue_*`

Kept for history and late bundles (`jit_late*`). New work should not grow that set.
On-demand debug: `ninja -C build test_issue_804` (EXCLUDE_FROM_ALL duals).
Legacy batches `test_issues_809_817_batch` / `test_issues_819_829_batch` are
EXCLUDE_FROM_ALL; coverage lives in domain suites.
