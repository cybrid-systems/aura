# 0 "/home/dev/code/aura/src/compiler/type_checker_impl.cpp"
# 0 "<built-in>"
# 0 "<command-line>"
# 1 "/usr/include/stdc-predef.h" 1 3
# 0 "<command-line>" 2
# 1 "/home/dev/code/aura/src/compiler/type_checker_impl.cpp"
module aura.compiler.type_checker;

namespace aura::compiler {

using namespace aura::ast;
using namespace aura::core;
using namespace aura::diag;





TypeEnv::TypeEnv(TypeRegistry& reg) : reg_(reg) {
    scopes_.emplace_back();
}

void TypeEnv::push_scope() { scopes_.emplace_back(); }
void TypeEnv::pop_scope() { if (scopes_.size() > 1) scopes_.pop_back(); }

void TypeEnv::bind(std::string name, TypeId type) {
    scopes_.back()[std::move(name)] = Binding{type, false, {}};
}

TypeId TypeEnv::lookup(const std::string& name) {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
        auto f = it->find(name);
        if (f != it->end()) return f->second.type;
    }
    return TypeId{};
}

bool TypeEnv::is_bound(const std::string& name) const {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it)
        if (it->count(name)) return true;
    return false;
}





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

    if (auto* f = reg_.func_of(id)) {
        std::vector<TypeId> new_args;
        for (auto& a : f->args) new_args.push_back(normalize(a));
        auto new_ret = normalize(f->ret);
        return reg_.register_func(std::move(new_args), new_ret);
    }
    if (auto* ft = reg_.forall_of(id)) {
        auto new_body = normalize(ft->body);
        return reg_.register_forall(ft->var, new_body);
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


    if (t1 == reg_.dynamic_type() || t2 == reg_.dynamic_type())
        return true;


    if (t1 == t2) return true;


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


    auto* f1 = reg_.func_of(t1);
    auto* f2 = reg_.func_of(t2);
    if (f1 && f2) {
        if (f1->args.size() != f2->args.size()) return false;
        for (std::size_t i = 0; i < f1->args.size(); i++)
            if (!consistent_unify(f1->args[i], f2->args[i])) return false;
        return consistent_unify(f1->ret, f2->ret);
    }




    if (!reg_.is_var(t1) && !reg_.is_var(t2) && !f1 && !f2) {

        return true;
    }


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





InferenceEngine::InferenceEngine(TypeRegistry& reg, DiagnosticCollector& diag)
    : reg_(reg), diag_(diag), cs_(reg), env_(reg) {
    init_primitive_env();
}



bool InferenceEngine::is_coercible(TypeId from, TypeId to) {
    if (from == to) return true;

    if (from == reg_.dynamic_type() || to == reg_.dynamic_type())
        return true;
    auto from_tag = reg_.tag_of(from);
    auto to_tag = reg_.tag_of(to);

    if ((from_tag == TypeTag::INT && to_tag == TypeTag::STRING) ||
        (from_tag == TypeTag::STRING && to_tag == TypeTag::INT))
        return true;

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
    auto Float = reg_.register_type(TypeTag::FLOAT, "Float");
    (void)Float;
    auto String = reg_.string_type();
    auto Dyn = reg_.dynamic_type();
    auto Void = reg_.void_type();
    auto Vector = reg_.lookup_type("Vector");
    auto Hash = reg_.lookup_type("Hash");


    register_primitive("+", {Dyn, Dyn}, Dyn);
    register_primitive("-", {Dyn, Dyn}, Dyn);
    register_primitive("*", {Dyn, Dyn}, Dyn);
    register_primitive("/", {Dyn, Dyn}, Dyn);


    register_primitive("=", {Dyn, Dyn}, Bool);
    register_primitive("<", {Dyn, Dyn}, Bool);
    register_primitive(">", {Dyn, Dyn}, Bool);
    register_primitive("<=", {Dyn, Dyn}, Bool);
    register_primitive(">=", {Dyn, Dyn}, Bool);




    register_primitive("and", {Dyn, Dyn}, Dyn);
    register_primitive("or", {Dyn, Dyn}, Dyn);


    register_primitive("not", {Dyn}, Bool);
    register_primitive("eq?", {Dyn, Dyn}, Bool);


    register_primitive("number?", {Dyn}, Bool);
    register_primitive("string?", {Dyn}, Bool);
    register_primitive("boolean?", {Dyn}, Bool);
    register_primitive("null?", {Dyn}, Bool);
    register_primitive("pair?", {Dyn}, Bool);
    register_primitive("procedure?",{Dyn}, Bool);
    register_primitive("list?", {Dyn}, Bool);
    register_primitive("equal?", {Dyn, Dyn}, Bool);


    register_primitive("string-append", {String, String}, String);
    register_primitive("string-length", {String}, Int);
    register_primitive("string-ref", {String, Int}, Int);
    register_primitive("substring", {String, Int, Int}, String);
    register_primitive("string=?", {String, String}, Bool);
    register_primitive("string<?", {String, String}, Bool);
    register_primitive("number->string", {Int}, String);
    register_primitive("string->number", {String}, Int);


    register_primitive("cons", {Dyn, Dyn}, Dyn);
    register_primitive("car", {Dyn}, Dyn);
    register_primitive("cdr", {Dyn}, Dyn);


    register_primitive("caar", {Dyn}, Dyn);
    register_primitive("cadr", {Dyn}, Dyn);
    register_primitive("cdar", {Dyn}, Dyn);
    register_primitive("cddr", {Dyn}, Dyn);
    register_primitive("caaar", {Dyn}, Dyn);
    register_primitive("caadr", {Dyn}, Dyn);
    register_primitive("cadar", {Dyn}, Dyn);
    register_primitive("caddr", {Dyn}, Dyn);
    register_primitive("cdaar", {Dyn}, Dyn);
    register_primitive("cdadr", {Dyn}, Dyn);
    register_primitive("cddar", {Dyn}, Dyn);
    register_primitive("cdddr", {Dyn}, Dyn);


    register_primitive("set-car!", {Dyn, Dyn}, Void);
    register_primitive("set-cdr!", {Dyn, Dyn}, Void);


    register_primitive("list", {Dyn}, Dyn);
    register_primitive("length", {Dyn}, Int);
    register_primitive("list-ref", {Dyn, Int}, Dyn);
    register_primitive("member", {Dyn, Dyn}, Dyn);
    register_primitive("append", {Dyn, Dyn}, Dyn);
    register_primitive("reverse", {Dyn}, Dyn);
    register_primitive("take", {Int, Dyn}, Dyn);
    register_primitive("drop", {Int, Dyn}, Dyn);
    register_primitive("foldl", {Dyn, Dyn, Dyn}, Dyn);



    {
        auto a = reg_.make_var("a");
        auto b = reg_.make_var("b");
        auto a_to_b = reg_.register_func({a}, b);
        auto list_a = reg_.dynamic_type();
        auto map_type = reg_.register_func({a_to_b, list_a}, b);
        auto forall_map = reg_.register_forall(a, reg_.register_forall(b, map_type));
        env_.bind("map", forall_map);
        env_.bind("filter", forall_map);
    }


    register_primitive("display", {Dyn}, Void);
    register_primitive("write", {Dyn}, Void);
    register_primitive("newline", {}, Void);
    register_primitive("error", {Dyn}, Void);
    register_primitive("assert", {Dyn}, Void);


    register_primitive("type-of", {Dyn}, reg_.type_type());
    register_primitive("type?", {Dyn, String}, Bool);


    register_primitive("read", {}, String);
    register_primitive("read-file", {String}, String);
    register_primitive("load-module", {String}, Dyn);
    register_primitive("import", {String}, Dyn);
    register_primitive("write-file",{String, String}, Void);
    register_primitive("file-exists?", {String}, Bool);
    register_primitive("gensym", {}, String);


    register_primitive("mutate:replace-type", {Dyn, String}, Dyn);
    register_primitive("mutate:record-patch", {Dyn, String, String}, Dyn);
    register_primitive("mutation-count", {}, Int);
    register_primitive("mutation-history", {Dyn}, Dyn);

    register_primitive("vector", {Dyn}, Vector);
    register_primitive("vector-ref", {Vector, Int}, Dyn);
    register_primitive("vector-set!", {Vector, Int, Dyn}, Void);
    register_primitive("vector-length", {Vector}, Int);
    register_primitive("vector?", {Dyn}, Bool);
    register_primitive("make-vector", {Int, Dyn}, Vector);



    register_primitive("hash", {Dyn}, Hash);
    register_primitive("hash-ref", {Hash, Dyn}, Dyn);
    register_primitive("hash-set!", {Hash, Dyn, Dyn}, Void);
    register_primitive("hash-length", {Hash}, Int);
    register_primitive("hash-keys", {Hash}, Dyn);
    register_primitive("hash-values", {Hash}, Dyn);
    register_primitive("hash?", {Dyn}, Bool);
    register_primitive("hash-remove!", {Hash, Dyn}, Bool);
    register_primitive("hash-has-key?", {Hash, Dyn}, Bool);


    register_primitive("modulo", {Int, Int}, Int);
    register_primitive("quotient", {Int, Int}, Int);
    register_primitive("remainder", {Int, Int}, Int);
    register_primitive("abs", {Int}, Int);
    register_primitive("gcd", {Int, Int}, Int);
    register_primitive("lcm", {Int, Int}, Int);
    register_primitive("min", {Dyn, Dyn}, Dyn);
    register_primitive("max", {Dyn, Dyn}, Dyn);


    register_primitive("char?", {Dyn}, Bool);
    register_primitive("char->integer", {Dyn}, Int);
    register_primitive("integer->char", {Int}, Int);
    register_primitive("string->list", {String}, Dyn);
    register_primitive("list->string", {Dyn}, String);
    register_primitive("read-line", {}, String);
    register_primitive("eof-object?", {Dyn}, Bool);


    register_primitive("integer?", {Dyn}, Bool);
    register_primitive("float?", {Dyn}, Bool);


    register_primitive("list->vector", {Dyn}, Vector);
    register_primitive("vector->list", {Vector}, Dyn);
}

TypeId InferenceEngine::lub(TypeId a, TypeId b) {
    if (a == b) return a;
    if (a == reg_.dynamic_type() || b == reg_.dynamic_type())
        return reg_.dynamic_type();

    if ((a == reg_.int_type() && b == reg_.lookup_type("Float")) ||
        (a == reg_.lookup_type("Float") && b == reg_.int_type()))
        return reg_.lookup_type("Float");
    return reg_.dynamic_type();
}






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


    if (fn.tag == NodeTag::Variable) {
        auto fn_name = pool.resolve(fn.sym_id);
        if (fn_name == "not" && cond.children.size() >= 2) {
            auto inner = analyze_predicate_flat(flat, pool, cond.child(1), reg);
            if (inner) { inner->is_negation = !inner->is_negation; return inner; }
            return std::nullopt;
        }


        if (fn_name == "and") {
            for (std::size_t i = 1; i < cond.children.size(); i++) {
                auto inner = analyze_predicate_flat(flat, pool, cond.child(i), reg);
                if (inner) return inner;
            }
            return std::nullopt;
        }


        if (fn_name == "or") {
            for (std::size_t i = 1; i < cond.children.size(); i++) {
                auto inner = analyze_predicate_flat(flat, pool, cond.child(i), reg);
                if (inner) return inner;
            }
            return std::nullopt;
        }


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
    return cs_.normalize(result);
}

TypeId InferenceEngine::synthesize_flat(FlatAST& flat, StringPool& pool, NodeId id, NodeView v) {
    cur_loc_ = {v.line, v.col, 0};




    if (!flat.is_dirty(id)) {
        auto cached = flat.type_id(id);
        if (cached > 0 && cached < reg_.size()) {
            return TypeId{cached, 1};
        }

    }

    TypeId result;
    using Tag = NodeTag;
    switch (v.tag) {
    case Tag::LiteralInt:
        result = (v.marker == SyntaxMarker::BoolLiteral) ? reg_.bool_type() : reg_.int_type();
        break;
    case Tag::LiteralFloat:
        result = reg_.lookup_type("Float"); break;
    case Tag::LiteralString:
        result = reg_.string_type(); break;
    case Tag::Variable:
        result = synthesize_flat_var(pool, v); break;
    case Tag::Call:
        result = synthesize_flat_call(flat, pool, v); break;
    case Tag::IfExpr:
        result = synthesize_flat_if(flat, pool, v); break;
    case Tag::Lambda:
        result = synthesize_flat_lambda(flat, pool, v); break;
    case Tag::Let:
        result = synthesize_flat_let(flat, pool, v, false); break;
    case Tag::LetRec:
        result = synthesize_flat_let(flat, pool, v, true); break;
    case Tag::Begin:
        result = synthesize_flat_begin(flat, pool, v); break;
    case Tag::TypeAnnotation:
        result = synthesize_flat_annotation(flat, pool, v); break;
    case Tag::Coercion:
        result = synthesize_flat(flat, pool, v.child(0), flat.get(v.child(0)));
        break;
    case Tag::Define:
        result = reg_.void_type(); break;
    case Tag::Set:
        result = reg_.void_type(); break;
    case Tag::Quote:
        result = reg_.dynamic_type(); break;
    case Tag::MacroDef: {
        env_.push_scope();
        std::vector<TypeId> param_types;
        for (auto pid : v.params) {
            auto pname = std::string(pool.resolve(pid));
            env_.bind(pname, reg_.dynamic_type());
            param_types.push_back(reg_.dynamic_type());
        }
        TypeId body_type = reg_.void_type();
        if (!v.children.empty()) {
            auto body_id = v.child(0);
            body_type = synthesize_flat(flat, pool, body_id, flat.get(body_id));
        }
        env_.pop_scope();
        result = reg_.register_func(std::move(param_types), body_type);
        break;
    }
    default:
        result = reg_.dynamic_type(); break;
    }


    flat.set_type(id, result.index);
    return result;
}

TypeId InferenceEngine::synthesize_flat_var(StringPool& pool, NodeView v) {
    auto name = pool.resolve(v.sym_id);
    if (name.empty()) {
        diag_.report(Diagnostic(ErrorKind::UnboundVariable, "unbound variable", cur_loc_));
        return reg_.dynamic_type();
    }
    std::string var_name(name);
    auto ty_raw = env_.lookup(var_name);
    if (!ty_raw.valid()) {
        diag_.report(Diagnostic(ErrorKind::UnboundVariable,
            "unbound variable: " + var_name, cur_loc_));
        return reg_.dynamic_type();
    }

    auto instantiate_all = [&](this const auto& self, TypeId tid) -> TypeId {
        auto* ft = reg_.forall_of(tid);
        if (!ft) return tid;
        auto inst = reg_.instantiate(tid, [this]() { return cs_.fresh_var(); });
        return self(inst);
    };
    if (reg_.forall_of(ty_raw)) {
        return instantiate_all(ty_raw);
    }
    return ty_raw;
}

TypeId InferenceEngine::synthesize_flat_call(FlatAST& flat, StringPool& pool, NodeView v) {

    if (v.children.empty()) return reg_.dynamic_type();

    auto func_id = v.child(0);
    TypeId func_type = synthesize_flat(flat, pool, func_id, flat.get(func_id));




    auto infer_arith = [&]() -> std::optional<TypeId> {
        auto callee_of_func = flat.get(func_id);
        if (callee_of_func.tag != NodeTag::Variable || callee_of_func.sym_id == INVALID_SYM)
            return std::nullopt;
        auto fname = pool.resolve(callee_of_func.sym_id);
        static const std::unordered_set<std::string> arith = {"+", "-", "*", "/"};
        if (!arith.count(std::string(fname))) return std::nullopt;



        if (v.children.size() < 3) {
            if (v.children.size() == 2) {
                auto t0 = synthesize_flat(flat, pool, v.child(1), flat.get(v.child(1)));
                t0 = cs_.normalize(t0);
                if (!reg_.is_var(t0)) return t0;
            }
            return reg_.int_type();
        }


        TypeId t0 = synthesize_flat(flat, pool, v.child(1), flat.get(v.child(1)));
        TypeId t1 = synthesize_flat(flat, pool, v.child(2), flat.get(v.child(2)));
        t0 = cs_.normalize(t0);
        t1 = cs_.normalize(t1);
        auto tag0 = reg_.tag_of(t0);
        auto tag1 = reg_.tag_of(t1);


        if (tag0 == TypeTag::INT && tag1 == TypeTag::INT) return reg_.int_type();
        if (tag0 == TypeTag::FLOAT && tag1 == TypeTag::FLOAT) return reg_.lookup_type("Float");
        if ((tag0 == TypeTag::INT && tag1 == TypeTag::FLOAT) ||
            (tag0 == TypeTag::FLOAT && tag1 == TypeTag::INT)) {

            if (tag0 == TypeTag::INT) cs_.consistent_unify(t0, reg_.lookup_type("Float"));
            if (tag1 == TypeTag::INT) cs_.consistent_unify(t1, reg_.lookup_type("Float"));
            return reg_.lookup_type("Float");
        }



        if (!reg_.is_var(t0) && !reg_.is_var(t1)) {

            return reg_.int_type();
        }


        auto result = cs_.fresh_var();
        cs_.consistent_unify(t0, result);
        cs_.consistent_unify(t1, result);
        return result;
    };
    if (auto arith_result = infer_arith()) {

        return *arith_result;
    }




    auto instantiate_all_direct = [&](this const auto& self, TypeId tid) -> TypeId {
        auto* ft = reg_.forall_of(tid);
        if (!ft) return tid;
        auto inst = reg_.instantiate(tid, [this]() { return cs_.fresh_var(); });
        return self(inst);
    };
    func_type = instantiate_all_direct(func_type);



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

        bool is_variadic = false;
        auto callee_v = flat.get(func_id);
        if (callee_v.sym_id != INVALID_SYM && callee_v.tag == NodeTag::Variable) {
            auto cname = pool.resolve(callee_v.sym_id);
            is_variadic = (cname == "and" || cname == "or"
                        || cname == "list" || cname == "vector"
                        || cname == "hash"
                        || cname == "+" || cname == "-"
                        || cname == "*" || cname == "/"
                        || cname == "=" || cname == "<"
                        || cname == ">" || cname == "<="
                        || cname == ">=");
        }
        if (num_args != ft.args.size() && !ft.args.empty() && !is_variadic) {
            auto msg = std::string("call '")
                     + std::string(pool.resolve(callee_v.sym_id))
                     + "': expected " + std::to_string(ft.args.size())
                     + " arguments, got " + std::to_string(num_args);
            diag_.report(Diagnostic(ErrorKind::ArityMismatch, std::move(msg), cur_loc_));
        }


        return ft.ret;
    }


    for (std::size_t i = 1; i < v.children.size(); i++)
        synthesize_flat(flat, pool, v.child(i), flat.get(v.child(i)));
    return reg_.dynamic_type();
}

TypeId InferenceEngine::synthesize_flat_lambda(FlatAST& flat, StringPool& pool, NodeView v) {

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

    if (v.children.empty()) return reg_.void_type();

    auto cond_id = v.child(0);
    check_flat(flat, pool, cond_id, reg_.bool_type());

    if (v.children.size() < 2) return reg_.void_type();
    auto then_id = v.child(1);
    if (then_id == NULL_NODE) return reg_.void_type();


    auto occ = analyze_predicate_flat(flat, pool, cond_id, reg_);

    if (occ && !occ->is_negation) {

        env_.push_scope();
        if (env_.is_bound(occ->var_name))
            env_.bind(occ->var_name, occ->refined_type);
        TypeId then_type = synthesize_flat(flat, pool, then_id, flat.get(then_id));
        env_.pop_scope();


        TypeId else_type = reg_.void_type();
        if (v.children.size() >= 3 && v.child(2) != NULL_NODE)
            else_type = synthesize_flat(flat, pool, v.child(2), flat.get(v.child(2)));
        return lub(then_type, else_type);
    }

    if (occ && occ->is_negation) {

        TypeId then_type = synthesize_flat(flat, pool, then_id, flat.get(then_id));


        TypeId else_type = reg_.void_type();
        env_.push_scope();
        if (env_.is_bound(occ->var_name))
            env_.bind(occ->var_name, occ->refined_type);
        if (v.children.size() >= 3 && v.child(2) != NULL_NODE)
            else_type = synthesize_flat(flat, pool, v.child(2), flat.get(v.child(2)));
        env_.pop_scope();
        return lub(then_type, else_type);
    }


    TypeId then_type = synthesize_flat(flat, pool, then_id, flat.get(then_id));
    TypeId else_type = reg_.void_type();
    if (v.children.size() >= 3 && v.child(2) != NULL_NODE)
        else_type = synthesize_flat(flat, pool, v.child(2), flat.get(v.child(2)));
    return lub(then_type, else_type);
}

TypeId InferenceEngine::synthesize_flat_let(FlatAST& flat, StringPool& pool, NodeView v, bool is_rec) {


    auto name = pool.resolve(v.sym_id);
    std::string var_name(name);

    if (is_rec) {
        env_.push_scope();

        TypeId fwd_var = cs_.fresh_var();
        env_.bind(var_name, fwd_var);


        TypeId val_type = reg_.void_type();
        if (!v.children.empty() && v.child(0) != NULL_NODE)
            val_type = synthesize_flat(flat, pool, v.child(0), flat.get(v.child(0)));


        cs_.consistent_unify(fwd_var, val_type);


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





    auto val_norm = cs_.normalize(val_type);
    auto fvs = reg_.free_vars(val_norm);
    if (!fvs.empty()) {


        TypeId poly = val_norm;
        for (auto& fv_id : fvs) {
            poly = reg_.register_forall(fv_id, poly);
        }
        env_.bind(var_name, poly);
    } else {
        env_.bind(var_name, val_norm);
    }

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





TypeId TypeChecker::infer_flat(FlatAST& flat, StringPool& pool, NodeId node, DiagnosticCollector& diag) {
    InferenceEngine engine(types, diag);
    return engine.infer_flat(flat, pool, node);
}

}
