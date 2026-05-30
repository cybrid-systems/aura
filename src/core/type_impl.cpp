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
    auto id = TypeId{
        .index = static_cast<std::uint32_t>(entries_.size()),
        .generation = next_generation_,
    };
    entries_.push_back(Entry{tag, std::move(name), std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt});
    name_to_id_[entries_.back().name] = id;
    return id;
}

TypeId TypeRegistry::register_func(std::vector<TypeId> args, TypeId ret) {
    auto id = TypeId{
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
    auto id = register_func(std::move(args), ret);
    if (id.valid() && id.index < entries_.size()) {
        entries_[id.index].name = std::move(name);
    }
    return id;
}

TypeId TypeRegistry::register_linear(TypeId inner) {
    auto id = TypeId{
        .index = static_cast<std::uint32_t>(entries_.size()),
        .generation = next_generation_,
    };
    std::string linear_name = "(Linear " + std::string(name_of(inner)) + ")";
    entries_.push_back(Entry{TypeTag::LINEAR, std::move(linear_name), std::nullopt, std::nullopt,
                             LinearType{inner}, std::nullopt, std::nullopt, std::nullopt, std::nullopt});
    name_to_id_[entries_.back().name] = id;
    return id;
}

TypeId TypeRegistry::register_module(ModuleType mt) {
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
                             std::nullopt, std::move(mt), std::nullopt, std::nullopt, std::nullopt});
    return id;
}

TypeId TypeRegistry::register_variant(VariantType vt) {
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
    auto id = TypeId{
        .index = static_cast<std::uint32_t>(entries_.size()),
        .generation = next_generation_,
    };
    auto eff_name = std::string("!") + name;
    entries_.push_back(Entry{TypeTag::EFFECT, std::move(eff_name), std::nullopt, std::nullopt,
                             std::nullopt, std::nullopt, std::nullopt,
                             EffectType{std::move(name), arg},
                             std::nullopt});
    return id;
}

TypeId TypeRegistry::register_capability(std::vector<std::string> effects, bool unrestricted) {
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
                             CapabilityType{std::move(effects), unrestricted}});
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
        if (arg_idx >= args.size())
            break; // 剩余 forall 保留
        std::unordered_map<std::uint32_t, TypeId> subst;
        subst[ft->var.index] = args[arg_idx++];
        result = substitute(ft->body, subst);
    }
    return result;
}

bool TypeRegistry::is_subtype(TypeId sub, TypeId sup) const {
    if (sub == sup)
        return true;
    if (sup == dynamic_type())
        return true;
    return false;
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
        return register_forall(ft->var, substitute(ft->body, subst));
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
            if (!vt) return "<variant>";
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
            if (!rt) return "<record>";
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

} // namespace aura::core
