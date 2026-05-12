module aura.compiler.type_checker;

namespace aura::compiler {

using namespace aura::ast;
using namespace aura::core;
using namespace aura::diag;

// ═══════════════════════════════════════════════════════════
// TypeEnv
// ═══════════════════════════════════════════════════════════

TypeEnv::TypeEnv(TypeRegistry& reg) : reg_(reg) {
    scopes_.emplace_back();
}

void TypeEnv::push_scope() { scopes_.emplace_back(); }
void TypeEnv::pop_scope()  { if (scopes_.size() > 1) scopes_.pop_back(); }

void TypeEnv::bind(std::string name, TypeId type) {
    scopes_.back()[std::move(name)] = Binding{type, false, {}};
}

TypeId TypeEnv::lookup(const std::string& name) {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
        auto f = it->find(name);
        if (f != it->end()) return f->second.type;
    }
    return TypeId{};  // invalid = not found
}

bool TypeEnv::is_bound(const std::string& name) const {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it)
        if (it->count(name)) return true;
    return false;
}

// ═══════════════════════════════════════════════════════════
// ConstraintSystem
// ═══════════════════════════════════════════════════════════

ConstraintSystem::ConstraintSystem(TypeRegistry& reg) : reg_(reg) {
    subst_.resize(32, TypeId{});
}

void ConstraintSystem::add(Constraint c) {
    constraints_.push_back(std::move(c));
}

TypeId ConstraintSystem::fresh_var() {
    return reg_.make_var("__t" + std::to_string(fresh_counter_++));
}

TypeId ConstraintSystem::normalize(TypeId id) {
    while (reg_.is_var(id)) {
        auto idx = id.index;
        if (idx >= subst_.size() || !subst_[idx].valid()) return id;
        id = subst_[idx];
    }
    return id;
}

bool ConstraintSystem::occurs_check(TypeId var, TypeId ty) {
    if (!reg_.is_var(var)) return false;
    ty = normalize(ty);
    if (var == ty) return true;
    if (auto* f = reg_.func_of(ty)) {
        for (auto a : f->args)
            if (occurs_check(var, a)) return true;
        return occurs_check(var, f->ret);
    }
    return false;
}

bool ConstraintSystem::consistent_unify(TypeId t1, TypeId t2) {
    t1 = normalize(t1);
    t2 = normalize(t2);

    // Any consistent with everything (sound gradual core)
    if (t1 == reg_.dynamic_type() || t2 == reg_.dynamic_type())
        return true;

    // Nominal equality
    if (t1 == t2) return true;

    // Type variable assignment
    if (reg_.is_var(t1)) {
        if (occurs_check(t1, t2)) return false;
        if (t1.index >= subst_.size()) subst_.resize(t1.index + 1);
        subst_[t1.index] = t2;
        return true;
    }
    if (reg_.is_var(t2)) {
        if (occurs_check(t2, t1)) return false;
        if (t2.index >= subst_.size()) subst_.resize(t2.index + 1);
        subst_[t2.index] = t1;
        return true;
    }

    // Function type decomposition
    auto* f1 = reg_.func_of(t1);
    auto* f2 = reg_.func_of(t2);
    if (f1 && f2) {
        if (f1->args.size() != f2->args.size()) return false;
        for (std::size_t i = 0; i < f1->args.size(); i++)
            if (!consistent_unify(f1->args[i], f2->args[i])) return false;
        return consistent_unify(f1->ret, f2->ret);
    }

    // Base type mismatch — caller can use coercion
    return false;
}

bool ConstraintSystem::solve() {
    for (auto& c : constraints_) {
        bool ok = consistent_unify(c.lhs, c.rhs);
        if (!ok) return false;
    }
    return true;
}

void ConstraintSystem::clear() {
    constraints_.clear();
    subst_.assign(subst_.size(), TypeId{});
    fresh_counter_ = 0;
}

// ═══════════════════════════════════════════════════════════
// InferenceEngine
// ═══════════════════════════════════════════════════════════

InferenceEngine::InferenceEngine(TypeRegistry& reg, DiagnosticCollector& diag)
    : reg_(reg), diag_(diag), cs_(reg), env_(reg) {}

TypeId InferenceEngine::infer(Expr* e) {
    if (!e) return reg_.dynamic_type();
    cs_.clear();
    auto result = synthesize(*e);
    if (!cs_.solve()) {
        diag_.report(Diagnostic(ErrorKind::TypeError, "type constraint solving failed"));
    }
    return result;
}

TypeId InferenceEngine::synthesize(const Expr& e) {
    return std::visit([this](const auto& node) -> TypeId {
        using T = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<T, LiteralIntNode>)
            return reg_.int_type();
        else if constexpr (std::is_same_v<T, LiteralStringNode>)
            return reg_.string_type();
        else if constexpr (std::is_same_v<T, VariableNode>)
            return synthesize_var(node);
        else if constexpr (std::is_same_v<T, CallNode>)
            return synthesize_call(node);
        else if constexpr (std::is_same_v<T, IfExprNode>)
            return synthesize_if(node);
        else if constexpr (std::is_same_v<T, LambdaNode>)
            return synthesize_lambda(node);
        else if constexpr (std::is_same_v<T, LetNode>)
            return synthesize_let(node);
        else if constexpr (std::is_same_v<T, LetRecNode>)
            return synthesize_letrec(node);
        else if constexpr (std::is_same_v<T, BeginNode>)
            return synthesize_begin(node);
        else if constexpr (std::is_same_v<T, TypeAnnotationNode>)
            return synthesize_annotation(node);
        else if constexpr (std::is_same_v<T, QuoteNode>)
            return reg_.dynamic_type();
        else if constexpr (std::is_same_v<T, DefineNode>)
            return reg_.void_type();
        else if constexpr (std::is_same_v<T, SetNode>)
            return reg_.void_type();
        else
            return reg_.dynamic_type();
    }, e.payload);
}

TypeId InferenceEngine::synthesize_var(const VariableNode& n) {
    auto ty = env_.lookup(n.name);
    if (!ty.valid()) {
        diag_.report(Diagnostic(ErrorKind::UnboundVariable, "unbound variable: " + n.name));
        return reg_.dynamic_type();
    }
    return ty;
}

TypeId InferenceEngine::synthesize_call(const CallNode& n) {
    TypeId func_type = synthesize(*n.function);
    auto* f_ty = reg_.func_of(func_type);

    if (f_ty) {
        // Known function type: check args, return expected return type
        std::size_t n_expected = std::min(f_ty->args.size(), n.args.size());
        for (std::size_t i = 0; i < n_expected; i++)
            check(*n.args[i], f_ty->args[i]);
        for (std::size_t i = n_expected; i < n.args.size(); i++)
            synthesize(*n.args[i]);  // extra args get dynamic
        return f_ty->ret;
    }

    // Unknown function type: check args dynamically
    for (auto* arg : n.args)
        synthesize(*arg);
    return reg_.dynamic_type();
}

TypeId InferenceEngine::synthesize_lambda(const LambdaNode& n) {
    env_.push_scope();
    std::vector<TypeId> param_types;
    for (auto& p : n.params) {
        auto tv = cs_.fresh_var();
        param_types.push_back(tv);
        env_.bind(p, tv);
    }
    TypeId body_type = synthesize(*n.body);
    env_.pop_scope();
    return reg_.register_func(std::move(param_types), body_type);
}

TypeId InferenceEngine::synthesize_if(const IfExprNode& n) {
    check(*n.condition, reg_.bool_type());
    if (!n.then_branch) return reg_.void_type();
    TypeId then_type = synthesize(*n.then_branch);
    TypeId else_type = n.else_branch ? synthesize(*n.else_branch) : reg_.void_type();
    return lub(then_type, else_type);
}

TypeId InferenceEngine::synthesize_let(const LetNode& n) {
    env_.push_scope();
    TypeId val_type = synthesize(*n.value);
    env_.bind(n.name, val_type);
    TypeId body_type = synthesize(*n.body);
    env_.pop_scope();
    return body_type;
}

TypeId InferenceEngine::synthesize_letrec(const LetRecNode& n) {
    env_.push_scope();
    // In letrec, the binding is visible in the value expression
    TypeId val_type = cs_.fresh_var();  // forward reference
    env_.bind(n.name, val_type);
    // Re-evaluate with proper type
    env_.pop_scope();
    env_.push_scope();
    TypeId actual_val_type = synthesize(*n.value);
    env_.bind(n.name, actual_val_type);
    TypeId body_type = synthesize(*n.body);
    env_.pop_scope();
    return body_type;
}

TypeId InferenceEngine::synthesize_begin(const BeginNode& n) {
    if (n.exprs.empty()) return reg_.void_type();
    TypeId last = reg_.void_type();
    for (auto* e : n.exprs)
        last = synthesize(*e);
    return last;
}

TypeId InferenceEngine::synthesize_annotation(const TypeAnnotationNode& n) {
    // Look up type name in registry
    auto inner_type = synthesize(*n.inner_expr);
    
    // Check type_name against registry
    if (!n.type_name.empty()) {
        // For now, all annotations are dynamic (just check inner)
        // TODO: resolve type_name → TypeId and check consistency
        check(*n.inner_expr, reg_.dynamic_type());
    }
    return inner_type;
}

void InferenceEngine::check(const Expr& e, TypeId expected) {
    TypeId inferred = synthesize(e);
    if (!cs_.consistent_unify(inferred, expected)) {
        auto msg = "type mismatch: expected "
                 + std::string(reg_.format_type(expected))
                 + ", got " + std::string(reg_.format_type(inferred));
        diag_.report(Diagnostic(ErrorKind::TypeError, std::move(msg)));
    }
}

TypeId InferenceEngine::lub(TypeId a, TypeId b) {
    if (a == b) return a;
    if (a == reg_.dynamic_type() || b == reg_.dynamic_type())
        return reg_.dynamic_type();
    // Int → Float promotion (reserved for L4)
    return reg_.dynamic_type();  // safe fallback
}

// ═══════════════════════════════════════════════════════════
// TypeChecker — Public API
// ═══════════════════════════════════════════════════════════

TypeId TypeChecker::infer(Expr* expr, DiagnosticCollector& diag) {
    InferenceEngine engine(types, diag);
    return engine.infer(expr);
}

} // namespace aura::compiler
