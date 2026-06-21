// evaluator_primitives_list.cpp — P0 step 3: list primitives
// aura.compiler.evaluator module partition; registered via evaluator_primitives_registry.cpp.

module;

#include <string>
#include <vector>

module aura.compiler.evaluator;

import std;
import aura.compiler.value;
import aura.compiler.evaluator_pure;

namespace aura::compiler::primitives_detail {

using EvalValue = types::EvalValue;
using PrimFn = std::function<EvalValue(std::span<const EvalValue>)>;
using PrimRegistrar = std::function<void(std::string, PrimFn)>;

using namespace types;

namespace {

bool is_end_of_list(const EvalValue& v) {
    return is_void(v) || (is_int(v) && as_int(v) == 0);
}

}  // namespace

void register_list_primitives(PrimRegistrar add, std::pmr::vector<Pair>& pairs,
                              std::pmr::vector<std::string>& string_heap,
                              std::vector<EvalValue>& error_values, Evaluator& ev) {
    auto apply_unary = [&ev](const EvalValue& fn, const EvalValue& arg) -> EvalValue {
        if (is_primitive(fn)) {
            auto slot = as_primitive_slot(fn);
            if (slot >= ev.primitives_.slot_count())
                return make_void();
            auto prim = ev.primitives_.lookup(ev.primitives_.name_for_slot(slot));
            if (!prim)
                return make_void();
            return (*prim)({arg});
        }
        if (is_closure(fn)) {
            auto cid = as_closure_id(fn);
            auto result = ev.apply_closure(cid, {arg});
            return result ? *result : make_void();
        }
        return make_void();
    };
    auto apply_pred = [&ev](const EvalValue& fn, const EvalValue& arg) -> bool {
        if (is_primitive(fn)) {
            auto slot = as_primitive_slot(fn);
            if (slot >= ev.primitives_.slot_count())
                return false;
            auto prim = ev.primitives_.lookup(ev.primitives_.name_for_slot(slot));
            if (!prim)
                return false;
            return aura::compiler::pure::is_truthy((*prim)({arg}));
        }
        if (is_closure(fn)) {
            auto cid = as_closure_id(fn);
            auto result = ev.apply_closure(cid, {arg});
            return result ? aura::compiler::pure::is_truthy(*result) : false;
        }
        return false;
    };
    auto apply_binary = [&ev](const EvalValue& fn, const EvalValue& acc, const EvalValue& arg) -> EvalValue {
        if (is_primitive(fn)) {
            auto slot = as_primitive_slot(fn);
            if (slot >= ev.primitives_.slot_count())
                return make_void();
            auto prim = ev.primitives_.lookup(ev.primitives_.name_for_slot(slot));
            if (!prim)
                return make_void();
            return (*prim)({acc, arg});
        }
        if (is_closure(fn)) {
            auto cid = as_closure_id(fn);
            auto result = ev.apply_closure(cid, {acc, arg});
            return result ? *result : make_void();
        }
        return make_void();
    };
    add("list", [&pairs](std::span<const EvalValue> a) {
        // Build proper list (pair chain ending with void)
        EvalValue result = make_void();
        for (auto it = a.rbegin(); it != a.rend(); ++it) {
            auto id = pairs.size();
            pairs.push_back({*it, result});
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
        return make_int(1);
    });
    add("null?", [](const auto& a) {
        return make_bool(!a.empty() && (is_void(a[0]) || (is_int(a[0]) && as_int(a[0]) == 0)));
    });
    add("length", [&pairs](std::span<const EvalValue> a) {
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
        return make_int(n);
    });
    add("list-ref", [&pairs, &string_heap, &error_values](std::span<const EvalValue> a) {
        if (a.size() < 2) {
            auto __s = string_heap.size();
            string_heap.push_back("list-ref: too few args");
            auto __e = error_values.size();
            error_values.push_back(make_string(__s));
            return make_error(__e);
        }
        auto v = a[0];
        auto pos = static_cast<std::size_t>(as_int(a[1]));
        for (std::size_t i = 0; i < pos; ++i) {
            if (!is_pair(v)) {
                auto __s = string_heap.size();
                string_heap.push_back("list-ref: index out of bounds");
                auto __e = error_values.size();
                error_values.push_back(make_string(__s));
                return make_error(__e);
            }
            auto idx = as_pair_idx(v);
            if (idx >= pairs.size()) {
                auto __s = string_heap.size();
                string_heap.push_back("list-ref: corrupted pair");
                auto __e = error_values.size();
                error_values.push_back(make_string(__s));
                return make_error(__e);
            }
            v = pairs[idx].cdr;
        }
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
    add("append", [&pairs](std::span<const EvalValue> a) {
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
    add("reverse", [&pairs](std::span<const EvalValue> a) {
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
            result = make_pair(new_id);
            v = pairs[idx].cdr;
        }
        return result;
    });
    add("map", [&pairs, apply_unary](std::span<const EvalValue> a) {
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

            auto mapped = apply_unary(a[0], pairs[idx].car);

            auto new_id = pairs.size();
            pairs.push_back({mapped, make_void()});
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
    add("filter", [&pairs, apply_pred](std::span<const EvalValue> a) {
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

            bool keep = apply_pred(a[0], pairs[idx].car);
            if (keep) {
                auto new_id = pairs.size();
                pairs.push_back({pairs[idx].car, make_void()});
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
    add("take", [&pairs](std::span<const EvalValue> a) {
        if (a.size() < 2)
            return make_void();
        auto n = static_cast<std::size_t>(as_int(a[0]));
        auto v = a[1];
        if (n == 0 || is_end_of_list(v))
            return make_void();
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
    add("foldl", [&pairs, apply_binary](std::span<const EvalValue> a) {
        if (a.size() < 3)
            return make_void();
        auto acc = a[1];
        auto lst = a[2];

        while (!is_end_of_list(lst)) {
            if (!is_pair(lst))
                break;
            auto idx = as_pair_idx(lst);
            if (idx >= pairs.size())
                break;
            acc = apply_binary(a[0], acc, pairs[idx].car);
            lst = pairs[idx].cdr;
        }
        return acc;
    });

}

}  // namespace aura::compiler::primitives_detail
