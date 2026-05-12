module;

#include <string>
#include <vector>
#include <cstdint>

export module aura.compiler.type_checker;

import std;
import aura.core.ast_flat;
import aura.core.type;
import aura.core;
import aura.diag;
import aura.core.arena;

namespace aura::compiler {

// ── TypeChecker Pass ──────────────────────────────────────────
// Lowers TypeAnnotationNodes from ParsedPhase → TypedPhase
// by verifying inner expression type matches annotation.
// Currently: strips annotations (L6.2 behavior)
// L6.3+: performs type checking on annotated expressions

// ── DiagnosticCollector — collects diagnostics during compilation ─
export class DiagnosticCollector {
public:
    void report(aura::diag::Diagnostic d) {
        diagnostics_.push_back(std::move(d));
    }
    
    bool has_errors() const {
        return !diagnostics_.empty();
    }
    
    std::span<const aura::diag::Diagnostic> diagnostics() const {
        return diagnostics_;
    }
    
    void clear() {
        diagnostics_.clear();
    }

private:
    std::vector<aura::diag::Diagnostic> diagnostics_;
};

export struct TypeChecker {
    core::TypeRegistry& types;
    DiagnosticCollector& diag;
    
    explicit TypeChecker(core::TypeRegistry& reg, DiagnosticCollector& d)
        : types(reg), diag(d) {}
    
    // Run type checker on FlatAST
    void run(aura::ast::FlatAST& flat);
    
    // Check a single expression against expected type
    // Returns true if type is consistent
    bool check_type(aura::ast::FlatAST& flat, aura::ast::NodeId node, core::TypeId expected);
    
    // Infer type of a FlatAST node
    core::TypeId infer_type(aura::ast::FlatAST& flat, aura::ast::NodeId node);
};

} // namespace aura::compiler
