// evaluator_primitives_builtins.cpp — P1-k: Primitives ctor (arithmetic/boolean builtins)
// aura.compiler.evaluator module partition.

module;

#include "core/arena_auto_policy_stats.h"
#include "primitives_meta.h"

#include <chrono>
#include <ctime>

module aura.compiler.evaluator;

import std;
import aura.compiler.evaluator_pure;
import aura.compiler.value;
import aura.diag;

namespace aura::compiler {

using aura::compiler::pure::is_truthy;
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

namespace {

    std::string mismatch_detail(const EvalValue& v, const std::pmr::vector<std::string>* heap) {
        std::string_view text;
        if (is_string(v) && heap != nullptr) {
            const auto idx = as_string_idx(v);
            if (idx < heap->size())
                text = (*heap)[idx];
        }
        return aura::compiler::pure::numeric_type_mismatch_detail(v, text);
    }

} // namespace

Primitives::Primitives() {
    // Issue #4463: non-number operand. Stderr keeps the legacy line.
    // The value is a primitive error, not a coerced int.
    auto type_error = [this](std::string detail) -> EvalValue {
        std::println(std::cerr, "error: {}", detail);
        if (error_values_ != nullptr && string_heap_ != nullptr) {
            return primitives_detail::make_primitive_error(
                *string_heap_, *error_values_, std::move(detail), primitive_error_counter_);
        }
        return make_error(0);
    };
    auto reject_non_number =
        [this, type_error](std::span<const EvalValue> a) -> std::optional<EvalValue> {
        if (const auto* bad = aura::compiler::pure::first_non_number(a))
            return type_error(mismatch_detail(*bad, string_heap_));
        return std::nullopt;
    };
    // ── Variadic arithmetic ────────────────────────────────────────
    // (+) → 0, (+ x) → x, (+ x y ...) → sum; float if any arg is float
    // Issue #146: forwarder to arithmetic_sum_pure. Issue #4463: a
    // non-number prints the type-mismatch line and returns an error
    // value (tc-strict-runtime-typed-arg-mismatch still matches stderr).
    table_["+"] = [this, reject_non_number](std::span<const EvalValue> a) {
        if (auto err = reject_non_number(a))
            return *err;
        std::span<const std::string> heap(string_heap_->data(), string_heap_->size());
        auto v = aura::compiler::pure::arithmetic_sum_pure(a, heap, nullptr);
        if (is_error(v))
            return make_error(0);
        return v;
    };
    // (-) → 0, (- x) → -x, (- x y ...) → x - y - z - ...
    table_["-"] = [this, reject_non_number](std::span<const EvalValue> a) {
        if (auto err = reject_non_number(a))
            return *err;
        auto v = aura::compiler::pure::arithmetic_sub_pure(a, *string_heap_, nullptr);
        if (is_error(v))
            return make_error(0);
        return v;
    };
    // (*) → 1, (* x) → x, (* x y ...) → product; float if any arg is float
    table_["*"] = [this, reject_non_number](std::span<const EvalValue> a) {
        if (auto err = reject_non_number(a))
            return *err;
        auto v = aura::compiler::pure::arithmetic_mul_pure(a, *string_heap_, nullptr);
        if (is_error(v))
            return make_error(0);
        return v;
    };
    // (/) → 1, (/ x) → 1/x, (/ x y ...) → x / y / z / ...
    // Issue #4461: integer division by zero is an error value. Float
    // zero stays IEEE. Empty (/) stays 1. Int64 overflow stays 0.
    // Issue #4463: a non-number is a type error, not a coerced divisor.
    table_["/"] = [this, reject_non_number](std::span<const EvalValue> a) {
        if (auto err = reject_non_number(a))
            return *err;
        auto r = aura::compiler::pure::arithmetic_div_pure(a, *string_heap_, nullptr);
        if (r)
            return *r;
        if (a.empty())
            return make_int(1);
        if (r.error().kind == aura::diag::ErrorKind::DivisionByZero) {
            if (error_values_ != nullptr && string_heap_ != nullptr) {
                return primitives_detail::make_primitive_error(
                    *string_heap_, *error_values_, "/: division by zero", primitive_error_counter_);
            }
            return make_error(0);
        }
        if (r.error().message.find("type mismatch") != std::string::npos) {
            if (error_values_ != nullptr && string_heap_ != nullptr) {
                return primitives_detail::make_primitive_error(
                    *string_heap_, *error_values_, r.error().message, primitive_error_counter_);
            }
            return make_error(0);
        }
        return make_int(0);
    };
    auto chain_cmp = [this, type_error](const auto& a, auto fn_int, auto fn_float) -> EvalValue {
        if (a.size() < 2)
            return make_bool(true);
        if (const auto* bad = aura::compiler::pure::first_non_number(a))
            return type_error(mismatch_detail(*bad, string_heap_));
        auto to_f = [](const EvalValue& v) -> double {
            return is_float(v) ? as_float(v) : static_cast<double>(as_int(v));
        };
        bool any_f = false;
        for (auto& v : a)
            if (is_float(v)) {
                any_f = true;
                break;
            }
        if (any_f) {
            for (std::size_t i = 1; i < a.size(); ++i)
                if (!fn_float(to_f(a[i - 1]), to_f(a[i])))
                    return make_bool(false);
            return make_bool(true);
        }
        for (std::size_t i = 1; i < a.size(); ++i)
            if (!fn_int(as_int(a[i - 1]), as_int(a[i])))
                return make_bool(false);
        return make_bool(true);
    };
    table_["="] = [chain_cmp](std::span<const EvalValue> a) {
        return chain_cmp(
            a, [](auto x, auto y) { return x == y; }, [](auto x, auto y) { return x == y; });
    };
    table_["<"] = [chain_cmp](std::span<const EvalValue> a) {
        return chain_cmp(
            a, [](auto x, auto y) { return x < y; }, [](auto x, auto y) { return x < y; });
    };
    table_[">"] = [chain_cmp](std::span<const EvalValue> a) {
        return chain_cmp(
            a, [](auto x, auto y) { return x > y; }, [](auto x, auto y) { return x > y; });
    };
    table_["<="] = [chain_cmp](std::span<const EvalValue> a) {
        return chain_cmp(
            a, [](auto x, auto y) { return x <= y; }, [](auto x, auto y) { return x <= y; });
    };
    table_[">="] = [chain_cmp](std::span<const EvalValue> a) {
        return chain_cmp(
            a, [](auto x, auto y) { return x >= y; }, [](auto x, auto y) { return x >= y; });
    };
    // Ghuloum Step 9: booleans
    table_["not"] = [](std::span<const EvalValue> a) {
        return make_bool(a.empty() || !is_truthy(a[0]));
    };
    table_["and"] = [](std::span<const EvalValue> a) {
        for (std::size_t i = 0; i + 1 < a.size(); ++i)
            if (!is_truthy(a[i]))
                return a[i];
        return a.empty() ? make_int(1) : a.back();
    };
    table_["or"] = [](std::span<const EvalValue> a) {
        for (std::size_t i = 0; i + 1 < a.size(); ++i)
            if (is_truthy(a[i]))
                return a[i];
        return a.empty() ? make_int(0) : a.back();
    };
    // Issue #2568: bit-identity first; for strings (quoted symbols are
    // string-heap interned) fall back to index equality which is identity
    // after #2568 intern fix. Content equality for strings is equal?.
    table_["eq?"] = [](std::span<const EvalValue> a) {
        if (a.size() < 2)
            return make_bool(false);
        if (a[0] == a[1])
            return make_bool(true);
        // String/symbol: same heap index ⇒ same interned name (eq? #t).
        // Different indices with same content: equal? is #t, eq? stays #f
        // only if intern failed — after #2568 intern, same name ⇒ same idx.
        if (is_string(a[0]) && is_string(a[1]))
            return make_bool(as_string_idx(a[0]) == as_string_idx(a[1]));
        return make_bool(false);
    };
    table_["current-time"] = [](std::span<const EvalValue> a) {
        (void)a;
        return make_int(static_cast<std::int64_t>(std::time(nullptr)));
    };
    // Issue #2655: sub-second clocks (parity with register_misc_primitives).
    table_["current-time-ms"] = [](std::span<const EvalValue> a) {
        (void)a;
        using clock = std::chrono::system_clock;
        const auto ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(clock::now().time_since_epoch())
                .count();
        return make_int(static_cast<std::int64_t>(ms));
    };
    table_["monotonic-ms"] = [](std::span<const EvalValue> a) {
        (void)a;
        using clock = std::chrono::steady_clock;
        const auto ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(clock::now().time_since_epoch())
                .count();
        return make_int(static_cast<std::int64_t>(ms));
    };
    // Populate ordered_names_ + fn_slots_ + name_to_slot_ + default meta_
    // for ctor builtins (registered via table_[] not add()).
    // Issue: name_to_slot_ must be filled here — Env::lookup falls back
    // to slot_for_name for first-class primitives like (foldl + 0 …).
    // Without the reverse index, +/*/- etc. are unbound as values while
    // still working in call position (lookup uses table_ directly).
    for (auto& [name, fn] : table_) {
        if (std::find(ordered_names_.begin(), ordered_names_.end(), name) == ordered_names_.end()) {
            const std::size_t slot = ordered_names_.size();
            ordered_names_.push_back(name);
            fn_slots_.push_back(fn);
            meta_.push_back(PrimMeta{});
            name_to_slot_[ordered_names_.back()] = slot;
        }
    }
    auto set_meta = [this](const char* name, PrimMeta meta) {
        auto slot = slot_for_name(name);
        if (slot < meta_.size())
            meta_[slot] = std::move(meta);
    };
    set_meta("+",
             {.arity = 255, .pure = true, .perf_tier = kPrimPerfHot, .doc = "Variadic addition."});
    set_meta(
        "-",
        {.arity = 255, .pure = true, .perf_tier = kPrimPerfHot, .doc = "Variadic subtraction."});
    set_meta(
        "*",
        {.arity = 255, .pure = true, .perf_tier = kPrimPerfHot, .doc = "Variadic multiplication."});
    set_meta("/",
             {.arity = 255, .pure = true, .perf_tier = kPrimPerfHot, .doc = "Variadic division."});
    set_meta("not",
             {.arity = 1, .pure = true, .perf_tier = kPrimPerfHot, .doc = "Boolean negation."});
    set_meta("car", {.arity = 1, .pure = true, .perf_tier = kPrimPerfHot, .doc = "Pair car."});
    set_meta("cdr", {.arity = 1, .pure = true, .perf_tier = kPrimPerfHot, .doc = "Pair cdr."});
    set_meta("cons", {.arity = 2, .pure = true, .perf_tier = kPrimPerfHot, .doc = "Pair cons."});
}
std::optional<PrimFn> Primitives::lookup(std::string_view n) const {
    // Issue #914/#1356: HotTierTable first, then main table_ (transparent hash).
    if (auto hit = hot_map_.find(n); hit != hot_map_.end()) {
        hot_dispatch_hits_.fetch_add(1, std::memory_order_relaxed);
        if (aura::core::arena_policy::in_render_hotpath())
            hot_dispatch_hits_render_.fetch_add(1, std::memory_order_relaxed);
        return hit->second;
    }
    if (aura::core::arena_policy::in_render_hotpath())
        cold_dispatch_fallback_.fetch_add(1, std::memory_order_relaxed);
    auto i = table_.find(n);
    return i != table_.end() ? std::optional(i->second) : std::nullopt;
}
// Implemented inline in class for lookup_cstr; slot_for_name O(1) via reverse index.
std::size_t Primitives::slot_for_name(std::string_view name) const {
    // Issue #899/#914: O(1) via reverse index (populated in add()).
    auto it = name_to_slot_.find(name);
    if (it != name_to_slot_.end())
        return it->second;
    return std::numeric_limits<std::size_t>::max();
}

} // namespace aura::compiler
