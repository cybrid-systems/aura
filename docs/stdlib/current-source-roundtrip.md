# current-source / snapshot roundtrip matrix (Issue #2921)

Locks dual-workspace source fidelity for agent closed loops:

| Issue | Concern |
|-------|---------|
| #2918 | `ast:snapshot` / `ast:diff` use `(current-source :workspace)` |
| #2919 | P0 unparse tags (type / linear / define-type / escapes / dotted lambda) |
| #2920 | FlatAST SSOT after mutate (no stale set-code text for JIT) |

## Test locations

| Artifact | Role |
|----------|------|
| `tests/compiler/test_current_source_roundtrip.cpp` | Table-driven C++ unit matrix |
| `tests/suite/current_source_roundtrip_2921.aura` | Suite smoke under `--load` |
| `./build.py current-source-roundtrip-2921` | Static coverage gate |

## Extending the table

Edit `kRoundtripNoMutate[]` in `test_current_source_roundtrip.cpp`:

```cpp
{"(new-form ...)", "note for humans"},
```

Requirements for each row:

1. `(set-code input)` succeeds  
2. `(current-source :workspace)` has **no** `<digits>` fallback for P0 tags  
3. Second `set-code` of the unparsed text yields the **same** unparse (stable)  
4. Prefer **reparse success + semantic equivalence** over byte-identical pretty-print  

When a new production `NodeTag` lands, add an unparse arm (#2919-style) **and** a table row here.

## Case map

1–3 Dual-workspace + snapshot  
4–9 No-mutate roundtrip table  
10 Mutate rebind + eval-current  
11 Snapshot restore  
12 No angle fallback  
13 Deep nest / depth cap  
14 Null workspace  
