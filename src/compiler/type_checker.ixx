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
    void collect_names(std::vector<std::string>& out) const;
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
    std::vector<std::int64_t> parent_;        // Union-Find parent (self=root, -1=uninitialized)
    std::vector<std::uint32_t> rank_;         // Union-Find rank (for union-by-rank)
    std::vector<aura::core::TypeId> binding_; // binding[rep] = concrete type for var rep
    uint64_t fresh_counter_ = 0;
    uint64_t first_free_var_ = 0; // first var index that belongs to this CS
public:
    explicit ConstraintSystem(aura::core::TypeRegistry& reg);
    void add(Constraint c);
    bool solve();
    void clear();
    aura::core::TypeId fresh_var();
    // Union-Find core
    aura::core::TypeId find_var(aura::core::TypeId id);
    bool unify(aura::core::TypeId t1, aura::core::TypeId t2);
    aura::core::TypeId find(aura::core::TypeId id); // normalize via Union-Find
    bool consistent_unify(aura::core::TypeId t1, aura::core::TypeId t2);
    bool consistent_subtype(aura::core::TypeId sub, aura::core::TypeId sup);
    bool occurs_check(aura::core::TypeId var, aura::core::TypeId ty);
    aura::core::TypeId normalize(aura::core::TypeId id);
};

// ── Ownership Environment (M4 Linear) ──────────────────────
export enum class OwnershipState : std::uint8_t {
    Owned,       // 拥有唯一所有权
    Moved,       // 所有权已转移
    Borrowed,    // 被不可变借用中
    MutBorrowed, // 被可变借用中
};

export struct OwnershipNote {
    aura::ast::NodeId node;
    std::string message;
    std::string kind; // "use-after-move" | "double-borrow" | "leaked-linear" | "invalid-state"
};

export class OwnershipEnv {
    std::vector<std::unordered_map<std::string, OwnershipState>> scopes_;
    // Tracks which variable bindings have had structural mutations applied
    // and need ownership re-validation on the next validate pass.
    std::unordered_set<std::string> ownership_dirty_;

public:
    explicit OwnershipEnv() { scopes_.emplace_back(); }

    void push_scope() { scopes_.emplace_back(); }
    void pop_scope() {
        if (scopes_.size() > 1)
            scopes_.pop_back();
    }

    void mark(const std::string& name, OwnershipState st) {
        // Always write to the current (innermost) scope.
        // This ensures that when the scope ends (pop_scope), the outer scope's
        // original state is restored — critical for lexical borrow scoping.
        scopes_.back()[name] = st;
    }

    OwnershipState get(const std::string& name) const {
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            auto f = it->find(name);
            if (f != it->end())
                return f->second;
        }
        return OwnershipState::Owned; // unknown vars assumed Owned
    }

    // Can read the variable (owned or imm-borrowed)
    bool can_use(const std::string& name) const {
        auto st = get(name);
        return st == OwnershipState::Owned || st == OwnershipState::Borrowed;
    }

    // Can move the variable (only if fully owned, no outstanding borrows)
    bool can_move(const std::string& name) const {
        return get(name) == OwnershipState::Owned;
    }

    // Can drop the variable (only if fully owned)
    bool can_drop(const std::string& name) const {
        return get(name) == OwnershipState::Owned;
    }

    // Can imm-borrow (allowed if owned or already imm-borrowed)
    bool can_borrow(const std::string& name) const {
        auto st = get(name);
        return st == OwnershipState::Owned || st == OwnershipState::Borrowed;
    }

    // Can mut-borrow (only if owned, exclusive access)
    bool can_mut_borrow(const std::string& name) const {
        return get(name) == OwnershipState::Owned;
    }

    std::string state_name(OwnershipState st) const {
        switch (st) {
            case OwnershipState::Owned:
                return "owned";
            case OwnershipState::Moved:
                return "moved";
            case OwnershipState::Borrowed:
                return "borrowed";
            case OwnershipState::MutBorrowed:
                return "mut-borrowed";
        }
        return "unknown";
    }

    // ── Ownership Dirt Tracking ───────────────────────────────
    // After structural mutations, mark affected bindings as ownership-dirty.
    void mark_ownership_dirty(const std::string& name) {
        ownership_dirty_.insert(name);
    }
    void mark_ownership_dirty_subtree(const std::vector<std::string>& names) {
        for (auto& n : names)
            ownership_dirty_.insert(n);
    }
    bool is_ownership_dirty(const std::string& name) const {
        return ownership_dirty_.count(name) > 0;
    }
    void clear_ownership_dirty() {
        ownership_dirty_.clear();
    }
    const std::unordered_set<std::string>& ownership_dirty_bindings() const {
        return ownership_dirty_;
    }

    // ── Post-Mutation Ownership Validation ────────────────────
    // Walks the AST within the dirty set, re-simulates ownership flow,
    // and reports any violations. Returns true if all checks pass.
    static bool validate_ownership(
        const aura::ast::FlatAST& flat,
        const aura::ast::StringPool& pool,
        aura::ast::NodeId root,
        const std::unordered_set<std::string>& dirty_bindings,
        std::vector<OwnershipNote>& notes_out);
};
export class InferenceEngine {
    aura::core::TypeRegistry& reg_;
    aura::diag::DiagnosticCollector& diag_;
    ConstraintSystem cs_;
    TypeEnv env_;
    OwnershipEnv ownership_env_;
    aura::diag::SourceLocation cur_loc_; // location of expression being checked

    // ADT constructors are looked up via TypeRegistry::get_adt_constructors()
public:
    // declared_modules: name → module_path, 用于跨模块错误定位
    std::unordered_map<std::string, std::string> declared_modules_;

    InferenceEngine(aura::core::TypeRegistry& reg, aura::diag::DiagnosticCollector& diag);

    // FlatAST inference entries
    aura::core::TypeId infer_flat(aura::ast::FlatAST& flat, aura::ast::StringPool& pool,
                                  aura::ast::NodeId node);
    void check_flat(aura::ast::FlatAST& flat, aura::ast::StringPool& pool, aura::ast::NodeId id,
                    aura::core::TypeId expected);

    // Initialize environment with primitive type signatures
    void init_primitive_env();

private:
    // FlatAST per-node-type inference
    aura::core::TypeId synthesize_flat(aura::ast::FlatAST& flat, aura::ast::StringPool& pool,
                                       aura::ast::NodeId id, aura::ast::NodeView v);
    aura::core::TypeId synthesize_flat_var(aura::ast::StringPool& pool, aura::ast::NodeView v);
    aura::core::TypeId synthesize_flat_call(aura::ast::FlatAST& flat, aura::ast::StringPool& pool,
                                            aura::ast::NodeView v);
    aura::core::TypeId synthesize_flat_lambda(aura::ast::FlatAST& flat, aura::ast::StringPool& pool,
                                              aura::ast::NodeView v);
    aura::core::TypeId synthesize_flat_if(aura::ast::FlatAST& flat, aura::ast::StringPool& pool,
                                          aura::ast::NodeView v);
    aura::core::TypeId synthesize_flat_let(aura::ast::FlatAST& flat, aura::ast::StringPool& pool,
                                           aura::ast::NodeId node_id, aura::ast::NodeView v,
                                           bool is_rec);
    aura::core::TypeId synthesize_flat_begin(aura::ast::FlatAST& flat, aura::ast::StringPool& pool,
                                             aura::ast::NodeView v);
    aura::core::TypeId synthesize_flat_annotation(aura::ast::FlatAST& flat,
                                                  aura::ast::StringPool& pool,
                                                  aura::ast::NodeView v);

    void check_flat_call(aura::ast::FlatAST& flat, aura::ast::StringPool& pool,
                         aura::ast::NodeView v, aura::core::TypeId expected);
    void check_flat_lambda(aura::ast::FlatAST& flat, aura::ast::StringPool& pool,
                           aura::ast::NodeView v, aura::core::TypeId expected);

    aura::core::TypeId lub(aura::core::TypeId a, aura::core::TypeId b);

    // Register all built-in primitives in the type environment
    void register_primitive(std::string name, std::vector<aura::core::TypeId> param_types,
                            aura::core::TypeId ret_type);
   void register_poly_primitive(std::string name,
                                 std::vector<aura::core::TypeId> param_types,
                                 aura::core::TypeId ret_type,
                                 std::vector<aura::core::TypeId> type_vars);

    // Check if two types are coercible (gradual L6.6)
    bool is_coercible(aura::core::TypeId from, aura::core::TypeId to);
};

// ── TypeChecker — Public API ─────────────────────────────
export struct TypeChecker {
    aura::core::TypeRegistry& types;
    aura::core::TypeId infer_flat(aura::ast::FlatAST& flat, aura::ast::StringPool& pool,
                                  aura::ast::NodeId node, aura::diag::DiagnosticCollector& diag);

    // 注入自定义类型签名（来自 declare-type / 模块类型声明）
    // 在 infer_flat 前调用。name_to_sig: (name → "param1 param2|rettype")。
    // 格式示例: "Int Int|Int" 表示 (Int, Int) -> Int
    // module_src: name → 来源模块文件（用于跨模块错误定位）
    void inject_type_sigs(
        const std::unordered_map<std::string, std::string>& sigs,
        const std::unordered_map<std::string, std::string>& module_src = {});

    // 查询已注入类型的来源模块名
    std::string declared_type_module(const std::string& name) const;

    explicit TypeChecker(aura::core::TypeRegistry& reg)
        : types(reg) {}

private:
    // name → 模块源文件路径（来自 inject_type_sigs 的 module_src）
    std::unordered_map<std::string, std::string> type_module_src_;
};

} // namespace aura::compiler
