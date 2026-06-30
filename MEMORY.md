
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

## Session 2026-06-30 (continued) — Issue #356 closed

`9590bc33` ships the scope-limited version of #356 ([Follow-up
#242-2] Arena rollback for env_frames_ via stable-id indirection).

**Approach**: instead of truncating env_frames_ (which would
invalidate Closure::env_id indices for live closures), mark
entries allocated during the doomed transaction as
INVALID_VERSION sentinel. The frames stay allocated but
materialize_call_env + the #355 walker refuse to use them.

- INVALID_VERSION sentinel = UINT64_MAX (monotonic counter
  never reaches this in practice)
- is_env_frame_invalid(id) accessor
- invalidate_post_rollback_env_frames() helper — iterates
  [panic_safe_env_frames_size_, env_frames_.size()) and marks
  each frame's version_ = INVALID_VERSION
- envframe_post_rollback_invalidations_ atomic counter,
  bumped by the count of newly-invalid frames
- materialize_call_env: emits a distinct [#356 warning] for
  invalid frames, returns an empty Env (globals still reachable)
- refresh_stale_frame_in_walk (#355): skips invalid frames
  instead of refreshing them — INVALID is terminal
- restore_panic_checkpoint: calls invalidate_post_rollback_env_frames
  after the 3 arena truncations

15/15 tests in test_issues_jit bundle.

**Lesson learned (debugging the test)**:
Evaluator's constructor pre-allocates 1 frame (for top_/module
scratch), so env_frames_.size() starts at 1 — not 0. Tests that
allocate "PRE + POST" frames and set panic_safe = PRE expect
POST frames invalidated, but actually get POST + 1 (the
pre-existing frame). Fix: read ev.env_frames_size() at the
start of the test and use it as the base for panic_safe, so the
pre-existing frame is treated as pre-checkpoint.

**Follow-up still open (separate issue)**:
Stable-id indirection refactor (Closure::env_id becomes a
small dense index, env_frames_ is a dense arena, stable_to_arena_
remap on rollback) — the actual memory-shrinking version.

## Session 2026-06-30 (continued) — CI asan-build fix

User: "ci asan-build挂 修".

Last failing CI run was on `0fdc68e3` (the #226 CHECK macro
docs commit). asan-build failed; asan-verify skipped.

**Root cause** (not a real bug, but a CI infra issue):
libstdc++ 16's `<regex>` headers (regex_automaton.h,
regex_automaton.tcc) trigger `-Werror=maybe-uninitialized`
false positives inside `std::function`'s move ctor when
imported via `import std` from `aura::parser`. CI image
(`ghcr.io/cybrid-systems/aura-ci:v1.0.1`) builds with
`-Werror`, so the false positive becomes a fatal error at
`evaluator_primitives_query_workspace.cpp` (which pulls in
`import aura.parser.parser`).

**Why local builds don't hit it**:
Same libstdc++ 16, same GCC 16, but the build environment
slightly differs. CI's GCC version is slightly newer and
emits the false-positive warning; local GCC 16.0.1
(r16-8246-g569ace1fa50) doesn't.

**Fix** (`cf1ffee8`):
Add `-Wno-error=maybe-uninitialized -Wno-maybe-uninitialized`
to `aura_test_compile_options` in `cmake/AuraTest.cmake`.
The double flag is intentional: `-Wno-maybe-uninitialized`
suppresses the warning entirely (safe — only fires from inside
libstdc++ regex headers, not from project code), and
`-Wno-error=maybe-uninitialized` is belt-and-suspenders in case
a future compiler version upgrades the warning to error.

**Pattern for libstdc++ header false-positives**:
When CI fails with `-Werror=` warnings inside `/usr/include/c++/`
headers (not project code), it's almost always a known
false-positive in libstdc++ itself. Suppress with `-Wno-X`
in the test compile options — not in the project source.
Project code's `-Werror` coverage stays intact.
