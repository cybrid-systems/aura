// evaluator_primitives_list.cpp — P0 step 3: list primitives
// aura.compiler.evaluator module partition; registered via evaluator_primitives_registry.cpp.

module;

#include "primitives_detail.h"
#include "observability_metrics.h"

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

namespace {

    bool is_end_of_list(const EvalValue& v) {
        return is_void(v) || (is_int(v) && as_int(v) == 0);
    }

} // namespace

void register_list_primitives(PrimRegistrar add, std::pmr::vector<Pair>& pairs,
                              std::pmr::vector<std::string>& string_heap,
                              std::vector<EvalValue>& error_values, Evaluator& ev) {
    auto apply_unary = [&ev](const EvalValue& fn, const EvalValue& arg,
                             bool list_hotpath = false) -> EvalValue {
        if (is_primitive(fn)) {
            auto slot = as_primitive_slot(fn);
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
        if (is_closure(fn)) {
            auto cid = as_closure_id(fn);
            ev.bump_primitives_apply_closure_calls();
            if (list_hotpath)
                ev.bump_list_estimated_cache_misses();
            auto result = ev.apply_closure(cid, {arg});
            return result ? *result : make_void();
        }
        return make_void();
    };
    auto apply_pred = [&ev](const EvalValue& fn, const EvalValue& arg,
                            bool list_hotpath = false) -> bool {
        if (is_primitive(fn)) {
            auto slot = as_primitive_slot(fn);
            ev.bump_primitives_apply_lookup_hits();
            auto prim = ev.primitives_.slot_lookup_fast(slot);
            if (!prim)
                return false;
            ev.bump_primitives_apply_fastpath_wins();
            ev.bump_list_intrinsic_dispatches();
            if (list_hotpath)
                ev.bump_list_soa_hits();
            primitives_detail::prim_record_fastpath_hit_for_slot(
                static_cast<CompilerMetrics*>(ev.compiler_metrics()), slot);
            return aura::compiler::pure::is_truthy((*prim)({arg}));
        }
        if (is_closure(fn)) {
            auto cid = as_closure_id(fn);
            ev.bump_primitives_apply_closure_calls();
            if (list_hotpath)
                ev.bump_list_estimated_cache_misses();
            auto result = ev.apply_closure(cid, {arg});
            return result ? aura::compiler::pure::is_truthy(*result) : false;
        }
        return false;
    };
    auto apply_binary = [&ev](const EvalValue& fn, const EvalValue& acc, const EvalValue& arg,
                              bool list_hotpath = false) -> EvalValue {
        if (is_primitive(fn)) {
            auto slot = as_primitive_slot(fn);
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
        if (is_closure(fn)) {
            auto cid = as_closure_id(fn);
            ev.bump_primitives_apply_closure_calls();
            if (list_hotpath)
                ev.bump_list_estimated_cache_misses();
            auto result = ev.apply_closure(cid, {acc, arg});
            return result ? *result : make_void();
        }
        return make_void();
    };
    add("list", [&pairs, &ev](std::span<const EvalValue> a) {
        // Build proper list (pair chain ending with void)
        EvalValue result = make_void();
        for (auto it = a.rbegin(); it != a.rend(); ++it) {
            auto id = pairs.size();
            pairs.push_back({*it, result});
            ev.bump_pair_alloc_count(); // Issue #614
            result = make_pair(id);
        }
        return result;
    });
    add("list?", [&pairs](std::span<const EvalValue> a) {
        if (a.empty())
            return make_bool(true);
        auto v = a[0];
        while (!is_end_of_list(v)) {
            if (!is_pair(v))
                return make_bool(false);
            auto idx = as_pair_idx(v);
            if (idx >= pairs.size())
                return make_bool(false);
            v = pairs[idx].cdr; // follow cdr chain
        }
        // Issue #2026-07-17 (commercial-readiness audit): predicates must
        // return make_bool(true), not make_int(1). Several stdlib sites
        // (filter, list-statistics, etc.) treat truthy values as numeric
        // and silently dropped list? results; the surface test worked
        // around this bug by checking (length ...) instead of (list? ...).
        // Now consistent with all other predicates.
        return make_bool(true);
    });
    add("null?", [](const auto& a) {
        return make_bool(!a.empty() && (is_void(a[0]) || (is_int(a[0]) && as_int(a[0]) == 0)));
    });
    add("length", [&pairs, &ev](std::span<const EvalValue> a) {
        if (a.empty())
            return make_int(0);
        auto v = a[0];
        std::int64_t n = 0;
        while (!is_end_of_list(v)) {
            if (!is_pair(v))
                return make_int(0);
            auto idx = as_pair_idx(v);
            if (idx >= pairs.size())
                return make_int(0);
            v = pairs[idx].cdr;
            n++;
        }
        // Issue #614: surface the cdr-walk cost so AI agents can
        // see list-depth vs pair_alloc in production.
        ev.bump_linear_traverse_count(static_cast<std::uint64_t>(n), static_cast<std::uint64_t>(n));
        return make_int(n);
    });
    add("list-ref", [&pairs, &string_heap, &error_values, &ev](std::span<const EvalValue> a) {
        auto* counter = ev.primitive_error_counter_ptr();
        if (a.size() < 2) {
            return make_primitive_error(string_heap, error_values, "list-ref: too few args",
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
                return make_primitive_error(string_heap, error_values, "list-ref: corrupted pair",
                                            counter);
            }
            v = pairs[idx].cdr;
        }
        // Issue #614: surface the cdr-walk depth for list-ref so the
        // high-water tracks worst-case positional access cost.
        ev.bump_linear_traverse_count(pos, pos);
        if (is_pair(v)) {
            auto idx = as_pair_idx(v);
            return idx < pairs.size() ? pairs[idx].car : make_int(0);
        }
        return v;
    });
    // (member val list) — Find val in list using content equality (equal?)
    // Returns the tail of the list starting with val, or #f if not found
    add("member", [&pairs, &string_heap](std::span<const EvalValue> a) {
        if (a.size() < 2)
            return make_int(0);
        auto& val = a[0];
        auto v = a[1];
        auto elem_eq = [&](const EvalValue& x, const EvalValue& y) -> bool {
            if (x == y)
                return true;
            if (is_int(x) && is_int(y))
                return as_int(x) == as_int(y);
            if (is_string(x) && is_string(y)) {
                auto xi = as_string_idx(x), yi = as_string_idx(y);
                return xi < string_heap.size() && yi < string_heap.size() &&
                       string_heap[xi] == string_heap[yi];
            }
            if (is_bool(x) && is_bool(y))
                return as_bool(x) == as_bool(y);
            return false;
        };
        while (!is_end_of_list(v)) {
            if (!is_pair(v))
                return make_int(0);
            auto idx = as_pair_idx(v);
            if (idx >= pairs.size())
                return make_int(0);
            if (elem_eq(pairs[idx].car, val))
                return v;
            v = pairs[idx].cdr;
        }
        return make_int(0);
    });
    // (append list ...) — Variadic: concatenate all provided lists
    add("append", [&pairs, &ev](std::span<const EvalValue> a) {
        if (a.empty())
            return make_void();
        if (a.size() < 2)
            return a[0];
        // Iteratively append all arguments
        auto result = a[0];
        for (std::size_t i = 1; i < a.size(); ++i) {
            auto list2 = a[i];
            if (is_end_of_list(result)) {
                result = list2;
                continue;
            }
            EvalValue new_result = make_void();
            EvalValue tail = make_void();
            auto v = result;
            while (!is_end_of_list(v)) {
                if (!is_pair(v)) {
                    result = a[0];
                    break;
                }
                auto idx = as_pair_idx(v);
                if (idx >= pairs.size()) {
                    result = a[0];
                    break;
                }
                auto new_id = pairs.size();
                pairs.push_back({pairs[idx].car, make_void()});
                ev.bump_pair_alloc_count(); // Issue #614
                auto new_pair = make_pair(new_id);
                if (is_void(new_result))
                    new_result = new_pair;
                else {
                    auto tidx = as_pair_idx(tail);
                    pairs[tidx].cdr = new_pair;
                }
                tail = new_pair;
                v = pairs[idx].cdr;
            }
            if (!is_void(tail)) {
                auto tidx = as_pair_idx(tail);
                pairs[tidx].cdr = list2;
            }
            if (!is_void(new_result))
                result = new_result;
        }
        return result;
    });
    add("reverse", [&pairs, &ev](std::span<const EvalValue> a) {
        if (a.empty())
            return make_int(0);
        auto v = a[0];
        EvalValue result = make_void();
        while (!is_end_of_list(v)) {
            if (!is_pair(v))
                return a[0];
            auto idx = as_pair_idx(v);
            if (idx >= pairs.size())
                return a[0];
            auto new_id = pairs.size();
            pairs.push_back({pairs[idx].car, result});
            ev.bump_pair_alloc_count(); // Issue #614
            result = make_pair(new_id);
            v = pairs[idx].cdr;
        }
        return result;
    });
    add("map", [&pairs, apply_unary, &ev](std::span<const EvalValue> a) {
        // (map func list) — apply func to each element, collect results
        if (a.size() < 2 || is_void(a[1]))
            return make_void();

        EvalValue result = make_void();
        EvalValue tail = make_void();
        bool first = true;
        EvalValue current = a[1];

        while (is_pair(current)) {
            auto idx = as_pair_idx(current);
            if (idx >= pairs.size())
                break;

            ev.bump_list_chain_traversals();
            ev.bump_list_estimated_cache_misses();
            auto mapped = apply_unary(a[0], pairs[idx].car, true);

            auto new_id = pairs.size();
            pairs.push_back({mapped, make_void()});
            ev.bump_pair_alloc_count(); // Issue #614
            auto new_pair = make_pair(new_id);

            if (first) {
                result = new_pair;
                tail = new_pair;
                first = false;
            } else {
                auto tail_idx = as_pair_idx(tail);
                if (tail_idx < pairs.size())
                    pairs[tail_idx].cdr = new_pair;
                tail = new_pair;
            }

            current = pairs[idx].cdr;
        }

        return result;
    });
    add("filter", [&pairs, apply_pred, &ev](std::span<const EvalValue> a) {
        // (filter pred list) — keep elements where pred returns truthy
        if (a.size() < 2 || is_void(a[1]))
            return make_void();

        EvalValue result = make_void();
        EvalValue tail = make_void();
        bool first = true;
        EvalValue current = a[1];

        while (is_pair(current)) {
            auto idx = as_pair_idx(current);
            if (idx >= pairs.size())
                break;

            ev.bump_list_chain_traversals();
            ev.bump_list_estimated_cache_misses();
            bool keep = apply_pred(a[0], pairs[idx].car, true);
            if (keep) {
                auto new_id = pairs.size();
                pairs.push_back({pairs[idx].car, make_void()});
                ev.bump_pair_alloc_count(); // Issue #614
                auto new_pair = make_pair(new_id);

                if (first) {
                    result = new_pair;
                    tail = new_pair;
                    first = false;
                } else {
                    auto tail_idx = as_pair_idx(tail);
                    if (tail_idx < pairs.size())
                        pairs[tail_idx].cdr = new_pair;
                    tail = new_pair;
                }
            }

            current = pairs[idx].cdr;
        }

        return result;
    });
    add("take", [&pairs, &ev](std::span<const EvalValue> a) {
        if (a.size() < 2)
            return make_void();
        auto n = static_cast<std::size_t>(as_int(a[0]));
        auto v = a[1];
        if (n == 0 || is_end_of_list(v))
            return make_void();
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
            auto new_id = pairs.size();
            pairs.push_back({pairs[idx].car, result});
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
            auto nid = pairs.size();
            pairs.push_back({pairs[idx].car, final});
            ev.bump_pair_alloc_count(); // Issue #614
            final = make_pair(nid);
            result = pairs[idx].cdr;
        }
        return final;
    });
    add("drop", [&pairs](std::span<const EvalValue> a) {
        if (a.size() < 2)
            return make_void();
        auto n = static_cast<std::size_t>(as_int(a[0]));
        auto v = a[1];
        for (std::size_t i = 0; i < n; ++i) {
            if (is_end_of_list(v))
                return make_void();
            if (!is_pair(v))
                return make_void();
            auto idx = as_pair_idx(v);
            if (idx >= pairs.size())
                return make_void();
            v = pairs[idx].cdr;
        }
        return v;
    });
    add("foldl", [&pairs, apply_binary, &ev](std::span<const EvalValue> a) {
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
    });

    // Issue #923 / #928: native iterative list-sort (no Aura recursion depth).
    // Sorts a proper list of numbers ascending via std::sort on a temp buffer.
    ev.primitives_.add(
        "list-sort",
        [&pairs, &ev](std::span<const EvalValue> a) -> EvalValue {
            if (a.empty())
                return make_void();
            std::vector<std::int64_t> buf;
            buf.reserve(64);
            auto v = a[0];
            while (!is_end_of_list(v)) {
                if (!is_pair(v))
                    return make_void(); // improper list
                auto idx = as_pair_idx(v);
                if (idx >= pairs.size())
                    return make_void();
                auto car = pairs[idx].car;
                if (!is_int(car) && !is_float(car))
                    return make_void(); // non-numeric
                if (is_int(car))
                    buf.push_back(as_int(car));
                else
                    buf.push_back(static_cast<std::int64_t>(as_float(car)));
                v = pairs[idx].cdr;
                ev.bump_list_chain_traversals();
            }
            std::sort(buf.begin(), buf.end());
            if (ev.compiler_metrics_) {
                auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics_);
                m->stdlib_list_iterative_sorts_total.fetch_add(1, std::memory_order_relaxed);
            }
            EvalValue result = make_void();
            for (auto it = buf.rbegin(); it != buf.rend(); ++it) {
                auto new_id = pairs.size();
                pairs.push_back({make_int(*it), result});
                ev.bump_pair_alloc_count();
                result = make_pair(new_id);
            }
            return result;
        },
        PrimMeta{.arity = 1,
                 .pure = true,
                 .safety_flags = 0,
                 .perf_tier = 1,
                 .security_level = 1,
                 .doc = "Iterative sort of a numeric list (Issue #923).",
                 .category = "general",
                 .schema = "(list) -> list"});
}

} // namespace aura::compiler::primitives_detail
