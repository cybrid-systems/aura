# EDA Domain (`eda:*`) — Issue #1968

**Status: commercial EDA vertical (deferred from SlimSurface core).**

Parent: #1965 (Phase 3 commercial_readiness scope) · Decision: **KEEP**.

## Decision

The 13 `eda:*` primitives are **kept** as a commercial electronic design
automation vertical, not deleted. They are already excluded from the
SlimSurface *core* budget via `DOMAIN_STATUS["eda:"] = "deferred"`.

| Option | Chosen | Why |
|---|---|---|
| Remove | No | ~60 tests (closed-loop SV, commercial emit fidelity, Guard safety, production infra) plus hardware-backend / self-evolution demos depend on them. |
| Keep + gate | **Yes** | Same pattern as #1967 (`tui:`): optional commercial surface + budget freeze. |

## Build flag: `AURA_ENABLE_EDA`

CMake option (default **ON** so existing CI / closed-loop tests keep working):

```bash
# Default full build (EDA registered)
cmake -B build -S .

# Slim / core-only: skip eda:* registration
cmake -B build_slim -S . -DAURA_ENABLE_EDA=OFF
```

When `AURA_ENABLE_EDA=0`:

- `register_eda_primitives` is a no-op (`evaluator_primitives_eda.cpp`)
- Three additional `eda:*` adds in `evaluator_primitives_compile.cpp`
  (`register_compile_p34` / `p35`) are compiled out

Source inventory for the primitive freeze still lists all 13 names
(source-scanned `add("eda:…")`) so the freeze baseline stays stable
across ON/OFF builds.

## Commercial domain budget

```text
COMMERCIAL_DOMAIN_BUDGETS["eda:"] = 13   # scripts/check_primitive_surface.py
```

`./build.py gate` runs `check_primitive_surface.py --strict`, which fails
if the source-scanned `eda:` count exceeds this budget. Raising the budget
requires an explicit PR edit to `COMMERCIAL_DOMAIN_BUDGETS` + justification.

## Surface (13 primitives)

| Primitive | TU |
|---|---|
| `eda:parse-netlist` | `evaluator_primitives_eda.cpp` |
| `eda:query-nodes` | same |
| `eda:mutate-add-instance` | same |
| `eda:waveform-snapshot` | same |
| `eda:run-hardware-feedback` | same |
| `eda:load-sv` | same |
| `eda:parse-verification-result` | same |
| `eda:ingest-result` | same |
| `eda:invoke-simulator` | same |
| `eda:validate-sv-emit-roundtrip` | same |
| `eda:run-verification-feedback` | `evaluator_primitives_compile.cpp` (p34) |
| `eda:run-commercial-simulator-stub` | same (p35) |
| `eda:demo-sv-self-evolution` | same (p35) |

Related modules: `eda_commercial_sim.ixx`, `eda_parse_common.ixx`, `sv_ir`,
`hardware_backend`.

## Related

- SlimSurface: #1448 / #1449
- Foundational EDA: #499 · closed-loop / commercial: #693 / #695 / #698
- Sibling commercial keep: #1967 (`tui:`)
- Sibling deferred domains: #1969–#1976
