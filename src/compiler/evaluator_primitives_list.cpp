// evaluator_primitives_list.cpp — P0 step 3: list primitives
// Issue #2996: migrate onto register_prim + PrimSpec (core TU).
// Issue #2998: residual silent sentinels → make_primitive_error.
// aura.compiler.evaluator module partition; registered via evaluator_primitives_registry.cpp.

module;
#include "compiler/locked_string_heap.hh" // Issue #4543

#include <string_view> // Issue #4628: values_equal takes a string_view op

#include "primitives_detail.h"
#include "observability_metrics.h"
#include "prim_heap_quota.hh" // Issue #2916 / #2997

#include "prim_registrar_scaffold.hh" // Issue #2996
#include "compiler/user_arity.hh"     // Issue #4624: call-site procedure arity
#include "core/sandbox.hh"            // Issue #4567: is_strict() on the JIT cdr face
#include "core/provenance_tracker.hh" // Issue #4567: multi_tenant_env_active() face arm

module aura.compiler.evaluator;

import std;
import aura.compiler.value;
import aura.compiler.evaluator_pure;

namespace aura::compiler::primitives_detail {

using EvalValue = types::EvalValue;
using PrimRegistrar = std::function<void(std::string, PrimFn)>;

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

// Issue #4628: the structural comparison equal? uses, shared with the
// member primitive (R7RS 6.4: member compares the elements with
// equal?). Defined in evaluator_primitives_runtime.cpp (same module).
// Issue #4780: overflow reports a walk that gave up (nesting deeper than
// the cap, or a cyclic spine); callers raise an error, never answer.
bool values_equal(Evaluator& ev, const EvalValue& x, const EvalValue& y, std::string_view op,
                  bool& denied, bool& overflow);

namespace {

    // Issue #2482: empty list is make_void() only. int 0 is a number —
    // never a list terminator. The historical `is_int && as_int==0` check
    // made (null? 0)/(list? 0) true and conflated numeric 0 with '().
    bool is_end_of_list(const EvalValue& v) noexcept {
        return is_void(v);
    }

    // Issue #4470: R7RS eqv?. Same exactness for numbers (so 2 and 2.0
    // differ), then identity. Chars are ints. Symbols compare by intern
    // index, the same way eq? does. Floats compare by value, not pool slot.
    bool values_eqv(const EvalValue& x, const EvalValue& y) {
        if (is_int(x) && is_int(y))
            return as_int(x) == as_int(y);
        if (is_float(x) && is_float(y))
            return as_float(x) == as_float(y);
        if (x == y)
            return true;
        if (is_string(x) && is_string(y))
            return as_string_idx(x) == as_string_idx(y);
        return false;
    }

    // Issue #4567: JIT cons lives in g_pair_slots, past the evaluator
    // pairs vector. length and list? follow that cdr the way car does.
    // The production face loads the tenant stamp only when sandbox != 0
    // and Strict or Restricted+MT. Soft/Off never loads it.
    enum class JitCdrKind : std::uint8_t { Missing, Denied, Ok };

    struct JitCdr {
        JitCdrKind kind = JitCdrKind::Missing;
        EvalValue value{};
    };

    JitCdr jit_pair_cdr(Evaluator& ev, std::size_t id, const char* op) {
        if (id >= g_pair_slots.size() || !g_pair_slots[id])
            return {};
        const bool prod_pair_face =
            ev.effect_sandbox_mode() != 0 &&
            (ev.effect_sandbox_mode() == 2 || ::aura::core::sandbox::is_strict() ||
             ::aura::core::provenance::multi_tenant_env_active());
        if (prod_pair_face) {
            const std::uint64_t slot_tenant =
                (id < g_pair_slot_tenants.size()) ? g_pair_slot_tenants[id] : 0;
            if (slot_tenant != ev.capability_tenant_id()) {
                (void)ev.check_workspace_isolation(ev.capability_tenant_id(), slot_tenant,
                                                   /*required_effects=*/0, op);
                return {JitCdrKind::Denied, {}};
            }
        }
        return {JitCdrKind::Ok, EvalValue{g_pair_slots[id]->cdr}};
    }

} // namespace

void register_list_primitives(PrimRegistrar add, std::pmr::vector<Pair>& pairs,
                              ::aura::compiler::LockedStringHeap& string_heap,
                              std::vector<EvalValue>& error_values, Evaluator& ev) {
    using aura::compiler::impure_general;
    using aura::compiler::pure_general;
    using aura::compiler::register_prim;

    // Issue #4624: apply a callback closure and keep a diagnostic the
    // body raises. Evaluator::apply_closure surfaces that diagnostic as
    // an error value (the #4586 shape), so the caller propagates it and
    // try can catch it. A nullopt is a REFUSED apply (a stale closure, an
    // arity miss, a densify refuse): it stays no value.
    auto apply_callback = [&ev](std::uint64_t cid, std::span<const EvalValue> args) -> EvalValue {
        auto r = ev.apply_closure(cid, args);
        return r ? *r : make_void();
    };

    auto apply_unary = [&ev, apply_callback](const EvalValue& fn, const EvalValue& arg,
                                             bool list_hotpath = false) -> EvalValue {
        // Issue #4228: deref define-cells (Soft std/math preds) before dispatch.
        EvalValue callee = fn;
        if (is_cell(callee)) {
            auto ci = as_cell_id(callee);
            if (ci < ev.cells().size())
                callee = ev.cells()[ci];
            else
                return make_void();
        }
        if (is_primitive(callee)) {
            auto slot = as_primitive_slot(callee);
            ev.bump_primitives_apply_lookup_hits();
            auto prim = ev.primitives_.slot_lookup_fast(slot);
            if (!prim)
                return make_void();
            ev.bump_primitives_apply_fastpath_wins();
            ev.bump_list_intrinsic_dispatches();
            if (list_hotpath)
                ev.bump_list_soa_hits();
            // Issue #479: per-slot fast-path hit for "which prim is hottest".
            primitives_detail::prim_record_fastpath_hit_for_slot(
                static_cast<CompilerMetrics*>(ev.compiler_metrics()), slot);
            return (*prim)({arg});
        }
        if (is_closure(callee)) {
            auto cid = as_closure_id(callee);
            ev.bump_primitives_apply_closure_calls();
            if (list_hotpath)
                ev.bump_list_estimated_cache_misses();
            // Issue #4624: a diagnostic the callback body raises must
            // not be swallowed — keep it as an error value.
            const EvalValue call_args[1] = {arg};
            return apply_callback(cid, call_args);
        }
        return make_void();
    };
    auto apply_pred = [&ev, apply_callback](const EvalValue& fn, const EvalValue& arg,
                                            bool list_hotpath = false) -> EvalValue {
        // Issue #4228: Soft std/math exports are define-cells. Env::lookup
        // returns the cell sentinel; Variable eval usually derefs, but
        // list hot-path preds must tolerate a cell reaching apply_pred
        // (same for apply_unary / apply_binary).
        // Issue #4586: return the value. An error is not a truth test;
        // only #f is false, so folding the error here kept the element.
        EvalValue pred = fn;
        if (is_cell(pred)) {
            auto ci = as_cell_id(pred);
            if (ci < ev.cells().size())
                pred = ev.cells()[ci];
            else
                return make_bool(false);
        }
        if (is_primitive(pred)) {
            auto slot = as_primitive_slot(pred);
            ev.bump_primitives_apply_lookup_hits();
            auto prim = ev.primitives_.slot_lookup_fast(slot);
            if (!prim)
                return make_bool(false);
            ev.bump_primitives_apply_fastpath_wins();
            ev.bump_list_intrinsic_dispatches();
            if (list_hotpath)
                ev.bump_list_soa_hits();
            primitives_detail::prim_record_fastpath_hit_for_slot(
                static_cast<CompilerMetrics*>(ev.compiler_metrics()), slot);
            return (*prim)({arg});
        }
        if (is_closure(pred)) {
            auto cid = as_closure_id(pred);
            ev.bump_primitives_apply_closure_calls();
            if (list_hotpath)
                ev.bump_list_estimated_cache_misses();
            // Issue #4624: same as map — a diagnostic the callback body
            // raises must not be swallowed.
            const EvalValue call_args[1] = {arg};
            return apply_callback(cid, call_args);
        }
        return make_bool(false);
    };
    auto apply_binary = [&ev, apply_callback](const EvalValue& fn, const EvalValue& acc,
                                              const EvalValue& arg,
                                              bool list_hotpath = false) -> EvalValue {
        // Issue #4228: deref define-cells before dispatch.
        EvalValue callee = fn;
        if (is_cell(callee)) {
            auto ci = as_cell_id(callee);
            if (ci < ev.cells().size())
                callee = ev.cells()[ci];
            else
                return make_void();
        }
        if (is_primitive(callee)) {
            auto slot = as_primitive_slot(callee);
            ev.bump_primitives_apply_lookup_hits();
            auto prim = ev.primitives_.slot_lookup_fast(slot);
            if (!prim)
                return make_void();
            ev.bump_primitives_apply_fastpath_wins();
            ev.bump_list_intrinsic_dispatches();
            if (list_hotpath)
                ev.bump_list_soa_hits();
            primitives_detail::prim_record_fastpath_hit_for_slot(
                static_cast<CompilerMetrics*>(ev.compiler_metrics()), slot);
            return (*prim)({acc, arg});
        }
        if (is_closure(callee)) {
            auto cid = as_closure_id(callee);
            ev.bump_primitives_apply_closure_calls();
            if (list_hotpath)
                ev.bump_list_estimated_cache_misses();
            // Issue #4624: same as map — a diagnostic the callback body
            // raises must not be swallowed.
            const EvalValue call_args[2] = {acc, arg};
            return apply_callback(cid, call_args);
        }
        return make_void();
    };
    register_prim(
        add, ev, "list",
        [&pairs, &string_heap, &error_values, &ev](std::span<const EvalValue> a) {
            // Build proper list (pair chain ending with void)
            // Issue #2651: lock pairs_ growth under multi-fiber fanout.
            // Issue #2916: soft pairs quota before bulk grow (Agent-visible error).
            // Issue #2997: one allow (skip when unlimited + small) + timed lock.
            // Issue #4453: geometric pairs reserve (monotonic arena; not size+n).
            if (a.empty())
                return make_void();
            const auto n = a.size();
            Evaluator::ListCtorLockHold hold(ev);
            // Issue #2997: unlimited + n <= kPrimHeapUnlimitedSmall skips allow().
            // Issue #4473: a limited or larger list still denies inside mint_pair,
            // which calls prim_heap_quota_allow only after a freelist miss.
            // pairs.size()+n is not a pre-deny — that charged reused slots.
            const bool small_unlimited =
                !ev.prim_heap_quota_limited(PrimHeapDim::Pairs) && n <= kPrimHeapUnlimitedSmall;
            if (!small_unlimited && !ev.prim_heap_quota_limited(PrimHeapDim::Pairs))
                (void)ev.prim_heap_quota_allow(PrimHeapDim::Pairs, pairs.size());
            pairs.reserve(monotonic_pairs_reserve_cap(pairs.size(), pairs.capacity(), n));
            EvalValue result = make_void();
            for (auto it = a.rbegin(); it != a.rend(); ++it) {
                const auto id = ev.mint_pair(*it, result, /*check_quota=*/!small_unlimited);
                if (id == Evaluator::kFileHeapMintDenied) {
                    return make_primitive_error(
                        string_heap, error_values,
                        std::string(prim_heap_quota_exceeded_msg(PrimHeapDim::Pairs)),
                        ev.primitive_error_counter_ptr());
                }
                result = make_pair(id);
            }
            ev.bump_pair_alloc_count_n(static_cast<std::uint64_t>(n));
            return result;
        },
        pure_general(255, "(...vals) -> list", "Construct a proper list from arguments."));
    register_prim(
        add, ev, "list?",
        [&pairs, &ev](std::span<const EvalValue> a) {
            if (a.empty())
                return make_bool(true);
            auto v = a[0];
            while (!is_end_of_list(v)) {
                if (!is_pair(v))
                    return make_bool(false);
                auto idx = as_pair_idx(v);
                if (idx >= pairs.size()) {
                    // In-range stays bool-only. Out of range, a readable
                    // JIT proper list is still a list. A prod deny is #f
                    // and still records the isolation audit.
                    auto slot = jit_pair_cdr(ev, idx, "list?");
                    if (slot.kind != JitCdrKind::Ok)
                        return make_bool(false);
                    v = slot.value;
                    continue;
                }
                v = pairs[idx].cdr; // follow cdr chain
            }
            // Issue #2026-07-17 (commercial-readiness audit): predicates must
            // return make_bool(true), not make_int(1). Several stdlib sites
            // (filter, list-statistics, etc.) treat truthy values as numeric
            // and silently dropped list? results; the surface test worked
            // around this bug by checking (length ...) instead of (list? ...).
            // Now consistent with all other predicates.
            return make_bool(true);
        },
        pure_general(1, "(any) -> bool", "True if value is a proper list ending in empty."));
    // Issue #2482: null? is true only for the empty list (void), not int 0.
    register_prim(
        add, ev, "null?", [](const auto& a) { return make_bool(!a.empty() && is_void(a[0])); },
        pure_general(1, "(any) -> bool", "True if value is the empty list."));
    register_prim(
        add, ev, "length",
        [&pairs, &string_heap, &error_values, &ev](std::span<const EvalValue> a) {
            if (a.empty())
                return make_int(0);
            auto v = a[0];
            std::int64_t n = 0;
            while (!is_end_of_list(v)) {
                // Issue #2482: non-pair (e.g. improper list ending in 0) stops
                // the walk and returns the count so far — not forced zero.
                // Bare non-list input (length 0) still yields 0 (n unchanged).
                if (!is_pair(v))
                    break;
                auto idx = as_pair_idx(v);
                if (idx >= pairs.size()) {
                    // Issue #4567: JIT pair table. Deny is #4111; a missing
                    // slot stops the walk the way an in-range miss does.
                    auto slot = jit_pair_cdr(ev, idx, "length");
                    if (slot.kind == JitCdrKind::Denied) {
                        return make_primitive_error(
                            string_heap, error_values,
                            "length: cross-tenant pair slot read denied (#4111)",
                            ev.primitive_error_counter_ptr());
                    }
                    if (slot.kind != JitCdrKind::Ok)
                        break;
                    v = slot.value;
                    n++;
                    continue;
                }
                // Issue #4505: the cdr walk itself reads a foreign mint's
                // slots — the count is foreign structure. Same reader gate
                // as car (required_effects 0, IsolationDeny in the helper).
                if (!ev.inrange_slot_tenant_allows(idx, /*vector_heap=*/false,
                                                   /*required_effects=*/0, "length")) {
                    return make_primitive_error(
                        string_heap, error_values,
                        "length: cross-tenant pair slot read denied (#4505)",
                        ev.primitive_error_counter_ptr());
                }
                v = pairs[idx].cdr;
                n++;
            }
            // Issue #614: surface the cdr-walk cost so AI agents can
            // see list-depth vs pair_alloc in production.
            ev.bump_linear_traverse_count(static_cast<std::uint64_t>(n),
                                          static_cast<std::uint64_t>(n));
            return make_int(n);
        },
        pure_general(1, "(list) -> int", "Length of a proper list (improper walk stops)."));
    register_prim(
        add, ev, "list-ref",
        [&pairs, &string_heap, &error_values, &ev](std::span<const EvalValue> a) {
            auto* counter = ev.primitive_error_counter_ptr();
            if (a.size() < 2) {
                return make_primitive_error(string_heap, error_values, "list-ref: too few args",
                                            counter);
            }
            if (!is_int(a[1]) || as_int(a[1]) < 0) {
                return make_primitive_error(string_heap, error_values,
                                            "list-ref: index must be a non-negative integer",
                                            counter);
            }
            auto v = a[0];
            auto pos = static_cast<std::size_t>(as_int(a[1]));
            for (std::size_t i = 0; i < pos; ++i) {
                if (!is_pair(v)) {
                    return make_primitive_error(string_heap, error_values,
                                                "list-ref: index out of bounds", counter);
                }
                auto idx = as_pair_idx(v);
                if (idx >= pairs.size()) {
                    return make_primitive_error(string_heap, error_values,
                                                "list-ref: corrupted pair", counter);
                }
                // Issue #4505: the walk reads foreign cdrs; compare the
                // mint stamp before the slot read.
                if (!ev.inrange_slot_tenant_allows(idx, /*vector_heap=*/false,
                                                   /*required_effects=*/0, "list-ref")) {
                    return make_primitive_error(
                        string_heap, error_values,
                        "list-ref: cross-tenant pair slot read denied (#4505)", counter);
                }
                v = pairs[idx].cdr;
            }
            // Issue #614: surface the cdr-walk depth for list-ref so the
            // high-water tracks worst-case positional access cost.
            ev.bump_linear_traverse_count(pos, pos);
            if (is_pair(v)) {
                auto idx = as_pair_idx(v);
                if (idx >= pairs.size()) {
                    return make_primitive_error(string_heap, error_values,
                                                "list-ref: corrupted pair", counter);
                }
                // Issue #4505: same reader gate before the payload returns.
                if (!ev.inrange_slot_tenant_allows(idx, /*vector_heap=*/false,
                                                   /*required_effects=*/0, "list-ref")) {
                    return make_primitive_error(
                        string_heap, error_values,
                        "list-ref: cross-tenant pair slot read denied (#4505)", counter);
                }
                return pairs[idx].car;
            }
            // Issue #2998: empty list at the landing index is OOB, not silent 0/void.
            return make_primitive_error(string_heap, error_values, "list-ref: index out of bounds",
                                        counter);
        },
        pure_general(2, "(list int) -> any", "Element at index in a list."));
    // (member val list) — Find val in list using content equality (equal?)
    // Not-found is #f (R7RS, Issue #4491). Malformed input is an error (#2998).
    register_prim(
        add, ev, "member",
        [&pairs, &string_heap, &error_values, &ev](std::span<const EvalValue> a) {
            auto* counter = ev.primitive_error_counter_ptr();
            if (a.size() < 2) {
                return make_primitive_error(string_heap, error_values, "member: too few args",
                                            counter);
            }
            auto& val = a[0];
            auto v = a[1];
            if (!is_end_of_list(v) && !is_pair(v)) {
                return make_primitive_error(string_heap, error_values, "member: not a list",
                                            counter);
            }
            while (!is_end_of_list(v)) {
                if (!is_pair(v)) {
                    return make_primitive_error(string_heap, error_values, "member: improper list",
                                                counter);
                }
                auto idx = as_pair_idx(v);
                if (idx >= pairs.size()) {
                    return make_primitive_error(string_heap, error_values, "member: corrupted pair",
                                                counter);
                }
                // Issue #4505: no foreign car feeds the equality compare and
                // the foreign sublist ref is not returned on a match.
                if (!ev.inrange_slot_tenant_allows(idx, /*vector_heap=*/false,
                                                   /*required_effects=*/0, "member")) {
                    return make_primitive_error(
                        string_heap, error_values,
                        "member: cross-tenant pair slot read denied (#4505)", counter);
                }
                // Issue #4628: R7RS 6.4 — member compares the elements with
                // equal?, not by identity. The old elem_eq only knew
                // identical values, ints, strings and bools, so a
                // structurally equal pair, vector or float was never found.
                // The shared comparison is the one equal? uses.
                bool denied = false;
                bool overflow = false;
                const bool hit =
                    values_equal(ev, pairs[idx].car, val, "member", denied, overflow);
                if (denied) {
                    return make_primitive_error(string_heap, error_values,
                                                "member: cross-tenant slot read denied (#4505)",
                                                counter);
                }
                if (overflow) {
                    return make_primitive_error(
                        string_heap, error_values,
                        "member: structure nested too deep or cyclic (#4780)", counter);
                }
                if (hit)
                    return v;
                v = pairs[idx].cdr;
            }
            return make_bool(false); // Issue #4491: not-found is #f (R7RS)
        },
        pure_general(2, "(any list) -> any", "Sublist whose car equals the key, or #f."));
    // Issue #4470: tree-walker primitive. Not lowered to IROpcode::Eq.
    register_prim(
        add, ev, "eqv?",
        [](std::span<const EvalValue> a) {
            if (a.size() < 2)
                return make_bool(false);
            return make_bool(values_eqv(a[0], a[1]));
        },
        pure_general(2, "(any any) -> bool",
                     "True when identical, or numbers of the same exactness."));
    register_prim(
        add, ev, "memv",
        [&pairs, &string_heap, &error_values, &ev](std::span<const EvalValue> a) {
            auto* counter = ev.primitive_error_counter_ptr();
            if (a.size() < 2) {
                return make_primitive_error(string_heap, error_values, "memv: too few args",
                                            counter);
            }
            auto& val = a[0];
            auto v = a[1];
            if (!is_end_of_list(v) && !is_pair(v)) {
                return make_primitive_error(string_heap, error_values, "memv: not a list", counter);
            }
            while (!is_end_of_list(v)) {
                if (!is_pair(v)) {
                    return make_primitive_error(string_heap, error_values, "memv: improper list",
                                                counter);
                }
                auto idx = as_pair_idx(v);
                if (idx >= pairs.size()) {
                    return make_primitive_error(string_heap, error_values, "memv: corrupted pair",
                                                counter);
                }
                // Issue #4505: same reader gate as member before the eqv?
                // compare can return the foreign pair ref.
                if (!ev.inrange_slot_tenant_allows(idx, /*vector_heap=*/false,
                                                   /*required_effects=*/0, "memv")) {
                    return make_primitive_error(string_heap, error_values,
                                                "memv: cross-tenant pair slot read denied (#4505)",
                                                counter);
                }
                if (values_eqv(pairs[idx].car, val))
                    return v;
                v = pairs[idx].cdr;
            }
            return make_bool(false); // Issue #4491
        },
        pure_general(2, "(any list) -> any", "Sublist whose car is eqv? to the key, or #f."));
    register_prim(
        add, ev, "assv",
        [&pairs, &string_heap, &error_values, &ev](std::span<const EvalValue> a) {
            auto* counter = ev.primitive_error_counter_ptr();
            if (a.size() < 2) {
                return make_primitive_error(string_heap, error_values, "assv: too few args",
                                            counter);
            }
            auto& key = a[0];
            auto v = a[1];
            if (!is_end_of_list(v) && !is_pair(v)) {
                return make_primitive_error(string_heap, error_values, "assv: not a list", counter);
            }
            while (!is_end_of_list(v)) {
                if (!is_pair(v)) {
                    return make_primitive_error(string_heap, error_values, "assv: improper list",
                                                counter);
                }
                auto idx = as_pair_idx(v);
                if (idx >= pairs.size()) {
                    return make_primitive_error(string_heap, error_values, "assv: corrupted pair",
                                                counter);
                }
                // Issue #4505: the spine slot and the entry pair both read
                // foreign mints; gate both before any car/cdr read.
                if (!ev.inrange_slot_tenant_allows(idx, /*vector_heap=*/false,
                                                   /*required_effects=*/0, "assv")) {
                    return make_primitive_error(string_heap, error_values,
                                                "assv: cross-tenant pair slot read denied (#4505)",
                                                counter);
                }
                auto entry = pairs[idx].car;
                if (!is_pair(entry)) {
                    return make_primitive_error(string_heap, error_values, "assv: not a pair",
                                                counter);
                }
                auto eidx = as_pair_idx(entry);
                if (eidx >= pairs.size()) {
                    return make_primitive_error(string_heap, error_values, "assv: corrupted pair",
                                                counter);
                }
                if (!ev.inrange_slot_tenant_allows(eidx, /*vector_heap=*/false,
                                                   /*required_effects=*/0, "assv")) {
                    return make_primitive_error(string_heap, error_values,
                                                "assv: cross-tenant pair slot read denied (#4505)",
                                                counter);
                }
                if (values_eqv(pairs[eidx].car, key))
                    return entry;
                v = pairs[idx].cdr;
            }
            return make_bool(false); // Issue #4491
        },
        pure_general(2, "(any list) -> any", "Alist pair whose car is eqv? to the key, or #f."));
    // (append list ...) — Variadic: concatenate all provided lists
    register_prim(
        add, ev, "append",
        [&pairs, &string_heap, &error_values, &ev](std::span<const EvalValue> a) {
            if (a.empty())
                return make_void();
            if (a.size() < 2)
                return a[0];
            // Issue #2997: snapshot prefix cars, one allow + reserve, cons onto last.
            Evaluator::ListCtorLockHold hold(ev);
            std::vector<EvalValue> cars;
            cars.reserve(16);
            for (std::size_t i = 0; i + 1 < a.size(); ++i) {
                auto v = a[i];
                while (!is_end_of_list(v)) {
                    if (!is_pair(v))
                        break;
                    auto idx = as_pair_idx(v);
                    if (idx >= pairs.size())
                        break;
                    // Issue #4505: deny before the car snapshot copies a
                    // foreign element into the caller-owned result list.
                    if (!ev.inrange_slot_tenant_allows(idx, /*vector_heap=*/false,
                                                       /*required_effects=*/0, "append")) {
                        return make_primitive_error(
                            string_heap, error_values,
                            "append: cross-tenant pair slot read denied (#4505)",
                            ev.primitive_error_counter_ptr());
                    }
                    cars.push_back(pairs[idx].car);
                    v = pairs[idx].cdr;
                }
            }
            const auto n = cars.size();
            if (n == 0)
                return a.back();
            // Issue #4473: per-mint quota inside mint_pair (freelist hits do
            // not count) replaces the pairs.size()+n pre-check.
            pairs.reserve(monotonic_pairs_reserve_cap(pairs.size(), pairs.capacity(), n));
            EvalValue result = a.back();
            for (auto it = cars.rbegin(); it != cars.rend(); ++it) {
                const auto id = ev.mint_pair(*it, result, /*check_quota=*/true);
                if (id == Evaluator::kFileHeapMintDenied) {
                    return make_primitive_error(
                        string_heap, error_values,
                        std::string(prim_heap_quota_exceeded_msg(PrimHeapDim::Pairs)),
                        ev.primitive_error_counter_ptr());
                }
                result = make_pair(id);
            }
            ev.bump_pair_alloc_count_n(static_cast<std::uint64_t>(n));
            return result;
        },
        pure_general(255, "(...lists) -> list", "Concatenate lists into one proper list."));
    register_prim(
        add, ev, "reverse",
        [&pairs, &string_heap, &error_values, &ev](std::span<const EvalValue> a) {
            auto* counter = ev.primitive_error_counter_ptr();
            if (a.empty()) {
                return make_primitive_error(string_heap, error_values, "reverse: too few args",
                                            counter);
            }
            if (is_end_of_list(a[0]))
                return make_void();
            if (!is_pair(a[0])) {
                return make_primitive_error(string_heap, error_values, "reverse: not a list",
                                            counter);
            }
            // Issue #2651: pairs_ push under alloc_storage_lock_.
            // Issue #2997: snapshot cars, one allow + reserve, timed lock.
            Evaluator::ListCtorLockHold hold(ev);
            std::vector<EvalValue> cars;
            auto v = a[0];
            while (!is_end_of_list(v)) {
                if (!is_pair(v)) {
                    return make_primitive_error(string_heap, error_values, "reverse: improper list",
                                                counter);
                }
                auto idx = as_pair_idx(v);
                if (idx >= pairs.size()) {
                    return make_primitive_error(string_heap, error_values,
                                                "reverse: corrupted pair", counter);
                }
                // Issue #4505: same reader gate as append before the
                // snapshot copies a foreign car.
                if (!ev.inrange_slot_tenant_allows(idx, /*vector_heap=*/false,
                                                   /*required_effects=*/0, "reverse")) {
                    return make_primitive_error(
                        string_heap, error_values,
                        "reverse: cross-tenant pair slot read denied (#4505)", counter);
                }
                cars.push_back(pairs[idx].car);
                v = pairs[idx].cdr;
            }
            const auto n = cars.size();
            if (n == 0)
                return make_void();
            // Issue #4473: per-mint quota inside mint_pair (freelist hits do
            // not count) replaces the pairs.size()+n pre-check.
            pairs.reserve(monotonic_pairs_reserve_cap(pairs.size(), pairs.capacity(), n));
            EvalValue result = make_void();
            for (const auto& car : cars) {
                const auto new_id = ev.mint_pair(car, result, /*check_quota=*/true);
                if (new_id == Evaluator::kFileHeapMintDenied) {
                    return make_primitive_error(
                        string_heap, error_values,
                        std::string(prim_heap_quota_exceeded_msg(PrimHeapDim::Pairs)),
                        ev.primitive_error_counter_ptr());
                }
                result = make_pair(new_id);
            }
            ev.bump_pair_alloc_count_n(static_cast<std::uint64_t>(n));
            return result;
        },
        pure_general(1, "(list) -> list", "Reverse a proper list."));
    // Issue #4616: R7RS n-ary map/for-each. A map call may carry more than
    // one list; the old body read only a[1] and silently dropped a[2..],
    // so (map f xs ys) called f with one argument. For-each lived only in
    // std/list as a two-parameter closure and raised an arity error on the
    // second list. Both walk every list in lockstep and stop at the
    // shortest one (R7RS).
    auto apply_n = [&ev, apply_callback](const EvalValue& fn, std::span<const EvalValue> args,
                                         bool list_hotpath = false) -> EvalValue {
        // Issue #4228: deref define-cells before dispatch.
        EvalValue callee = fn;
        if (is_cell(callee)) {
            auto ci = as_cell_id(callee);
            if (ci < ev.cells().size())
                callee = ev.cells()[ci];
            else
                return make_void();
        }
        if (is_primitive(callee)) {
            auto slot = as_primitive_slot(callee);
            ev.bump_primitives_apply_lookup_hits();
            auto prim = ev.primitives_.slot_lookup_fast(slot);
            if (!prim)
                return make_void();
            ev.bump_primitives_apply_fastpath_wins();
            ev.bump_list_intrinsic_dispatches();
            if (list_hotpath)
                ev.bump_list_soa_hits();
            primitives_detail::prim_record_fastpath_hit_for_slot(
                static_cast<CompilerMetrics*>(ev.compiler_metrics()), slot);
            return (*prim)(args);
        }
        if (is_closure(callee)) {
            auto cid = as_closure_id(callee);
            ev.bump_primitives_apply_closure_calls();
            if (list_hotpath)
                ev.bump_list_estimated_cache_misses();
            // Issue #4624: a diagnostic the callback body raises must
            // not be swallowed — keep it as an error value.
            return apply_callback(cid, args);
        }
        return make_void();
    };
    // Snapshot the cars of every list argument, then apply the procedure to
    // the k-th tuple. Returns the mapped list (collect) or void (for-each).
    auto walk_lists = [&pairs, &string_heap, &error_values, apply_n, &ev](
                          std::span<const EvalValue> a, const char* op, bool collect) -> EvalValue {
        if (a.size() < 2) {
            return make_primitive_error(string_heap, error_values,
                                        std::string(op) + ": too few args",
                                        ev.primitive_error_counter_ptr());
        }
        const std::size_t nlists = a.size() - 1;
        // Issue #2997: snapshot every list under one lock, apply outside.
        // Issue #4505: deny before the snapshot copies a foreign car.
        std::vector<EvalValue> flat;
        std::vector<std::size_t> offs(nlists + 1, 0);
        {
            Evaluator::ListCtorLockHold hold(ev);
            for (std::size_t i = 0; i < nlists; ++i) {
                auto current = a[i + 1];
                if (!is_end_of_list(current) && !is_pair(current)) {
                    return make_primitive_error(string_heap, error_values,
                                                std::string(op) + ": argument " +
                                                    std::to_string(i + 2) + " is not a list",
                                                ev.primitive_error_counter_ptr());
                }
                while (is_pair(current)) {
                    auto idx = as_pair_idx(current);
                    if (idx >= pairs.size())
                        break;
                    if (!ev.inrange_slot_tenant_allows(idx, /*vector_heap=*/false,
                                                       /*required_effects=*/0, op)) {
                        return make_primitive_error(
                            string_heap, error_values,
                            std::string(op) + ": cross-tenant pair slot read denied (#4505)",
                            ev.primitive_error_counter_ptr());
                    }
                    flat.push_back(pairs[idx].car);
                    current = pairs[idx].cdr;
                    ev.bump_list_chain_traversals();
                    ev.bump_list_estimated_cache_misses();
                }
                offs[i + 1] = flat.size();
            }
        }
        std::size_t n = offs[1];
        for (std::size_t i = 1; i < nlists; ++i)
            n = std::min(n, offs[i + 1] - offs[i]);
        std::vector<EvalValue> tuple(nlists);
        std::vector<EvalValue> mapped;
        if (collect)
            mapped.reserve(n);
        // Issue #4480: flat/tuple/mapped are the only roots for the row
        // results while the callback runs; pin them across the call.
        const Evaluator::TwVecPin pin_flat(flat);
        const Evaluator::TwVecPin pin_tuple(tuple);
        const Evaluator::TwVecPin pin_mapped(mapped);
        for (std::size_t k = 0; k < n; ++k) {
            for (std::size_t i = 0; i < nlists; ++i)
                tuple[i] = flat[offs[i] + k];
            // Issue #4586: a callback error must leave the walk.
            auto one = apply_n(a[0], tuple, true);
            if (is_error(one))
                return one;
            if (collect)
                mapped.push_back(std::move(one));
        }
        if (!collect || n == 0)
            return make_void();
        Evaluator::ListCtorLockHold hold(ev);
        // Issue #4473: per-mint quota inside mint_pair (freelist hits do
        // not count) replaces the pairs.size()+n pre-check.
        pairs.reserve(monotonic_pairs_reserve_cap(pairs.size(), pairs.capacity(), n));
        EvalValue result = make_void();
        for (auto it = mapped.rbegin(); it != mapped.rend(); ++it) {
            const auto id = ev.mint_pair(*it, result, /*check_quota=*/true);
            if (id == Evaluator::kFileHeapMintDenied) {
                return make_primitive_error(
                    string_heap, error_values,
                    std::string(prim_heap_quota_exceeded_msg(PrimHeapDim::Pairs)),
                    ev.primitive_error_counter_ptr());
            }
            result = make_pair(id);
        }
        ev.bump_pair_alloc_count_n(static_cast<std::uint64_t>(n));
        return result;
    };
    register_prim(
        add, ev, "map",
        [walk_lists](std::span<const EvalValue> a) {
            return walk_lists(a, "map", /*collect=*/true);
        },
        pure_general(255, "(fn list ...) -> list",
                     "Apply fn to the k-th element of every list; stop at the shortest."));
    register_prim(
        add, ev, "for-each",
        [walk_lists](std::span<const EvalValue> a) {
            return walk_lists(a, "for-each", /*collect=*/false);
        },
        // Issue #4616: for-each invokes fn for its effects; impure_general keeps
        // the pure_general discovery defaults minus purity (no IO/mutation flags).
        impure_general(255, "(fn list ...) -> unspecified",
                       "Apply fn to the k-th element of every list for effect; "
                       "stop at the shortest."));
    register_prim(
        add, ev, "filter",
        [&pairs, &string_heap, &error_values, apply_pred, &ev](std::span<const EvalValue> a) {
            // (filter pred list) — keep elements where pred returns truthy
            // Issue #4228: snapshot cars under lock (same shape as map), then
            // apply Soft/native preds outside. Walking pairs_ live while
            // Soft std/math closures allocate via apply_closure dropped keeps
            // for direct (filter even? (list …)) while apply/let stayed green.
            if (a.size() < 2) {
                return make_primitive_error(string_heap, error_values, "filter: too few args",
                                            ev.primitive_error_counter_ptr());
            }
            if (is_void(a[1]))
                return make_void();
            if (!is_pair(a[1])) {
                return make_primitive_error(string_heap, error_values, "filter: not a list",
                                            ev.primitive_error_counter_ptr());
            }

            std::vector<EvalValue> cars;
            {
                Evaluator::ListCtorLockHold hold(ev);
                auto current = a[1];
                while (is_pair(current)) {
                    auto idx = as_pair_idx(current);
                    if (idx >= pairs.size())
                        break;
                    // Issue #4505: same reader gate as map — the predicate
                    // must not observe a foreign car either.
                    if (!ev.inrange_slot_tenant_allows(idx, /*vector_heap=*/false,
                                                       /*required_effects=*/0, "filter")) {
                        return make_primitive_error(
                            string_heap, error_values,
                            "filter: cross-tenant pair slot read denied (#4505)",
                            ev.primitive_error_counter_ptr());
                    }
                    cars.push_back(pairs[idx].car);
                    current = pairs[idx].cdr;
                    ev.bump_list_chain_traversals();
                    ev.bump_list_estimated_cache_misses();
                }
            }

            std::vector<EvalValue> kept;
            kept.reserve(cars.size());
            // Issue #4492: same face as map (#4480). cars/kept are the only
            // host-side roots for the snapshot elements while pred runs; a
            // tree-walker reclaim triggered inside an allocating pred would
            // freelist them (e.g. when the input list is a temporary such as
            // (filter p (map f xs))) and the result chain below would alias
            // recycled slots. Pin both for the whole apply + construction.
            const Evaluator::TwVecPin pin_cars(cars);
            const Evaluator::TwVecPin pin_kept(kept);
            for (const auto& car : cars) {
                // Issue #4586: same as map. The error used to be truthy,
                // so filter kept the element and try did not catch it.
                auto pred_v = apply_pred(a[0], car, true);
                if (is_error(pred_v))
                    return pred_v;
                if (aura::compiler::pure::is_truthy(pred_v))
                    kept.push_back(car);
            }

            Evaluator::ListCtorLockHold hold(ev);
            const auto n = kept.size();
            if (n == 0)
                return make_void();
            // Issue #4473: per-mint quota inside mint_pair (freelist hits do
            // not count) replaces the pairs.size()+n pre-check.
            pairs.reserve(monotonic_pairs_reserve_cap(pairs.size(), pairs.capacity(), n));
            EvalValue result = make_void();
            for (auto it = kept.rbegin(); it != kept.rend(); ++it) {
                const auto id = ev.mint_pair(*it, result, /*check_quota=*/true);
                if (id == Evaluator::kFileHeapMintDenied) {
                    return make_primitive_error(
                        string_heap, error_values,
                        std::string(prim_heap_quota_exceeded_msg(PrimHeapDim::Pairs)),
                        ev.primitive_error_counter_ptr());
                }
                result = make_pair(id);
            }
            ev.bump_pair_alloc_count_n(static_cast<std::uint64_t>(n));
            return result;
        },
        pure_general(2, "(pred list) -> list", "Keep elements where pred is truthy."));
    register_prim(
        add, ev, "take",
        [&pairs, &string_heap, &error_values, &ev](std::span<const EvalValue> a) {
            auto* counter = ev.primitive_error_counter_ptr();
            if (a.size() < 2) {
                return make_primitive_error(string_heap, error_values, "take: too few args",
                                            counter);
            }
            // Issue #4177: R7RS/SRFI-1 argument order — (take lst k) with the
            // list first. The previous (take k lst) contract made every
            // R7RS-order call fall into this count guard and return an opaque
            // primitive-error value (display -> <unknown>, pair? -> #f),
            // leaving the native binding unusable for list prefix/suffix.
            // Count guard stays ahead of the list guard so (take (list 1) "x")
            // keeps signalling the bad-count arm.
            if (!is_int(a[1]) || as_int(a[1]) < 0) {
                return make_primitive_error(string_heap, error_values,
                                            "take: count must be a non-negative integer", counter);
            }
            auto n = static_cast<std::size_t>(as_int(a[1]));
            auto v = a[0];
            if (n == 0 || is_end_of_list(v))
                return make_void();
            if (!is_pair(v)) {
                return make_primitive_error(string_heap, error_values, "take: not a list", counter);
            }
            // B-list-2026-07-18-A fix: bound n by list length BEFORE the
            // for-loop so the early-exit path (when n > length) cannot
            // bypass the post-loop reverse. Without this bound, callers
            // got the un-reversed accumulator as the return value, e.g.
            // (take 5 (list 10 20 30 40)) returned (40 30 20 10) instead
            // of (10 20 30 40). Walk the list once for length, clamp n,
            // then run the loop body exactly min(n, length) times.
            {
                std::size_t len = 0;
                auto probe = v;
                while (is_pair(probe)) {
                    auto pidx = as_pair_idx(probe);
                    if (pidx >= pairs.size())
                        break;
                    ++len;
                    probe = pairs[pidx].cdr;
                }
                if (n > len)
                    n = len;
            }
            EvalValue result = make_void();
            // Build result in reverse then reverse it
            for (std::size_t i = 0; i < n; ++i) {
                if (!is_pair(v))
                    return result;
                auto idx = as_pair_idx(v);
                if (idx >= pairs.size())
                    return result;
                const auto new_id = ev.mint_pair(pairs[idx].car, result, /*check_quota=*/true);
                if (new_id == Evaluator::kFileHeapMintDenied) {
                    return make_primitive_error(
                        string_heap, error_values,
                        std::string(prim_heap_quota_exceeded_msg(PrimHeapDim::Pairs)),
                        ev.primitive_error_counter_ptr());
                }
                ev.bump_pair_alloc_count(); // Issue #614
                result = make_pair(new_id);
                v = pairs[idx].cdr;
            }
            // Reverse to get correct order
            EvalValue final = make_void();
            while (!is_end_of_list(result)) {
                if (!is_pair(result))
                    break;
                auto idx = as_pair_idx(result);
                if (idx >= pairs.size())
                    break;
                const auto nid = ev.mint_pair(pairs[idx].car, final, /*check_quota=*/true);
                if (nid == Evaluator::kFileHeapMintDenied) {
                    return make_primitive_error(
                        string_heap, error_values,
                        std::string(prim_heap_quota_exceeded_msg(PrimHeapDim::Pairs)),
                        ev.primitive_error_counter_ptr());
                }
                ev.bump_pair_alloc_count(); // Issue #614
                final = make_pair(nid);
                result = pairs[idx].cdr;
            }
            return final;
        },
        pure_general(2, "(list int) -> list", "Prefix of n elements from a list."));
    register_prim(
        add, ev, "drop",
        [&pairs, &string_heap, &error_values, &ev](std::span<const EvalValue> a) {
            auto* counter = ev.primitive_error_counter_ptr();
            if (a.size() < 2) {
                return make_primitive_error(string_heap, error_values, "drop: too few args",
                                            counter);
            }
            // Issue #4177: R7RS/SRFI-1 argument order — (drop lst k) with the
            // list first; see the take note above for the opaque-face root
            // cause. Count guard stays ahead of the list guard.
            if (!is_int(a[1]) || as_int(a[1]) < 0) {
                return make_primitive_error(string_heap, error_values,
                                            "drop: count must be a non-negative integer", counter);
            }
            auto n = static_cast<std::size_t>(as_int(a[1]));
            auto v = a[0];
            if (n == 0)
                return v;
            if (is_end_of_list(v))
                return make_void();
            if (!is_pair(v)) {
                return make_primitive_error(string_heap, error_values, "drop: not a list", counter);
            }
            for (std::size_t i = 0; i < n; ++i) {
                if (is_end_of_list(v))
                    return make_void(); // documented: drop past end → empty
                if (!is_pair(v)) {
                    return make_primitive_error(string_heap, error_values, "drop: improper list",
                                                counter);
                }
                auto idx = as_pair_idx(v);
                if (idx >= pairs.size()) {
                    return make_primitive_error(string_heap, error_values, "drop: corrupted pair",
                                                counter);
                }
                v = pairs[idx].cdr;
            }
            return v;
        },
        pure_general(2, "(list int) -> list", "List after dropping n elements."));
    register_prim(
        add, ev, "foldl",
        [&pairs, apply_binary, &ev](std::span<const EvalValue> a) {
            if (a.size() < 3)
                return make_void();
            auto acc = a[1];
            auto lst = a[2];
            std::uint64_t steps = 0;

            while (!is_end_of_list(lst)) {
                if (!is_pair(lst))
                    break;
                auto idx = as_pair_idx(lst);
                if (idx >= pairs.size())
                    break;
                ev.bump_list_chain_traversals();
                ev.bump_list_estimated_cache_misses();
                acc = apply_binary(a[0], acc, pairs[idx].car, true);
                lst = pairs[idx].cdr;
                ++steps;
            }
            // Issue #614: bump the cdr-walk count once at the end.
            ev.bump_linear_traverse_count(steps, steps);
            return acc;
        },
        pure_general(3, "(fn acc list) -> any", "Left fold fn over list starting at acc."));

    // Issue #923 / #928: native iterative list-sort (no Aura recursion depth).
    // Issue #4549: the buffer used to be std::vector<int64_t>, so Floats came
    // back truncated to Ints and any string made it return () — silent data
    // loss that std/list `sort` passed through (() is a list). Now:
    //   (list-sort xs)        all numbers: numeric order, each element keeps
    //                         its own Int/Float type (stable; NaN sorts last);
    //                         all strings: lexicographic (byte) order.
    //   (list-sort xs less?)  user comparator, either argument order
    //   (list-sort less? xs)  (stable bottom-up merge, n log n calls).
    // Anything else (mixed kinds, bools, improper list) is a primitive
    // error, never a silent ().
    register_prim(
        add, ev, "list-sort",
        [&pairs, &string_heap, &error_values, &ev,
         apply_binary](std::span<const EvalValue> a) -> EvalValue {
            auto fail = [&](std::string msg) -> EvalValue {
                return make_primitive_error(string_heap, error_values, std::move(msg),
                                            ev.primitive_error_counter_ptr());
            };
            if (a.empty())
                return fail("list-sort: expected (list-sort list [less?]) (#4549)");
            if (a.size() > 2)
                return fail("list-sort: too many arguments; expected (list-sort list [less?]) "
                            "(#4549)");
            auto is_proc = [](const EvalValue& v) {
                return is_closure(v) || is_primitive(v) || is_cell(v);
            };
            EvalValue lst = a[0];
            EvalValue less = make_void();
            const bool has_less = a.size() == 2;
            if (has_less) {
                less = a[1];
                if (is_proc(a[0]) && !is_proc(a[1])) {
                    lst = a[1];
                    less = a[0];
                }
                if (!is_proc(less))
                    return fail("list-sort: comparator must be a procedure (#4549)");
            }

            // Collect the elements (same reader gates as length / car).
            std::vector<EvalValue> buf;
            buf.reserve(64);
            auto v = lst;
            while (!is_end_of_list(v)) {
                if (!is_pair(v))
                    return fail("list-sort: not a proper list (#4549)");
                auto idx = as_pair_idx(v);
                if (idx >= pairs.size()) {
                    // Issue #4567: JIT pair table.
                    auto slot = jit_pair_cdr(ev, idx, "list-sort");
                    if (slot.kind == JitCdrKind::Denied)
                        return fail("list-sort: cross-tenant pair slot read denied (#4111)");
                    if (slot.kind != JitCdrKind::Ok)
                        return fail("list-sort: not a proper list (#4549)");
                    buf.push_back(EvalValue{g_pair_slots[idx]->car});
                    v = slot.value;
                    continue;
                }
                if (!ev.inrange_slot_tenant_allows(idx, /*vector_heap=*/false,
                                                   /*required_effects=*/0, "list-sort"))
                    return fail("list-sort: cross-tenant pair slot read denied (#4505)");
                buf.push_back(pairs[idx].car);
                v = pairs[idx].cdr;
                ev.bump_list_chain_traversals();
            }

            if (has_less) {
                // Stable bottom-up merge sort. A comparator error aborts the
                // sort and is returned as-is.
                std::vector<EvalValue> tmp(buf.size());
                EvalValue cmp_error = make_void();
                bool failed = false;
                auto before = [&](const EvalValue& x, const EvalValue& y) -> bool {
                    if (failed)
                        return false;
                    auto r = apply_binary(less, x, y);
                    if (is_error(r)) {
                        failed = true;
                        cmp_error = r;
                        return false;
                    }
                    return aura::compiler::pure::is_truthy(r);
                };
                const std::size_t n = buf.size();
                for (std::size_t width = 1; width < n && !failed; width *= 2) {
                    for (std::size_t lo = 0; lo < n && !failed; lo += 2 * width) {
                        const std::size_t mid = std::min(lo + width, n);
                        const std::size_t hi = std::min(lo + 2 * width, n);
                        std::size_t i = lo, j = mid, k = lo;
                        while (i < mid && j < hi) {
                            // Take from the right run only when it is strictly
                            // before the left head: keeps equal keys stable.
                            if (before(buf[j], buf[i]))
                                tmp[k++] = buf[j++];
                            else
                                tmp[k++] = buf[i++];
                        }
                        while (i < mid)
                            tmp[k++] = buf[i++];
                        while (j < hi)
                            tmp[k++] = buf[j++];
                    }
                    if (!failed)
                        buf.swap(tmp);
                }
                if (failed)
                    return cmp_error;
            } else if (!buf.empty()) {
                bool all_num = true;
                bool all_str = true;
                for (const auto& e : buf) {
                    all_num = all_num && (is_int(e) || is_float(e));
                    all_str = all_str && is_string(e);
                }
                if (all_num) {
                    auto key = [](const EvalValue& e) -> long double {
                        // long double holds every int64 exactly on x86-64.
                        return is_int(e) ? static_cast<long double>(as_int(e))
                                         : static_cast<long double>(as_float(e));
                    };
                    std::stable_sort(buf.begin(), buf.end(),
                                     [&key](const EvalValue& x, const EvalValue& y) {
                                         if (is_int(x) && is_int(y))
                                             return as_int(x) < as_int(y);
                                         const auto kx = key(x);
                                         const auto ky = key(y);
                                         const bool nx = std::isnan(kx);
                                         const bool ny = std::isnan(ky);
                                         if (nx || ny)
                                             return !nx && ny; // NaN last
                                         return kx < ky;
                                     });
                } else if (all_str) {
                    std::vector<std::pair<std::string, EvalValue>> keyed;
                    keyed.reserve(buf.size());
                    for (const auto& e : buf) {
                        const auto sidx = as_string_idx(e);
                        if (sidx < string_heap.size() &&
                            !ev.inrange_string_tenant_allows(sidx, /*required_effects=*/0,
                                                             "list-sort"))
                            return fail("list-sort: cross-tenant string slot read denied (#4505)");
                        keyed.emplace_back(
                            sidx < string_heap.size() ? string_heap[sidx] : std::string{}, e);
                    }
                    std::stable_sort(keyed.begin(), keyed.end(), [](const auto& x, const auto& y) {
                        return x.first < y.first;
                    });
                    for (std::size_t i = 0; i < buf.size(); ++i)
                        buf[i] = keyed[i].second;
                } else {
                    return fail("list-sort: elements must be all numbers or all strings; "
                                "pass a comparator: (list-sort list less?) (#4549)");
                }
            }
            if (ev.compiler_metrics_) {
                auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics_);
                m->stdlib_list_iterative_sorts_total.fetch_add(1, std::memory_order_relaxed);
            }
            // Issue #2651 / #2997: timed lock + reserve + one allow.
            Evaluator::ListCtorLockHold hold(ev);
            const auto n = buf.size();
            // Issue #4473: per-mint quota inside mint_pair (freelist hits do
            // not count) replaces the pairs.size()+n pre-check.
            pairs.reserve(monotonic_pairs_reserve_cap(pairs.size(), pairs.capacity(), n));
            EvalValue result = make_void();
            for (auto it = buf.rbegin(); it != buf.rend(); ++it) {
                const auto new_id = ev.mint_pair(*it, result, /*check_quota=*/true);
                if (new_id == Evaluator::kFileHeapMintDenied) {
                    return make_primitive_error(
                        string_heap, error_values,
                        std::string(prim_heap_quota_exceeded_msg(PrimHeapDim::Pairs)),
                        ev.primitive_error_counter_ptr());
                }
                result = make_pair(new_id);
            }
            ev.bump_pair_alloc_count_n(static_cast<std::uint64_t>(n));
            return result;
        },
        pure_general(255, "(list [less?]) -> list",
                     "Sort a list: numbers numerically, strings lexicographically, or by a "
                     "(less? a b) comparator (stable)."));
}

} // namespace aura::compiler::primitives_detail
