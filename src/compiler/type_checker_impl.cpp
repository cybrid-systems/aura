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

// ── Occurrence Typing (L6.7) ─────────────────────────────────
// Analyze if-condition for type predicates like (string? x)
struct OccurrenceInfo {
    std::string var_name;
    TypeId refined_type;
    bool is_negation = false;
};

// Try to extract type predicate info from a condition expression
static std::optional<OccurrenceInfo> analyze_predicate(const ast::Expr& cond, core::TypeRegistry& reg) {
    // Handle (not p) — recurse and swap
    auto* call = std::get_if<ast::CallNode>(&cond.payload);
    if (!call || call->args.empty()) return std::nullopt;
    
    auto* fn = std::get_if<ast::VariableNode>(&call->function->payload);
    
    // Check for (not p)
    if (fn && fn->name == "not" && !call->args.empty()) {
        auto inner = analyze_predicate(*call->args[0], reg);
        if (inner) { inner->is_negation = !inner->is_negation; return inner; }
        return std::nullopt;
    }
    
    // Check for (and p1 p2) — return first found
    if (fn && fn->name == "and") {
        for (auto* arg : call->args) {
            auto inner = analyze_predicate(*arg, reg);
            if (inner) return inner;
        }
        return std::nullopt;
    }
    
    // Check for (or p1 p2) — return first found
    if (fn && fn->name == "or") {
        for (auto* arg : call->args) {
            auto inner = analyze_predicate(*arg, reg);
            if (inner) return inner;
        }
        return std::nullopt;
    }
    
    // Must be a predicate call: (pred? x)
    if (call->args.size() != 1) return std::nullopt;
    auto* pred = std::get_if<ast::VariableNode>(&call->function->payload);
    auto* arg = std::get_if<ast::VariableNode>(&call->args[0]->payload);
    if (!pred || !arg) return std::nullopt;
    
    // Map predicate name → refined type
    // References the TypeRegistry from the inference engine context
    if (pred->name == "string?")
        return OccurrenceInfo{arg->name, reg.string_type()};
    else if (pred->name == "number?")
        return OccurrenceInfo{arg->name, reg.int_type()};
    else if (pred->name == "boolean?")
        return OccurrenceInfo{arg->name, reg.bool_type()};
    else if (pred->name == "null?")
        return OccurrenceInfo{arg->name, reg.void_type()};
    else if (pred->name == "pair?")
        return OccurrenceInfo{arg->name, reg.dynamic_type()};  // Pair: dynamic for now
    else if (pred->name == "procedure?")
        return OccurrenceInfo{arg->name, reg.dynamic_type()};  // (-> Any Any): dynamic
    
    return std::nullopt;
}

TypeId InferenceEngine::synthesize_if(const IfExprNode& n) {
    check(*n.condition, reg_.bool_type());
    if (!n.then_branch) return reg_.void_type();
    
    // Occurrence typing: analyze condition for type predicates
    auto occ = analyze_predicate(*n.condition, reg_);
    
    if (occ && !occ->is_negation) {
        // Then-branch: variable has refined type
        env_.push_scope();
        if (env_.is_bound(occ->var_name))
            env_.bind(occ->var_name, occ->refined_type);
        TypeId then_type = synthesize(*n.then_branch);
        env_.pop_scope();
        
        // Else-branch: no refinement (keeps original type)
        TypeId else_type = n.else_branch ? synthesize(*n.else_branch) : reg_.void_type();
        return lub(then_type, else_type);
    }
    
    if (occ && occ->is_negation) {
        // Then-branch: no refinement
        TypeId then_type = synthesize(*n.then_branch);
        
        // Else-branch: variable has refined type
        env_.push_scope();
        if (env_.is_bound(occ->var_name))
            env_.bind(occ->var_name, occ->refined_type);
        TypeId else_type = n.else_branch ? synthesize(*n.else_branch) : reg_.void_type();
        env_.pop_scope();
        return lub(then_type, else_type);
    }
    
    // No predicate found: standard if typing
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
    auto inner_type = synthesize(*n.inner_expr);
    
    if (!n.type_name.empty()) {
        auto expected = reg_.lookup_type(n.type_name);
        if (!expected.valid()) {
            diag_.report(Diagnostic(ErrorKind::TypeError,
                "unknown type: " + n.type_name));
        } else {
            check(*n.inner_expr, expected);
        }
    }
    return inner_type;
}

void InferenceEngine::check_call(const CallNode& n, TypeId expected) {
    // Synthesize the call's type normally, then check against expected
    TypeId inferred = synthesize_call(n);
    if (!cs_.consistent_unify(inferred, expected)) {
        auto msg = "call return type mismatch: expected "
                 + std::string(reg_.format_type(expected))
                 + ", got " + std::string(reg_.format_type(inferred));
        diag_.report(Diagnostic(ErrorKind::TypeError, std::move(msg)));
    }
}

void InferenceEngine::check_lambda(const LambdaNode& n, TypeId expected) {
    // If expected is a function type, use its param types to guide inference
    auto* f_ty = reg_.func_of(expected);
    if (!f_ty) {
        diag_.report(Diagnostic(ErrorKind::TypeError,
            "expected a function type but got "
            + std::string(reg_.format_type(expected))));
        return;
    }
    if (f_ty->args.size() != n.params.size()) {
        diag_.report(Diagnostic(ErrorKind::ArityMismatch,
            "lambda expects " + std::to_string(n.params.size())
            + " parameters but context provides "
            + std::to_string(f_ty->args.size())));
        return;
    }
    env_.push_scope();
    for (std::size_t i = 0; i < n.params.size(); ++i)
        env_.bind(n.params[i], f_ty->args[i]);
    check(*n.body, f_ty->ret);
    env_.pop_scope();
}

void InferenceEngine::check(const Expr& e, TypeId expected) {
    // Dispatch to node-specific check when available for better error messages
    // and context-guided inference (e.g., lambda param types from expected type)
    std::visit([this, &expected](const auto& node) {
        using T = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<T, CallNode>)
            check_call(node, expected);
        else if constexpr (std::is_same_v<T, LambdaNode>)
            check_lambda(node, expected);
        else {
            TypeId inferred = synthesize(node);
            if (!cs_.consistent_unify(inferred, expected)) {
                auto msg = "type mismatch: expected "
                         + std::string(reg_.format_type(expected))
                         + ", got " + std::string(reg_.format_type(inferred));
                diag_.report(Diagnostic(ErrorKind::TypeError, std::move(msg)));
            }
        }
    }, e.payload);
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
