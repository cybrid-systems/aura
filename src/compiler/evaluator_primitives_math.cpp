// evaluator_primitives_math.cpp — P0 step 6: m4-linear, regex, math, arithmetic
// aura.compiler.evaluator module partition; registered via evaluator_primitives_registry.cpp.

module;
#include <span>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <functional>
#include <regex>
#include <string>
#include "runtime_shared.h"

module aura.compiler.evaluator;

import std;
import aura.compiler.value;
import aura.compiler.evaluator_pure;

namespace aura::compiler::primitives_detail {

using EvalValue = types::EvalValue;
using PrimRegistrar = std::function<void(std::string, PrimFn)>;

using namespace types;

namespace {

std::int64_t coerce_to_int(const EvalValue& v, std::span<const std::string> heap) {
    auto r = aura::compiler::pure::coerce_to_int_pure(v, heap);
    if (r)
        return *r;
    return 0;
}

} // namespace

void register_math_regex_and_arithmetic_primitives(
    PrimRegistrar add, std::pmr::vector<Pair>& pairs, std::pmr::vector<std::string>& string_heap,
    std::vector<EvalValue>& error_values, std::atomic<std::uint64_t>* primitive_error_counter) {

    // ── M4 linear-type primitives (Issue #108 part 3) ─────────────
    add("m4-move", [](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty())
            return make_void();
        return a[0];
    });
    add("m4-borrow", [](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty())
            return make_void();
        return a[0];
    });
    add("m4-return!", [](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        return make_void();
    });
    add("define-linear", [](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        return make_void();
    });

    // ── Regex ──────────────────────────────────────────────────
    add("regex-match?", [&string_heap](std::span<const EvalValue> a) {
        if (a.size() < 2 || !is_string(a[0]) || !is_string(a[1]))
            return make_int(0);
        auto pi = as_string_idx(a[0]), si = as_string_idx(a[1]);
        if (pi >= string_heap.size() || si >= string_heap.size())
            return make_int(0);
        try {
            std::regex re(string_heap[pi]);
            return make_int(std::regex_search(string_heap[si], re) ? 1 : 0);
        } catch (...) {
            return make_int(0);
        }
    });

    add("regex-find", [&string_heap](std::span<const EvalValue> a) {
        if (a.size() < 2 || !is_string(a[0]) || !is_string(a[1]))
            return make_void();
        auto pi = as_string_idx(a[0]), si = as_string_idx(a[1]);
        if (pi >= string_heap.size() || si >= string_heap.size())
            return make_void();
        try {
            std::regex re(string_heap[pi]);
            std::smatch m;
            if (std::regex_search(string_heap[si], m, re)) {
                auto id = string_heap.size();
                string_heap.push_back(m.str());
                return make_string(id);
            }
        } catch (...) {
        }
        return make_void();
    });

    add("regex-replace", [&string_heap](std::span<const EvalValue> a) {
        if (a.size() < 3 || !is_string(a[0]) || !is_string(a[1]) || !is_string(a[2]))
            return make_void();
        auto pi = as_string_idx(a[0]), si = as_string_idx(a[1]), ri = as_string_idx(a[2]);
        if (pi >= string_heap.size() || si >= string_heap.size() || ri >= string_heap.size())
            return make_void();
        try {
            std::regex re(string_heap[pi]);
            auto result = std::regex_replace(string_heap[si], re, string_heap[ri]);
            auto id = string_heap.size();
            string_heap.push_back(std::move(result));
            return make_string(id);
        } catch (...) {
            return make_void();
        }
    });

    add("regex-split", [&string_heap, &pairs](std::span<const EvalValue> a) {
        if (a.size() < 2 || !is_string(a[0]) || !is_string(a[1]))
            return make_void();
        auto pi = as_string_idx(a[0]), si = as_string_idx(a[1]);
        if (pi >= string_heap.size() || si >= string_heap.size())
            return make_void();
        try {
            std::regex re(string_heap[pi]);
            std::sregex_token_iterator it(string_heap[si].begin(), string_heap[si].end(), re, -1);
            std::sregex_token_iterator end;
            EvalValue result = make_void();
            std::vector<std::string> parts;
            for (; it != end; ++it)
                parts.push_back(it->str());
            for (auto it2 = parts.rbegin(); it2 != parts.rend(); ++it2) {
                auto sid = string_heap.size();
                string_heap.push_back(*it2);
                auto pid = pairs.size();
                pairs.push_back({make_string(sid), result});
                result = make_pair(pid);
            }
            return result;
        } catch (...) {
            return make_void();
        }
    });

    // ── Math ────────────────────────────────────────────────────
    auto to_double = [](const EvalValue& v) -> double {
        if (is_float(v))
            return as_float(v);
        if (is_int(v))
            return static_cast<double>(as_int(v));
        return 0.0;
    };

    add("sin", [to_double](const auto& a) {
        if (a.empty())
            return make_float(0.0);
        return make_float(std::sin(to_double(a[0])));
    });
    add("cos", [to_double](const auto& a) {
        if (a.empty())
            return make_float(0.0);
        return make_float(std::cos(to_double(a[0])));
    });
    add("tan", [to_double](const auto& a) {
        if (a.empty())
            return make_float(0.0);
        return make_float(std::tan(to_double(a[0])));
    });
    add("asin", [to_double](const auto& a) {
        if (a.empty())
            return make_float(0.0);
        return make_float(std::asin(to_double(a[0])));
    });
    add("acos", [to_double](const auto& a) {
        if (a.empty())
            return make_float(0.0);
        return make_float(std::acos(to_double(a[0])));
    });
    add("atan", [to_double](const auto& a) {
        if (a.empty())
            return make_float(0.0);
        return make_float(std::atan(to_double(a[0])));
    });
    add("log", [to_double](const auto& a) {
        if (a.empty())
            return make_float(0.0);
        return make_float(std::log(to_double(a[0])));
    });
    add("log10", [to_double](const auto& a) {
        if (a.empty())
            return make_float(0.0);
        return make_float(std::log10(to_double(a[0])));
    });
    add("exp", [to_double](const auto& a) {
        if (a.empty())
            return make_float(0.0);
        return make_float(std::exp(to_double(a[0])));
    });
    add("pow", [to_double](const auto& a) {
        if (a.size() < 2)
            return make_float(0.0);
        return make_float(std::pow(to_double(a[0]), to_double(a[1])));
    });
    add("sqrt", [to_double](const auto& a) {
        if (a.empty())
            return make_float(0.0);
        return make_float(std::sqrt(to_double(a[0])));
    });
    add("floor", [to_double](const auto& a) {
        if (a.empty())
            return make_float(0.0);
        return make_float(std::floor(to_double(a[0])));
    });
    add("ceil", [to_double](const auto& a) {
        if (a.empty())
            return make_float(0.0);
        return make_float(std::ceil(to_double(a[0])));
    });
    add("round", [to_double](const auto& a) {
        if (a.empty())
            return make_float(0.0);
        return make_float(std::round(to_double(a[0])));
    });

    add("inexact->exact", [](const auto& a) -> EvalValue {
        if (a.empty())
            return make_int(0);
        if (types::is_float(a[0]))
            return types::make_int(static_cast<std::int64_t>(types::as_float(a[0])));
        return a[0];
    });

    // ── Arithmetic extensions ─────────────────────────────────────
    add("modulo", [&string_heap, &error_values, primitive_error_counter](std::span<const EvalValue> a) {
        if (a.size() < 2)
            return make_int(0);
        auto divisor = coerce_to_int(a[1], string_heap);
        if (divisor == 0) {
            return make_primitive_error(string_heap, error_values, "modulo: division by zero",
                                      primitive_error_counter);
        }
        auto n = coerce_to_int(a[0], string_heap);
        auto r = n % divisor;
        if (r < 0)
            r += (divisor > 0 ? divisor : -divisor);
        return make_int(r);
    });
    add("mod", [&string_heap, &error_values, primitive_error_counter](std::span<const EvalValue> a) {
        if (a.size() < 2)
            return make_int(0);
        auto divisor = coerce_to_int(a[1], string_heap);
        if (divisor == 0) {
            return make_primitive_error(string_heap, error_values, "mod: division by zero",
                                      primitive_error_counter);
        }
        auto n = coerce_to_int(a[0], string_heap);
        auto r = n % divisor;
        if (r < 0)
            r += (divisor > 0 ? divisor : -divisor);
        return make_int(r);
    });
    add("quotient", [&string_heap, &error_values, primitive_error_counter](std::span<const EvalValue> a) {
        if (a.size() < 2)
            return make_int(0);
        auto divisor = coerce_to_int(a[1], string_heap);
        if (divisor == 0) {
            return make_primitive_error(string_heap, error_values, "quotient: division by zero",
                                      primitive_error_counter);
        }
        return make_int(coerce_to_int(a[0], string_heap) / divisor);
    });
    add("remainder", [&string_heap, &error_values, primitive_error_counter](std::span<const EvalValue> a) {
        if (a.size() < 2)
            return make_int(0);
        auto divisor = coerce_to_int(a[1], string_heap);
        if (divisor == 0) {
            return make_primitive_error(string_heap, error_values, "remainder: division by zero",
                                      primitive_error_counter);
        }
        return make_int(coerce_to_int(a[0], string_heap) % divisor);
    });
    add("abs", [&string_heap](std::span<const EvalValue> a) {
        if (a.empty())
            return make_int(0);
        if (is_float(a[0]))
            return make_float(std::abs(as_float(a[0])));
        auto n = coerce_to_int(a[0], string_heap);
        return make_int(n < 0 ? -n : n);
    });
    add("gcd", [&string_heap](std::span<const EvalValue> a) {
        if (a.empty())
            return make_int(0);
        auto to_int = [&](const EvalValue& v) { return coerce_to_int(v, string_heap); };
        auto r = to_int(a[0]);
        auto abs_gcd = [](std::int64_t x, std::int64_t y) -> std::int64_t {
            x = x < 0 ? -x : x;
            y = y < 0 ? -y : y;
            while (y != 0) {
                auto t = y;
                y = x % y;
                x = t;
            }
            return x;
        };
        for (std::size_t i = 1; i < a.size(); ++i)
            r = abs_gcd(r, to_int(a[i]));
        return make_int(r);
    });
    add("lcm", [&string_heap](std::span<const EvalValue> a) {
        if (a.empty())
            return make_int(1);
        auto to_int = [&](const EvalValue& v) { return coerce_to_int(v, string_heap); };
        auto r = to_int(a[0]);
        auto gcd = [](std::int64_t x, std::int64_t y) -> std::int64_t {
            x = x < 0 ? -x : x;
            y = y < 0 ? -y : y;
            if (x == 0 || y == 0)
                return 0;
            while (y != 0) {
                auto t = y;
                y = x % y;
                x = t;
            }
            return x;
        };
        for (std::size_t i = 1; i < a.size(); ++i) {
            auto n = to_int(a[i]);
            auto g = gcd(r, n);
            r = (g == 0) ? 0 : (r / g) * n;
        }
        if (r < 0)
            r = -r;
        return make_int(r);
    });
    add("min", [&string_heap](std::span<const EvalValue> a) {
        if (a.empty())
            return make_int(0);
        bool any_f = false;
        for (auto& v : a)
            if (is_float(v)) {
                any_f = true;
                break;
            }
        if (any_f) {
            auto to_f = [&](const EvalValue& v) {
                return is_float(v) ? as_float(v)
                                   : static_cast<double>(coerce_to_int(v, string_heap));
            };
            double r = to_f(a[0]);
            for (std::size_t i = 1; i < a.size(); ++i)
                r = std::min(r, to_f(a[i]));
            return make_float(r);
        }
        std::int64_t r = coerce_to_int(a[0], string_heap);
        for (std::size_t i = 1; i < a.size(); ++i)
            r = std::min(r, coerce_to_int(a[i], string_heap));
        return make_int(r);
    });
    add("max", [&string_heap](std::span<const EvalValue> a) {
        if (a.empty())
            return make_int(0);
        bool any_f = false;
        for (auto& v : a)
            if (is_float(v)) {
                any_f = true;
                break;
            }
        if (any_f) {
            auto to_f = [&](const EvalValue& v) {
                return is_float(v) ? as_float(v)
                                   : static_cast<double>(coerce_to_int(v, string_heap));
            };
            double r = to_f(a[0]);
            for (std::size_t i = 1; i < a.size(); ++i)
                r = std::max(r, to_f(a[i]));
            return make_float(r);
        }
        std::int64_t r = coerce_to_int(a[0], string_heap);
        for (std::size_t i = 1; i < a.size(); ++i)
            r = std::max(r, coerce_to_int(a[i], string_heap));
        return make_int(r);
    });
}

} // namespace aura::compiler::primitives_detail