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
    entries_.push_back(Entry{tag, std::move(name), std::nullopt});
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
    entries_.push_back(Entry{tag, tmp_name, std::move(ft)});
    std::string name = "(";
    for (auto& a : entries_.back().func->args)
        name += std::string(name_of(a)) + " ";
    name += std::string("-> ") + std::string(name_of(ret)) + ")";
    entries_.back().name = name;
    return id;
}

TypeId TypeRegistry::register_forall(TypeId var, TypeId body) {
    auto id = TypeId{
        .index = static_cast<std::uint32_t>(entries_.size()),
        .generation = next_generation_,
    };
    std::string forall_name = std::string("forall ") + std::string(name_of(var)) + " " + std::string(name_of(body));
    entries_.push_back(Entry{TypeTag::FORALL, std::move(forall_name), std::nullopt});
    return id;
}

TypeId TypeRegistry::make_var(std::string name) {
    if (name.empty()) name = "__t" + std::to_string(entries_.size());
    auto id = TypeId{
        .index = static_cast<std::uint32_t>(entries_.size()),
        .generation = next_generation_,
    };
    entries_.push_back(Entry{TypeTag::TYPE_VAR, std::move(name), std::nullopt});
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

const FuncType* TypeRegistry::func_of(TypeId id) const {
    if (id.index < entries_.size() && entries_[id.index].func)
        return &*entries_[id.index].func;
    return nullptr;
}

bool TypeRegistry::is_var(TypeId id) const {
    return id.index < entries_.size() && entries_[id.index].tag == TypeTag::TYPE_VAR;
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
        default:
            return std::string(name_of(id));
    }
}

} // namespace aura::core
