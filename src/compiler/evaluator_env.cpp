// evaluator_env.cpp — P1-d: Env, EnvFrame, EnvView, and env-frame arena
// aura.compiler.evaluator module partition.

module;

#include "runtime_shared.h"
#include "observability_metrics.h"

module aura.compiler.evaluator;

import std;
import aura.core.ast;
import aura.compiler.value;

namespace aura::compiler {

using types::EvalValue;
// Issue #918 Phase 1: explicit using-declarations (no `using namespace`).
using types::as_bool;
using types::as_cell_id;
using types::as_closure_id;
using types::as_float;
using types::as_hash_idx;
using types::as_int;
using types::as_pair_idx;
using types::as_primitive_slot;
using types::as_string_idx;
using types::as_vector_idx;
using types::EvalValue;
using types::is_bool;
using types::is_cell;
using types::is_closure;
using types::is_error;
using types::is_float;
using types::is_hash;
using types::is_int;
using types::is_pair;
using types::is_primitive;
using types::is_string;
using types::is_vector;
using types::is_void;
using types::make_bool;
using types::make_cell;
using types::make_closure;
using types::make_error;
using types::make_float;
using types::make_hash;
using types::make_int;
using types::make_pair;
using types::make_primitive;
using types::make_string;
using types::make_vector;
using types::make_void;

// Depth guard: protects Env::lookup against cyclic parent chains
// (thread_local since lookup can be called from multiple fibers)
static constexpr std::size_t MAX_ENV_DEPTH = 1024;
thread_local std::size_t g_env_lookup_depth = 0;
std::optional<EvalValue> Env::lookup(std::string_view n) const {
    // The pre (!n.empty()) is on the declaration in evaluator.ixx.
    if (++g_env_lookup_depth > MAX_ENV_DEPTH) {
        --g_env_lookup_depth;
        return std::nullopt;
    }
    struct _ {
        ~_() { --g_env_lookup_depth; }
    } dec;

    // 1. Check local bindings — Issue #894: O(1) via binding_index_
    if (auto it = binding_index_.find(n); it != binding_index_.end()) {
        const auto idx = it->second;
        if (idx < bindings_.size() && bindings_[idx].first == n)
            return bindings_[idx].second;
    }
    // Fallback linear scan (index may be stale after rare rewrites)
    for (auto it = bindings_.rbegin(); it != bindings_.rend(); ++it)
        if (it->first == n) {
            return it->second;
        }
    // 1b. Issue #1482 restore: bindings_symid_ is PRIMARY. materialize_call_env
    // copies only the SymId array (string bindings_ stay empty). Free vars /
    // letrec recursive names captured via bind_symid therefore live only here.
    // When pool_ is set (apply_closure sets it from the closure pool), resolve
    // the name via intern and hit the SymId path before walking parents.
    if (pool_ && !bindings_symid_.empty()) {
        auto s = const_cast<aura::ast::StringPool*>(pool_)->intern(n);
        for (auto it = bindings_symid_.rbegin(); it != bindings_symid_.rend(); ++it) {
            if (it->first == s)
                return it->second;
        }
    }
    // 2. Check parent
    if (parent_) {
        return parent_->lookup(n);
    }
    // 2b. Issue #232 fallback: SoA walk via parent_id_ when parent_
    // is null but parent_id_ is set. This happens for env frames
    // materialized by materialize_call_env from a closure captured
    // in a stack env (e.g., named-let → letrec → lambda capture).
    // Without this walk, the closure body can't see bindings
    // from the surrounding scope.
    //
    // Issue #145 P0 follow-up: hold env_frames_mtx_ as a
    // shared lock for the entire walk. env_frame returns a
    // reference into env_frames_; holding the lock prevents
    // the main thread's alloc_env_frame from reallocating the
    // deque's map array underneath us (which would free the
    // map pointer we're reading).
    if (parent_id_ != NULL_ENV_ID && owner_) {
        std::shared_lock<std::shared_mutex> env_rlock(owner_->env_frames_lock());
        const EnvFrame& pfr = owner_->env_frame(parent_id_);
        // Walk the parent frame's bindings (string-keyed)
        for (auto& b : pfr.bindings_) {
            if (b.first == n) {
                if (is_cell(b.second)) {
                    auto ci = as_cell_id(b.second);
                    if (ci < owner_->cells().size())
                        return owner_->cells()[ci];
                }
                return b.second;
            }
        }
        // Issue #1482: EnvFrame primary storage is bindings_symid_ (string
        // bindings_ often empty post-capture). Resolve free vars from outer
        // let/define that only live on the SymId path.
        if (pool_ && !pfr.bindings_symid_.empty()) {
            auto s = const_cast<aura::ast::StringPool*>(pool_)->intern(n);
            for (auto it = pfr.bindings_symid_.rbegin(); it != pfr.bindings_symid_.rend(); ++it) {
                if (it->first == s) {
                    if (is_cell(it->second)) {
                        auto ci = as_cell_id(it->second);
                        if (ci < owner_->cells().size())
                            return owner_->cells()[ci];
                    }
                    return it->second;
                }
            }
        }
        // Recurse via SoA: walk the parent's parent_id_ chain.
        // Capture the result but only return if non-null — if the
        // recursive lookup returns nullopt, fall through to the
        // primitive + ADT fallbacks below (otherwise primitives
        // like `+` and `*` would be reported as unbound variables).
        if (pfr.parent_id != NULL_ENV_ID) {
            Env tmp;
            tmp.set_owner(owner_);
            tmp.set_parent_id(pfr.parent_id);
            if (auto r = tmp.lookup(n))
                return *r;
        }
        // Final fallback: the frame at parent_id_ has the snapshot
        // of the env at capture time. If that env is still live
        // (e.g., it'"'"'s a heap-allocated module env or the top_
        // env), check its live bindings too. The frame is a snapshot
        // and may be stale if bindings were added after the frame
        // was created (e.g., via require in a nested module load).
        // We use the owner_ pointer to find the live env: for index 0
        // it'"'"'s top_, for higher indices we walk the env_frames_
        // pool to find a matching live env. The simplest case
        // (index 0 = top_) is the most common and we handle that
        // directly via owner_->top().
        if (parent_id_ == 0 && owner_) {
            // Check live top_ env'"'"'s bindings
            for (auto it = const_cast<aura::compiler::Env&>(owner_->top_env()).bindings().rbegin();
                 it != const_cast<aura::compiler::Env&>(owner_->top_env()).bindings().rend();
                 ++it) {
                if (it->first == n) {
                    if (is_cell(it->second)) {
                        auto ci = as_cell_id(it->second);
                        if (ci < owner_->cells().size())
                            return owner_->cells()[ci];
                    }
                    return it->second;
                }
            }
        }
    }
    // 3. Fallback: check primitives (allows passing names like `+` as values)
    if (primitives_) {
        auto slot = primitives_->slot_for_name(n);
        if (slot != std::numeric_limits<std::size_t>::max()) {
            return make_primitive(slot);
        }
    }
    // 4. Fallback: check ADT constructors (Issue #108 part 4 Phase 1).
    //    Bypasses Begin scoping. Registered via (adt:register-constructors ...)
    //    from parse_datatype. Returns a primitive ref that, when applied,
    //    builds (cons "CtorName" arg1 arg2 ...).
    {
        // Step 2.3: use per-evaluator adt_runtime_ (FFI pattern).
        // adt_runtime_ lives on the Evaluator, not on Env; access
        // via the owner_ back-pointer (Issue #145 Phase 2.2 set
        // the owner_ on every Env registered with the Evaluator).
        if (owner_) {
            if (auto slot = owner_->adt_runtime().find_ctor(std::string(n)))
                return make_primitive(*slot);
        }
    }
    return std::nullopt;
}

// ── Env::lookup_binding: returns raw binding (cell sentinel as-is) ─
std::optional<EvalValue> Env::lookup_binding(std::string_view n) const {
    // The pre (!n.empty()) is on the declaration in evaluator.ixx.
    for (auto it = bindings_.rbegin(); it != bindings_.rend(); ++it)
        if (it->first == n)
            return it->second;
    return parent_ ? parent_->lookup_binding(n) : std::nullopt;
}

// Issue #145: SymId fast path. Pushes to bindings_symid_
// (canonical) and mirrors to bindings_ (string form) if
// pool_ is set. The mirror is needed because the lambda body
// uses the string-based lookup to find the param (the parser
// interns names but the body's `lookup(name)` does the
// string-keyed loop). Without the mirror, lambda params would
// be invisible to body code.
//
// Issue #1482 Commit 1 made bindings_symid_ PRIMARY and dropped
// the eager mirror, intending #1550/#1551 to migrate Variable
// lookup to SymId. That migration is incomplete: Variable still
// uses Env::lookup(name). Restoring the mirror (when pool_ is set)
// unblocks let/letrec-bound lambdas (tree-walker path) — CI
// gradual/suite/integ/bash regressions ("unbound variable: x").
void Env::bind_symid(aura::ast::SymId s, types::EvalValue v) {
    bind_symid_with_linear_state(s, std::move(v), linear_rt::Untracked);
}

void Env::bind_symid_with_linear_state(aura::ast::SymId s, types::EvalValue v, std::uint8_t state) {
    bindings_symid_.emplace_back(s, std::move(v));
    bindings_linear_ownership_state_.push_back(state);
    // Mirror into string-keyed bindings_ so Env::lookup(name) finds
    // lambda params bound via apply_closure / Path B. Only when pool_
    // can resolve SymId → name (canonical or captured pool).
    if (pool_) {
        const auto name = pool_->resolve(s);
        if (!name.empty()) {
            const auto& val = bindings_symid_.back().second;
            const std::string key(name);
            if (auto it = binding_index_.find(key); it != binding_index_.end() &&
                                                    it->second < bindings_.size() &&
                                                    bindings_[it->second].first == key) {
                bindings_[it->second].second = val;
            } else {
                bindings_.emplace_back(key, val);
                binding_index_[bindings_.back().first] = bindings_.size() - 1;
            }
        }
    }
}

void Env::bind_with_linear_state(std::string_view n, types::EvalValue v, std::uint8_t state) {
    bindings_.emplace_back(std::string(n), std::move(v));
    binding_index_[bindings_.back().first] = bindings_.size() - 1;
    // Keep SymId SoA + linear state in lockstep when pool is set.
    if (pool_) {
        // pool_ is const*; intern is non-const but safe on shared pool.
        auto s = const_cast<aura::ast::StringPool*>(pool_)->intern(n);
        bindings_symid_.emplace_back(s, bindings_.back().second);
        bindings_linear_ownership_state_.push_back(state);
    }
}

bool Env::set_linear_ownership_state(aura::ast::SymId s, std::uint8_t state) {
    for (std::size_t i = bindings_symid_.size(); i > 0; --i) {
        const std::size_t idx = i - 1;
        if (bindings_symid_[idx].first == s) {
            if (idx >= bindings_linear_ownership_state_.size())
                bindings_linear_ownership_state_.resize(bindings_symid_.size(),
                                                        linear_rt::Untracked);
            bindings_linear_ownership_state_[idx] = state;
            return true;
        }
    }
    return false;
}

bool Env::set_linear_ownership_state_by_name(std::string_view n, std::uint8_t state) {
    if (pool_) {
        auto s = const_cast<aura::ast::StringPool*>(pool_)->intern(n);
        if (set_linear_ownership_state(s, state))
            return true;
    }
    return false;
}

// Issue #145: SymId-based lookup. Iterates bindings_symid_
// (which the bind_symid path writes to) with integer compare.
// Most-recent-binding-wins semantics, matching the string-based
// lookup's rbegin/rend iteration order.
std::optional<types::EvalValue> Env::lookup_by_symid(aura::ast::SymId s) const {
    for (auto it = bindings_symid_.rbegin(); it != bindings_symid_.rend(); ++it) {
        if (it->first == s) {
            // P0 step 2: return raw binding (sentinel as-is). No cells_
            // member anymore. Deref (if cell) happens at caller using
            // central Evaluator cells_ when in Evaluator scope, or
            // explicit cells param for mutation paths (lookup_cell_*).
            return it->second;
        }
    }
    // Issue #1128: try SoA owner chain first, then fall through to
    // legacy parent_ so mixed SoA/legacy graphs still resolve.
    if (owner_ && parent_id_ != NULL_ENV_ID) {
        if (auto r = owner_->lookup_by_symid_chain(parent_id_, s))
            return r;
    }
    return parent_ ? parent_->lookup_by_symid(s) : std::nullopt;
}

// Issue #207 (Cycle 1): bindings_with_names() — materializes
// the named version of the bindings. Walks the SymId-keyed
// bindings_symid_ array and resolves each SymId to a name
// string via pool_->resolve(). Returns a new vector with
// (name, value) pairs in the same order as bindings_symid_.
//
// This is the "current behavior, as a derived view" per the
// issue body. The legacy bindings_ array still holds the
// string-keyed version, but new code should use this helper
// to read the named view (rather than reading bindings_
// directly). When the migration completes (Cycle 2+), the
// legacy bindings_ array is dropped and bindings_with_names()
// becomes the only path to get a named view.
//
// For envs without pool_ set, the name is rendered as
// "@<symid:N>" where N is the SymId value. This is a
// fallback for envs that haven't been migrated yet.
std::vector<std::pair<std::string, types::EvalValue>> Env::bindings_with_names() const {
    std::vector<std::pair<std::string, types::EvalValue>> out;
    out.reserve(bindings_symid_.size());
    for (const auto& [sym, val] : bindings_symid_) {
        std::string name;
        if (pool_) {
            // pool_->resolve() returns the canonical name
            // (string_view) for this SymId. If the SymId
            // is not in the pool (defensive), the resolved
            // view is empty.
            std::string_view resolved = pool_->resolve(sym);
            if (!resolved.empty())
                name.assign(resolved.data(), resolved.size());
        }
        if (name.empty()) {
            // Fallback: render the SymId as a string for
            // display purposes.
            name = "@symid:" + std::to_string(sym);
        }
        out.emplace_back(std::move(name), val);
    }
    return out;
}

// Issue #145 follow-up / Phase 2.5.0: lookup_by_intern — the
// SymId-first migration scaffold for the eventual bindings_
// drop. Takes a name string, interns via the given pool
// (legacy closure-captured / env-captured pool for backward
// compat; canonical_pool() for new code), then routes through
// lookup_by_symid. The primitive + ADT-constructor fallbacks
// mirror Env::lookup's behavior (step 3 + 4) so callers that
// switch to this helper get the same observable result as
// lookup(name) when no binding is found.
//
// Note: this is a SCOPED migration tool, not a permanent API.
// When Phase 2.5 ships the actual drop of bindings_, the
// helper either goes away (callers intern once + use
// lookup_by_symid directly) or stays as a thin convenience
// wrapper. The 6 current Env::lookup(name) call sites
// (apply_closure parent walk, EnvView parent walk, module
// lookup, fn_name lookup, eval-time lookup, capture lookup)
// will migrate one per Phase 2.5.0 commit.
std::optional<types::EvalValue> Env::lookup_by_intern(std::string_view n,
                                                      const aura::ast::StringPool* pool) const {
    // Resolve which pool to use: legacy passed-in pool if
    // non-null, else fall back to the env's own pool_ (set
    // via set_pool for closures that captured a non-canonical
    // pool), else nullptr. The lookup_by_symid call below
    // will then route through whichever pool is appropriate.
    // Note: use_pool is non-const because intern() mutates the
    // pool. const_cast is safe here because the env holds the
    // pool by non-const pointer (pool_) or by the caller's
    // pointer (legacy). The lookup_by_intern method itself is
    // logically const (no observable env state change beyond
    // the pool's intern side effect, which is idempotent for
    // already-interned names).
    aura::ast::StringPool* use_pool = const_cast<aura::ast::StringPool*>(pool ? pool : pool_);
    if (!use_pool) {
        // No pool available — can't intern. Fall through to
        // the legacy string-based lookup as a last resort.
        return lookup(n);
    }
    auto sym = use_pool->intern(n);
    auto found = lookup_by_symid(sym);
    if (found)
        return found;
    // Fallbacks mirror Env::lookup's primitive + ADT paths.
    // These are not SymId-specific — the slot_for_name lookup
    // uses the string name directly.
    if (primitives_) {
        auto slot = primitives_->slot_for_name(n);
        if (slot != std::numeric_limits<std::size_t>::max()) {
            return make_primitive(slot);
        }
    }
    {
        // Step 2.3: use per-evaluator adt_runtime_ (FFI pattern).
        // adt_runtime_ lives on the Evaluator, not on Env; access
        // via the owner_ back-pointer (see Env::lookup fix above
        // for the same pattern).
        if (owner_) {
            if (auto slot = owner_->adt_runtime().find_ctor(std::string(n)))
                return make_primitive(*slot);
        }
    }
    return std::nullopt;
}

// ═══════════════════════════════════════════════════════════════
// Issue #145 Phase 2.1 — EnvFrame SoA infrastructure
// ═══════════════════════════════════════════════════════════════
//
// EnvFrame is the SoA counterpart to Env. Same data layout, but
// `parent_id_` (EnvId, uint32_t) replaces `parent_` (Env*). The
// methods below are the "local-only" variants — they operate on
// one frame and do NOT walk the parent chain. Walk-aware access
// lives on Evaluator (walk_env_frames, lookup_by_symid_chain).
//
// Today Env and EnvFrame coexist. Migration is Phase 2.2.

// EnvFrame::bind — parallel to Env::bind (which writes only
// to bindings_, no mirror). SymId mirroring happens via
// bind_symid (the fast path). If you need both arrays in sync,
// bind via SymId + have pool_ set.
void EnvFrame::bind(const std::string& n, types::EvalValue v) {
    bind_with_linear_state(n, std::move(v), linear_rt::Untracked);
}

// EnvFrame::bind_symid — parallel to Env::bind_symid. Mirrors
// to bindings_ when pool_ is set so legacy lookup(string)
// callers still find the param.
void EnvFrame::bind_symid(aura::ast::SymId s, types::EvalValue v) {
    bind_symid_with_linear_state(s, std::move(v), linear_rt::Untracked);
}

// Issue #1539: bind with explicit linear ownership state.
void EnvFrame::bind_with_linear_state(const std::string& n, types::EvalValue v,
                                      std::uint8_t state) {
    bindings_.emplace_back(n, v);
    // If pool_ can resolve name → SymId, keep primary SoA in sync.
    if (pool_) {
        auto s = const_cast<aura::ast::StringPool*>(pool_)->intern(n);
        bindings_symid_.emplace_back(s, v);
        bindings_linear_ownership_state_.push_back(state);
    }
}

void EnvFrame::bind_symid_with_linear_state(aura::ast::SymId s, types::EvalValue v,
                                            std::uint8_t state) {
    bindings_symid_.emplace_back(s, v);
    bindings_linear_ownership_state_.push_back(state);
    if (pool_) {
        std::string_view sv = pool_->resolve(s);
        if (!sv.empty())
            bindings_.emplace_back(std::string(sv), v);
    }
}

bool EnvFrame::set_linear_ownership_state(aura::ast::SymId s, std::uint8_t state) {
    for (std::size_t i = bindings_symid_.size(); i > 0; --i) {
        const std::size_t idx = i - 1;
        if (bindings_symid_[idx].first == s) {
            if (idx >= bindings_linear_ownership_state_.size())
                bindings_linear_ownership_state_.resize(bindings_symid_.size(),
                                                        linear_rt::Untracked);
            bindings_linear_ownership_state_[idx] = state;
            return true;
        }
    }
    return false;
}

bool EnvFrame::set_linear_ownership_state_by_name(const std::string& n, std::uint8_t state) {
    if (!pool_)
        return false;
    return set_linear_ownership_state(const_cast<aura::ast::StringPool*>(pool_)->intern(n), state);
}

// EnvFrame::lookup_local — Env::lookup minus parent walk +
// primitive + ADT fallbacks. Pure frame-local lookup. Use
// Evaluator::walk_env_frames for chain-aware lookup.
//
// P0 migration: EnvFrame no longer holds a cells_ pointer
// (removed from struct for pure data / no pointer-to-heap).
// If the bound value is a cell sentinel, we return it as-is.
// Cell resolution is centralized in the Evaluator's
// lookup_by_symid_chain (and legacy Env paths) using the
// owning Evaluator's central cells_ pmr::vector. This
// makes frames fully index-driven and reallocation-safe.
std::optional<types::EvalValue> EnvFrame::lookup_local(const std::string& n) const {
    for (auto it = bindings_.rbegin(); it != bindings_.rend(); ++it) {
        if (it->first == n) {
            return it
                ->second; // sentinel returned; deref happens at caller (Evaluator chain or legacy)
        }
    }
    return std::nullopt;
}

// EnvFrame::lookup_local_by_symid — Env::lookup_by_symid minus
// parent walk. Pure frame-local SymId compare.
//
// P0 migration: same as lookup_local — return raw sentinel
// (cell or not). Central deref lives in Evaluator.
std::optional<types::EvalValue> EnvFrame::lookup_local_by_symid(aura::ast::SymId s) const {
    for (auto it = bindings_symid_.rbegin(); it != bindings_symid_.rend(); ++it) {
        if (it->first == s) {
            return it->second;
        }
    }
    return std::nullopt;
}

// Evaluator::alloc_env_frame — append a new EnvFrame and return
// its id. The id is the new size()-1 of env_frames_. Returns
// NULL_ENV_ID on overflow (>4G envs, which is unreachable in
// practice for a single evaluator lifetime).
//
// P0 (EnvFrame SoA): frames are allocated with only parent_id +
// primitives_. No cells_ pointer (pure data). Cell resolution
// centralized later in lookup paths using Evaluator::cells_.
aura::compiler::EnvId Evaluator::alloc_env_frame(EnvId parent_id, const Primitives* primitives) {
    // Issue #145 P0 follow-up: unique_lock on env_frames_mtx_
    // to serialize push_back with fiber-thread readers
    // (materialize_call_env). The deque's map array can be
    // reallocated when the deque grows past the current map
    // capacity, freeing the map pointer a fiber thread is
    // reading via env_frame[id]. Unique lock makes the
    // push_back atomic with respect to readers.
    std::unique_lock<std::shared_mutex> wlock(env_frames_mtx_);
    if (env_frames_.size() >= NULL_ENV_ID) {
        // 4G envs reached. Return NULL to signal overflow;
        // callers should treat this as fatal (env allocation
        // exhausted) — but we keep going rather than abort
        // to make the failure mode visible.
        return NULL_ENV_ID;
    }
    // Issue #1384: construct the frame locally with version_ =
    // current defuse_version_ BEFORE push_back, so any reader
    // that observes the new index via env_frames_.size() sees a
    // valid version_ (not the default 0, which is the "never
    // stamped" sentinel and would be wrongly classified as stale
    // by is_env_frame_stale once defuse_version_ > 0).
    EnvFrame fr(parent_id, primitives, defuse_version_.load(std::memory_order_acquire));
    env_frames_.push_back(std::move(fr));
    return static_cast<EnvId>(env_frames_.size() - 1);
}

// Evaluator::alloc_env_frame_from_env — Issue #145 Phase 2.3.
// Mirror an Env's parallel binding arrays into a new frame in
// env_frames_. The new frame's parent_id defaults to
// e.parent_id() (preserving the captured env's parent-chain
// position in the SoA arena); callers can override with an
// explicit parent_id if they want to re-parent the frame.
//
// Issue #1482 restore: copy BOTH bindings_symid_ (PRIMARY) and
// legacy string bindings_ into the frame. Define / multi-define
// begin often bind via Env::bind without a pool (string-only); if
// we drop string bindings at capture, set! free vars (total-pass)
// and recursive nested defines (seen?) become unbound after
// materialize_call_env. SymId path remains primary for new binds
// that go through bind_symid / bind_with_linear_state(+pool).
//
// primitives_/pool_/cells_ deliberately not part of the frame
// (cells_ removed entirely from EnvFrame struct in P0 step 1;
// resolution centralized on Evaluator). This keeps frames
// as pure, index-only data.
aura::compiler::EnvId Evaluator::alloc_env_frame_from_env(const Env& e, EnvId parent_id) {
    EnvId pid = (parent_id != NULL_ENV_ID) ? parent_id : e.parent_id();
    EnvId id = alloc_env_frame(pid);
    if (id == NULL_ENV_ID)
        return NULL_ENV_ID;
    // Issue #145 P0 follow-up: hold a shared lock while
    // mutating the freshly-allocated frame. A concurrent
    // alloc_env_frame on another thread (e.g. a fiber)
    // could push_back and reallocate env_frames_'s map
    // array, invalidating our `fr` reference.
    std::unique_lock<std::shared_mutex> wlock(env_frames_mtx_);
    EnvFrame& fr = env_frames_[id];
    // e is `const`, so .bindings()/.bindings_symid() return
    // std::span (const overload).
    auto bss = e.bindings_symid();
    fr.bindings_symid_.assign(bss.begin(), bss.end());
    // Dual-path capture: preserve string-keyed cells (Define without pool).
    auto bs = e.bindings();
    fr.bindings_.assign(bs.begin(), bs.end());
    // Issue #1539: copy linear ownership SoA (pad/truncate to match).
    auto los = e.bindings_linear_ownership_state();
    fr.bindings_linear_ownership_state_.assign(los.begin(), los.end());
    if (fr.bindings_linear_ownership_state_.size() < fr.bindings_symid_.size()) {
        fr.bindings_linear_ownership_state_.resize(fr.bindings_symid_.size(), linear_rt::Untracked);
    } else if (fr.bindings_linear_ownership_state_.size() > fr.bindings_symid_.size()) {
        fr.bindings_linear_ownership_state_.resize(fr.bindings_symid_.size());
    }
    ensure_envframe_dual_path_consistency(fr);
    // Issue #1384: re-stamp version_ AFTER all assignments so the
    // frame captures defuse_version_ at COMPLETION, not at the
    // moment alloc_env_frame returned. Without this, a defuse
    // bump landing between alloc_env_frame and the assignments
    // would leave the frame with a stale version_ (frame is
    // wrongly classified as stale by is_env_frame_stale even
    // though its bindings are current).
    fr.version_ = defuse_version_.load(std::memory_order_acquire);
    return id;
}

// Issue #1482 Commit 3 (desync strengthen, part A): post-Commit 1/2 the legacy
// `bindings_` array is no longer eagerly populated — it stays empty until a
// caller invokes `Env::bindings_with_names()` (which materializes via
// `pool_->resolve()` + `"@<symid:N>"` fallback). The legacy size-only
// check `bindings_.size() != bindings_symid_.size()` will now always fire
// (0 vs N) for any non-empty frame, which is observability noise, not
// semantic desync. Kept the legacy `bump_envframe_desync_detected()` counter
// as a "lazy materialization not yet fired" hint; the real semantic check
// (compare `Env::bindings_with_names()` output against a synthesized expected
// view from `bindings_symid_` + `pool_->resolve()`) is deferred to followup
// `#1550` (dual-path consistency validation in walk/GC paths).
//
// Future work (Commit 3 part B, deferred): replace this size-only check with
// a semantic comparison that compares `Env::bindings_with_names()` output
// against a synthesized expected view derived from `bindings_symid_` +
// `pool_->resolve()`. That is O(N) per frame at consistency-check time but
// catches actual mismatches (not just empty-vs-populated).
void Evaluator::ensure_envframe_dual_path_consistency(const EnvFrame& fr) const noexcept {
    if (fr.bindings_.size() != fr.bindings_symid_.size()) {
        bump_envframe_desync_detected();
    } else {
        bump_bindings_dual_sync_count();
    }
}

// Evaluator::materialize_call_env — Issue #145 Phase 2.3.
// Build a fresh Env for evaluating a closure body. When the
// closure's captured env is registered in env_frames_
// (cl.env_id ≠ NULL_ENV_ID), rebuild the call env from the
// frame's bindings and wire the SoA walk (owner_ +
// parent_id_) so lookup_cell_ptr / lookup_cell_index route
// parent lookups through walk_env_frames instead of pointer
// chase. Otherwise fall back to copying the legacy `cl.env`
// raw pointer — this preserves correct behavior for any
// stack-allocated local-eval closures that have not yet been
// registered in the SoA arena.
//
// primitives_/cells_/pool_ are NOT set here — they are
// runtime support pointers that the caller wires after
// materialization (apply_closure sets them inline; TCO tail
// call sites set them via tail_env.set_*). Keeping them out
// of this helper makes the helper usable from any code path
// that has a closure but might not need a fully wired env.
Env Evaluator::materialize_call_env(const Closure& cl) {
    // Issue #417: cross-TU invariant probe on Env materialization.
    ensure_mutation_invariants();
    // P0 complete: legacy cl.env path removed. All closures have
    // env_id set at capture time (via alloc_env_frame_from_env).
    // Always use SoA path for GC-safety and no pointer chasing.
    Env ne;
    // Defensive: a closure with env_id == NULL_ENV_ID would
    // trip the env_frame() contract and crash. This can happen
    // when the closure was constructed via a path that
    // skipped alloc_env_frame_from_env (e.g. a ClosureView
    // copy, a default-constructed Closure moved into
    // closures_[cid], or a frame that was pruned by a
    // future gc-temp cycle). Fall back to a fresh Env with
    // no captured bindings — the body will see globals via
    // the workspace walk, which is correct for lambda
    // bodies that don't actually reference the captured
    // scope (the most common case for this failure mode).
    if (cl.env_id == NULL_ENV_ID || cl.env_id >= env_frames_.size()) {
        // Issue #1510: post-compact cleared env_id or OOB → empty Env
        // fallback (globals still reachable; no dangling frame walk).
        if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics_))
            m->materialize_fallback_total.fetch_add(1, std::memory_order_relaxed);
        return ne;
    }
    // Issue #1542: linear post-mutate enforce at materialize entry
    // (parity with apply_closure → closure_needs_safe_fallback).
    // Covers TCO / eval_data_as_code sites that call materialize_call_env
    // without going through the apply_closure dual check.
    //
    // Call BEFORE taking env_frames_mtx_: linear_post_mutate_enforce
    // acquires its own shared lock, and std::shared_mutex is not
    // recursive — nesting would deadlock. Same counter family as
    // #1478 (`linear_post_mutate_enforcements`).
    //
    // On Moved capture: safe fallback to empty Env (globals still
    // reachable) — matches INVALID_VERSION / OOB recovery shape so
    // call sites never walk a frame with use-after-move tags.
    if (!linear_post_mutate_enforce(cl.env_id)) {
        ne.set_env_version(defuse_version_.load(std::memory_order_acquire));
        if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics_))
            m->materialize_fallback_total.fetch_add(1, std::memory_order_relaxed);
        return ne;
    }
    // Issue #145 P0 follow-up: hold env_frames_mtx_ shared
    // lock for the duration of the frame read. The frame
    // reference must remain valid through .bindings(),
    // .bindings_symid_(), and .parent_id reads. Without
    // this lock, a concurrent alloc_env_frame on the main
    // thread (during eval, mutate:rebind, etc.) could
    // reallocate the deque's map array, freeing the map
    // pointer a fiber thread (running apply_closure →
    // materialize_call_env) is reading.
    std::shared_lock<std::shared_mutex> env_rlock(env_frames_mtx_);
    const EnvFrame& fr = env_frame(cl.env_id);
    // Issue #683: linear-capturing closure materialize gate — bridge_epoch
    // tracked closures participate in linear ownership + EnvFrame version_.
    if (cl.bridge_epoch != 0 && compiler_metrics_) {
        const auto cur_ver = defuse_version_.load(std::memory_order_acquire);
        const auto ok = validate_linear_ownership_state(1, fr.version_, cur_ver, cl.bridge_epoch,
                                                        current_bridge_epoch());
        auto* m = static_cast<struct CompilerMetrics*>(compiler_metrics_);
        m->linear_post_mutate_enforcements_total.fetch_add(1, std::memory_order_relaxed);
        if (!ok) {
            m->linear_violations_caught_total.fetch_add(1, std::memory_order_relaxed);
            m->linear_deopt_on_mismatch_total.fetch_add(1, std::memory_order_relaxed);
            m->linear_gc_safepoint_violations.fetch_add(1, std::memory_order_relaxed);
            m->linear_postmutate_escape_violations_prevented_total.fetch_add(
                1, std::memory_order_relaxed);
        } else {
            m->linear_check_pass_count_.fetch_add(1, std::memory_order_relaxed);
            m->linear_postmutate_guard_boundary_linear_safe_total.fetch_add(
                1, std::memory_order_relaxed);
            m->linear_postmutate_env_version_sync_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    // Issue #418: dual-path consistency probe on materialize.
    ensure_envframe_dual_path_consistency(fr);
    // Issue #1269: enforce dual-path + version stamp on every
    // materialize path — refresh stale frames before binding walk
    // so legacy bindings_ cannot bypass SymId version checks.
    if (is_env_frame_stale(cl.env_id)) {
        refresh_stale_frame_in_walk(cl.env_id, "materialize_call_env");
        if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics_))
            m->envframe_dualpath_materialize_refresh.fetch_add(1, std::memory_order_relaxed);
    }
    // Issue #242: detect a stale frame (captured before the
    // current mutation epoch). The frame's bindings might be
    // inconsistent with the post-mutation state — log a
    // warning + bump the frame's version_ so subsequent
    // lookups see it as fresh. We don't refresh the bindings
    // themselves (that would require re-capturing against a
    // new env, which is out of scope for the P0 ship); the
    // warning + version bump is enough to make the staleness
    // observable and prevent repeated warnings.
    //
    // Issue #317 follow-up: the version bump + stats counter
    // bump are still unconditional (the future-proofing is
    // cheap and the observability counter is essential for
    // (query:envframe-dualpath-stats)). The stderr warning
    // emission is now gated on the AURA_VERBOSE_ENVFRAME env
    // var — production runs + the default CI flow stay silent,
    // while operators debugging a real staleness issue can set
    // the var to opt back in. The test suite (suite/edsl_self
    // _test.aura) uses default settings, so the warning no
    // longer appears in the suite output.
    if (fr.version_ == INVALID_VERSION) {
        // Issue #356: post-rollback invalid frame. Do NOT
        // refresh — the bindings may reference AST nodes / pool
        // strings that no longer exist. Emit a distinct
        // warning (gated behind AURA_VERBOSE_ENVFRAME, same as
        // the regular stale warning) and return an empty Env so
        // the closure body sees no captured bindings (it can
        // still find globals via the workspace walk).
        static const char* verbose_env = std::getenv("AURA_VERBOSE_ENVFRAME");
        if (verbose_env && verbose_env[0] != '0' && verbose_env[0] != '\0') {
            std::println(std::cerr,
                         "[#356 warning] materialize_call_env: post-rollback "
                         "EnvFrame id={} (frame.version_=INVALID). "
                         "Bindings skipped — closure captured against a doomed "
                         "transaction. Returning empty Env (globals still reachable).",
                         cl.env_id);
        }
        // Still stamp the empty Env with the current version so
        // downstream callers see a consistent snapshot.
        ne.set_env_version(defuse_version_.load(std::memory_order_acquire));
        // Issue #1510: post-rollback / invalid frame → fallback path.
        if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics_))
            m->materialize_fallback_total.fetch_add(1, std::memory_order_relaxed);
        return ne;
    }
    if (fr.version_ < defuse_version_.load(std::memory_order_acquire)) {
        // Mutate the version under the same shared lock. A
        // shared_lock allows multiple readers but blocks
        // writers (alloc_env_frame); since we're not adding
        // or removing frames, just updating a metadata
        // field, the shared lock is sufficient (no other
        // reader depends on version_ being immutable).
        const_cast<EnvFrame&>(fr).version_ = defuse_version_.load(std::memory_order_acquire);
        // Issue #543: bump the envframe_stale_refresh_count_
        // observability counter so the (query:envframe-
        // dualpath-stats) primitive can report stale-refresh
        // events for production monitoring. Stats-only
        // (relaxed-ordering); doesn't affect control flow.
        bump_envframe_stale_refresh_count();
        // Issue #741: EnvFrame version re-stamp on materialize for
        // quote/lambda closures held across partial re-lower.
        bump_incremental_closure_env_version_resync();
        // Issue #317 follow-up: emit the diagnostic only when
        // the operator has opted in. Default silent. The
        // (query:envframe-dualpath-stats) primitive is the
        // canonical source for staleness telemetry; this
        // stderr message is for one-off debugging.
        static const char* verbose_env = std::getenv("AURA_VERBOSE_ENVFRAME");
        if (verbose_env && verbose_env[0] != '0' && verbose_env[0] != '\0') {
            // Logging is best-effort — a fiber thread might not
            // have a tty. We use std::println(std::cerr, ...) so
            // the warning is always emitted (not just in debug).
            std::println(std::cerr,
                         "[#242 warning] materialize_call_env: stale EnvFrame id={} "
                         "(frame.version_={}, current defuse_version_={}). "
                         "Bindings may be inconsistent with post-mutation state. "
                         "Bumped frame.version_ to silence future warnings.",
                         cl.env_id, fr.version_, defuse_version_.load(std::memory_order_acquire));
        }
    }
    // Issue #1482 restore: rehydrate BOTH paths from the capture frame.
    // SymId PRIMARY for new code; string bindings for set!/lookup and
    // defines that never had a pool at bind time.
    ne.bindings_symid_mut() = fr.bindings_symid_;
    if (!fr.bindings_.empty())
        ne.replace_string_bindings(fr.bindings_);
    // Issue #1539: copy linear ownership SoA into materialized Env.
    ne.bindings_linear_ownership_state_mut() = fr.bindings_linear_ownership_state_;
    if (ne.bindings_linear_ownership_state_mut().size() < ne.bindings_symid().size()) {
        ne.bindings_linear_ownership_state_mut().resize(ne.bindings_symid().size(),
                                                        linear_rt::Untracked);
    }
    // Also need access from Env for alloc_env_frame_from_env copy source:
    // fr.bindings_linear_ownership_state_ is public on EnvFrame.
    if (fr.parent_id != NULL_ENV_ID) {
        ne.set_owner(this);
        ne.set_parent_id(fr.parent_id);
    }
    // Issue #286: stamp the new Env with the current
    // defuse_version_. This gives the materialized Env the same
    // snapshot semantics as the EnvFrame it was built from,
    // and lets future lookups via walk_env_frames (or
    // Env::lookup_cell_ptr) cheaply detect a stale chain by
    // comparing this stamp against the owner's current
    // defuse_version_. The stamp is captured AFTER the frame
    // version_ check above so a stale frame's bindings are
    // re-stamped to current, and the new Env reflects that
    // current value.
    ne.set_env_version(defuse_version_.load(std::memory_order_acquire));
    return ne;
}

// Issue #1545: walk tree-walker live closures_ under unique lock so
// callers can mutate (e.g. mark invalid). Parallel to
// AuraJIT::walk_active_closures / IRExecutor::walk_runtime_closures.
void Evaluator::walk_active_closures(const ActiveClosureWalkFn& fn) {
    if (!fn)
        return;
    std::unique_lock<std::shared_mutex> wlock(closures_mtx_);
    for (auto& [id, cl] : closures_)
        fn(id, cl);
}

// Issue #1545 / #1486 / #1494: scan live closures for linear captures.
// A capture is "linear" if the EnvFrame SoA has any state != Untracked.
// mark_invalid → stamp Closure::bridge_epoch = 0 so apply_closure /
// closure_needs_safe_fallback takes the safe path (is_bridge_stale).
// only_if_moved: restrict mark to closures with at least one Moved
// binding (mutation-boundary closed-loop; invalidate uses false).
// filter_env_id: when not NULL_ENV_ID, only closures whose env_id
// matches are considered (#1494 env-scoped scan on enforce path).
Evaluator::LinearLiveClosureScanResult
Evaluator::scan_live_closures_for_linear_captures(bool mark_invalid, bool only_if_moved,
                                                  EnvId filter_env_id) noexcept {
    LinearLiveClosureScanResult out;
    if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics_))
        m->linear_live_closure_scans_total.fetch_add(1, std::memory_order_relaxed);

    // Lock order: closures unique → env_frames shared (same as apply
    // materialize relative ordering; no reverse acquire elsewhere).
    std::unique_lock<std::shared_mutex> cl_lock(closures_mtx_);
    std::shared_lock<std::shared_mutex> env_lock(env_frames_mtx_);
    for (auto& [id, cl] : closures_) {
        (void)id;
        ++out.examined;
        if (cl.env_id == NULL_ENV_ID || cl.env_id >= env_frames_.size())
            continue;
        if (filter_env_id != NULL_ENV_ID && cl.env_id != filter_env_id)
            continue;
        const EnvFrame& fr = env_frames_[cl.env_id];
        bool has_linear = false;
        bool has_moved = false;
        for (const auto s : fr.bindings_linear_ownership_state_) {
            if (s != linear_rt::Untracked)
                has_linear = true;
            if (s == linear_rt::Moved)
                has_moved = true;
        }
        if (!has_linear)
            continue;
        ++out.with_linear_capture;
        if (has_moved)
            ++out.with_moved_capture;
        const bool should_mark = mark_invalid && (!only_if_moved || has_moved);
        if (should_mark) {
            // Force safe_fallback on next apply regardless of later
            // restamp attempts that only update matching epochs.
            cl.bridge_epoch = 0;
            ++out.marked_invalid;
            if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics_)) {
                m->linear_live_closures_marked_invalid_total.fetch_add(1,
                                                                       std::memory_order_relaxed);
                // Also surface on the safe-fallback family so agents
                // correlating #1475 / #1545 see the invalidate path.
                m->compiler_closure_safe_fallbacks.fetch_add(1, std::memory_order_relaxed);
                // Issue #1494 AC1: mark-invalid on Moved is a prevented
                // use-after-move (align with linear_post_mutate_enforce).
                if (has_moved)
                    m->linear_ownership_violation_prevented.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    return out;
}

ClosureId Evaluator::register_active_closure(Closure cl) {
    stamp_closure_bridge_epoch(cl);
    const ClosureId id = next_id();
    std::unique_lock<std::shared_mutex> wlock(closures_mtx_);
    closures_[id] = std::move(cl);
    return id;
}

std::optional<Closure> Evaluator::find_active_closure(ClosureId id) const {
    std::shared_lock<std::shared_mutex> rlock(closures_mtx_);
    auto it = closures_.find(id);
    if (it == closures_.end())
        return std::nullopt;
    return it->second;
}

// Issue #242: is_env_frame_stale — true if the frame's
// stamped version is older than the current defuse_version_
// (i.e. captured before a mutation that may have invalidated
// the captured scope). Returns true for invalid ids as a
// safety net so callers can treat invalid frames as stale.
bool Evaluator::is_env_frame_stale(EnvId id) const {
    if (id == NULL_ENV_ID || id >= env_frames_.size())
        return true;
    // env_frames_ is a deque guarded by env_frames_mtx_; a
    // shared_lock keeps the frame alive across the load.
    std::shared_lock<std::shared_mutex> rlock(env_frames_mtx_);
    return env_frames_[id].version_ < defuse_version_.load(std::memory_order_acquire);
}

// Issue #1478 / #1539 / #1542: linear post-mutate enforcement.
// Parallel to is_env_frame_stale — extends pre-call safety to
// linear ownership state on captured bindings.
//
// Behavior (#1539 real scan):
//   - Bumps linear_post_mutate_enforcements on every call when
//     env_id has a valid frame.
//   - Walks bindings_linear_ownership_state_; returns false if
//     any binding is Moved (use-after-move / post-mutate).
//
// Entry points:
//   1. apply_closure → closure_needs_safe_fallback (#1478)
//   2. materialize_call_env (#1542) — covers TCO / non-apply sites
//   3. linear_post_mutate_enforce_all (#1538) + JIT probe (#1540)
//
// Coordination with dual-epoch contract:
//   - Read-side: #1475 is_env_frame_stale + this helper.
//   - Write-side: #1476 atomic_bump_epochs_and_stamp_bridge.
//   - JIT-side: #1477 capture_fn_epoch + is_fn_epoch_stale.
bool Evaluator::linear_post_mutate_enforce(EnvId env_id) const noexcept {
    if (env_id == NULL_ENV_ID || env_id >= env_frames_.size())
        return true; // no captures to validate, or invalid id (safety net)
    auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
    if (m) {
        m->linear_post_mutate_enforcements.fetch_add(1, std::memory_order_relaxed);
    }
    // Issue #1539: real per-cell linear scan via bindings_linear_ownership_state_.
    // Return false if any captured binding is Moved (use-after-move / post-mutate
    // violation). Other states (Owned/Borrowed/MutBorrowed/Untracked) are safe.
    std::shared_lock<std::shared_mutex> rlock(env_frames_mtx_);
    if (env_id >= env_frames_.size())
        return true;
    const EnvFrame& fr = env_frames_[env_id];
    const std::size_t n =
        std::min(fr.bindings_symid_.size(), fr.bindings_linear_ownership_state_.size());
    for (std::size_t i = 0; i < n; ++i) {
        if (fr.bindings_linear_ownership_state_[i] == linear_rt::Moved) {
            if (m) {
                m->linear_ownership_violation_prevented.fetch_add(1, std::memory_order_relaxed);
                m->linear_violations_caught_total.fetch_add(1, std::memory_order_relaxed);
            }
            return false;
        }
    }
    return true;
}

bool Evaluator::mark_linear_binding_moved(Env& env, aura::ast::SymId s) {
    bool any = env.set_linear_ownership_state(s, linear_rt::Moved);
    const EnvId pid = env.parent_id();
    if (pid != NULL_ENV_ID && pid < env_frames_.size()) {
        std::unique_lock<std::shared_mutex> wlock(env_frames_mtx_);
        if (pid < env_frames_.size())
            any = env_frames_[pid].set_linear_ownership_state(s, linear_rt::Moved) || any;
    }
    return any;
}

bool Evaluator::mark_linear_binding_moved_by_name(Env& env, std::string_view name) {
    bool any = env.set_linear_ownership_state_by_name(name, linear_rt::Moved);
    const EnvId pid = env.parent_id();
    if (pid != NULL_ENV_ID && pid < env_frames_.size()) {
        std::unique_lock<std::shared_mutex> wlock(env_frames_mtx_);
        if (pid < env_frames_.size())
            any = env_frames_[pid].set_linear_ownership_state_by_name(std::string(name),
                                                                      linear_rt::Moved) ||
                  any;
    }
    return any;
}

// Issue #1538: combined post-mutation linear pipeline — runtime half.
// Snapshot frame ids under env_frames_mtx_, then call
// linear_post_mutate_enforce per id (avoids nested shared_lock).
// O(F) in number of live frames.
Evaluator::LinearPostMutateSweepResult Evaluator::linear_post_mutate_enforce_all() const noexcept {
    LinearPostMutateSweepResult out;
    std::vector<EnvId> ids;
    {
        std::shared_lock<std::shared_mutex> rlock(env_frames_mtx_);
        ids.reserve(env_frames_.size());
        for (EnvId id = 0; id < env_frames_.size(); ++id) {
            if (env_frames_[id].version_ == INVALID_VERSION)
                continue;
            ids.push_back(id);
        }
    }
    for (EnvId id : ids) {
        ++out.frames_checked;
        if (!linear_post_mutate_enforce(id))
            out.all_safe = false;
    }
    if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics_); m && out.frames_checked > 0) {
        m->linear_post_mutate_enforcements_total.fetch_add(1, std::memory_order_relaxed);
    }
    return out;
}

// Issue #355: refresh_stale_frame_in_walk — single source of
// truth for the "saw a stale frame during a walk" pattern.
//
// Mirrors the warning + version-bump + stats-counter logic that
// materialize_call_env applies for closure-body entry. Walks
// (lookup_by_symid_chain / walk_env_frame_roots / Env::lookup_
// cell_ptr / Env::lookup_cell_index) call this on the const ref
// they hold via walk_env_frames so the staleness is observed
// the same way across every consumer path.
//
// The version_ bump uses const_cast on the const ref. Callers
// must hold the shared env_frames_mtx_ read lock for the
// duration of the frame ref (same as materialize_call_env); the
// exclusive lock excludes walker threads by design.
void Evaluator::refresh_stale_frame_in_walk(EnvId id, const char* site) const {
    if (id == NULL_ENV_ID || id >= env_frames_.size())
        return; // invalid frame — let the walk skip it as before
    // Caller already holds the shared lock; re-entering would
    // deadlock under a shared_mutex. Use the existing reference
    // path. We index env_frames_ directly since we're in a
    // member function (the frame lifetime is safe because the
    // caller's shared lock excludes alloc_env_frame writes).
    const std::uint64_t current = defuse_version_.load(std::memory_order_acquire);
    const EnvFrame& fr = env_frames_[id];
    // Issue #356: never "refresh" a post-rollback invalid
    // frame. INVALID_VERSION is terminal — the bindings may
    // reference AST nodes / pool strings that no longer exist
    // after restore_panic_checkpoint truncated cells_/pairs_/
    // string_heap_. The walker should skip and emit a
    // distinct [#356 warning], not bump version_ + log a
    // stale-refresh that would imply the frame is safe to use.
    if (fr.version_ == INVALID_VERSION) {
        static const char* verbose_env = std::getenv("AURA_VERBOSE_ENVFRAME");
        if (verbose_env && verbose_env[0] != '0' && verbose_env[0] != '\0') {
            std::println(std::cerr,
                         "[#356 warning] {}: post-rollback EnvFrame id={} "
                         "(frame.version_=INVALID). Walk stopped at this frame.",
                         site, id);
        }
        return;
    }
    if (fr.version_ >= current) {
        // Already refreshed by a concurrent walker since this
        // walk started. Nothing to do — the version_ gate
        // already silences the warning.
        return;
    }
    // Bump the frame's version_ to silence future warnings at
    // every walk site (same pattern as materialize_call_env).
    const_cast<EnvFrame&>(fr).version_ = current;
    // Stats: bump the canonical stale-refresh counter so
    // (query:envframe-dualpath-stats) reports the event.
    bump_envframe_stale_refresh_count();
    // Optional stderr warning, gated behind AURA_VERBOSE_ENVFRAME
    // (same env var as materialize_call_env uses, so operators
    // can opt into all staleness diagnostics at once).
    static const char* verbose_env = std::getenv("AURA_VERBOSE_ENVFRAME");
    if (verbose_env && verbose_env[0] != '0' && verbose_env[0] != '\0') {
        std::println(std::cerr,
                     "[#242 warning] {}: stale EnvFrame id={} "
                     "(frame.version_={}, current defuse_version_={}). "
                     "Bindings may be inconsistent with post-mutation state. "
                     "Bumped frame.version_ to silence future warnings.",
                     site, id, fr.version_, current);
    }
}

// Issue #356: is_env_frame_invalid — true if the frame has
// been marked INVALID_VERSION by a post-rollback invalidation
// pass. Distinct from is_env_frame_stale (which tests against
// the current defuse_version_ and can be self-healing); an
// invalid frame is terminal — never refreshable.
bool Evaluator::is_env_frame_invalid(EnvId id) const {
    if (id == NULL_ENV_ID || id >= env_frames_.size())
        return true;
    // Shared lock to keep the frame reference alive across the
    // load (consistent with is_env_frame_stale). The version_
    // load is atomic-friendly (uint64_t) but the shared_mutex
    // guards against an alloc_env_frame concurrent reallocation.
    std::shared_lock<std::shared_mutex> rlock(env_frames_mtx_);
    return env_frames_[id].version_ == INVALID_VERSION;
}

// Issue #741: proactive EnvFrame version re-stamp for live
// tree-walker closures after invalidate_function / impact_scope
// partial re-lower. Walks closures_ and refreshes stale captured
// env frames so long-lived quote/lambda closures don't trip
// closure_needs_safe_fallback on their next apply.
std::uint64_t Evaluator::resync_live_closure_env_versions_on_invalidate() {
    std::uint64_t resynced = 0;
    const std::uint64_t current = defuse_version_.load(std::memory_order_acquire);
    std::shared_lock<std::shared_mutex> cl_lock(closures_mtx_);
    std::shared_lock<std::shared_mutex> ef_lock(env_frames_mtx_);
    for (const auto& [cid, cl] : closures_) {
        (void)cid;
        if (cl.env_id == NULL_ENV_ID || cl.env_id >= env_frames_.size())
            continue;
        const EnvFrame& fr = env_frames_[cl.env_id];
        if (fr.version_ == INVALID_VERSION || fr.version_ >= current)
            continue;
        refresh_stale_frame_in_walk(cl.env_id, "resync_live_closure_env_on_invalidate");
        bump_incremental_closure_env_version_resync();
        ++resynced;
    }
    return resynced;
}

// Issue #356: invalidate_post_rollback_env_frames — mark
// every env_frames_ entry at indices
// [panic_safe_env_frames_size_, env_frames_.size()) as
// INVALID_VERSION without shrinking the deque.
//
// Issue #1360 supersedes this on the restore path with
// truncate_env_frames_to_checkpoint() (actual shrink). This
// helper remains for unit tests and as a soft-fail path.
//
// Thread-safety: exclusive env_frames_lock(). Callers must NOT
// hold any reader lock at the time of the call.
void Evaluator::invalidate_post_rollback_env_frames() {
    const std::size_t checkpoint_size = panic_safe_env_frames_size_;
    const std::size_t current_size = env_frames_.size();
    if (checkpoint_size >= current_size)
        return; // nothing to invalidate
    std::unique_lock<std::shared_mutex> wlock(env_frames_lock());
    std::uint64_t invalidated = 0;
    for (std::size_t i = checkpoint_size; i < current_size; ++i) {
        if (env_frames_[i].version_ != INVALID_VERSION) {
            env_frames_[i].version_ = INVALID_VERSION;
            ++invalidated;
        }
    }
    if (invalidated > 0) {
        bump_envframe_post_rollback_invalidations(invalidated);
    }
}

// Issue #1360: shrink env_frames_ to the panic checkpoint size.
// Append-only EnvId: pre-checkpoint indices [0, N) are unchanged
// and remain valid for live Closure::env_id. Post-checkpoint
// EnvIds become OOB; resolve_env_frame returns nullptr (no UAF).
// Also marks doomed frames INVALID_VERSION before erase so any
// concurrent reader that raced before resize sees terminal
// invalid, then bumps env_generation_ + truncate counters.
std::size_t Evaluator::truncate_env_frames_to_checkpoint() {
    const std::size_t checkpoint_size = panic_safe_env_frames_size_;
    std::unique_lock<std::shared_mutex> wlock(env_frames_lock());
    const std::size_t current_size = env_frames_.size();
    if (checkpoint_size >= current_size)
        return 0;
    const std::size_t dropped = current_size - checkpoint_size;
    // Soft-mark first (helps race windows + keeps #356 counter hot)
    std::uint64_t invalidated = 0;
    for (std::size_t i = checkpoint_size; i < current_size; ++i) {
        if (env_frames_[i].version_ != INVALID_VERSION) {
            env_frames_[i].version_ = INVALID_VERSION;
            ++invalidated;
        }
    }
    if (invalidated > 0)
        bump_envframe_post_rollback_invalidations(invalidated);
    // Actually reclaim memory / cap growth
    env_frames_.resize(checkpoint_size);
    ++env_generation_;
    bump_envframe_truncate(dropped);
    return dropped;
}

const EnvFrame* Evaluator::resolve_env_frame(EnvId id) const noexcept {
    if (id == NULL_ENV_ID || id >= env_frames_.size())
        return nullptr;
    return &env_frames_[id];
}

EnvFrame* Evaluator::resolve_env_frame_mut(EnvId id) noexcept {
    if (id == NULL_ENV_ID || id >= env_frames_.size())
        return nullptr;
    return &env_frames_[id];
}

// Issue #1386: compact env_frames_ arena — reclaim stale frames
// that are not referenced by any live Closure. Rewrites
// Closure::env_id via a remap table so apply_closure materializes
// the right frame post-compact. Bumps defuse_version_ so any
// stale bridge_epoch snapshot re-bridges via closure_bridge_.
//
// Algorithm:
//   1. unique_lock(env_frames_mtx_)
//   2. shared_lock(closures_mtx_) — build referenced set
//   3. live[i] = (version_ >= defuse_version_) || referenced[i]
//   4. Build new_env_frames + remap[old -> new or -1]
//   5. unique_lock(closures_mtx_) — rewrite Closure::env_id
//   6. swap env_frames_, defuse_version_.fetch_add(1)
//
// Locking order: env_frames_mtx_ -> closures_mtx_ (consistent
// with apply_closure path which takes closures_mtx_ first then
// materializes from env_frames_ under no env_frames_mtx_ hold —
// a fiber holding shared closures_mtx_ blocks our unique take,
// avoiding the classic upgrade deadlock).
std::size_t Evaluator::compact_env_frames() {
    // Issue #1401: acquire the interlock FIRST so we serialize with
    // load_module_file. compact_env_frames rewrites Closure::env_id
    // via remap; without the interlock, a concurrent load_module_file
    // could add closures_ to a stale view of env_frames_ (frame
    // already reclaimed or remap table wrong).
    std::lock_guard interlock(compact_env_frames_lock_);
    // Issue #1545 / #1568: pre-compact full boundary consistency —
    // scan + force Drop linear captures + epoch fence before env_id
    // remap so apply never walks remapped frames with use-after-move.
    // (GC root audit deferred to end of compact where restamp/reg run.)
    {
        (void)scan_live_closures_for_linear_captures(/*mark_invalid=*/true);
        (void)linear_post_mutate_enforce_all();
    }
    std::unique_lock<std::shared_mutex> env_lock(env_frames_mtx_);
    const std::size_t orig_size = env_frames_.size();
    if (orig_size == 0) {
        // Still bump defuse_version_ so the caller sees a
        // post-compact epoch marker (consistency with the
        // non-empty path). Issue #1510/#1526: also bump bridge_epoch
        // (+ AOT table via service hook) for dual-domain lockstep.
        defuse_version_.fetch_add(1, std::memory_order_release);
        if (bridge_epoch_bump_fn_ && compiler_service_)
            bridge_epoch_bump_fn_(compiler_service_);
        if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics_))
            m->envframe_compact_epoch_bumps_total.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    const auto current_defuse = defuse_version_.load(std::memory_order_acquire);

    // Step 2: build referenced set from closures_ (shared lock).
    // A frame is referenced if any live Closure's env_id == i.
    std::vector<bool> referenced(orig_size, false);
    {
        std::shared_lock<std::shared_mutex> cl_lock(closures_mtx_);
        for (const auto& kv : closures_) {
            const auto id = kv.second.env_id;
            if (id != NULL_ENV_ID && id < orig_size) {
                referenced[id] = true;
            }
        }
    }

    // Step 3 + 4: build new_env_frames + remap. Iterate env_frames_
    // once; move live frames into new_env_frames, record remap[i] =
    // new index for live frames, -1 for dead frames. After the loop
    // env_frames_ contains a mix of valid (dead) and moved-from (live)
    // frames — but we replace it with new_env_frames anyway, so the
    // state is transient and harmless.
    std::vector<std::int64_t> remap(orig_size, -1);
    std::deque<EnvFrame> new_env_frames;
    for (std::size_t i = 0; i < orig_size; ++i) {
        const bool live = env_frames_[i].version_ >= current_defuse || referenced[i];
        if (live) {
            remap[i] = static_cast<std::int64_t>(new_env_frames.size());
            new_env_frames.push_back(std::move(env_frames_[i]));
        }
        // else: remap[i] stays -1 (from init), don't touch
    }

    // Issue #1510: rewrite EnvFrame::parent_id through the same
    // remap table. Without this, SoA parent walks post-compact
    // resolve to wrong (or reclaimed) slots — UAF / wrong capture.
    std::size_t parent_rewrites = 0;
    for (auto& fr : new_env_frames) {
        if (fr.parent_id == NULL_ENV_ID || fr.parent_id >= remap.size())
            continue;
        const auto np = remap[fr.parent_id];
        if (np >= 0) {
            fr.parent_id = static_cast<EnvId>(np);
            ++parent_rewrites;
        } else {
            fr.parent_id = NULL_ENV_ID;
            ++parent_rewrites;
        }
    }

    // Step 5: rewrite Closure::env_id (unique lock).
    // All closures pointing to a freed frame would have been
    // marked non-referenced in step 2, so the frame would have
    // been reclaimed. But — defense in depth — if any closure
    // got env_id pointing to a freed frame between the shared
    // read and the unique take (closure was added/updated),
    // remap returns -1 and we clear env_id to NULL_ENV_ID.
    std::size_t rewritten = 0;
    {
        std::unique_lock<std::shared_mutex> cl_lock(closures_mtx_);
        for (auto& kv : closures_) {
            const auto id = kv.second.env_id;
            if (id == NULL_ENV_ID || id >= remap.size())
                continue;
            const auto new_id = remap[id];
            if (new_id >= 0) {
                kv.second.env_id = static_cast<EnvId>(new_id);
                ++rewritten;
            } else {
                // Frame was reclaimed; clear the dangling env_id.
                kv.second.env_id = NULL_ENV_ID;
            }
        }
    }

    // Issue #1510: optional IR / external env_id remap (runtime_closures_).
    // Still under compact_env_frames_lock_ (interlock held). Restamp of
    // IRClosure::bridge_epoch happens after dual-epoch bump below.
    if (compact_env_remap_fn_)
        compact_env_remap_fn_(compact_env_remap_ctx_, remap.data(), remap.size());

    const std::size_t reclaimed = orig_size - new_env_frames.size();
    env_frames_ = std::move(new_env_frames);

    // Issue #1510 / #1526: atomic dual-epoch pair under interlock —
    // defuse_version_ (EnvFrame freshness) + bridge_epoch (closure
    // is_bridge_stale) + AOT table epoch (via service bump hook).
    // Then restamp survivor Closure::bridge_epoch to the NEW epoch so
    // remapped env_id + matching bridge_epoch stay dual-check consistent
    // (JIT will not see "fresh epoch + dangling env_id" race).
    defuse_version_.fetch_add(1, std::memory_order_release);
    ++env_generation_;
    if (bridge_epoch_bump_fn_ && compiler_service_)
        bridge_epoch_bump_fn_(compiler_service_);

    // Issue #1526: restamp tree-walker Closure::bridge_epoch → current.
    std::size_t restamped = 0;
    {
        const auto cur_bridge = current_bridge_epoch();
        std::unique_lock<std::shared_mutex> cl_lock(closures_mtx_);
        for (auto& kv : closures_) {
            // Restamp any previously-tracked closure (non-zero) so it
            // matches post-compact dual-epoch. Leave 0 (untracked) alone.
            if (kv.second.bridge_epoch != 0) {
                kv.second.bridge_epoch = cur_bridge;
                ++restamped;
            }
        }
    }
    // IR restamp: service-installed remap already rewrote env_id; if the
    // ctx supports a second restamp pass it is invoked via the same hook
    // with n==0 as a restamp signal, OR via dedicated restamp after bump.
    // Prefer explicit restamp callback when set.
    if (compact_env_restamp_fn_) {
        const auto cur_bridge = current_bridge_epoch();
        restamped += compact_env_restamp_fn_(compact_env_remap_ctx_, cur_bridge);
    }

    bump_envframe_compact(reclaimed, rewritten);
    // Issue #1510 / #1526 metrics (CompilerMetrics surface).
    if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics_)) {
        m->envframe_compact_rewrites_total.fetch_add(rewritten + parent_rewrites,
                                                     std::memory_order_relaxed);
        m->envframe_compact_epoch_bumps_total.fetch_add(1, std::memory_order_relaxed);
        m->envframe_compact_bridge_restamps_total.fetch_add(restamped, std::memory_order_relaxed);
        // Issue #1543: restamped survivor closures are re-registered as
        // GC roots under the post-compact bridge_epoch (env_id remapped).
        if (restamped > 0) {
            m->linear_ownership_gc_root_registrations_total.fetch_add(restamped,
                                                                      std::memory_order_relaxed);
        }
    }
    // Drop env_frames exclusive lock before #1543 audit (audit collect
    // only needs closures_mtx_; avoid holding exclusive while auditing).
    env_lock.unlock();
    (void)run_linear_gc_root_audit(kLinearGcRootAuditCompact);
    return reclaimed;
}

// Evaluator::lookup_by_symid_chain — demonstrate the SoA walk.
// Walks env_frames_ via index lookup (no pointer chase) and
// returns the first match (closest frame wins, shadowing
// semantics match Env::lookup_by_symid).
//
// P0: Cell deref is now centralized here using the Evaluator's
// own central `cells_` pmr vector (the only owner of the
// re-allocatable cell heap). EnvFrame no longer stores a
// cells_ pointer; frames are pure data + indices. This is
// the canonical path for new SoA code. Legacy Env paths
// (still using Env::cells_ pointer) remain for transition.
std::optional<types::EvalValue> Evaluator::lookup_by_symid_chain(EnvId start,
                                                                 aura::ast::SymId s) const {
    std::optional<types::EvalValue> result;
    const auto version_snap = defuse_version_.load(std::memory_order_acquire);
    // Hold the shared lock across the walk so frame refs stay
    // valid; refresh_stale_frame_in_walk needs the shared lock
    // to be held (consistent with materialize_call_env).
    std::shared_lock<std::shared_mutex> rlock(env_frames_mtx_);
    walk_env_frames(start, [&](EnvId cur, const EnvFrame& fr) {
        // Issue #264: skip frames stamped before the current
        // mutation epoch (stale under concurrent mutate/compact).
        if (fr.version_ < version_snap) {
            // Issue #543: bump the envframe_version_mismatch_in_walk_
            // counter so (query:envframe-dualpath-stats) can
            // report walk-time version mismatches for production
            // monitoring. Stats-only (relaxed-ordering); doesn't
            // affect control flow.
            bump_envframe_version_mismatch_in_walk();
            // Issue #355: refresh the stale frame (bump its
            // version_ + emit the [#242 warning] gated behind
            // AURA_VERBOSE_ENVFRAME) so the walker surfaces the
            // same staleness diagnostic that
            // materialize_call_env does, and subsequent walks
            // see it as fresh.
            refresh_stale_frame_in_walk(cur, "lookup_by_symid_chain");
            // After refresh, still consult this frame (do not skip).
            // Skipping + #1128 parent fallthrough returned pre-rebind
            // bindings and broke mutate:rebind / dep-chain p0 tests.
        }
        auto v = fr.lookup_local_by_symid(s);
        if (v.has_value()) {
            auto val = *v;
            if (is_cell(val)) {
                // Issue #1130: snapshot cell after acquire fence so a
                // concurrent cell store is visible; re-check size so we
                // never index past a concurrent shrink (push-only today).
                auto idx = as_cell_id(val);
                std::atomic_thread_fence(std::memory_order_acquire);
                const auto n = cells_.size();
                if (idx < n)
                    result = cells_[idx]; // EvalValue is int64 POD copy
                else
                    result = val; // defensive
            } else {
                result = std::move(val);
            }
            return false; // stop walking — closest frame wins
        }
        return true; // continue walking
    });
    return result;
}
void Evaluator::walk_env_frame_roots(std::vector<std::int64_t>& pair_roots_out,
                                     std::vector<std::int64_t>& closure_roots_out) const {
    // De-dup: a pair/closure may be bound in multiple envs.
    // Using a small set per pass; if the size grows beyond
    // a threshold, we fall back to dedup-after-the-fact
    // (mark vectors also de-dup via the set() semantic).
    // For now, just always set — the GC's mark_env_frame_roots
    // is idempotent (set() is a no-op if already set).
    const auto version_snap = defuse_version_.load(std::memory_order_acquire);
    // Hold the shared lock across the iteration so refresh_stale_frame_in_walk
    // can safely bump the frame's version_ (same precondition as
    // materialize_call_env + lookup_by_symid_chain).
    std::shared_lock<std::shared_mutex> rlock(env_frames_mtx_);
    for (EnvId cur = 0; cur < env_frames_.size(); ++cur) {
        const EnvFrame& fr = env_frames_[cur];
        // Issue #543: skip frames stamped before the current
        // mutation epoch (stale under concurrent mutate/compact).
        // Bumping gc_walk_safe_skips_ lets
        // (query:envframe-dualpath-stats) report how many
        // frames the GC walk safely skipped. Stats-only
        // (relaxed-ordering); doesn't affect control flow.
        if (fr.version_ < version_snap) {
            bump_envframe_gc_walk_safe_skips();
            // Issue #355: refresh the stale frame (bump its
            // version_ + emit the [#242 warning] gated behind
            // AURA_VERBOSE_ENVFRAME) so the GC walk surfaces
            // the same staleness diagnostic that
            // materialize_call_env + lookup_by_symid_chain do.
            refresh_stale_frame_in_walk(cur, "walk_env_frame_roots");
            continue;
        }
        // Dual-path length/order consistency check (#543).
        // The two storage arrays should hold the same number
        // of entries (one per binding). A mismatch indicates
        // either a desync in the bind path or a future bug
        // where one array is updated without the other. Bump
        // the desync counter (warning only — the GC walk
        // still walks both arrays below, so this is purely
        // observability). Stats-only (relaxed-ordering).
        if (fr.bindings_.size() != fr.bindings_symid_.size()) {
            bump_envframe_desync_detected();
        }
        // Walk the name-keyed bindings. bindings_symid_ is
        // populated when pool_ is set; bindings_ is always
        // populated. We walk BOTH to be safe (they should
        // hold the same values, but checking is cheap).
        for (const auto& [name, val] : fr.bindings_) {
            (void)name;
            if (is_pair(val)) {
                pair_roots_out.push_back(static_cast<std::int64_t>(as_pair_idx(val)));
            } else if (is_closure(val)) {
                closure_roots_out.push_back(static_cast<std::int64_t>(as_closure_id(val)));
            }
        }
        for (const auto& [sym, val] : fr.bindings_symid_) {
            (void)sym;
            if (is_pair(val)) {
                pair_roots_out.push_back(static_cast<std::int64_t>(as_pair_idx(val)));
            } else if (is_closure(val)) {
                closure_roots_out.push_back(static_cast<std::int64_t>(as_closure_id(val)));
            }
        }
    }
}
// ── Issue #145: EnvView / ClosureView impls ──────────────────
//
// make_env_view: build a zero-copy view over an Env's
// bindings. The spans stay valid as long as the Env does
// (no vector reallocation expected — the Env's bindings_
// grows monotonically within a single eval).
EnvView make_env_view(const Env& env) {
    EnvView v;
    v.string_bindings = env.bindings();
    // The SymId-keyed array is private; access via a const
    // accessor friend. We add the accessor below.
    v.symid_bindings = env.bindings_symid();
    v.parent = env.parent();
    return v;
}

std::optional<EvalValue> EnvView::lookup(const std::string& name) const {
    for (auto it = string_bindings.rbegin(); it != string_bindings.rend(); ++it)
        if (it->first == name)
            return it->second;
    return parent ? parent->lookup(name) : std::nullopt;
}

std::optional<EvalValue> EnvView::lookup_by_intern(const std::string& n,
                                                   const aura::ast::StringPool* pool) const {
    // Mirror Env::lookup_by_intern: intern via the resolved
    // pool, route through lookup_by_symid, return local
    // symid_bindings lookup if not found, then fall through
    // to the parent walk. EnvView has no primitive/ADT
    // fallbacks (those live on Env, not EnvView), so the
    // behavior matches EnvView::lookup for the "name not
    // found" case: nullopt.
    if (!pool)
        return std::nullopt; // EnvView: no fallback pool
    // const_cast is safe — intern() is logically idempotent
    // (already-interned names are no-ops) and EnvView callers
    // pass a non-const pool pointer (canonical_pool() or
    // closure-captured). The function is logically const
    // (no observable EnvView state change).
    auto sym = const_cast<aura::ast::StringPool*>(pool)->intern(n);
    for (auto it = symid_bindings.rbegin(); it != symid_bindings.rend(); ++it)
        if (it->first == sym)
            return it->second;
    return parent ? parent->lookup_by_intern(n, pool) : std::nullopt;
}

std::optional<EvalValue> EnvView::lookup_by_symid(aura::ast::SymId s) const {
    for (auto it = symid_bindings.rbegin(); it != symid_bindings.rend(); ++it)
        if (it->first == s)
            return it->second;
    return parent ? parent->lookup_by_symid(s) : std::nullopt;
}

ClosureView make_closure_view(const Closure& cl) {
    ClosureView v;
    v.params = std::span<const aura::ast::SymId>(cl.params.data(), cl.params.size());
    v.body_id = cl.body_id;
    v.dotted = cl.dotted;
    v.flat = cl.flat;
    v.pool = cl.pool;
    // P0: no more cl.env; only env_id.
    v.env_id = cl.env_id;
    v.owner_arena = cl.owner_arena;
    v.name = cl.name;
    return v;
}
// ── Env::lookup_cell_ptr: returns EvalValue* ──────────────────
//
// Issue #145 Phase 2.2: parent walk migrates to
// walk_env_frames when owner_ + parent_id_ are set (SoA path).
// Legacy Env* walk preserved as fallback for stack-allocated
// Envs that aren't registered in env_frames_ (local eval scopes
// before Phase 2.6 ships the rename).
EvalValue* Env::lookup_cell_ptr(std::string_view n, std::vector<EvalValue>* cells) const {
    if (!cells)
        return nullptr;
    // 1. Local bindings (no walk needed)
    for (auto& b : bindings_) {
        if (b.first == n) {
            if (is_cell(b.second)) {
                auto ci = as_cell_id(b.second);
                if (ci < cells->size())
                    return &(*cells)[ci];
            }
            return nullptr;
        }
    }
    // 2. Walk parent chain — prefer SoA walk via env_frames_
    //    when owner_ + parent_id_ are both set. Canonical path
    //    for registered Envs (top_, modules_, arena-allocated
    //    envs). Cache-friendly index lookup replaces pointer
    //    chase; shadowing semantics preserved (closest frame
    //    wins, walk_env_frames stops at first match).
    //
    //    Issue #286: snapshot defuse_version_ once at walk
    //    start, then skip frames whose version_ is older than
    //    the snapshot (the stale-frames check from #242). This
    //    keeps lookup behavior consistent with
    //    lookup_by_symid_chain (which already does the check).
    if (owner_ && parent_id_ != NULL_ENV_ID) {
        const auto version_snap = owner_->get_defuse_version();
        EvalValue* result = nullptr;
        // Issue #355: take the shared lock for the duration of
        // the walk so refresh_stale_frame_in_walk can bump the
        // stale frame's version_ safely (consistent with
        // materialize_call_env + lookup_by_symid_chain +
        // walk_env_frame_roots). The shared lock also keeps the
        // frame reference alive across the visitor call.
        std::shared_lock<std::shared_mutex> rlock(owner_->env_frames_lock());
        owner_->walk_env_frames(parent_id_, [&](EnvId cur, const EnvFrame& f) {
            // Skip frames stamped before the current mutation epoch.
            if (f.version_ < version_snap) {
                // Issue #355: refresh the stale frame (bump
                // version_ + emit [#242 warning] gated behind
                // AURA_VERBOSE_ENVFRAME) so this parent walk
                // path surfaces the same staleness diagnostic
                // as materialize_call_env.
                owner_->refresh_stale_frame_in_walk(cur, "Env::lookup_cell_ptr");
                return true; // continue walking past the stale frame
            }
            for (auto& b : f.bindings_) {
                if (b.first == n) {
                    if (is_cell(b.second)) {
                        auto ci = as_cell_id(b.second);
                        if (ci < cells->size())
                            result = &(*cells)[ci];
                    }
                    return false; // stop walking
                }
            }
            return true;
        });
        return result;
    }
    // 3. Legacy pointer walk (preserved for unregistered Envs).
    //    Same shadowing semantics: closest frame wins.
    for (auto* p = parent_; p; p = p->parent_) {
        for (auto& b : p->bindings_) {
            if (b.first == n) {
                if (is_cell(b.second)) {
                    auto ci = as_cell_id(b.second);
                    if (ci < cells->size())
                        return &(*cells)[ci];
                }
                return nullptr;
            }
        }
    }
    return nullptr;
}

// ── Env::lookup_cell_index: returns uint64_t (stable) ─────────────
//
// Issue #145 Phase 2.2: same dual-path pattern as
// lookup_cell_ptr. SoA walk via env_frames_ when registered,
// legacy pointer walk otherwise.
std::optional<std::uint64_t> Env::lookup_cell_index(std::string_view n) const {
    // 1. Local bindings (string path)
    for (auto& b : bindings_) {
        if (b.first == n) {
            if (is_cell(b.second))
                return as_cell_id(b.second);
            return std::nullopt;
        }
    }
    // 1b. Issue #1482: SymId PRIMARY when string bindings empty
    // (materialize may only have rehydrated bindings_symid_).
    if (pool_ && !bindings_symid_.empty()) {
        auto s = const_cast<aura::ast::StringPool*>(pool_)->intern(n);
        for (auto it = bindings_symid_.rbegin(); it != bindings_symid_.rend(); ++it) {
            if (it->first == s) {
                if (is_cell(it->second))
                    return as_cell_id(it->second);
                return std::nullopt;
            }
        }
    }
    // 2. SoA walk via env_frames_ when registered
    if (owner_ && parent_id_ != NULL_ENV_ID) {
        const auto version_snap = owner_->get_defuse_version();
        std::optional<std::uint64_t> result;
        // Issue #355: take the shared lock so refresh_stale_frame_in_walk
        // can bump the stale frame's version_ safely (same
        // precondition as the other walker sites).
        std::shared_lock<std::shared_mutex> rlock(owner_->env_frames_lock());
        owner_->walk_env_frames(parent_id_, [&](EnvId cur, const EnvFrame& f) {
            // Issue #355: skip + refresh stale frames in the
            // parent walk. This path was previously missing
            // the staleness check entirely (the version_ gate
            // existed only in lookup_cell_ptr /
            // lookup_by_symid_chain / walk_env_frame_roots).
            if (f.version_ < version_snap) {
                owner_->refresh_stale_frame_in_walk(cur, "Env::lookup_cell_index");
                return true; // continue walking past the stale frame
            }
            for (auto& b : f.bindings_) {
                if (b.first == n) {
                    if (is_cell(b.second))
                        result = as_cell_id(b.second);
                    return false;
                }
            }
            // SymId path on parent frames
            if (pool_ && !f.bindings_symid_.empty()) {
                auto s = const_cast<aura::ast::StringPool*>(pool_)->intern(n);
                for (auto it = f.bindings_symid_.rbegin(); it != f.bindings_symid_.rend(); ++it) {
                    if (it->first == s) {
                        if (is_cell(it->second))
                            result = as_cell_id(it->second);
                        return false;
                    }
                }
            }
            return true;
        });
        return result;
    }
    // 3. Legacy pointer walk
    for (auto* p = parent_; p; p = p->parent_) {
        for (auto& b : p->bindings_) {
            if (b.first == n) {
                if (is_cell(b.second))
                    return as_cell_id(b.second);
                return std::nullopt;
            }
        }
    }
    return std::nullopt;
}

} // namespace aura::compiler
