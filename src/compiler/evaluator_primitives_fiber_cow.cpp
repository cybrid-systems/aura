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
    if (!reg.read(owner, key, name, shadow)) {
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
        return make_bool(reg.read(owner, fiber_cow_overlay_key(), name, local));
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
                const auto ci = private_cell != 0 && private_cell < ev.cells().size()
                                    ? private_cell
                                    : static_cast<std::uint64_t>(ev.alloc_cell(value));
                ev.top_env().bind(name, make_cell(ci));
            });
        overlay_sync_tls(owner); // #4715 AC2: disarmed after commit (0 on success)
        if (!r.conflict.empty()) {
            return make_primitive_error(
                ev.string_heap_, ev.error_values_,
                std::string("fiber:cow-conflict: ") + r.conflict +
                    " was committed by another fiber after this overlay began",
                ev.primitive_error_counter_ptr());
        }
        return make_bool(r.ok);
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
        std::string out = std::format(
            "begins={} commits={} fast-forward={} merged={} conflicts={} rollbacks={} writes={} "
            "active={}",
            s.begins, s.commits, s.fast_forward, s.merged, s.conflicts, s.rollbacks, s.writes,
            s.active);
        return make_string(ev.string_heap_.mint(out));
    });
}

} // namespace aura::compiler::primitives_detail
