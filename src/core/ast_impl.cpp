module aura.core.ast;
import aura.core.type;

namespace aura::ast {

// ── Patch application ──────────────────────────────────────────
bool apply_patches(FlatAST& ast, std::span<const Patch> patches) {
    // The pre (!patches.empty()) is on the declaration in
    // ast.ixx. The p.node < ast.size() check below is repeated
    // in the loop body (a soft runtime check that returns false
    // on stale id, rather than aborting via contract).
    for (auto& p : patches) {
        if (!ast.is_valid(p.node))
            return false;
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
    // Validate all patched nodes (debug builds assert, release returns false on violation)
    for (auto& p : patches) {
        auto err = ast.validate_node(p.node, /*fail_on_error=*/true);
        if (!err.empty())
            return false;
    }
    return true;
}

// ── Delta fixup (for deserialization) ──────────────────────────
void fixup_deltas(FlatAST& ast) {
    for (NodeId id = 0; id < ast.size(); ++id) {
        auto children = ast.children_mutable(id);
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

namespace {
    std::string make_node_error(std::uint32_t id, const std::string& msg) {
        return "[node " + std::to_string(id) + "] " + msg;
    }
}

// ── Node Validation ────────────────────────────────────────────
// Checks invariants defined by kNodeMeta for each node.
// Returns a description of the first violation, or empty string if valid.

std::string FlatAST::validate_node(NodeId id, bool fail_on_error) const {
    // AURA_PRE
    if (!is_valid(id))
        std::abort();
    if (id >= size())
        return make_node_error(id, "node ID out of range");

    auto tag = tag_[id];
    auto raw_idx = static_cast<std::size_t>(tag) - 1;
    if (raw_idx >= kNodeMeta.size()) {
        auto msg = make_node_error(id, "invalid tag value " + std::to_string(static_cast<int>(tag)));
        if (fail_on_error) throw std::logic_error(msg);
        return msg;
    }

    auto& m = kNodeMeta[raw_idx];

    // Gap sentinel check
    if (m.name == "<gap>") {
        auto msg = make_node_error(id, "node has gap tag (unused tag value)");
        if (fail_on_error) throw std::logic_error(msg);
        return msg;
    }

    // Tag/name consistency
    if (m.tag != tag) {
        auto msg = make_node_error(id, "tag mismatch: meta says " + std::string(m.name) +
                                      " but node has tag " + std::to_string(static_cast<int>(tag)));
        if (fail_on_error) throw std::logic_error(msg);
        return msg;
    }

    // Issue #220: child_count now lives in children_[id].size()
    // (the per-node std::pmr::vector<NodeId>), not in the
    // legacy child_count_ SoA column (which is gone).
    auto child_count = children(id).size();
    auto fixed = m.fixed_children;

    // Minimum children check
    if (child_count < fixed) {
        auto msg = make_node_error(id, std::string(m.name) + " requires " + std::to_string(fixed) +
                                      " fixed children, got " + std::to_string(child_count));
        if (fail_on_error) throw std::logic_error(msg);
        return msg;
    }

    // Variable children check: if has_var_children, child_count must be >= fixed
    // If not has_var_children, child_count must exactly equal fixed (or match a known pattern)
    if (!m.has_var_children && child_count != fixed) {
        // Special case: some nodes with fixed_children=0 have flexible children (Begin/DefineModule)
        // We only enforce exact match for nodes with fixed_children > 0
        if (fixed > 0) {
            auto msg = make_node_error(id, std::string(m.name) + " expects exactly " +
                                          std::to_string(fixed) + " children, got " +
                                          std::to_string(child_count));
            if (fail_on_error) throw std::logic_error(msg);
            return msg;
        }
    }

    // String field check
    if (m.has_string && sym_id_[id] == INVALID_SYM) {
        auto msg = make_node_error(id, std::string(m.name) + " requires a symbol (sym_id), got INVALID_SYM");
        if (fail_on_error) throw std::logic_error(msg);
        return msg;
    }

    // Param count check (Lambda-like nodes)
    // Lambda has fixed_children=1 for body, params in separate param arrays
    // The presence of params is checked via has_params flag, not children

    return {}; // valid
}

std::size_t FlatAST::validate_all_nodes(bool fail_on_error) const {
    std::size_t violations = 0;
    for (NodeId id = 0; id < size(); ++id) {
        auto err = validate_node(id, fail_on_error);
        if (!err.empty())
            ++violations;
    }
    return violations;
}

std::size_t FlatAST::validate_all_nodes(std::vector<ValidationError>& errors) const {
    std::size_t count = 0;
    for (NodeId id = 0; id < size(); ++id) {
        auto err = validate_node(id, false);
        if (!err.empty()) {
            ++count;
            // Parse the error string into expected/actual
            ValidationError ve;
            ve.node = id;
            ve.message = err;
            errors.push_back(ve);
        }
    }
    return count;
}

} // namespace aura::ast
