# Design: Issue #58 — Module / non-module boundary documentation

## Context

Issue #58 reports that "C++26 module restrictions in `value.ixx`
prevent forward-declaring constexpr constants and tags, blocking
short-string optimization, Shape system, and custom calling
conventions." The proposed solution was three steps:

1. Move `ShapeTag` / `TypeTag` / `RefType` constants to a traditional
   `.h` header.
2. Refactor `EvalValue` to use `std::variant` + `std::bit_cast`.
3. Add clear documentation on module vs non-module boundaries.

## What I found

Step 1 is **already done** (just not documented as one policy):

- `ShapeTag` lives in `src/compiler/shape.h` (traditional header).
- `RefType` and the bias sentinels (`FLOAT_BIAS_VAL`, `STRING_BIAS_VAL`)
  live in `src/compiler/value_tags.h` (traditional header).
- Both headers are `#include`d by `value.ixx` in its `module;`
  preamble so the constants are visible inside the module.
- `TypeTag` stays in `src/core/type.ixx` as an `export enum class`.
  Moving it to a header would force every consumer to use both
  `import aura.core.type` AND `#include "type_tag.h"`, which loses
  the module's encapsulation benefits. See "Why TypeTag stays in a
  module" below.

Step 2 (`std::variant` refactor) is a large change with ABI impact
that interacts with `lib/runtime.c`. Not done in this PR — punted.

Step 3 (this PR) is what I'm doing: write down the rule so a future
contributor adding a new tag doesn't have to reverse-engineer it
from three files.

## The rule (one place, then propagate)

### Principle

**C++26 modules in this codebase are for the compiler core only.
lib/runtime.c, JIT C++ runtime glue, and any non-module
translation unit consume the *header* (value_tags.h), not the
module's interface.**

The module `.ixx` file owns:
- the high-level interface (helper functions, type IDs, structural
  types like `EvalValue`),
- the rules for constructing and inspecting values.

The header `.h` file owns:
- the low-level encoding details (bit patterns, bias sentinels, ref
  type ids),
- the ABI contract that `lib/runtime.c` and any non-module C/C++
  consumer must obey.

The header is `module;` pre-included into the module so the module
implementation can use the same constants. Nothing inside the
header is `export`ed; it's an implementation detail of the module.

### Three tiers of header inclusion

| Header         | Who includes it | Why                                        |
| -------------- | --------------- | ------------------------------------------ |
| `value_tags.h` | value.ixx (via `module;` `#include`) | Low-level encoding constants shared with runtime C |
| `value_tags.h` | lib/runtime.c   | C side, no module knowledge                |
| `value_tags.h` | Any non-module .cpp in the compiler | C++ side that needs bit-pattern helpers     |
| `shape.h`      | shape_profiler.cpp, service.ixx (non-module .cpp), JIT runtime | Recursive `Shape` type can't be a module     |
| `type.ixx`     | (only via `import aura.core.type`) | Module-exported; no header needed for module consumers |

### Why TypeTag stays in a module

`TypeTag` is the only tag system that has both:
- a *rich* type algebra (subtyping, variance, forall, register_func
  with deduplication) — all implemented in `TypeRegistry`
  (`src/core/type.ixx`), and
- *every other module* references it via `import aura.core.type`.

If we put `TypeTag` in a header:
- Every `.cpp` that already `import`s the module would need both
  `import` and `#include <type_tag.h>` to use the enum.
- The deduplication / forall / variance logic in `TypeRegistry`
  can't move to a header (it depends on templates, `std::pmr`,
  `std::optional`, etc.) so the module would still exist and we'd
  just be splitting the tag definition off from its machinery.
- The header would have to mirror the module's exact numeric values,
  opening a drift risk that the current single-source-of-truth
  avoids.

So the rule is: **tag systems whose values are widely used across
modules live in modules** (TypeTag, IROpcode, NodeTag). **Tag
systems whose values are encoding details shared with non-module
code live in headers** (RefType, ShapeTag, bias sentinels).

### Encoding for the four data shapes

The on-disk/in-memory layout of a value is encoded in the 64-bit
`EvalValue::val`. The four shapes that aren't "Ref" (i.e., that
are encoded in the val bits directly) are:

| Encoding    | val range                          | type_id  |
| ----------- | ---------------------------------- | -------- |
| Fixnum int  | bit 0 = 0, val > FLOAT_BIAS_VAL    | INT      |
| Float       | FLOAT_BIAS_VAL < val <= STRING_BIAS_VAL | FLOAT |
| String      | val <= STRING_BIAS_VAL             | STRING   |
| Special     | val & 3 == 3 (so val ∈ {3,7,11})   | BOOL,BOOL,Void |
| Ref         | bit 0-1 = 01, bits 2-5 = RefType  | (varies) |

The header documents the ranges and the constructors. The module
documents the high-level `make_*` / `is_*` / `as_*` API.

### Cross-cutting tag map

`ShapeTag` (in `shape.h`) maps to `RefType` (in `value_tags.h`).
The mapping table is in `value_tags.h` (just below the RefType
declarations). When a new `ShapeTag` is added, three things update
in lockstep:

1. `shape.h`: add the new tag to the `ShapeTag` enum class.
2. `value_tags.h`: add the corresponding `RefType` id (if the new
   shape encodes as a ref).
3. `shape_profiler.cpp::shape_of_value`: add the `case
   ShapeTag::X: return SHAPE_X;` mapping.

`shape_profiler.cpp:49-55` is the canonical mapping function — a
future contributor should look there when adding a shape.

## The three updates made in this PR

### 1. `src/core/type.ixx` (top of file)

Added a 40-line module-boundary docblock explaining:
- what's exported and who consumes it
- why `TypeTag` stays in the module instead of moving to a header
- what to do if you need a TypeTag equivalent in a non-module TU

### 2. `src/compiler/value.ixx` (top of file)

Added a 30-line module-boundary docblock explaining:
- the split between EXPORTED (high-level) and header-only
  (low-level encoding) symbols
- why this split exists (lib/runtime.c and JIT C++ glue)
- where to look for each kind

### 3. `src/compiler/value_tags.h` (after RefType declarations)

Added a 30-line tag-map table listing all `ShapeTag` ↔ `RefType`
mappings and the encoding rules for the four non-ref shapes
(fixnum, float, string, special). Plus a pointer to the canonical
mapping function in `shape_profiler.cpp`.

### 4. `src/compiler/shape.h` (top of file)

Updated the existing header comment to point to this design doc
in addition to `value_tags.h`.

## What's NOT done in this PR

Step 2 of the original issue (refactor `EvalValue` to
`std::variant` + `std::bit_cast`) is a much larger change. It
touches every consumer of `EvalValue` across the compiler and
requires coordinating with `lib/runtime.c`'s C-side representation.
Not appropriate for a doc PR.

Step 1 is fully addressed by the existing header split — the
"missing" part was just the documentation, which is what this PR
adds.

## Acceptance

Issue #58 acceptance #3 ("Add clear documentation on module vs
non-module boundaries") is now satisfied. Acceptance #1 is already
satisfied by the existing `value_tags.h` and `shape.h` files
(separately from this PR). Acceptance #2 is the `std::variant`
refactor, deferred.

## Test impact

No new tests — this is a documentation-only change. The static
asserts in `value_tags.h` (e.g. `RefKeyword == 11`) and in
`shape.h` already catch drift if someone changes a numeric
value without updating both sides.

## Files changed

- `src/core/type.ixx` (top-of-file docblock)
- `src/compiler/value.ixx` (top-of-file docblock)
- `src/compiler/value_tags.h` (tag-map table)
- `src/compiler/shape.h` (header comment update + pointer to this doc)
- `docs/design/issue-58-module-boundaries.md` (this file)
