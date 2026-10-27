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
        cow::CommitResult r = reg.commit(owner, fiber_cow_overlay_key(), epoch,
                                         [&ev](const std::string& name, const EvalValue& value) {
                                             // Applies under the registry lock: commits are
                                             // serialized, so the top-binding append cannot
                                             // interleave with a sibling commit of the same env.
                                             // Bind a CELL, matching the evaluator top-level define
                                             // shape (define binds make_cell(ci) then fills
                                             // cells_[ci]); a raw value binding would read as an
                                             // unboxed value to set!/lookup_cell_index.
                                             const auto ci = ev.alloc_cell(value);
                                             ev.top_env().bind(name, make_cell(ci));
                                         });
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
        return make_bool(reg.rollback(owner, fiber_cow_overlay_key()));
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
