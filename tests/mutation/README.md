# tests/mutation/

Mutation / Guard / occurrence-dirty / typed-audit regression drivers.

## Prefer extending batches

| Target | Theme |
|--------|--------|
| `test_mutation_guard_unit_batch` | MutationBoundaryGuard RAII / CAS / MBP macro / dirty-clear guards |
| `test_mutation_occurrence_dirty_batch` | occurrence dirty/blame post-mutate + fine dirty relower |
| `test_mutation_aot_unit_batch` | AOT metrics lazy / region / hot-update typed audit |
| `test_mutation_typed_audit_batch` | TypedMutationAudit + solve_delta locality + linear post-mutate |
| `test_mutate_batch` / `test_linear_batch` / `test_mutation_boundary_batch` / … | Existing large family batches |

Do not grow new `tests/mutation/test_*.cpp` for routine Guard/occurrence ACs — add a section to the matching batch.

Registered heavy standalones (bundle / custom) and large stress files remain separate.
