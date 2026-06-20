// evaluator_primitives_pair.cpp — P0 step 2: pair/list/string heap primitives
// extracted from evaluator_impl.cpp::init_pair_primitives().
//
// Same module partition as evaluator_impl.cpp (aura.compiler.evaluator).
// Registration uses PrimRegistrar + heap refs (FFI/adt_runtime pattern).

module;

#include <algorithm>
#include <charconv>
#include <string>
#include "runtime_shared.h"

module aura.compiler.evaluator;

import std;
import aura.compiler.value;

namespace aura::compiler::primitives_detail {

using EvalValue = types::EvalValue;
using PrimFn = std::function<EvalValue(std::span<const EvalValue>)>;
using PrimRegistrar = std::function<void(std::string, PrimFn)>;

struct PairStringHeaps {
    std::pmr::vector<Pair>& pairs;
    std::pmr::vector<std::string>& string_heap;
    std::vector<EvalValue>& error_values;
};

using namespace types;

void register_pair_and_string_primitives(PrimRegistrar add, std::pmr::vector<Pair>& pairs,
                                         std::pmr::vector<std::string>& string_heap,
                                         std::vector<EvalValue>& error_values) {
    PairStringHeaps heaps{pairs, string_heap, error_values};
    add("string-copy", [&heaps](std::span<const EvalValue> a) {
        if (a.empty() || !is_string(a[0]))
            return make_bool(false);
        // Strings are immutable-like; just return the same reference
        return a[0];
    });
    add("string-fill!", [&heaps](std::span<const EvalValue> a) {
        if (a.size() < 2 || !is_string(a[0]) || !is_int(a[1]))
            return make_void();
        auto idx = as_string_idx(a[0]);
        if (idx >= heaps.string_heap.size())
            return make_void();
        auto fill_char = static_cast<char>(as_int(a[1]));
        std::fill(heaps.string_heap[idx].begin(), heaps.string_heap[idx].end(), fill_char);
        return make_void();
    });
    add("string->list", [&heaps](std::span<const EvalValue> a) {
        if (a.empty() || !is_string(a[0]))
            return make_bool(false);
        auto idx = as_string_idx(a[0]);
        if (idx >= heaps.string_heap.size())
            return make_bool(false);
        auto& s = heaps.string_heap[idx];
        EvalValue result = make_void();
        for (auto it = s.rbegin(); it != s.rend(); ++it) {
            auto pid = heaps.pairs.size();
            heaps.pairs.push_back(
                {make_int(static_cast<std::int64_t>(static_cast<unsigned char>(*it))), result});
            result = make_pair(pid);
        }
        return result;
    });
    add("list->string", [&heaps](std::span<const EvalValue> a) {
        if (a.empty())
            return make_bool(false);
        auto v = a[0];
        std::string result;
        while (is_pair(v)) {
            auto p = as_pair_idx(v);
            if (p >= heaps.pairs.size())
                break;
            auto car = heaps.pairs[p].car;
            if (is_int(car)) {
                result += static_cast<char>(as_int(car));
            } else if (is_string(car)) {
                auto sidx = as_string_idx(car);
                if (sidx < heaps.string_heap.size())
                    result += heaps.string_heap[sidx];
            }
            v = heaps.pairs[p].cdr;
        }
        auto sidx = heaps.string_heap.size();
        heaps.string_heap.push_back(result);
        return make_string(sidx);
    });
    add("string-join", [&heaps](std::span<const EvalValue> a) {
        if (a.size() < 2 || !is_string(a[1]))
            return make_bool(false);
        auto delim_idx = as_string_idx(a[1]);
        if (delim_idx >= heaps.string_heap.size())
            return make_bool(false);
        auto& delim = heaps.string_heap[delim_idx];
        std::string result;
        bool first = true;
        auto v = a[0];
        while (is_pair(v)) {
            auto p = as_pair_idx(v);
            if (p >= heaps.pairs.size())
                break;
            auto car = heaps.pairs[p].car;
            if (is_string(car)) {
                auto sidx = as_string_idx(car);
                if (sidx < heaps.string_heap.size()) {
                    if (!first)
                        result += delim;
                    result += heaps.string_heap[sidx];
                    first = false;
                }
            }
            v = heaps.pairs[p].cdr;
        }
        auto sidx = heaps.string_heap.size();
        heaps.string_heap.push_back(result);
        return make_string(sidx);
    });

    // ── Pair / List / String primitives ─────────────────────────
    add("cons", [&heaps](std::span<const EvalValue> a) {
        auto id = heaps.pairs.size();
        heaps.pairs.push_back({a[0], a[1]});
        return make_pair(id);
    });
    add("car", [&heaps](std::span<const EvalValue> a) {
        if (a.empty() || !is_pair(a[0])) {
            do {
                auto __e_sidx = heaps.string_heap.size();
                heaps.string_heap.push_back("car: not a pair");
                auto __e_eidx = heaps.error_values.size();
                heaps.error_values.push_back(make_string(__e_sidx));
                return make_error(__e_eidx);
            } while (0);
        }
        auto id = as_pair_idx(a[0]);
        if (id < heaps.pairs.size())
            return heaps.pairs[id].car;
        // Fallback to shared pair storage (JIT/arena pairs)
        if (id < g_pair_slots.size() && g_pair_slots[id])
            return types::EvalValue{g_pair_slots[id]->car};
        return make_int(0);
    });
    add("cdr", [&heaps](std::span<const EvalValue> a) {
        if (a.empty() || !is_pair(a[0])) {
            do {
                auto __e_sidx = heaps.string_heap.size();
                heaps.string_heap.push_back("cdr: not a pair");
                auto __e_eidx = heaps.error_values.size();
                heaps.error_values.push_back(make_string(__e_sidx));
                return make_error(__e_eidx);
            } while (0);
        }
        auto id = as_pair_idx(a[0]);
        if (id < heaps.pairs.size())
            return heaps.pairs[id].cdr;
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
    add("caar", [&heaps](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_pair(a[0]))
            return make_void();
        auto idx = as_pair_idx(a[0]);
        if (idx >= heaps.pairs.size())
            return make_void();
        auto c = heaps.pairs[idx].car;
        if (!is_pair(c))
            return make_void();
        return heaps.pairs[as_pair_idx(c)].car;
    });
    add("cadr", [&heaps](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_pair(a[0]))
            return make_void();
        auto idx = as_pair_idx(a[0]);
        if (idx >= heaps.pairs.size())
            return make_void();
        auto c = heaps.pairs[idx].cdr;
        if (!is_pair(c))
            return make_void();
        return heaps.pairs[as_pair_idx(c)].car;
    });
    add("cdar", [&heaps](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_pair(a[0]))
            return make_void();
        auto idx = as_pair_idx(a[0]);
        if (idx >= heaps.pairs.size())
            return make_void();
        auto c = heaps.pairs[idx].car;
        if (!is_pair(c))
            return make_void();
        return heaps.pairs[as_pair_idx(c)].cdr;
    });
    add("cddr", [&heaps](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_pair(a[0]))
            return make_void();
        auto idx = as_pair_idx(a[0]);
        if (idx >= heaps.pairs.size())
            return make_void();
        auto c = heaps.pairs[idx].cdr;
        if (!is_pair(c))
            return make_void();
        return heaps.pairs[as_pair_idx(c)].cdr;
    });
    add("caaar", [&heaps](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_pair(a[0]))
            return make_void();
        auto idx = as_pair_idx(a[0]);
        if (idx >= heaps.pairs.size())
            return make_void();
        auto c = heaps.pairs[idx].car;
        if (!is_pair(c))
            return make_void();
        auto d = heaps.pairs[as_pair_idx(c)].car;
        if (!is_pair(d))
            return make_void();
        return heaps.pairs[as_pair_idx(d)].car;
    });
    add("caadr", [&heaps](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_pair(a[0]))
            return make_void();
        auto idx = as_pair_idx(a[0]);
        if (idx >= heaps.pairs.size())
            return make_void();
        auto c = heaps.pairs[idx].cdr;
        if (!is_pair(c))
            return make_void();
        auto d = heaps.pairs[as_pair_idx(c)].car;
        if (!is_pair(d))
            return make_void();
        return heaps.pairs[as_pair_idx(d)].car;
    });
    add("cadar", [&heaps](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_pair(a[0]))
            return make_void();
        auto idx = as_pair_idx(a[0]);
        if (idx >= heaps.pairs.size())
            return make_void();
        auto c = heaps.pairs[idx].car;
        if (!is_pair(c))
            return make_void();
        auto d = heaps.pairs[as_pair_idx(c)].cdr;
        if (!is_pair(d))
            return make_void();
        return heaps.pairs[as_pair_idx(d)].car;
    });
    add("caddr", [&heaps](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_pair(a[0]))
            return make_void();
        auto idx = as_pair_idx(a[0]);
        if (idx >= heaps.pairs.size())
            return make_void();
        auto c = heaps.pairs[idx].cdr;
        if (!is_pair(c))
            return make_void();
        auto d = heaps.pairs[as_pair_idx(c)].cdr;
        if (!is_pair(d))
            return make_void();
        return heaps.pairs[as_pair_idx(d)].car;
    });
    add("cdaar", [&heaps](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_pair(a[0]))
            return make_void();
        auto idx = as_pair_idx(a[0]);
        if (idx >= heaps.pairs.size())
            return make_void();
        auto c = heaps.pairs[idx].car;
        if (!is_pair(c))
            return make_void();
        auto d = heaps.pairs[as_pair_idx(c)].car;
        if (!is_pair(d))
            return make_void();
        return heaps.pairs[as_pair_idx(d)].cdr;
    });
    add("cdadr", [&heaps](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_pair(a[0]))
            return make_void();
        auto idx = as_pair_idx(a[0]);
        if (idx >= heaps.pairs.size())
            return make_void();
        auto c = heaps.pairs[idx].cdr;
        if (!is_pair(c))
            return make_void();
        auto d = heaps.pairs[as_pair_idx(c)].car;
        if (!is_pair(d))
            return make_void();
        return heaps.pairs[as_pair_idx(d)].cdr;
    });
    add("cddar", [&heaps](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_pair(a[0]))
            return make_void();
        auto idx = as_pair_idx(a[0]);
        if (idx >= heaps.pairs.size())
            return make_void();
        auto c = heaps.pairs[idx].car;
        if (!is_pair(c))
            return make_void();
        auto d = heaps.pairs[as_pair_idx(c)].cdr;
        if (!is_pair(d))
            return make_void();
        return heaps.pairs[as_pair_idx(d)].cdr;
    });
    add("cdddr", [&heaps](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_pair(a[0]))
            return make_void();
        auto idx = as_pair_idx(a[0]);
        if (idx >= heaps.pairs.size())
            return make_void();
        auto c = heaps.pairs[idx].cdr;
        if (!is_pair(c))
            return make_void();
        auto d = heaps.pairs[as_pair_idx(c)].cdr;
        if (!is_pair(d))
            return make_void();
        return heaps.pairs[as_pair_idx(d)].cdr;
    });

    // ── Mutable pair operations ───────────────────────────────────
    add("set-car!", [&heaps](std::span<const EvalValue> a) -> EvalValue {
        if (a.size() < 2 || !is_pair(a[0]))
            return make_void();
        auto idx = as_pair_idx(a[0]);
        if (idx < heaps.pairs.size()) {
            heaps.pairs[idx].car = a[1];
        } else if (idx < g_pair_slots.size() && g_pair_slots[idx]) {
            g_pair_slots[idx]->car = a[1].val;
        }
        return make_void();
    });
    add("set-cdr!", [&heaps](std::span<const EvalValue> a) -> EvalValue {
        if (a.size() < 2 || !is_pair(a[0]))
            return make_void();
        auto idx = as_pair_idx(a[0]);
        if (idx < heaps.pairs.size()) {
            heaps.pairs[idx].cdr = a[1];
        } else if (idx < g_pair_slots.size() && g_pair_slots[idx]) {
            g_pair_slots[idx]->cdr = a[1].val;
        }
        return make_void();
    });

    add("string?", [&heaps](std::span<const EvalValue> a) {
        if (a.empty())
            return make_bool(false);
        return make_bool(is_string(a[0]));
    });
    add("string-append", [&heaps](std::span<const EvalValue> a) {
        std::string result;
        for (auto& v : a) {
            if (is_string(v)) {
                auto idx = as_string_idx(v);
                if (idx < heaps.string_heap.size())
                    result += heaps.string_heap[idx];
            } else if (is_int(v)) {
                result += std::to_string(as_int(v));
            }
        }
        auto id = heaps.string_heap.size();
        heaps.string_heap.push_back(std::move(result));
        return make_string(id);
    });
    add("string-length", [&heaps](std::span<const EvalValue> a) {
        if (a.empty())
            return make_int(0);
        std::size_t len = 0;
        if (is_string(a[0])) {
            auto idx = as_string_idx(a[0]);
            len = (idx < heaps.string_heap.size()) ? heaps.string_heap[idx].size() : 0;
        } else if (is_int(a[0])) {
            len = std::to_string(as_int(a[0])).size();
        }
        return make_int(static_cast<std::int64_t>(len));
    });
    add("string-ref", [&heaps](std::span<const EvalValue> a) {
        if (a.size() < 2) {
            auto __i = heaps.string_heap.size();
            heaps.string_heap.push_back("string-ref: too few args");
            auto __e = heaps.error_values.size();
            heaps.error_values.push_back(make_string(__i));
            return make_error(__e);
        }
        std::string s;
        if (is_string(a[0])) {
            auto idx = as_string_idx(a[0]);
            if (idx < heaps.string_heap.size())
                s = heaps.string_heap[idx];
        } else if (is_int(a[0])) {
            s = std::to_string(as_int(a[0]));
        }
        auto pos = static_cast<std::size_t>(as_int(a[1]));
        if (pos >= s.size()) {
            auto __i = heaps.string_heap.size();
            heaps.string_heap.push_back("string-ref: index out of bounds");
            auto __e = heaps.error_values.size();
            heaps.error_values.push_back(make_string(__i));
            return make_error(__e);
        }
        if (pos < s.size())
            return make_int(static_cast<std::int64_t>(static_cast<unsigned char>(s[pos])));
        return make_int(0);
    });
    add("substring", [&heaps](std::span<const EvalValue> a) {
        if (a.size() < 3)
            return make_int(0);
        std::string s_buf;
        const std::string* sp = nullptr;
        if (is_string(a[0])) {
            auto idx = as_string_idx(a[0]);
            if (idx < heaps.string_heap.size())
                sp = &heaps.string_heap[idx];
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
            auto id = heaps.string_heap.size();
            heaps.string_heap.push_back("");
            return make_string(id);
        }
        auto sub = s.substr(start, end - start);
        auto nid = heaps.string_heap.size();
        heaps.string_heap.push_back(std::move(sub));
        return make_string(nid);
    });
    add("string=?", [&heaps](std::span<const EvalValue> a) {
        if (a.size() < 2)
            return make_bool(false);
        auto to_str = [&heaps](const EvalValue& v) -> std::string {
            if (is_string(v)) {
                auto idx = as_string_idx(v);
                return (idx < heaps.string_heap.size()) ? heaps.string_heap[idx] : "";
            }
            if (is_int(v))
                return std::to_string(as_int(v));
            return "";
        };
        return make_bool(to_str(a[0]) == to_str(a[1]));
    });
    add("string<?", [&heaps](std::span<const EvalValue> a) {
        if (a.size() < 2)
            return make_bool(false);
        auto to_str = [&heaps](const EvalValue& v) -> std::string {
            if (is_string(v)) {
                auto idx = as_string_idx(v);
                return (idx < heaps.string_heap.size()) ? heaps.string_heap[idx] : "";
            }
            if (is_int(v))
                return std::to_string(as_int(v));
            return "";
        };
        return make_bool(to_str(a[0]) < to_str(a[1]));
    });
    add("string->number", [&heaps](std::span<const EvalValue> a) {
        if (a.size() < 1 || a.size() > 2)
            return make_int(0);
        auto to_str = [&heaps](const EvalValue& v) -> std::string {
            if (is_string(v)) {
                auto idx = as_string_idx(v);
                return (idx < heaps.string_heap.size()) ? heaps.string_heap[idx] : "";
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
            return make_bool(false);
        }
    });

    add("string-index", [&heaps](std::span<const EvalValue> a) {
        if (a.size() < 2)
            return make_int(-1);
        auto haystack = (is_string(a[0]) && as_string_idx(a[0]) < heaps.string_heap.size())
                            ? heaps.string_heap[as_string_idx(a[0])]
                            : "";
        auto needle = (is_string(a[1]) && as_string_idx(a[1]) < heaps.string_heap.size())
                          ? heaps.string_heap[as_string_idx(a[1])]
                          : "";
        auto start = (a.size() > 2 && is_int(a[2])) ? static_cast<std::size_t>(as_int(a[2])) : 0;
        if (needle.empty())
            return make_int(0);
        auto pos = haystack.find(needle, start);
        return make_int(pos != std::string::npos ? static_cast<std::int64_t>(pos) : -1);
    });

    add("number->string", [&heaps](std::span<const EvalValue> a) {
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
        auto id = heaps.string_heap.size();
        heaps.string_heap.push_back(std::move(s));
        return make_string(id);
    });
    add("string->number", [&heaps](std::span<const EvalValue> a) {
        if (a.empty() || !is_string(a[0]))
            return make_bool(false);
        auto i = as_string_idx(a[0]);
        if (i >= heaps.string_heap.size())
            return make_bool(false);
        auto& str = heaps.string_heap[i];
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

}  // namespace aura::compiler::primitives_detail
