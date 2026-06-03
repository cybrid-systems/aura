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

// ── Gradual typing: type_tag for CoercionNode ────────────
// Maps types to coercion tags used by CastOp at runtime.
// 0=Int, 1=String, 2=Bool, 3=Dynamic, 4=Float
static std::uint32_t type_tag_for_coercion(aura::core::TypeId tid,
                                           const aura::core::TypeRegistry* type_reg) {
    if (!type_reg)
        return 3;
    auto tag = type_reg->tag_of(tid);
    switch (tag) {
        case aura::core::TypeTag::INT:
            return 0;
        case aura::core::TypeTag::STRING:
            return 1;
        case aura::core::TypeTag::BOOL:
            return 2;
        case aura::core::TypeTag::FLOAT:
            return 4;
        default:
            return 3; // DYNAMIC / unknown
    }
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
    // Issue #71: auto-detect polymorphism. If the bound type is a
    // Forall wrapper, mark the binding as polymorphic so lookup
    // knows to instantiate it.
    bool is_poly = reg_.forall_of(type) != nullptr;
    scopes_.back()[std::move(name)] = Binding{type, is_poly, {}};
}

TypeId TypeEnv::lookup(const std::string& name) {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
        auto f = it->find(name);
        if (f != it->end()) {
            // Issue #71: let-polymorphism. If the binding is
            // polymorphic, return a fresh instantiation so each
            // use site gets its own copy. (Without this, bound
            // vars would leak across use sites and unification
            // would conflict.)
            if (f->second.is_poly) {
                return reg_.instantiate_forall(f->second.type, {});
            }
            return f->second.type;
        }
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

    // FuncType
    if (auto* f = reg_.func_of(ty)) {
        for (auto a : f->args)
            if (occurs_check(var, a))
                return true;
        return occurs_check(var, f->ret);
    }

    // ForallType
    if (auto* ft = reg_.forall_of(ty)) {
        return occurs_check(var, ft->body);
    }

    // LinearType
    if (auto* lt = reg_.linear_of(ty)) {
        return occurs_check(var, lt->inner);
    }

    // ModuleType
    if (auto* mt = reg_.module_of(ty)) {
        for (auto& [name, t] : mt->members)
            if (occurs_check(var, t))
                return true;
    }

    // VariantType
    if (auto* vt = reg_.variant_of(ty)) {
        for (auto& [name, args] : vt->variants)
            for (auto& a : args)
                if (occurs_check(var, a))
                    return true;
    }

    // RecordType
    if (auto* rt = reg_.record_of(ty)) {
        for (auto& [name, t] : rt->fields)
            if (occurs_check(var, t))
                return true;
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
    if (t1 == reg_.dynamic_type() || t2 == reg_.dynamic_type()) {
        // If one side is Any and the other is a type variable, bind the
        // variable to Any.  This prevents the free var from escaping into
        // let-polymorphism generalization where it would be ∀-quantified,
        // destroying the Any boundary (soundness fix, #18).
        if (reg_.is_var(t1))
            unify(t1, reg_.dynamic_type());
        else if (reg_.is_var(t2))
            unify(t2, reg_.dynamic_type());
        return true;
    }

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
    // Bind declared type sigs (from inject_type_sigs) to the env.
    // We use the explicit name → TypeId map (declared_sigs_) set by
    // TypeChecker::infer_flat, instead of scanning the registry for
    // __decl_ prefix. The old scan had a latent bug exposed by
    // TypeId interning (Issue #70 follow-up #1): when multiple names
    // shared the same TypeId via dedup, the last writer won the name
    // field, so only the last name was bound to the env. (See the
    // 312-5 / test_aura_type_multi_func regression.)
    // The declared_sigs_ map preserves every name → TypeId binding
    // regardless of TypeId sharing.
    // (The __decl_ scan is intentionally removed; the registry
    // names are now purely for formatting / debugging.)
}

void InferenceEngine::bind_declared_sigs() {
    // Bind each declared name to its TypeId in the env. Called by
    // TypeChecker::infer_flat after constructing the engine, so
    // the explicit name → TypeId map (set post-construction) takes
    // effect. (See ctor comment for why we don't scan the registry.)
    for (auto& [name, tid] : declared_sigs_) {
        if (tid.valid() && !name.empty()) {
            env_.bind(name, tid);
        }
    }
}


bool InferenceEngine::is_coercible(TypeId from, TypeId to) {
    if (from == to)
        return true;
    // Dynamic coerce to/from anything (gradual core, always allowed)
    if (from == reg_.dynamic_type() || to == reg_.dynamic_type())
        return true;
    // Issue #79: In strict mode, cross-type coercions are TypeErrors,
    // not silent "Notes" that pass through has_errors() == false. We
    // only allow numeric narrowing (Float → Int) because that's a real
    // number-narrows-to-integer operation, not a stringification.
    if (strict_) {
        auto from_tag = reg_.tag_of(from);
        auto to_tag = reg_.tag_of(to);
        // Float → Int is the only cross-type coercion allowed in strict mode
        if (from_tag == TypeTag::FLOAT && to_tag == TypeTag::INT)
            return true;
        return false;
    }
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

    // ── Capability Effects（#9 Agent OS 安全） ────────────
    auto EffIO = reg_.register_effect("IO");
    auto EffMutation = reg_.register_effect("Mutation");
    auto EffFileRead = reg_.register_effect("FileRead");
    auto EffFileWrite = reg_.register_effect("FileWrite");
    auto EffNetwork = reg_.register_effect("Network");
    auto EffAgentMsg = reg_.register_effect("AgentMsg");
    (void)EffIO; (void)EffMutation; (void)EffFileRead;
    (void)EffFileWrite; (void)EffNetwork; (void)EffAgentMsg;

    // Mutation primitives 带效果标注
    register_primitive("mutate:rebind", {String, String}, Dyn);
    register_primitive("mutate:replace-type", {Int, String}, Dyn);
    register_primitive("mutate:replace-value", {Int, Dyn, String}, Dyn);
    register_primitive("mutate:set-body", {String, String}, Dyn);
    register_primitive("mutate:splice", {Int, Int}, Dyn);
    register_primitive("mutate:wrap", {Int, String}, Dyn);
    register_primitive("mutate:tweak-literal", {Int, Int}, Dyn);

    // IO/网络原语
    register_primitive("load-module", {String}, Dyn);
    register_primitive("write-file", {String, String}, Void);
    register_primitive("read-file", {String}, String);

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
    register_primitive("string-index", {String, String, Int}, Int);
    register_primitive("string->number", {String}, Dyn);
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

    // ── Capability primitives ──
    register_primitive("with-capability", {String, Dyn}, Dyn);
    register_primitive("capability?", {String}, Bool);
    register_primitive("check-capability", {String}, Bool);
    register_primitive("capability-stack", {}, Dyn);
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
                        // Same variable: in the then-branch of (and p1 p2),
                        // the variable satisfies BOTH predicates, so use
                        // the intersection.  If both refine to the same type,
                        // keep it; otherwise conservatively fall back to Any.
                        if (result->refined_type != inner->refined_type)
                            result->refined_type = reg.dynamic_type();
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
                         .with_blame(BlameInfo{BlameParty::Implicit, "", "compile"})
                         .with_suggestion("check for type mismatches in function arguments, return values, or recursive bindings"));
    }
    auto normalized = cs_.normalize(result);
    // Update the root's cached type with the final resolved type after solving.
    // Individual sub-nodes' caches are updated during their synthesize_flat calls.
    // Those may store TYPE_VARs that get resolved here; next incremental pass
    // will re-compute stale TYPE_VAR caches and write back the resolved type.
    flat.set_type(id, normalized.index);
    flat.clear_dirty(id);
    return normalized;
}

TypeId InferenceEngine::synthesize_flat(FlatAST& flat, StringPool& pool, NodeId id, NodeView v) {
    cur_loc_ = {v.line, v.col, 0};

    // Incremental: if node is clean AND has a resolved cached type, return cached result.
    // Dirty propagation ensures ancestors of mutated nodes are marked dirty,
    // so clean nodes' cached types remain valid.
    // Skip TYPE_VAR cache entries — they are stale pre-solve type variables
    // that were cached before constraint solving resolved them.
    if (!flat.is_dirty(id)) {
        auto cached = flat.type_id(id);
        if (cached > 0 && cached < reg_.size()) {
            auto tid = TypeId{cached, 1};
            // Issue #72: also reject cached types that CONTAIN
            // TYPE_VARs (not just types whose top-level tag is
            // TYPE_VAR). Pre-solve cached types often have free
            // vars in polymorphic contexts, and those vars are
            // stale (the union-find has been cleared). free_vars()
            // returning empty means the type is fully resolved.
            if (reg_.free_vars(tid).empty()) {
                ++stats_.cache_hits;
                return tid;
            }
            ++stats_.stale_cache;
        }
        ++stats_.cache_misses;
        // Clean but not cached / stale cache: fall through to recompute
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
        case Tag::Coercion: {
            // CoercionNode: return the target type (not inner type)
            // Inner expression was checked when the CoercionNode was inserted.
            auto target_tid = flat.type_id(id);
            if (target_tid != 0) {
                result = TypeId{target_tid, 1};
            } else {
                // Fallback: synthesize inner (no target type available)
                result = synthesize_flat(flat, pool, v.child(0), flat.get(v.child(0)));
            }
            break;
        }
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
                            // Issue #79: linear-resource violations are
                            // System-level (not Caller) — the resource
                            // state is invariant of the call site.
                            diag_.report(Diagnostic(ErrorKind::TypeError, msg, cur_loc_)
                                             .with_blame(BlameInfo{BlameParty::System, "", "compile"})
                                             .with_suggestion("rebind " + var_name +
                                                              " to a fresh value, or end the active borrow first"));
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
                            diag_.report(Diagnostic(ErrorKind::TypeError, msg, cur_loc_)
                                             .with_blame(BlameInfo{BlameParty::System, "", "compile"})
                                             .with_suggestion("end the active mutable borrow of " + var_name +
                                                              " before taking an immutable borrow"));
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
                            diag_.report(Diagnostic(ErrorKind::TypeError, msg, cur_loc_)
                                             .with_blame(BlameInfo{BlameParty::System, "", "compile"})
                                             .with_suggestion("end any active borrows of " + var_name +
                                                              " before taking a mutable borrow"));
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
                            diag_.report(Diagnostic(ErrorKind::TypeError, msg, cur_loc_)
                                             .with_blame(BlameInfo{BlameParty::System, "", "compile"})
                                             .with_suggestion("end active borrows of " + var_name +
                                                              " before dropping"));
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
            // Always look up or create a named type entry so ADT ctors can be
            // registered against it (for match exhaustiveness checking).
            // For parametric types, we still want a named entry for the ADT
            // itself; the parametric instance is built via Forall.
            auto named_tid = reg_.lookup_type(type_name);
            if (!named_tid.valid()) {
                named_tid = reg_.register_type(aura::core::TypeTag::VARIANT, type_name);
            }
            if (type_params.empty()) {
                // Look up or create concrete variant type
                variant_type = named_tid;
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
        case Tag::Define: {
            auto def_name = pool.resolve(v.sym_id);
            if (!v.children.empty()) {
                auto val_type = synthesize_flat(flat, pool, v.child(0), flat.get(v.child(0)));
                if (def_name.size() > 0)
                    env_.bind(std::string(def_name), val_type);
            }
            result = reg_.void_type();
            break;
        }
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
        case Tag::DefineModule: {
            // (define-module (Name :T ...) body...)
            // Scan body for Define/Export, build a ModuleType with export signatures.
            auto mod_name = pool.resolve(v.sym_id);
            std::vector<std::pair<std::string, TypeId>> members;
            std::unordered_set<std::string> exports;
            std::vector<std::string> type_param_names;
            std::vector<TypeId> type_param_vars;

            // Push scope for type param bindings
            env_.push_scope();
            for (auto sym : v.params) {
                auto pname = std::string(pool.resolve(sym));
                auto tv = cs_.fresh_var();
                type_param_names.push_back(pname);
                type_param_vars.push_back(tv);
                // Bind the type param name as a type-level variable in env,
                // so body function signatures can reference it.
                env_.bind(pname, tv);
            }

            for (auto cid : v.children) {
                auto cv = flat.get(cid);
                if (cv.tag == NodeTag::Define && cv.sym_id != INVALID_SYM) {
                    auto fn_name = std::string(pool.resolve(cv.sym_id));
                    TypeId fn_type = reg_.dynamic_type();
                    if (cv.children.size() > 0) {
                        auto val_id = cv.child(0);
                        fn_type = synthesize_flat(flat, pool, val_id, flat.get(val_id));
                        // Normalize only the return type to resolve constrained type vars
                        // (e.g., + returns Int). Leave param types as-is so type param
                        // substitution can still match their type var IDs.
                        if (auto* ft = reg_.func_of(fn_type)) {
                            auto new_ret = cs_.normalize(ft->ret);
                            fn_type = reg_.register_func(ft->args, new_ret);
                        }
                    }
                    members.push_back({fn_name, fn_type});
                } else if (cv.tag == NodeTag::Export) {
                    for (auto eid : cv.children) {
                        auto ev = flat.get(eid);
                        if (ev.tag == NodeTag::Variable && ev.sym_id != INVALID_SYM)
                            exports.insert(std::string(pool.resolve(ev.sym_id)));
                    }
                }
            }
            env_.pop_scope();

            // Only include exported members in ModuleType
            std::vector<std::pair<std::string, TypeId>> export_members;
            for (auto& [name, ty] : members) {
                if (exports.empty() || exports.count(name))
                    export_members.push_back({name, ty});
            }

            ModuleType mt{std::move(export_members)};
            mt.type_params = std::move(type_param_names);
            mt.type_param_vars = std::move(type_param_vars);
            auto mt_id = reg_.register_module(std::move(mt));
            env_.bind(std::string(mod_name), mt_id);
            result = mt_id;
            break;
        }
        default:
            result = reg_.dynamic_type();
            break;
    }

    // Cache result for future incremental calls.
    // Store the type index even if it's a fresh var — after constraint solving
    // in infer_flat, the root's cache will be updated with the normalized type.
    // The cache read path skips TYPE_VAR entries, so stale vars cause a
    // re-compute which then stores the resolved type.
    flat.set_type(id, result.index);
    flat.clear_dirty(id);
    return result;
}

TypeId InferenceEngine::synthesize_flat_var(StringPool& pool, NodeView v) {
    auto name = pool.resolve(v.sym_id);
    if (name.empty()) {
        diag_.report(Diagnostic(ErrorKind::UnboundVariable, "(empty name)", cur_loc_));
        return reg_.dynamic_type();
    }
    std::string var_name(name);

    // Module member access: name:member → look up module type, extract member type
    auto colon = var_name.find(':');
    if (colon != std::string::npos && colon > 0) {
        auto mod_name = var_name.substr(0, colon);
        auto member_name = var_name.substr(colon + 1);
        auto mod_ty = env_.lookup(mod_name);
        if (mod_ty.valid() && reg_.module_of(mod_ty)) {
            auto* mt = reg_.module_of(mod_ty);
            for (auto& [mname, mtype] : mt->members) {
                if (mname == member_name)
                    return mtype;
            }
            // Member not found in module type — return Dyn and report warning
            // Issue #79: BlameParty::Caller (the wrong member name came from
            // the call site) + closest-match suggestion so AI agents can
            // auto-fix the typo.
            std::vector<std::string> candidates;
            for (auto& [mname, mtype] : mt->members)
                candidates.push_back(mname);
            auto best = closest_match(member_name, candidates);
            std::string sugg = best.empty()
                ? std::string("check the member list of module " + mod_name)
                : ("did you mean '" + mod_name + ":" + best + "'?");
            diag_.report(Diagnostic(ErrorKind::TypeError,
                "no member '" + member_name + "' in module " + mod_name, cur_loc_)
                .with_blame(BlameInfo{BlameParty::Caller, "", "compile"})
                .with_suggestion(std::move(sugg)));
            // Issue #79: would tag the node, but synthesize_flat_var doesn't
            // take a flat reference. The diagnostic carries the source
            // location, which AuraQuery can match against node positions.
            return reg_.dynamic_type();
        }
        // Module not found — fall through to normal variable lookup
    }

    auto ty_raw = env_.lookup(var_name);
    if (!ty_raw.valid()) {
        // Collect candidate names from environment for "did you mean" suggestion
        std::vector<std::string> candidates;
        env_.collect_names(candidates);
        auto best = closest_match(var_name, candidates);

        // 跨模块错误定位：检查是否从 .aura-type 声明了此函数
        std::string mod_info;
        auto mod_it = declared_modules_.find(var_name);
        if (mod_it != declared_modules_.end())
            mod_info = " (from module '" + mod_it->second + "')";

        auto msg = var_name;
        if (!mod_info.empty())
            msg += mod_info;

        auto d = Diagnostic(
            ErrorKind::UnboundVariable, msg, cur_loc_)
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
            // Issue #79: in strict mode, treat two ground types as a mismatch
            // unless they are equal (consistent_unify's gradual-core fallback
            // silently says Int ~ String is OK, which violates strict mode).
            // In non-strict mode, keep the original gradual behavior.
            bool arg_exp_unify = cs_.consistent_unify(arg_type, ft.args[i]);
            if (strict_ && arg_exp_unify) {
                // Verify the unification was for real (same type or Dynamic),
                // not just the "ground types are consistent" fallback.
                auto a_norm = cs_.find(arg_type);
                auto p_norm = cs_.find(ft.args[i]);
                bool dynamic_ok = (a_norm == reg_.dynamic_type() ||
                                    p_norm == reg_.dynamic_type());
                // Issue #79: in strict mode, the only ground-type compatibility
                // is identity. TypeRegistry::type_equals is private; compare
                // by structural format (interned types are canonical, so
                // string equality means the types are the same).
                if (!dynamic_ok &&
                    reg_.format_type(a_norm) != reg_.format_type(p_norm)) {
                    arg_exp_unify = false;
                }
            }
            if (!arg_exp_unify) {
                // Issue #79: tag the offending argument node with the error
                // kind so AuraQuery `(has-error? N)` can find it directly.
                flat.set_node_error(arg_id, static_cast<std::uint8_t>(ErrorKind::TypeError));
                if (is_coercible(arg_type, ft.args[i])) {
                    auto msg = std::string("argument ") + std::to_string(i) + ": coercion from " +
                               std::string(reg_.format_type(arg_type)) + " to " +
                               std::string(reg_.format_type(ft.args[i]));
                    // Issue #79: in non-strict mode this is a Note (gradual);
                    // in strict mode, is_coercible() only allows Float→Int,
                    // which is a real numeric narrowing and gets a CoercionNode.
                    diag_.report(Diagnostic(ErrorKind::Note, std::move(msg), saved_loc)
                                     .with_suggestion("consider adding a type annotation (: arg"
                                                      " " + std::string(reg_.format_type(ft.args[i])) +
                                                      ") to make this static"));
                    // ── Gradual Typing: insert CoercionNode into AST ──
                    // Wraps the argument expression with a CoercionNode that
                    // signals the lowering phase to emit a CastOp at runtime.
                    auto type_tag = type_tag_for_coercion(ft.args[i], &reg_);
                    auto coercion_id = flat.add_coercion(
                        arg_id, type_tag, ft.args[i].index);
                    // Copy source location from the call node for blame tracking
                    flat.set_loc(coercion_id, v.line, v.col);
                    flat.set_child(v.id, static_cast<std::uint32_t>(i + 1), coercion_id);
                } else {
                    auto msg = std::string("argument ") + std::to_string(i) + ": expected " +
                               std::string(reg_.format_type(ft.args[i])) + ", got " +
                               std::string(reg_.format_type(arg_type));
                    diag_.report(Diagnostic(ErrorKind::TypeError, std::move(msg), saved_loc)
                                     .with_blame(BlameInfo{BlameParty::Caller, "", "compile"}));
                }
            } else if (arg_type == reg_.dynamic_type() &&
                       ft.args[i] != reg_.dynamic_type()) {
                // Dynamic → Static: insert CoercionNode for runtime type check
                auto type_tag = type_tag_for_coercion(ft.args[i], &reg_);
                auto coercion_id = flat.add_coercion(
                    arg_id, type_tag, ft.args[i].index);
                flat.set_loc(coercion_id, v.line, v.col);
                flat.set_child(v.id, static_cast<std::uint32_t>(i + 1), coercion_id);
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
            // Issue #79: tag the call node so AuraQuery can find it.
            flat.set_node_error(v.id, static_cast<std::uint8_t>(ErrorKind::ArityMismatch));
        }


        return ft.ret;
    }

    // Module type: functor call (Stack Int) → return the ModuleType
    // With type annotations on lambda params, the member types reference the formal
    // type param vars directly. Substitution replaces them with concrete arg types.
    if (auto* mt = reg_.module_of(func_type)) {
        if (!mt->type_params.empty() && !mt->type_param_vars.empty()) {
            // Infer actual type for each argument
            std::unordered_map<std::uint32_t, TypeId> subst;
            for (std::size_t i = 0; i < mt->type_params.size() && (i + 1) < v.children.size(); ++i) {
                auto arg_id = v.child(i + 1);
                auto arg_v = flat.get(arg_id);
                TypeId arg_type;
                if (arg_v.tag == NodeTag::Variable) {
                    auto type_name = pool.resolve(arg_v.sym_id);
                    auto known = reg_.lookup_type(std::string(type_name));
                    if (known.valid())
                        arg_type = known;
                    else
                        arg_type = synthesize_flat(flat, pool, arg_id, arg_v);
                } else {
                    arg_type = synthesize_flat(flat, pool, arg_id, arg_v);
                }
                subst[mt->type_param_vars[i].index] = arg_type;
            }
            // Substitute type vars in member types using TypeRegistry::substitute
            std::vector<std::pair<std::string, TypeId>> new_members;
            for (auto& [mname, mtype] : mt->members)
                new_members.push_back({mname, reg_.substitute(mtype, subst)});
            ModuleType result_mt{std::move(new_members)};
            return reg_.register_module(std::move(result_mt));
        }
        return func_type;
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
    for (std::size_t pi = 0; pi < v.params.size(); ++pi) {
        auto sym = v.params[pi];
        std::string pname(pool.resolve(sym));
        // Check for type annotation on this parameter
        TypeId param_type;
        if (pi < v.param_annotations.size() && v.param_annotations[pi] != NULL_NODE) {
            auto annot_id = v.param_annotations[pi];
            auto annot_v = flat.get(annot_id);
            if (annot_v.tag == NodeTag::TypeAnnotation) {
                // TypeAnnotation: sym_id = type name (simple) OR child(1) = type expr (compound)
                // Simple type: (: x Int) — type_name = "Int"
                // Compound type: (: s (List :T)) — type_expr_id = child(1)
                auto type_name = pool.resolve(annot_v.sym_id);
                if (!type_name.empty()) {
                    // Simple type name: try registry then env
                    param_type = reg_.lookup_type(std::string(type_name));
                    if (!param_type.valid()) {
                        auto env_ty = env_.lookup(std::string(type_name));
                        if (env_ty.valid())
                            param_type = env_ty;
                    }
                } // compound type annotations (List :T) fall through to fresh_var
            }
        }
        if (!param_type.valid())
            param_type = cs_.fresh_var();
        param_types.push_back(param_type);
        env_.bind(pname, param_type);
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
        ownership_env_.push_scope();
        if (env_.is_bound(occ->var_name))
            env_.bind(occ->var_name, occ->refined_type);
        TypeId then_type = synthesize_flat(flat, pool, then_id, flat.get(then_id));
        ownership_env_.pop_scope();
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
        ownership_env_.push_scope();
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

    // ── Match exhaustiveness check ──
    // Detect match on ADT by checking if let name is __match_tmp.
    // The previous implementation iterated over all ADTs in the registry and
    // picked the first one (bug: incorrect when multiple ADTs are defined).
    // Now we use the actual inferred type of the subject value (val_norm).
    auto let_name = std::string(pool.resolve(v.sym_id));
    if (let_name == "__match_tmp" && !v.children.empty()) {
        auto* scan_minfo = flat.get_match_info(node_id);

        // Wildcard covers everything — nothing to check.
        if (scan_minfo && !scan_minfo->has_wildcard) {
            // Look up the ADT constructors of the actual subject type.
            // val_norm is the normalized type of the value bound to __match_tmp.
            // Common case: subject is already an ADT value (e.g. (Some 42)).
            // Edge case: subject is a constructor function (e.g. (let ((x Red))
            // — then x has type (-> Color) and we want to check the return type.
            TypeId subject_type = val_norm;
            if (reg_.tag_of(subject_type) == TypeTag::FUNC) {
                auto* f = reg_.func_of(subject_type);
                if (f) subject_type = f->ret;
            }
            const std::vector<std::string>* ctors = reg_.get_adt_constructors(subject_type);
            if (!ctors && reg_.tag_of(subject_type) == TypeTag::TYPE_VAR) {
                // Parametric ADT case: the subject type is a type variable that
                // stands in for `List a` etc. ctors aren't directly registered
                // against the type variable, so fall back to scanning the
                // registry for an ADT whose first used_ctor matches one of
                // the constructors we know about.
                for (auto sid : scan_minfo->used_constructors) {
                    auto cname = std::string(pool.resolve(sid));
                    for (std::size_t ti = 0; ti < reg_.size(); ++ti) {
                        auto tid2 = TypeId{static_cast<std::uint32_t>(ti), 1};
                        auto* c2 = reg_.get_adt_constructors(tid2);
                        if (!c2) continue;
                        if (std::find(c2->begin(), c2->end(), cname) != c2->end()) {
                            ctors = c2;
                            subject_type = tid2;
                            break;
                        }
                    }
                    if (ctors) break;
                }
                if (!ctors) {
                    for (auto sid : scan_minfo->candidate_constructors) {
                        auto cname = std::string(pool.resolve(sid));
                        for (std::size_t ti = 0; ti < reg_.size(); ++ti) {
                            auto tid2 = TypeId{static_cast<std::uint32_t>(ti), 1};
                            auto* c2 = reg_.get_adt_constructors(tid2);
                            if (!c2) continue;
                            if (std::find(c2->begin(), c2->end(), cname) != c2->end()) {
                                ctors = c2;
                                subject_type = tid2;
                                break;
                            }
                        }
                        if (ctors) break;
                    }
                }
            }
            if (ctors) {
                // Build the set of effective used constructors. Definite uses
                // (Call patterns) are always counted. Bare-identifier candidates
                // are counted only if they are real constructors of this ADT
                // (so a variable binding like `(let ((x 5)) (match x (a a) ...))`
                // doesn't false-positive on a non-existent `a` constructor).
                std::vector<std::string> used_eff;
                used_eff.reserve(scan_minfo->used_constructors.size() +
                                 scan_minfo->candidate_constructors.size());
                for (auto sid : scan_minfo->used_constructors)
                    used_eff.push_back(std::string(pool.resolve(sid)));
                for (auto sid : scan_minfo->candidate_constructors) {
                    auto cname = std::string(pool.resolve(sid));
                    if (std::find(ctors->begin(), ctors->end(), cname) != ctors->end())
                        used_eff.push_back(std::move(cname));
                }

                // Find missing constructors
                std::vector<std::string> missing;
                for (auto& cname : *ctors) {
                    if (std::find(used_eff.begin(), used_eff.end(), cname) == used_eff.end())
                        missing.push_back(cname);
                }

                if (!missing.empty()) {
                    auto type_name = std::string(reg_.name_of(subject_type));
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
            // If subject isn't an ADT (Int, String, etc.) we don't try to
            // enforce exhaustiveness — non-ADT subjects are typically matched
            // with literal/wildcard patterns and the runtime check is enough.
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
                // ── Gradual Typing: insert CoercionNode ──
                auto type_tag = type_tag_for_coercion(expected, &reg_);
                auto coercion_id = flat.add_coercion(
                    id, type_tag, expected.index);
                // Copy source location from the expression for blame tracking
                auto src_v = flat.get(id);
                flat.set_loc(coercion_id, src_v.line, src_v.col);
                // Update parent's child reference
                auto parent_id = flat.parent_of(id);
                if (parent_id != aura::ast::NULL_NODE) {
                    auto parent_v = flat.get(parent_id);
                    for (std::size_t ci = 0; ci < parent_v.children.size(); ++ci) {
                        if (parent_v.child(static_cast<std::uint32_t>(ci)) == id) {
                            flat.set_child(parent_id, static_cast<std::uint32_t>(ci), coercion_id);
                            break;
                        }
                    }
                }
            } else {
                auto msg = "type mismatch: expected " + std::string(reg_.format_type(expected)) +
                           ", got " + std::string(reg_.format_type(inferred));
                diag_.report(Diagnostic(ErrorKind::TypeError, std::move(msg), cur_loc_)
                                 .with_blame(BlameInfo{BlameParty::Annotation, "", "compile"}));
            }
        } else if (inferred == reg_.dynamic_type() &&
                   expected != reg_.dynamic_type()) {
            // Dynamic → Static boundary: consistent_unify succeeded because
            // DYNAMIC is consistent with everything, but we need a runtime
            // check at the boundary. Insert CoercionNode for CastOp emission.
            auto type_tag = type_tag_for_coercion(expected, &reg_);
            auto coercion_id = flat.add_coercion(
                id, type_tag, expected.index);
            auto src_v = flat.get(id);
            flat.set_loc(coercion_id, src_v.line, src_v.col);
            auto parent_id = flat.parent_of(id);
            if (parent_id != aura::ast::NULL_NODE) {
                auto parent_v = flat.get(parent_id);
                for (std::size_t ci = 0; ci < parent_v.children.size(); ++ci) {
                    if (parent_v.child(static_cast<std::uint32_t>(ci)) == id) {
                        flat.set_child(parent_id, static_cast<std::uint32_t>(ci), coercion_id);
                        break;
                    }
                }
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
            // ── Gradual Typing: wrap the entire call in a CoercionNode ──
            auto type_tag = type_tag_for_coercion(expected, &reg_);
            auto coercion_id = flat.add_coercion(
                v.id, type_tag, expected.index);
            // Copy source location from the call node for blame tracking
            flat.set_loc(coercion_id, v.line, v.col);
            // Replace v.id's reference in parent with the CoercionNode
            auto parent_id = flat.parent_of(v.id);
            if (parent_id != aura::ast::NULL_NODE) {
                auto parent_v = flat.get(parent_id);
                for (std::size_t ci = 0; ci < parent_v.children.size(); ++ci) {
                    if (parent_v.child(static_cast<std::uint32_t>(ci)) == v.id) {
                        flat.set_child(parent_id, static_cast<std::uint32_t>(ci), coercion_id);
                        break;
                    }
                }
            }
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
                         .with_blame(BlameInfo{BlameParty::Annotation, "", "compile"})
                         .with_suggestion("the context annotation does not match a function; check the type annotation or the binding site"));
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

void TypeChecker::inject_type_sigs(
    const std::unordered_map<std::string, std::string>& sigs,
    const std::unordered_map<std::string, std::string>& module_src) {
    auto lookup = [&](const std::string& name) -> TypeId {
        if (name == "Int")    return types.int_type();
        if (name == "Bool")   return types.bool_type();
        if (name == "String") return types.string_type();
        if (name == "Float")  return types.lookup_type("Float");
        if (name == "Void")   return types.void_type();
        if (name == "Any" || name == "Dyn") return types.dynamic_type();
        return types.dynamic_type();
    };
    for (auto& [name, sig] : sigs) {
        auto pipe = sig.find('|');
        if (pipe == std::string::npos) continue;
        std::vector<TypeId> param_types;
        std::istringstream iss(sig.substr(0, pipe));
        std::string tok;
        while (iss >> tok) param_types.push_back(lookup(tok));
        auto tid = types.register_func_named(std::move(param_types),
                                              lookup(sig.substr(pipe + 1)),
                                              "__decl_" + name);
        // Record the name → TypeId mapping so InferenceEngine can
        // bind each declared name to the env, even if multiple names
        // share the same TypeId post-interning. (See #70 follow-up
        // interning + #77 regression: 312-5 / test_aura_type_multi_func.)
        type_sigs_[name] = tid;
        auto mod_it = module_src.find(name);
        if (mod_it != module_src.end() && !mod_it->second.empty()) {
            type_module_src_[name] = mod_it->second;
        }
    }
}

std::string TypeChecker::declared_type_module(const std::string& name) const {
    auto it = type_module_src_.find(name);
    if (it != type_module_src_.end())
        return it->second;
    return "";
}



TypeId TypeChecker::infer_flat(FlatAST& flat, StringPool& pool, NodeId node,
                               DiagnosticCollector& diag) {
    InferenceEngine engine(types, diag);
    engine.declared_modules_ = type_module_src_;
    engine.declared_sigs_ = type_sigs_;
    engine.set_strict(strict_);  // Issue #79: plumb strict mode
    engine.bind_declared_sigs();
    auto result = engine.infer_flat(flat, pool, node);
    // Accumulate stats for the TypeChecker (Issue #72).
    auto es = engine.stats();
    stats_.cache_hits += es.cache_hits;
    stats_.cache_misses += es.cache_misses;
    stats_.stale_cache += es.stale_cache;
    return result;
}

// ── Ownership Validation ────────────────────────────────────────
//
// Walks the AST post-mutation, re-simulates ownership flow for bindings
// in the dirty set, and reports violations. This detects:
//   - Use-after-move: a variable used after its value has been moved
//   - Double-borrow: mutable borrow after existing borrow
//   - Leaked linear resource: linear-typed binding not moved/dropped
//   - Invalid ownership state: any other inconsistency
//
// The validation uses a mimimal OwnershipEnv that only tracks bindings
// in the dirty set. Clean bindings are assumed correct (validated at
// the last full type-check pass).
//
bool OwnershipEnv::validate_ownership(
    const FlatAST& flat,
    const StringPool& pool,
    NodeId root,
    const std::unordered_set<std::string>& dirty_bindings,
    std::vector<OwnershipNote>& notes_out) {
    if (dirty_bindings.empty())
        return true;

    // Build a temporary ownership environment from scratch, seeded with
    // Owned for all dirty bindings. This gives us a clean starting point
    // to detect violations.
    OwnershipEnv tmp_env;
    for (auto& name : dirty_bindings) {
        tmp_env.mark(name, OwnershipState::Owned);
    }

    // Issue #74: scope-aware tracking. The previous implementation
    // walked ops in a flat list, ignoring scope nesting. As a result,
    // a linear resource declared in a let body that was never moved
    // before the let ended was silently passed (the final-Owned
    // check was a no-op comment). We now maintain a scope stack so
    // that on scope exit, we can detect linear bindings that ended
    // in Owned state (i.e., never moved or dropped) and report them
    // as leaked-linear.
    //
    // Scope structure:
    //   - Each Let (body) introduces a new scope
    //   - Each Lambda body is a new scope
    //   - Each If then/else branch is a new scope
    //   - The whole Begin is one scope (per-expression scopes would
    //     be too granular for typical Lisp code)
    struct ScopeInfo {
        NodeId exit_node = NULL_NODE;             // node that ends this scope
        std::unordered_set<std::string> introduced;  // bindings declared here
    };
    std::vector<ScopeInfo> scope_stack;
    scope_stack.push_back({root, {}});  // root scope ends at root

    bool all_pass = true;

    // Helper: process an op and update state.
    auto apply_op = [&](NodeId op_node, NodeTag op_type,
                        const std::string& target_var) -> void {
        switch (op_type) {
            case NodeTag::Move:
                if (!tmp_env.can_move(target_var)) {
                    auto st = tmp_env.get(target_var);
                    notes_out.push_back({op_node,
                                         "use-after-move: " + target_var + " is " +
                                             tmp_env.state_name(st),
                                         "use-after-move"});
                    all_pass = false;
                }
                tmp_env.mark(target_var, OwnershipState::Moved);
                break;
            case NodeTag::Borrow:
                if (!tmp_env.can_borrow(target_var)) {
                    auto st = tmp_env.get(target_var);
                    std::string kind;
                    if (st == OwnershipState::MutBorrowed)
                        kind = "double-borrow";
                    else
                        kind = "invalid-state";
                    notes_out.push_back({op_node,
                                         "immutable borrow of " + target_var +
                                             " denied — current state: " +
                                             tmp_env.state_name(st),
                                         kind});
                    all_pass = false;
                }
                tmp_env.mark(target_var, OwnershipState::Borrowed);
                break;
            case NodeTag::MutBorrow:
                if (!tmp_env.can_mut_borrow(target_var)) {
                    auto st = tmp_env.get(target_var);
                    std::string kind;
                    if (st == OwnershipState::Borrowed || st == OwnershipState::MutBorrowed)
                        kind = "double-borrow";
                    else
                        kind = "invalid-state";
                    notes_out.push_back({op_node,
                                         "mutable borrow of " + target_var +
                                             " denied — current state: " +
                                             tmp_env.state_name(st),
                                         kind});
                    all_pass = false;
                }
                tmp_env.mark(target_var, OwnershipState::MutBorrowed);
                break;
            case NodeTag::Drop:
                if (!tmp_env.can_drop(target_var)) {
                    auto st = tmp_env.get(target_var);
                    notes_out.push_back({op_node,
                                         "cannot drop " + target_var + " — " +
                                             tmp_env.state_name(st),
                                         "leaked-linear"});
                    all_pass = false;
                }
                tmp_env.mark(target_var, OwnershipState::Moved);
                break;
            default:
                break;
        }
    };

    // Helper: pop a scope and check for leaks.
    auto pop_scope = [&]() -> void {
        if (scope_stack.size() <= 1) return;  // never pop root
        auto info = scope_stack.back();
        scope_stack.pop_back();
        for (auto& name : info.introduced) {
            // Only check linear bindings that are in the dirty set
            // (other bindings are not under ownership simulation).
            if (dirty_bindings.count(name) == 0) continue;
            auto st = tmp_env.get(name);
            if (st == OwnershipState::Owned) {
                // Linear resource declared in this scope was never
                // moved or dropped. That's a leak.
                notes_out.push_back({info.exit_node,
                                     "leaked linear resource: " + name +
                                         " (never moved or dropped at end of scope)",
                                     "leaked-linear"});
                all_pass = false;
            }
        }
    };

    // Recursive walker.
    auto walk = [&](this const auto& self, NodeId id) -> void {
        if (id >= flat.size())
            return;
        auto v = flat.get(id);

        // Process op nodes (Move/Borrow/MutBorrow/Drop).
        if (v.tag == NodeTag::Move || v.tag == NodeTag::Borrow ||
            v.tag == NodeTag::MutBorrow || v.tag == NodeTag::Drop) {
            if (!v.children.empty()) {
                auto inner_v = flat.get(v.child(0));
                if (inner_v.tag == NodeTag::Variable) {
                    auto var_name = std::string(pool.resolve(inner_v.sym_id));
                    if (dirty_bindings.count(var_name)) {
                        apply_op(id, v.tag, var_name);
                    }
                }
            }
        }

        // Handle scope-introducing nodes.
        if (v.tag == NodeTag::Let) {
            // add_let layout: children = [val, body]; name in sym_id_
            // Standard ML/Scheme semantics: the bound name is introduced
            // in the let BODY's scope (not the outer). The value is
            // evaluated in the outer scope. The body scope pops when the
            // let ends, and any linear bindings still Owned at that point
            // are reported as leaked.
            std::string name;
            if (v.sym_id != INVALID_SYM) {
                name = std::string(pool.resolve(v.sym_id));
            }
            // Process value (child 0) in current scope.
            if (v.children.size() >= 1 && v.child(0) != NULL_NODE)
                self(v.child(0));
            // Push new scope for the body, with x as introduced.
            if (v.children.size() >= 2 && v.child(1) != NULL_NODE) {
                ScopeInfo body_scope;
                body_scope.exit_node = id;
                if (!name.empty()) {
                    body_scope.introduced.insert(name);
                }
                scope_stack.push_back(std::move(body_scope));
                self(v.child(1));
                pop_scope();
            }
            return;
        }
        if (v.tag == NodeTag::Lambda) {
            // (lambda (params...) body) — children: [0..n-1]=params, [n]=body
            // Params and body are in a new scope.
            scope_stack.push_back({id, {}});
            // Add all param names to the new scope's introduced set.
            for (std::size_t i = 0; i + 1 < v.children.size(); ++i) {
                auto param = flat.get(v.child(i));
                if (param.tag == NodeTag::Variable && param.sym_id != INVALID_SYM) {
                    auto name = std::string(pool.resolve(param.sym_id));
                    scope_stack.back().introduced.insert(name);
                }
            }
            // Process body.
            if (!v.children.empty()) {
                auto last = v.children.back();
                if (last != NULL_NODE)
                    self(last);
            }
            pop_scope();
            return;
        }
        if (v.tag == NodeTag::IfExpr) {
            // (if cond then else) — children: [0]=cond, [1]=then, [2]=else (optional)
            // cond is in current scope; then/else are in new scopes.
            if (!v.children.empty() && v.child(0) != NULL_NODE)
                self(v.child(0));
            if (v.children.size() >= 2 && v.child(1) != NULL_NODE) {
                scope_stack.push_back({id, {}});
                self(v.child(1));
                pop_scope();
            }
            if (v.children.size() >= 3 && v.child(2) != NULL_NODE) {
                scope_stack.push_back({id, {}});
                self(v.child(2));
                pop_scope();
            }
            return;
        }

        // Default: recurse into children in current scope.
        for (auto c : v.children) {
            if (c != NULL_NODE)
                self(c);
        }
    };
    walk(root);

    return all_pass;
}

} // namespace aura::compiler
