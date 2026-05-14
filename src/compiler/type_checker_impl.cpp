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
    : reg_(reg), diag_(diag), cs_(reg), env_(reg) {
    init_primitive_env();
}



bool InferenceEngine::is_coercible(TypeId from, TypeId to) {
    if (from == to) return true;
    // Dynamic coerce to/from anything (gradual core)
    if (from == reg_.dynamic_type() || to == reg_.dynamic_type())
        return true;
    auto from_tag = reg_.tag_of(from);
    auto to_tag = reg_.tag_of(to);
    // Int ↔ String
    if ((from_tag == TypeTag::INT && to_tag == TypeTag::STRING) ||
        (from_tag == TypeTag::STRING && to_tag == TypeTag::INT))
        return true;
    // Int ↔ Bool (truthiness / 0/1)
    if ((from_tag == TypeTag::INT && to_tag == TypeTag::BOOL) ||
        (from_tag == TypeTag::BOOL && to_tag == TypeTag::INT))
        return true;
    return false;
}

void InferenceEngine::register_primitive(std::string name, std::vector<TypeId> param_types, TypeId ret_type) {
    auto func_type = reg_.register_func(std::move(param_types), ret_type);
    env_.bind(std::move(name), func_type);
}

void InferenceEngine::init_primitive_env() {
    auto Int = reg_.int_type();
    auto Bool = reg_.bool_type();
    auto String = reg_.string_type();
    auto Dyn = reg_.dynamic_type();
    auto Void = reg_.void_type();
    auto Vector = reg_.lookup_type("Vector");

    // Arithmetic: (Int, Int) -> Int
    register_primitive("+",  {Int, Int}, Int);
    register_primitive("-",  {Int, Int}, Int);
    register_primitive("*",  {Int, Int}, Int);
    register_primitive("/",  {Int, Int}, Int);

    // Comparison: (Int, Int) -> Int (0/1 at runtime)
    register_primitive("=",  {Int, Int}, Int);
    register_primitive("<",  {Int, Int}, Int);
    register_primitive(">",  {Int, Int}, Int);
    register_primitive("<=", {Int, Int}, Int);
    register_primitive(">=", {Int, Int}, Int);

    // Boolean logic: runtime #t/#f are lexed as Int 0/1, so
    // truthiness-checking ops work on any value.
    // and/or are variadic — minimal signature uses 2 args
    register_primitive("and", {Dyn, Dyn}, Dyn);
    register_primitive("or",  {Dyn, Dyn}, Dyn);

    // not: works on any truthy/falsy value (runtime: a[0] == 0 → 1)
    register_primitive("not", {Dyn}, Int);
    register_primitive("eq?", {Dyn, Dyn}, Bool);

    // Type predicates return 0/1 which are Int at the type level
    register_primitive("number?",   {Dyn}, Int);
    register_primitive("string?",   {Dyn}, Int);
    register_primitive("boolean?",  {Dyn}, Int);
    register_primitive("null?",     {Dyn}, Int);
    register_primitive("pair?",     {Dyn}, Int);
    register_primitive("procedure?",{Dyn}, Int);
    register_primitive("list?",     {Dyn}, Int);
    register_primitive("equal?",    {Dyn, Dyn}, Int);

    // String operations
    register_primitive("string-append",  {String, String}, String);
    register_primitive("string-length",  {String}, Int);
    register_primitive("string-ref",     {String, Int}, Int);
    register_primitive("substring",      {String, Int, Int}, String);
    register_primitive("string=?",       {String, String}, Int);
    register_primitive("string<?",       {String, String}, Int);
    register_primitive("number->string", {Int}, String);
    register_primitive("string->number", {String}, Int);

    // Pair operations
    register_primitive("cons", {Dyn, Dyn}, Dyn);
    register_primitive("car",  {Dyn}, Dyn);
    register_primitive("cdr",  {Dyn}, Dyn);

    // List operations
    register_primitive("list",    {Dyn}, Dyn);       // varargs — minimal
    register_primitive("length",  {Dyn}, Int);
    register_primitive("list-ref", {Dyn, Int}, Dyn);
    register_primitive("member",  {Dyn, Dyn}, Dyn);
    register_primitive("append",  {Dyn, Dyn}, Dyn);
    register_primitive("reverse", {Dyn}, Dyn);
    register_primitive("map",     {Dyn, Dyn}, Dyn);
    register_primitive("filter",  {Dyn, Dyn}, Dyn);

    // I/O
    register_primitive("display", {Dyn}, Void);
    register_primitive("write",   {Dyn}, Void);
    register_primitive("newline", {},    Void);
    register_primitive("error",   {Dyn}, Void);
    register_primitive("assert",  {Dyn}, Void);

    // Introspection
    register_primitive("type-of", {Dyn}, reg_.type_type());
    register_primitive("type?",   {Dyn, String}, Bool);

    // Misc
    register_primitive("read",      {}, String);
    register_primitive("read-file", {String}, String);
    register_primitive("write-file",{String, String}, Void);
    register_primitive("file-exists?", {String}, Int);
    register_primitive("gensym",    {}, String);
    // Vector primitives
    register_primitive("vector",        {Dyn}, Vector);  // varargs — minimal
    register_primitive("vector-ref",    {Vector, Int}, Dyn);
    register_primitive("vector-set!",   {Vector, Int, Dyn}, Void);
    register_primitive("vector-length", {Vector}, Int);
    register_primitive("vector?",       {Dyn}, Bool);
    register_primitive("make-vector",   {Int, Dyn}, Vector);
    // List<->Vector conversion

}

TypeId InferenceEngine::lub(TypeId a, TypeId b) {
    if (a == b) return a;
    if (a == reg_.dynamic_type() || b == reg_.dynamic_type())
        return reg_.dynamic_type();
    // Int → Float promotion (reserved for L4)
    return reg_.dynamic_type();  // safe fallback
}

// ═══════════════════════════════════════════════════════════
// FlatAST Inference (bypasses Expr* reconstruction)
// ═══════════════════════════════════════════════════════════

// FlatAST version of analyze_predicate
struct OccurrenceInfoFlat {
    std::string var_name;
    TypeId refined_type;
    bool is_negation = false;
};

static std::optional<OccurrenceInfoFlat> analyze_predicate_flat(
    const FlatAST& flat, const StringPool& pool, NodeId cond_id, TypeRegistry& reg)
{
    auto cond = flat.get(cond_id);
    if (cond.tag != NodeTag::Call || cond.children.empty()) return std::nullopt;

    auto fn_id = cond.child(0);
    auto fn = flat.get(fn_id);

    // Check for (not p)
    if (fn.tag == NodeTag::Variable) {
        auto fn_name = pool.resolve(fn.sym_id);
        if (fn_name == "not" && cond.children.size() >= 2) {
            auto inner = analyze_predicate_flat(flat, pool, cond.child(1), reg);
            if (inner) { inner->is_negation = !inner->is_negation; return inner; }
            return std::nullopt;
        }

        // Check for (and p1 p2) — return first found
        if (fn_name == "and") {
            for (std::size_t i = 1; i < cond.children.size(); i++) {
                auto inner = analyze_predicate_flat(flat, pool, cond.child(i), reg);
                if (inner) return inner;
            }
            return std::nullopt;
        }

        // Check for (or p1 p2) — return first found
        if (fn_name == "or") {
            for (std::size_t i = 1; i < cond.children.size(); i++) {
                auto inner = analyze_predicate_flat(flat, pool, cond.child(i), reg);
                if (inner) return inner;
            }
            return std::nullopt;
        }

        // Check for (type? x "TypeName")
        if (fn_name == "type?" && cond.children.size() == 3) {
            auto var_id = cond.child(1);
            auto var_node = flat.get(var_id);
            auto type_lit_id = cond.child(2);
            auto type_lit = flat.get(type_lit_id);
            if (var_node.tag == NodeTag::Variable && type_lit.tag == NodeTag::LiteralString) {
                auto type_name = pool.resolve(type_lit.sym_id);
                auto type_id = reg.lookup_type(std::string(type_name));
                if (type_id.valid()) {
                    return OccurrenceInfoFlat{
                        std::string(pool.resolve(var_node.sym_id)), type_id
                    };
                }
            }
            return std::nullopt;
        }

        // Must be a predicate call: (pred? x)
        if (cond.children.size() == 2) {
            auto arg_id = cond.child(1);
            auto arg = flat.get(arg_id);
            if (arg.tag == NodeTag::Variable) {
                auto var_name = pool.resolve(arg.sym_id);
                if (fn_name == "string?")
                    return OccurrenceInfoFlat{std::string(var_name), reg.string_type()};
                else if (fn_name == "number?")
                    return OccurrenceInfoFlat{std::string(var_name), reg.int_type()};
                else if (fn_name == "boolean?")
                    return OccurrenceInfoFlat{std::string(var_name), reg.bool_type()};
                else if (fn_name == "null?")
                    return OccurrenceInfoFlat{std::string(var_name), reg.void_type()};
                else if (fn_name == "pair?")
                    return OccurrenceInfoFlat{std::string(var_name), reg.dynamic_type()};
                else if (fn_name == "procedure?")
                    return OccurrenceInfoFlat{std::string(var_name), reg.dynamic_type()};
            }
        }
    }

    return std::nullopt;
}

TypeId InferenceEngine::infer_flat(FlatAST& flat, StringPool& pool, NodeId id) {
    if (id == NULL_NODE || id >= flat.size()) return reg_.dynamic_type();
    cs_.clear();
    auto result = synthesize_flat(flat, pool, id, flat.get(id));
    if (!cs_.solve()) {
        diag_.report(Diagnostic(ErrorKind::TypeError, "type constraint solving failed", cur_loc_));
    }
    return result;
}

TypeId InferenceEngine::synthesize_flat(FlatAST& flat, StringPool& pool, NodeId /*id*/, NodeView v) {
    cur_loc_ = {v.line, v.col, 0};

    using Tag = NodeTag;
    switch (v.tag) {
    case Tag::LiteralInt:
        return reg_.int_type();
    case Tag::LiteralString:
        return reg_.string_type();
    case Tag::Variable:
        return synthesize_flat_var(pool, v);
    case Tag::Call:
        return synthesize_flat_call(flat, pool, v);
    case Tag::IfExpr:
        return synthesize_flat_if(flat, pool, v);
    case Tag::Lambda:
        return synthesize_flat_lambda(flat, pool, v);
    case Tag::Let:
        return synthesize_flat_let(flat, pool, v, false);
    case Tag::LetRec:
        return synthesize_flat_let(flat, pool, v, true);
    case Tag::Begin:
        return synthesize_flat_begin(flat, pool, v);
    case Tag::TypeAnnotation:
        return synthesize_flat_annotation(flat, pool, v);
    case Tag::Coercion:
        return synthesize_flat(flat, pool, v.child(0), flat.get(v.child(0)));
    case Tag::Define:
        return reg_.void_type();
    case Tag::Set:
        return reg_.void_type();
    case Tag::Quote:
        return reg_.dynamic_type();
    default:
        return reg_.dynamic_type();
    }
}

TypeId InferenceEngine::synthesize_flat_var(StringPool& pool, NodeView v) {
    auto name = pool.resolve(v.sym_id);
    if (name.empty()) {
        diag_.report(Diagnostic(ErrorKind::UnboundVariable, "unbound variable", cur_loc_));
        return reg_.dynamic_type();
    }
    std::string var_name(name);
    auto ty = env_.lookup(var_name);
    if (!ty.valid()) {
        diag_.report(Diagnostic(ErrorKind::UnboundVariable,
            "unbound variable: " + var_name, cur_loc_));
        return reg_.dynamic_type();
    }
    return ty;
}

TypeId InferenceEngine::synthesize_flat_call(FlatAST& flat, StringPool& pool, NodeView v) {
    // v.child(0) = function, v.child(1..n) = args
    if (v.children.empty()) return reg_.dynamic_type();

    auto func_id = v.child(0);
    TypeId func_type = synthesize_flat(flat, pool, func_id, flat.get(func_id));

    // COPY func type before processing args — synthesize_flat may call
    // register_func which can reallocate entries_, invalidating func_of* pointers.
    std::optional<FuncType> f_ty_copy;
    if (auto* ft = reg_.func_of(func_type))
        f_ty_copy = *ft;

    if (f_ty_copy) {
        auto& ft = *f_ty_copy;
        auto saved_loc = cur_loc_;
        std::size_t n_expected = std::min(ft.args.size(),
            v.children.size() > 1 ? v.children.size() - 1 : 0);
        for (std::size_t i = 0; i < n_expected; i++) {
            auto arg_id = v.child(i + 1);
            TypeId arg_type = synthesize_flat(flat, pool, arg_id, flat.get(arg_id));
            if (!cs_.consistent_unify(arg_type, ft.args[i])) {
                if (is_coercible(arg_type, ft.args[i])) {
                    auto msg = std::string("argument ")
                             + std::to_string(i)
                             + ": coercion from "
                             + std::string(reg_.format_type(arg_type))
                             + " to " + std::string(reg_.format_type(ft.args[i]));
                    diag_.report(Diagnostic(ErrorKind::Note, std::move(msg), saved_loc));
                } else {
                    auto msg = std::string("argument ")
                             + std::to_string(i)
                             + ": expected " + std::string(reg_.format_type(ft.args[i]))
                             + ", got " + std::string(reg_.format_type(arg_type));
                    diag_.report(Diagnostic(ErrorKind::TypeError, std::move(msg), saved_loc));
                }
            }
        }
        std::size_t num_args = v.children.size() > 1 ? v.children.size() - 1 : 0;
        // Skip arity check for variadic primitives
        bool is_variadic = false;
        auto callee_v = flat.get(func_id);
        if (callee_v.sym_id != INVALID_SYM && callee_v.tag == NodeTag::Variable) {
            auto cname = pool.resolve(callee_v.sym_id);
            is_variadic = (cname == "and" || cname == "or");
        }
        if (num_args != ft.args.size() && !ft.args.empty() && !is_variadic) {
            auto msg = "call: expected " + std::to_string(ft.args.size())
                     + " arguments, got " + std::to_string(num_args);
            diag_.report(Diagnostic(ErrorKind::ArityMismatch, std::move(msg), cur_loc_));
        }
        return ft.ret;
    }

    // Unknown function type: check args dynamically
    for (std::size_t i = 1; i < v.children.size(); i++)
        synthesize_flat(flat, pool, v.child(i), flat.get(v.child(i)));
    return reg_.dynamic_type();
}

TypeId InferenceEngine::synthesize_flat_lambda(FlatAST& flat, StringPool& pool, NodeView v) {
    // body = v.child(0), params = v.params (span of SymId)
    env_.push_scope();
    std::vector<TypeId> param_types;
    for (auto sym : v.params) {
        auto tv = cs_.fresh_var();
        param_types.push_back(tv);
        std::string pname(pool.resolve(sym));
        env_.bind(pname, tv);
    }
    TypeId body_type = reg_.void_type();
    if (!v.children.empty()) {
        auto body_id = v.child(0);
        body_type = synthesize_flat(flat, pool, body_id, flat.get(body_id));
    }
    env_.pop_scope();
    return reg_.register_func(std::move(param_types), body_type);
}

TypeId InferenceEngine::synthesize_flat_if(FlatAST& flat, StringPool& pool, NodeView v) {
    // children: 0=condition, 1=then_branch, 2=else_branch (can be NULL_NODE)
    if (v.children.empty()) return reg_.void_type();

    auto cond_id = v.child(0);
    check_flat(flat, pool, cond_id, reg_.bool_type());

    if (v.children.size() < 2) return reg_.void_type();
    auto then_id = v.child(1);
    if (then_id == NULL_NODE) return reg_.void_type();

    // Occurrence typing: analyze condition for type predicates
    auto occ = analyze_predicate_flat(flat, pool, cond_id, reg_);

    if (occ && !occ->is_negation) {
        // Then-branch: variable has refined type
        env_.push_scope();
        if (env_.is_bound(occ->var_name))
            env_.bind(occ->var_name, occ->refined_type);
        TypeId then_type = synthesize_flat(flat, pool, then_id, flat.get(then_id));
        env_.pop_scope();

        // Else-branch: no refinement (keeps original type)
        TypeId else_type = reg_.void_type();
        if (v.children.size() >= 3 && v.child(2) != NULL_NODE)
            else_type = synthesize_flat(flat, pool, v.child(2), flat.get(v.child(2)));
        return lub(then_type, else_type);
    }

    if (occ && occ->is_negation) {
        // Then-branch: no refinement
        TypeId then_type = synthesize_flat(flat, pool, then_id, flat.get(then_id));

        // Else-branch: variable has refined type
        TypeId else_type = reg_.void_type();
        env_.push_scope();
        if (env_.is_bound(occ->var_name))
            env_.bind(occ->var_name, occ->refined_type);
        if (v.children.size() >= 3 && v.child(2) != NULL_NODE)
            else_type = synthesize_flat(flat, pool, v.child(2), flat.get(v.child(2)));
        env_.pop_scope();
        return lub(then_type, else_type);
    }

    // No predicate found: standard if typing
    TypeId then_type = synthesize_flat(flat, pool, then_id, flat.get(then_id));
    TypeId else_type = reg_.void_type();
    if (v.children.size() >= 3 && v.child(2) != NULL_NODE)
        else_type = synthesize_flat(flat, pool, v.child(2), flat.get(v.child(2)));
    return lub(then_type, else_type);
}

TypeId InferenceEngine::synthesize_flat_let(FlatAST& flat, StringPool& pool, NodeView v, bool is_rec) {
    // children: 0=value, 1=body, name from v.sym_id
    // If is_rec, the binding is visible in the value expression too
    auto name = pool.resolve(v.sym_id);
    std::string var_name(name);

    if (is_rec) {
        // Forward reference pattern
        env_.push_scope();
        TypeId val_type = cs_.fresh_var();
        env_.bind(var_name, val_type);
        env_.pop_scope();

        env_.push_scope();
        TypeId actual_val_type = reg_.void_type();
        if (!v.children.empty() && v.child(0) != NULL_NODE)
            actual_val_type = synthesize_flat(flat, pool, v.child(0), flat.get(v.child(0)));
        env_.bind(var_name, actual_val_type);
        TypeId body_type = reg_.void_type();
        if (v.children.size() >= 2 && v.child(1) != NULL_NODE)
            body_type = synthesize_flat(flat, pool, v.child(1), flat.get(v.child(1)));
        env_.pop_scope();
        return body_type;
    }

    env_.push_scope();
    TypeId val_type = reg_.void_type();
    if (!v.children.empty() && v.child(0) != NULL_NODE)
        val_type = synthesize_flat(flat, pool, v.child(0), flat.get(v.child(0)));
    env_.bind(var_name, val_type);
    TypeId body_type = reg_.void_type();
    if (v.children.size() >= 2 && v.child(1) != NULL_NODE)
        body_type = synthesize_flat(flat, pool, v.child(1), flat.get(v.child(1)));
    env_.pop_scope();
    return body_type;
}

TypeId InferenceEngine::synthesize_flat_begin(FlatAST& flat, StringPool& pool, NodeView v) {
    if (v.children.empty()) return reg_.void_type();
    TypeId last = reg_.void_type();
    for (auto child_id : v.children)
        last = synthesize_flat(flat, pool, child_id, flat.get(child_id));
    return last;
}

TypeId InferenceEngine::synthesize_flat_annotation(FlatAST& flat, StringPool& pool, NodeView v) {
    // child(0) = inner_expr, sym_id = type name string
    if (v.children.empty()) return reg_.dynamic_type();
    auto inner_id = v.child(0);
    TypeId inner_type = synthesize_flat(flat, pool, inner_id, flat.get(inner_id));

    auto type_name = pool.resolve(v.sym_id);
    if (!type_name.empty()) {
        auto expected = reg_.lookup_type(std::string(type_name));
        if (!expected.valid()) {
            diag_.report(Diagnostic(ErrorKind::TypeError,
                "unknown type: " + std::string(type_name), cur_loc_));
        } else {
            check_flat(flat, pool, inner_id, expected);
        }
    }
    return inner_type;
}

void InferenceEngine::check_flat(FlatAST& flat, StringPool& pool, NodeId id, TypeId expected) {
    if (id == NULL_NODE || id >= flat.size()) return;
    auto v = flat.get(id);
    cur_loc_ = {v.line, v.col, 0};

    if (v.tag == NodeTag::Call)
        check_flat_call(flat, pool, v, expected);
    else if (v.tag == NodeTag::Lambda)
        check_flat_lambda(flat, pool, v, expected);
    else {
        TypeId inferred = synthesize_flat(flat, pool, id, v);
        if (!cs_.consistent_unify(inferred, expected)) {
            if (is_coercible(inferred, expected)) {
                auto msg = "coercion from "
                         + std::string(reg_.format_type(inferred))
                         + " to " + std::string(reg_.format_type(expected));
                diag_.report(Diagnostic(ErrorKind::Note, std::move(msg), cur_loc_));
            } else {
                auto msg = "type mismatch: expected "
                         + std::string(reg_.format_type(expected))
                         + ", got " + std::string(reg_.format_type(inferred));
                diag_.report(Diagnostic(ErrorKind::TypeError, std::move(msg), cur_loc_));
            }
        }
    }
}

void InferenceEngine::check_flat_call(FlatAST& flat, StringPool& pool, NodeView v, TypeId expected) {
    // Synthesize the call's type normally, then check against expected
    TypeId inferred = synthesize_flat_call(flat, pool, v);
    if (!cs_.consistent_unify(inferred, expected)) {
        if (is_coercible(inferred, expected)) {
            auto msg = "call return type: coercion from "
                     + std::string(reg_.format_type(inferred))
                     + " to " + std::string(reg_.format_type(expected));
            diag_.report(Diagnostic(ErrorKind::Note, std::move(msg), cur_loc_));
        } else {
            auto msg = "call return type mismatch: expected "
                     + std::string(reg_.format_type(expected))
                     + ", got " + std::string(reg_.format_type(inferred));
            diag_.report(Diagnostic(ErrorKind::TypeError, std::move(msg), cur_loc_));
        }
    }
}

void InferenceEngine::check_flat_lambda(FlatAST& flat, StringPool& pool, NodeView v, TypeId expected) {
    auto* f_ty = reg_.func_of(expected);
    if (!f_ty) {
        diag_.report(Diagnostic(ErrorKind::TypeError,
            "expected a function type but got "
            + std::string(reg_.format_type(expected)), cur_loc_));
        return;
    }
    if (f_ty->args.size() != v.params.size()) {
        diag_.report(Diagnostic(ErrorKind::ArityMismatch,
            "lambda expects " + std::to_string(v.params.size())
            + " parameters but context provides "
            + std::to_string(f_ty->args.size()), cur_loc_));
        return;
    }
    env_.push_scope();
    for (std::size_t i = 0; i < v.params.size(); ++i) {
        std::string pname(pool.resolve(v.params[i]));
        env_.bind(pname, f_ty->args[i]);
    }
    if (!v.children.empty() && v.child(0) != NULL_NODE)
        check_flat(flat, pool, v.child(0), f_ty->ret);
    env_.pop_scope();
}

// ═══════════════════════════════════════════════════════════
// TypeChecker — Public API
// ═══════════════════════════════════════════════════════════

TypeId TypeChecker::infer_flat(FlatAST& flat, StringPool& pool, NodeId node, DiagnosticCollector& diag) {
    InferenceEngine engine(types, diag);
    return engine.infer_flat(flat, pool, node);
}

} // namespace aura::compiler
