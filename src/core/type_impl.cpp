module aura.core.type;

namespace aura::core {

TypeRegistry::TypeRegistry() {
    register_type(TypeTag::DYNAMIC, "Any");
    register_type(TypeTag::INT, "Int");
    register_type(TypeTag::BOOL, "Bool");
    register_type(TypeTag::STRING, "String");
    register_type(TypeTag::VOID, "Void");
    register_type(TypeTag::TYPE, "Type");
    register_type(TypeTag::VECTOR, "Vector");
    register_type(TypeTag::FLOAT, "Float");
    register_type(TypeTag::PAIR, "Pair");
    register_type(TypeTag::HASH, "Hash");
}

TypeId TypeRegistry::register_type(TypeTag tag, std::string name) {
    // Dedup: same tag + same name returns the existing TypeId.
    for (std::uint32_t i = 0; i < entries_.size(); ++i) {
        if (entries_[i].tag == tag && entries_[i].name == name) {
            return TypeId{i, next_generation_};
        }
    }
    auto id = TypeId{
        .index = static_cast<std::uint32_t>(entries_.size()),
        .generation = next_generation_,
    };
    entries_.push_back(Entry{tag, std::move(name), std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt});
    name_to_id_[entries_.back().name] = id;
    return id;
}

TypeId TypeRegistry::register_func(std::vector<TypeId> args, TypeId ret) {
    // Dedup: same (args, ret) returns the existing TypeId. The
    // existing entry's name may differ from what we'd compute
    // (e.g. user named it via register_func_named); we keep the
    // first registration's name to preserve identity.
    for (std::uint32_t i = 0; i < entries_.size(); ++i) {
        if (entries_[i].tag != TypeTag::FUNC || !entries_[i].func) continue;
        const auto& f = *entries_[i].func;
        if (f.args.size() != args.size()) continue;
        if (!(f.ret == ret) && !type_equals(f.ret, ret)) continue;
        bool match = true;
        for (std::size_t j = 0; j < args.size(); ++j) {
            if (f.args[j] == args[j]) continue;
            if (!type_equals(f.args[j], args[j])) { match = false; break; }
        }
        if (match) return TypeId{i, next_generation_};
    }
    const auto id = TypeId{
        .index = static_cast<std::uint32_t>(entries_.size()),
        .generation = next_generation_,
    };
    auto ft = FuncType{std::move(args), ret};
    auto tag = TypeTag::FUNC;
    std::string tmp_name = "(" + std::to_string(ft.args.size()) + "->)";
    entries_.push_back(Entry{tag, tmp_name, std::move(ft), std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt});
    std::string name = "(";
    for (auto& a : entries_.back().func->args)
        name += std::string(name_of(a)) + " ";
    name += std::string("-> ") + std::string(name_of(ret)) + ")";
    entries_.back().name = name;
    return id;
}

TypeId TypeRegistry::register_func_named(std::vector<TypeId> args, TypeId ret, std::string name) {
    // Register via the standard path (which dedups), then OVERWRITE
    // the name. If the same (args, ret) was already registered, we
    // still update the name so the user's chosen name takes effect.
    auto id = register_func(std::move(args), ret);
    if (id.valid() && id.index < entries_.size()) {
        entries_[id.index].name = std::move(name);
    }
    return id;
}

TypeId TypeRegistry::register_linear(TypeId inner) {
    // Dedup: same inner returns the existing TypeId.
    for (std::uint32_t i = 0; i < entries_.size(); ++i) {
        if (entries_[i].tag != TypeTag::LINEAR || !entries_[i].linear) continue;
        const auto& l = *entries_[i].linear;
        if (l.inner == inner || type_equals(l.inner, inner)) {
            return TypeId{i, next_generation_};
        }
    }
    auto id = TypeId{
        .index = static_cast<std::uint32_t>(entries_.size()),
        .generation = next_generation_,
    };
    std::string linear_name = "(Linear " + std::string(name_of(inner)) + ")";
    entries_.push_back(Entry{TypeTag::LINEAR, std::move(linear_name), std::nullopt, std::nullopt,
                             LinearType{inner}, std::nullopt, std::nullopt, std::nullopt,
                             std::nullopt, std::nullopt, std::nullopt});
    name_to_id_[entries_.back().name] = id;
    return id;
}

TypeId TypeRegistry::register_module(ModuleType mt) {
    // Dedup: same member list (in order) returns the existing TypeId.
    for (std::uint32_t i = 0; i < entries_.size(); ++i) {
        if (entries_[i].tag != TypeTag::MODULE || !entries_[i].module_type) continue;
        const auto& m = *entries_[i].module_type;
        if (m.members.size() != mt.members.size()) continue;
        bool match = true;
        for (std::size_t j = 0; j < m.members.size(); ++j) {
            if (m.members[j].first != mt.members[j].first) { match = false; break; }
            if (m.members[j].second == mt.members[j].second) continue;
            if (!type_equals(m.members[j].second, mt.members[j].second)) { match = false; break; }
        }
        if (match) return TypeId{i, next_generation_};
    }
    auto id = TypeId{
        .index = static_cast<std::uint32_t>(entries_.size()),
        .generation = next_generation_,
    };
    std::string name = "Module{";
    for (auto& [n, t] : mt.members) {
        if (name.back() != '{') name += ", ";
        name += n + ": " + std::string(name_of(t));
    }
    name += "}";
    entries_.push_back(Entry{TypeTag::MODULE, std::move(name), std::nullopt, std::nullopt,
                             std::nullopt, std::move(mt), std::nullopt, std::nullopt,
                             std::nullopt, std::nullopt, std::nullopt});
    return id;
}

TypeId TypeRegistry::register_variant(VariantType vt) {
    // Dedup: same constructor list (in order) returns the existing TypeId.
    for (std::uint32_t i = 0; i < entries_.size(); ++i) {
        if (entries_[i].tag != TypeTag::VARIANT || !entries_[i].variant) continue;
        const auto& v = *entries_[i].variant;
        if (v.variants.size() != vt.variants.size()) continue;
        bool match = true;
        for (std::size_t j = 0; j < v.variants.size(); ++j) {
            if (v.variants[j].first != vt.variants[j].first) { match = false; break; }
            if (v.variants[j].second.size() != vt.variants[j].second.size()) { match = false; break; }
            for (std::size_t k = 0; k < v.variants[j].second.size(); ++k) {
                if (v.variants[j].second[k] == vt.variants[j].second[k]) continue;
                if (!type_equals(v.variants[j].second[k], vt.variants[j].second[k])) {
                    match = false;
                    break;
                }
            }
            if (!match) break;
        }
        if (match) return TypeId{i, next_generation_};
    }
    auto id = TypeId{
        .index = static_cast<std::uint32_t>(entries_.size()),
        .generation = next_generation_,
    };
    std::string name = "Variant{";
    for (auto& [n, args] : vt.variants) {
        if (name.back() != '{') name += ", ";
        name += "(" + n;
        for (auto& a : args)
            name += " " + std::string(name_of(a));
        name += ")";
    }
    name += "}";
    entries_.push_back(Entry{TypeTag::VARIANT, std::move(name), std::nullopt, std::nullopt,
                             std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt,
                             std::move(vt), std::nullopt});
    return id;
}

TypeId TypeRegistry::register_record(RecordType rt) {
    // Dedup: same field list (in order) returns the existing TypeId.
    for (std::uint32_t i = 0; i < entries_.size(); ++i) {
        if (entries_[i].tag != TypeTag::RECORD || !entries_[i].record) continue;
        const auto& r = *entries_[i].record;
        if (r.fields.size() != rt.fields.size()) continue;
        bool match = true;
        for (std::size_t j = 0; j < r.fields.size(); ++j) {
            if (r.fields[j].first != rt.fields[j].first) { match = false; break; }
            if (r.fields[j].second == rt.fields[j].second) continue;
            if (!type_equals(r.fields[j].second, rt.fields[j].second)) { match = false; break; }
        }
        if (match) return TypeId{i, next_generation_};
    }
    auto id = TypeId{
        .index = static_cast<std::uint32_t>(entries_.size()),
        .generation = next_generation_,
    };
    std::string name = "Record{";
    for (auto& [n, t] : rt.fields) {
        if (name.back() != '{') name += ", ";
        name += n + ": " + std::string(name_of(t));
    }
    name += "}";
    entries_.push_back(Entry{TypeTag::RECORD, std::move(name), std::nullopt, std::nullopt,
                             std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt,
                             std::nullopt, std::move(rt)});
    return id;
}

const VariantType* TypeRegistry::variant_of(TypeId id) const {
    if (id.index < entries_.size() && entries_[id.index].variant)
        return &*entries_[id.index].variant;
    return nullptr;
}

const RecordType* TypeRegistry::record_of(TypeId id) const {
    if (id.index < entries_.size() && entries_[id.index].record)
        return &*entries_[id.index].record;
    return nullptr;
}

const ModuleType* TypeRegistry::module_of(TypeId id) const {
    if (id.index < entries_.size() && entries_[id.index].module_type)
        return &*entries_[id.index].module_type;
    return nullptr;
}

TypeId TypeRegistry::register_effect(std::string name, TypeId arg) {
    // Dedup: same name + same arg returns the existing TypeId.
    for (std::uint32_t i = 0; i < entries_.size(); ++i) {
        if (entries_[i].tag != TypeTag::EFFECT || !entries_[i].effect) continue;
        const auto& e = *entries_[i].effect;
        if (e.name != name) continue;
        if (e.arg == arg || type_equals(e.arg, arg)) {
            return TypeId{i, next_generation_};
        }
    }
    auto id = TypeId{
        .index = static_cast<std::uint32_t>(entries_.size()),
        .generation = next_generation_,
    };
    auto eff_name = std::string("!") + name;
    entries_.push_back(Entry{TypeTag::EFFECT, std::move(eff_name), std::nullopt, std::nullopt,
                             std::nullopt, std::nullopt, std::nullopt,
                             EffectType{std::move(name), arg},
                             std::nullopt, std::nullopt, std::nullopt});
    return id;
}

TypeId TypeRegistry::register_capability(std::vector<std::string> effects, bool unrestricted) {
    // Dedup: same effect set (order-independent) + same unrestricted
    // returns the existing TypeId.
    for (std::uint32_t i = 0; i < entries_.size(); ++i) {
        if (entries_[i].tag != TypeTag::CAPABILITY || !entries_[i].capability) continue;
        const auto& c = *entries_[i].capability;
        if (c.is_unrestricted != unrestricted) continue;
        if (c.effects.size() != effects.size()) continue;
        // Set equality (order-independent).
        bool all_found = true;
        for (auto& e : effects) {
            bool found = false;
            for (auto& e2 : c.effects) {
                if (e == e2) { found = true; break; }
            }
            if (!found) { all_found = false; break; }
        }
        if (all_found) return TypeId{i, next_generation_};
    }
    auto id = TypeId{
        .index = static_cast<std::uint32_t>(entries_.size()),
        .generation = next_generation_,
    };
    std::string name = "Capability{";
    bool first = true;
    for (auto& e : effects) {
        if (!first) name += ", ";
        name += e;
        first = false;
    }
    name += "}";
    if (unrestricted)
        name = "Capability{*}";
    entries_.push_back(Entry{TypeTag::CAPABILITY, std::move(name), std::nullopt, std::nullopt,
                             std::nullopt, std::nullopt, std::nullopt, std::nullopt,
                             CapabilityType{std::move(effects), unrestricted},
                             std::nullopt, std::nullopt});
    return id;
}

const CapabilityType* TypeRegistry::capability_of(TypeId id) const {
    if (id.index < entries_.size() && entries_[id.index].capability)
        return &*entries_[id.index].capability;
    return nullptr;
}

const EffectType* TypeRegistry::effect_of(TypeId id) const {
    if (id.index < entries_.size() && entries_[id.index].effect)
        return &*entries_[id.index].effect;
    return nullptr;
}

TypeId TypeRegistry::register_forall(TypeId var, TypeId body) {
    // Dedup: same var index + same body returns the existing TypeId.
    // (Caveat: bound var is by INDEX. Two calls with structurally
    // equal bodies but different vars collapse — correct per
    // structural-identity semantics.)
    for (std::uint32_t i = 0; i < entries_.size(); ++i) {
        if (entries_[i].tag != TypeTag::FORALL || !entries_[i].forall) continue;
        const auto& f = *entries_[i].forall;
        if (f.var.index != var.index) continue;
        if (f.body == body || type_equals(f.body, body)) {
            return TypeId{i, next_generation_};
        }
    }
    auto id = TypeId{
        .index = static_cast<std::uint32_t>(entries_.size()),
        .generation = next_generation_,
    };
    auto name_var = name_of(var);
    auto name_body = name_of(body);
    std::string forall_name =
        std::string("∀") + std::string(name_var) + ". " + std::string(name_body);
    entries_.push_back(Entry{TypeTag::FORALL, std::move(forall_name), std::nullopt,
                             ForallType{var, body}, std::nullopt, std::nullopt,
                             std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt});
    return id;
}

TypeId TypeRegistry::make_var(std::string name) {
    if (name.empty())
        name = "__t" + std::to_string(entries_.size());
    auto id = TypeId{
        .index = static_cast<std::uint32_t>(entries_.size()),
        .generation = next_generation_,
    };
    entries_.push_back(Entry{TypeTag::TYPE_VAR, std::move(name), std::nullopt, std::nullopt,
                             std::nullopt, std::nullopt, std::nullopt, std::nullopt,
                             std::nullopt, std::nullopt, std::nullopt});
    return id;
}

TypeTag TypeRegistry::tag_of(TypeId id) const {
    if (id.index < entries_.size())
        return entries_[id.index].tag;
    return TypeTag::DYNAMIC;
}

std::string_view TypeRegistry::name_of(TypeId id) const {
    if (id.index < entries_.size())
        return entries_[id.index].name;
    return "<invalid>";
}

const LinearType* TypeRegistry::linear_of(TypeId id) const {
    if (id.index < entries_.size() && entries_[id.index].linear)
        return &*entries_[id.index].linear;
    return nullptr;
}

const ForallType* TypeRegistry::forall_of(TypeId id) const {
    if (id.index < entries_.size() && entries_[id.index].forall)
        return &*entries_[id.index].forall;
    return nullptr;
}

const FuncType* TypeRegistry::func_of(TypeId id) const {
    if (id.index < entries_.size() && entries_[id.index].func)
        return &*entries_[id.index].func;
    return nullptr;
}

bool TypeRegistry::is_var(TypeId id) const {
    return id.index < entries_.size() && entries_[id.index].tag == TypeTag::TYPE_VAR;
}

TypeId TypeRegistry::instantiate(TypeId forall_id, std::function<TypeId()> fresh_var) {
    auto* ft = forall_of(forall_id);
    if (!ft)
        return forall_id;
    // Build substitution map: bound var → fresh var, then delegate to substitute()
    std::unordered_map<std::uint32_t, TypeId> subst;
    subst[ft->var.index] = fresh_var();
    return substitute(ft->body, subst);
}

TypeId TypeRegistry::instantiate_forall(TypeId forall_id,
                                         const std::vector<TypeId>& args) {
    TypeId result = forall_id;
    std::size_t arg_idx = 0;
    while (auto* ft = forall_of(result)) {
        // Issue #76 fix: instead of breaking when args runs out,
        // continue with a fresh type variable for the residual
        // bound var. This keeps the returned type fully instantiated
        // (no free vars), so any monomorphic consumer stays sound.
        std::unordered_map<std::uint32_t, TypeId> subst;
        if (arg_idx < args.size()) {
            subst[ft->var.index] = args[arg_idx++];
        } else {
            subst[ft->var.index] = make_var("");  // fresh, unnamed
        }
        result = substitute(ft->body, subst);
    }
    return result;
}

// Issue #70: real structural subtyping. See docs/design/issue-70-subtyping-design.md
// for the full design. The public is_subtype delegates to the depth-limited
// helper so the public API stays clean.
bool TypeRegistry::is_subtype(TypeId sub, TypeId sup) const {
    return is_subtype_impl(sub, sup, 0);
}

bool TypeRegistry::is_subtype_impl(TypeId sub, TypeId sup, int depth) const {
    // Safety net: pathological types could cycle if someone interns them
    // (shouldn't happen, but bounded recursion keeps us safe).
    if (depth > 64) return false;

    // Reflexivity: T <: T.
    if (sub == sup) return true;

    // Dynamic: T <: Any. Any is the top type (not a subtype of any
    // concrete type — but it IS equal to itself via the reflexivity
    // check above).
    if (sup == dynamic_type()) return true;

    // Type variables: defer to substitution. The caller's
    // instantiate/substitute pipeline will resolve these. Returning
    // true here makes subtyping "open" w.r.t. unresolved vars, which
    // matches the design where free vars are placeholders.
    if (is_var(sub) || is_var(sup)) return true;

    auto sub_tag = tag_of(sub);
    auto sup_tag = tag_of(sup);

    // Different leaf tags → not subtype. A few cross-tag equalities
    // would belong here (e.g. Int <-> Float coercion) but those are
    // handled by the explicit coercion pass, not by is_subtype.
    if (sub_tag != sup_tag) return false;

    switch (sub_tag) {
        case TypeTag::FUNC: {
            // (A1->A2) <: (B1->B2) iff
            //   B1 <: A1  (contravariant in arg)
            //   A2 <: B2  (covariant in return)
            auto* sf = func_of(sub);
            auto* st = func_of(sup);
            if (!sf || !st) return false;
            if (sf->args.size() != st->args.size()) return false;
            for (std::size_t i = 0; i < sf->args.size(); ++i) {
                if (!is_subtype_impl(st->args[i], sf->args[i], depth + 1)) return false;
            }
            return is_subtype_impl(sf->ret, st->ret, depth + 1);
        }
        case TypeTag::RECORD: {
            // Width subtyping: sub has at least the fields of sup,
            // with each shared field's type being a subtype.
            auto* sr = record_of(sub);
            auto* tr = record_of(sup);
            if (!sr || !tr) return false;
            for (auto& [name, sup_type] : tr->fields) {
                TypeId sub_type{};
                for (auto& [n2, t2] : sr->fields) {
                    if (n2 == name) { sub_type = t2; break; }
                }
                if (!sub_type.valid()) return false;
                if (!is_subtype_impl(sub_type, sup_type, depth + 1)) return false;
            }
            return true;
        }
        case TypeTag::VARIANT: {
            // Width subtyping the other way: sub's constructors must
            // each match a sup constructor by name (Aura variants are
            // nominal by constructor name) with covariant args.
            auto* sv = variant_of(sub);
            auto* tv = variant_of(sup);
            if (!sv || !tv) return false;
            for (auto& [name, sub_args] : sv->variants) {
                bool found = false;
                for (auto& [n2, sup_args] : tv->variants) {
                    if (n2 != name) continue;
                    if (sub_args.size() != sup_args.size()) return false;
                    bool args_ok = true;
                    for (std::size_t i = 0; i < sub_args.size(); ++i) {
                        if (!is_subtype_impl(sub_args[i], sup_args[i], depth + 1)) {
                            args_ok = false;
                            break;
                        }
                    }
                    if (args_ok) { found = true; break; }
                }
                if (!found) return false;
            }
            return true;
        }
        case TypeTag::LINEAR: {
            auto* sl = linear_of(sub);
            auto* tl = linear_of(sup);
            if (!sl || !tl) return false;
            return is_subtype_impl(sl->inner, tl->inner, depth + 1);
        }
        case TypeTag::MODULE: {
            // Width subtyping on members, like records.
            auto* sm = module_of(sub);
            auto* tm = module_of(sup);
            if (!sm || !tm) return false;
            for (auto& [name, sup_type] : tm->members) {
                TypeId sub_type{};
                for (auto& [n2, t2] : sm->members) {
                    if (n2 == name) { sub_type = t2; break; }
                }
                if (!sub_type.valid()) return false;
                if (!is_subtype_impl(sub_type, sup_type, depth + 1)) return false;
            }
            return true;
        }
        case TypeTag::FORALL: {
            // Forall subtyping requires alpha-renaming + substitution.
            // Out of scope for the initial fix (see design doc).
            return false;
        }
        case TypeTag::CAPABILITY: {
            // Cap{e1,e2,...} <: Cap{e1',e2',...} iff sup's effects are a
            // subset of sub's effects. A more restrictive cap (fewer
            // effects) is a subtype of a less restrictive one — that
            // matches the access-modelling direction.
            auto* sc = capability_of(sub);
            auto* tc = capability_of(sup);
            if (!sc || !tc) return false;
            for (auto& e : tc->effects) {
                bool found = false;
                for (auto& e2 : sc->effects) {
                    if (e == e2) { found = true; break; }
                }
                if (!found) return false;
            }
            return true;
        }
        case TypeTag::EFFECT: {
            // Effect types are nominal: same name = same effect.
            return std::string(name_of(sub)) == std::string(name_of(sup));
        }
        // Leaf types (INT, BOOL, STRING, VOID, TYPE, VECTOR, FLOAT,
        // PAIR, HASH): handled by the identity check at the top.
        default:
            return false;
    }
}

TypeId TypeRegistry::lookup_type(const std::string& name) const {
    auto it = name_to_id_.find(name);
    if (it != name_to_id_.end())
        return it->second;
    return TypeId{};
}

TypeId TypeRegistry::substitute(TypeId ty,
                                 const std::unordered_map<std::uint32_t, TypeId>& subst) {
    auto it = subst.find(ty.index);
    if (it != subst.end())
        return it->second;

    switch (tag_of(ty)) {
    case TypeTag::FUNC: {
        auto* f = func_of(ty);
        std::vector<TypeId> new_args;
        for (auto& a : f->args)
            new_args.push_back(substitute(a, subst));
        return register_func(std::move(new_args), substitute(f->ret, subst));
    }
    case TypeTag::FORALL: {
        auto* ft = forall_of(ty);
        // Issue #77: capture avoidance. The bound var inside the
        // body is a fresh variable (in the HM sense) that happens
        // to share an index with whatever was outer. Shadow it by
        // removing its index from subst before recursing, so a free
        // var in the body that coincidentally shares the index
        // doesn't get captured by an outer subst entry.
        auto inner_subst = subst;
        inner_subst.erase(ft->var.index);
        return register_forall(ft->var, substitute(ft->body, inner_subst));
    }
    case TypeTag::LINEAR: {
        auto* lt = linear_of(ty);
        return register_linear(substitute(lt->inner, subst));
    }
    case TypeTag::MODULE: {
        auto* mt = module_of(ty);
        ModuleType new_mt;
        for (auto& [n, t] : mt->members)
            new_mt.members.push_back({n, substitute(t, subst)});
        new_mt.type_params = mt->type_params;
        for (auto& v : mt->type_param_vars)
            new_mt.type_param_vars.push_back(substitute(v, subst));
        return register_module(std::move(new_mt));
    }
    case TypeTag::VARIANT: {
        auto* vt = variant_of(ty);
        VariantType new_vt;
        for (auto& [name, args] : vt->variants) {
            std::vector<TypeId> new_args;
            for (auto& a : args)
                new_args.push_back(substitute(a, subst));
            new_vt.variants.push_back({name, std::move(new_args)});
        }
        return register_variant(std::move(new_vt));
    }
    case TypeTag::RECORD: {
        auto* rt = record_of(ty);
        RecordType new_rt;
        for (auto& [name, type] : rt->fields)
            new_rt.fields.push_back({name, substitute(type, subst)});
        return register_record(std::move(new_rt));
    }
    default:
        return ty;
    }
}

std::vector<TypeId> TypeRegistry::free_vars(TypeId id) const {
    std::vector<TypeId> result;
    if (!id.valid() || id.index >= entries_.size())
        return result;
    std::vector<TypeId> stack = {id};
    std::unordered_set<std::uint32_t> seen;
    std::unordered_set<std::uint32_t> bound; // variables bound by enclosing ForallType
    while (!stack.empty()) {
        auto cur = stack.back();
        stack.pop_back();
        if (!cur.valid() || cur.index >= entries_.size())
            continue;
        if (!seen.insert(cur.index).second)
            continue;
        if (auto* ft = forall_of(cur)) {
            bound.insert(ft->var.index);
            stack.push_back(ft->body);
            continue;
        }
        if (entries_[cur.index].tag == TypeTag::TYPE_VAR) {
            if (!bound.contains(cur.index))
                result.push_back(cur); // preserve exact TypeId with generation
            continue;
        }
        if (auto* f = func_of(cur)) {
            stack.push_back(f->ret);
            for (auto& a : f->args)
                stack.push_back(a);
        }
        if (auto* lt = linear_of(cur)) {
            stack.push_back(lt->inner);
        }
        if (auto* mt = module_of(cur)) {
            for (auto& [n, t] : mt->members)
                stack.push_back(t);
        }
        if (auto* vt = variant_of(cur)) {
            for (auto& [name, args] : vt->variants)
                for (auto& a : args)
                    stack.push_back(a);
        }
        if (auto* rt = record_of(cur)) {
            for (auto& [name, t] : rt->fields)
                stack.push_back(t);
        }
    }
    return result;
}

std::string TypeRegistry::format_type(TypeId id) const {
    auto tag = tag_of(id);
    switch (tag) {
        case TypeTag::FUNC: {
            auto* f = func_of(id);
            if (!f)
                return "<func>";
            std::string s = "(";
            for (std::size_t i = 0; i < f->args.size(); i++) {
                if (i > 0)
                    s += " ";
                s += format_type(f->args[i]);
            }
            return s + " -> " + format_type(f->ret) + ")";
        }
        case TypeTag::TYPE_VAR:
            return std::string(name_of(id));
        case TypeTag::FORALL: {
            auto* ft = forall_of(id);
            if (!ft)
                return "<forall>";
            return "∀" + format_type(ft->var) + ". " + format_type(ft->body);
        }
        case TypeTag::LINEAR: {
            auto* lt = linear_of(id);
            if (!lt)
                return "<linear>";
            return "(Linear " + format_type(lt->inner) + ")";
        }
        case TypeTag::MODULE: {
            auto* mt = module_of(id);
            if (!mt) return "<module>";
            std::string s = "Module{";
            for (auto& [n, t] : mt->members) {
                if (s.back() != '{') s += ", ";
                s += n + ": " + format_type(t);
            }
            return s + "}";
        }
        case TypeTag::VARIANT: {
            auto* vt = variant_of(id);
            if (!vt) {
                // Registered via register_type(name) — use the stored name
                auto n = name_of(id);
                if (!n.empty() && n != "<variant>")
                    return std::string(n);
                return "<variant>";
            }
            std::string s = "Variant{";
            for (auto& [name, args] : vt->variants) {
                if (s.back() != '{') s += ", ";
                s += "(" + name;
                for (auto& a : args)
                    s += " " + format_type(a);
                s += ")";
            }
            return s + "}";
        }
        case TypeTag::RECORD: {
            auto* rt = record_of(id);
            if (!rt) {
                auto n = name_of(id);
                if (!n.empty() && n != "<record>")
                    return std::string(n);
                return "<record>";
            }
            std::string s = "Record{";
            for (auto& [name, type] : rt->fields) {
                if (s.back() != '{') s += ", ";
                s += name + ": " + format_type(type);
            }
            return s + "}";
        }
        case TypeTag::CAPABILITY: {
            auto* cap = capability_of(id);
            if (!cap) return "<capability>";
            if (cap->is_unrestricted)
                return "Capability{*}";
            std::string s = "Capability{";
            for (std::size_t i = 0; i < cap->effects.size(); ++i) {
                if (i > 0) s += ", ";
                s += cap->effects[i];
            }
            return s + "}";
        }
        case TypeTag::EFFECT: {
            auto* eff = effect_of(id);
            if (!eff) return "<effect>";
            auto s = std::string("!") + eff->name;
            if (eff->arg.valid())
                s += "[" + format_type(eff->arg) + "]";
            return s;
        }
        default:
            return std::string(name_of(id));
    }
}

void TypeRegistry::register_adt_constructors(TypeId type_id,
                                              std::vector<std::string> constructors) {
    if (type_id.valid() && type_id.index < entries_.size()) {
        entries_[type_id.index].adt_constructors = std::move(constructors);
    }
}

const std::vector<std::string>* TypeRegistry::get_adt_constructors(TypeId type_id) const {
    if (type_id.valid() && type_id.index < entries_.size() &&
        entries_[type_id.index].adt_constructors.has_value()) {
        return &(*entries_[type_id.index].adt_constructors);
    }
    return nullptr;
}

// Issue #78: TypeRegistry unbounded memory growth.
// compact() reclaims all non-predefined entries, bumps the generation
// counter (invalidating all TypeIds from prior generations), and
// re-registers the 9 predefined types so int_type() / string_type()
// etc. continue to work. After this call, any TypeId with
// `generation < generation()` is stale.
std::uint32_t TypeRegistry::compact() {
    std::uint32_t before = static_cast<std::uint32_t>(entries_.size());
    // Bump generation FIRST so any in-flight TypeId registrations
    // that race with us get a stale generation and can be detected.
    ++next_generation_;
    // Clear all entries. shrink_to_fit() to actually release memory
    // (the std::optional members can leave residual capacity even
    // after clear()).
    entries_.clear();
    entries_.shrink_to_fit();
    name_to_id_.clear();
    name_to_id_.rehash(0);  // rehash to smallest bucket count
    // Re-register the 9 predefined types so the basic lookups work.
    // Each new TypeId carries the bumped generation.
    register_type(TypeTag::DYNAMIC, "Any");
    register_type(TypeTag::INT, "Int");
    register_type(TypeTag::BOOL, "Bool");
    register_type(TypeTag::STRING, "String");
    register_type(TypeTag::VOID, "Void");
    register_type(TypeTag::TYPE, "Type");
    register_type(TypeTag::VECTOR, "Vector");
    register_type(TypeTag::FLOAT, "Float");
    register_type(TypeTag::PAIR, "Pair");
    register_type(TypeTag::HASH, "Hash");
    std::uint32_t reclaimed = before - static_cast<std::uint32_t>(entries_.size());
    return reclaimed;
}

// Issue #70 follow-up: TypeId interning. Two helpers that compare
// TypeIds by structural content rather than by raw id. Used by the
// register_* methods to dedup identical types.

// Structural hash. FNV-1a, combining the tag with the relevant
// components. Order matters for records/variants/modules (use the
// insertion order — which is the canonical order). Sets (capability
// effects) sort first for hash invariance.
std::uint64_t TypeRegistry::type_hash(TypeId a) const {
    auto tag = tag_of(a);
    std::uint64_t h = 0xcbf29ce484222325ull;
    auto mix = [&](std::uint64_t v) { h = (h ^ v) * 0x100000001b3ull; };
    mix(static_cast<std::uint64_t>(tag));
    switch (tag) {
        case TypeTag::TYPE_VAR:  mix(a.index); break;
        case TypeTag::FUNC: {
            auto* f = func_of(a);
            if (f) {
                mix(f->args.size());
                for (auto arg : f->args) mix(type_hash(arg));
                mix(type_hash(f->ret));
            }
            break;
        }
        case TypeTag::LINEAR: {
            auto* l = linear_of(a);
            if (l) mix(type_hash(l->inner));
            break;
        }
        case TypeTag::MODULE: {
            auto* m = module_of(a);
            if (m) {
                mix(m->members.size());
                for (auto& [n, t] : m->members) {
                    mix(std::hash<std::string>{}(n));
                    mix(type_hash(t));
                }
            }
            break;
        }
        case TypeTag::VARIANT: {
            auto* v = variant_of(a);
            if (v) {
                mix(v->variants.size());
                for (auto& [n, args] : v->variants) {
                    mix(std::hash<std::string>{}(n));
                    mix(args.size());
                    for (auto a : args) mix(type_hash(a));
                }
            }
            break;
        }
        case TypeTag::RECORD: {
            auto* r = record_of(a);
            if (r) {
                mix(r->fields.size());
                for (auto& [n, t] : r->fields) {
                    mix(std::hash<std::string>{}(n));
                    mix(type_hash(t));
                }
            }
            break;
        }
        case TypeTag::EFFECT: {
            auto* e = effect_of(a);
            if (e) {
                mix(std::hash<std::string>{}(e->name));
                mix(type_hash(e->arg));
            }
            break;
        }
        case TypeTag::CAPABILITY: {
            auto* c = capability_of(a);
            if (c) {
                // Sort effects for order-independent hash.
                std::vector<std::string> sorted(c->effects);
                std::sort(sorted.begin(), sorted.end());
                mix(c->is_unrestricted);
                mix(sorted.size());
                for (auto& e : sorted) mix(std::hash<std::string>{}(e));
            }
            break;
        }
        case TypeTag::FORALL: {
            auto* f = forall_of(a);
            if (f) {
                mix(f->var.index);
                mix(type_hash(f->body));
            }
            break;
        }
        default: break;  // leaves: tag alone is enough
    }
    return h;
}

// Structural equality. Same-tag rules: args/ret/inner/members/variants/
// fields compared recursively. For capability, the effect SETS must
// match (order-independent). For different tags, never equal.
bool TypeRegistry::type_equals(TypeId a, TypeId b) const {
    if (a == b) return true;
    auto tag_a = tag_of(a);
    auto tag_b = tag_of(b);
    if (tag_a != tag_b) return false;
    switch (tag_a) {
        case TypeTag::TYPE_VAR:  return a.index == b.index;
        case TypeTag::FUNC: {
            auto* fa = func_of(a);
            auto* fb = func_of(b);
            if (!fa || !fb) return false;
            if (fa->args.size() != fb->args.size()) return false;
            for (std::size_t i = 0; i < fa->args.size(); ++i) {
                if (fa->args[i] == fb->args[i]) continue;
                if (!type_equals(fa->args[i], fb->args[i])) return false;
            }
            if (fa->ret == fb->ret) return true;
            return type_equals(fa->ret, fb->ret);
        }
        case TypeTag::LINEAR: {
            auto* la = linear_of(a);
            auto* lb = linear_of(b);
            if (!la || !lb) return false;
            if (la->inner == lb->inner) return true;
            return type_equals(la->inner, lb->inner);
        }
        case TypeTag::MODULE: {
            auto* ma = module_of(a);
            auto* mb = module_of(b);
            if (!ma || !mb) return false;
            if (ma->members.size() != mb->members.size()) return false;
            for (std::size_t i = 0; i < ma->members.size(); ++i) {
                if (ma->members[i].first != mb->members[i].first) return false;
                if (ma->members[i].second == mb->members[i].second) continue;
                if (!type_equals(ma->members[i].second, mb->members[i].second)) return false;
            }
            return true;
        }
        case TypeTag::VARIANT: {
            auto* va = variant_of(a);
            auto* vb = variant_of(b);
            if (!va || !vb) return false;
            if (va->variants.size() != vb->variants.size()) return false;
            for (std::size_t i = 0; i < va->variants.size(); ++i) {
                if (va->variants[i].first != vb->variants[i].first) return false;
                if (va->variants[i].second.size() != vb->variants[i].second.size()) return false;
                for (std::size_t j = 0; j < va->variants[i].second.size(); ++j) {
                    if (va->variants[i].second[j] == vb->variants[i].second[j]) continue;
                    if (!type_equals(va->variants[i].second[j], vb->variants[i].second[j])) return false;
                }
            }
            return true;
        }
        case TypeTag::RECORD: {
            auto* ra = record_of(a);
            auto* rb = record_of(b);
            if (!ra || !rb) return false;
            if (ra->fields.size() != rb->fields.size()) return false;
            for (std::size_t i = 0; i < ra->fields.size(); ++i) {
                if (ra->fields[i].first != rb->fields[i].first) return false;
                if (ra->fields[i].second == rb->fields[i].second) continue;
                if (!type_equals(ra->fields[i].second, rb->fields[i].second)) return false;
            }
            return true;
        }
        case TypeTag::EFFECT: {
            auto* ea = effect_of(a);
            auto* eb = effect_of(b);
            if (!ea || !eb) return false;
            return ea->name == eb->name && type_equals(ea->arg, eb->arg);
        }
        case TypeTag::CAPABILITY: {
            auto* ca = capability_of(a);
            auto* cb = capability_of(b);
            if (!ca || !cb) return false;
            if (ca->is_unrestricted != cb->is_unrestricted) return false;
            if (ca->effects.size() != cb->effects.size()) return false;
            // Order-independent set comparison.
            for (auto& e : ca->effects) {
                bool found = false;
                for (auto& e2 : cb->effects) {
                    if (e == e2) { found = true; break; }
                }
                if (!found) return false;
            }
            return true;
        }
        case TypeTag::FORALL: {
            auto* fa = forall_of(a);
            auto* fb = forall_of(b);
            if (!fa || !fb) return false;
            if (fa->var.index != fb->var.index) return false;
            if (fa->body == fb->body) return true;
            return type_equals(fa->body, fb->body);
        }
        default:
            return false;  // leaves: handled by a == b above (only same tag)
    }
}

} // namespace aura::core
