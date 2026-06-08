# Issue #132 — Refactor: split large methods in CompilerService (service.ixx)

## Status: 🟡 Partial (first extraction shipped; 5+ other planned extractions deferred)

The issue describes splitting service.ixx's large
methods (`exec_jit` ~500, `compile_module` ~400, `eval`
~300 lines) into focused classes. Actual sizes:
- `eval`: 328 lines
- `exec_jit`: 450 lines
- `compile_module`: 213 lines

This PR ships the first extraction: pure AST walker
helpers (`find_top_level_defines`, `collect_user_bindings`)
extracted from inline walkers in `compile_module` and
`eval`. The 5+ other planned extractions (JITExecutor,
IRPipelineRunner, IncrementalCompiler,
DependencyGraphManager, IRCacheManager) are deferred as
follow-ups.

## What changed

### 1. New module: `src/compiler/ast_walkers.ixx`

Two pure free functions:

```cpp
// Walk a FlatAST and collect all top-level (define ...)
// forms. Nested defines inside lambda bodies are NOT
// collected (they're not "top-level" for caching).
export std::vector<std::pair<std::string, NodeId>>
find_top_level_defines(const FlatAST&, const StringPool&, NodeId);

// Walk a FlatAST and collect all user-binding names
// (via Define or TypeAnnotation). These are the names
// that subsequent eval calls should NOT fall through to
// the IR pipeline.
export std::vector<std::string>
collect_user_bindings(const FlatAST&, const StringPool&, NodeId);
```

### 2. CompilerService uses the new helpers

`src/compiler/service.ixx`:
- Removed the inline `DefFinder` struct inside
  `compile_module` (was 25 lines) — replaced with
  `aura::compiler::find_top_level_defines(flat, pool, expanded)`.
- Removed the duplicate `DefineFinder` struct inside
  `cache_define` (was 25 lines) — replaced with the
  same free function.
- Removed the inline `track_names` lambda inside
  `eval` (was 18 lines) — replaced with
  `aura::compiler::collect_user_bindings(flat, pool, expanded_root)`.

### 3. Regression tests (11/11 pass)

`tests/test_issue_132.cpp` exercises the new pure
functions. Coverage:
- **find_top_level_defines basic** (3 tests): extracts
  all top-level defines in document order.
- **find_top_level_defines skips nested** (2 tests):
  nested defines inside lambdas are NOT collected.
- **find_top_level_defines edge cases** (2 tests):
  NULL_NODE and non-define roots return empty.
- **collect_user_bindings** (4 tests): extracts both
  Define and TypeAnnotation bindings.

## Why the new design works

### Why extract AST walkers before splitting exec_jit/eval/compile_module

The issue's headline targets (`exec_jit`, `eval`,
`compile_module`) are 200-500 lines each. Splitting them
into JITExecutor / IRPipelineRunner / etc. would touch
hundreds of lines and risk breaking behavior. The AST
walkers are a much smaller, lower-risk extraction: each
is 20-30 lines, self-contained, and has no `this->`
dependency. By extracting them first, we:
1. Establish the pattern (pure free functions, separate
   module, focused responsibility)
2. Verify the new module infrastructure works
3. Get test coverage on the AST walker logic in
   isolation

A future issue can then split the larger methods,
following the same pattern (extract focused helpers
first, then split the top-level methods).

### Why `find_top_level_defines` is a free function, not a class

The walker has no state. It's a one-shot AST traversal
that returns a result. A class with a `walk()` method
would be a verbose form of the same thing. The free
function is the natural fit:
- Inputs: `const FlatAST&`, `const StringPool&`, `NodeId`
- Output: `vector<pair<string, NodeId>>`
- Side effects: none (read-only on the AST)

### Why `collect_user_bindings` is separate from `find_top_level_defines`

The two walkers look similar but serve different
purposes:
- `find_top_level_defines` returns `(name, NodeId)`
  pairs for IR caching purposes.
- `collect_user_bindings` returns just the names for
  the `user_bindings_` set, which tracks what the user
  has defined so subsequent eval calls don't fall through
  to the IR pipeline.

Different outputs, different consumers. A single walker
that returns both would force callers to ignore one or
the other. The separate functions are cleaner.

## Known limitations (out of scope for #132)

The issue proposed 5+ major extractions:
- **JITExecutor** (shape specialization + cache lookup)
- **IRPipelineRunner** (pass pipeline execution)
- **IncrementalCompiler** (incremental compilation logic)
- **DependencyGraphManager** (dep_graph_ state and ops)
- **IRCacheManager** (ir_cache_ state and ops)

None of these are in this PR. Each is a substantial
refactor (tens of member variables, hundreds of
call sites) that would benefit from the pattern
established by this PR (pure free functions first,
then class-based extractions).

The `service.ixx` line count went from 4143 to 4085
(-58 lines, -1.4%). The AST walker extractions removed
~80 lines of inline walkers; the new module has 115
lines including the closing brace. Net: +57 lines but
with the walkers now testable in isolation.

## Acceptance criteria (this PR)

- Pure free functions extracted: ✓ (2 new functions)
- CompilerService methods clearly separate pure
  computation from side effects: ✓ (walkers extracted
  from `eval` and `compile_module`)
- New functions have no `this->` mutable state: ✓
- All tests pass: ✓ (integ 148/148, typecheck 10/10,
  17 per-issue tests)

## Test status

- `integ`: 148/148 ✓
- `typecheck`: 10/10 ✓
- `test_issue_115..132` all 17 pass ✓

## What (if anything) is still open

- Split `exec_jit` into JITExecutor (shape spec +
  cache lookup) and IRPipelineRunner (pass pipeline)
- Split `compile_module` into IncrementalCompiler,
  DependencyGraphManager, IRCacheManager
- Extract `dep_graph_` to a `DependencyGraph` class
  (Issue #132 sub-task)
- Extract `ir_cache_v2_` to an `IRCacheManager` class
- Extract the pass pipeline execution to an
  `IRPipelineRunner` class

2 files changed, 2 files added, 0 files removed.
