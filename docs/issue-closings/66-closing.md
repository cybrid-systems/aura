## Status: partial — root-level cache in place, subtree-level re-eval not yet

Issue #66 is **partially** resolved. The root-level incremental eval
caching landed in commit `e8d5312` (referenced as Issue #32b), and
partial typecheck caching landed in commit `88f9202` (Issue #72).

### What's working (verified by inspection)

1. **FlatAST dirty tracking** is in place: `mark_dirty`, `mark_dirty_upward`,
   `mark_subtree_dirty`, `is_dirty`, `clear_dirty`, `clear_all_dirty`
   (`src/core/ast.ixx:905-924`).

2. **All `mutate:*` primitives call `mark_dirty_upward()` on the
   affected node** (12 call sites in `src/compiler/evaluator_impl.cpp`).
   Ancestors of a mutated node are also marked, so clean subtrees'
   cached types are guaranteed valid.

3. **Partial typecheck** (`src/compiler/type_checker_impl.cpp:1155-1180`):
   - If a node is `!is_dirty` and has a cached `TypeId` with no free
     type vars (Issue #72 fix), return the cached type immediately
   - This skips re-typechecking for clean subtrees

4. **Root-level eval cache** (`src/compiler/evaluator_impl.cpp:4370-4389`):
   - If the root is `!is_dirty` AND we have a cached result, return it
   - Otherwise full eval, then cache the result and `clear_all_dirty()`

5. **IR value cache** (`src/compiler/evaluator_impl.cpp:14033`): the IR
   executor uses `value_cache_` to skip re-evaluating clean nodes whose
   cached values are still valid.

### What's NOT done (per the issue's full acceptance criteria)

- **Subtree-level re-eval** beyond the root-level cache. Today, if the
  root is dirty (which is true after any `mutate:*`), the full eval is
  re-run even if only one subtree actually changed.
- **No measured 5x speedup benchmark** on 500+ line programs — the
  tree-walker doesn't skip clean subtrees within a single eval pass.
- **`typecheck-current` is not exposed in the `--serve` protocol** (the
  design doc's P2 items 7+).

### Practical impact

For the EDSL mutation use case (single small mutation followed by
re-eval), the root-level cache means:

- If the mutation is to a leaf and the rest of the program doesn't
  depend on it: full re-eval is unavoidable (the tree-walker doesn't
  track fine-grained dependencies).
- If the program is unchanged (re-running `eval-current`): cached, ~0s.

So the speedup exists for the "re-eval without any change" case
(common in agent observation loops) but not yet for the "small change,
big speedup" case the issue asks for. Closing this as a tracked
follow-up, not a full close.

**Recommendation**: open a follow-up issue for subtree-level re-eval
+ dependency tracking. Until then, root-level cache gives meaningful
improvement for the unchanged-re-eval case and is worth keeping.

Closing this issue as **partially resolved** (root-level cache + partial
typecheck landed) with a new follow-up to be opened for the remaining
subtree-level re-eval.
