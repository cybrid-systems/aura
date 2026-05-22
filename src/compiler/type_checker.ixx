module;
#include <cstdint>

export module aura.compiler.type_checker;

import std;
import aura.core;
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
    std::vector<std::int64_t> parent_;    // Union-Find parent (self=root, -1=uninitialized)
    std::vector<std::uint32_t> rank_;      // Union-Find rank (for union-by-rank)
    std::vector<aura::core::TypeId> binding_; // binding[rep] = concrete type for var rep
    uint64_t fresh_counter_ = 0;
    uint64_t first_free_var_ = 0;          // first var index that belongs to this CS
public:
    explicit ConstraintSystem(aura::core::TypeRegistry& reg);
    void add(Constraint c);
    bool solve();
    void clear();
    aura::core::TypeId fresh_var();
    // Union-Find core
    aura::core::TypeId find_var(aura::core::TypeId id);
    bool unify(aura::core::TypeId t1, aura::core::TypeId t2);
    aura::core::TypeId find(aura::core::TypeId id);  // normalize via Union-Find
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
    aura::diag::SourceLocation cur_loc_;  // location of expression being checked
public:
    InferenceEngine(aura::core::TypeRegistry& reg, aura::diag::DiagnosticCollector& diag);

    // FlatAST inference entries
    aura::core::TypeId infer_flat(aura::ast::FlatAST& flat,
                                   aura::ast::StringPool& pool,
                                   aura::ast::NodeId node);
    void check_flat(aura::ast::FlatAST& flat,
                    aura::ast::StringPool& pool,
                    aura::ast::NodeId id,
                    aura::core::TypeId expected);

    // Initialize environment with primitive type signatures
    void init_primitive_env();

private:
    // FlatAST per-node-type inference
    aura::core::TypeId synthesize_flat(aura::ast::FlatAST& flat,
                                        aura::ast::StringPool& pool,
                                        aura::ast::NodeId id,
                                        aura::ast::NodeView v);
    aura::core::TypeId synthesize_flat_var(aura::ast::StringPool& pool,
                                            aura::ast::NodeView v);
    aura::core::TypeId synthesize_flat_call(aura::ast::FlatAST& flat,
                                             aura::ast::StringPool& pool,
                                             aura::ast::NodeView v);
    aura::core::TypeId synthesize_flat_lambda(aura::ast::FlatAST& flat,
                                               aura::ast::StringPool& pool,
                                               aura::ast::NodeView v);
    aura::core::TypeId synthesize_flat_if(aura::ast::FlatAST& flat,
                                           aura::ast::StringPool& pool,
                                           aura::ast::NodeView v);
    aura::core::TypeId synthesize_flat_let(aura::ast::FlatAST& flat,
                                            aura::ast::StringPool& pool,
                                            aura::ast::NodeView v,
                                            bool is_rec);
    aura::core::TypeId synthesize_flat_begin(aura::ast::FlatAST& flat,
                                              aura::ast::StringPool& pool,
                                              aura::ast::NodeView v);
    aura::core::TypeId synthesize_flat_annotation(aura::ast::FlatAST& flat,
                                                   aura::ast::StringPool& pool,
                                                   aura::ast::NodeView v);

    void check_flat_call(aura::ast::FlatAST& flat,
                         aura::ast::StringPool& pool,
                         aura::ast::NodeView v,
                         aura::core::TypeId expected);
    void check_flat_lambda(aura::ast::FlatAST& flat,
                           aura::ast::StringPool& pool,
                           aura::ast::NodeView v,
                           aura::core::TypeId expected);

    aura::core::TypeId lub(aura::core::TypeId a, aura::core::TypeId b);

    // Register all built-in primitives in the type environment
    void register_primitive(std::string name, std::vector<aura::core::TypeId> param_types, aura::core::TypeId ret_type);

    // Check if two types are coercible (gradual L6.6)
    bool is_coercible(aura::core::TypeId from, aura::core::TypeId to);
};

// ── TypeChecker — Public API ─────────────────────────────
export struct TypeChecker {
    aura::core::TypeRegistry& types;
    explicit TypeChecker(aura::core::TypeRegistry& reg) : types(reg) {}
    aura::core::TypeId infer_flat(aura::ast::FlatAST& flat,
                                   aura::ast::StringPool& pool,
                                   aura::ast::NodeId node,
                                   aura::diag::DiagnosticCollector& diag);
};

} // namespace aura::compiler
