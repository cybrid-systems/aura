module aura.compiler.type_checker;

namespace aura::compiler {

using namespace aura::ast;
using namespace aura::core;
using namespace aura::diag;

// ── Edit distance for "did you mean" suggestions ────────────────
static std::size_t edit_distance(std::string_view a, std::string_view b) {
    auto m = a.size(), n = b.size();
    if (m == 0) return n;
    if (n == 0) return m;
    std::vector<std::size_t> prev(n + 1), cur(n + 1);
    for (std::size_t j = 0; j <= n; ++j) prev[j] = j;
    for (std::size_t i = 1; i <= m; ++i) {
        cur[0] = i;
        for (std::size_t j = 1; j <= n; ++j) {
            auto cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost});
        }
        std::swap(prev, cur);
    }
    return prev[n];
}

static std::string closest_match(std::string_view name,
                                  const std::vector<std::string>& candidates,
                                  std::size_t max_dist = 3) {
    std::string best;
    std::size_t best_dist = max_dist + 1;
    for (auto& c : candidates) {
        auto d = edit_distance(name, c);
        if (d < best_dist) {
            best_dist = d;
            best = c;
        }
    }
    return best;
}

// ═══════════════════════════════════════════════════════════
// TypeEnv
// ═══════════════════════════════════════════════════════════

TypeEnv::TypeEnv(TypeRegistry& reg)
    : reg_(reg) {
    scopes_.emplace_back();
}

void TypeEnv::push_scope() {
    scopes_.emplace_back();
}
void TypeEnv::pop_scope() {
    if (scopes_.size() > 1)
        scopes_.pop_back();
}

void TypeEnv::bind(std::string name, TypeId type) {
    scopes_.back()[std::move(name)] = Binding{type, false, {}};
}

TypeId TypeEnv::lookup(const std::string& name) {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
        auto f = it->find(name);
        if (f != it->end())
            return f->second.type;
    }
    return TypeId{}; // invalid = not found
}

bool TypeEnv::is_bound(const std::string& name) const {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it)
        if (it->count(name))
            return true;
    return false;
}

void TypeEnv::collect_names(std::vector<std::string>& out) const {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it)
        for (auto& [name, _] : *it)
            out.push_back(name);
}

// ═══════════════════════════════════════════════════════════
// ConstraintSystem — Union-Find based
// ═══════════════════════════════════════════════════════════

ConstraintSystem::ConstraintSystem(TypeRegistry& reg)
    : reg_(reg) {}

void ConstraintSystem::add(Constraint c) {
    constraints_.push_back(std::move(c));
}

TypeId ConstraintSystem::fresh_var() {
    auto id = reg_.make_var("__t" + std::to_string(fresh_counter_++));
    // Ensure Union-Find arrays are sized for this variable
    auto idx = static_cast<std::size_t>(id.index);
    if (idx >= parent_.size()) {
        parent_.resize(idx + 64, -1);
        rank_.resize(idx + 64, 0);
        binding_.resize(idx + 64, TypeId{});
    }
    // Initialize as root (self-parent). Note: clear() sets parent to -1
    // to indicate uninitialized; each fresh_var must set its own parent.
    if (parent_[idx] < 0)
        parent_[idx] = static_cast<std::int64_t>(idx);
    if (first_free_var_ == 0 || id.index < first_free_var_)
        first_free_var_ = id.index;
    return id;
}

// Find representative with path compression
std::int64_t find_rep(const std::vector<std::int64_t>& parent, std::int64_t idx) {
    while (idx >= 0 && static_cast<std::size_t>(idx) < parent.size() &&
           parent[static_cast<std::size_t>(idx)] >= 0 &&
           parent[static_cast<std::size_t>(idx)] != idx) {
        idx = parent[static_cast<std::size_t>(idx)];
    }
    return idx;
}

TypeId ConstraintSystem::find_var(TypeId id) {
    if (!reg_.is_var(id) || id.index >= parent_.size())
        return id;
    auto idx = static_cast<std::size_t>(id.index);
    // Uninitialized variable (parent = -1) — not yet used in any unification
    if (parent_[idx] < 0)
        return TypeId{static_cast<std::uint32_t>(idx), id.generation};
    auto p = static_cast<std::int64_t>(idx);
    // Path compression: find root
    while (static_cast<std::size_t>(p) < parent_.size() &&
           parent_[static_cast<std::size_t>(p)] >= 0 &&
           static_cast<std::size_t>(parent_[static_cast<std::size_t>(p)]) !=
               static_cast<std::size_t>(p)) {
        p = parent_[static_cast<std::size_t>(p)];
    }
    auto root = static_cast<std::size_t>(p);
    // Compress path: make all nodes on the path point directly to root
    auto q = static_cast<std::int64_t>(idx);
    while (static_cast<std::size_t>(q) < parent_.size() &&
           parent_[static_cast<std::size_t>(q)] >= 0 &&
           static_cast<std::size_t>(parent_[static_cast<std::size_t>(q)]) !=
               static_cast<std::size_t>(q)) {
        auto next = parent_[static_cast<std::size_t>(q)];
        parent_[static_cast<std::size_t>(q)] = static_cast<std::int64_t>(root);
        q = next;
    }
    // If root has a binding, return the bound type (concrete type or root variable)
    if (root < binding_.size() && binding_[root].valid())
        return binding_[root];
    // Return the root as a type variable
    return TypeId{static_cast<std::uint32_t>(root), id.generation};
}

// Find with full type resolution (normalize via Union-Find)
TypeId ConstraintSystem::find(TypeId id) {
    if (reg_.is_var(id) && id.index < parent_.size() && parent_[id.index] != -1) {
        auto found = find_var(id);
        if (found != id)
            return find(found);
        return found;
    }
    // Recurse into compound types
    if (auto* f = reg_.func_of(id)) {
        bool changed = false;
        std::vector<TypeId> new_args;
        for (auto& a : f->args) {
            auto na = find(a);
            new_args.push_back(na);
            if (na != a)
                changed = true;
        }
        auto new_ret = find(f->ret);
        if (new_ret != f->ret)
            changed = true;
        if (changed)
            return reg_.register_func(std::move(new_args), new_ret);
    }
    if (auto* ft = reg_.forall_of(id)) {
        auto new_body = find(ft->body);
        if (new_body != ft->body)
            return reg_.register_forall(ft->var, new_body);
    }
    return id;
}

TypeId ConstraintSystem::normalize(TypeId id) {
    return find(id);
}

bool ConstraintSystem::occurs_check(TypeId var, TypeId ty) {
    if (!reg_.is_var(var))
        return false;
    ty = find(ty);
    if (var == ty)
        return true;
    if (auto* f = reg_.func_of(ty)) {
        for (auto a : f->args)
            if (occurs_check(var, a))
                return true;
        return occurs_check(var, f->ret);
    }
    return false;
}

bool ConstraintSystem::unify(TypeId t1, TypeId t2) {
    t1 = find(t1);
    t2 = find(t2);
    if (t1 == t2)
        return true;

    // Assign variable to type
    if (reg_.is_var(t1)) {
        if (occurs_check(t1, t2))
            return false;
        auto idx = static_cast<std::size_t>(t1.index);
        if (idx >= parent_.size()) {
            parent_.resize(idx + 64, -1);
            rank_.resize(idx + 64, 0);
            binding_.resize(idx + 64, TypeId{});
        }
        if (parent_[idx] < 0)
            parent_[idx] = static_cast<std::int64_t>(idx);
        if (!reg_.is_var(t2)) {
            // Bind variable to concrete type
            binding_[idx] = t2;
        } else {
            auto idx2 = static_cast<std::size_t>(t2.index);
            if (idx2 >= parent_.size()) {
                parent_.resize(idx2 + 64, -1);
                rank_.resize(idx2 + 64, 0);
                binding_.resize(idx2 + 64, TypeId{});
            }
            if (parent_[idx2] < 0)
                parent_[idx2] = static_cast<std::int64_t>(idx2);
            auto r1 = idx;
            auto r2 = idx2;
            // Find roots via find_var for proper path compression
            auto f1 = find_var(t1);
            auto f2 = find_var(t2);
            if (reg_.is_var(f1))
                r1 = static_cast<std::size_t>(f1.index);
            if (reg_.is_var(f2))
                r2 = static_cast<std::size_t>(f2.index);
            if (r1 == r2)
                return true;
            if (rank_[r1] < rank_[r2]) {
                std::swap(r1, r2);
                std::swap(idx, idx2);
            }
            parent_[r2] = static_cast<std::int64_t>(r1);
            if (rank_[r1] == rank_[r2])
                rank_[r1]++;
            // Merge bindings: if r2 had a binding, move to r1
            if (binding_[r2].valid()) {
                if (!binding_[r1].valid()) {
                    binding_[r1] = binding_[r2];
                } else if (binding_[r1] != binding_[r2]) {
                    return false; // conflicting bindings
                }
            }
        }
        return true;
    }
    if (reg_.is_var(t2))
        return unify(t2, t1);

    // Function type decomposition
    auto* f1 = reg_.func_of(t1);
    auto* f2 = reg_.func_of(t2);
    if (f1 && f2) {
        if (f1->args.size() != f2->args.size())
            return false;
        for (std::size_t i = 0; i < f1->args.size(); i++)
            if (!unify(f1->args[i], f2->args[i]))
                return false;
        return unify(f1->ret, f2->ret);
    }

    // Nominal equality for non-variable, non-function types
    return t1 == t2;
}

bool ConstraintSystem::consistent_unify(TypeId t1, TypeId t2) {
    t1 = find(t1);
    t2 = find(t2);

    // Any consistent with everything (sound gradual core)
    if (t1 == reg_.dynamic_type() || t2 == reg_.dynamic_type())
        return true;

    // Nominal equality
    if (t1 == t2)
        return true;

    // Strict unify for variables (via Union-Find)
    if (reg_.is_var(t1) || reg_.is_var(t2))
        return unify(t1, t2);

    // Function type decomposition (consistent)
    // Uses consistent_subtype with proper variance:
    //   (-> T1 T2) ~ (-> T1' T2')   when   T1' <:sub T1  AND  T2 <:sub T2'
    // i.e. parameter contravariance, return covariance
    auto* f1 = reg_.func_of(t1);
    auto* f2 = reg_.func_of(t2);
    if (f1 && f2) {
        if (f1->args.size() != f2->args.size())
            return false;
        // Parameter contravariance: T2_i <:sub T1_i
        for (std::size_t i = 0; i < f1->args.size(); i++)
            if (!consistent_subtype(f2->args[i], f1->args[i]))
                return false;
        // Return covariance: T1_ret <:sub T2_ret
        return consistent_subtype(f1->ret, f2->ret);
    }

    // Ground type consistency: any two ground/base types are CONSISTENT
    // (they may need runtime coercion, but the type system allows it)
    if (!reg_.is_var(t1) && !reg_.is_var(t2) && !f1 && !f2) {
        return true;
    }

    return false;
}

bool ConstraintSystem::consistent_subtype(TypeId sub, TypeId sup) {
    sub = find(sub);
    sup = find(sup);

    // Any is the top type: everything is a subtype of Any
    // (including Any itself — reflexivity)
    if (sup == reg_.dynamic_type())
        return true;
    // Nothing is a subtype of a non-Any ground type if sub is Any
    // (Any <: Int fails — insert runtime check instead)
    if (sub == reg_.dynamic_type() && sup != reg_.dynamic_type())
        return true; // consistent_subtype allows it (runtime coercion)

    // Reflexivity
    if (sub == sup)
        return true;

    // Type variables: unify them (consistent assignment)
    if (reg_.is_var(sub) || reg_.is_var(sup))
        return unify(sub, sup);

    // Function subtype with variance
    auto* f_sub = reg_.func_of(sub);
    auto* f_sup = reg_.func_of(sup);
    if (f_sub && f_sup) {
        if (f_sub->args.size() != f_sup->args.size())
            return false;
        // Parameter contravariance: sup.args[k] <:sub sub.args[k]
        for (std::size_t i = 0; i < f_sub->args.size(); i++)
            if (!consistent_subtype(f_sup->args[i], f_sub->args[i]))
                return false;
        // Return covariance: sub.ret <:sub sup.ret
        return consistent_subtype(f_sub->ret, f_sup->ret);
    }

    // Non-function, non-variable ground types are consistent
    // (runtime coercion applies)
    return true;
}

bool ConstraintSystem::solve() {
    // Worklist: process all constraints, then re-process until fixpoint
    std::vector<std::size_t> worklist;
    for (std::size_t i = 0; i < constraints_.size(); i++)
        worklist.push_back(i);

    std::size_t max_passes = 10; // prevent infinite loops
    while (!worklist.empty() && max_passes-- > 0) {
        auto current = std::move(worklist);
        worklist.clear();
        for (auto idx : current) {
            auto& c = constraints_[idx];
            bool ok;
            if (c.kind == Constraint::EQUAL)
                ok = unify(c.lhs, c.rhs);
            else
                ok = consistent_unify(c.lhs, c.rhs);
            if (!ok)
                return false;
            // Re-check remaining constraints if we resolved new variables
        }
        // Re-add constraints whose variables were just resolved
        // In Union-Find, unification is persistent, so no need to re-check
        // unless there's a specific need. For completeness, we check if
        // any constraint's variables now resolve differently.
    }
    return true;
}

void ConstraintSystem::clear() {
    constraints_.clear();
    parent_.assign(parent_.size(), -1);
    rank_.assign(rank_.size(), 0);
    binding_.assign(binding_.size(), TypeId{});
    fresh_counter_ = 0;
    first_free_var_ = 0;
} // ═══════════════════════════════════════════════════════════
// InferenceEngine
// ═══════════════════════════════════════════════════════════

InferenceEngine::InferenceEngine(TypeRegistry& reg, DiagnosticCollector& diag)
    : reg_(reg)
    , diag_(diag)
    , cs_(reg)
    , env_(reg) {
    init_primitive_env();
}


bool InferenceEngine::is_coercible(TypeId from, TypeId to) {
    if (from == to)
        return true;
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
    // Float ↔ Int (numeric coercion)
    if ((from_tag == TypeTag::FLOAT && to_tag == TypeTag::INT) ||
        (from_tag == TypeTag::INT && to_tag == TypeTag::FLOAT))
        return true;
    // Float ↔ String
    if ((from_tag == TypeTag::FLOAT && to_tag == TypeTag::STRING) ||
        (from_tag == TypeTag::STRING && to_tag == TypeTag::FLOAT))
        return true;
    // Float ↔ Bool
    if ((from_tag == TypeTag::FLOAT && to_tag == TypeTag::BOOL) ||
        (from_tag == TypeTag::BOOL && to_tag == TypeTag::FLOAT))
        return true;
    return false;
}

void InferenceEngine::register_primitive(std::string name, std::vector<TypeId> param_types,
                                         TypeId ret_type) {
    auto func_type = reg_.register_func(std::move(param_types), ret_type);
    env_.bind(std::move(name), func_type);
}

void InferenceEngine::register_poly_primitive(std::string name,
                                               std::vector<TypeId> param_types,
                                               TypeId ret_type,
                                               std::vector<TypeId> type_vars) {
    auto func_type = reg_.register_func(std::move(param_types), ret_type);
    for (auto it = type_vars.rbegin(); it != type_vars.rend(); ++it)
        func_type = reg_.register_forall(*it, func_type);
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

    // Arithmetic: (Number, Number) -> Number (Int or Float promotion)
    register_primitive("+", {Dyn, Dyn}, Dyn);
    register_primitive("-", {Dyn, Dyn}, Dyn);
    register_primitive("*", {Dyn, Dyn}, Dyn);
    register_primitive("/", {Dyn, Dyn}, Dyn);

    // Comparison: (Number, Number) -> Bool
    register_primitive("=", {Dyn, Dyn}, Bool);
    register_primitive("<", {Dyn, Dyn}, Bool);
    register_primitive(">", {Dyn, Dyn}, Bool);
    register_primitive("<=", {Dyn, Dyn}, Bool);
    register_primitive(">=", {Dyn, Dyn}, Bool);

    // Boolean logic: runtime #t/#f are lexed as Int 0/1, so
    // truthiness-checking ops work on any value.
    // and/or are variadic — minimal signature uses 2 args
    register_primitive("and", {Dyn, Dyn}, Dyn);
    register_primitive("or", {Dyn, Dyn}, Dyn);

    // not: works on any truthy/falsy value (runtime: a[0] == 0 → 1)
    register_primitive("not", {Dyn}, Bool);
    register_primitive("eq?", {Dyn, Dyn}, Bool);

    // Type predicates return Bool
    register_primitive("number?", {Dyn}, Bool);
    register_primitive("string?", {Dyn}, Bool);
    register_primitive("boolean?", {Dyn}, Bool);
    register_primitive("null?", {Dyn}, Bool);
    register_primitive("pair?", {Dyn}, Bool);
    register_primitive("procedure?", {Dyn}, Bool);
    register_primitive("list?", {Dyn}, Bool);
    register_primitive("equal?", {Dyn, Dyn}, Bool);

    // String operations
    register_primitive("string-append", {String, String}, String);
    register_primitive("string-length", {String}, Int);
    register_primitive("string-ref", {String, Int}, Int);
    register_primitive("substring", {String, Int, Int}, String);
    register_primitive("string=?", {String, String}, Bool);
    register_primitive("string<?", {String, String}, Bool);
    register_primitive("number->string", {Int}, String);
    register_primitive("string->number", {String}, Int);

    // Pair operations
    register_primitive("cons", {Dyn, Dyn}, Dyn);
    register_primitive("car", {Dyn}, Dyn);
    register_primitive("cdr", {Dyn}, Dyn);
    // car/cdr with polymorphic pair types for ADT match
    {
        auto a = cs_.fresh_var();
        auto b = cs_.fresh_var();
        auto pair_type = reg_.register_func({a}, b);
        auto car_type = reg_.register_func({pair_type}, a);
        env_.bind("car", reg_.register_forall(a, reg_.register_forall(b, car_type)));
    }
    {
        auto a = cs_.fresh_var();
        auto b = cs_.fresh_var();
        auto pair_type = reg_.register_func({a}, b);
        auto cdr_type = reg_.register_func({pair_type}, b);
        env_.bind("cdr", reg_.register_forall(a, reg_.register_forall(b, cdr_type)));
    }

    // Cadr/Caddr shorthands
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

    // Mutable pair operations
    register_primitive("set-car!", {Dyn, Dyn}, Void);
    register_primitive("set-cdr!", {Dyn, Dyn}, Void);

    // List operations
    register_primitive("list", {Dyn}, Dyn); // varargs — minimal
    register_primitive("length", {Dyn}, Int);
    register_primitive("list-ref", {Dyn, Int}, Dyn);
    register_primitive("member", {Dyn, Dyn}, Dyn);
    register_primitive("append", {Dyn, Dyn}, Dyn);
    register_primitive("reverse", {Dyn}, Dyn);
    register_primitive("take", {Int, Dyn}, Dyn);
    register_primitive("drop", {Int, Dyn}, Dyn);
    register_primitive("foldl", {Dyn, Dyn, Dyn}, Dyn);
    // Polymorphic map/filter: ∀a b. ((a -> b), list a) -> b
    // The list types are approximated as Any for now (no proper List type).
    // The function contract (a→b) enforces type consistency between args and results.
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

    // I/O
    register_primitive("display", {Dyn}, Void);
    register_primitive("write", {Dyn}, Void);
    register_primitive("newline", {}, Void);
    register_primitive("error", {Dyn}, Void);
    register_primitive("assert", {Dyn}, Void);

    // Introspection
    register_primitive("type-of", {Dyn}, reg_.type_type());
    register_primitive("type?", {Dyn, String}, Bool);

    // Misc
    register_primitive("read", {}, String);
    register_primitive("read-file", {String}, String);
    register_primitive("load-module", {String}, Dyn);
    register_primitive("import", {String}, Dyn);
    register_primitive("write-file", {String, String}, Void);
    register_primitive("file-exists?", {String}, Bool);
    register_primitive("gensym", {}, String);

    // Typed mutation operators (runtime-only, minimal type info)
    register_primitive("mutate:replace-type", {Dyn, String}, Dyn);
    register_primitive("mutate:record-patch", {Dyn, String, String}, Dyn);
    register_primitive("mutation-count", {}, Int);
    register_primitive("mutation-history", {Dyn}, Dyn);
    // Vector primitives
    register_primitive("vector", {Dyn}, Vector); // varargs — minimal
    register_primitive("vector-ref", {Vector, Int}, Dyn);
    register_primitive("vector-set!", {Vector, Int, Dyn}, Void);
    register_primitive("vector-length", {Vector}, Int);
    register_primitive("vector?", {Dyn}, Bool); // already Bool
    register_primitive("make-vector", {Int, Dyn}, Vector);
    // List<->Vector conversion

    // Hash primitives
    register_primitive("hash", {Dyn}, Hash);
    register_primitive("hash-ref", {Hash, Dyn}, Dyn);
    register_primitive("hash-set!", {Hash, Dyn, Dyn}, Void);
    register_primitive("hash-length", {Hash}, Int);
    register_primitive("hash-keys", {Hash}, Dyn);
    register_primitive("hash-values", {Hash}, Dyn);
    register_primitive("hash?", {Dyn}, Bool);
    register_primitive("hash-remove!", {Hash, Dyn}, Bool);
    register_primitive("hash-has-key?", {Hash, Dyn}, Bool);

    // Numeric extension primitives
    register_primitive("modulo", {Int, Int}, Int);
    register_primitive("quotient", {Int, Int}, Int);
    register_primitive("remainder", {Int, Int}, Int);
    register_primitive("abs", {Int}, Int);
    register_primitive("gcd", {Int, Int}, Int);
    register_primitive("lcm", {Int, Int}, Int);
    register_primitive("min", {Dyn, Dyn}, Dyn);
    register_primitive("max", {Dyn, Dyn}, Dyn);

    // Character primitives
    register_primitive("char?", {Dyn}, Bool);
    register_primitive("char->integer", {Dyn}, Int);
    register_primitive("integer->char", {Int}, Int);
    register_primitive("string->list", {String}, Dyn);
    register_primitive("list->string", {Dyn}, String);
    register_primitive("read-line", {}, String);
    register_primitive("eof-object?", {Dyn}, Bool);

    // Additional type predicates
    register_primitive("integer?", {Dyn}, Bool);
    register_primitive("float?", {Dyn}, Bool);

    // Missing list/vector conversions
    register_primitive("list->vector", {Dyn}, Vector);
    register_primitive("vector->list", {Vector}, Dyn);

    // ── Stdlib type signatures ────────────────────────────
    // Generic type parameters for polymorphic stdlib functions
    auto _a = reg_.make_var("a");
    auto _b = reg_.make_var("b");
    auto _c = reg_.make_var("c");
    auto _d = reg_.make_var("d");
    auto _num = reg_.make_var("num");

    // std/list: (a -> b) -> (list a) -> (list b)
    register_poly_primitive("map", {reg_.register_func({_a}, _b), reg_.register_func({}, Dyn)}, Dyn, {_a, _b});
    register_poly_primitive("filter", {reg_.register_func({_a}, Bool), reg_.register_func({}, Dyn)}, Dyn, {_a});
    register_poly_primitive("foldl", {reg_.register_func({_a, _b}, _b), _b, reg_.register_func({}, Dyn)}, _b, {_a, _b});
    register_poly_primitive("range", {Int, Int}, Dyn, {});
    register_poly_primitive("length", {Dyn}, Int, {});
    register_poly_primitive("reverse", {Dyn}, Dyn, {_a});
    register_poly_primitive("zip", {Dyn, Dyn}, Dyn, {_a, _b});
    register_poly_primitive("take", {Int, Dyn}, Dyn, {_a});
    register_poly_primitive("drop", {Int, Dyn}, Dyn, {_a});
    register_poly_primitive("flatten", {Dyn}, Dyn, {_a});
    register_poly_primitive("partition", {reg_.register_func({_a}, Bool), Dyn}, Dyn, {_a});
    register_poly_primitive("sort", {Dyn, reg_.register_func({_a, _a}, Bool)}, Dyn, {_a});
    register_poly_primitive("append", {Dyn, Dyn}, Dyn, {_a});
    register_poly_primitive("member", {Dyn, Dyn}, Bool, {_a});

    // std/string
    register_primitive("string-split", {String, String}, Dyn);
    register_primitive("string-trim", {String}, String);
    register_primitive("string-join", {Dyn, String}, String);

    // std/hash
    register_primitive("hash-keys", {Dyn}, Dyn);
    register_primitive("hash-values", {Dyn}, Dyn);
    register_primitive("hash-ref", {Dyn, Dyn}, Dyn);
    register_primitive("hash-has-key?", {Dyn, Dyn}, Bool);
    register_primitive("hash-set!", {Dyn, Dyn, Dyn}, Void);
    register_primitive("hash-length", {Dyn}, Int);
    register_primitive("hash-count", {Dyn}, Int);

    // std/iter
    register_primitive("for-each", {reg_.register_func({_a}, Dyn), Dyn}, Void);
    register_primitive("for", {Dyn, reg_.register_func({_a}, Dyn)}, Void);

    // std/math
    register_poly_primitive("square", {_num}, _num, {_num});
    register_poly_primitive("sqrt", {_num}, _num, {_num});
    register_primitive("pi", {}, Float);
    register_poly_primitive("abs", {_num}, _num, {_num});
    register_poly_primitive("min", {_num, _num}, _num, {_num});
    register_poly_primitive("max", {_num, _num}, _num, {_num});
    register_primitive("sin", {_num}, _num);
    register_primitive("cos", {_num}, _num);
    register_primitive("tan", {_num}, _num);
    register_primitive("floor", {_num}, _num);
    register_primitive("ceil", {_num}, _num);
    register_primitive("round", {_num}, _num);
    register_primitive("exp", {_num}, _num);
    register_primitive("log", {_num}, _num);
    register_primitive("pow", {_num, _num}, _num);
    register_primitive("rand", {}, Float);
    register_primitive("rand-int", {Int}, Int);
    register_primitive("mean", {Dyn}, _num);
    register_primitive("median", {Dyn}, _num);
    register_primitive("stddev", {Dyn}, _num);
    register_primitive("sum", {Dyn}, Int);
    register_primitive("product", {Dyn}, Int);
    register_primitive("factorial", {Int}, Int);

    // std/io
    register_primitive("file-exists?", {String}, Bool);
    register_primitive("file-size", {String}, Int);
    register_primitive("file-copy", {String, String}, Bool);
    register_primitive("file-delete", {String}, Bool);
    register_primitive("file-read", {String}, String);
    register_primitive("file-write", {String, String}, Void);
    register_primitive("file->string", {String}, String);
    register_primitive("string->file", {String, String}, Void);
    register_primitive("file->lines", {String}, Dyn);

    // std/data (trie)
    register_primitive("make-trie", {}, Dyn);
    register_primitive("trie-insert", {Dyn, String}, Dyn);
    register_primitive("trie-search", {Dyn, String}, Bool);
    register_primitive("trie-prefix?", {Dyn, String}, Bool);
    register_primitive("trie-keys", {Dyn}, Dyn);

    // std/csv
    register_primitive("csv-parse", {String}, Dyn);
    register_primitive("csv->rows", {String}, Dyn);
    register_primitive("csv->table", {String}, Dyn);
    register_primitive("csv-select", {Dyn, Dyn}, Dyn);
    register_primitive("csv-filter", {reg_.register_func({Dyn}, Bool), Dyn}, Dyn);
    register_primitive("csv-header", {Dyn}, Dyn);
    register_primitive("column-names", {Dyn}, Dyn);

    // std/json
    register_primitive("json-parse", {String}, Dyn);
    register_primitive("json-stringify", {Dyn}, String);
    register_primitive("json-value", {Dyn}, String);
    register_primitive("json-arr-items", {Dyn, reg_.register_func({Dyn}, Dyn)}, Dyn);
    register_primitive("json-obj-items", {Dyn, Dyn, reg_.register_func({Dyn}, Dyn)}, Dyn);

    // std/socket
    register_primitive("tcp-connect", {String, Int}, Dyn);
    register_primitive("tcp-send", {Dyn, String}, Void);
    register_primitive("tcp-recv", {Dyn}, String);
    register_primitive("tcp-close", {Dyn}, Void);

    // std/algorithm
    register_poly_primitive("sorted?", {Dyn, reg_.register_func({_a, _a}, Bool)}, Bool, {_a});
    register_poly_primitive("sort-by", {Dyn, reg_.register_func({_a}, _b)}, Dyn, {_a, _b});
    register_poly_primitive("sort-stable", {Dyn, reg_.register_func({_a, _a}, Bool)}, Dyn, {_a});
    register_poly_primitive("unique", {Dyn}, Dyn, {_a});
    register_poly_primitive("min-by", {Dyn, reg_.register_func({_a}, _b)}, Dyn, {_a, _b});
    register_poly_primitive("max-by", {Dyn, reg_.register_func({_a}, _b)}, Dyn, {_a, _b});
    register_poly_primitive("permutations", {Dyn}, Dyn, {_a});
    register_poly_primitive("merge-sorted", {Dyn, Dyn, reg_.register_func({_a, _a}, Bool)}, Dyn, {_a});

    // std/combinators
    register_poly_primitive("identity", {_a}, _a, {_a});
    register_poly_primitive("const", {_a, _b}, _a, {_a, _b});
    register_poly_primitive("flip", {reg_.register_func({_a, _b}, _c)}, reg_.register_func({_b, _a}, _c), {_a, _b, _c});
    register_poly_primitive("compose", {reg_.register_func({_b}, _c), reg_.register_func({_a}, _b)}, reg_.register_func({_a}, _c), {_a, _b, _c});
    register_poly_primitive("complement", {reg_.register_func({_a}, Bool)}, reg_.register_func({_a}, Bool), {_a});

    // std/datetime
    register_primitive("timestamp", {}, Int);
    register_primitive("timestamp->year", {Int}, Int);
    register_primitive("timestamp->month", {Int}, Int);
    register_primitive("timestamp->day", {Int}, Int);
    register_primitive("timestamp->hour", {Int}, Int);
    register_primitive("timestamp->minute", {Int}, Int);
    register_primitive("leap-year?", {Int}, Bool);
    register_primitive("days-in-month", {Int, Int}, Int);

    // std/random
    register_primitive("make-random", {}, Dyn);
    register_primitive("random-next", {Dyn}, Dyn);
    register_primitive("random-integer", {Dyn}, Int);
    register_primitive("random-float", {Dyn}, Float);
    register_primitive("random-range", {Int, Int, Dyn}, Int);
    register_primitive("shuffle", {Dyn, Dyn}, Dyn);

    // std/set
    register_primitive("set", {Dyn}, Dyn);
    register_primitive("set-add", {Dyn, Dyn}, Dyn);
    register_primitive("set-remove", {Dyn, Dyn}, Dyn);
    register_primitive("set-member?", {Dyn, Dyn}, Bool);
    register_primitive("set-empty?", {Dyn}, Bool);
    register_primitive("set-union", {Dyn, Dyn}, Dyn);
    register_primitive("set-intersect", {Dyn, Dyn}, Dyn);
    register_primitive("set-difference", {Dyn, Dyn}, Dyn);
    register_primitive("set->list", {Dyn}, Dyn);
    register_primitive("list->set", {Dyn}, Dyn);
    register_primitive("set-size", {Dyn}, Int);
    register_primitive("set-subset?", {Dyn, Dyn}, Bool);
    register_primitive("set-equal?", {Dyn, Dyn}, Bool);

    // std/queue
    register_primitive("make-queue", {}, Dyn);
    register_primitive("enqueue", {Dyn, Dyn}, Dyn);
    register_primitive("dequeue", {Dyn}, Dyn);
    register_primitive("queue-front", {Dyn}, Dyn);
    register_primitive("queue-rest", {Dyn}, Dyn);
    register_primitive("queue-empty?", {Dyn}, Bool);
    register_primitive("queue-length", {Dyn}, Int);
    register_primitive("queue->list", {Dyn}, Dyn);
    register_primitive("list->queue", {Dyn}, Dyn);

    // std/stack
    register_primitive("make-stack", {}, Dyn);
    register_primitive("stack-push", {Dyn, Dyn}, Dyn);
    register_primitive("stack-pop", {Dyn}, Dyn);
    register_primitive("stack-top", {Dyn}, Dyn);
    register_primitive("stack-empty?", {Dyn}, Bool);
    register_primitive("stack-length", {Dyn}, Int);
    register_primitive("stack->list", {Dyn}, Dyn);
    register_primitive("list->stack", {Dyn}, Dyn);

    // std/evolve
    register_primitive("evolve-strategy", {String}, Void);
}

TypeId InferenceEngine::lub(TypeId a, TypeId b) {
    if (a == b)
        return a;
    if (a == reg_.dynamic_type() || b == reg_.dynamic_type())
        return reg_.dynamic_type();
    // Int → Float promotion
    if ((a == reg_.int_type() && b == reg_.lookup_type("Float")) ||
        (a == reg_.lookup_type("Float") && b == reg_.int_type()))
        return reg_.lookup_type("Float");
    return reg_.dynamic_type(); // safe fallback
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

static std::optional<OccurrenceInfoFlat> analyze_predicate_flat(const FlatAST& flat,
                                                                const StringPool& pool,
                                                                NodeId cond_id, TypeRegistry& reg) {
    auto cond = flat.get(cond_id);
    if (cond.tag != NodeTag::Call || cond.children.empty())
        return std::nullopt;

    auto fn_id = cond.child(0);
    auto fn = flat.get(fn_id);

    // Check for (not p)
    if (fn.tag == NodeTag::Variable) {
        auto fn_name = pool.resolve(fn.sym_id);
        if (fn_name == "not" && cond.children.size() >= 2) {
            auto inner = analyze_predicate_flat(flat, pool, cond.child(1), reg);
            if (inner) {
                inner->is_negation = !inner->is_negation;
                return inner;
            }
            return std::nullopt;
        }

        // Check for (and p1 p2) — combine predicates for the same variable
        if (fn_name == "and") {
            std::optional<OccurrenceInfoFlat> result;
            for (std::size_t i = 1; i < cond.children.size(); i++) {
                auto inner = analyze_predicate_flat(flat, pool, cond.child(i), reg);
                if (inner) {
                    if (!result) {
                        result = inner;
                    } else if (inner->var_name == result->var_name) {
                        // Same variable: combine types via lub
                        auto combined = reg.register_func(
                            {result->refined_type, inner->refined_type}, reg.void_type());
                        result->refined_type = combined;
                    }
                }
            }
            return result;
        }

        // Check for (or p1 p2) — return first found (conservative: then-branch unknowns)
        if (fn_name == "or") {
            for (std::size_t i = 1; i < cond.children.size(); i++) {
                auto inner = analyze_predicate_flat(flat, pool, cond.child(i), reg);
                if (inner)
                    return inner;
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
                    return OccurrenceInfoFlat{std::string(pool.resolve(var_node.sym_id)), type_id};
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
                else if (fn_name == "number?" || fn_name == "integer?")
                    return OccurrenceInfoFlat{std::string(var_name), reg.int_type()};
                else if (fn_name == "boolean?")
                    return OccurrenceInfoFlat{std::string(var_name), reg.bool_type()};
                else if (fn_name == "null?" || fn_name == "void?")
                    return OccurrenceInfoFlat{std::string(var_name), reg.void_type()};
                else if (fn_name == "pair?")
                    return OccurrenceInfoFlat{
                        std::string(var_name),
                        reg.register_func({reg.dynamic_type()}, reg.dynamic_type())};
                else if (fn_name == "symbol?")
                    return OccurrenceInfoFlat{std::string(var_name), reg.dynamic_type()};
                else if (fn_name == "float?")
                    return OccurrenceInfoFlat{std::string(var_name), reg.lookup_type("Float")};
                else if (fn_name == "hash?")
                    return OccurrenceInfoFlat{std::string(var_name), reg.dynamic_type()};
                else if (fn_name == "procedure?")
                    return OccurrenceInfoFlat{std::string(var_name), reg.dynamic_type()};
            }
        }

    }

    return std::nullopt;
}

TypeId InferenceEngine::infer_flat(FlatAST& flat, StringPool& pool, NodeId id) {
    if (id == NULL_NODE || id >= flat.size())
        return reg_.dynamic_type();
    cs_.clear();
    auto result = synthesize_flat(flat, pool, id, flat.get(id));
    if (!cs_.solve()) {
        diag_.report(Diagnostic(ErrorKind::TypeError, "type constraint solving failed", cur_loc_)
                         .with_blame(BlameInfo{BlameParty::Implicit, "", "compile"}));
    }
    return cs_.normalize(result);
}

TypeId InferenceEngine::synthesize_flat(FlatAST& flat, StringPool& pool, NodeId id, NodeView v) {
    cur_loc_ = {v.line, v.col, 0};

    // Incremental: if node is clean AND has cached type, return cached result.
    // Dirty propagation ensures ancestors of mutated nodes are marked dirty,
    // so clean nodes' cached types remain valid.
    if (!flat.is_dirty(id)) {
        auto cached = flat.type_id(id);
        if (cached > 0 && cached < reg_.size()) {
            return TypeId{cached, 1};
        }
        // Clean but not cached (first run or newly inserted node): fall through
    }

    TypeId result;
    using Tag = NodeTag;
    switch (v.tag) {
        case Tag::LiteralInt:
            result = (v.marker == SyntaxMarker::BoolLiteral) ? reg_.bool_type() : reg_.int_type();
            break;
        case Tag::LiteralFloat:
            result = reg_.lookup_type("Float");
            break;
        case Tag::LiteralString:
            result = reg_.string_type();
            break;
        case Tag::Variable:
            result = synthesize_flat_var(pool, v);
            break;
        case Tag::Call:
            result = synthesize_flat_call(flat, pool, v);
            break;
        case Tag::IfExpr:
            result = synthesize_flat_if(flat, pool, v);
            break;
        case Tag::Lambda:
            result = synthesize_flat_lambda(flat, pool, v);
            break;
        case Tag::Let:
            result = synthesize_flat_let(flat, pool, id, v, false);
            break;
        case Tag::LetRec:
            result = synthesize_flat_let(flat, pool, id, v, true);
            break;
        case Tag::Begin:
            result = synthesize_flat_begin(flat, pool, v);
            break;
        case Tag::TypeAnnotation:
            result = synthesize_flat_annotation(flat, pool, v);
            break;
        case Tag::Coercion:
            result = synthesize_flat(flat, pool, v.child(0), flat.get(v.child(0)));
            break;
        case Tag::Linear:
            // (Linear e): wrap type as (Linear T) for ownership tracking
            if (!v.children.empty()) {
                auto inner_type = synthesize_flat(flat, pool, v.child(0), flat.get(v.child(0)));
                result = reg_.register_linear(inner_type);
            } else {
                result = reg_.dynamic_type();
            }
            break;
        case Tag::Move: {
            // (move e): check ownership, mark Moved, same type
            TypeId inner_type = reg_.void_type();
            if (!v.children.empty()) {
                auto inner_v = flat.get(v.child(0));
                if (inner_v.tag == NodeTag::Variable) {
                    auto var_name = std::string(pool.resolve(inner_v.sym_id));
                    auto var_ty = env_.lookup(var_name);
                    bool is_linear = var_ty.valid() && reg_.linear_of(var_ty) != nullptr;
                    if (is_linear) {
                        if (!ownership_env_.can_move(var_name)) {
                            auto st = ownership_env_.get(var_name);
                            auto msg = "cannot move " + var_name + " — " +
                                       ownership_env_.state_name(st);
                            diag_.report(Diagnostic(ErrorKind::TypeError, msg, cur_loc_));
                        }
                        ownership_env_.mark(var_name, OwnershipState::Moved);
                    }
                }
                inner_type = synthesize_flat(flat, pool, v.child(0), flat.get(v.child(0)));
            }
            result = inner_type;
            break;
        }
        case Tag::Borrow: {
            // (& e): immutable borrow, mark Borrowed
            TypeId inner_type = reg_.void_type();
            if (!v.children.empty()) {
                auto inner_v = flat.get(v.child(0));
                if (inner_v.tag == NodeTag::Variable) {
                    auto var_name = std::string(pool.resolve(inner_v.sym_id));
                    auto var_ty = env_.lookup(var_name);
                    bool is_linear = var_ty.valid() && reg_.linear_of(var_ty) != nullptr;
                    if (is_linear) {
                        if (!ownership_env_.can_borrow(var_name)) {
                            auto st = ownership_env_.get(var_name);
                            auto msg = "cannot borrow " + var_name + " — " +
                                       ownership_env_.state_name(st);
                            diag_.report(Diagnostic(ErrorKind::TypeError, msg, cur_loc_));
                        }
                        ownership_env_.mark(var_name, OwnershipState::Borrowed);
                    }
                }
                inner_type = synthesize_flat(flat, pool, v.child(0), flat.get(v.child(0)));
            }
            result = inner_type;
            break;
        }
        case Tag::MutBorrow: {
            // (&mut e): mutable borrow, exclusive access
            TypeId inner_type = reg_.void_type();
            if (!v.children.empty()) {
                auto inner_v = flat.get(v.child(0));
                if (inner_v.tag == NodeTag::Variable) {
                    auto var_name = std::string(pool.resolve(inner_v.sym_id));
                    auto var_ty = env_.lookup(var_name);
                    bool is_linear = var_ty.valid() && reg_.linear_of(var_ty) != nullptr;
                    if (is_linear) {
                        if (!ownership_env_.can_mut_borrow(var_name)) {
                            auto st = ownership_env_.get(var_name);
                            auto msg = "cannot mutably borrow " + var_name + " — " +
                                       ownership_env_.state_name(st);
                            diag_.report(Diagnostic(ErrorKind::TypeError, msg, cur_loc_));
                        }
                        ownership_env_.mark(var_name, OwnershipState::MutBorrowed);
                    }
                }
                inner_type = synthesize_flat(flat, pool, v.child(0), flat.get(v.child(0)));
            }
            result = inner_type;
            break;
        }
        case Tag::Drop:
            // (drop e): consume inner, return Void
            if (!v.children.empty()) {
                auto inner_v = flat.get(v.child(0));
                if (inner_v.tag == NodeTag::Variable) {
                    auto var_name = std::string(pool.resolve(inner_v.sym_id));
                    auto var_ty = env_.lookup(var_name);
                    bool is_linear = var_ty.valid() && reg_.linear_of(var_ty) != nullptr;
                    if (is_linear) {
                        if (!ownership_env_.can_drop(var_name)) {
                            auto st = ownership_env_.get(var_name);
                            auto msg = "cannot drop " + var_name + " — " +
                                       ownership_env_.state_name(st);
                            diag_.report(Diagnostic(ErrorKind::TypeError, msg, cur_loc_));
                        }
                        ownership_env_.mark(var_name, OwnershipState::Moved);
                    }
                }
                synthesize_flat(flat, pool, v.child(0), flat.get(v.child(0)));
            }
            result = reg_.void_type();
            break;
        case Tag::DefineType: {
            // (define-type (Name params...) (Ctor fields...) ...)
            // Register the type and bind constructors with proper function types.
            auto type_name = std::string(pool.resolve(v.sym_id));

            // Create type variables for type parameters
            std::vector<TypeId> type_params;
            for (auto pid : v.params) {
                auto pname = std::string(pool.resolve(pid));
                auto tv = cs_.fresh_var();
                type_params.push_back(tv);
            }

            // Create the variant type itself (parametric if needed)
            // For now, use the registry to create a named type entry
            TypeId variant_type;
            if (type_params.empty()) {
                // Look up or create concrete variant type
                variant_type = reg_.lookup_type(type_name);
                if (!variant_type.valid()) {
                    variant_type = reg_.register_type(aura::core::TypeTag::VARIANT, type_name);
                }
            } else if (type_params.size() == 1) {
                // Single-param type: use the type var as return marker
                // Forall instantiation will propagate the concrete type
                variant_type = type_params[0];
            } else {
                // Multi-param: use first param as marker for now
                variant_type = type_params[0];
            }

            // Register each constructor with field types
            for (auto cid : v.children) {
                if (cid >= flat.size())
                    continue;
                auto cv = flat.get(cid);
                if (cv.tag != aura::ast::NodeTag::Quote || cv.children.empty())
                    continue;
                auto quoted = cv.child(0);
                if (quoted >= flat.size())
                    continue;

                // Extract constructor name and field types from the quoted list
                // Format: (cons 'ctor-name (cons 'ft1 (cons 'ft2 ...)))
                std::string ctor_name;
                std::vector<TypeId> field_types;

                auto walk = quoted;
                while (walk < flat.size()) {
                    auto nv = flat.get(walk);
                    if (nv.tag != aura::ast::NodeTag::Pair)
                        break;
                    auto car_id = nv.child(0);
                    auto cdr_id = nv.child(1);
                    if (car_id >= flat.size())
                        break;

                    auto car_v = flat.get(car_id);
                    if (car_v.tag == aura::ast::NodeTag::Variable && ctor_name.empty()) {
                        // First element is constructor name
                        ctor_name = std::string(pool.resolve(car_v.sym_id));
                    } else if (car_v.tag == aura::ast::NodeTag::Variable) {
                        // Field type name — look up or create a type variable
                        auto ft_name = std::string(pool.resolve(car_v.sym_id));
                        // Check if it's a type parameter
                        bool is_param = false;
                        for (std::size_t pi = 0; pi < v.params.size(); ++pi) {
                            auto pname = std::string(pool.resolve(v.params[pi]));
                            if (pname == ft_name) {
                                field_types.push_back(type_params[pi]);
                                is_param = true;
                                break;
                            }
                        }
                        if (!is_param) {
                            // Look up built-in type name
                            auto ftid = reg_.lookup_type(ft_name);
                            if (ftid.valid())
                                field_types.push_back(ftid);
                            else
                                field_types.push_back(reg_.dynamic_type());
                        }
                    }
                    walk = cdr_id;
                }

                if (ctor_name.empty())
                    continue;

                // Record constructor for match exhaustiveness checking
                auto tid = reg_.lookup_type(type_name);
                if (tid.valid()) {
                    // Collect all constructors for this ADT
                    auto existing = reg_.get_adt_constructors(tid);
                    if (!existing || std::find(existing->begin(), existing->end(),
                                                ctor_name) == existing->end()) {
                        auto ctors = existing ? *existing : std::vector<std::string>{};
                        ctors.push_back(ctor_name);
                        reg_.register_adt_constructors(tid, ctors);
                    }
                }

                // Build constructor type: (field-type-1 ... -> variant-type)
                TypeId ctor_type;
                if (field_types.empty()) {
                    // No fields: (-> variant-type)
                    ctor_type = reg_.register_func({}, variant_type);
                } else if (field_types.size() == 1) {
                    // Single field: (field-type -> variant-type)
                    ctor_type = reg_.register_func(field_types, variant_type);
                } else {
                    // Multiple fields: nested functions (field1 -> (field2 -> ... (-> variant-type)))
                    TypeId rest = variant_type;
                    for (auto it = field_types.rbegin(); it != field_types.rend(); ++it)
                        rest = reg_.register_func({*it}, rest);
                    ctor_type = rest;
                }

                // Wrap in forall for polymorphic types (e.g. ∀a. (a -> Option a))
                if (!type_params.empty()) {
                    TypeId poly_type = ctor_type;
                    // Build nested forall from last to first
                    for (auto it = type_params.rbegin(); it != type_params.rend(); ++it) {
                        poly_type = reg_.register_forall(*it, poly_type);
                    }
                    env_.bind(ctor_name, poly_type);
                } else {
                    env_.bind(ctor_name, ctor_type);
                }
            }
            result = reg_.void_type();
            break;
        }
        case Tag::Define:
            result = reg_.void_type();
            break;
        case Tag::Set:
            result = reg_.void_type();
            break;
        case Tag::Quote:
            result = reg_.dynamic_type();
            break;
        case Tag::MacroDef: {
            env_.push_scope();
            std::vector<TypeId> param_types;
            for (auto pid : v.params) {
                auto pname = std::string(pool.resolve(pid));
                auto pv = cs_.fresh_var();
                env_.bind(pname, pv);
                param_types.push_back(pv);
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
            result = reg_.dynamic_type();
            break;
    }

    // Cache result for future incremental calls
    flat.set_type(id, result.index);
    return result;
}

TypeId InferenceEngine::synthesize_flat_var(StringPool& pool, NodeView v) {
    auto name = pool.resolve(v.sym_id);
    if (name.empty()) {
        diag_.report(Diagnostic(ErrorKind::UnboundVariable, "(empty name)", cur_loc_));
        return reg_.dynamic_type();
    }
    std::string var_name(name);
    auto ty_raw = env_.lookup(var_name);
    if (!ty_raw.valid()) {
        // Collect candidate names from environment for "did you mean" suggestion
        std::vector<std::string> candidates;
        env_.collect_names(candidates);
        auto best = closest_match(var_name, candidates);
        auto d = Diagnostic(
            ErrorKind::UnboundVariable, var_name, cur_loc_)
            .with_suggestion(!best.empty()
                ? "did you mean '" + best + "'?"
                : "");
        diag_.report(std::move(d));
        return reg_.dynamic_type();
    }
    // M4 ownership: checked explicitly in Move/Borrow/Drop handlers
    // Fully instantiate forall types (peel all ∀ layers) with fresh variables
    auto instantiate_all = [&](this const auto& self, TypeId tid) -> TypeId {
        auto* ft = reg_.forall_of(tid);
        if (!ft)
            return tid;
        auto inst = reg_.instantiate(tid, [this]() { return cs_.fresh_var(); });
        return self(inst);
    };
    if (reg_.forall_of(ty_raw)) {
        return instantiate_all(ty_raw);
    }
    return ty_raw;
}

TypeId InferenceEngine::synthesize_flat_call(FlatAST& flat, StringPool& pool, NodeView v) {
    // v.child(0) = function, v.child(1..n) = args
    if (v.children.empty())
        return reg_.dynamic_type();

    auto func_id = v.child(0);
    TypeId func_type = synthesize_flat(flat, pool, func_id, flat.get(func_id));

    // Special inference for arithmetic primitives: constrain return via arg types.
    // This gives us (Int, Int) -> Int inference inside lambdas where args are type vars,
    // and (Float, x) -> Float promotion without losing specificity to Dyn.
    auto infer_arith = [&]() -> std::optional<TypeId> {
        auto callee_of_func = flat.get(func_id);
        if (callee_of_func.tag != NodeTag::Variable || callee_of_func.sym_id == INVALID_SYM)
            return std::nullopt;
        auto fname = pool.resolve(callee_of_func.sym_id);
        static const std::unordered_set<std::string> arith = {"+", "-", "*", "/"};
        if (!arith.count(std::string(fname)))
            return std::nullopt;

        // Variadic arith with 0 or 1 args
        // (+) → Int, (+ x) → type of x (or Int if unknown)
        if (v.children.size() < 3) {
            if (v.children.size() == 2) {
                auto t0 = synthesize_flat(flat, pool, v.child(1), flat.get(v.child(1)));
                t0 = cs_.normalize(t0);
                if (!reg_.is_var(t0))
                    return t0;
            }
            return reg_.int_type();
        }

        // Synthesize arg types (pure lookup for variables, no side effects)
        TypeId t0 = synthesize_flat(flat, pool, v.child(1), flat.get(v.child(1)));
        TypeId t1 = synthesize_flat(flat, pool, v.child(2), flat.get(v.child(2)));
        t0 = cs_.normalize(t0);
        t1 = cs_.normalize(t1);
        auto tag0 = reg_.tag_of(t0);
        auto tag1 = reg_.tag_of(t1);

        // Both concrete: return the wider type
        if (tag0 == TypeTag::INT && tag1 == TypeTag::INT)
            return reg_.int_type();
        if (tag0 == TypeTag::FLOAT && tag1 == TypeTag::FLOAT)
            return reg_.lookup_type("Float");
        if ((tag0 == TypeTag::INT && tag1 == TypeTag::FLOAT) ||
            (tag0 == TypeTag::FLOAT && tag1 == TypeTag::INT)) {
            // Coerce Int to Float
            if (tag0 == TypeTag::INT)
                cs_.consistent_unify(t0, reg_.lookup_type("Float"));
            if (tag1 == TypeTag::INT)
                cs_.consistent_unify(t1, reg_.lookup_type("Float"));
            return reg_.lookup_type("Float");
        }

        // Both concrete but not INT/FLOAT: runtime will coerce to numeric
        // e.g., (+ "42" 1) → String coerce to Int at runtime
        if (!reg_.is_var(t0) && !reg_.is_var(t1)) {
            // Return Int for arithmetic (runtime handles coercion)
            return reg_.int_type();
        }

        // At least one is a type variable: create a fresh result var and constrain
        auto result = cs_.fresh_var();
        cs_.consistent_unify(t0, result);
        cs_.consistent_unify(t1, result);
        return result;
    };
    if (auto arith_result = infer_arith()) {
        // Mark args as checked (synthesize_flat already processed them)
        return *arith_result;
    }

    // Instantiate Forall types before extracting function signature.
    // This is needed for Let-Polymorphism: (let ((f (lambda (x) ...))) (f 42))
    // where f's type is generalized to ∀t. (t -> ret)
    auto instantiate_all_direct = [&](this const auto& self, TypeId tid) -> TypeId {
        auto* ft = reg_.forall_of(tid);
        if (!ft)
            return tid;
        auto inst = reg_.instantiate(tid, [this]() { return cs_.fresh_var(); });
        return self(inst);
    };
    func_type = instantiate_all_direct(func_type);

    // COPY func type before processing args — synthesize_flat may call
    // register_func which can reallocate entries_, invalidating func_of* pointers.
    std::optional<FuncType> f_ty_copy;
    if (auto* ft = reg_.func_of(func_type))
        f_ty_copy = *ft;

    if (f_ty_copy) {
        auto& ft = *f_ty_copy;
        auto saved_loc = cur_loc_;
        std::size_t n_expected =
            std::min(ft.args.size(), v.children.size() > 1 ? v.children.size() - 1 : 0);
        for (std::size_t i = 0; i < n_expected; i++) {
            auto arg_id = v.child(i + 1);
            TypeId arg_type = synthesize_flat(flat, pool, arg_id, flat.get(arg_id));
            if (!cs_.consistent_unify(arg_type, ft.args[i])) {
                if (is_coercible(arg_type, ft.args[i])) {
                    auto msg = std::string("argument ") + std::to_string(i) + ": coercion from " +
                               std::string(reg_.format_type(arg_type)) + " to " +
                               std::string(reg_.format_type(ft.args[i]));
                    diag_.report(Diagnostic(ErrorKind::Note, std::move(msg), saved_loc));
                } else {
                    auto msg = std::string("argument ") + std::to_string(i) + ": expected " +
                               std::string(reg_.format_type(ft.args[i])) + ", got " +
                               std::string(reg_.format_type(arg_type));
                    diag_.report(Diagnostic(ErrorKind::TypeError, std::move(msg), saved_loc)
                                     .with_blame(BlameInfo{BlameParty::Caller, "", "compile"}));
                }
            }
        }
        std::size_t num_args = v.children.size() > 1 ? v.children.size() - 1 : 0;
        // Skip arity check for variadic primitives
        bool is_variadic = false;
        auto callee_v = flat.get(func_id);
        if (callee_v.sym_id != INVALID_SYM && callee_v.tag == NodeTag::Variable) {
            auto cname = pool.resolve(callee_v.sym_id);
            is_variadic =
                (cname == "and" || cname == "or" || cname == "list" || cname == "vector" ||
                 cname == "hash" || cname == "+" || cname == "-" || cname == "*" || cname == "/" ||
                 cname == "=" || cname == "<" || cname == ">" || cname == "<=" || cname == ">=");
        }
        if (num_args != ft.args.size() && !ft.args.empty() && !is_variadic) {
            auto msg = std::string("call '") + std::string(pool.resolve(callee_v.sym_id)) +
                       "': expected " + std::to_string(ft.args.size()) + " arguments, got " +
                       std::to_string(num_args);
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
    ownership_env_.push_scope();
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
    if (v.children.empty())
        return reg_.void_type();

    auto cond_id = v.child(0);
    check_flat(flat, pool, cond_id, reg_.bool_type());

    if (v.children.size() < 2)
        return reg_.void_type();
    auto then_id = v.child(1);
    if (then_id == NULL_NODE)
        return reg_.void_type();

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
        ownership_env_.pop_scope();
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

// Value restriction (design §13.4 T-Let-Poly-Gradual):
// In gradual context, only syntactic values are generalized.
// Non-value lets (calls, if, etc.) stay monomorphic to avoid
// type pollution from cast/Any interactions.
static bool is_syntactic_value(NodeId id, const FlatAST& flat) {
    auto v = flat.get(id);
    switch (v.tag) {
        case NodeTag::LiteralInt:
        case NodeTag::LiteralFloat:
        case NodeTag::LiteralString:
        case NodeTag::Variable:
        case NodeTag::Lambda:
        case NodeTag::Quote:
            return true;
        case NodeTag::TypeAnnotation:
            // (: x T) — check inner
            if (!v.children.empty())
                return is_syntactic_value(v.child(0), flat);
            return false;
        case NodeTag::Coercion:
            // (cast e T) — check inner
            if (!v.children.empty())
                return is_syntactic_value(v.child(0), flat);
            return false;
        default:
            return false;
    }
}

TypeId InferenceEngine::synthesize_flat_let(FlatAST& flat, StringPool& pool,
                                            aura::ast::NodeId node_id,
                                            NodeView v, bool is_rec) {
    // children: 0=value, 1=body, name from v.sym_id

    // If is_rec, the binding is visible in the value expression too
    auto name = pool.resolve(v.sym_id);
    std::string var_name(name);

    if (is_rec) {
        env_.push_scope();
        ownership_env_.push_scope();
        // Bind name to a fresh type variable (forward reference)
        TypeId fwd_var = cs_.fresh_var();
        env_.bind(var_name, fwd_var);

        // Evaluate the value expression with the binding in scope
        TypeId val_type = reg_.void_type();
        if (!v.children.empty() && v.child(0) != NULL_NODE)
            val_type = synthesize_flat(flat, pool, v.child(0), flat.get(v.child(0)));

        // Unify forward reference with the actual value type
        cs_.consistent_unify(fwd_var, val_type);

        // Body type (fact is now resolved via the type variable)
        TypeId body_type = reg_.void_type();
        if (v.children.size() >= 2 && v.child(1) != NULL_NODE)
            body_type = synthesize_flat(flat, pool, v.child(1), flat.get(v.child(1)));
        ownership_env_.pop_scope();
        env_.pop_scope();
        return body_type;
    }

    env_.push_scope();
    ownership_env_.push_scope();
    TypeId val_type = reg_.void_type();
    if (!v.children.empty() && v.child(0) != NULL_NODE)
        val_type = synthesize_flat(flat, pool, v.child(0), flat.get(v.child(0)));

    // Value Restriction (design §13.4):
    // Only generalize syntactic values. Non-value lets (calls, if, etc.)
    // stay monomorphic to prevent type pollution from cast/Any.
    auto val_norm = cs_.normalize(val_type);
    if (is_syntactic_value(v.child(0), flat)) {
        auto fvs = reg_.free_vars(val_norm);
        if (!fvs.empty()) {
            // Let-Polymorphism: generalize over free type variables
            TypeId poly = val_norm;
            for (auto& fv_id : fvs) {
                poly = reg_.register_forall(fv_id, poly);
            }
            env_.bind(var_name, poly);
        } else {
            env_.bind(var_name, val_norm);
        }
    } else {
        // Non-syntactic value: bind monomorphically (no generalization)
        env_.bind(var_name, val_norm);
    }

    // ── Match exhaustiveness check — recursive ADT support ──
    // Detect match on ADT by checking if let name is __match_tmp
    // and the value is a constructor call.
    auto let_name = std::string(pool.resolve(v.sym_id));
    if (let_name == "__match_tmp" && !v.children.empty()) {
        auto* scan_minfo = flat.get_match_info(node_id);

        // Scan TypeRegistry for ADTs
        for (std::size_t i = 0; i < reg_.size(); ++i) {
            auto tid = TypeId{static_cast<std::uint32_t>(i), 1};
            auto* ctors = reg_.get_adt_constructors(tid);
            if (!ctors) continue;

            auto type_name = std::string(reg_.name_of(tid));

            // Helper: recursively collect missing constructors with depth limit
            // to support recursive ADTs like (List a) → (Nil) (Cons a List)
            std::vector<std::string> missing;
            auto check_adt_recursive =
                [&](this const auto& self, const std::vector<std::string>& ctor_names,
                    const std::vector<aura::ast::SymId>& used_sym,
                    std::string_view context_name, int depth) -> void {
                if (depth <= 0 || ctor_names.empty())
                    return;
                for (auto& cname : ctor_names) {
                    auto found = std::find_if(
                        used_sym.begin(), used_sym.end(),
                        [&](SymId sid) {
                            return pool.resolve(sid) == cname;
                        });
                    if (found == used_sym.end()) {
                        missing.push_back(cname);
                    }
                }
            };

            // Run recursive check (depth limit = 3 for nested ADTs)
            if (scan_minfo && !scan_minfo->has_wildcard) {
                std::vector<aura::ast::SymId> used = scan_minfo->used_constructors;
                check_adt_recursive(
                    *ctors, used, type_name, 3);

                // Report all missing constructors
                if (!missing.empty()) {
                    std::string msg = "match: ";
                    if (missing.size() == 1) {
                        msg += "missing constructor '" + missing[0] + "'";
                    } else {
                        msg += "missing constructors: ";
                        for (std::size_t mi = 0; mi < missing.size(); ++mi) {
                            if (mi > 0) msg += ", ";
                            msg += "'" + missing[mi] + "'";
                        }
                    }
                    msg += " in " + type_name;
                    // Add fix-it suggestion (chained on temporary, no self-move)
                    if (missing.size() == 1) {
                        diag_.report(Diagnostic(ErrorKind::TypeError, msg, cur_loc_)
                            .with_suggestion(
                                "add clause for '" + missing[0] + "' pattern"));
                    } else {
                        std::string suggest = "add clauses for ";
                        for (std::size_t mi = 0; mi < missing.size(); ++mi) {
                            if (mi > 0) suggest += ", ";
                            suggest += "'" + missing[mi] + "'";
                        }
                        diag_.report(Diagnostic(ErrorKind::TypeError, msg, cur_loc_)
                            .with_suggestion(suggest));
                    }
                }
            }

            // Emit a note for the ADT being matched
            diag_.report(Diagnostic(ErrorKind::Note,
                "match on '" + type_name +
                "' (" + std::to_string(ctors->size()) + " constructors)",
                cur_loc_));
            break; // Only process the first ADT found
        }
    }

    TypeId body_type = reg_.void_type();
    if (v.children.size() >= 2 && v.child(1) != NULL_NODE)
        body_type = synthesize_flat(flat, pool, v.child(1), flat.get(v.child(1)));
    ownership_env_.pop_scope();
    env_.pop_scope();
    return body_type;
}

TypeId InferenceEngine::synthesize_flat_begin(FlatAST& flat, StringPool& pool, NodeView v) {
    if (v.children.empty())
        return reg_.void_type();
    TypeId last = reg_.void_type();
    for (auto child_id : v.children)
        last = synthesize_flat(flat, pool, child_id, flat.get(child_id));
    return last;
}

TypeId InferenceEngine::synthesize_flat_annotation(FlatAST& flat, StringPool& pool, NodeView v) {
    // child(0) = inner_expr, sym_id = type name string
    if (v.children.empty())
        return reg_.dynamic_type();
    auto inner_id = v.child(0);
    TypeId inner_type = synthesize_flat(flat, pool, inner_id, flat.get(inner_id));

    auto type_name = pool.resolve(v.sym_id);
    if (!type_name.empty()) {
        auto expected = reg_.lookup_type(std::string(type_name));
        if (!expected.valid()) {
            diag_.report(
                Diagnostic(ErrorKind::TypeError, "unknown type: " + std::string(type_name),
                           cur_loc_)
                    .with_blame(BlameInfo{BlameParty::Annotation,
                                          "(: ... " + std::string(type_name) + ")", "compile"}));
        } else {
            check_flat(flat, pool, inner_id, expected);
        }
    }
    return inner_type;
}

void InferenceEngine::check_flat(FlatAST& flat, StringPool& pool, NodeId id, TypeId expected) {
    if (id == NULL_NODE || id >= flat.size())
        return;
    auto v = flat.get(id);
    cur_loc_ = {v.line, v.col, 0};

    if (v.tag == NodeTag::Call)
        check_flat_call(flat, pool, v, expected);
    else if (v.tag == NodeTag::Lambda)
        check_flat_lambda(flat, pool, v, expected);
    else if (v.tag == NodeTag::IfExpr) {
        // If in check mode: check condition is Bool, check both branches
        // against expected, and unify them
        if (v.children.size() < 2)
            return;
        auto cond_id = v.child(0);
        auto then_id = v.child(1);
        // Condition must be Bool
        auto cond_type = synthesize_flat(flat, pool, cond_id, flat.get(cond_id));
        cs_.consistent_unify(cond_type, reg_.bool_type());
        // Check both branches against expected
        check_flat(flat, pool, then_id, expected);
        if (v.children.size() >= 3 && v.child(2) != NULL_NODE)
            check_flat(flat, pool, v.child(2), expected);
    } else if (v.tag == NodeTag::Let || v.tag == NodeTag::LetRec) {
        // Let in check mode: check value, then check body against expected
        bool is_rec = (v.tag == NodeTag::LetRec);
        auto name = pool.resolve(v.sym_id);
        std::string var_name(name);
        env_.push_scope();
        ownership_env_.push_scope();
        if (!v.children.empty() && v.child(0) != NULL_NODE) {
            auto val_id = v.child(0);
            // Check the value expression against expected if annotated
            // For now: synthesize val, bind it
            TypeId val_type =
                is_rec ? cs_.fresh_var() : synthesize_flat(flat, pool, val_id, flat.get(val_id));
            env_.bind(var_name, val_type);
        }
        if (v.children.size() >= 2 && v.child(1) != NULL_NODE)
            check_flat(flat, pool, v.child(1), expected);
        ownership_env_.pop_scope();
        env_.pop_scope();
    } else if (v.tag == NodeTag::Begin) {
        // Begin in check mode: check last expression against expected,
        // synthesize all others for side effects
        for (std::size_t i = 0; i < v.children.size(); ++i) {
            auto child_id = v.child(i);
            if (child_id == NULL_NODE)
                continue;
            if (i + 1 == v.children.size()) {
                check_flat(flat, pool, child_id, expected);
            } else {
                synthesize_flat(flat, pool, child_id, flat.get(child_id));
            }
        }
    } else if (v.tag == NodeTag::TypeAnnotation) {
        // Annotation in check mode: check inner against expected,
        // then check inner against annotation type
        if (v.children.empty())
            return;
        auto inner_id = v.child(0);
        auto type_name = pool.resolve(v.sym_id);
        if (!type_name.empty()) {
            auto ann_type = reg_.lookup_type(std::string(type_name));
            if (ann_type.valid()) {
                // Check inner against both annotation type and expected
                if (reg_.is_subtype(ann_type, expected) ||
                    cs_.consistent_unify(ann_type, expected)) {
                    // Annotation type is compatible with expected: check inner against ann_type
                    check_flat(flat, pool, inner_id, ann_type);
                } else {
                    // Annotation conflicts with expected context
                    auto msg = "annotation type " + std::string(reg_.format_type(ann_type)) +
                               " conflicts with context expecting " +
                               std::string(reg_.format_type(expected));
                    diag_.report(Diagnostic(ErrorKind::TypeError, std::move(msg), cur_loc_)
                                     .with_blame(BlameInfo{BlameParty::Annotation, "", "compile"}));
                }
            } else {
                synthesize_flat(flat, pool, inner_id, flat.get(inner_id));
            }
        } else {
            synthesize_flat(flat, pool, inner_id, flat.get(inner_id));
        }
    } else if (v.tag == NodeTag::Set) {
        // (set! var value): synthesize the value, unify with var's type
        if (v.children.size() >= 1 && v.child(0) != NULL_NODE) {
            auto val_id = v.child(0);
            auto val_type = synthesize_flat(flat, pool, val_id, flat.get(val_id));
            // Look up variable type from env
            auto var_name = std::string(pool.resolve(v.sym_id));
            auto var_type = env_.lookup(var_name);
            if (var_type.valid()) {
                cs_.consistent_unify(val_type, var_type);
            }
            // Also unify with expected context
            cs_.consistent_unify(val_type, expected);
        }
    } else if (v.tag == NodeTag::Define) {
        // (define name value): check value against expected if matched
        if (v.children.size() >= 1 && v.child(0) != NULL_NODE) {
            auto val_id = v.child(0);
            auto val_type = synthesize_flat(flat, pool, val_id, flat.get(val_id));
            // For define, check that value type is consistent with expected context
            // (define is a declaration, not an expression, so the expected context
            //  is about the defined value, not the define node itself)
        }
        // Define returns Void — no check against expected needed
    } else {
        TypeId inferred = synthesize_flat(flat, pool, id, v);
        if (!cs_.consistent_unify(inferred, expected)) {
            if (is_coercible(inferred, expected)) {
                auto msg = "coercion from " + std::string(reg_.format_type(inferred)) + " to " +
                           std::string(reg_.format_type(expected));
                diag_.report(Diagnostic(ErrorKind::Note, std::move(msg), cur_loc_));
            } else {
                auto msg = "type mismatch: expected " + std::string(reg_.format_type(expected)) +
                           ", got " + std::string(reg_.format_type(inferred));
                diag_.report(Diagnostic(ErrorKind::TypeError, std::move(msg), cur_loc_)
                                 .with_blame(BlameInfo{BlameParty::Annotation, "", "compile"}));
            }
        }
    }
}

void InferenceEngine::check_flat_call(FlatAST& flat, StringPool& pool, NodeView v,
                                      TypeId expected) {
    // Synthesize the call's type normally, then check against expected
    TypeId inferred = synthesize_flat_call(flat, pool, v);
    if (!cs_.consistent_unify(inferred, expected)) {
        if (is_coercible(inferred, expected)) {
            auto msg = "call return type: coercion from " +
                       std::string(reg_.format_type(inferred)) + " to " +
                       std::string(reg_.format_type(expected));
            diag_.report(Diagnostic(ErrorKind::Note, std::move(msg), cur_loc_));
        } else {
            auto msg = "call return type mismatch: expected " +
                       std::string(reg_.format_type(expected)) + ", got " +
                       std::string(reg_.format_type(inferred));
            diag_.report(Diagnostic(ErrorKind::TypeError, std::move(msg), cur_loc_)
                             .with_blame(BlameInfo{BlameParty::Caller, "", "compile"}));
        }
    }
}

void InferenceEngine::check_flat_lambda(FlatAST& flat, StringPool& pool, NodeView v,
                                        TypeId expected) {
    auto* f_ty = reg_.func_of(expected);
    if (!f_ty) {
        diag_.report(Diagnostic(ErrorKind::TypeError,
                                "expected a function type but got " +
                                    std::string(reg_.format_type(expected)),
                                cur_loc_)
                         .with_blame(BlameInfo{BlameParty::Annotation, "", "compile"}));
        return;
    }
    if (f_ty->args.size() != v.params.size()) {
        diag_.report(Diagnostic(ErrorKind::ArityMismatch,
                                "lambda expects " + std::to_string(v.params.size()) +
                                    " parameters but context provides " +
                                    std::to_string(f_ty->args.size()),
                                cur_loc_));
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

TypeId TypeChecker::infer_flat(FlatAST& flat, StringPool& pool, NodeId node,
                               DiagnosticCollector& diag) {
    InferenceEngine engine(types, diag);
    return engine.infer_flat(flat, pool, node);
}

} // namespace aura::compiler
