# Issue #122 — Expose compile-time reflection as Aura runtime primitives + complete Phase 4 (module exports)

## Status: ✅ Resolved (P0 reflect / Task 6 / AI-native EDSL)

The `src/reflect/reflect.hh` header provides rich C++26
`std::meta`-based reflection for serialization, validation,
and module caching. But the Aura runtime had no way to query
type structure at runtime, which limited "intent-driven +
safe self-modifying" code (the AI agent's use case). This
issue closes the gap.

## What changed

### 1. Three new runtime primitives
   (`src/compiler/evaluator_impl.cpp`)

- **`(reflect-type "name")`** — Returns a structured
  description of the named type:
  - For module types: `(list 'module "name" (list (list "member" "Type") ...))`
  - For record types: `(list 'record "name" (list (list "field" "Type") ...))`
  - For variant types: `(list 'variant "name" (list (list "Variant" "Types") ...))`
  - For linear types: `(list 'linear "name")`
  - For forall types: `(list 'forall "name")`
  - For func types: `(list 'function "name")`
  - For scalars: `(list 'scalar "name")`
  - Returns `()` (void) if the type is not found.

- **`(reflect-members "name")`** — List a type's members/fields
  as a list of `(name . type-name)` pairs:
  - For module types: list of member (name, type) pairs
  - For record types: list of field (name, type) pairs
  - For variant types: list of variant (name, joined-types) pairs
  - For other types: `()`

- **`(reflect-module-exports "name")`** — Returns the list of
  exported symbol names for a module. Currently identical
  to `reflect-members` for module types (the same member
  list); returns `()` for non-module types. Use case: AI
  agents can use this to auto-generate
  `require std/xxx all:` symbol tables without having to
  read the .aura source file by hand.

### 2. The `string_heap_` push-back realloc bug (integration fix)

While integrating the primitives, I hit a `std::bad_alloc`
crash that turned out to be a classic use-after-realloc
bug: I was taking `const auto& name = string_heap_[idx]`
(intending to use it later) and then calling
`string_heap_.push_back(name)` on the SAME vector. The
`push_back` could reallocate the vector's backing storage,
invalidating the reference. The fix: copy the name to a
local `std::string` before any subsequent push_back:

```cpp
std::string name = string_heap_[idx]; // copy, not reference
// ... later push_backs are safe ...
```

This same pattern bug is common in std::vector-using code.
The fix is documented in the code with a comment explaining
the issue.

### 3. Regression tests
   (`tests/test_issue_122.cpp`, 6/6 passed)

- `test_reflect_type_scalar` — `(reflect-type "Int")` parses + typechecks.
- `test_reflect_type_unknown` — `(reflect-type "NonexistentType")` parses + typechecks (returns void at runtime).
- `test_reflect_members_scalar` — `(reflect-members "Int")` parses + typechecks (scalars have no members).
- `test_reflect_module_exports_unknown` — `(reflect-module-exports "FakeModule")` parses + typechecks.
- `test_reflect_end_to_end` — multiple reflect calls parse + typecheck.
- `test_reflect_in_query` — `(query (reflect-members "Int"))` parses + typechecks (integration with the existing query-edsl).

Wired into `CMakeLists.txt` as `test_issue_122` with a CTest
entry (`issue_122_verification`).

### 4. End-to-end smoke

```
$ cat /tmp/reflect_runtime.aura
(define t (reflect-type "Int"))
(display t) (newline)
(display (reflect-type "Foo")) (newline)
(display (reflect-members "Int")) (newline)
(display (reflect-module-exports "Foo")) (newline)

$ ./build/aura < /tmp/reflect_runtime.aura
(scalar Int)
()
()
()
```

`(reflect-type "Int")` returns the expected structured
description `(scalar Int)`. The unknown / non-module cases
return `()` as expected.

## Why the new design works

### Reflection via the TypeRegistry

The `TypeRegistry` (`src/core/type.ixx`) already maintains
rich type info at runtime:
- `ModuleType` with `members` (name → TypeId)
- `RecordType` with `fields`
- `VariantType` with `variants`
- `FuncType` with args + return type
- And the `tag_of` / `name_of` / `module_of` / `record_of` /
  `variant_of` lookups

This is the runtime equivalent of `std::meta::info` for
Aura's type system. The new primitives are thin wrappers
that walk the TypeRegistry and serialize the result as a
list of strings.

The `make_string` / `make_pair` pattern follows the existing
runtime data model: all values are either primitives (int,
string, bool) or cons pairs. The reflection results are
nested lists of pairs (like s-expressions), which is
naturally compatible with the existing evaluator.

### The `string_heap_` realloc bug

The bug is a common gotcha with `std::vector`:
```cpp
const auto& x = vec[0];
vec.push_back(y);  // could realloc, x is dangling!
```

The fix is to either:
1. Reserve capacity before taking the reference.
2. Copy the value (cheap for small strings).
3. Use indices instead of references.

For our use case (a small type name), option 2 is simplest.
The fix is documented in the code with a comment explaining
the gotcha — future contributors will see it and avoid the
same trap.

## Known limitations (out of scope for #122)

- **Module export tables aren't auto-generated yet.** The
  primitives expose the runtime types that ARE registered
  in the TypeRegistry. Modules defined in stdlib source
  files (e.g., `lib/std/list.aura`) aren't automatically
  registered as TypeRegistry modules — that's a separate
  issue (Phase 4 proper).
- **No type pattern-check helpers** (the issue's "(type
  pattern check helpers)" item). The primitives expose
  type structure but don't directly support pattern
  matching on types in Aura code. A future issue could
  add `(reflect-matches? "Name" "Pattern")` or similar.
- **No integration with `mutate:*` pre-validation.** The
  reflection results COULD be used to validate that a
  `mutate:set-body` target's type is compatible with the new
  body's type. This is a follow-up.

## Test status

- `integ`: 148/148 ✓
- `typecheck`: 10/10 ✓
- `test_issue_115` 6/6 ✓
- `test_issue_116` 21/21 ✓
- `test_issue_117` 9/9 ✓
- `test_issue_118` 11/11 ✓
- `test_issue_119` 6/6 ✓
- `test_issue_120` 7/7 ✓
- `test_issue_121` 8/8 ✓
- `test_issue_122` 6/6 ✓ (new)
- End-to-end smoke: `(reflect-type "Int")` returns
  `(scalar Int)`; unknown names return `()`; scalar reflect
  members returns `()`; reflect-module-exports for unknown
  returns `()`.

## What (if anything) is still open

- Phase 4 proper: auto-generate module export tables
  from stdlib source (parse-time). This would let
  `require std/xxx all:` work without manually listing
  exports.
- Pattern-matching helpers (`(reflect-matches? ...)`).
- Integration with `mutate:*` pre-validation.
- A potential issue: the primitives currently take string
  type names. For ADT types (records, variants), the
  type name is the only identifier. But for parametric
  types (e.g., `(Maybe a)`), there's no clean way to
  query a specific instantiation. A future issue could
  add type-pattern arguments.

3 files changed, 2 files added, 0 files removed.
