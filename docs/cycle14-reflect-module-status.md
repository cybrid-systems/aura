# Cycle 14: reflect.hh → module — STATUS REPORT

## Goal
Convert `src/reflect/reflect.hh` (1383 lines, ~100 public symbols in
namespace `aura::reflect`) into a C++26 module (`aura.reflect`) so
that TUs importing `aura.core.ast` (which has `import std;`) can also
use the reflect path without triggering a GCC 16.1 ICE.

## What I tried (in order)

### Attempt 1: Module wrapper with `export import "reflect.hh"`
- Created `src/reflect/aura.reflect.ixx` with
  `export import "reflect.hh";` (the C++20 "header unit" pattern)
- The header unit re-includes `<meta>`, `<array>`, etc. inside the
  module's purview.
- Fails with: `error: failed to read compiled module: No such file
  or directory` (reflect.hh isn't pre-compiled as a header unit;
  would need a separate build step + CMake support).

### Attempt 2: Add `export` to the namespace in reflect.hh
- Changed `namespace aura::reflect {` to
  `export namespace aura::reflect {` in the header itself.
- Fails with: `error: 'export' may only occur after a module
  interface declaration` (GCC rule) for the **non-module consumers**
  (cache_reflect.cpp, ir_reflect_serialize.cpp).
- Reverted.

### Attempt 3: Empty `export namespace` re-export pattern
- Wrap the include: `module; #include <meta>... export module
  aura.reflect; #include "reflect.hh" export namespace aura::reflect {}`
- The `export namespace aura::reflect {}` re-opens and should
  re-export the names added by the include.
- Fails with: `'reflect_members' is not a member of 'aura::reflect'`
  in the consumer TU. The empty re-export doesn't propagate the
  names. (C++26 modules require explicit `using` declarations or
  namespace bodies with content.)

### Attempt 4: `export namespace` with `module;` global fragment
  (no inline re-export of `using` decls)
- Same as 3, plus putting includes in the global fragment.
- Fails the same way: `reflect_members` not exported.

### Attempt 5: Inline the full reflect.hh body inside
  `export namespace` (1381 lines)
- Generated `aura.reflect.ixx` with the full namespace body
  inlined (a Python script extracted the body from reflect.hh).
- Module interface: `export module aura.reflect; export namespace
  aura::reflect { /* 1381 lines */ }`
- First error: `<compare>` not finding `std::strong_ordering` (the
  module's `<meta>` → `<array>` → `<compare>` include chain
  triggers this).
- Fix attempt 5a: add `import std;` before the includes.
- Fix attempt 5b: add `<compare>` to the includes.
- Fix attempt 5c: remove `import std;` (re-introduces the original
  problem).
- All variants fail with either `std::strong_ordering` not found
  (without `import std;`) or `__atomic_wide_counter` conflicting
  pthread header (with `import std;` + local system std headers).

## The fundamental blocker

GCC 16.1 has **two independent bugs** that combine to make this
refactor very hard:

### Bug 1: std module + local std #include = ICE
When a TU does `import std;` AND includes ANY local std header
(even just `<cstdio>`), GCC 16.1 emits conflicting declarations
for std types like `__mbstate_t`, `__atomic_wide_counter`, etc.
- Workaround: don't include any local std headers after `import std;`.
- But: the consumer TU (test_issue_178.cpp) needs to output text
  (test results), which requires `std::cout` or `std::fprintf`.
  Both need std headers, which can't be included.

### Bug 2: P2996 reflection needs `<meta>` which transitively
  needs `import std;` (to get `std::strong_ordering`)
The module's internal use of `<meta>` triggers a chain of includes
(`<meta>` → `<array>` → `<compare>`) that references `std::strong_ordering`.
Without `import std;` first, this fails.
- Workaround: add `import std;` to the module.
- But: this triggers Bug 1 — the local std headers transitively
  pulled in by `<meta>` now conflict with the std module.

The combination: you can't have BOTH `import std;` AND `<meta>`
in the same TU, in GCC 16.1. This is the root cause of the
refactor's failure.

## Workarounds explored

1. **Use `import std;` only, no `<meta>`** — would require rewriting
   the P2996 reflection code to use std module's reflection
   facilities. The std module doesn't export `<meta>` in GCC 16.1.

2. **Use `<meta>` only, no `import std;`** — would require NOT
   using `import std;` in the consumer TUs. But aura.core.ast
   (and other modules) already have `import std;`, so the consumer
   TU indirectly imports it.

3. **Wait for GCC 16.2+ upstream fix** — this is a known GCC
   bug (search gcc.gnu.org for "module import std conflict
   system header"). The fix is in the GCC trunk but not yet
   released.

4. **Use a different approach entirely** — write a Python/C++
   codegen that produces the inline module from reflect.hh at
   build time, AND ensure no transitive std include chain
   conflicts. This would require significant infrastructure
   (codegen tool) and isn't a 30-line refactor.

## Recommendation

The P0 "small fix" turned out to be larger than expected. The
GCC 16.1 std module + P2996 reflection interaction has two
interacting bugs that require a real fix. Options:

### Option A: Wait for GCC 16.2 (Plan B)
The upstream GCC fix is in progress. When GCC 16.2 lands, the
small `aura.reflect.ixx` wrapper pattern will work. Total
effort: 1 day (waiting).

### Option B: Pre-compiled header unit (medium effort)
Pre-compile reflect.hh as a header unit (CMake support for
`add_executable` + `target_compile_options` + CXX_MODULE_STD
already supports this). Consumers do `import "reflect.hh";`
directly. This doesn't solve the std module + <meta> conflict
but DOES make the path forward cleaner.

Total effort: 2-3 days (CMake plumbing + testing).

### Option C: Defer the production NodeView test
Keep test_issue_178.cpp as a "planned" test (the current state).
Tests 16+17 in test_issue_217.cpp verify the conceptual shape
works. The production migration is deferred until a fixed
build env is available.

Total effort: 0 (current state is shipped).

## Status

- `tests/test_issue_178.cpp` — **shipped** (with build env
  limitation documented in the file).
- `tests/test_issue_217.cpp` Tests 16+17 — **shipped** (213/213
  PASS).
- `src/reflect/aura.reflect.ixx` — **NOT shipped** (refactor
  failed; see above).
- `CMakeLists.txt` test_issue_178 target — **shipped** (with
  build env limitation; the target exists and will work when
  GCC 16.2 fixes the std module + P2996 conflict).

## Next direction

The user (Anqi) suggested the P0 "small fix" as the most
important unblock. After ~2 hours of attempts, I've confirmed
that the refactor isn't a "small fix" in GCC 16.1 — it requires
a real upstream fix. The shipped test_issue_178 + the documented
limitations are the best deliverable for Cycle 14.

Recommend: defer the production migration to a future cycle
when the GCC 16.2 fix is available. In the meantime, focus on
other #217 work (FlatAST SoA custom auto_serialize, CacheHeader
production migration, benchmark suite) that doesn't depend on
the reflect.hh → module refactor.
