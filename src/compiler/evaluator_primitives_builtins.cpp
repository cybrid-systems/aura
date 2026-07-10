// evaluator_primitives_builtins.cpp — P1-k: Primitives ctor (arithmetic/boolean builtins)
// aura.compiler.evaluator module partition.

module;


module aura.compiler.evaluator;

import std;
import aura.compiler.evaluator_pure;
import aura.compiler.value;

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

    std::int64_t coerce_to_int(const EvalValue& v, std::span<const std::string> heap) {
        auto r = aura::compiler::pure::coerce_to_int_pure(v, heap);
        if (r)
            return *r;
        if (v.val != 3 && is_string(v) && !heap.empty()) {
            auto idx = as_string_idx(v);
            if (idx < heap.size()) {
                std::println(std::cerr, "error: type mismatch — expected Int, got String '{}'",
                             heap[idx]);
            }
        }
        return 0;
    }

} // namespace

Primitives::Primitives() {
    // ── Variadic arithmetic ────────────────────────────────────────
    // (+) → 0, (+ x) → x, (+ x y ...) → sum; float if any arg is float
    // Issue #146 (7th extract): the body is now a 1-line
    // forwarder to aura::compiler::pure::arithmetic_sum_pure.
    // The diag sink is std::cerr to preserve the legacy "type
    // mismatch" stderr emission that the stateful coerce_to_int
    // wrapper used to provide — regression tests
    // tc-strict-runtime-typed-arg-mismatch and
    // tc-type-var-name-from-param depend on this.
    table_["+"] = [this](std::span<const EvalValue> a) {
        std::span<const std::string> heap(string_heap_->data(), string_heap_->size());
        return aura::compiler::pure::arithmetic_sum_pure(a, heap, &std::cerr);
    };
    // (-) → 0, (- x) → -x, (- x y ...) → x - y - z - ...
    // Issue #212 Phase 3: thin forwarder to arithmetic_sub_pure.
    table_["-"] = [this](std::span<const EvalValue> a) {
        return aura::compiler::pure::arithmetic_sub_pure(a, *string_heap_, &std::cerr);
    };
    // (*) → 1, (* x) → x, (* x y ...) → product; float if any arg is float
    // Issue #212 Phase 3: thin forwarder to arithmetic_mul_pure.
    table_["*"] = [this](std::span<const EvalValue> a) {
        return aura::compiler::pure::arithmetic_mul_pure(a, *string_heap_, &std::cerr);
    };
    // (/) → 1, (/ x) → 1.0/x (float reciprocal), (/ x y ...) → x / y / z / ...
    // Issue #212 Phase 3: thin forwarder to arithmetic_div_pure.
    // On div-by-zero the pure function returns an unexpected Diagnostic;
    // we mirror the legacy behavior (silent 0 result for non-empty divisor
    // lists, or 0 for the reciprocal case) so the existing test surface
    // doesn't change. A future migration can surface the error.
    table_["/"] = [this](std::span<const EvalValue> a) {
        auto r = aura::compiler::pure::arithmetic_div_pure(a, *string_heap_, &std::cerr);
        if (r)
            return *r;
        // Error path: legacy behavior. For (/) empty → 1 (but
        // the pure function already errors on empty, so legacy
        // returned 1; we mirror that). For other errors (div by
        // zero, type mismatch), legacy returned 0; mirror that.
        if (a.empty())
            return make_int(1);
        return make_int(0);
    };
    auto chain_cmp = [this](const auto& a, auto fn_int, auto fn_float) -> EvalValue {
        if (a.size() < 2)
            return make_bool(true);
        auto to_f = [this](const EvalValue& v) -> double {
            return is_float(v) ? as_float(v) : static_cast<double>(coerce_to_int(v, *string_heap_));
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
            if (!fn_int(coerce_to_int(a[i - 1], *string_heap_), coerce_to_int(a[i], *string_heap_)))
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
    table_["eq?"] = [](std::span<const EvalValue> a) {
        return make_bool(a.size() >= 2 && a[0] == a[1]);
    };
    table_["current-time"] = [](std::span<const EvalValue> a) {
        (void)a;
        return make_int(static_cast<std::int64_t>(std::time(nullptr)));
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
    set_meta("+", {.arity = 255, .pure = true, .doc = "Variadic addition."});
    set_meta("-", {.arity = 255, .pure = true, .doc = "Variadic subtraction."});
    set_meta("not", {.arity = 1, .pure = true, .doc = "Boolean negation."});
}
std::optional<PrimFn> Primitives::lookup(std::string_view n) const {
    // Issue #914: transparent hash — no temporary std::string.
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
