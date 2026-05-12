module;

#include <string>
#include <vector>
#include <cstdint>
#include <unordered_map>

export module aura.compiler.type_checker;

import std;
import aura.core.ast;
import aura.core.type;
import aura.diag;

namespace aura::compiler {

// ── Type Environment ─────────────────────────────────────
export class TypeEnv {
    aura::core::TypeRegistry& reg_;
    struct Binding {
        aura::core::TypeId type;
        bool is_poly = false;
        std::vector<aura::core::TypeId> type_args;
    };
    std::vector<std::unordered_map<std::string, Binding>> scopes_;
public:
    explicit TypeEnv(aura::core::TypeRegistry& reg);
    void push_scope();
    void pop_scope();
    void bind(std::string name, aura::core::TypeId type);
    aura::core::TypeId lookup(const std::string& name);
    bool is_bound(const std::string& name) const;
};

// ── Constraint System ────────────────────────────────────
export struct Constraint {
    enum Kind { EQUAL, CONSISTENT };
    Kind kind;
    aura::core::TypeId lhs, rhs;
};

export class ConstraintSystem {
    aura::core::TypeRegistry& reg_;
    std::vector<Constraint> constraints_;
    std::vector<aura::core::TypeId> subst_;
    uint64_t fresh_counter_ = 0;
public:
    explicit ConstraintSystem(aura::core::TypeRegistry& reg);
    void add(Constraint c);
    bool solve();
    void clear();
    aura::core::TypeId fresh_var();
    bool consistent_unify(aura::core::TypeId t1, aura::core::TypeId t2);
    bool occurs_check(aura::core::TypeId var, aura::core::TypeId ty);
    aura::core::TypeId normalize(aura::core::TypeId id);
};

// ── Inference Engine ─────────────────────────────────────
export class InferenceEngine {
    aura::core::TypeRegistry& reg_;
    aura::diag::DiagnosticCollector& diag_;
    ConstraintSystem cs_;
    TypeEnv env_;
public:
    InferenceEngine(aura::core::TypeRegistry& reg, aura::diag::DiagnosticCollector& diag);

    // Top-level entry
    aura::core::TypeId infer(aura::ast::Expr* e);

    // Synthesis (top-down)
    aura::core::TypeId synthesize(const aura::ast::Expr& e);

    // Checking (bottom-up with expected type)
    void check(const aura::ast::Expr& e, aura::core::TypeId expected);

private:
    // Per-node-type inference
    aura::core::TypeId synthesize_var(const aura::ast::VariableNode& n);
    aura::core::TypeId synthesize_call(const aura::ast::CallNode& n);
    aura::core::TypeId synthesize_lambda(const aura::ast::LambdaNode& n);
    aura::core::TypeId synthesize_if(const aura::ast::IfExprNode& n);
    aura::core::TypeId synthesize_let(const aura::ast::LetNode& n);
    aura::core::TypeId synthesize_letrec(const aura::ast::LetRecNode& n);
    aura::core::TypeId synthesize_begin(const aura::ast::BeginNode& n);
    aura::core::TypeId synthesize_annotation(const aura::ast::TypeAnnotationNode& n);

    void check_call(const aura::ast::CallNode& n, aura::core::TypeId expected);
    void check_lambda(const aura::ast::LambdaNode& n, aura::core::TypeId expected);

    aura::core::TypeId lub(aura::core::TypeId a, aura::core::TypeId b);
};

// ── TypeChecker — Public API ─────────────────────────────
export struct TypeChecker {
    aura::core::TypeRegistry& types;
    explicit TypeChecker(aura::core::TypeRegistry& reg) : types(reg) {}
    aura::core::TypeId infer(aura::ast::Expr* expr, aura::diag::DiagnosticCollector& diag);
};

} // namespace aura::compiler
