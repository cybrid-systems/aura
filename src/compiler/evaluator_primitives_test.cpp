// evaluator_primitives_test.cpp — P0 step 27: run-tests primitive
// aura.compiler.evaluator module partition; registered via evaluator_primitives_registry.cpp.

module;

#include "runtime_shared.h"
#include "primitives_detail.h"
#include "observability_metrics.h"

module aura.compiler.evaluator;

import std;
import aura.core.ast;
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

void register_test_primitives(PrimRegistrar add, Evaluator& ev) {

    // Issue #751: (primitives:contract-probe has-counter? uses-guard?) —
    // runtime capture-contract probe for CI/tests; records a violation
    // when has-counter? is #f on a mutate-style path.
    add("primitives:contract-probe", [&ev](const auto& a) -> EvalValue {
        const bool has_counter = !a.empty() && is_bool(a[0]) && as_bool(a[0]);
        const bool uses_guard = a.size() >= 2 && is_bool(a[1]) && as_bool(a[1]);
        if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics()))
            primitives_detail::prim_check_capture_contract(m, has_counter, uses_guard);
        return make_bool(has_counter && uses_guard);
    });

    add("run-tests", [&ev](const auto&) -> EvalValue {
        auto& bindings = ev.top_.bindings();
        int total_suites = 0, total_passed = 0, total_failed = 0;

        for (auto& [name, val] : bindings) {
            if (name.size() <= 5 || name.substr(0, 5) != "test:")
                continue;

            // Dereference cell if needed (define stores via cell)
            auto actual = val;
            if (is_cell(val) && as_cell_id(val) < ev.cells_.size())
                actual = ev.cells_[as_cell_id(val)];
            if (!is_pair(actual))
                continue;

            total_suites++;
            auto idx = as_pair_idx(actual);
            if (idx >= ev.pairs_.size()) {
                total_failed++;
                continue;
            }

            // Suite name
            std::string suite_name;
            if (is_string(ev.pairs_[idx].car)) {
                auto sid = as_string_idx(ev.pairs_[idx].car);
                if (sid < ev.string_heap_.size())
                    suite_name = ev.string_heap_[sid];
            }

            // Walk and evaluate each stored check form
            auto forms = ev.pairs_[idx].cdr;
            int sp = 0, sf = 0;

            while (is_pair(forms)) {
                auto fi = as_pair_idx(forms);
                if (fi >= ev.pairs_.size())
                    break;
                auto check_form = ev.pairs_[fi].car;
                forms = ev.pairs_[fi].cdr;

                // Convert check_form data back to AST and evaluate.
                // Use ev.temp_arena_ so (gc-temp) reclaims parse state after
                // each check clause (was: ev.arena_ = monotonic = leaked).
                if (!ev.arena_) {
                    sf++;
                    continue;
                }
                auto alloc = ev.temp_arena_->allocator();
                auto* cf_pool = ev.temp_arena_->create<aura::ast::StringPool>(alloc);
                auto* cf_flat = ev.temp_arena_->create<aura::ast::FlatAST>(alloc);
                auto ast_root = ev.data_to_flat(check_form, *cf_flat, *cf_pool, 0);
                if (ast_root == aura::ast::NULL_NODE) {
                    sf++;
                    continue;
                }
                cf_flat->root = ast_root;

                auto result = ev.eval_flat(*cf_flat, *cf_pool, ast_root, ev.top_);
                if (!result) {
                    sf++;
                    continue;
                }

                bool ok = is_int(*result) && as_int(*result) == 1;
                if (ok) {
                    sp++;
                    total_passed++;
                } else {
                    sf++;
                    total_failed++;
                }
            }

            std::fprintf(stderr, "  Suite '%s': %d/%d passed\n", suite_name.c_str(), sp, sp + sf);
        }

        std::string summary = std::to_string(total_suites) +
                              " suites: " + std::to_string(total_passed) + " passed, " +
                              std::to_string(total_failed) + " failed";
        auto sidx = ev.string_heap_.size();
        ev.string_heap_.push_back(summary);
        return make_string(sidx);
    });
}

} // namespace aura::compiler::primitives_detail
