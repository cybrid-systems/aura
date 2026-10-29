// evaluator_primitives_fiber_cow.cpp — Issue #4715: per-fiber COW overlay
// primitives for env bindings (begin / define / set! / get / commit /
// rollback / stats).
//
// The data layer (private overlay map, commit log, deterministic write-write
// conflict verdict) lives in compiler/fiber_cow_overlay.hh. This TU is the
// evaluator face: it resolves the calling body, reads/writes the shared env
// on commit under the registry lock, and reuses the fiber capability gate so
// Strict sandbox semantics carry over unchanged from fiber:spawn.

module;

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "runtime_shared.h"
#include "security_capabilities.h"
#include "serve/fiber.h"
#include "core/workspace_epoch.hh"
#include "compiler/locked_string_heap.hh" // LockedStringHeap for the name helper
// #4715: header-only overlay data layer. Included in the GLOBAL module
// fragment: it only declares templates (no EvalValue), so it needs no
// module import, and including <algorithm>/<mutex> after `import std`
// would re-declare std entities inside this module (GCC error).
#include "compiler/fiber_cow_overlay.hh"

module aura.compiler.evaluator;

import std;
import aura.core.ast;
import aura.compiler.value;

namespace aura::compiler::primitives_detail {

namespace {

    using EvalValue = types::EvalValue;
    using types::as_cell_id;
    using types::as_string_idx;
    using types::is_cell;
    using types::is_closure;
    using types::is_string;
    using types::make_bool;
    using types::make_cell;
    using types::make_hash;
    using types::make_int;
    using types::make_string;
    using types::make_void;

    // Domain tag for bodies with no runtime fiber id (CLI thread backend:
    // aura_fiber_current_id() answers 0 inside a plain std::thread body). Each
    // such body owns its thread and never migrates (#4707 pins CLI bodies to
    // one thread), so the thread identity is a valid per-fiber key. Scheduler /
    // serve-async bodies answer a real id and are keyed by it directly.
    constexpr std::uint64_t kThreadKeyDomain = 0x8000'0000'0000'0000ULL;

    std::uint64_t fiber_cow_overlay_key() {
        const std::uint64_t fid = aura_fiber_current_id();
        if (fid != 0)
            return fid;
        const auto tid = std::hash<std::thread::id>{}(std::this_thread::get_id());
        return kThreadKeyDomain | (static_cast<std::uint64_t>(tid) & (kThreadKeyDomain - 1));
    }

    // The string heap is passed in rather than read off the evaluator: this
    // helper sits outside the friend entry point, so it has no private access.
    std::string overlay_arg_name(const LockedStringHeap& heap, const EvalValue& v) {
        if (!is_string(v))
            return {};
        const auto idx = as_string_idx(v);
        return idx < heap.size() ? std::string(heap[idx]) : std::string();
    }

    // ── Issue #4715 AC2: the env-path fast-path gate ────────────────────
    // Non-zero while the calling thread runs a body with an overlay open. The
    // env read path loads this thread_local once and touches the registry
    // (and its mutex) only when it is set, so single-body / non-overlay
    // programs keep byte-identical semantics at one TLS read per lookup.
    void overlay_sync_tls(const void* owner) {
        const bool open = cow::registry<EvalValue>().active(owner, fiber_cow_overlay_key());
        t_fiber_cow_overlay_depth = open ? 1u : 0u;
    }

    // Issue #4715 AC2: is this value safe to hold privately until commit?
    //
    // Tree-walker closures are NOT: rebinding a define dirties it, which
    // must-deopts the closure the shared cell still holds (name_live_closure +
    // mark_define_dirty_fn_), so a private copy of the NEW closure while the
    // shared cell keeps the OLD one leaves every sibling body calling an
    // invalidated closure ("eval_flat: invalid closure"). Until the workspace
    // slice of this overlay (the follow-up half of #4715) rebases the AST
    // node together with the binding, a closure-valued define keeps the
    // ordinary write-through semantics - visibly the new version, whole, and
    // never torn. Scalars, strings and heap values are immutable-ish and the
    // private cell is a GC root, so they capture safely.
    bool overlay_capturable(const EvalValue& v) {
        return !is_closure(v);
    }

    // ── Issue #4715 follow-up: structured conflict condition ────────────
    //
    // The historical contract is the string "fiber:cow-conflict: <name>"
    // (tests and callers match that prefix), so it stays byte-identical and
    // the condition fields ride along as trailing key=value pairs — the same
    // additive shape query:mutation-log uses. Programmatic callers read the
    // condition object from (fiber:overlay-conflict).
    std::string overlay_conflict_message(const cow::ConflictInfo& ci) {
        std::string out = "fiber:cow-conflict: " + ci.binding + " binding=" + ci.binding +
                          " winner-fiber=" + std::to_string(ci.winner_fiber) +
                          " our-epoch=" + std::to_string(ci.base_epoch) +
                          " committed-epoch=" + std::to_string(ci.winner_epoch) +
                          " our-seq=" + std::to_string(ci.base_seq) +
                          " winner-seq=" + std::to_string(ci.winner_seq) +
                          " kind=" + (ci.read_only ? "read-set" : "write-write");
        out += ci.read_only
                   ? " (a binding this overlay read was committed by another fiber after it began)"
                   : " (the same binding was committed by another fiber after this overlay began)";
        return out;
    }

    // Issue #4715 follow-up: the condition object. The epoch / sequence
    // numbers are fixnums; binding, kind and winner-fiber are strings (the
    // fiber key is a 64-bit token that does not fit the fixnum payload). A
    // hash (not a list) so callers use the same (hash-ref cond "…") shape as
    // every other report.
    EvalValue overlay_conflict_condition(Evaluator& ev, const cow::ConflictInfo& ci) {
        auto* ht = FlatHashTable::create(16);
        if (!ht)
            return make_bool(false);
        auto meta = ht->metadata();
        auto keys = ht->keys();
        auto vals = ht->values();
        auto put = [&](std::string_view k, const EvalValue& v) {
            std::uint64_t h = 0xcbf29ce484222325ULL;
            for (unsigned char c : k)
                h = (h ^ c) * 0x100000001b3ULL;
            auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
            if (fp == 0xFF)
                fp = 0xFE;
            for (std::size_t at = 0; at < ht->capacity; ++at) {
                const auto idx = ((h >> 1) + at) & (ht->capacity - 1);
                if (meta[idx] != 0xFF)
                    continue;
                meta[idx] = fp;
                keys[idx] =
                    make_string(static_cast<std::uint64_t>(ev.push_string_heap(std::string(k))))
                        .val;
                vals[idx] = v.val;
                ++ht->size;
                return;
            }
        };
        const auto str = [&ev](const std::string& s) {
            return make_string(static_cast<std::uint64_t>(ev.push_string_heap(s)));
        };
        put("binding", str(ci.binding));
        // The overlay key is a 64-bit token (scheduler fiber id, or the
        // thread-key domain for CLI bodies) and does NOT fit the fixnum
        // payload, so the winner is carried losslessly as a decimal string —
        // make_int would trip the #1519 range contract on a high-bit key.
        put("winner-fiber", str(std::to_string(ci.winner_fiber)));
        put("winner-seq", make_int(static_cast<std::int64_t>(ci.winner_seq)));
        put("our-epoch", make_int(static_cast<std::int64_t>(ci.base_epoch)));
        put("committed-epoch", make_int(static_cast<std::int64_t>(ci.winner_epoch)));
        put("our-seq", make_int(static_cast<std::int64_t>(ci.base_seq)));
        put("kind", str(ci.read_only ? "read-set" : "write-write"));
        put("read-only", make_bool(ci.read_only));
        put("schema", make_int(4715));
        // Publish under the workspace write lock: the hash arena may compact
        // g_hash_tables on another thread (runtime_shared.h contract), and the
        // owner stamp must land on the exact slot we just pushed.
        aura_lock_workspace_write();
        const auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        aura_hash_stamp_new_table_owner();
        aura_unlock_workspace_write();
        return make_hash(hidx);
    }

    // ── Issue #4715 follow-up: provenance record for an overlay commit ──
    //
    // The workspace mutation log is the AI-facing audit trail (query
    // :mutation-log). An overlay commit changes env bindings, not AST nodes,
    // so without this record the commit is invisible: target_node is
    // NULL_NODE and the summary carries the committed bindings plus the
    // winner / epoch info. A rejected commit leaves a RolledBack record
    // naming the winner (the audit trail must show the dropped attempt).
    // Runs under the registry lock (NotifyFn), so overlay commits append in
    // commit order and the shared mutation-id counter is not raced by them.
    void record_overlay_commit(Evaluator& ev, std::uint64_t epoch, const cow::CommitResult& r) {
        aura::ast::FlatAST* flat = ev.workspace_flat();
        if (!flat)
            return;
        // A rejected commit applied nothing, so the record names the dropped
        // attempt's bindings; a successful one names what it published.
        const auto& names = r.applied_names.empty() ? r.attempted_names : r.applied_names;
        std::string bindings;
        for (const auto& n : names) {
            if (!bindings.empty())
                bindings += ',';
            bindings += n;
        }
        if (bindings.empty())
            bindings = "-";
        std::string summary;
        const bool rejected = r.conflict_info.valid();
        if (rejected) {
            summary = std::format(
                "bindings={} conflict={} winner-fiber={} our-epoch={} committed-epoch={} "
                "our-seq={} winner-seq={} kind={}",
                bindings, r.conflict_info.binding, r.conflict_info.winner_fiber,
                r.conflict_info.base_epoch, r.conflict_info.winner_epoch, r.conflict_info.base_seq,
                r.conflict_info.winner_seq, r.conflict_info.read_only ? "read-set" : "write-write");
        } else {
            summary = std::format("bindings={} epoch={} fast-forward={}", bindings, epoch,
                                  r.fast_forward ? 1 : 0);
        }
        flat->add_mutation(aura::ast::NULL_NODE, "fiber:overlay-commit", "", "", summary,
                           rejected ? aura::ast::MutationStatus::RolledBack
                                    : aura::ast::MutationStatus::Committed);
    }

    // Copy one private cell payload. The cell identity is fixed once captured,
    // so a concurrent commit can only be observed before or after the top-env
    // rebind, never as a half-written value (AC2 "old or new, never torn").
    bool overlay_cell_value(Evaluator& ev, std::uint64_t cell, EvalValue& out) {
        std::lock_guard<std::recursive_mutex> lk(ev.cells_storage_mutex());
        if (cell >= ev.cells().size())
            return false;
        out = ev.cells()[static_cast<std::size_t>(cell)];
        return true;
    }

} // namespace

// Issue #4715 AC2: definition of the module-wide gate declared in the
// interface. One entity per module: evaluator_env.cpp / evaluator_eval_flat.cpp
// (read and write paths) and this TU (overlay primitives) share it.
thread_local std::uint32_t t_fiber_cow_overlay_depth = 0;

bool fiber_cow_overlay_env_read(Evaluator& ev, std::string_view name, EvalValue& out) {
    if (t_fiber_cow_overlay_depth == 0)
        return false;
    const void* const owner = static_cast<const void*>(&ev);
    auto& reg = cow::registry<EvalValue>();
    const auto key = fiber_cow_overlay_key();
    // A captured define / set! is materialized as a private cell: copy its
    // payload (one EvalValue) so the fiber sees its own write while every
    // other body keeps reading the committed cell.
    if (auto cell = reg.read_cell(owner, key, name))
        return overlay_cell_value(ev, *cell, out);
    return reg.read(owner, key, name, out);
}

std::optional<std::uint64_t> fiber_cow_overlay_env_cell(Evaluator& ev, std::string_view name) {
    if (t_fiber_cow_overlay_depth == 0)
        return std::nullopt;
    return cow::registry<EvalValue>().read_cell(static_cast<const void*>(&ev),
                                                fiber_cow_overlay_key(), name);
}

std::optional<std::size_t> fiber_cow_overlay_capture_define(Evaluator& ev, std::string_view name,
                                                            const EvalValue& value) {
    if (t_fiber_cow_overlay_depth == 0 || name.empty() || !overlay_capturable(value))
        return std::nullopt;
    const void* const owner = static_cast<const void*>(&ev);
    auto& reg = cow::registry<EvalValue>();
    const auto key = fiber_cow_overlay_key();
    std::uint64_t cell = 0;
    if (auto existing = reg.read_cell(owner, key, name))
        cell = *existing;
    if (cell != 0) {
        // A second rebind of the same name inside one body keeps the fiber's
        // cell identity: still private, still one pointer swap at commit.
        std::lock_guard<std::recursive_mutex> lk(ev.cells_storage_mutex());
        if (cell < ev.cells().size())
            ev.cells()[static_cast<std::size_t>(cell)] = value;
        else
            cell = 0;
    }
    if (cell == 0)
        cell = static_cast<std::uint64_t>(ev.alloc_cell(value));
    reg.write_cell(owner, key, name, value, cell, aura::core::current_mutation_epoch());
    overlay_sync_tls(owner);
    return static_cast<std::size_t>(cell);
}

std::optional<std::size_t> fiber_cow_overlay_capture_define_cell(Evaluator& ev,
                                                                 std::string_view name,
                                                                 const EvalValue& value,
                                                                 std::uint64_t cell) {
    if (t_fiber_cow_overlay_depth == 0 || name.empty() || !overlay_capturable(value))
        return std::nullopt;
    // Only the shared top-env binding is overlay material: a define inside a
    // call frame rewrites that frame's cell, which no sibling body reads.
    const auto top_binding = ev.top_env().lookup_binding(name);
    if (!top_binding || !is_cell(*top_binding) || as_cell_id(*top_binding) != cell)
        return std::nullopt;
    return fiber_cow_overlay_capture_define(ev, name, value);
}

bool fiber_cow_overlay_capture_set(Evaluator& ev, std::string_view name, const EvalValue& value,
                                   const Env& env) {
    if (t_fiber_cow_overlay_depth == 0 || name.empty() || !overlay_capturable(value))
        return false;
    const void* const owner = static_cast<const void*>(&ev);
    auto& reg = cow::registry<EvalValue>();
    const auto key = fiber_cow_overlay_key();
    EvalValue shadow = make_void();
    if (!reg.peek(owner, key, name, shadow)) {
        // Only a top-level binding is overlay material. When the name resolves
        // to a local (parameter / let) cell the ordinary in-frame write stays:
        // the overlay is a rebind layer over the shared env, not over lexical
        // scope.
        const auto top_binding = ev.top_env().lookup_binding(name);
        if (!top_binding || !is_cell(*top_binding))
            return false;
        const auto top_cell = as_cell_id(*top_binding);
        const auto resolved = env.lookup_cell_index(name);
        if (!resolved || *resolved != top_cell)
            return false;
    }
    (void)fiber_cow_overlay_capture_define(ev, name, value);
    return true;
}


// ── Issue #4761: atomic read-modify-write primitives ──────────────────────
// (atomic:update! "name" f), (atomic:hash-update! h k f [default]),
// (atomic:counter [init]), (atomic:add! c n), (atomic:counter-value c).
// update!/hash-update! are optimistic CAS loops: read the current value
// under the RMW lock, apply f OUTSIDE any lock (f may yield, spawn or
// re-enter), then re-check under the lock and publish only if nobody moved
// the slot in between; otherwise retry with the fresh value. So concurrent
// atomic RMWs on one slot never lose an update. A plain set!/hash-set! racing
// an RMW is not serialized against it (minimal scope, see #4761).
namespace {

std::mutex& atomic_rmw_mutex_4761() {
    static std::mutex m;
    return m;
}

struct AtomicCounters4761 {
    std::shared_mutex mu;
    std::vector<std::unique_ptr<std::atomic<std::int64_t>>> cells;
};

AtomicCounters4761& atomic_counters_4761() {
    static auto* c = new AtomicCounters4761(); // leaked: outlives fiber threads
    return *c;
}

constexpr std::string_view kAtomicCounterTag4761 = "%atomic-counter";
constexpr int kAtomicMaxRetries4761 = 1'000'000;

} // namespace

void register_fiber_cow_primitives(std::function<void(std::string, PrimFn)> add, Evaluator& ev) {
    auto& reg = cow::registry<EvalValue>();
    const void* const owner = static_cast<const void*>(&ev);

    // #4715: same gate as fiber:spawn — a Strict sandbox without the fiber
    // capability cannot open or touch an overlay.
    // Stateless gate: it takes the evaluator by reference at call time. A
    // gate closure captured by reference into the primitive lambdas would
    // dangle once register_fiber_cow_primitives returned (the registry keeps
    // the primitive lambdas for the evaluator lifetime).
    auto deny = [](Evaluator& ev, std::string_view what) -> std::optional<EvalValue> {
        if (!ev.sandbox_mode())
            return std::nullopt;
        if (ev.has_capability(aura::compiler::security::kCapFiber) ||
            ev.has_capability(aura::compiler::security::kCapWildcard))
            return std::nullopt;
        ev.bump_capability_denial();
        return make_primitive_error(ev.string_heap_, ev.error_values_,
                                    std::string("capability denied: ") + std::string(what) +
                                        " required",
                                    ev.primitive_error_counter_ptr());
    };

    add("fiber:overlay-begin", [&ev, &reg, owner, deny](std::span<const EvalValue>) -> EvalValue {
        if (auto denied = deny(ev, "fiber"))
            return *denied;
        // Snapshot the workspace COW mutation epoch: equal at commit time
        // means the shared env did not move => fast-forward path.
        const std::uint64_t epoch = aura::core::current_mutation_epoch();
        const std::uint64_t base = reg.begin(owner, fiber_cow_overlay_key(), epoch);
        // #4715 AC2: arm the env-path fast path for this body.
        overlay_sync_tls(owner);
        return make_int(static_cast<std::int64_t>(base));
    });

    // (fiber:overlay-define name value) — private define; nothing reaches the
    // shared env until fiber:overlay-commit.
    add("fiber:overlay-define",
        [&ev, &reg, owner, deny](std::span<const EvalValue> a) -> EvalValue {
            if (auto denied = deny(ev, "fiber"))
                return *denied;
            if (a.size() < 2)
                return make_bool(false);
            const std::string name = overlay_arg_name(ev.string_heap_, a[0]);
            if (name.empty())
                return make_bool(false);
            reg.write(owner, fiber_cow_overlay_key(), name, a[1],
                      aura::core::current_mutation_epoch());
            overlay_sync_tls(owner); // #4715 AC2: write() auto-begins
            return make_bool(true);
        });

    // (fiber:overlay-set! name value) — private rebind of a base binding.
    add("fiber:overlay-set!", [&ev, &reg, owner, deny](std::span<const EvalValue> a) -> EvalValue {
        if (auto denied = deny(ev, "fiber"))
            return *denied;
        if (a.size() < 2)
            return make_bool(false);
        const std::string name = overlay_arg_name(ev.string_heap_, a[0]);
        if (name.empty())
            return make_bool(false);
        reg.write(owner, fiber_cow_overlay_key(), name, a[1], aura::core::current_mutation_epoch());
        overlay_sync_tls(owner); // #4715 AC2: write() auto-begins
        return make_bool(true);
    });

    // (fiber:overlay-get name default) — overlay value, else the committed
    // env value, else default. The read copies one EvalValue: a concurrent
    // commit/rebind can only be observed before or after, never torn.
    add("fiber:overlay-get", [&ev, &reg, owner, deny](std::span<const EvalValue> a) -> EvalValue {
        if (auto denied = deny(ev, "fiber"))
            return *denied;
        if (a.empty())
            return make_bool(false);
        const std::string name = overlay_arg_name(ev.string_heap_, a[0]);
        if (name.empty())
            return a.size() > 1 ? a[1] : make_bool(false);
        EvalValue local = make_void();
        if (reg.read(owner, fiber_cow_overlay_key(), name, local))
            return local;
        if (auto committed = ev.top_env().lookup(name)) {
            // Top-level defines bind a cell (Env::bind(make_cell(ci))); resolve
            // it so the caller sees the value the evaluator would see.
            if (is_cell(*committed)) {
                const auto ci = as_cell_id(*committed);
                if (ci < ev.cells_.size())
                    return ev.cells_[ci];
            }
            return *committed;
        }
        return a.size() > 1 ? a[1] : make_bool(false);
    });

    // (fiber:overlay-has name) — overlay-local membership only.
    add("fiber:overlay-has", [&ev, &reg, owner, deny](std::span<const EvalValue> a) -> EvalValue {
        if (auto denied = deny(ev, "fiber"))
            return *denied;
        if (a.empty())
            return make_bool(false);
        const std::string name = overlay_arg_name(ev.string_heap_, a[0]);
        if (name.empty())
            return make_bool(false);
        EvalValue local = make_void();
        // Membership probe only: peek() keeps it out of the read set.
        return make_bool(reg.peek(owner, fiber_cow_overlay_key(), name, local));
    });

    // (fiber:overlay-commit) — merge the overlay into the shared env.
    // #t: every binding applied (fast-forward or per-binding merge).
    // error object: write-write conflict; nothing applied, overlay dropped.
    add("fiber:overlay-commit", [&ev, &reg, owner, deny](std::span<const EvalValue>) -> EvalValue {
        if (auto denied = deny(ev, "fiber"))
            return *denied;
        const std::uint64_t epoch = aura::core::current_mutation_epoch();
        // #4715 AC2: the env read path holds the top-env bindings lock while it
        // consults the registry, so publish in the same order here (bindings ->
        // registry). Env::BindingsGuard is per-thread re-entrant, so the bind()
        // below does not re-take it.
        auto bind_guard = ev.top_env().lock_bindings_exclusive();
        cow::CommitResult r = reg.commit(
            owner, fiber_cow_overlay_key(), epoch,
            [&ev](const std::string& name, const EvalValue& value, std::uint64_t private_cell) {
                // Applies under the registry lock: commits are
                // serialized, so the top-binding append cannot
                // interleave with a sibling commit of the same env.
                // Bind a CELL, matching the evaluator top-level define
                // shape (define binds make_cell(ci) then fills
                // cells_[ci]); a raw value binding would read as an
                // unboxed value to set!/lookup_cell_index.
                //
                // #4715 AC2: a define / set! captured by the
                // interception already owns a private cell - publish
                // that exact cell so a reader sees the old cell or
                // this one, never a half-written payload.
                //
                // #4757: when the name is already bound to a top-level
                // cell, write the committed value INTO that cell instead
                // of rebinding the name to the private one. set! sites,
                // closures and compiled code resolve a global to its cell
                // once; rebinding orphaned that cell, so every later
                // (set! x ...) from a procedure body wrote a cell no
                // reader looked at. The in-place store is the same
                // single-cell write a plain set! does (under the cells
                // storage lock), so readers still see old or new.
                if (auto existing = ev.top_env().lookup_binding(name);
                    existing && is_cell(*existing)) {
                    const auto old_ci = as_cell_id(*existing);
                    std::lock_guard<std::recursive_mutex> lk(ev.cells_storage_mutex());
                    if (old_ci < ev.cells().size()) {
                        ev.cells()[static_cast<std::size_t>(old_ci)] = value;
                        return;
                    }
                }
                const auto ci = private_cell != 0 && private_cell < ev.cells().size()
                                    ? private_cell
                                    : static_cast<std::uint64_t>(ev.alloc_cell(value));
                ev.top_env().bind(name, make_cell(ci));
            },
            // #4715 follow-up: provenance record, appended under the registry
            // lock so the audit trail lands in commit order.
            [&ev, epoch](const cow::CommitResult& res) { record_overlay_commit(ev, epoch, res); });
        overlay_sync_tls(owner); // #4715 AC2: disarmed after commit (0 on success)
        if (!r.conflict.empty()) {
            // The condition object is the structured form of the same
            // verdict; the historical "fiber:cow-conflict: <name>" prefix is
            // preserved at the head of the error string.
            cow::ConflictInfo ci = r.conflict_info;
            if (!ci.valid())
                ci.binding = r.conflict;
            return make_primitive_error(ev.string_heap_, ev.error_values_,
                                        overlay_conflict_message(ci),
                                        ev.primitive_error_counter_ptr());
        }
        return make_bool(r.ok);
    });

    // (fiber:overlay-conflict) — Issue #4715 follow-up: the structured
    // condition of the last rejected commit by the calling fiber, as a hash
    // (binding / winner-fiber / our-epoch / committed-epoch / our-seq /
    // winner-seq / kind / read-only / schema), or #f when there is none.
    add("fiber:overlay-conflict",
        [&ev, &reg, owner, deny](std::span<const EvalValue>) -> EvalValue {
            if (auto denied = deny(ev, "fiber"))
                return *denied;
            const auto ci = reg.last_conflict(owner, fiber_cow_overlay_key());
            if (!ci)
                return make_bool(false);
            return overlay_conflict_condition(ev, *ci);
        });

    // (fiber:overlay-rollback) — failure / cancel path: drop the overlay.
    add("fiber:overlay-rollback", [&reg, owner](std::span<const EvalValue>) -> EvalValue {
        const bool dropped = reg.rollback(owner, fiber_cow_overlay_key());
        overlay_sync_tls(owner); // #4715 AC2: disarmed after rollback
        return make_bool(dropped);
    });

    // (fiber:overlay-pending) — number of bindings held privately right now.
    add("fiber:overlay-pending", [&reg, owner](std::span<const EvalValue>) -> EvalValue {
        return make_int(static_cast<std::int64_t>(reg.pending(owner, fiber_cow_overlay_key())));
    });

    // (fiber:overlay-report) — deterministic observability string; the test
    // rows pin the counters so a vacuous pass (no overlay ever opened)
    // cannot go green.
    add("fiber:overlay-report", [&ev, &reg, owner, deny](std::span<const EvalValue>) -> EvalValue {
        const cow::OverlayStats s = reg.stats(owner);
        // #4715 follow-up appends the read-set subset; every pre-follow-up
        // field keeps its name and position so existing pins stay valid.
        std::string out = std::format(
            "begins={} commits={} fast-forward={} merged={} conflicts={} rollbacks={} writes={} "
            "active={} read-conflicts={}",
            s.begins, s.commits, s.fast_forward, s.merged, s.conflicts, s.rollbacks, s.writes,
            s.active, s.read_conflicts);
        return make_string(ev.string_heap_.mint(out));
    });

    // Issue #4761: atomic read-modify-write primitives (see the note above
    // register_fiber_cow_primitives). Defined here: the evaluator fields are
    // reachable only from this friend entry point.
    auto atomic_err_4761 = [](Evaluator& ev, std::string msg) -> EvalValue {
        return make_primitive_error(ev.string_heap_, ev.error_values_, std::move(msg),
                                    ev.primitive_error_counter_ptr());
    };

    auto atomic_apply_4761 = [](Evaluator& ev, EvalValue fn,
                                std::span<const EvalValue> args) -> std::optional<EvalValue> {
        if (is_cell(fn)) {
            const auto ci = as_cell_id(fn);
            if (ci >= ev.cells_.size())
                return std::nullopt;
            fn = ev.cells_[ci];
        }
        if (is_primitive(fn)) {
            auto prim = ev.primitives_.slot_lookup_fast(as_primitive_slot(fn));
            if (!prim)
                return std::nullopt;
            return (*prim)(args);
        }
        if (is_closure(fn))
            return ev.apply_closure(as_closure_id(fn), args);
        return std::nullopt;
    };

    // Counter handle: (%atomic-counter . index).
    auto atomic_counter_index_4761 = [](Evaluator& ev, const EvalValue& v) -> std::optional<std::size_t> {
        if (!is_pair(v))
            return std::nullopt;
        std::lock_guard slock(ev.alloc_storage_lock_);
        const auto pid = as_pair_idx(v);
        if (pid >= ev.pairs_.size())
            return std::nullopt;
        const auto& pr = ev.pairs_[pid];
        if (!is_string(pr.car) || !is_int(pr.cdr))
            return std::nullopt;
        const auto sidx = as_string_idx(pr.car);
        if (sidx >= ev.string_heap_.size() ||
            std::string_view(ev.string_heap_[sidx]) != kAtomicCounterTag4761)
            return std::nullopt;
        const auto idx = as_int(pr.cdr);
        if (idx < 0)
            return std::nullopt;
        return static_cast<std::size_t>(idx);
    };

        add("atomic:update!", [&ev, atomic_err_4761, atomic_apply_4761](std::span<const EvalValue> a) -> EvalValue {
            if (a.size() < 2)
                return atomic_err_4761(ev, "atomic:update!: expected (atomic:update! name f)");
            const std::string name = overlay_arg_name(ev.string_heap_, a[0]);
            if (name.empty())
                return atomic_err_4761(ev, "atomic:update!: name must be a string or symbol");
            auto bound = ev.top_env().lookup(name);
            if (!bound || !is_cell(*bound) || as_cell_id(*bound) >= ev.cells_.size())
                return atomic_err_4761(ev, "atomic:update!: " + name +
                                               " is not a top-level define (define it first)");
            const auto ci = as_cell_id(*bound);
            for (int attempt = 0; attempt < kAtomicMaxRetries4761; ++attempt) {
                EvalValue old;
                {
                    std::lock_guard lk(atomic_rmw_mutex_4761());
                    old = ev.cells_[ci];
                }
                const EvalValue arg[1] = {old};
                auto nv = atomic_apply_4761(ev, a[1], arg);
                if (!nv)
                    return atomic_err_4761(ev, "atomic:update!: f is not a procedure of one argument");
                if (is_error(*nv))
                    return *nv;
                {
                    std::lock_guard lk(atomic_rmw_mutex_4761());
                    if (ev.cells_[ci] == old) {
                        ev.cells_[ci] = *nv;
                        return *nv;
                    }
                }
                std::this_thread::yield();
            }
            return atomic_err_4761(ev, "atomic:update!: too much contention on " + name);
        });

        add("atomic:hash-update!", [&ev, atomic_err_4761, atomic_apply_4761](std::span<const EvalValue> a) -> EvalValue {
            if (a.size() < 3 || !is_hash(a[0]))
                return atomic_err_4761(
                    ev, "atomic:hash-update!: expected (atomic:hash-update! hash key f [default])");
            auto ref = ev.primitives_.lookup("hash-ref");
            auto has = ev.primitives_.lookup("hash-has-key?");
            auto set = ev.primitives_.lookup("hash-set!");
            if (!ref || !has || !set)
                return atomic_err_4761(ev, "atomic:hash-update!: hash primitives unavailable");
            const bool has_default = a.size() >= 4;
            auto read = [&](bool& present) -> EvalValue {
                present = is_bool(( *has)({a[0], a[1]})) && as_bool((*has)({a[0], a[1]}));
                return present ? (*ref)({a[0], a[1]}) : (has_default ? a[3] : make_void());
            };
            for (int attempt = 0; attempt < kAtomicMaxRetries4761; ++attempt) {
                EvalValue old;
                bool present = false;
                {
                    std::lock_guard lk(atomic_rmw_mutex_4761());
                    old = read(present);
                }
                if (!present && !has_default)
                    return atomic_err_4761(ev, "atomic:hash-update!: key not found and no default");
                const EvalValue arg[1] = {old};
                auto nv = atomic_apply_4761(ev, a[2], arg);
                if (!nv)
                    return atomic_err_4761(ev,
                                           "atomic:hash-update!: f is not a procedure of one argument");
                if (is_error(*nv))
                    return *nv;
                {
                    std::lock_guard lk(atomic_rmw_mutex_4761());
                    bool now_present = false;
                    const EvalValue cur = read(now_present);
                    if (now_present == present && (!present || cur == old)) {
                        (*set)({a[0], a[1], *nv});
                        return *nv;
                    }
                }
                std::this_thread::yield();
            }
            return atomic_err_4761(ev, "atomic:hash-update!: too much contention");
        });

        add("atomic:counter", [&ev, atomic_err_4761](std::span<const EvalValue> a) -> EvalValue {
            std::int64_t init = 0;
            if (!a.empty()) {
                if (!is_int(a[0]))
                    return atomic_err_4761(ev, "atomic:counter: initial value must be an integer");
                init = as_int(a[0]);
            }
            auto& c = atomic_counters_4761();
            std::size_t idx = 0;
            {
                std::unique_lock lk(c.mu);
                idx = c.cells.size();
                c.cells.push_back(std::make_unique<std::atomic<std::int64_t>>(init));
            }
            std::lock_guard slock(ev.alloc_storage_lock_);
            const auto tag = ev.string_heap_.mint(std::string(kAtomicCounterTag4761));
            const auto pid = ev.pairs_.size();
            ev.pairs_.push_back({make_string(tag), make_int(static_cast<std::int64_t>(idx))});
            return types::make_pair(pid);
        });

        auto counter_op = [&ev, atomic_err_4761, atomic_counter_index_4761](std::span<const EvalValue> a, std::string_view who,
                                bool add_mode) -> EvalValue {
            if (a.empty())
                return atomic_err_4761(ev, std::string(who) + ": expected a counter");
            auto idx = atomic_counter_index_4761(ev, a[0]);
            if (!idx)
                return atomic_err_4761(ev, std::string(who) + ": not an atomic:counter");
            std::int64_t delta = 1;
            if (add_mode && a.size() >= 2) {
                if (!is_int(a[1]))
                    return atomic_err_4761(ev, std::string(who) + ": delta must be an integer");
                delta = as_int(a[1]);
            }
            auto& c = atomic_counters_4761();
            std::shared_lock lk(c.mu);
            if (*idx >= c.cells.size())
                return atomic_err_4761(ev, std::string(who) + ": stale counter");
            auto& cell = *c.cells[*idx];
            if (!add_mode)
                return make_int(cell.load(std::memory_order_seq_cst));
            return make_int(cell.fetch_add(delta, std::memory_order_seq_cst) + delta);
        };
        add("atomic:add!", [counter_op](std::span<const EvalValue> a) -> EvalValue {
            return counter_op(a, "atomic:add!", true);
        });
        add("atomic:counter-value", [counter_op](std::span<const EvalValue> a) -> EvalValue {
            return counter_op(a, "atomic:counter-value", false);
        });

}

} // namespace aura::compiler::primitives_detail
