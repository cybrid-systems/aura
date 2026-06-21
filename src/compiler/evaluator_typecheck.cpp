// evaluator_typecheck.cpp — P1-j: inline typecheck helpers
// extracted from evaluator_impl.cpp.

module;

#include <string>
#include <unordered_map>

module aura.compiler.evaluator;

import std;
import aura.core.ast;
import aura.core.type;
import aura.compiler.type_checker;
import aura.compiler.coercion_map;
import aura.compiler.value;
import aura.diag;

namespace aura::compiler {

using types::EvalValue;
using namespace types;
using namespace aura::diag;


// Issue #107 part 4: inline typecheck helpers. Caller MUST hold
// workspace_mtx_ (shared or unique). The two helpers share the
// same infer_flat + diag-drain pattern; they differ only in
// return type — string for sites that need the error message,
// bool for sites that only need pass/fail. Both are members
// of Evaluator (so they can access the privates below).
std::string Evaluator::run_typecheck_no_lock() {
    if (!workspace_flat_ || !workspace_pool_)
        return std::string("no workspace");
    if (!type_registry_) {
        type_registry_ = new aura::core::TypeRegistry();
    }
    auto& treg = *static_cast<aura::core::TypeRegistry*>(type_registry_);
    aura::compiler::TypeChecker tc(treg);
    if (!declared_type_sigs_.empty()) {
        std::unordered_map<std::string, std::string> sig_map;
        std::unordered_map<std::string, std::string> mod_src_map;
        for (auto& [name, decl] : declared_type_sigs_) {
            sig_map[name] = decl.type_str;
            if (!decl.module_file.empty())
                mod_src_map[name] = decl.module_file;
        }
        tc.inject_type_sigs(sig_map, mod_src_map);
    }
    aura::diag::DiagnosticCollector diag;
    auto result = tc.infer_flat(*workspace_flat_, *workspace_pool_, workspace_flat_->root, diag);
    workspace_flat_->clear_all_dirty();
    std::string out = "type: " + treg.format_type(result) + "\n";
    auto all_diags = diag.diagnostics();
    if (all_diags.empty()) {
        out += "no errors\n";
    } else {
        out += "diagnostics:\n";
        for (auto& d : all_diags) {
            out += "  [" + std::to_string(static_cast<int>(d.kind)) + "] " + d.format() + "\n";
        }
    }
    return out;
}

bool Evaluator::run_typecheck_no_lock_bool() {
    // Same as the string version but returns pass/fail directly
    // without formatting. Cheaper for hot fuzzer loops.
    //
    // Issue #116: this is called from the fuzzy/evolutionary loop
    // (compute_fitness), which then `eval`s the workspace. The
    // workspace must be lowering-ready, so we apply the deferred
    // CoercionMap before returning.
    if (!workspace_flat_ || !workspace_pool_)
        return true;
    if (!type_registry_) {
        type_registry_ = new aura::core::TypeRegistry();
    }
    auto& treg = *static_cast<aura::core::TypeRegistry*>(type_registry_);
    aura::compiler::TypeChecker tc(treg);
    if (!declared_type_sigs_.empty()) {
        std::unordered_map<std::string, std::string> sig_map;
        std::unordered_map<std::string, std::string> mod_src_map;
        for (auto& [name, decl] : declared_type_sigs_) {
            sig_map[name] = decl.type_str;
            if (!decl.module_file.empty())
                mod_src_map[name] = decl.module_file;
        }
        tc.inject_type_sigs(sig_map, mod_src_map);
    }
    aura::diag::DiagnosticCollector diag;
    tc.infer_flat(*workspace_flat_, *workspace_pool_, workspace_flat_->root, diag);
    // Issue #116: apply deferred coercions — the caller (fuzzer
    // loop) will then `eval` the workspace via compute_fitness,
    // which needs CoercionNodes present for the IR generator.
    {
        auto cm = tc.take_coercions();
        if (!cm.empty()) {
            aura::compiler::apply_coercion_map(*workspace_flat_, cm);
        }
    }
    workspace_flat_->clear_all_dirty();
    return diag.diagnostics().empty();
}

} // namespace aura::compiler
