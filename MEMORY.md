
## Session 2026-06-30 — bundle heap-use-after-free (test_issues_jit crash)

CI failure: test_issues_jit bundle SIGABRT during test_issue_297
with "corrupted size vs. prev_size while consolidating". 6 tests
in the bundle failed; bundle exited with rc=-6.

Root cause was TWO coupled bugs, both needed:

**Bug 1 (src/core/ast.ixx)**: commit e5f559bf ("fix warning")
removed `node_first_mutation_` from the move-ctor and explicit
ctor init lists to silence a -Wreorder warning. Default member
init used `new_delete_resource` while sibling columns used the
arena's monotonic_buffer_resource — mixed allocators during
~FlatAST(). Restored `node_first_mutation_(std::move(...))` and
`node_first_mutation_(alloc)` to the two init lists. Copy ctor
/ copy assign already had it.

**Bug 2 (src/core/arena.ixx) — the REAL one**: compact()/defrag()
called `resource_.release()` in rebuild_resource_() which reset
the bump pointer to `buffer_.data()`. Next arena_.create<FlatAST>
allocated at the buffer start, OVERWRITING earlier live FlatASTs
that dtors_ still held pointers to. When ~CompilerService() ran
~arena_() → run_destructors(), ~FlatAST() was called twice on
the same arena address. The first call decremented binding_gens_
use_count 1→0 and freed the control block; the second call
read use_count of the freed block — heap-use-after-free.

Diag confirmed: 5 ctors + 4 dtors before ASan abort, with ctor
#5 at the same arena address as ctor #1 (arena memory reuse).

Fix: rebuild_resource_() now a no-op. Bump pointer stays at
start + stats_.used (just past live data). std::vector::resize
shrink preserves capacity, so the bump pointer remains valid.

Commit 02dd097e pushed to origin/main. Verified:
  test_issue_300 standalone: 10/10 PASS
  test_issues_jit bundle: 57/57 PASS (was the failing bundle)
  All 4 JIT bundles ASan-clean.
  
test_issue_226 test_doomsday_stress has a SEPARATE pre-existing
UAF (verified via git stash before fix) — concurrent fiber
stress, unrelated to this fix. Track as separate issue.

**Pattern**: when an ASan-detected UAF appears on a member that
"shouldn't" be shared (shared_ptr with use_count=1 in every
~dtor), check if the destructor is being invoked twice on the
same memory because of arena address reuse after a reset/rebuild.

## Session 2026-06-30 (continued) — pre-existing CHECK macro UAF (#226 follow-up)

The pre-existing UAF in test_issue_226 test_doomsday_stress was
a different bug from the FlatAST/binding_gens_ one:

`tests/test_harness.hpp:101` CHECK macro used
`const auto& _check_msg = (msg);`. With msg = `("..." + ...).c_str()`,
the const reference binds to the const char* value but the underlying
temporary std::string is NOT bound to anything that extends its
lifetime. By the time std::println's format_args dereferences the
stored pointer (inside std::formatter<char const*, char>::format,
via strlen / memcpy on a string_view constructed from the raw
pointer), the buffer is freed. The [class.temporary] lifetime
extension rule for "reference parameters in function calls" doesn't
apply because _check_msg is a local variable, not a function parameter.

Same bug in `tests/issue_test_harness.hpp` (re-evaluated the msg
expression in each branch — same dangling-pointer hazard).

Fix: store _check_msg as `const std::string _check_msg = (msg);`
(by value). std::string's converting ctor accepts both const char*
(deep copy) and std::string, so all existing call sites work unchanged.

Commit 41d3aed8 pushed to origin/main.

**Pattern**: when a CHECK macro (or similar printf-style helper) is
ASan-flagged on a `READ of size N at 0x...` inside strlen/memcpy
called from std::formatter<char const*, char>::format, the macro
is holding a reference to a pointer whose target (a temporary
std::string) was destroyed before the print actually dereferenced
it. Fix: own the message as a std::string by value in the macro.

## Total today
- 3 commits to origin/main (02dd097e + cce076ab + 41d3aed8)
- 2 unrelated heap-corruption bugs fixed (FlatAST binding_gens_ +
  CHECK macro dangling pointer)
- 1 of them (#300 follow-up) caused the user-reported CI failure;
  the other (#226 pre-existing) was a latent bug surfaced by ASan

## Session 2026-06-30 (continued) — Issue #355 closed

`99e04e49` wires #242's stale-frame detection into all 4 SoA
parent walks (lookup_by_symid_chain + walk_env_frame_roots +
Env::lookup_cell_ptr + Env::lookup_cell_index). New helper
`Evaluator::refresh_stale_frame_in_walk(id, site)` is the
single source of truth for the "saw stale frame during walk"
pattern (bump version_ + bump stats counter + emit
[#242 warning] gated behind AURA_VERBOSE_ENVFRAME).

lookup_cell_index was missing the version check entirely —
the worst of the 4 sites. AC8 explicitly verifies it now
bumps stale_refresh_count_.

18/18 tests in test_issues_jit bundle. 0 ASan errors. No
regression on #242 (which only consumed the helper indirectly
via materialize_call_env).

**Follow-up still open:** pre-existing test_issue_226
doomsday_stress UAF (separate CHECK macro dangling-pointer
bug, fixed earlier this session as 41d3aed8).
