# Env::bindings_ full SymId migration (Issue #174)

**Status:** Design + caller inventory. Migration not started (1-2w of focused work).
**Date:** 2026-06-13
**Workstream:** 3 of #145 (Phase 2)

## Goal

Drop `Env::bindings_` (the legacy string-keyed `std::vector<std::pair<std::string, EvalValue>>`)
entirely. Use only `Env::bindings_symid_` (the parallel SymId-keyed array). Every Env
must have a `pool_` (currently optional).

## Caller inventory (52 sites)

Identified by `grep -rE "bindings_\b|\.bindings\(\)"` (excluding `bindings_symid_`):

### Category 1: Env's own methods (~10 sites, evaluator.ixx)
- `bind(string, value)` — line 86: keeps bindings_ as primary, calls bind_symid to mirror
- `bindings()` accessor — line 133-135: returns `vector<pair<string, EvalValue>>&`
- `bindings_` member — line 166 (primary Env)
- `bindings_` member — line 220 (EnvView's bindings_ for iteration)
- `mirrors to bindings_ when pool_ is set` — line 93, 230 (the dual-path logic)

**Migration**: remove the string-keyed fields, make `bindings_symid_` the only storage.
`bindings()` returns `span<pair<SymId, EvalValue>>`. `bind(string, value)` becomes
`pool_->intern(name); bind_symid(sym, value)`.

### Category 2: evaluator_impl.cpp callers (~25 sites)
- `bindings_.rbegin()` / `bindings_.rend()` lookups in apply_closure / copy_env (lines 148, 189, 211, 235)
- `bindings_.emplace_back` mirrors in bind (line 211)
- `bindings()` iteration in `fr.bindings_.assign(...)` (line 254)
- `bindings()` return in `v.string_bindings = env.bindings()` (line 308)
- `bindings()` iteration in `(run-tests)` primitive (line 3237+)
- The `for (auto& b : bindings_)` / `for (auto& b : f.bindings_)` / `for (auto& b : p->bindings_)` loops
  in various primitives (lines 380+, 410+, 500+)

**Migration**: each `bindings_.X` becomes `bindings_symid_.X`. The `bind(string, ...)` callers
call `pool_->intern(name)` first, then `bind_symid(sym, ...)`. The `(run-tests)` primitive
uses `pool_->resolve(sym_id)` to get the name for display.

### Category 3: service.ixx callers (~7 sites)
- `user_bindings_` (separate from Env::bindings_; unrelated to the migration — different
  mechanism for tracking "user-defined" symbols)
- `for (auto& b : const_cast<aura::compiler::Env&>(*e).bindings())` in `eval_string_for_test`
  primitive (line ~4800)
- 2 more `const_cast<Env&>(...).bindings()` in display logic (lines ~5000, ~5100)

**Migration**: same pattern as Category 2 — use `bindings_symid_()` accessor, resolve
SymId via `pool_->resolve()` for display.

### Category 4: destructors / cleanup (~2 sites)
- `loop manually called env->~Env() to free bindings_' heap` (line 316, evaluator.ixx)
- The std::string in `bindings_` is the heap allocation that needs freeing

**Migration**: with bindings_ gone, the destructor becomes trivial (no heap strings to free).

## Migration plan (per ship cycle)

The full migration is 1-2 weeks of focused work, but it can be broken into
shippable units that don't break the world:

### Cycle 1 (smallest — no behavior change)
- Add `Env::bindings_symid_iter()` accessor that returns `span<pair<SymId, EvalValue>>`
  (a view over the existing array)
- Add `Env::bindings_with_names()` accessor that materializes the named
  version (uses `pool_->resolve()`) — the current behavior, but as a
  derived view rather than primary storage
- Add a metric: `bindings_legacy_uses` counter that bumps on every
  access to the legacy `bindings_` / `bindings()` accessor
- Test: verify the new accessor returns the same data as the old one
- This commit ships the API surface + observability; no callers migrated

### Cycle 2 (migrate evaluator_impl.cpp's internal loops)
- The 25 sites in evaluator_impl.cpp are internal; migrate them
  one file at a time
- The dual-path `bind(string, value)` becomes `bind(SymId, value)`
  + a `bind_string(const std::string&, value)` convenience that
  interns and calls bind_symid
- This commit migrates internal code; the public Env API still
  has the old methods (now thin wrappers over the new ones)

### Cycle 3 (migrate service.ixx)
- The 7 sites in service.ixx use the public API
- The (run-tests) primitive needs `pool_->resolve(sym_id)` for display
- This commit migrates public-API consumers

### Cycle 4 (drop bindings_ entirely)
- Once no callers use the legacy accessors, delete the field
- The destructor becomes trivial
- The metric counter goes to 0 in all tests (verify with
  test_issue_174.cpp)
- This commit ships the actual drop; "storage halved" benefit
  is realized

## Test infrastructure (test_issue_174.cpp, new)

### Test 1: bindings_symid_iter() returns same data as bindings()
- Build an Env with 3 bindings (via the SymId API)
- Call `bindings_symid_iter()` and `bindings()` (legacy)
- Verify they iterate the same (SymId, EvalValue) pairs
  (the legacy accessor needs `pool_->resolve()` to get the name)

### Test 2: bindings_legacy_uses counter starts at 0 and bumps on access
- Fresh Env: counter == 0
- Call `env.bindings()` once: counter == 1
- Call `env.bindings_symid_iter()` once: counter == 0 (no bump on new API)

### Test 3: bind_symid works without pool_
- This is a precondition for the migration: the SymId path
  must work even when pool_ is null (it does today, but a
  test prevents regression)

### Test 4: bind(string, value) convenience routes through intern + bind_symid
- Build an Env with a pool
- Call `env.bind("foo", value)` — verify it ends up in
  bindings_symid_ with the right SymId

## Effort estimate

- Cycle 1: 0.5 day (add accessors + metric + test)
- Cycle 2: 3-5 days (25 sites, each is small)
- Cycle 3: 1-2 days (7 sites in service.ixx)
- Cycle 4: 0.5 day (delete the field, verify tests)
- Total: 1-2 weeks focused work

The marathon session this design doc was written in (3 hours,
16 commits) closed the related workstreams #172 (EnvFrame SoA)
and #173 (heap vectors + stable IDs). #174 is the natural next
step in the #145 Phase 2 / Phase 2.5 progression.

## Why this is a follow-up (not in the original #174 commit)

The migration has 52 caller sites across 2 major files. Migrating
them in one commit is feasible (the diff is mechanical) but the
blast radius is large — any mistake in the bindings_ removal would
break 9/9 test suites simultaneously. The 4-cycle plan above
keeps each commit small and verifiable, with the metric counter
providing observability for the legacy-accessor path during the
migration.
