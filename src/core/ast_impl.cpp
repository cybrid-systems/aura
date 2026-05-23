module aura.core.ast;
import aura.core.type;

namespace aura::ast {

// ── Patch application ──────────────────────────────────────────
bool apply_patches(FlatAST& ast, std::span<const Patch> patches) {
    for (auto& p : patches) {
        if (p.node >= ast.size())
            return false;
        switch (p.field_offset) {
            case 0:
                ast.tag(p.node) = static_cast<NodeTag>(p.new_value);
                break;
            case 1:
                ast.int_val(p.node) = static_cast<std::int64_t>(p.new_value);
                break;
            case 2:
                ast.sym_id(p.node) = static_cast<SymId>(p.new_value);
                break;
            default:
                return false;
        }
    }
    return true;
}

// ── Delta fixup (for deserialization) ──────────────────────────
void fixup_deltas(FlatAST& ast) {
    for (NodeId id = 0; id < ast.size(); ++id) {
        auto children = const_cast<FlatAST&>(ast).children(id);
        for (auto& cid : children) {
            if (cid != NULL_NODE)
                cid += id;
        }
    }
}

void FlatAST::resolve_type_ids(aura::core::TypeRegistry& reg, StringPool& pool) {
    for (std::size_t i = 0; i < tag_.size(); ++i) {
        if (tag_[i] == NodeTag::TypeAnnotation) {
            auto sym = sym_id_[i];
            auto sv = pool.resolve(sym);
            std::string name(sv);
            if (!name.empty()) {
                auto tid = reg.lookup_type(name);
                if (tid.valid() && i < type_id_.size())
                    type_id_[i] = tid.index;
            }
        }
    }
}

} // namespace aura::ast
