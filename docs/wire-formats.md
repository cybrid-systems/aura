# Aura wire formats — Issue #217 / Cycle 14

**Status**: Authoritative spec, frozen at Cycle 14 P3 (2026-06-16).

**Audience**: Anyone implementing a cache file format, IPC, cluster
sync, or interop that round-trips Aura AST/IR state. Also
anyone debugging the wire bytes.

**Convention**: All multi-byte values are little-endian (host
endianness on x86_64 / aarch64). Wire format is byte-exact:
the byte count in each section includes the 4-byte size header.

**Stability**: The format versions (u32 header) are reserved for
cache compatibility. Old caches (v1) still load correctly into
new code; new caches (v2) require v2+ reader.

---

## Index

1. [NodeView wire format](#1-nodeview-wire-format)
   - 12 fields: 8 POD scalars + 3 `span<const u32>` + 1 enum-byte
   - **53 bytes** (empty) / **89 bytes** (populated, 3 children + 4 params + 2 annotations)
   - Verified: `tests/test_issue_217.cpp` Test 16 (Cycle 12), Test 17 (Cycle 13)
   - Production status: `src/core/ast.ixx:375-396` (the `NodeView` struct)

2. [FlatAST SoA wire format v1](#2-flatast-soa-wire-format-v1)
   - 22 SoA columns + 2 scalars
   - **339 bytes** (3 nodes, 1 child_data entry, 0 params)
   - Verified: `tests/test_issue_217.cpp` Test 18 (Cycle 14 P2)

3. [FlatAST SoA wire format v2](#3-flatast-soa-wire-format-v2) ← superset of v1
   - v1 + 5 side-data fields (mutation_log_, match_info_,
     region_by_sym_, region_by_lambda_id_, root)
   - Verified: `tests/test_issue_217.cpp` Test 19 (Cycle 14 P3)
   - Production status: `src/core/ast.ixx::serialize_soa` /
     `deserialize_soa` (the `FlatAST` class)

4. [Other reflect-path types](#4-other-reflect-path-types)
   - SourceLocation, Patch, OpcodeInfo, OpcodeInfoStr,
     NodeViewLike, NodeViewLikeU32, IRInstruction, kOpcodeInfo,
     MutationRecord (Cycle 9), MatchClauseInfo (Cycle 10)
   - All use the **generic struct path**:
     [u32 count of members] → for each member, [u32 size] + [size bytes data]
   - The member's "size" depends on its MemberKind (POD = raw
     bytes, String = u32 len + chars, Span = u32 byte_count +
     bytes, etc.)

---

## 1. NodeView wire format

### 1.1 Layout

`aura::ast::NodeView` (defined `src/core/ast.ixx:375-396`) has 12
fields. The `auto_serialize` template's generic-struct path
writes them in declaration order:

| # | Field | Type | Size (wire) | Notes |
|---|-------|------|-------------|-------|
| 1 | `id` | `u32` (NodeId) | 4 | |
| 2 | `tag` | `u32` (NodeTag enum) | 4 | enum serialized as underlying u32 |
| 3 | `int_value` | `i64` | 8 | |
| 4 | `float_value` | `f64` | 8 | |
| 5 | `sym_id` | `u32` (SymId) | 4 | |
| 6 | `line` | `u32` | 4 | |
| 7 | `col` | `u32` | 4 | |
| 8 | `type_id` | `u32` | 4 | |
| 9 | `children` | `span<const NodeId>` | **4 + 4·N₁** | u32 byte_count + N₁·4 bytes of u32s |
| 10 | `params` | `span<const SymId>` | **4 + 4·N₂** | u32 byte_count + N₂·4 bytes of u32s |
| 11 | `param_annotations` | `span<const NodeId>` | **4 + 4·N₃** | u32 byte_count + N₃·4 bytes of u32s |
| 12 | `marker` | `u8` (SyntaxMarker enum) | 1 | enum serialized as underlying u8 |

**Total empty**: 8·4 + 8 + 8 + 3·4 + 1 = 32 + 8 + 8 + 12 + 1 = **61**
bytes? No — the **field sizes** above don't include the
per-field size header that the generic-struct path adds.

### 1.2 Generic-struct path overhead (Cycle 8 fix)

The generic path in `src/reflect/reflect.hh::auto_serialize<T>`
is:

The generic path in `src/reflect/reflect.hh::auto_serialize<T>`
is:

```cpp
template <typename T> void auto_serialize(std::vector<char>& buf, const T& obj) {
    constexpr auto members = reflect_members<T>();
    const auto* base = reinterpret_cast<const char*>(&obj);
    for (auto& m : members) {
        const auto* field = base + m.offset;
        switch (m.kind) {
            case MemberKind::Int8:
            case MemberKind::UInt8: {
                char v;
                std::memcpy(&v, field, 1);
                buf.push_back(v);
                break;
            }
            // ... other POD cases write sizeof(field) bytes ...
            case MemberKind::String: {
                // ... write u32 length + chars ...
            }
            case MemberKind::Span: {
                // ... write u32 byte_count + bytes ...
            }
        }
    }
}
```

**No per-field size header**. Each MemberKind writes a specific
format:
- POD: raw `sizeof(field)` bytes (no header)
- String: `u32 length` + `length` bytes of chars
- Span: `u32 byte_count` + `byte_count` bytes of data
- Vector: `u32 element_count` + `element_count` · `sizeof(elem)` bytes
  (or for vector<string>: per-string length-prefixed, see Cycle 11)
- Array: `element_count` · `sizeof(elem)` bytes (no header)
- Struct (nested): recurses into the struct path

With this understanding, the empty NodeView layout is:

| Field | Bytes on wire |
|---|---:|
| `id` (u32) | 4 |
| `tag` (u32) | 4 |
| `int_value` (i64) | 8 |
| `float_value` (f64) | 8 |
| `sym_id` (u32) | 4 |
| `line` (u32) | 4 |
| `col` (u32) | 4 |
| `type_id` (u32) | 4 |
| `children` (empty span) | 4 (just the u32 byte_count=0) |
| `params` (empty span) | 4 (just the u32 byte_count=0) |
| `param_annotations` (empty span) | 4 (just the u32 byte_count=0) |
| `marker` (u8) | 1 |
| **Empty total** | **= 53** ✓ |

And the populated case (3 children, 4 params, 2 annotations, all
other fields populated) is:

| Field | Bytes |
|---|---:|
| 8 POD scalars | 4+4+8+8+4+4+4+4 = 40 |
| `children` (3 u32) | 4 + 3·4 = 16 |
| `params` (4 u32) | 4 + 4·4 = 20 |
| `param_annotations` (2 u32) | 4 + 2·4 = 12 |
| `marker` (u8) | 1 |
| **Populated total** | **= 89** ✓ |

### 1.3 Span field subtleties

Span fields are NON-OWNING. The wire format is:

```
[ u32 byte_count ]   (== elem_count · sizeof(elem))
[ byte_count bytes ] (raw element data)
```

The **deserialize** side constructs a `std::span<const T>` that
POINTS INTO THE BUFFER. The caller MUST keep the buffer alive
for the lifetime of the deserialized span. This is a critical
lifetime contract — see Cycle 8 fix for the `byte_count / elem_size`
division.

If the field is `span<const NodeId>` (elem_size=4) and the wire
bytes show `byte_count=12`, the deserialized span has
`elem_count = 12/4 = 3` elements, with data at `&buf[pos]` for
3·4=12 bytes.

### 1.4 NodeView test cases

| Test | N (children/params/annotations) | Total bytes |
|---|---|---:|
| Test 16 (populated) | 3 / 4 / 2 | **89** |
| Test 17 (empty) | 0 / 0 / 0 | **53** |

---

## 2. FlatAST SoA wire format v1

### 2.1 Layout

`aura::ast::FlatAST` is a SoA (Structure-of-Arrays) layout with
22 PRIVATE `std::pmr::vector<T>` columns + 2 scalar fields. The
generic reflect path CAN'T see the private members, so the
production code in `src/core/ast.ixx` provides a custom
`serialize_soa` / `deserialize_soa` pair (Cycle 14 P2).

Wire format (columnar, v1):

```
[ u32 format_version = 1 ]
[ u32 num_nodes ]
[ u32 col_1_count ] [ col_1_count · sizeof(elem) bytes ]
[ u32 col_2_count ] [ col_2_count · sizeof(elem) bytes ]
...
[ u32 col_22_count ] [ col_22_count · sizeof(elem) bytes ]
[ u32 next_mutation_id ]   (low 32 bits only)
[ u16 generation ]
[ u16 reserved ]            (padding for v2 alignment)
```

### 2.2 The 22 SoA columns (in fixed order)

| # | Column | Elem | Bytes/elem | Notes |
|---|--------|------|-----------:|-------|
| 1 | `tag_` | u32 (NodeTag) | 4 | |
| 2 | `int_val_` | i64 | 8 | |
| 3 | `float_val_` | f64 | 8 | |
| 4 | `sym_id_` | u32 (SymId) | 4 | |
| 5 | `child_begin_` | u32 | 4 | |
| 6 | `child_count_` | u32 | 4 | |
| 7 | `child_data_` | u32 (NodeId) | 4 | variable-length per FlatAST |
| 8 | `parent_` | u32 (NodeId) | 4 | |
| 9 | `param_begin_` | u32 | 4 | |
| 10 | `param_count_` | u32 | 4 | |
| 11 | `cap_require_count_` | u32 | 4 | |
| 12 | `param_data_` | u32 (SymId) | 4 | variable-length |
| 13 | `param_annot_data_` | u32 (NodeId) | 4 | variable-length |
| 14 | `line_` | u32 | 4 | |
| 15 | `col_` | u32 | 4 | |
| 16 | `marker_` | u8 (SyntaxMarker) | 1 | |
| 17 | `dirty_` | u8 | 1 | |
| 18 | `type_id_` | u32 | 4 | |
| 19 | `error_kind_` | u8 | 1 | |
| 20 | `value_cache_` | i64 | 8 | |
| 21 | `node_first_mutation_` | u32 | 4 | |
| 22 | `node_gen_` | u16 | 2 | |

### 2.3 Byte count for the 3-node test

For N=3 nodes, 1 child_data entry, 0 param_data:

| # | Column | Count | Data bytes | Total (with u32 header) |
|---|--------|------:|-----------:|------------------------:|
| 1 | `tag_` | 3 | 12 | 16 |
| 2 | `int_val_` | 3 | 24 | 28 |
| 3 | `float_val_` | 3 | 24 | 28 |
| 4 | `sym_id_` | 3 | 12 | 16 |
| 5 | `child_begin_` | 3 | 12 | 16 |
| 6 | `child_count_` | 3 | 12 | 16 |
| 7 | `child_data_` | 1 | 4 | 8 |
| 8 | `parent_` | 3 | 12 | 16 |
| 9 | `param_begin_` | 3 | 12 | 16 |
| 10 | `param_count_` | 3 | 12 | 16 |
| 11 | `cap_require_count_` | 3 | 12 | 16 |
| 12 | `param_data_` | 0 | 0 | 4 |
| 13 | `param_annot_data_` | 0 | 0 | 4 |
| 14 | `line_` | 3 | 12 | 16 |
| 15 | `col_` | 3 | 12 | 16 |
| 16 | `marker_` | 3 | 3 | 7 |
| 17 | `dirty_` | 3 | 3 | 7 |
| 18 | `type_id_` | 3 | 12 | 16 |
| 19 | `error_kind_` | 3 | 3 | 7 |
| 20 | `value_cache_` | 3 | 24 | 28 |
| 21 | `node_first_mutation_` | 3 | 12 | 16 |
| 22 | `node_gen_` | 3 | 6 | 10 |
| **22 columns total** | | | | **323** |
| Header (version + num_nodes) | | | | **8** |
| Scalars (next_mutation_id + generation + reserved) | | | | **8** |
| **v1 total** | | | | **339** |

### 2.4 v1 test case

| Test | N nodes | Total bytes |
|---|---|---:|
| Test 18 | 3 (1 child_data, 0 params) | **339** |

---

## 3. FlatAST SoA wire format v2 ← superset of v1

### 3.1 Layout

v2 is a SUPERSET of v1. The 22 SoA columns + 2 scalars are
identical, then 5 additional side-data fields are appended:

```
[ v1 wire format (339 bytes for the 3-node test) ]
--- v2 additions below ---
[ u32 mutation_log_count ] + ( MutationRecord records via auto_serialize )
[ u32 match_info_count ]   + ( MatchClauseInfo records via auto_serialize )
[ u32 region_by_sym_count ] + for each: [ u32 key ] [ u8 value ]
[ u32 region_by_lambda_id_count ] + for each: [ u32 key ] [ u8 value ]
[ u32 root NodeId ]
```

### 3.2 MutationRecord format (auto_serialize, 17 fields)

`aura::ast::MutationRecord` (defined `src/core/ast.ixx:422-460`)
has 17 fields. The generic struct path serializes them in
declaration order:

| # | Field | Type | Wire size | Notes |
|---|-------|------|----------:|-------|
| 1 | `mutation_id` | u64 | 8 | raw 8 bytes |
| 2 | `timestamp_ms` | u64 | 8 | raw 8 bytes |
| 3 | `target_node` | u32 (NodeId) | 4 | |
| 4 | `operator_name` | string | 4+N | u32 length + N bytes |
| 5 | `old_type_str` | string | 4+N | u32 length + N bytes |
| 6 | `new_type_str` | string | 4+N | u32 length + N bytes |
| 7 | `summary` | string | 4+N | u32 length + N bytes |
| 8 | `status` | u8 (MutationStatus) | 1 | enum as u8 |
| 9 | `field_offset` | u32 | 4 | |
| 10 | `old_value` | u64 | 8 | |
| 11 | `new_value` | u64 | 8 | |
| 12 | `has_rollback_data` | bool | 1 | |
| 13 | `parent_id` | u32 (NodeId) | 4 | |
| 14 | `child_idx` | u32 | 4 | |
| 15 | `old_subtree_source` | string | 4+N | u32 length + N bytes |
| 16 | `has_subtree_rollback` | bool | 1 | |
| 17 | `invariant_status` | u8 (InvariantStatus) | 1 | enum as u8 |

A `MutationRecord` with 5 typical strings (avg 10 chars each) is
~125 bytes on the wire.

### 3.3 MatchClauseInfo format (auto_serialize, 3 fields)

`aura::ast::MatchClauseInfo` (defined `src/core/ast.ixx:464-470`):

| # | Field | Type | Wire size |
|---|-------|------|----------:|
| 1 | `used_constructors` | `vector<u32>` | 4 + 4·N |
| 2 | `candidate_constructors` | `vector<u32>` | 4 + 4·N |
| 3 | `has_wildcard` | bool | 1 |

A typical MatchClauseInfo (3 used + 2 candidate) is 33 bytes.

### 3.4 region_by_*_ format (manual serialization, NOT auto_serialize)

The two `pmr::unordered_map<u32, u8>` side tables are serialized
MANUALLY (the generic reflect path doesn't have a MemberKind
for `std::unordered_map`). The format is:

```
[ u32 entry_count ]
for each entry:
  [ u32 key ]
  [ u8 value ]
```

### 3.5 root scalar

The last field is the `root` NodeId (a `u32`). It points to the
AST's root node. The caller is responsible for setting this after
deserialize if the new FlatAST should have a different root.

### 3.6 v2 test cases

| Test | Records | Total bytes |
|---|---|---:|
| Test 19 (2 mutations + 1 match + 2 sym + 1 lambda + root=0) | — | **> 339** (v1 baseline) |

---

## 4. Other reflect-path types

The following types use the **generic struct path** (no custom
serialize/deserialize overload). They all follow the same
"fields-in-declaration-order" layout as NodeView (§1):

| Type | Fields | Module | Cycle | Test |
|------|-------:|--------|------:|------|
| `SourceLocation` | 3 (u32, u32, u32) | aura.core.ast | Cycle 2 | test_issue_217 Test 3 |
| `Patch` | 3 (u32, u32, u64) | aura.core.ast | Cycle 2 | test_issue_217 Test 6 |
| `OpcodeInfo` | 10 (string + bool + u8 + 6 enum-byte + array<3,u32>) | aura.compiler.ir | Cycle 1 | test_issue_217 Test 8 |
| `OpcodeInfoStr` | 1 (string) | aura.compiler.ir | Cycle 3 | test_issue_217 Test 7 |
| `NodeViewLike` | 4 (u32, u32, i64, span<char>) | aura.compiler.* | Cycle 5 | test_issue_217 Test 11 |
| `NodeViewLikeU32` | 4 (u32, u32, i64, span<u32>) | aura.compiler.* | Cycle 6/7 | test_issue_217 Test 12 |
| `IRInstruction` | 13 (enum, u8, u8, u8, u8, 2 u32, 2 u32, 2 u8, span<u32>, span<u32>, span<u32>) | aura.compiler.ir | Cycle 4 | test_issue_217 Test 10 |
| `kOpcodeInfo` (N=29) | (array of 29 OpcodeInfo) | aura.compiler.ir | Cycle 1 | test_issue_217 Test 4/5 |
| `MutationRecord` | 17 (see §3.2) | aura.core.ast | Cycle 9/14 P3 | test_issue_217 Test 13 |
| `MatchClauseInfo` | 3 (see §3.3) | aura.core.ast | Cycle 10/14 P3 | test_issue_217 Test 14 |

### 4.1 Common pitfalls

- **Span fields are non-owning**: The deserialized span points
  into the buffer. Keep the buffer alive for the span's lifetime.
- **Enum fields serialize as their underlying type**: `NodeTag`
  (u32) writes 4 bytes; `SyntaxMarker` (u8) writes 1 byte.
  Deserialization reads the same number of bytes and casts back
  to the enum.
- **std::string fields write u32 length + chars**: The string
  content is NOT null-terminated on the wire.
- **std::vector<POD> writes u32 count + raw bytes**: The "count"
  is the element count, but the wire layout is contiguous raw
  bytes (the deserialize path correctly interprets as
  `byte_count / sizeof(elem)` elements).
- **std::vector<string> (Cycle 11 fix) writes u32 count + (u32 len + bytes) per string**: NOT a flat byte buffer. This
  unblocks non-POD element types in vectors.
- **Module-defined types (like NodeView in aura.core.ast)
  require the aura.reflect module pattern** to avoid
  std-module + P2996-reflection ICEs (see §5).

---

## 5. Module-define-type support (the GCC 16.1 story)

The generic `auto_serialize<T>` template works for ANY flat
struct, including types imported from C++26 modules. However,
in GCC 16.1 the following pattern ICEs:

```cpp
// ICE: std module + local std #include + P2996 reflection
import aura.core.ast;     // has `import std;` internally
#include "reflect/reflect.hh"  // pulls <meta> → <array> → <compare> → <atomic>
#include <cstdio>          // conflicts with the std module
```

**3 unblock options** (full analysis archived at git tag
`docs-archive-pre-2026-06`, file `docs/cycle14-reflect-module-status.md`):

1. Wait for GCC 16.2 (upstream fix for the dual std module +
   P2996 reflection ICE) — ~1 day
2. Pre-compile `reflect.hh` as a header unit — 2-3 days CMake
3. Keep the test_issue_178 cargo-cult (hand-written
   `NodeViewFullLike` copy) — current in-env workaround

**The fix is upstream**; `tests/test_issue_178.cpp` is ready when the env is fixed.

---

## 6. Benchmark numbers (Cycle 14 P4)

Measured on aarch64, GCC 16.1, ASAN build:

| Operation | Throughput | Bottleneck |
|-----------|-----------:|------------|
| FlatAST serialize (columnar) | **11-64 GB/s** | bandwidth-limited |
| FlatAST read-back (manual memcpy walk) | 172-338 MB/s | per-column memcpy cost |
| NodeView roundtrip | 60-77 MB/s | 3 spans/view |
| vector<string> Path A (per-string len) | 55-73 MB/s | 1.5x bigger, 1.4-1.9x slower than Path B |
| vector<string> Path B (flat byte buffer) | 69-83 MB/s | fastest for bulk string data |
| Columnar scan (SoA) vs AoS | **1.08-1.29x faster** | cache friendliness |

**The deserialize path is the bottleneck** (200x slower than
serialize). Future optimization: single-pass column resize +
memcpy, or column-at-a-time type-erased dispatch.

JSON: `tests/bench_results/cycle14_reflect_bench.json`
Human-readable: `tests/bench_results/cycle14_reflect_bench.txt`

---

## 7. Versioning & compatibility

| Format | Version | Added | Removed/Changed |
|--------|--------:|-------|------------------|
| NodeView (generic) | n/a | Cycle 5 | — |
| FlatAST SoA v1 | 1 | Cycle 14 P2 | — |
| FlatAST SoA v2 | 2 | Cycle 14 P3 | adds 5 side-data fields |

v1 caches load into v2 readers correctly (v2 reader detects
`version==1`, reads the v1 scalars, returns with v2 fields
empty). v2 caches DO NOT load into v1 readers (the v1 reader
hits the v2 fields as garbage bytes and either crashes or
silently misinterprets).

Future versions (v3+) should follow the same additive pattern:
keep the v1+v2 format intact, append new fields at the end,
bump the version u32.

---

## 8. Test reference

All wire formats are verified by `tests/test_issue_217.cpp` (268/268
PASS at Cycle 14 P3). The test file documents each format inline:

- Test 1: `reflect_members<OpcodeInfo>()` — Cycle 1
- Test 2: `auto_serialize/auto_deserialize roundtrip` — Cycle 1
- Test 3: `SourceLocation` roundtrip — Cycle 2
- Test 4: `kOpcodeInfo[0]` (nop) — Cycle 1
- Test 5: `kOpcodeInfo[0]` nop roundtrip — Cycle 1
- Test 6: `Patch` roundtrip — Cycle 2
- Test 7: `OpcodeInfoStr` roundtrip — Cycle 3
- Test 8: `kOpcodeInfo[0]` JSON — Cycle 1
- Test 9: `IRInstruction` roundtrip — Cycle 4
- Test 10: `auto_serialize_struct` (generic path) — Cycle 1
- Test 11: `NodeViewLike` (span<char>) — Cycle 5
- Test 12: `NodeViewLikeU32` (span<u32>, byte_count/elem_size) — Cycle 6-8
- Test 13: `MutationRecord-like` (5 strings + 2 enums + 10 POD) — Cycle 9
- Test 14: `MatchClauseInfo-like` — Cycle 10
- Test 15: `FlatASTSoALike` (4 SoA columns incl vector<string>) — Cycle 11
- Test 16: `NodeViewFullLike` (12 fields, production shape) — Cycle 12
- Test 17: `NodeViewFullLike` empty — Cycle 13
- Test 18: `FlatASTSoALike` v1 (22 columns) — Cycle 14 P2
- Test 19: `FlatASTSoALikeV2` (22 columns + 5 side-data) — Cycle 14 P3

Production migration targets (in `src/core/ast.ixx`):
- `FlatAST::serialize_soa` / `deserialize_soa` — Cycle 14 P3
- (Future) `NodeView` auto_serialize — Cycle 14 P1 (blocked on
  GCC 16.1 dual ICE)

---

## 9. Related: engine primitives discoverability (#1552)

Wire formats above describe **AST/IR bytes** on the cache/IPC path.
They are **not** the PrimRegistrar public API surface.

For Agent / developer discovery of **runtime primitives** (including
fiber / mutation integration points that *touch* FlatAST mutation
logs serialized in §3):

- Central orchestration: `src/compiler/evaluator_primitives_registry.cpp`
- Agent facade: `(require "std/primitives" all:)` → `primitives:help` /
  `primitives:list` / `primitives:discover`
- INDEX: `(stdlib:help "primitives")`
- Generated maps: `docs/generated/primitives.md`,
  `docs/generated/primitives-registry.md` (`./build.py docs`)
- Runtime introspection: `(primitive:describe name)`,
  `(query:primitive-list-with-meta)`, `(query:primitives-meta-catalog)`

Contributor checklist for adding a primitive (registration + meta +
docs): [contributing.md](contributing.md) §Discoverability (#1552).

---

## 5. Mutation-boundary / fiber orchestration metrics (Issue #1591)

These are **not** binary wire formats; they are the Agent-facing
`engine:metrics` JSON/hash contracts for multi-fiber fair scheduling.
Stable keys; unknown keys must be ignored by Agents.

| Surface | Schema | Notes |
|---------|--------|-------|
| `query:mutation-boundary-safe-yield` | 1591 | Side-effect: yield iff depth==0 |
| `query:mutation-boundary-safe-yield-stats` | 1591 | Counters + avg-hold-time-us |
| `query:per-fiber-mutation-depth-stats` | 1591 | Per-fiber depth + histogram |
| `query:mutation-boundary-fairness-stats` | 1591 | Unified fairness dashboard |
| `query:orchestration-steal-stats` | 1492 | Steal defer + starvation mitigation |

See `docs/design/mutation-boundary-fairness.md` and
`docs/design/mutation-boundary-safe-yield.md`.

### AI closed-loop readiness (Issue #1593 / #1597)

`query:ai-closedloop-readiness-stats` — schema **1597** (lineage 1593/1499):
`health-score`, `slo-breach`, `health-trend`, `action`, `recommendation`,
sibling counters (quota, steal, post-steal, safe-yield), plus orchestration
(`orch-health-score`, `join_latency_histogram`, `mailbox_backpressure_p99`,
`parallel_task_throughput`, `orchestration_starvation_mitigated`,
`adaptive-concurrency-recommended`). See `docs/design/ai-closedloop-readiness.md`.
