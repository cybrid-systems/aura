// evaluator_primitives_pair.cpp — P0 step 2: pair/list/string heap primitives
// aura.compiler.evaluator module partition; registered via evaluator_primitives_registry.cpp.
//
// aura.compiler.evaluator module partition.
// Registration uses PrimRegistrar + heap refs (FFI/adt_runtime pattern).

module;


#include "runtime_shared.h"

module aura.compiler.evaluator;

import std;
import aura.compiler.value;

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

void register_pair_and_string_primitives(PrimRegistrar add, Evaluator& ev,
                                         std::pmr::vector<Pair>& pairs,
                                         std::pmr::vector<std::string>& string_heap,
                                         std::vector<EvalValue>& error_values,
                                         std::atomic<std::uint64_t>* primitive_error_counter) {
    add("string-copy", [&pairs, &string_heap, &error_values](std::span<const EvalValue> a) {
        if (a.empty() || !is_string(a[0]))
            return make_bool(false);
        // Strings are immutable-like; just return the same reference
        return a[0];
    });
    add("string-fill!", [&pairs, &string_heap, &error_values](std::span<const EvalValue> a) {
        if (a.size() < 2 || !is_string(a[0]) || !is_int(a[1]))
            return make_void();
        auto idx = as_string_idx(a[0]);
        if (idx >= string_heap.size())
            return make_void();
        auto fill_char = static_cast<char>(as_int(a[1]));
        std::fill(string_heap[idx].begin(), string_heap[idx].end(), fill_char);
        return make_void();
    });
    add("string->list", [&pairs, &string_heap, &error_values](std::span<const EvalValue> a) {
        if (a.empty() || !is_string(a[0]))
            return make_bool(false);
        auto idx = as_string_idx(a[0]);
        if (idx >= string_heap.size())
            return make_bool(false);
        auto& s = string_heap[idx];
        EvalValue result = make_void();
        for (auto it = s.rbegin(); it != s.rend(); ++it) {
            auto pid = pairs.size();
            pairs.push_back(
                {make_int(static_cast<std::int64_t>(static_cast<unsigned char>(*it))), result});
            result = make_pair(pid);
        }
        return result;
    });
    add("list->string", [&pairs, &string_heap, &error_values](std::span<const EvalValue> a) {
        if (a.empty())
            return make_bool(false);
        auto v = a[0];
        std::string result;
        while (is_pair(v)) {
            auto p = as_pair_idx(v);
            if (p >= pairs.size())
                break;
            auto car = pairs[p].car;
            if (is_int(car)) {
                result += static_cast<char>(as_int(car));
            } else if (is_string(car)) {
                auto sidx = as_string_idx(car);
                if (sidx < string_heap.size())
                    result += string_heap[sidx];
            }
            v = pairs[p].cdr;
        }
        auto sidx = string_heap.size();
        string_heap.push_back(result);
        return make_string(sidx);
    });
    add("string-join", [&pairs, &string_heap, &error_values](std::span<const EvalValue> a) {
        if (a.size() < 2 || !is_string(a[1]))
            return make_bool(false);
        auto delim_idx = as_string_idx(a[1]);
        if (delim_idx >= string_heap.size())
            return make_bool(false);
        auto& delim = string_heap[delim_idx];
        std::string result;
        bool first = true;
        auto v = a[0];
        while (is_pair(v)) {
            auto p = as_pair_idx(v);
            if (p >= pairs.size())
                break;
            auto car = pairs[p].car;
            if (is_string(car)) {
                auto sidx = as_string_idx(car);
                if (sidx < string_heap.size()) {
                    if (!first)
                        result += delim;
                    result += string_heap[sidx];
                    first = false;
                }
            }
            v = pairs[p].cdr;
        }
        auto sidx = string_heap.size();
        string_heap.push_back(result);
        return make_string(sidx);
    });

    // ── Pair / List / String primitives ─────────────────────────
    add("cons", [&pairs, &string_heap, &error_values](std::span<const EvalValue> a) {
        auto id = pairs.size();
        pairs.push_back({a[0], a[1]});
        return make_pair(id);
    });
    add("car", [&pairs, &string_heap, &error_values,
                primitive_error_counter](std::span<const EvalValue> a) {
        if (a.empty() || !is_pair(a[0])) {
            return make_primitive_error(string_heap, error_values, "car: not a pair",
                                        primitive_error_counter);
        }
        auto id = as_pair_idx(a[0]);
        if (id < pairs.size())
            return pairs[id].car;
        // Fallback to shared pair storage (JIT/arena pairs)
        if (id < g_pair_slots.size() && g_pair_slots[id])
            return types::EvalValue{g_pair_slots[id]->car};
        return make_int(0);
    });
    add("cdr", [&pairs, &string_heap, &error_values,
                primitive_error_counter](std::span<const EvalValue> a) {
        if (a.empty() || !is_pair(a[0])) {
            return make_primitive_error(string_heap, error_values, "cdr: not a pair",
                                        primitive_error_counter);
        }
        auto id = as_pair_idx(a[0]);
        if (id < pairs.size())
            return pairs[id].cdr;
        // Fallback to shared pair storage (JIT/arena pairs)
        if (id < g_pair_slots.size() && g_pair_slots[id])
            return types::EvalValue{g_pair_slots[id]->cdr};
        return make_int(0);
    });
    add("pair?", [](const auto& a) {
        if (a.empty())
            return make_bool(false);
        return make_bool(is_pair(a[0]));
    });

    // ── Cadr / Caddr shorthands ────────────────────────────────────
    add("caar", [&pairs, &string_heap, &error_values](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_pair(a[0]))
            return make_void();
        auto idx = as_pair_idx(a[0]);
        if (idx >= pairs.size())
            return make_void();
        auto c = pairs[idx].car;
        if (!is_pair(c))
            return make_void();
        return pairs[as_pair_idx(c)].car;
    });
    add("cadr", [&pairs, &string_heap, &error_values](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_pair(a[0]))
            return make_void();
        auto idx = as_pair_idx(a[0]);
        if (idx >= pairs.size())
            return make_void();
        auto c = pairs[idx].cdr;
        if (!is_pair(c))
            return make_void();
        return pairs[as_pair_idx(c)].car;
    });
    add("cdar", [&pairs, &string_heap, &error_values](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_pair(a[0]))
            return make_void();
        auto idx = as_pair_idx(a[0]);
        if (idx >= pairs.size())
            return make_void();
        auto c = pairs[idx].car;
        if (!is_pair(c))
            return make_void();
        return pairs[as_pair_idx(c)].cdr;
    });
    add("cddr", [&pairs, &string_heap, &error_values](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_pair(a[0]))
            return make_void();
        auto idx = as_pair_idx(a[0]);
        if (idx >= pairs.size())
            return make_void();
        auto c = pairs[idx].cdr;
        if (!is_pair(c))
            return make_void();
        return pairs[as_pair_idx(c)].cdr;
    });
    add("caaar", [&pairs, &string_heap, &error_values](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_pair(a[0]))
            return make_void();
        auto idx = as_pair_idx(a[0]);
        if (idx >= pairs.size())
            return make_void();
        auto c = pairs[idx].car;
        if (!is_pair(c))
            return make_void();
        auto d = pairs[as_pair_idx(c)].car;
        if (!is_pair(d))
            return make_void();
        return pairs[as_pair_idx(d)].car;
    });
    add("caadr", [&pairs, &string_heap, &error_values](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_pair(a[0]))
            return make_void();
        auto idx = as_pair_idx(a[0]);
        if (idx >= pairs.size())
            return make_void();
        auto c = pairs[idx].cdr;
        if (!is_pair(c))
            return make_void();
        auto d = pairs[as_pair_idx(c)].car;
        if (!is_pair(d))
            return make_void();
        return pairs[as_pair_idx(d)].car;
    });
    add("cadar", [&pairs, &string_heap, &error_values](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_pair(a[0]))
            return make_void();
        auto idx = as_pair_idx(a[0]);
        if (idx >= pairs.size())
            return make_void();
        auto c = pairs[idx].car;
        if (!is_pair(c))
            return make_void();
        auto d = pairs[as_pair_idx(c)].cdr;
        if (!is_pair(d))
            return make_void();
        return pairs[as_pair_idx(d)].car;
    });
    add("caddr", [&pairs, &string_heap, &error_values](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_pair(a[0]))
            return make_void();
        auto idx = as_pair_idx(a[0]);
        if (idx >= pairs.size())
            return make_void();
        auto c = pairs[idx].cdr;
        if (!is_pair(c))
            return make_void();
        auto d = pairs[as_pair_idx(c)].cdr;
        if (!is_pair(d))
            return make_void();
        return pairs[as_pair_idx(d)].car;
    });
    add("cdaar", [&pairs, &string_heap, &error_values](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_pair(a[0]))
            return make_void();
        auto idx = as_pair_idx(a[0]);
        if (idx >= pairs.size())
            return make_void();
        auto c = pairs[idx].car;
        if (!is_pair(c))
            return make_void();
        auto d = pairs[as_pair_idx(c)].car;
        if (!is_pair(d))
            return make_void();
        return pairs[as_pair_idx(d)].cdr;
    });
    add("cdadr", [&pairs, &string_heap, &error_values](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_pair(a[0]))
            return make_void();
        auto idx = as_pair_idx(a[0]);
        if (idx >= pairs.size())
            return make_void();
        auto c = pairs[idx].cdr;
        if (!is_pair(c))
            return make_void();
        auto d = pairs[as_pair_idx(c)].car;
        if (!is_pair(d))
            return make_void();
        return pairs[as_pair_idx(d)].cdr;
    });
    add("cddar", [&pairs, &string_heap, &error_values](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_pair(a[0]))
            return make_void();
        auto idx = as_pair_idx(a[0]);
        if (idx >= pairs.size())
            return make_void();
        auto c = pairs[idx].car;
        if (!is_pair(c))
            return make_void();
        auto d = pairs[as_pair_idx(c)].cdr;
        if (!is_pair(d))
            return make_void();
        return pairs[as_pair_idx(d)].cdr;
    });
    add("cdddr", [&pairs, &string_heap, &error_values](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_pair(a[0]))
            return make_void();
        auto idx = as_pair_idx(a[0]);
        if (idx >= pairs.size())
            return make_void();
        auto c = pairs[idx].cdr;
        if (!is_pair(c))
            return make_void();
        auto d = pairs[as_pair_idx(c)].cdr;
        if (!is_pair(d))
            return make_void();
        return pairs[as_pair_idx(d)].cdr;
    });

    // ── Mutable pair operations ───────────────────────────────────
    // Issue #1397 + #1399: set-car!/set-cdr! mutate an existing
    // pair slot. The pair-index resolution + field write must be
    // atomic relative to concurrent `pairs_.push_back` /
    // `pairs_.resize` that may reallocate the underlying vector
    // (invalidating the resolved index).
    //
    // #1397 wrapped the field write under `alloc_storage_lock_`,
    // but idx was resolved BEFORE the lock — leaving a window
    // where another fiber's push_back could realloc pairs_ between
    // idx capture and the locked write, producing a write to the
    // wrong slot (or to freed memory).
    //
    // #1399 fix: resolve idx INSIDE the lock_guard so the
    // (idx capture → bounds check → field write) sequence is one
    // atomic critical section. Combined with #1397, this closes
    // the cross-fiber pair-mutation race.
    add("set-car!",
        [&ev, &pairs, &string_heap, &error_values](std::span<const EvalValue> a) -> EvalValue {
            if (a.size() < 2 || !is_pair(a[0]))
                return make_void();
            // Issue #1397 + #1399: resolve idx + bounds check + field
            // write all under alloc_storage_lock_ so concurrent
            // push_back / resize cannot realloc pairs_ between
            // idx capture and write.
            std::lock_guard lock(ev.alloc_storage_lock_);
            auto idx = as_pair_idx(a[0]);
            if (idx < pairs.size()) {
                pairs[idx].car = a[1];
            } else if (idx < g_pair_slots.size() && g_pair_slots[idx]) {
                g_pair_slots[idx]->car = a[1].val;
            }
            return make_void();
        });
    add("set-cdr!",
        [&ev, &pairs, &string_heap, &error_values](std::span<const EvalValue> a) -> EvalValue {
            if (a.size() < 2 || !is_pair(a[0]))
                return make_void();
            // Issue #1397 + #1399: same atomic guard as set-car! —
            // idx capture inside lock_guard, not before.
            std::lock_guard lock(ev.alloc_storage_lock_);
            auto idx = as_pair_idx(a[0]);
            if (idx < pairs.size()) {
                pairs[idx].cdr = a[1];
            } else if (idx < g_pair_slots.size() && g_pair_slots[idx]) {
                g_pair_slots[idx]->cdr = a[1].val;
            }
            return make_void();
        });

    add("string?", [&pairs, &string_heap, &error_values](std::span<const EvalValue> a) {
        if (a.empty())
            return make_bool(false);
        return make_bool(is_string(a[0]));
    });
    add("string-append", [&pairs, &string_heap, &error_values](std::span<const EvalValue> a) {
        std::string result;
        for (auto& v : a) {
            if (is_string(v)) {
                auto idx = as_string_idx(v);
                if (idx < string_heap.size())
                    result += string_heap[idx];
            } else if (is_int(v)) {
                result += std::to_string(as_int(v));
            }
        }
        auto id = string_heap.size();
        string_heap.push_back(std::move(result));
        return make_string(id);
    });
    add("string-length", [&pairs, &string_heap, &error_values](std::span<const EvalValue> a) {
        if (a.empty())
            return make_int(0);
        std::size_t len = 0;
        if (is_string(a[0])) {
            auto idx = as_string_idx(a[0]);
            len = (idx < string_heap.size()) ? string_heap[idx].size() : 0;
        } else if (is_int(a[0])) {
            len = std::to_string(as_int(a[0])).size();
        }
        return make_int(static_cast<std::int64_t>(len));
    });
    add("string-ref", [&pairs, &string_heap, &error_values](std::span<const EvalValue> a) {
        if (a.size() < 2) {
            auto __i = string_heap.size();
            string_heap.push_back("string-ref: too few args");
            auto __e = error_values.size();
            error_values.push_back(make_string(__i));
            return make_error(__e);
        }
        std::string s;
        if (is_string(a[0])) {
            auto idx = as_string_idx(a[0]);
            if (idx < string_heap.size())
                s = string_heap[idx];
        } else if (is_int(a[0])) {
            s = std::to_string(as_int(a[0]));
        }
        auto pos = static_cast<std::size_t>(as_int(a[1]));
        if (pos >= s.size()) {
            auto __i = string_heap.size();
            string_heap.push_back("string-ref: index out of bounds");
            auto __e = error_values.size();
            error_values.push_back(make_string(__i));
            return make_error(__e);
        }
        if (pos < s.size())
            return make_int(static_cast<std::int64_t>(static_cast<unsigned char>(s[pos])));
        return make_int(0);
    });
    add("substring", [&pairs, &string_heap, &error_values](std::span<const EvalValue> a) {
        if (a.size() < 3)
            return make_int(0);
        std::string s_buf;
        const std::string* sp = nullptr;
        if (is_string(a[0])) {
            auto idx = as_string_idx(a[0]);
            if (idx < string_heap.size())
                sp = &string_heap[idx];
        } else if (is_int(a[0])) {
            s_buf = std::to_string(as_int(a[0]));
            sp = &s_buf;
        }
        if (!sp)
            return make_int(0);
        const auto& s = *sp;
        auto start = static_cast<std::size_t>(as_int(a[1]));
        auto end = static_cast<std::size_t>(as_int(a[2]));
        if (start > s.size())
            start = s.size();
        if (end > s.size())
            end = s.size();
        if (start >= end) {
            auto id = string_heap.size();
            string_heap.push_back("");
            return make_string(id);
        }
        auto sub = s.substr(start, end - start);
        auto nid = string_heap.size();
        string_heap.push_back(std::move(sub));
        return make_string(nid);
    });
    add("string=?", [&pairs, &string_heap, &error_values](std::span<const EvalValue> a) {
        if (a.size() < 2)
            return make_bool(false);
        auto to_str = [&pairs, &string_heap, &error_values](const EvalValue& v) -> std::string {
            if (is_string(v)) {
                auto idx = as_string_idx(v);
                return (idx < string_heap.size()) ? string_heap[idx] : "";
            }
            if (is_int(v))
                return std::to_string(as_int(v));
            return "";
        };
        return make_bool(to_str(a[0]) == to_str(a[1]));
    });
    add("string<?", [&pairs, &string_heap, &error_values](std::span<const EvalValue> a) {
        if (a.size() < 2)
            return make_bool(false);
        auto to_str = [&pairs, &string_heap, &error_values](const EvalValue& v) -> std::string {
            if (is_string(v)) {
                auto idx = as_string_idx(v);
                return (idx < string_heap.size()) ? string_heap[idx] : "";
            }
            if (is_int(v))
                return std::to_string(as_int(v));
            return "";
        };
        return make_bool(to_str(a[0]) < to_str(a[1]));
    });
    add("string->number", [&pairs, &string_heap, &error_values](std::span<const EvalValue> a) {
        if (a.size() < 1 || a.size() > 2)
            return make_int(0);
        auto to_str = [&pairs, &string_heap, &error_values](const EvalValue& v) -> std::string {
            if (is_string(v)) {
                auto idx = as_string_idx(v);
                return (idx < string_heap.size()) ? string_heap[idx] : "";
            }
            return "";
        };
        auto s = to_str(a[0]);
        auto radix = (a.size() > 1 && is_int(a[1])) ? static_cast<int>(as_int(a[1])) : 10;
        try {
            if (s.find('.') != std::string::npos)
                return make_float(std::stod(s));
            return make_int(static_cast<std::int64_t>(std::stoll(s, nullptr, radix)));
        } catch (...) {
            // [SILENCE-PRIM-#615] string-to-int/float parse failure —
            // (#) returns #f on parse failure per the documented pair
            // primitive contract; raising would break every existing
            // caller that relies on #f as the parse-failure sentinel.
            return make_bool(false);
        }
    });

    add("string-index", [&pairs, &string_heap, &error_values](std::span<const EvalValue> a) {
        if (a.size() < 2)
            return make_int(-1);
        auto haystack = (is_string(a[0]) && as_string_idx(a[0]) < string_heap.size())
                            ? string_heap[as_string_idx(a[0])]
                            : "";
        auto needle = (is_string(a[1]) && as_string_idx(a[1]) < string_heap.size())
                          ? string_heap[as_string_idx(a[1])]
                          : "";
        auto start = (a.size() > 2 && is_int(a[2])) ? static_cast<std::size_t>(as_int(a[2])) : 0;
        if (needle.empty())
            return make_int(0);
        auto pos = haystack.find(needle, start);
        return make_int(pos != std::string::npos ? static_cast<std::int64_t>(pos) : -1);
    });

    add("number->string", [&pairs, &string_heap, &error_values](std::span<const EvalValue> a) {
        if (a.empty())
            return make_int(0);
        std::string s;
        if (is_float(a[0]))
            s = std::to_string(as_float(a[0]));
        else if (is_int(a[0]))
            s = std::to_string(as_int(a[0]));
        else
            s = "0";
        // Trim trailing zeros from float representation
        if (is_float(a[0])) {
            auto dot = s.find('.');
            if (dot != std::string::npos) {
                auto last = s.find_last_not_of('0');
                if (last > dot)
                    s = s.substr(0, last + 1);
                else
                    s = s.substr(0, dot);
            }
        }
        auto id = string_heap.size();
        string_heap.push_back(std::move(s));
        return make_string(id);
    });
    add("string->number", [&pairs, &string_heap, &error_values](std::span<const EvalValue> a) {
        if (a.empty() || !is_string(a[0]))
            return make_bool(false);
        auto i = as_string_idx(a[0]);
        if (i >= string_heap.size())
            return make_bool(false);
        auto& str = string_heap[i];
        if (str.empty())
            return make_bool(false);
        // Trim leading/trailing whitespace (CSV fields, user input)
        auto first = str.find_first_not_of(" \t\r\n");
        if (first == std::string::npos)
            return make_bool(false);
        auto last = str.find_last_not_of(" \t\r\n");
        std::string_view trimmed(str.data() + first, last - first + 1);
        // Use from_chars — entire trimmed string must be consumed
        const char* start = trimmed.data();
        const char* end = start + trimmed.size();
        // Try float first (includes ints like "42" → 42.0)
        double fval;
        auto [pfloat, ec_float] = std::from_chars(start, end, fval);
        if (ec_float == std::errc{} && pfloat == end) {
            // Check if it has a decimal point or exponent → return float
            if (trimmed.find('.') != std::string_view::npos ||
                trimmed.find('e') != std::string_view::npos ||
                trimmed.find('E') != std::string_view::npos)
                return make_float(fval);
            return make_int(static_cast<std::int64_t>(fval));
        }
        return make_bool(false);
    });
}

} // namespace aura::compiler::primitives_detail
