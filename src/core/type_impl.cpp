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
    entries_.push_back(Entry{tag, std::move(name), std::nullopt, std::nullopt, std::nullopt});
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
    entries_.push_back(Entry{tag, tmp_name, std::move(ft), std::nullopt});
    std::string name = "(";
    for (auto& a : entries_.back().func->args)
        name += std::string(name_of(a)) + " ";
    name += std::string("-> ") + std::string(name_of(ret)) + ")";
    entries_.back().name = name;
    return id;
}

TypeId TypeRegistry::register_linear(TypeId inner) {
    auto id = TypeId{
        .index = static_cast<std::uint32_t>(entries_.size()),
        .generation = next_generation_,
    };
    std::string linear_name = "(Linear " + std::string(name_of(inner)) + ")";
    entries_.push_back(Entry{TypeTag::LINEAR, std::move(linear_name), std::nullopt, std::nullopt, LinearType{inner}});
    name_to_id_[entries_.back().name] = id;
    return id;
}

TypeId TypeRegistry::register_forall(TypeId var, TypeId body) {
    auto id = TypeId{
        .index = static_cast<std::uint32_t>(entries_.size()),
        .generation = next_generation_,
    };
    auto name_var = name_of(var);
    auto name_body = name_of(body);
    std::string forall_name = std::string("∀") + std::string(name_var) + ". " + std::string(name_body);
    entries_.push_back(Entry{TypeTag::FORALL, std::move(forall_name), std::nullopt, ForallType{var, body}, std::nullopt});
    return id;
}

TypeId TypeRegistry::make_var(std::string name) {
    if (name.empty()) name = "__t" + std::to_string(entries_.size());
    auto id = TypeId{
        .index = static_cast<std::uint32_t>(entries_.size()),
        .generation = next_generation_,
    };
    entries_.push_back(Entry{TypeTag::TYPE_VAR, std::move(name), std::nullopt, std::nullopt});
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
    if (!ft) return forall_id;
    // Build substitution map: bound var → fresh var
    auto fresh = fresh_var();
    // Recursively replace the bound variable in the body
    auto replace_var = [&](this const auto& self, TypeId tid) -> TypeId {
        if (tid == ft->var) return fresh;
        if (auto* f = func_of(tid)) {
            std::vector<TypeId> new_args;
            for (auto& a : f->args) new_args.push_back(self(a));
            return register_func(std::move(new_args), self(f->ret));
        }
        // Forall body could be nested
        if (auto* f2 = forall_of(tid)) {
            auto new_body = self(f2->body);
            return register_forall(f2->var, new_body);
        }
        return tid;
    };
    return replace_var(ft->body);
}

bool TypeRegistry::is_subtype(TypeId sub, TypeId sup) const {
    if (sub == sup) return true;
    if (sup == dynamic_type()) return true;
    return false;
}

TypeId TypeRegistry::lookup_type(const std::string& name) const {
    auto it = name_to_id_.find(name);
    if (it != name_to_id_.end()) return it->second;
    return TypeId{};
}

std::vector<TypeId> TypeRegistry::free_vars(TypeId id) const {
    std::vector<TypeId> result;
    if (!id.valid() || id.index >= entries_.size()) return result;
    std::vector<TypeId> stack = {id};
    std::unordered_set<std::uint32_t> seen;
    while (!stack.empty()) {
        auto cur = stack.back(); stack.pop_back();
        if (!cur.valid() || cur.index >= entries_.size()) continue;
        if (!seen.insert(cur.index).second) continue;
        if (entries_[cur.index].tag == TypeTag::TYPE_VAR) {
            result.push_back(cur);  // preserve exact TypeId with generation
            continue;
        }
        if (auto* f = func_of(cur)) {
            stack.push_back(f->ret);
            for (auto& a : f->args) stack.push_back(a);
        }
        if (auto* ft = forall_of(cur)) {
            stack.push_back(ft->body);
        }
    }
    return result;
}

std::string TypeRegistry::format_type(TypeId id) const {
    auto tag = tag_of(id);
    switch (tag) {
        case TypeTag::FUNC: {
            auto* f = func_of(id);
            if (!f) return "<func>";
            std::string s = "(";
            for (std::size_t i = 0; i < f->args.size(); i++) {
                if (i > 0) s += " ";
                s += format_type(f->args[i]);
            }
            return s + " -> " + format_type(f->ret) + ")";
        }
        case TypeTag::TYPE_VAR:
            return std::string(name_of(id));
        case TypeTag::FORALL: {
            auto* ft = forall_of(id);
            if (!ft) return "<forall>";
            return "∀" + format_type(ft->var) + ". " + format_type(ft->body);
        }
        case TypeTag::LINEAR: {
            auto* lt = linear_of(id);
            if (!lt) return "<linear>";
            return "(Linear " + format_type(lt->inner) + ")";
        }
        default:
            return std::string(name_of(id));
    }
}

} // namespace aura::core
